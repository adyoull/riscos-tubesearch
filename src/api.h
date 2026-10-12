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
   continuation token instead. Items are added to list, which must stay
   until the callback (or api_cancel). */
api_op *api_list (int kind, const char *arg, int more, yt_list *list, api_list_cb cb, void *ctx);
api_op *api_play (const char *video_id, int maxq, api_play_cb cb, void *ctx);

/* Forget an operation: its callback is never called. */
void api_cancel (api_op *op);

/* Tests and the command line: the raw answer of every request is also
   written to files in this directory (NULL: not). */
extern const char *api_dump_dir;

#endif
