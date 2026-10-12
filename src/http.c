/* http.c - see http.h. Part of TubeSearch. MIT licence. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "http.h"

#define MAX_LANES 6
#define MAX_HEAD (64 * 1024)
#define MAX_BODY (32 * 1024 * 1024)
#define CONNECT_TIMEOUT_CS 12000     /* 2 minutes: AcornSSL may be asking about a certificate */
#define IDLE_TIMEOUT_CS 3000         /* 30 s with nothing arriving */
#define KEEPALIVE_CS 4000            /* reuse an idle connection for 40 s */
#define POLL_BYTES (512 * 1024)      /* at most this much per lane per http_poll */

enum { L_NONE, L_CONNECTING, L_SENDING, L_RECEIVING, L_IDLE };
enum { B_LENGTH, B_CHUNKED, B_CLOSE, B_NONE };
enum { C_SIZE, C_DATA, C_DATA_END, C_TRAILER, C_DONE };

struct http_req {
  http_req *next;
  char host[96];
  int tag;
  http_done_fn done;
  void *ctx;
  int cancelled;
  int retried;
  int is_get, redirects;
  char *headers;              /* the extra header lines, kept for a redirect */

  char *out;                  /* the request, as sent */
  size_t outlen, sent;

  int status;
  char err[160];
  char *head;                 /* response headers, until parsed */
  size_t headlen, headcap;
  int head_done;
  int body_mode;
  long remaining;             /* B_LENGTH: bytes still to come; C_DATA: in this chunk */
  int chunk_state;
  char line[64];              /* a chunk-size or trailer line */
  size_t linelen;
  int conn_close;             /* the server says it will close */
  char *body;
  size_t bodylen, bodycap;
  int got_bytes;              /* any of the response arrived */
};

typedef struct {
  char host[96];
  void *conn;
  int state;
  unsigned since;             /* when the state started, or the last progress */
  int uses;                   /* requests done on this connection */
  http_req *cur;
  http_req *queue;
} lane;

static const http_transport *T;
static lane lanes[MAX_LANES];
static int connections_made;

void http_init (const http_transport *t)
{
  T = t;
}

int http_connections_made (void)
{
  return connections_made;
}

static unsigned now (void)
{
  return T && T->now_cs ? T->now_cs () : 0;
}

static void free_req (http_req *r)
{
  free (r->headers);
  free (r->out);
  free (r->head);
  free (r->body);
  free (r);
}

static lane *lane_for (const char *host)
{
  lane *free_lane = NULL;
  for (int i = 0; i < MAX_LANES; i++)
    {
      if (lanes[i].host[0] && !strcmp (lanes[i].host, host))
        return &lanes[i];
      if (!free_lane && !lanes[i].host[0])
        free_lane = &lanes[i];
    }
  if (!free_lane)            /* all in use: reuse one with nothing to do */
    for (int i = 0; i < MAX_LANES; i++)
      if (!lanes[i].cur && !lanes[i].queue)
        {
          if (lanes[i].conn)
            T->close (lanes[i].conn);
          memset (&lanes[i], 0, sizeof lanes[i]);
          free_lane = &lanes[i];
          break;
        }
  if (free_lane)
    {
      memset (free_lane, 0, sizeof *free_lane);
      snprintf (free_lane->host, sizeof free_lane->host, "%s", host);
    }
  return free_lane;
}

/* The request text in r->out. 0, or -1 out of memory. */
static int build_out (http_req *r, const char *method, const char *path, const char *headers,
                      const char *body, size_t bodylen)
{
  size_t hl = headers ? strlen (headers) : 0;
  size_t cap = strlen (method) + strlen (path) + strlen (r->host) + hl + bodylen + 128;
  char *o = malloc (cap);
  size_t at;
  if (!o)
    return -1;
  at = (size_t) snprintf (o, cap, "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: keep-alive\r\n", method, path, r->host);
  if (hl)
    {
      memcpy (o + at, headers, hl);
      at += hl;
    }
  if (body)
    at += (size_t) snprintf (o + at, cap - at, "Content-Length: %lu\r\n", (unsigned long) bodylen);
  memcpy (o + at, "\r\n", 2);
  at += 2;
  if (body && bodylen)
    {
      memcpy (o + at, body, bodylen);
      at += bodylen;
    }
  free (r->out);
  r->out = o;
  r->outlen = at;
  r->sent = 0;
  return 0;
}

