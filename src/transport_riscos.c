/* transport_riscos.c - the http.h transport on RISC OS: a non-blocking
   TCP socket (UnixLib) handed to the AcornSSL module for TLS.

   This is how riscos-ffmpeg's tls_acornssl.c (patch 0018, used by Reel)
   drives AcornSSL, which works on the Pi:
   - AcornSSL_CreateSession takes over the connected RISC OS socket;
   - SO_ACORNSSL_HOSTNAME gives the name for SNI and the certificate check;
   - FIONBIO through AcornSSL_Ioctl makes Recv/Send return EWOULDBLOCK
     instead of waiting;
   - the handshake happens inside Recv/Send: until it's over they fail
     with ENOTCONN (longer while AcornSSL's own task asks the desktop user
     about a certificate it can't verify), so it is driven with 1-byte
     MSG_PEEK reads from null events, and the desktop keeps running.

   Part of TubeSearch. MIT licence. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <kernel.h>
#include "http.h"
#include "transport.h"
#include "proxy.h"

/* UnixLib: the RISC OS socket handle behind a file descriptor */
extern int __get_ro_socket (int sockfd);

#define XAcornSSL_Ioctl          (0x050F81 | 0x20000)
#define XAcornSSL_Close          (0x050F84 | 0x20000)
#define XAcornSSL_Recv           (0x050F87 | 0x20000)
#define XAcornSSL_CreateSession  (0x050F88 | 0x20000)
#define XAcornSSL_Setsockopt     (0x050F8B | 0x20000)
#define XAcornSSL_Version        (0x050F8D | 0x20000)
#define XAcornSSL_Send           (0x050F8F | 0x20000)
#define XOS_ReadMonotonicTime    (0x42 | 0x20000)
#define CreateSession_New        0
#define SO_ACORNSSL_HOSTNAME     0x11E0
#define ACORNSSL_SOL_SOCKET      0xFFFF
#define ACORNSSL_FIONBIO         0x8004667E   /* _IOW('f', 126, int) */
#define ACORNSSL_MSG_PEEK        0x2

/* Internet module errors: 0x20E00 + a BSD errno */
#define DCI4_BASE        0x20E00
#define DCI4_EINTR       4
#define DCI4_EWOULDBLOCK 35
#define DCI4_EINPROGRESS 36
#define DCI4_ENOTCONN    57

enum { S_TCP, S_TUNNEL, S_HANDSHAKE, S_READY };

typedef struct {
  int fd;
  int ssl;                    /* AcornSSL handle, 0 = none yet */
  int state;
  char host[96];
  proxy_tunnel *tunnel;       /* through a proxy: the CONNECT, until it's answered */
} conn;

/* A few addresses remembered: the Resolver is asked once per host. */
static struct { char host[128]; struct in_addr addr; } dns[8];
static int dns_next;

static int dci4_errno (const _kernel_oserror *e)
{
  return (e->errnum & ~0xFF) == DCI4_BASE ? (int) (e->errnum & 0xFF) : -1;
}

static int lookup (const char *host, struct in_addr *out, char *err, size_t errlen)
{
  struct hostent *h;
  for (int i = 0; i < 8; i++)
    if (!strcmp (dns[i].host, host))
      {
        *out = dns[i].addr;
        return 0;
      }
  h = gethostbyname (host);
  if (!h || h->h_addrtype != AF_INET || !h->h_addr_list[0])
    {
      snprintf (err, errlen, "Can't find %s (is the network up?)", host);
      return -1;
    }
  memcpy (out, h->h_addr_list[0], sizeof *out);
  snprintf (dns[dns_next].host, sizeof dns[dns_next].host, "%s", host);
  dns[dns_next].addr = *out;
  dns_next = (dns_next + 1) % 8;
  return 0;
}

static void *t_open (const char *host, int port, int tls, char *err, size_t errlen)
{
  struct sockaddr_in sa;
  conn *c;
  int one = 1;
  const proxy_cfg *px = proxy_for (host);
  (void) tls;                                   /* (always TLS) */
  memset (&sa, 0, sizeof sa);
  sa.sin_family = AF_INET;
  sa.sin_port = htons ((unsigned short) (px ? px->port : port));
  if (lookup (px ? px->host : host, &sa.sin_addr, err, errlen))
    return NULL;
  if (!(c = calloc (1, sizeof *c)) || (px && !(c->tunnel = malloc (sizeof *c->tunnel))))
    {
      free (c);
      snprintf (err, errlen, "Out of memory");
      return NULL;
    }
  if (px && proxy_tunnel_init (c->tunnel, px, host, port))
    {
      snprintf (err, errlen, "The request for the proxy %s is too long", proxy_name ());
      free (c->tunnel);
      free (c);
      return NULL;
    }
  snprintf (c->host, sizeof c->host, "%s", host);
  c->fd = socket (AF_INET, SOCK_STREAM, 0);
  if (c->fd < 0)
    {
      snprintf (err, errlen, "Can't make a socket: %s", strerror (errno));
      free (c->tunnel);
      free (c);
      return NULL;
    }
  ioctl (c->fd, FIONBIO, &one);
  if (connect (c->fd, (struct sockaddr *) &sa, sizeof sa) < 0 && errno != EINPROGRESS && errno != EALREADY)
    {
      snprintf (err, errlen, "Can't connect to %s: %s", px ? proxy_name () : host, strerror (errno));
      close (c->fd);
      free (c->tunnel);
      free (c);
      return NULL;
    }
  c->state = S_TCP;
  return c;
}

