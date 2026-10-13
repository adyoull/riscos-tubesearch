/* Tests proxy.c and proxy_tunnel.c: the proxy setting's forms, no_proxy,
   the CONNECT request, and reading the proxy's answer over a socket pair
   (in pieces, refusals, and never reading into the TLS bytes after it).
   Part of TubeSearch. MIT licence. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include "../src/proxy.h"

static int failed, checks;
#define CHECK(c) do { checks++; if (!(c)) { failed++; fprintf (stderr, "FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static void test_settings (void)
{
  char err[200];
  const proxy_cfg *p;
  proxy_tunnel t;
  CHECK (proxy_set ("", NULL, err, sizeof err) == 0 && !proxy_for ("www.youtube.com") && !*proxy_name ());
  CHECK (proxy_set (NULL, NULL, err, sizeof err) == 0 && !proxy_for ("www.youtube.com"));
  CHECK (proxy_set ("http://proxy.example:3128/", NULL, err, sizeof err) == 0);
  p = proxy_for ("www.youtube.com");
  CHECK (p && !strcmp (p->host, "proxy.example") && p->port == 3128 && !p->auth[0]);
  CHECK (!strcmp (proxy_name (), "proxy.example:3128"));
  CHECK (proxy_set ("http://10.0.0.2:8080", NULL, err, sizeof err) == 0 && proxy_for ("i.ytimg.com")
         && !strcmp (proxy_for ("i.ytimg.com")->host, "10.0.0.2"));
  /* as av_url_split: the host part ends at / ? or # */
  CHECK (proxy_set ("http://h.example:3128?x=1", NULL, err, sizeof err) == 0 && proxy_for ("a")->port == 3128);
  CHECK (proxy_set ("http://h.example:3128#frag", NULL, err, sizeof err) == 0 && proxy_for ("a")->port == 3128);
  /* user:password, %-decoded, up to the last @; base64 of "chris:p@ss:w" */
  CHECK (proxy_set ("http://chris:p%40ss:w@vps.example:8888", NULL, err, sizeof err) == 0);
  p = proxy_for ("www.youtube.com");
  CHECK (p && !strcmp (p->host, "vps.example") && p->port == 8888 && !strcmp (p->auth, "Y2hyaXM6cEBzczp3"));
  CHECK (proxy_set ("http://chris:p@ss@vps.example:1", NULL, err, sizeof err) == 0 && !strcmp (proxy_for ("x")->host, "vps.example")
         && !strcmp (proxy_for ("x")->auth, "Y2hyaXM6cEBzcw=="));
  CHECK (proxy_set ("http://bob@vps.example:1", NULL, err, sizeof err) == 0 && !strcmp (proxy_for ("x")->auth, "Ym9i"));
  /* what FFmpeg (Reel) wouldn't use isn't used, and is said so */
  CHECK (proxy_set ("vps.example:3128", NULL, err, sizeof err) < 0 && strstr (err, "http://") && !proxy_for ("x"));
  CHECK (proxy_set ("HTTP://vps.example:3128", NULL, err, sizeof err) < 0 && !proxy_for ("x"));
  CHECK (proxy_set ("socks5://vps.example:1080", NULL, err, sizeof err) < 0 && !proxy_for ("x"));
  CHECK (proxy_set ("http://vps.example/", NULL, err, sizeof err) < 0 && strstr (err, "port") && !proxy_for ("x"));
  CHECK (proxy_set ("http://vps.example:99999", NULL, err, sizeof err) < 0 && strstr (err, "port") && !proxy_for ("x"));
  CHECK (proxy_set ("http://chris:secret@vps.example:80x", NULL, err, sizeof err) < 0 && !strstr (err, "secret"));
  CHECK (proxy_set ("http://:3128", NULL, err, sizeof err) < 0 && strstr (err, "name"));
  CHECK (proxy_set ("http://[::1]:3128", NULL, err, sizeof err) < 0);
  {
    char big[400];
    memset (big, 'u', sizeof big);
    memcpy (big, "http://", 7);
    strcpy (big + 300, "@vps.example:1");
    CHECK (proxy_set (big, NULL, err, sizeof err) < 0 && strstr (err, "too long") && !proxy_for ("x"));
    /* the longest allowed fits the CONNECT request */
    memset (big + 7, 'u', 289);
    strcpy (big + 7 + 289, "@vps.example:1");
    CHECK (proxy_set (big, NULL, err, sizeof err) == 0 && strlen (proxy_for ("x")->auth) == 388);
    CHECK (proxy_tunnel_init (&t, proxy_for ("x"), "youtubei.googleapis.com", 443) == 0
           && !memcmp (t.out + t.out_len - 4, "\r\n\r\n", 4));
    memset (big, 'h', 300);
    big[300] = 0;
    CHECK (proxy_tunnel_init (&t, proxy_for ("x"), big, 443) < 0);
  }
  /* no_proxy, as FFmpeg's ff_http_match_no_proxy */
  CHECK (proxy_set ("http://vps.example:3128", "localhost, .lan,example.org *.googlevideo.com", err, sizeof err) == 0);
  CHECK (!proxy_for ("localhost") && !proxy_for ("nas.lan") && !proxy_for ("lan") && !proxy_for ("example.org")
         && !proxy_for ("www.example.org") && !proxy_for ("rr1.googlevideo.com"));
  CHECK (proxy_for ("www.youtube.com") && proxy_for ("notexample.org") && proxy_for ("www.EXAMPLE.org"));
  CHECK (proxy_set ("http://vps.example:3128", "*", err, sizeof err) == 0 && !proxy_for ("www.youtube.com"));
  CHECK (proxy_set ("http://vps.example:3128", " , ", err, sizeof err) == 0 && proxy_for ("www.youtube.com"));
  /* proxy_refresh: the variables, read again; 1 only when they change */
  setenv ("http_proxy", "http://envproxy:1234/", 1);
  unsetenv ("no_proxy");
  CHECK (proxy_refresh (NULL, err, sizeof err) == 1 && proxy_for ("a") && !strcmp (proxy_for ("a")->host, "envproxy"));
  CHECK (proxy_refresh (NULL, err, sizeof err) == 0 && proxy_for ("a"));
  setenv ("no_proxy", "a", 1);
  CHECK (proxy_refresh (NULL, err, sizeof err) == 1 && !proxy_for ("a") && proxy_for ("b"));
  setenv ("http_proxy", "envproxy:1234", 1);
  CHECK (proxy_refresh (NULL, err, sizeof err) == -1 && !proxy_for ("b"));
  CHECK (proxy_refresh (NULL, err, sizeof err) == 0);                   /* (reported once) */
  CHECK (proxy_refresh ("http://cli:9/", err, sizeof err) == 1 && !strcmp (proxy_for ("b")->host, "cli"));
  CHECK (proxy_refresh ("none", err, sizeof err) == 1 && !proxy_for ("b"));
  unsetenv ("http_proxy");
  unsetenv ("no_proxy");
  CHECK (proxy_refresh (NULL, err, sizeof err) == 1 && !proxy_for ("b"));  /* no_proxy went */
  CHECK (proxy_reply_status ("HTTP/1.1 200 Connection established\r\n\r\n", 39) == 200);
  CHECK (proxy_reply_status ("HTTP/1.0 407 Auth\r\n\r\n", 21) == 407);
  CHECK (proxy_reply_status ("SSH-2.0-OpenSSH\r\n\r\n", 19) < 0);
}