/* Puts r at the end of its host's queue. 0, or -1 (no lane free). */
static int enqueue (http_req *r)
{
  lane *l = lane_for (r->host);
  http_req **q;
  if (!l)
    return -1;
  r->next = NULL;
  for (q = &l->queue; *q; q = &(*q)->next)
    ;
  *q = r;
  return 0;
}

static http_req *make (const char *method, const char *host, const char *path, const char *headers,
                       const char *body, size_t bodylen, int tag, http_done_fn done, void *ctx)
{
  http_req *r;
  if (!T || strlen (host) >= sizeof r->host)
    return NULL;
  if (!(r = calloc (1, sizeof *r)))
    return NULL;
  snprintf (r->host, sizeof r->host, "%s", host);
  r->tag = tag;
  r->done = done;
  r->ctx = ctx;
  r->is_get = !strcmp (method, "GET");
  if ((headers && !(r->headers = strdup (headers)))
      || build_out (r, method, path, headers, body, bodylen) || enqueue (r))
    {
      free_req (r);
      return NULL;
    }
  return r;
}

http_req *http_get (const char *host, const char *path, const char *headers,
                    int tag, http_done_fn done, void *ctx)
{
  return make ("GET", host, path, headers, NULL, 0, tag, done, ctx);
}

http_req *http_post (const char *host, const char *path, const char *headers,
                     const char *body, size_t bodylen, int tag, http_done_fn done, void *ctx)
{
  return make ("POST", host, path, headers, body ? body : "", body ? bodylen : 0, tag, done, ctx);
}

int http_status (const http_req *r) { return r->status; }
const char *http_error (const http_req *r) { return r->err; }

const char *http_body (const http_req *r, size_t *len)
{
  if (len)
    *len = r->bodylen;
  return r->body ? r->body : "";
}

char *http_take_body (http_req *r, size_t *len)
{
  char *b = r->body;
  if (len)
    *len = r->bodylen;
  if (!b)
    b = calloc (1, 1);
  r->body = NULL;
  r->bodylen = r->bodycap = 0;
  return b;
}

/* Removes r from its lane's queue; 1 if it was there. */
static int unqueue (http_req *r)
{
  for (int i = 0; i < MAX_LANES; i++)
    for (http_req **q = &lanes[i].queue; *q; q = &(*q)->next)
      if (*q == r)
        {
          *q = r->next;
          return 1;
        }
  return 0;
}

void http_cancel (http_req *r)
{
  if (!r)
    return;
  if (unqueue (r))
    free_req (r);
  else
    r->cancelled = 1;          /* in progress: freed when it ends */
}

void http_cancel_tag (int tag)
{
  for (int i = 0; i < MAX_LANES; i++)
    {
      http_req **q = &lanes[i].queue;
      while (*q)
        if ((*q)->tag == tag)
          {
            http_req *r = *q;
            *q = r->next;
            free_req (r);
          }
        else
          q = &(*q)->next;
      if (lanes[i].cur && lanes[i].cur->tag == tag)
        lanes[i].cur->cancelled = 1;
    }
}

static void close_lane (lane *l)
{
  if (l->conn)
    T->close (l->conn);
  l->conn = NULL;
  l->state = L_NONE;
  l->uses = 0;
}

static const char *header_value (const char *head, const char *name);

/* A GET answered 301/302/303/307/308 with a Location: sent again there, as
   the same http_req (so a caller's pointer stays good). 1 if it was. */
static int redirect (http_req *r)
{
  char host[96], path[2048];
  const char *loc, *p;
  size_t n;
  if (!r->is_get || !r->head || r->redirects >= 5
      || !(r->status == 301 || r->status == 302 || r->status == 303 || r->status == 307 || r->status == 308)
      || !(loc = header_value (r->head, "Location")))
    return 0;
  n = strcspn (loc, "\r\n");
  if (!strncmp (loc, "https://", 8) || !strncmp (loc, "//", 2))
    {
      p = loc + (loc[0] == '/' ? 2 : 8);
      size_t hl = strcspn (p, "/\r\n");
      if (!hl || hl >= sizeof host)
        return 0;
      memcpy (host, p, hl);
      host[hl] = 0;
      p += hl;
      n -= (size_t) (p - loc);
    }
  else if (loc[0] == '/')
    {
      snprintf (host, sizeof host, "%s", r->host);
      p = loc;
    }
  else
    return 0;                                  /* (http://, or relative: not followed) */
  if (n == 0)
    {
      p = "/";
      n = 1;
    }
  if (n >= sizeof path)
    return 0;
  memcpy (path, p, n);
  path[n] = 0;
  if (strchr (host, ':'))
    return 0;                                  /* (another port: not followed) */
  snprintf (r->host, sizeof r->host, "%s", host);
  free (r->head);
  free (r->body);
  r->head = r->body = NULL;
  r->headlen = r->headcap = r->bodylen = r->bodycap = 0;
  r->head_done = r->status = r->conn_close = r->got_bytes = r->retried = 0;
  r->body_mode = 0;
  r->remaining = 0;
  r->linelen = 0;
  r->err[0] = 0;
  r->redirects++;
  if (build_out (r, "GET", path, r->headers, NULL, 0) || enqueue (r))
    {
      snprintf (r->err, sizeof r->err, "out of memory");
      return 0;
    }
  return 1;
}

