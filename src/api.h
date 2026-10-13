/* api.h - YouTube operations that finish later: fetch a list (search,
   related videos, a channel's videos, or more of any of them) or the
   streams of a video. The first one also gets a visitor ID from YouTube,
   which the others then carry. Driven by http_poll(). Part of TubeSearch. MIT. */
#ifndef TUBE_API_H
#define TUBE_API_H
#include "yt.h"

typedef struct api_op api_op;

/* added: items added to the list (>= 0), or -1 with err. */
typedef void (*api_list_cb) (int added, const char *err, void *ctx);
/* p: the streams (the callback may keep p's strings by taking them; the
   rest is freed after it returns), or NULL with err. */
typedef void (*api_play_cb) (yt_play *p, const char *err, void *ctx);

/* kind: YT_LIST_*; arg: the query, video ID or channel ID; more: arg is a
   continuation token instead (which carries the search's filter); date:
   a search's upload-date filter, YT_DATE_*. Items are added to list, which
   must stay until the callback (or api_cancel). */
api_op *api_list (int kind, const char *arg, int more, int date, yt_list *list, api_list_cb cb, void *ctx);
/* (Either may call cb before returning, and then returns NULL.) */
api_op *api_play (const char *video_id, int maxq, api_play_cb cb, void *ctx);

/* Forget an operation: its callback is never called. */
void api_cancel (api_op *op);

/* ---- the TV fallback ----------------------------------------------------

   When the player client gives nothing playable (or Config's stream_client
   says tv), the video is asked for again as YouTube's TV app, whose
   addresses carry challenges answered by the player's JavaScript in
   yt-dlp's solver (solvejob.h). The program around this provides: */
typedef struct {
  const char *work_dir;       /* the job, answer and player files */
  const char *cache_dir;      /* prepared players and signature timestamps */
  /* The solver scripts to use, and their version. 0, or -1 if there are none. */
  int (*scripts) (char *lib, size_t libsize, char *core, size_t coresize, char *version, size_t versize);
  /* Runs the job (solvejob.h): done(ctx) when OUT is written (or the run
     ended without it). 0 if started (done may be called before it returns). */
  int (*solve) (const char *job, const char *out, void (*done) (void *ctx), void *ctx);
  /* The solve of ctx isn't wanted any more (done must not be called). */
  void (*solve_cancel) (void *ctx);
  /* The scripts of this version failed: use others (the bundled ones). */
  void (*scripts_failed) (const char *version);
  /* What's happening, for the status line (op_ctx: api_play's ctx). */
  void (*progress) (const char *what, void *op_ctx);
} api_tv_host;

/* NULL: no TV fallback (as before). The host is used, not copied. */
void api_set_tv_host (const api_tv_host *host);

/* Tests and the command line: the raw answer of every request is also
   written to files in this directory (NULL: not). */
extern const char *api_dump_dir;

#endif
