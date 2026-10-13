/* proxy.h - connections through a web proxy (HTTP CONNECT), exactly as
   Reel's FFmpeg makes them: the proxy is told "CONNECT www.youtube.com:443",
   and once it answers 200 the TLS session runs through it to YouTube as
   usual (the certificate is still checked against YouTube's name).

   Which proxy: the http_proxy variable, the one FFmpeg reads, so that
   TubeSearch and Reel always agree. They must: YouTube's video addresses
   work only from the connection that asked for them. So the variable is
   read the way FFmpeg 5.1 reads it (libavformat tls.c, av_url_split,
   ff_http_match_no_proxy):
   - only a value starting http:// is a proxy (FFmpeg ignores anything
     else, so TubeSearch does too, and says so);
   - http://[user:password@]host:port[/...]: the port is needed (FFmpeg
     fails without one); user:password is %-decoded, and sent as Basic;
   - no_proxy: names separated by spaces or commas; "*" matches all; a
     leading "*" and then "." are dropped; a name matches itself and its
     subdomains, case-sensitively.
   On RISC OS the variables are read from the system each time (proxy_
   refresh), as Reel will read them when it starts.

   Part of TubeSearch. MIT licence. */
#ifndef TUBE_PROXY_H
#define TUBE_PROXY_H
#include <stddef.h>

typedef struct {
  char host[128];
  int port;
  char auth[400];             /* base64 user:password, or "" */
} proxy_cfg;

/* Sets the proxy from spec (an http_proxy value; NULL or "": no proxy)
   and no_proxy (may be NULL). 0, or -1 with the reason (and no proxy). */
int proxy_set (const char *spec, const char *no_proxy, char *err, size_t errlen);

/* Reads http_proxy and no_proxy (or uses override instead of http_proxy,
   for tscli's -proxy; "none" for none) when they've changed since the last
   call: 1 if they had (and the proxy is set from them), 0 if not, -1 with
   the reason (no proxy; reported once per change). */
int proxy_refresh (const char *override, char *err, size_t errlen);

/* The proxy to reach host through, or NULL to connect directly. */
const proxy_cfg *proxy_for (const char *host);

/* For messages: "proxy.example:3128", or "" for none. Never the password. */
const char *proxy_name (void);

/* ---- the tunnel, on a connected non-blocking socket ------------------- */

typedef struct {
  char out[700];
  int out_len, sent;
  char in[2048];
  int in_len;
} proxy_tunnel;

/* Prepares the CONNECT request for host:port through p: 0, or -1 if it
   doesn't fit. */
int proxy_tunnel_init (proxy_tunnel *t, const proxy_cfg *p, const char *host, int port);

/* Sends the request and reads the proxy's answer, never reading past it
   (the TLS bytes after it are the server's). 1 when the tunnel is open,
   0 to call again later, -1 with the reason. */
int proxy_tunnel_step (proxy_tunnel *t, int fd, char *err, size_t errlen);

/* The status line's code, from a whole reply header (for the tests):
   >= 100, or -1 if it isn't one. */
int proxy_reply_status (const char *head, size_t len);

#endif
