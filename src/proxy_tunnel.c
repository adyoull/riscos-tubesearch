/* proxy_tunnel.c - proxy_tunnel_step (proxy.h): the socket side, kept
   apart from proxy.c so that the tests of the rest need no sockets.
   Part of TubeSearch. MIT licence. */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include "proxy.h"

extern char proxy_name_buf[160];
#define name proxy_name_buf

static int would_block (void)
{
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ENOTCONN;
}

int proxy_tunnel_step (proxy_tunnel *t, int fd, char *err, size_t errlen)
{
  while (t->sent < t->out_len)
    {
#ifdef MSG_NOSIGNAL
      ssize_t k = send (fd, t->out + t->sent, (size_t) (t->out_len - t->sent), MSG_NOSIGNAL);
#else
      ssize_t k = send (fd, t->out + t->sent, (size_t) (t->out_len - t->sent), 0);
#endif
      if (k < 0)
        {
          if (would_block ())
            return 0;
          snprintf (err, errlen, "The proxy %s closed the connection (%s)", name, strerror (errno));
          return -1;
        }
      t->sent += (int) k;
    }
  for (;;)
    {
      /* peek, and take only up to the end of the header */
      char buf[512];
      ssize_t k = recv (fd, buf, sizeof buf, MSG_PEEK);
      int take = 0, end = 0;
      if (k < 0)
        {
          if (would_block ())
            return 0;
          snprintf (err, errlen, "The proxy %s closed the connection (%s)", name, strerror (errno));
          return -1;
        }
      if (k == 0)
        {
          snprintf (err, errlen, "The proxy %s closed the connection", name);
          return -1;
        }
      for (take = 0; take < (int) k; take++)
        {
          if (t->in_len + 1 >= (int) sizeof t->in)
            {
              snprintf (err, errlen, "The proxy %s gave too long an answer", name);
              return -1;
            }
          t->in[t->in_len++] = buf[take];
          t->in[t->in_len] = 0;
          if (t->in_len >= 4 && !memcmp (t->in + t->in_len - 4, "\r\n\r\n", 4))
            {
              end = 1;
              take++;
              break;
            }
        }
      if (recv (fd, buf, (size_t) take, 0) != take)
        {
          snprintf (err, errlen, "The proxy %s's answer couldn't be read", name);
          return -1;
        }
      if (end)
        {
          int code = proxy_reply_status (t->in, (size_t) t->in_len);
          if (code >= 200 && code < 300)
            return 1;
          if (code < 0)
            snprintf (err, errlen, "The proxy %s gave an answer TubeSearch doesn't understand", name);
          else
            {
              const char *line = t->in + 9;
              size_t n = strcspn (line, "\r\n");
              snprintf (err, errlen, "The proxy %s refused: %.*s%s", name, (int) (n > 80 ? 80 : n), line,
                        code == 407 ? " (check the user name and password in the proxy setting)" : "");
            }
          return -1;
        }
    }
}