/* The current request is over (r->status set, or r->err): call back. */
static void finish (lane *l, int keep)
{
  http_req *r = l->cur;
  l->cur = NULL;
  if (keep && l->conn && !r->conn_close)
    {
      l->state = L_IDLE;
      l->since = now ();
    }
  else
    close_lane (l);
  if (!r->cancelled && redirect (r))
    return;                                    /* (the same request, on its way to the new address) */
  if (!r->cancelled && r->done)
    r->done (r, r->ctx);
  free_req (r);
}

static void fail (lane *l, const char *fmt, const char *detail)
{
  http_req *r = l->cur;
  /* A reused connection the server had closed: once more on a new one. */
  if (r && !r->got_bytes && !r->retried && l->uses > 0)
    {
      close_lane (l);
      r->retried = 1;
      r->sent = 0;
      r->next = l->queue;
      l->queue = r;
      l->cur = NULL;
      return;
    }
  if (r)
    {
      snprintf (r->err, sizeof r->err, fmt, detail ? detail : "");
      r->status = 0;
      finish (l, 0);
    }
  else
    close_lane (l);
}

static int body_add (http_req *r, const char *d, size_t n)
{
  if (r->bodylen + n + 1 > r->bodycap)
    {
      size_t cap = r->bodycap ? r->bodycap * 2 : 16384;
      char *b;
      while (cap < r->bodylen + n + 1)
        cap *= 2;
      if (cap > MAX_BODY + 1 || !(b = realloc (r->body, cap)))
        return -1;
      r->body = b;
      r->bodycap = cap;
    }
  memcpy (r->body + r->bodylen, d, n);
  r->bodylen += n;
  r->body[r->bodylen] = 0;
  return 0;
}

static const char *header_value (const char *head, const char *name)
{
  size_t nl = strlen (name);
  const char *p = strstr (head, "\r\n");
  while (p && p[2] && !(p[2] == '\r' && p[3] == '\n'))
    {
      p += 2;
      if (!strncasecmp (p, name, nl) && p[nl] == ':')
        {
          p += nl + 1;
          while (*p == ' ' || *p == '\t')
            p++;
          return p;
        }
      p = strstr (p, "\r\n");
    }
  return NULL;
}

static int value_has (const char *v, const char *word)
{
  size_t wl = strlen (word);
  if (!v)
    return 0;
  for (const char *p = v; *p && *p != '\r'; p++)
    if (!strncasecmp (p, word, wl))
      return 1;
  return 0;
}

/* The status line and headers have arrived (head ends with \r\n\r\n). */
static int parse_head (http_req *r)
{
  const char *h = r->head, *v;
  int major = 1, minor = 1;
  if (sscanf (h, "HTTP/%d.%d %d", &major, &minor, &r->status) != 3)
    return -1;
  if (r->status >= 100 && r->status < 200)
    return 1;                                  /* 100 Continue etc: another head follows */
  v = header_value (h, "Connection");
  r->conn_close = value_has (v, "close") || (major == 1 && minor == 0 && !value_has (v, "keep-alive"));
  if (r->status == 204 || r->status == 304)
    r->body_mode = B_NONE;
  else if (value_has (header_value (h, "Transfer-Encoding"), "chunked"))
    {
      r->body_mode = B_CHUNKED;
      r->chunk_state = C_SIZE;
    }
  else if ((v = header_value (h, "Content-Length")))
    {
      r->body_mode = B_LENGTH;
      r->remaining = strtol (v, NULL, 10);
      if (r->remaining < 0 || r->remaining > MAX_BODY)
        return -1;
    }
  else
    {
      r->body_mode = B_CLOSE;
      r->conn_close = 1;
    }
  r->head_done = 1;
  return 0;
}

