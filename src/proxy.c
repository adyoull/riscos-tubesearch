/* proxy.c - see proxy.h. Part of TubeSearch. MIT licence. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "proxy.h"
#if defined __riscos && !defined __FAKE_RISCOS
#include <kernel.h>
#define XOS_ReadVarVal (0x23 | 0x20000)
#endif

static proxy_cfg proxy;
static int have_proxy;
static char no_list[1024];
char proxy_name_buf[160];
#define name proxy_name_buf

static void base64 (const unsigned char *in, size_t n, char *out, size_t size)
{
  static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t o = 0;
  for (size_t i = 0; i < n && o + 5 <= size; i += 3)
    {
      unsigned v = (unsigned) in[i] << 16 | (i + 1 < n ? (unsigned) in[i + 1] << 8 : 0)
                   | (i + 2 < n ? in[i + 2] : 0);
      out[o++] = t[v >> 18 & 63];
      out[o++] = t[v >> 12 & 63];
      out[o++] = i + 1 < n ? t[v >> 6 & 63] : '=';
      out[o++] = i + 2 < n ? t[v & 63] : '=';
    }
  out[o] = 0;
}

static int hexval (int c)
{
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
       : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* %-decodes n bytes of s onto the end of out. */
static size_t pct_decode (const char *s, size_t n, char *out, size_t at, size_t size)
{
  for (size_t i = 0; i < n && at + 1 < size; i++)
    if (s[i] == '%' && i + 2 < n && hexval (s[i + 1]) >= 0 && hexval (s[i + 2]) >= 0)
      {
        out[at++] = (char) (hexval (s[i + 1]) * 16 + hexval (s[i + 2]));
        i += 2;
      }
    else
      out[at++] = s[i];
  out[at] = 0;
  return at;
}

int proxy_set (const char *spec, const char *no_proxy, char *err, size_t errlen)
{
  const char *s, *at, *end, *colon;
  size_t hl;
  have_proxy = 0;
  name[0] = 0;
  memset (&proxy, 0, sizeof proxy);
  snprintf (no_list, sizeof no_list, "%s", no_proxy ? no_proxy : "");
  if (!spec || !*spec)
    return 0;
  if (strncmp (spec, "http://", 7))
    {
      /* FFmpeg (Reel) ignores it: so must TubeSearch, or they'd differ */
      snprintf (err, errlen, "http_proxy should start http:// (as http://host:port/); "
                "Reel ignores it otherwise, so TubeSearch does too");
      return -1;
    }
  s = spec + 7;
  end = s + strcspn (s, "/?#");
  /* user:password@ (up to the last @ before the host, as FFmpeg reads it) */
  at = NULL;
  for (const char *p = s; p < end; p++)
    if (*p == '@')
      at = p;
  if (at)
    {
      char cred[300];
      size_t n;
      if ((size_t) (at - s) >= 290)
        {
          snprintf (err, errlen, "The user name and password in http_proxy are too long for TubeSearch");
          return -1;
        }
      n = pct_decode (s, (size_t) (at - s), cred, 0, sizeof cred);
      base64 ((const unsigned char *) cred, n, proxy.auth, sizeof proxy.auth);
      memset (cred, 0, sizeof cred);
      s = at + 1;
    }
  if (*s == '[')
    {
      snprintf (err, errlen, "TubeSearch can't use a proxy given by its IPv6 address");
      memset (&proxy, 0, sizeof proxy);
      return -1;
    }
  colon = memchr (s, ':', (size_t) (end - s));
  hl = (size_t) ((colon ? colon : end) - s);
  if (!hl || hl >= sizeof proxy.host)
    {
      snprintf (err, errlen, hl ? "The proxy's name in http_proxy is too long" : "http_proxy has no proxy name in it");
      memset (&proxy, 0, sizeof proxy);
      return -1;
    }
  memcpy (proxy.host, s, hl);
  proxy.host[hl] = 0;
  {
    char *e;
    long p = colon ? strtol (colon + 1, &e, 10) : 0;
    if (!colon || e != end || p < 1 || p > 65535)
      {
        snprintf (err, errlen, "http_proxy needs the proxy's port, as http://%s:3128/ (Reel's FFmpeg needs it too)",
                  proxy.host);
        memset (&proxy, 0, sizeof proxy);
        return -1;
      }
    proxy.port = (int) p;
  }
  have_proxy = 1;
  snprintf (name, sizeof name, "%s:%d", proxy.host, proxy.port);
  return 0;
}

