#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct net_ctx net_ctx_t;

typedef struct {
    char *data;
    size_t len, cap;
} buf_t;

/* One TLS context (CA chain + RNG). Not thread-safe: one per thread. */
net_ctx_t *net_new(const char *ca_file);
void net_free(net_ctx_t *c);

/* GET following redirects; body (de-chunked) into out. Returns HTTP status or <0. */
int http_get(net_ctx_t *c, const char *url, size_t max_body, buf_t *out);

/* Follow a redirect chain to its final URL without downloading the body.
 * Returns 0 and fills `final` on success. */
int http_resolve(net_ctx_t *c, const char *url, char *final, size_t cap);

/* One request, no redirects (PUT/DELETE/GET with an optional JSON body). */
int http_send(net_ctx_t *c, const char *method, const char *url, const char *json_body, buf_t *out);

/* Streaming GET with an optional byte range (to < 0: open-ended), positioned at
 * the body. status/total (file size from Content-Range, else Content-Length)
 * are filled. Follows up to 5 redirects (http_stream_url() says where it
 * ended up). NULL on failure. */
typedef struct net_stream net_stream_t;
net_stream_t *http_stream_open(net_ctx_t *c, const char *url, int64_t from, int64_t to,
                               int *status, int64_t *total);
int http_stream_read(net_stream_t *s, char *buf, size_t n);   /* >0, 0 EOF, <0 error */
const char *http_stream_url(net_stream_t *s);                 /* where it ended up (after redirects) */
void http_stream_close(net_stream_t *s);

void buf_free(buf_t *b);

/* The TLS session cache on disk (resume across restarts). */
void net_sessions_save(const char *path);
void net_sessions_load(const char *path);