/* The tunnel over a socket pair: our end non-blocking, the "proxy" end
   written by the test. */
static int tunnel (const char *spec, const char *const *answer, char *req, size_t reqsize, char *err,
                   size_t errlen, char *after, size_t aftersize)
{
  int sv[2], k = 0;
  proxy_tunnel t;
  ssize_t n;
  if (socketpair (AF_UNIX, SOCK_STREAM, 0, sv))
    return -2;
  fcntl (sv[0], F_SETFL, fcntl (sv[0], F_GETFL) | O_NONBLOCK);
  proxy_set (spec, NULL, err, errlen);
  proxy_tunnel_init (&t, proxy_for ("www.youtube.com"), "www.youtube.com", 443);
  CHECK (proxy_tunnel_step (&t, sv[0], err, errlen) == 0);          /* sent; nothing back yet */
  n = read (sv[1], req, reqsize - 1);
  req[n > 0 ? n : 0] = 0;
  for (int i = 0; answer[i] && k == 0; i++)
    {
      if (*answer[i])
        CHECK (write (sv[1], answer[i], strlen (answer[i])) == (ssize_t) strlen (answer[i]));
      else
        close (sv[1]), sv[1] = -1;
      k = proxy_tunnel_step (&t, sv[0], err, errlen);
    }
  after[0] = 0;
  if (k == 1)
    {
      n = recv (sv[0], after, aftersize - 1, 0);
      after[n > 0 ? n : 0] = 0;
    }
  close (sv[0]);
  if (sv[1] >= 0)
    close (sv[1]);
  return k;
}

static void test_tunnel (void)
{
  char req[1000], err[300], after[100];
  static const char *const ok[] = { "HTTP/1.1 200 Connection established\r\n", "Proxy-Agent: x\r\n\r\n\x16\x03\x03 TLS",
                                    NULL };
  static const char *const pieces[] = { "HTTP/1.", "0 200 OK\r", "\n", "\r", "\n", NULL };
  static const char *const refused[] = { "HTTP/1.1 407 Proxy Authentication Required\r\nContent-Length: 5\r\n\r\nnope!",
                                         NULL };
  static const char *const forbidden[] = { "HTTP/1.1 403 Forbidden\r\n\r\n", NULL };
  static const char *const hangup[] = { "HTTP/1.1 200", "", NULL };
  CHECK (tunnel ("http://vps.example:3128", ok, req, sizeof req, err, sizeof err, after, sizeof after) == 1);
  CHECK (!strncmp (req, "CONNECT www.youtube.com:443 HTTP/1.1\r\nHost: www.youtube.com:443\r\n", 65));
  CHECK (!strstr (req, "Proxy-Authorization") && strstr (req, "\r\n\r\n") == req + strlen (req) - 4);
  CHECK (!strcmp (after, "\x16\x03\x03 TLS"));                     /* the server's bytes left for TLS */
  CHECK (tunnel ("http://u:pw@vps.example:3128", pieces, req, sizeof req, err, sizeof err, after, sizeof after) == 1);
  CHECK (strstr (req, "Proxy-Authorization: Basic dTpwdw==\r\n") != NULL);
  CHECK (tunnel ("http://vps.example:3128", refused, req, sizeof req, err, sizeof err, after, sizeof after) == -1);
  CHECK (strstr (err, "refused: 407 Proxy Authentication Required") && strstr (err, "user name and password"));
  CHECK (tunnel ("http://vps.example:3128", forbidden, req, sizeof req, err, sizeof err, after, sizeof after) == -1);
  CHECK (strstr (err, "vps.example:3128 refused: 403 Forbidden") != NULL);
  CHECK (tunnel ("http://vps.example:3128", hangup, req, sizeof req, err, sizeof err, after, sizeof after) == -1);
  CHECK (strstr (err, "closed the connection") != NULL);
}

int main (void)
{
  test_settings ();
  test_tunnel ();
  printf ("test_proxy: %d checks, %d failed\n", checks, failed);
  return failed != 0;
}
