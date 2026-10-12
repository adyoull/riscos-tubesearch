/* api.c - see api.h. Part of TubeSearch. MIT licence. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "api.h"
#include "http.h"

#define TAG_API 1

const char *api_dump_dir;

enum { OP_LIST, OP_PLAY };
enum { V_NONE, V_ASKING, V_DONE };

struct api_op {
  api_op *next;               /* while waiting for the visitor ID */
  int type, kind, more, maxq;
  char *arg;
  yt_list *list;
  api_list_cb list_cb;
  api_play_cb play_cb;
  void *ctx;
  http_req *req;
};

static int visitor_state;
static api_op *waiting;

static void dump (const char *what, const char *body, size_t len)
{
  static int n;
  char path[512];
  FILE *f;
  if (!api_dump_dir)
    return;
  snprintf (path, sizeof path, "%s/%02d-%s.json", api_dump_dir, ++n, what);
  if ((f = fopen (path, "wb")))
    {
      fwrite (body, 1, len, f);
      fclose (f);
    }
}

static void free_op (api_op *op)
{
  free (op->arg);
  free (op);
}

static void done (http_req *r, void *ctx)
{
  api_op *op = ctx;
  char err[256];
  size_t len;
  const char *body = http_body (r, &len);
  int status = http_status (r);

  op->req = NULL;
  dump (op->type == OP_PLAY ? "player" : op->kind == YT_LIST_SEARCH ? "search"
        : op->kind == YT_LIST_RELATED ? "next" : "browse", body, len);
  err[0] = 0;
  if (!status)
    snprintf (err, sizeof err, "Couldn't reach YouTube: %s", http_error (r));
  else if (status != 200 && (len == 0 || body[0] != '{'))
    snprintf (err, sizeof err, "YouTube answered %d", status);

  if (op->type == OP_PLAY)
    {
      yt_play p;
      if (!err[0] && yt_parse_player (body, len, op->maxq, &p, err, sizeof err) == 0)
        {
          op->play_cb (&p, NULL, op->ctx);
          yt_play_free (&p);
        }
      else
        op->play_cb (NULL, err, op->ctx);
    }
  else
    {
      int added = -1;
      if (!err[0])
        added = yt_parse_list (body, len, op->kind, op->list, err, sizeof err);
      op->list_cb (added, added < 0 ? err : NULL, op->ctx);
    }
  free_op (op);
}

/* Sends op's request; -1 if it couldn't be made (out of memory). */
static int send_op (api_op *op)
{
  yt_req *q;
  if (op->type == OP_PLAY)
    q = yt_req_player (op->arg);
  else if (op->more)
    q = yt_req_more (op->kind, op->arg);
  else if (op->kind == YT_LIST_SEARCH)
    q = yt_req_search (op->arg);
  else if (op->kind == YT_LIST_RELATED)
    q = yt_req_related (op->arg);
  else
    q = yt_req_channel (op->arg);
  if (q)
    op->req = http_post (q->host, q->path, q->headers, q->body, strlen (q->body), TAG_API, done, op);
  yt_req_free (q);
  return op->req ? 0 : -1;
}

/* Sends the operations that waited for the visitor ID. */
static void send_waiting (void)
{
  while (waiting)
    {
      api_op *op = waiting;
      waiting = op->next;
      op->next = NULL;
      if (send_op (op))
        {
          if (op->type == OP_PLAY)
            op->play_cb (NULL, "Out of memory", op->ctx);
          else
            op->list_cb (-1, "Out of memory", op->ctx);
          free_op (op);
        }
    }
}

static void visitor_done (http_req *r, void *ctx)
{
  size_t len;
  const char *body = http_body (r, &len);
  (void) ctx;
  dump ("visitor", body, len);
  /* without one, carry on anyway: YouTube may still answer */
  if (http_status (r) == 200)
    yt_parse_visitor (body, len);
  visitor_state = yt_visitor ? V_DONE : V_NONE;
  send_waiting ();
}

/* NULL if the request couldn't even be made (the callback isn't called). */
static api_op *start (api_op *op)
{
  if (visitor_state == V_DONE)
    {
      if (send_op (op))
        {
          free_op (op);
          return NULL;
        }
      return op;
    }
  op->next = waiting;
  waiting = op;
  if (visitor_state == V_NONE)
    {
      yt_req *q = yt_req_visitor ();
      http_req *r = q ? http_post (q->host, q->path, q->headers, q->body, strlen (q->body), TAG_API + 1,
                                   visitor_done, NULL) : NULL;
      yt_req_free (q);
      if (r)
        visitor_state = V_ASKING;
      else
        {
          waiting = op->next;               /* no visitor ID then: straight on */
          op->next = NULL;
          if (send_op (op))
            {
              free_op (op);
              return NULL;
            }
        }
    }
  return op;
}

api_op *api_list (int kind, const char *arg, int more, yt_list *list, api_list_cb cb, void *ctx)
{
  api_op *op = calloc (1, sizeof *op);
  if (!op || !(op->arg = strdup (arg)))
    {
      free (op);
      return NULL;
    }
  op->type = OP_LIST;
  op->kind = kind;
  op->more = more;
  op->list = list;
  op->list_cb = cb;
  op->ctx = ctx;
  return start (op);
}

api_op *api_play (const char *video_id, int maxq, api_play_cb cb, void *ctx)
{
  api_op *op = calloc (1, sizeof *op);
  if (!op || !(op->arg = strdup (video_id)))
    {
      free (op);
      return NULL;
    }
  op->type = OP_PLAY;
  op->maxq = maxq;
  op->play_cb = cb;
  op->ctx = ctx;
  return start (op);
}

void api_cancel (api_op *op)
{
  if (!op)
    return;
  for (api_op **w = &waiting; *w; w = &(*w)->next)
    if (*w == op)
      {
        *w = op->next;
        free_op (op);
        return;
      }
  if (op->req)
    {
      http_cancel (op->req);
      free_op (op);
    }
}
