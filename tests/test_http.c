/* Tests http.c against a fake transport: responses arriving in small
   random pieces, chunked bodies, keep-alive, a server that closed an idle
   connection, Connection: close, cancelling, malformed answers and time-
   outs. Part of TubeSearch. MIT licence. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "http.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* ---- the fake server ---------------------------------------------------- */

typedef struct { const char *bytes; int close_after; int drop; } canned;

#define MAX_CANNED 16
static struct { const char *host; canned c[MAX_CANNED]; int n, next; } servers[4];
static int nservers;

typedef struct fconn {
  int server;
  int ready_after;            /* polls before ready */
  int never_ready;
  char rx[65536]; size_t rxlen; size_t rxdone;
  char *tx; size_t txlen, txpos, txcap;
  int close_at_end;           /* close once tx is sent */
  int closed;
} fconn;

static unsigned clock_cs;
static unsigned fake_now (void) { return clock_cs; }
static int never_ready_next;

static void server (const char *host)
{
  servers[nservers].host = host;
  servers[nservers].n = servers[nservers].next = 0;
  nservers++;
}

static void reply (const char *bytes, int close_after, int drop)
{
  canned *c = &servers[nservers - 1].c[servers[nservers - 1].n++];
  c->bytes = bytes;
  c->close_after = close_after;
  c->drop = drop;
}

static void *f_open (const char *host, int port, int tls, char *err, size_t errlen)
{
  fconn *c;
  (void) port; (void) tls;
  for (int i = 0; i < nservers; i++)
    if (!strcmp (servers[i].host, host))
      {
        c = calloc (1, sizeof *c);
        c->server = i;
        c->ready_after = rand () % 3;
        c->never_ready = never_ready_next;
        return c;
      }
  snprintf (err, errlen, "no such host: %s", host);
  return NULL;
}

static int f_ready (void *p, char *err, size_t errlen)
{
  fconn *c = p;
  (void) err; (void) errlen;
  if (c->never_ready)
    return 0;
  return c->ready_after-- <= 0;
}

static void tx_add (fconn *c, const char *b)
{
  size_t l = strlen (b);
  if (c->txlen + l > c->txcap)
    {
      c->txcap = (c->txlen + l) * 2;
      c->tx = realloc (c->tx, c->txcap);
    }
  memcpy (c->tx + c->txlen, b, l);
  c->txlen += l;
}

/* Answers each whole request that has arrived. */
static void serve (fconn *c)
{
  for (;;)
    {
      char *start = c->rx + c->rxdone, *end;
      const char *cl;
      size_t need;
      c->rx[c->rxlen] = 0;
      if (!(end = strstr (start, "\r\n\r\n")))
        return;
      need = (size_t) (end + 4 - start);
      if ((cl = strstr (start, "Content-Length: ")) && cl < end)
        need += (size_t) atol (cl + 16);
      if (c->rxlen - c->rxdone < need)
        return;
      c->rxdone += need;
      {
        int s = c->server;
        canned *k = servers[s].next < servers[s].n ? &servers[s].c[servers[s].next++] : NULL;
        if (!k || k->drop)
          {
            c->closed = 1;
            return;
          }
        tx_add (c, k->bytes);
        if (k->close_after)
          c->close_at_end = 1;
      }
    }
}

static int f_send (void *p, const char *b, int len)
{
  fconn *c = p;
  int n;
  if (c->closed)
    return HTTP_FAIL;
  if (rand () % 4 == 0)
    return HTTP_AGAIN;
  n = 1 + rand () % (len < 300 ? len : 300);
  memcpy (c->rx + c->rxlen, b, (size_t) n);
  c->rxlen += (size_t) n;
  serve (c);
  return n;
}

static int f_recv (void *p, char *b, int len)
{
  fconn *c = p;
  int n;
  if (c->txpos == c->txlen)
    return c->closed || c->close_at_end ? 0 : HTTP_AGAIN;
  if (rand () % 3 == 0)
    return HTTP_AGAIN;
  n = 1 + rand () % 37;
  if (n > len) n = len;
  if ((size_t) n > c->txlen - c->txpos) n = (int) (c->txlen - c->txpos);
  memcpy (b, c->tx + c->txpos, (size_t) n);
  c->txpos += (size_t) n;
  return n;
}

static int closes;
static void f_close (void *p)
{
  fconn *c = p;
  closes++;
  free (c->tx);
  free (c);
}

static const http_transport fake = { f_open, f_ready, f_recv, f_send, f_close, fake_now };

/* ---- the client side ---------------------------------------------------- */

typedef struct { int called, status; char body[4096]; char err[160]; } result;

static void done (http_req *r, void *ctx)
{
  result *res = ctx;
  size_t len;
  const char *b = http_body (r, &len);
  res->called++;
  res->status = http_status (r);
  snprintf (res->body, sizeof res->body, "%.*s", (int) len, b);
  snprintf (res->err, sizeof res->err, "%s", http_error (r));
}

static void run (void)
{
  for (int i = 0; i < 200000 && http_poll (); i++)
    clock_cs += (i % 50) == 0;
}

static void reset (void)
{
  http_shutdown ();
  nservers = 0;
  never_ready_next = 0;
}

