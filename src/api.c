/* api.c - see api.h. Part of TubeSearch. MIT licence. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "api.h"
#include "http.h"
#include "solvejob.h"

#define TAG_API 1

#ifdef __riscos
#define SEP "."
#else
#define SEP "/"
#endif

const char *api_dump_dir;

enum { OP_LIST, OP_PLAY };
enum { V_NONE, V_ASKING, V_DONE };
/* a play: the player client, then (if it gives nothing playable) the TV
   client: the player's ID, its JavaScript, the TV answer, the solve */
enum { P_PLAYER, P_IFRAME, P_PLAYERJS, P_TV, P_SOLVE, P_HLS };

struct api_op {
  api_op *next;               /* while waiting for the visitor ID */
  int type, kind, more, maxq, date;
  char *arg;
  yt_list *list;
  api_list_cb list_cb;
  api_play_cb play_cb;
  void *ctx;
  http_req *req;
  /* plays */
  int state;
  char *first_err;            /* why the player client's answer wouldn't do */
  char player_id[16];
  int sts, retried;
  yt_play tv;                 /* the TV answer, waiting for its challenges */
  solvejob job;
  char version[32], lib[400], core[400];
  char job_path[400], out_path[400], player_path[400];
};

static int visitor_state;
static api_op *waiting;
static const api_tv_host *tv_host;
static int job_counter;

void api_set_tv_host (const api_tv_host *host)
{
  tv_host = host;
}

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
  if (op->type == OP_PLAY && op->state == P_SOLVE)
    {
      remove (op->job_path);
      remove (op->out_path);
    }
  yt_play_free (&op->tv);
  free (op->first_err);
  free (op->arg);
  free (op);
}

/* ---- plays ---------------------------------------------------------------- */

static void progress (api_op *op, const char *what)
{
  if (tv_host && tv_host->progress)
    tv_host->progress (what, op->ctx);
}

/* The play is over: the streams, or why not. */
static void play_finish (api_op *op, yt_play *p, const char *err)
{
  char msg[600];
  if (!p && op->first_err && err && strcmp (op->first_err, err))
    {
      snprintf (msg, sizeof msg, "%s (and as YouTube's TV app: %s)", op->first_err, err);
      err = msg;
    }
  op->play_cb (p, p ? NULL : err, op->ctx);
  free_op (op);
}

/* The player client's streams: as HLS when that's wanted (yt_want_hls),
   which needs the master playlist first; otherwise as they are. */
static void hls_done (http_req *r, void *ctx);
static void deliver (api_op *op, yt_play *p)
{
  char host[128], path[2048];
  if (p != &op->tv)
    {
      yt_play_free (&op->tv);
      op->tv = *p;                               /* (op owns the streams now) */
      memset (p, 0, sizeof *p);
    }
  if (yt_want_hls (&op->tv) && !yt_split_url (op->tv.hls_url, host, sizeof host, path, sizeof path))
    {
      static char headers[400];
      snprintf (headers, sizeof headers, "User-Agent: %s\r\n", yt_config_get ("media_user_agent"));
      op->state = P_HLS;
      if ((op->req = http_get (host, path, headers, TAG_API, hls_done, op)))
        return;
    }
  play_finish (op, &op->tv, NULL);
}

static void hls_done (http_req *r, void *ctx)
{
  api_op *op = ctx;
  size_t len;
  const char *body = http_body (r, &len);
  char *v = NULL, *a = NULL, codec[32], err[200];
  int q = 0;
  op->req = NULL;
  dump ("hls", body, len);
  if (http_status (r) == 200
      && !yt_hls_choose (body, len, op->tv.hls_url, op->maxq, &v, &a, &q, codec, sizeof codec, err, sizeof err))
    {
      free (op->tv.video_url);
      free (op->tv.audio_url);
      op->tv.video_url = v;
      op->tv.audio_url = a;
      op->tv.quality = q;
      op->tv.hls = 1;
      snprintf (op->tv.codec, sizeof op->tv.codec, "%s", codec);
    }
  /* (otherwise the DASH streams, or for a live stream the master playlist itself) */
  else if (!op->tv.live && op->tv.video_url && op->tv.audio_url)
    {
      free (op->tv.hls_url);
      op->tv.hls_url = NULL;
    }
  else
    {
      free (op->tv.video_url);
      free (op->tv.audio_url);
      op->tv.video_url = op->tv.audio_url = NULL;
    }
  play_finish (op, &op->tv, NULL);
}