static int start_tls (conn *c, char *err, size_t errlen)
{
  _kernel_swi_regs r;
  _kernel_oserror *e;
  int one = 1;
  r.r[0] = __get_ro_socket (c->fd);
  r.r[1] = CreateSession_New;
  r.r[2] = 0;
  if ((e = _kernel_swi (XAcornSSL_CreateSession, &r, &r)))
    {
      snprintf (err, errlen, "AcornSSL: %s", e->errmess);
      return -1;
    }
  c->ssl = r.r[0];
  r.r[0] = c->ssl;                              /* SNI, and the certificate must be for this name */
  r.r[1] = ACORNSSL_SOL_SOCKET;
  r.r[2] = SO_ACORNSSL_HOSTNAME;
  r.r[3] = (int) c->host;
  r.r[4] = sizeof (const char *);
  if ((e = _kernel_swi (XAcornSSL_Setsockopt, &r, &r)))
    {
      snprintf (err, errlen, "AcornSSL (host name): %s", e->errmess);
      return -1;
    }
  r.r[0] = c->ssl;
  r.r[1] = ACORNSSL_FIONBIO;
  r.r[2] = (int) &one;
  if ((e = _kernel_swi (XAcornSSL_Ioctl, &r, &r)))
    {
      snprintf (err, errlen, "AcornSSL (FIONBIO): %s", e->errmess);
      return -1;
    }
  c->state = S_HANDSHAKE;
  return 0;
}

static _kernel_oserror *ssl_io (int swi, int handle, void *buf, int len, int flags, int *done)
{
  _kernel_swi_regs r;
  _kernel_oserror *e;
  r.r[0] = handle;
  r.r[1] = (int) buf;
  r.r[2] = len;
  r.r[3] = flags;
  e = _kernel_swi (swi, &r, &r);
  *done = e ? -1 : r.r[0];
  return e;
}

static int t_ready (void *p, char *err, size_t errlen)
{
  conn *c = p;
  if (c->state == S_TCP)
    {
      fd_set w;
      struct timeval tv = { 0, 0 };
      int soerr = 0;
      socklen_t sl = sizeof soerr;
      FD_ZERO (&w);
      FD_SET (c->fd, &w);
      if (select (c->fd + 1, NULL, &w, NULL, &tv) <= 0)
        return 0;
      if (getsockopt (c->fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) == 0 && soerr)
        {
          snprintf (err, errlen, "Can't connect to %s: %s", c->tunnel ? proxy_name () : c->host,
                    strerror (soerr));
          return -1;
        }
      c->state = c->tunnel ? S_TUNNEL : S_HANDSHAKE;
      if (!c->tunnel && start_tls (c, err, errlen))
        return -1;
    }
  if (c->state == S_TUNNEL)
    {
      int k = proxy_tunnel_step (c->tunnel, c->fd, err, errlen);
      if (k <= 0)
        return k;
      free (c->tunnel);
      c->tunnel = NULL;
      if (start_tls (c, err, errlen))
        return -1;
    }
  if (c->state == S_HANDSHAKE)
    {
      unsigned char peek;
      int got;
      _kernel_oserror *e = ssl_io (XAcornSSL_Recv, c->ssl, &peek, 1, ACORNSSL_MSG_PEEK, &got);
      if (!e || dci4_errno (e) == DCI4_EWOULDBLOCK)
        c->state = S_READY;                     /* the handshake is over */
      else if (dci4_errno (e) == DCI4_ENOTCONN || dci4_errno (e) == DCI4_EINPROGRESS
               || dci4_errno (e) == DCI4_EINTR)
        return 0;
      else
        {
          snprintf (err, errlen, "Secure connection to %s failed: %s", c->host, e->errmess);
          return -1;
        }
    }
  return 1;
}

static int t_recv (void *p, char *buf, int len)
{
  conn *c = p;
  int got;
  _kernel_oserror *e = ssl_io (XAcornSSL_Recv, c->ssl, buf, len, 0, &got);
  if (!e)
    return got;                                 /* 0: closed */
  switch (dci4_errno (e))
    {
    case DCI4_EWOULDBLOCK: case DCI4_ENOTCONN: case DCI4_EINPROGRESS: case DCI4_EINTR:
      return HTTP_AGAIN;
    }
  return HTTP_FAIL;
}

static int t_send (void *p, const char *buf, int len)
{
  conn *c = p;
  int done;
  _kernel_oserror *e = ssl_io (XAcornSSL_Send, c->ssl, (void *) buf, len, 0, &done);
  if (!e)
    return done;
  switch (dci4_errno (e))
    {
    case DCI4_EWOULDBLOCK: case DCI4_ENOTCONN: case DCI4_EINPROGRESS: case DCI4_EINTR:
      return HTTP_AGAIN;
    }
  return HTTP_FAIL;
}

static void t_close (void *p)
{
  conn *c = p;
  if (c->ssl)
    {
      _kernel_swi_regs r;
      r.r[0] = c->ssl;
      _kernel_swi (XAcornSSL_Close, &r, &r);    /* close_notify; the socket stays ours */
    }
  close (c->fd);
  free (c->tunnel);
  free (c);
}

static unsigned t_now (void)
{
  _kernel_swi_regs r;
  if (_kernel_swi (XOS_ReadMonotonicTime, &r, &r))
    return 0;
  return (unsigned) r.r[0];
}

const http_transport transport_system = { t_open, t_ready, t_recv, t_send, t_close, t_now };

int transport_check (char *err, size_t errlen)
{
  _kernel_swi_regs r;
  if (_kernel_swi (XAcornSSL_Version, &r, &r))
    {
      snprintf (err, errlen, "TubeSearch needs the AcornSSL module (RISC OS 5.28 or later): "
                "RMLoad System:Modules.Network.URL.AcornSSL");
      return -1;
    }
  return 0;
}

void transport_wait (int cs)
{
  unsigned until = t_now () + (unsigned) cs;
  while ((int) (t_now () - until) < 0)
    ;
}