/* Takes received bytes. Returns how many were used (the rest belong to
   nothing: a server that sent more than it said), 1 in *complete when the
   response is whole, or -1 on a malformed response. */
static long consume (http_req *r, const char *d, size_t n, int *complete)
{
  size_t used = 0;
  *complete = 0;
  r->got_bytes = 1;
  while (used < n && !*complete)
    {
      if (!r->head_done)
        {
          /* gather the head; it ends at the first blank line */
          char c = d[used++];
          if (r->headlen + 2 > r->headcap)
            {
              size_t cap = r->headcap ? r->headcap * 2 : 1024;
              char *nh;
              if (cap > MAX_HEAD || !(nh = realloc (r->head, cap)))
                return -1;
              r->head = nh;
              r->headcap = cap;
            }
          r->head[r->headlen++] = c;
          r->head[r->headlen] = 0;
          if (r->headlen >= 4 && !memcmp (r->head + r->headlen - 4, "\r\n\r\n", 4))
            {
              int k = parse_head (r);
              if (k < 0)
                return -1;
              if (k == 1)
                {
                  r->headlen = 0;              /* 1xx: wait for the real one */
                  continue;
                }
              if (r->body_mode == B_NONE || (r->body_mode == B_LENGTH && r->remaining == 0))
                *complete = 1;
            }
          continue;
        }
      switch (r->body_mode)
        {
        case B_LENGTH:
          {
            size_t take = n - used < (size_t) r->remaining ? n - used : (size_t) r->remaining;
            if (body_add (r, d + used, take))
              return -1;
            used += take;
            r->remaining -= (long) take;
            if (!r->remaining)
              *complete = 1;
            break;
          }
        case B_CLOSE:
          if (body_add (r, d + used, n - used))
            return -1;
          used = n;
          break;
        case B_CHUNKED:
          switch (r->chunk_state)
            {
            case C_SIZE:
            case C_TRAILER:
              {
                char c = d[used++];
                if (r->linelen + 1 >= sizeof r->line)
                  {
                    if (r->chunk_state == C_SIZE)
                      return -1;
                    r->linelen = 1;            /* (a long trailer line: only its end matters) */
                    r->line[0] = 'x';
                  }
                r->line[r->linelen++] = c;
                r->line[r->linelen] = 0;
                if (r->linelen >= 2 && r->line[r->linelen - 2] == '\r' && c == '\n')
                  {
                    if (r->chunk_state == C_SIZE)
                      {
                        char *end;
                        long size = strtol (r->line, &end, 16);
                        if (end == r->line || size < 0 || size > MAX_BODY)
                          return -1;
                        r->remaining = size;
                        r->chunk_state = size ? C_DATA : C_TRAILER;
                      }
                    else if (r->linelen == 2)
                      {
                        r->chunk_state = C_DONE;
                        *complete = 1;
                      }
                    r->linelen = 0;
                  }
                break;
              }
            case C_DATA:
              {
                size_t take = n - used < (size_t) r->remaining ? n - used : (size_t) r->remaining;
                if (body_add (r, d + used, take))
                  return -1;
                used += take;
                r->remaining -= (long) take;
                if (!r->remaining)
                  {
                    r->chunk_state = C_DATA_END;
                    r->linelen = 0;
                  }
                break;
              }
            case C_DATA_END:
              {
                char c = d[used++];
                if (c == '\n')
                  r->chunk_state = C_SIZE;
                else if (c != '\r')
                  return -1;
                break;
              }
            }
          break;
        default:
          *complete = 1;
          break;
        }
    }
  return (long) used;
}