int main (void)
{
  result a, b, c, d;
  srand (12345);
  http_init (&fake);

  for (int round = 0; round < 50; round++)
    {
      /* keep-alive: content-length, chunked, and 1xx then 200 */
      reset ();
      server ("www.example.com");
      reply ("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 11\r\n\r\nhello world", 0, 0);
      reply ("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5;ext=1\r\nhello\r\n1\r\n \r\n0005\r\nworld\r\n0\r\nX-Trailer: 1\r\n\r\n", 0, 0);
      reply ("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 404 Not Found\r\ncontent-length: 4\r\n\r\nnope", 0, 0);
      memset (&a, 0, sizeof a); memset (&b, 0, sizeof b); memset (&c, 0, sizeof c);
      {
        int before = http_connections_made ();
        http_get ("www.example.com", "/a", NULL, 1, done, &a);
        http_post ("www.example.com", "/b", "Content-Type: application/json\r\n", "{\"x\":1}", 7, 1, done, &b);
        http_get ("www.example.com", "/c", NULL, 1, done, &c);
        run ();
        CHECK (a.called == 1 && a.status == 200 && !strcmp (a.body, "hello world"));
        CHECK (b.called == 1 && b.status == 200 && !strcmp (b.body, "hello world"));
        CHECK (c.called == 1 && c.status == 404 && !strcmp (c.body, "nope"));
        CHECK (http_connections_made () - before == 1);
      }

      /* the server closed the kept connection: tried again on a new one */
      reset ();
      server ("www.example.com");
      reply ("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", 0, 0);
      reply ("", 0, 1);                                    /* dropped */
      reply ("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nagain", 0, 0);
      memset (&a, 0, sizeof a); memset (&b, 0, sizeof b);
      http_get ("www.example.com", "/1", NULL, 1, done, &a);
      run ();
      http_get ("www.example.com", "/2", NULL, 1, done, &b);
      run ();
      CHECK (a.status == 200 && !strcmp (a.body, "ok"));
      CHECK (b.called == 1 && b.status == 200 && !strcmp (b.body, "again"));

      /* Connection: close, and a body that runs to the close */
      reset ();
      server ("www.example.com");
      reply ("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 3\r\n\r\none", 1, 0);
      reply ("HTTP/1.0 200 OK\r\n\r\nuntil the end", 1, 0);
      memset (&a, 0, sizeof a); memset (&b, 0, sizeof b);
      {
        int before = http_connections_made ();
        http_get ("www.example.com", "/1", NULL, 1, done, &a);
        http_get ("www.example.com", "/2", NULL, 1, done, &b);
        run ();
        CHECK (!strcmp (a.body, "one") && !strcmp (b.body, "until the end") && b.status == 200);
        CHECK (http_connections_made () - before == 2);
      }

      /* two hosts at once; cancelling (queued, and in progress) */
      reset ();
      server ("api.example.com");
      reply ("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\napi", 0, 0);
      reply ("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nxxx", 0, 0);
      server ("img.example.com");
      reply ("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nimg", 0, 0);
      reply ("HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nimg2", 0, 0);
      memset (&a, 0, sizeof a); memset (&b, 0, sizeof b); memset (&c, 0, sizeof c); memset (&d, 0, sizeof d);
      {
        http_req *rb, *rc;
        http_get ("api.example.com", "/1", NULL, 1, done, &a);
        rb = http_get ("api.example.com", "/2", NULL, 1, done, &b);
        rc = http_get ("img.example.com", "/1", NULL, 2, done, &c);
        http_get ("img.example.com", "/2", NULL, 2, done, &d);
        http_cancel (rb);                                  /* still queued */
        http_poll ();
        http_cancel (rc);                                  /* in progress */
        run ();
        CHECK (a.called == 1 && !strcmp (a.body, "api"));
        CHECK (b.called == 0 && c.called == 0);
        CHECK (d.called == 1 && (!strcmp (d.body, "img2") || !strcmp (d.body, "img")));  /* (rc may go before it was sent) */
      }
      reset ();
      server ("img.example.com");
      reply ("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nimg", 0, 0);
      memset (&a, 0, sizeof a); memset (&b, 0, sizeof b);
      http_get ("img.example.com", "/1", NULL, 7, done, &a);
      http_get ("img.example.com", "/2", NULL, 7, done, &b);
      http_cancel_tag (7);
      run ();
      CHECK (a.called == 0 && b.called == 0 && !http_busy ());

      /* malformed answers, unknown hosts, timeouts */
      reset ();
      server ("bad.example.com");
      reply ("SMTP ready\r\n\r\n", 0, 0);
      reply ("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n", 0, 0);
      memset (&a, 0, sizeof a); memset (&b, 0, sizeof b); memset (&c, 0, sizeof c);
      http_get ("bad.example.com", "/1", NULL, 1, done, &a);
      run ();
      http_get ("bad.example.com", "/2", NULL, 1, done, &b);
      http_get ("nowhere.example.com", "/", NULL, 1, done, &c);
      run ();
      CHECK (a.called == 1 && a.status == 0 && strstr (a.err, "malformed"));
      CHECK (b.called == 1 && b.status == 0 && strstr (b.err, "malformed"));
      CHECK (c.called == 1 && c.status == 0 && strstr (c.err, "no such host"));
      reset ();
      server ("slow.example.com");
      never_ready_next = 1;
      memset (&a, 0, sizeof a);
      http_get ("slow.example.com", "/", NULL, 1, done, &a);
      for (int i = 0; i < 100 && http_poll (); i++)
        clock_cs += 200;
      CHECK (a.called == 1 && a.status == 0 && strstr (a.err, "timed out"));
    }
  reset ();
  printf (failures ? "test_http: %d FAILED\n" : "test_http: all passed\n", failures);
  return failures != 0;
}
