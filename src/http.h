/* http.h - HTTP/1.1 requests that never block, for a Wimp program.

   Requests are queued per host; each host has one connection, kept open
   between requests (keep-alive), so the hosts are fetched from at the same
   time but one request at a time on each. http_poll() moves everything on
   as far as it can without waiting and must be called often (on null
   events) while http_busy(). A finished request's callback is called from
   http_poll().

   The connection itself (TCP and TLS) is the transport's: AcornSSL on
   RISC OS (transport_riscos.c), OpenSSL on Linux (transport_openssl.c),
   or a fake in the tests.

   Part of Tube. MIT licence. */
#ifndef TUBE_HTTP_H
#define TUBE_HTTP_H
#include <stddef.h>

#define HTTP_AGAIN (-1)       /* would block: try again later */
#define HTTP_FAIL (-2)        /* the connection failed */

typedef struct http_transport {
  /* Starts connecting (port 443 with TLS). NULL with a message in err. */
  void *(*open) (const char *host, int port, int tls, char *err, size_t errlen);
  /* 1 once connected (and the TLS handshake is over), 0 not yet, -1 failed. */
  int (*ready) (void *conn, char *err, size_t errlen);
  /* Bytes read (> 0), 0 at the end (closed), HTTP_AGAIN or HTTP_FAIL. */
  int (*recv) (void *conn, char *buf, int len);
  /* Bytes written (>= 0), HTTP_AGAIN or HTTP_FAIL. */
  int (*send) (void *conn, const char *buf, int len);
  void (*close) (void *conn);
  /* A clock in centiseconds. */
  unsigned (*now_cs) (void);
} http_transport;

typedef struct http_req http_req;
typedef void (*http_done_fn) (http_req *r, void *ctx);

void http_init (const http_transport *t);
void http_shutdown (void);                   /* closes everything; no callbacks */

/* Queues a request to https://host/path. headers: extra header lines
   ("Name: value\r\n"...), or NULL. body (POST) is copied. tag groups
   requests for http_cancel_tag. done is called once, when it finishes
   (successfully or not); the request is freed after done returns. */
http_req *http_get (const char *host, const char *path, const char *headers,
                    int tag, http_done_fn done, void *ctx);
http_req *http_post (const char *host, const char *path, const char *headers,
                     const char *body, size_t bodylen, int tag, http_done_fn done, void *ctx);

/* In the callback: the status (200...), or 0 if the request failed (then
   http_error says why). The body, and taking it over (malloc'd, NUL after). */
int http_status (const http_req *r);
const char *http_error (const http_req *r);
const char *http_body (const http_req *r, size_t *len);
char *http_take_body (http_req *r, size_t *len);

/* Forget a request: its callback is never called. (Safe at any time
   before the callback.) */
void http_cancel (http_req *r);
void http_cancel_tag (int tag);

/* Does what can be done now. Returns 1 while anything is still to do. */
int http_poll (void);
int http_busy (void);

/* Tests: per-host connection count so far. */
int http_connections_made (void);

#endif