static int tv_allowed (void)
{
  return tv_host && tv_host->solve && tv_host->scripts && strcmp (yt_config_get ("stream_client"), "player");
}

static void cache_file (const char *leaf, char *out, size_t size)
{
  snprintf (out, size, "%s" SEP "%s", tv_host->cache_dir, leaf);
}

static int read_sts (const char *id)
{
  char path[400], leaf[64];
  FILE *f;
  int v = 0;
  snprintf (leaf, sizeof leaf, "%s-sts", id);
  cache_file (leaf, path, sizeof path);
  if ((f = fopen (path, "r")))
    {
      if (fscanf (f, "%d", &v) != 1)
        v = 0;
      fclose (f);
    }
  return v;
}

static void write_sts (const char *id, int sts)
{
  char path[400], leaf[64];
  FILE *f;
  snprintf (leaf, sizeof leaf, "%s-sts", id);
  cache_file (leaf, path, sizeof path);
  if ((f = fopen (path, "w")))
    {
      fprintf (f, "%d\n", sts);
      fclose (f);
    }
}

static int play_step (api_op *op);
static void start_solve (api_op *op);

static void solve_done (void *ctx)
{
  api_op *op = ctx;
  char *n_out[2] = { 0 }, *sig_out[2] = { 0 }, err[400];
  int kind, rc;
  err[0] = 0;
  rc = solvejob_read_result (op->out_path, &op->job, n_out, sig_out, err, sizeof err, &kind);
  if (rc == 0 && yt_play_apply (&op->tv, (const char *const *) n_out, (const char *const *) sig_out))
    {
      rc = -1;
      snprintf (err, sizeof err, "Out of memory");
    }
  for (int i = 0; i < 2; i++)
    {
      free (n_out[i]);
      free (sig_out[i]);
    }
  remove (op->job_path);
  remove (op->out_path);
  op->state = P_TV;                          /* (the files are gone) */
  if (rc == 0)
    {
      remove (op->player_path);              /* (prepared and kept now: the code isn't needed) */
      play_finish (op, &op->tv, NULL);
      return;
    }
  if (!op->retried && kind == SOLVE_CACHE)
    {
      /* the prepared player didn't work (the helper has deleted it): from its code */
      FILE *f;
      op->retried = 1;
      if ((f = fopen (op->player_path, "rb")) && !fclose (f))
        start_solve (op);
      else
        {
          op->state = P_PLAYERJS;
          play_step (op);
        }
      return;
    }
  if (!op->retried && kind == SOLVE_SCRIPT && tv_host->scripts_failed)
    {
      /* those scripts don't work: once more with the others (the bundled ones) */
      char old[32];
      snprintf (old, sizeof old, "%s", op->version);
      tv_host->scripts_failed (old);
      if (tv_host->scripts (op->lib, sizeof op->lib, op->core, sizeof op->core, op->version, sizeof op->version) == 0
          && strcmp (old, op->version))
        {
          FILE *f;
          op->retried = 1;
          if (solvejob_cached (tv_host->cache_dir, op->player_id, op->version)
              || ((f = fopen (op->player_path, "rb")) && !fclose (f)))
            start_solve (op);
          else
            {
              op->state = P_PLAYERJS;
              play_step (op);
            }
          return;
        }
    }
  play_finish (op, NULL, err[0] ? err : "the challenge couldn't be answered");
}