/* A variable's value, read now (on RISC OS from the system, as Reel will
   read it when it starts; UnixLib's getenv has only the start-up copy). */
static const char *read_var (const char *var, char *buf, size_t size)
{
#if defined __riscos && !defined __FAKE_RISCOS
  _kernel_swi_regs r;
  r.r[0] = (int) var;
  r.r[1] = (int) buf;
  r.r[2] = (int) size - 1;
  r.r[3] = 0;
  r.r[4] = 3;                                   /* expanded to a string */
  if (_kernel_swi (XOS_ReadVarVal, &r, &r))
    return NULL;
  buf[r.r[2]] = 0;
  return buf;
#else
  const char *v = getenv (var);
  if (!v)
    return NULL;
  snprintf (buf, size, "%s", v);
  return buf;
#endif
}

int proxy_refresh (const char *override, char *err, size_t errlen)
{
  static char last[1600];
  static int done;
  char spec[512], no[1024], now[1600];
  const char *sp, *np;
  if (override)
    sp = !strcmp (override, "none") ? "" : override;
  else
    sp = read_var ("http_proxy", spec, sizeof spec);
  np = read_var ("no_proxy", no, sizeof no);
  snprintf (now, sizeof now, "%s\n%s", sp ? sp : "", np ? np : "");
  if (done && !strcmp (now, last))
    return 0;
  done = 1;
  snprintf (last, sizeof last, "%s", now);
  if (proxy_set (sp, np, err, errlen))
    return -1;
  return 1;
}

/* FFmpeg's match_host_pattern */
static int match_host (const char *pattern, size_t len_p, const char *host)
{
  size_t len_h = strlen (host);
  if (len_p == 1 && *pattern == '*')
    return 1;
  if (len_p && *pattern == '*')
    pattern++, len_p--;
  if (len_p && *pattern == '.')
    pattern++, len_p--;
  if (len_p > len_h || strncmp (pattern, host + len_h - len_p, len_p))
    return 0;
  return len_h == len_p || host[len_h - len_p - 1] == '.';
}

static int bypass (const char *host)
{
  const char *p = no_list;
  for (;;)
    {
      size_t n;
      p += strspn (p, " ,");
      n = strcspn (p, " ,");
      if (match_host (p, n, host))
        return 1;
      if (!p[n])
        return 0;
      p += n + 1;
    }
}

const proxy_cfg *proxy_for (const char *host)
{
  return have_proxy && !bypass (host) ? &proxy : NULL;
}

const char *proxy_name (void)
{
  return name;
}

int proxy_tunnel_init (proxy_tunnel *t, const proxy_cfg *p, const char *host, int port)
{
  int n;
  memset (t, 0, sizeof *t);
  n = snprintf (t->out, sizeof t->out, "CONNECT %s:%d HTTP/1.1\r\nHost: %s:%d\r\n%s%s%s"
                "User-Agent: TubeSearch\r\n\r\n", host, port, host, port,
                p->auth[0] ? "Proxy-Authorization: Basic " : "", p->auth, p->auth[0] ? "\r\n" : "");
  if (n < 0 || n >= (int) sizeof t->out)
    return -1;
  t->out_len = n;
  return 0;
}

int proxy_reply_status (const char *head, size_t len)
{
  int code = 0, i = 0;
  if (len < 12 || strncmp (head, "HTTP/1.", 7) || head[8] != ' ')
    return -1;
  for (i = 9; i < 12; i++)
    {
      if (!isdigit ((unsigned char) head[i]))
        return -1;
      code = code * 10 + head[i] - '0';
    }
  return code >= 100 ? code : -1;
}
