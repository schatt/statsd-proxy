/**
 * Copyright (c) 2015, Chao Wang <hit9@icloud.com>
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "ctx.h"
#include "ketama.h"
#include "log.h"
#include "proxy.h"
#include "parser.h"

/* Start proxy in a thread. */
void *
thread_start(void *arg)
{
    struct ctx *ctx = arg;
    int err;

    if ((err = ctx_init(ctx)) == CTX_OK) {
        server_start(ctx);
    } else {
        exit(1);
    }
}

/* Start server. */
int
server_start(struct ctx *ctx)
{
    assert(ctx != NULL);
    assert(ctx->sfd > 0);

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr=htonl(INADDR_ANY);
    addr.sin_port=htons(ctx->port);

    if (bind(ctx->sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_error("failed to bind server on port %d", ctx->port);
        return PROXY_EBIND;
    }

    log_info("listening on udp://127.0.0.1:%d..", ctx->port);

    int n;
    struct buf *buf = ctx->buf;

    ctx->state = CTX_SERVER_RUNNING;

    for(; ctx->state != CTX_SERVER_STOPPED; ) {
        while (1) {
            if (buf_grow(buf, buf->len + BUF_READ_UNIT) != BUF_OK)
                return PROXY_ENOMEM;

            if ((n = recv(ctx->sfd, buf->data + buf->len,
                            (buf->cap - buf->len) * sizeof(char), 0)) < 0)
                break;

            buf->len += n;

            relay_buf(ctx);
        }
    }

    return PROXY_OK;
}

/* Relay buffer to backends. Note: all network etc. errors on single
 * relay will be ignored. */
void
relay_buf(struct ctx *ctx)
{
    assert(ctx != NULL);
    assert(ctx->buf != NULL);

    struct parser_result result;
    int n, n_parsed = 0;
    char *data = ctx->buf->data;
    size_t len = ctx->buf->len;
    struct ketama_node node;
    struct sockaddr_in addr;

    while ((n = parse(&result, data, len)) > 0) {
        node = ketama_node_get(ctx->ring, result.key, result.len);
        addr = ctx->addrs[node.idx];

        if (sendto(ctx->cfd, result.block, result.blen, 0,
                    (struct sockaddr *)&addr, sizeof(struct sockaddr_in)) < 0)
            log_warn("failed to send data");

        data += n;
        len -= n;
        n_parsed += n;
    }

    /* `buf_lrm` won't clear unfinished data */
    buf_lrm(ctx->buf, n_parsed);

    /* but we should clear it if the current data
     * is too large */
    if (ctx->buf->len > BUF_UNFINISH_MAX)
        buf_clear(ctx->buf);
}