/* One lane: as far as it goes without waiting. Returns 1 if it moved. */
static int step (lane *l)
{
  char buf[16384];
  char err[128];
  int moved = 0;
  size_t budget = POLL_BYTES;

  for (;;)
    {
      unsigned t = now ();
      switch (l->state)
        {
        case L_IDLE:
          if (!l->queue)
            {
              if (t - l->since > KEEPALIVE_CS)
                close_lane (l);
              return moved;
            }
          if (t - l->since > KEEPALIVE_CS)
            {
              close_lane (l);
              continue;
            }
          /* send the next on this connection */
          /* FALLTHROUGH */
        case L_NONE:
          if (!l->queue)
            return moved;
          l->cur = l->queue;
          l->queue = l->cur->next;
          l->cur->next = NULL;
          if (l->cur->cancelled)
            {
              free_req (l->cur);
              l->cur = NULL;
              continue;
            }
          moved = 1;
          if (!l->conn)
            {
              err[0] = 0;
              l->conn = T->open (l->host, 443, 1, err, sizeof err);
              if (!l->conn)
                {
                  fail (l, "%s", err[0] ? err : "can't connect");
                  continue;
                }
              connections_made++;
              l->uses = 0;
              l->state = L_CONNECTING;
            }
          else
            l->state = L_SENDING;
          l->since = t;
          continue;

        case L_CONNECTING:
          {
            int k;
            err[0] = 0;
            k = T->ready (l->conn, err, sizeof err);
            if (k < 0)
              {
                fail (l, "%s", err[0] ? err : "connection failed");
                moved = 1;
                continue;
              }
            if (k == 0)
              {
                if (t - l->since > CONNECT_TIMEOUT_CS)
                  {
                    fail (l, "%s", "timed out connecting");
                    moved = 1;
                    continue;
                  }
                return moved;
              }
            l->state = L_SENDING;
            l->since = t;
            moved = 1;
            continue;
          }

        case L_SENDING:
          {
            http_req *r = l->cur;
            int k;
            if (r->cancelled && r->sent == 0)
              {
                finish (l, 1);
                moved = 1;
                continue;
              }
            k = T->send (l->conn, r->out + r->sent, (int) (r->outlen - r->sent));
            if (k == HTTP_AGAIN)
              {
                if (t - l->since > IDLE_TIMEOUT_CS)
                  {
                    fail (l, "%s", "timed out sending");
                    moved = 1;
                    continue;
                  }
                return moved;
              }
            if (k < 0)
              {
                fail (l, "%s", "the connection broke while sending");
                moved = 1;
                continue;
              }
            if (k > 0)
              {
                r->sent += (size_t) k;
                l->since = t;
                moved = 1;
              }
            if (r->sent == r->outlen)
              {
                /* (r->out is kept: sent again if the connection was stale) */
                l->state = L_RECEIVING;
                l->since = t;
              }
            continue;
          }

        case L_RECEIVING:
          {
            http_req *r = l->cur;
            int k, complete = 0;
            long used;
            if (!budget)
              return moved;
            k = T->recv (l->conn, buf, (int) (sizeof buf < budget ? sizeof buf : budget));
            if (k == HTTP_AGAIN)
              {
                if (t - l->since > IDLE_TIMEOUT_CS)
                  {
                    fail (l, "%s", "timed out waiting for the server");
                    moved = 1;
                    continue;
                  }
                return moved;
              }
            if (k == 0 || k == HTTP_FAIL)
              {
                moved = 1;
                if (r->head_done && r->body_mode == B_CLOSE)
                  {
                    r->conn_close = 1;
                    finish (l, 0);
                    continue;
                  }
                fail (l, "%s", k == 0 ? "the server closed the connection" : "the connection broke");
                continue;
              }
            moved = 1;
            budget -= (size_t) k;
            l->since = t;
            used = consume (r, buf, (size_t) k, &complete);
            if (used < 0)
              {
                r->retried = 1;                  /* (not worth trying again) */
                fail (l, "%s", r->bodylen + 1 >= MAX_BODY ? "the answer was too big" : "the server's answer was malformed");
                continue;
              }
            if (complete)
              {
                l->uses++;
                if ((size_t) used < (size_t) k)
                  r->conn_close = 1;             /* extra bytes: don't trust this connection */
                finish (l, 1);
              }
            continue;
          }
        }
      return moved;
    }
}

int http_poll (void)
{
  for (int i = 0; i < MAX_LANES; i++)
    if (lanes[i].host[0])
      step (&lanes[i]);
  return http_busy ();
}

int http_busy (void)
{
  for (int i = 0; i < MAX_LANES; i++)
    if (lanes[i].cur || lanes[i].queue)
      return 1;
  return 0;
}

void http_shutdown (void)
{
  for (int i = 0; i < MAX_LANES; i++)
    {
      lane *l = &lanes[i];
      while (l->queue)
        {
          http_req *r = l->queue;
          l->queue = r->next;
          free_req (r);
        }
      if (l->cur)
        free_req (l->cur);
      l->cur = NULL;
      if (l->conn && T)
        T->close (l->conn);
      memset (l, 0, sizeof *l);
    }
}
