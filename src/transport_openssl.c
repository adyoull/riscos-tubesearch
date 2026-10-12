/* transport_openssl.c - the http.h transport for Linux (and other Unix):
   non-blocking sockets and OpenSSL. Used by the command-line tool on a
   PC and by the tests (TUBE_CONNECT=host:port sends every connection to a
   local test server; TUBE_INSECURE=1 accepts its certificate).
   Part of Tube. MIT licence. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include "http.h"
#include "transport.h"

typedef struct {
  int fd;
  SSL *ssl;
  int connected;
} conn;

static SSL_CTX *ctx;

static void *t_open (const char *host, int port, int tls, char *err, size_t errlen)
{
  struct addrinfo hints, *ai = NULL;
  char portstr[16], hostbuf[256];
  const char *chost = host, *redirect = getenv ("TUBE_CONNECT");
  conn *c;
  int rc;
  (void) tls;
  if (!ctx)
    {
      ctx = SSL_CTX_new (TLS_client_method ());
      if (!ctx)
        {
          snprintf (err, errlen, "OpenSSL failed to start");
          return NULL;
        }
      if (getenv ("TUBE_INSECURE"))
        SSL_CTX_set_verify (ctx, SSL_VERIFY_NONE, NULL);
      else
        {
          SSL_CTX_set_verify (ctx, SSL_VERIFY_PEER, NULL);
          SSL_CTX_set_default_verify_paths (ctx);
        }
    }
  snprintf (portstr, sizeof portstr, "%d", port);
  if (redirect && *redirect)
    {
      const char *colon = strrchr (redirect, ':');
      snprintf (hostbuf, sizeof hostbuf, "%.*s", colon ? (int) (colon - redirect) : (int) strlen (redirect), redirect);
      chost = hostbuf;
      if (colon)
        snprintf (portstr, sizeof portstr, "%s", colon + 1);
    }
  memset (&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if ((rc = getaddrinfo (chost, portstr, &hints, &ai)) != 0 || !ai)
    {
      snprintf (err, errlen, "can't find %s (%s)", host, gai_strerror (rc));
      return NULL;
    }
  c = calloc (1, sizeof *c);
  c->fd = socket (ai->ai_family, SOCK_STREAM, 0);
  if (c->fd < 0)
    {
      snprintf (err, errlen, "socket: %s", strerror (errno));
      freeaddrinfo (ai);
      free (c);
      return NULL;
    }
  fcntl (c->fd, F_SETFL, fcntl (c->fd, F_GETFL) | O_NONBLOCK);
  if (connect (c->fd, ai->ai_addr, ai->ai_addrlen) < 0 && errno != EINPROGRESS)
    {
      snprintf (err, errlen, "can't connect to %s: %s", host, strerror (errno));
      freeaddrinfo (ai);
      close (c->fd);
      free (c);
      return NULL;
    }
  freeaddrinfo (ai);
  c->ssl = SSL_new (ctx);
  SSL_set_fd (c->ssl, c->fd);
  SSL_set_tlsext_host_name (c->ssl, host);
  if (!getenv ("TUBE_INSECURE"))
    SSL_set1_host (c->ssl, host);
  SSL_set_connect_state (c->ssl);
  return c;
}

static int t_ready (void *p, char *err, size_t errlen)
{
  conn *c = p;
  int k;
  if (!c->connected)
    {
      fd_set w;
      struct timeval tv = { 0, 0 };
      int soerr = 0;
      socklen_t sl = sizeof soerr;
      FD_ZERO (&w);
      FD_SET (c->fd, &w);
      if (select (c->fd + 1, NULL, &w, NULL, &tv) <= 0)
        return 0;
      getsockopt (c->fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
      if (soerr)
        {
          snprintf (err, errlen, "can't connect: %s", strerror (soerr));
          return -1;
        }
      c->connected = 1;
    }
  k = SSL_do_handshake (c->ssl);
  if (k == 1)
    return 1;
  switch (SSL_get_error (c->ssl, k))
    {
    case SSL_ERROR_WANT_READ:
    case SSL_ERROR_WANT_WRITE:
      return 0;
    }
  snprintf (err, errlen, "TLS handshake failed (%s)", ERR_reason_error_string (ERR_get_error ()));
  return -1;
}

static int t_recv (void *p, char *buf, int len)
{
  conn *c = p;
  int k = SSL_read (c->ssl, buf, len);
  if (k > 0)
    return k;
  switch (SSL_get_error (c->ssl, k))
    {
    case SSL_ERROR_WANT_READ:
    case SSL_ERROR_WANT_WRITE:
      return HTTP_AGAIN;
    case SSL_ERROR_ZERO_RETURN:
      return 0;
    }
  return HTTP_FAIL;
}

static int t_send (void *p, const char *buf, int len)
{
  conn *c = p;
  int k = SSL_write (c->ssl, buf, len);
  if (k > 0)
    return k;
  switch (SSL_get_error (c->ssl, k))
    {
    case SSL_ERROR_WANT_READ:
    case SSL_ERROR_WANT_WRITE:
      return HTTP_AGAIN;
    }
  return HTTP_FAIL;
}

static void t_close (void *p)
{
  conn *c = p;
  SSL_free (c->ssl);
  close (c->fd);
  free (c);
}

static unsigned t_now (void)
{
  struct timespec ts;
  clock_gettime (CLOCK_MONOTONIC, &ts);
  return (unsigned) (ts.tv_sec * 100 + ts.tv_nsec / 10000000);
}

const http_transport transport_system = { t_open, t_ready, t_recv, t_send, t_close, t_now };

int transport_check (char *err, size_t errlen)
{
  (void) err; (void) errlen;
  return 0;
}

void transport_wait (int cs)
{
  struct timespec ts = { cs / 100, (cs % 100) * 10000000L };
  nanosleep (&ts, NULL);
}