/* The TV answer needs its challenges answered: hand them to the solver. */
static void start_solve (api_op *op)
{
  int n = ++job_counter;
  op->state = P_SOLVE;
  snprintf (op->job_path, sizeof op->job_path, "%s" SEP "TubeSearchJob%d", tv_host->work_dir, n);
  snprintf (op->out_path, sizeof op->out_path, "%s" SEP "TubeSearchAns%d", tv_host->work_dir, n);
  memset (&op->job, 0, sizeof op->job);
  op->job.player_id = op->player_id;
  op->job.player_file = solvejob_cached (tv_host->cache_dir, op->player_id, op->version) ? "" : op->player_path;
  op->job.cache_dir = tv_host->cache_dir;
  op->job.lib = op->lib;
  op->job.core = op->core;
  op->job.version = op->version;
  yt_play_challenges (&op->tv, op->job.n, &op->job.nn, op->job.sig, &op->job.nsig);
  remove (op->out_path);
  if (solvejob_write (op->job_path, &op->job))
    {
      op->state = P_TV;
      play_finish (op, NULL, "the solve job couldn't be written");
      return;
    }
  progress (op, *op->job.player_file
            ? "Answering YouTube's challenge (the first video after a YouTube update takes a while)..."
            : "Answering YouTube's challenge...");
  if (tv_host->solve (op->job_path, op->out_path, solve_done, op))
    {
      remove (op->job_path);
      op->state = P_TV;
      play_finish (op, NULL, "the JavaScript helper couldn't be started");
    }
}

static void play_http_done (http_req *r, void *ctx)
{
  api_op *op = ctx;
  char err[400];
  size_t len;
  const char *body = http_body (r, &len);
  int status = http_status (r), final;
  yt_play p;

  op->req = NULL;
  dump (op->state == P_PLAYER ? "player" : op->state == P_IFRAME ? "iframe_api"
        : op->state == P_PLAYERJS ? "playerjs" : "player-tv", body, len);
  err[0] = 0;
  if (!status)
    snprintf (err, sizeof err, "Couldn't reach YouTube: %s", http_error (r));
  else if (status != 200 && (len == 0 || body[0] != '{'))
    snprintf (err, sizeof err, "YouTube answered %d", status);

  switch (op->state)
    {
    case P_PLAYER:
      final = 0;
      if (!err[0] && (yt_parse_player (body, len, op->maxq, &p, err, sizeof err) == 0 || (final = yt_player_final, 0)))
        {
          if (!yt_play_needs_solving (&p))
            {
              deliver (op, &p);
              return;
            }
          yt_play_free (&p);
          snprintf (err, sizeof err, "the player client's addresses need YouTube's JavaScript");
        }
      if (!tv_allowed () || !status || final)
        {
          play_finish (op, NULL, err);
          return;
        }
      op->first_err = strdup (err);
      op->state = P_IFRAME;
      play_step (op);
      return;
    case P_IFRAME:
      if (err[0] || yt_player_id (body, len, op->player_id))
        {
          play_finish (op, NULL, err[0] ? err : "YouTube's player version wasn't found");
          return;
        }
      snprintf (op->player_path, sizeof op->player_path, "%s" SEP "Player-%s", tv_host->work_dir, op->player_id);
      op->sts = read_sts (op->player_id);
      op->state = op->sts && solvejob_cached (tv_host->cache_dir, op->player_id, op->version) ? P_TV : P_PLAYERJS;
      play_step (op);
      return;
    case P_PLAYERJS:
      {
        FILE *f;
        if (!err[0])
          op->sts = yt_player_sts (body, len);
        if (!err[0] && !op->sts)
          snprintf (err, sizeof err, "YouTube's player has no signature timestamp");
        if (!err[0] && (!(f = fopen (op->player_path, "wb")) || fwrite (body, 1, len, f) != len || fclose (f)))
          snprintf (err, sizeof err, "the player's JavaScript couldn't be saved");
        if (err[0])
          {
            play_finish (op, NULL, err);
            return;
          }
        write_sts (op->player_id, op->sts);
        if (op->tv.video_url)
          {
            start_solve (op);                  /* (fetched again for a retry: the TV answer is here) */
            return;
          }
        op->state = P_TV;
        play_step (op);
        return;
      }
    case P_TV:
      if (err[0] || yt_parse_player (body, len, op->maxq, &op->tv, err, sizeof err))
        {
          play_finish (op, NULL, err);
          return;
        }
      if (!yt_play_needs_solving (&op->tv))
        {
          play_finish (op, &op->tv, NULL);
          return;
        }
      /* the challenges need the player's code unless it is prepared already */
      if (!solvejob_cached (tv_host->cache_dir, op->player_id, op->version))
        {
          FILE *f = fopen (op->player_path, "rb");
          if (!f)
            {
              /* (cached when the TV answer was asked for, but gone since) */
              yt_play_free (&op->tv);
              op->state = P_PLAYERJS;
              play_step (op);
              return;
            }
          fclose (f);
        }
      start_solve (op);
      return;
    }
}

/* Sends the request for op's present state. -1 if it couldn't: then op
   is finished (its callback called) and freed. */
static int play_step (api_op *op)
{
  char path[256];
  yt_req *q = NULL;
  if (op->state == P_IFRAME)
    {
      if (tv_host->scripts (op->lib, sizeof op->lib, op->core, sizeof op->core, op->version, sizeof op->version))
        {
          play_finish (op, NULL, "the challenge solver's scripts are missing");
          return -1;
        }
      progress (op, "Asking as YouTube's TV app...");
      op->req = http_get (YT_PLAYER_HOST, YT_IFRAME_API_PATH, yt_web_get_headers (), TAG_API, play_http_done, op);
    }
  else if (op->state == P_PLAYERJS)
    {
      progress (op, "Fetching YouTube's player...");
      yt_player_path (op->player_id, path, sizeof path);
      op->req = http_get (YT_PLAYER_HOST, path, yt_web_get_headers (), TAG_API, play_http_done, op);
    }
  else if (op->state == P_TV)
    {
      q = yt_req_player_tv (op->arg, op->sts);
      if (q)
        op->req = http_post (q->host, q->path, q->headers, q->body, strlen (q->body), TAG_API, play_http_done, op);
      yt_req_free (q);
    }
  if (!op->req)
    {
      play_finish (op, NULL, "Out of memory");
      return -1;
    }
  return 0;
}

/* ---- lists ---------------------------------------------------------------- */

static void list_done (http_req *r, void *ctx)
{
  api_op *op = ctx;
  char err[256];
  size_t len;
  const char *body = http_body (r, &len);
  int status = http_status (r);
  int added = -1;

  op->req = NULL;
  dump (op->kind == YT_LIST_SEARCH ? "search" : op->kind == YT_LIST_RELATED ? "next" : "browse", body, len);
  err[0] = 0;
  if (!status)
    snprintf (err, sizeof err, "Couldn't reach YouTube: %s", http_error (r));
  else if (status != 200 && (len == 0 || body[0] != '{'))
    snprintf (err, sizeof err, "YouTube answered %d", status);
  if (!err[0])
    added = yt_parse_list (body, len, op->kind, op->list, err, sizeof err);
  op->list_cb (added, added < 0 ? err : NULL, op->ctx);
  free_op (op);
}

/* Sends op's first request: 0; -1 if it couldn't be made (out of memory);
   1 if op is finished already (its callback called, and freed). */
static int send_op (api_op *op)
{
  yt_req *q;
  if (op->type == OP_PLAY)
    {
      if (!strcmp (yt_config_get ("stream_client"), "tv") && tv_allowed ())
        {
          op->state = P_IFRAME;
          return play_step (op) ? 1 : 0;
        }
      q = yt_req_player (op->arg);
    }
  else if (op->more)
    q = yt_req_more (op->kind, op->arg);
  else if (op->kind == YT_LIST_SEARCH)
    q = yt_req_search (op->arg, op->date);
  else if (op->kind == YT_LIST_RELATED)
    q = yt_req_related (op->arg);
  else
    q = yt_req_channel (op->arg);
  if (q)
    op->req = http_post (q->host, q->path, q->headers, q->body, strlen (q->body), TAG_API,
                         op->type == OP_PLAY ? play_http_done : list_done, op);
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
      if (send_op (op) < 0)
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

/* NULL if the request couldn't even be made (the callback isn't called),
   or if it has finished already (the callback has been called). */
static api_op *start (api_op *op)
{
  if (visitor_state == V_DONE)
    {
      int k = send_op (op);
      if (k < 0)
        free_op (op);
      return k ? NULL : op;
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
          int k;
          waiting = op->next;               /* no visitor ID then: straight on */
          op->next = NULL;
          k = send_op (op);
          if (k < 0)
            free_op (op);
          if (k)
            return NULL;
        }
    }
  return op;
}

api_op *api_list (int kind, const char *arg, int more, int date, yt_list *list, api_list_cb cb, void *ctx)
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
  op->date = date;
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
  op->state = P_PLAYER;
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
  else if (op->type == OP_PLAY && op->state == P_SOLVE)
    {
      if (tv_host && tv_host->solve_cancel)
        tv_host->solve_cancel (op);
      free_op (op);
    }
}
