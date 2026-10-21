/* subs.h - channel subscriptions, kept on this machine (no YouTube
   account): the list of channels, reading it from a file and writing it
   back, bringing in a list exported elsewhere (Google Takeout's
   subscriptions.csv, NewPipe's or FreeTube's export), and the channels'
   newest videos from YouTube's per-channel feeds
   (www.youtube.com/feeds/videos.xml?channel_id=UC...).

   Part of TubeSearch. MIT licence. */
#ifndef TUBE_SUBS_H
#define TUBE_SUBS_H
#include <stddef.h>
#include "yt.h"

typedef struct {
  char id[40];              /* UC... */
  char *name;               /* UTF-8 */
  char *thumb;              /* its picture's address, or "" */
} yt_sub;

typedef struct {
  yt_sub *v;
  int n, cap;
  long seen;                /* when the subscriptions' videos were last shown (Unix time), 0 never */
} yt_subs;

/* The file: "# ..." comments, "seen <time>", then a line a channel:
   "UC...<tab>name<tab>picture". A missing file is an empty list (0);
   -1 out of memory. */
int subs_load (yt_subs *s, const char *path);
int subs_save (const yt_subs *s, const char *path);              /* 0, or -1 */
void subs_free (yt_subs *s);
int subs_find (const yt_subs *s, const char *id);                /* its index, or -1 */
/* 1 added; 0 there already (its name and picture brought up to date when
   given); -1 not a channel ID, or out of memory. */
int subs_add (yt_subs *s, const char *id, const char *name, const char *thumb);
int subs_remove (yt_subs *s, const char *id);                    /* 1 removed, 0 not there */
/* Is id a channel ID (UC and 22 more letters, digits, - or _)? */
int subs_is_channel_id (const char *id, size_t len);

/* Brings in an exported list: Google Takeout's subscriptions.csv
   ("Channel Id,Channel Url,Channel Title"), NewPipe's subscriptions JSON
   ("url": ".../channel/UC...", "name"), or FreeTube's profiles/
   subscriptions (lines of JSON with "id": "UC...", "name"), or a
   TubeSearch Subscriptions file. The number added (*found: the different
   channels in the text), or -1 if it isn't one of them. */
int subs_import (yt_subs *s, const char *text, size_t len, int *found);

/* The subscribed channels as tiles: YT_CHANNEL items in a yt_list ("My
   channels"), in the order subscribed. The number added. */
int subs_channel_list (const yt_subs *s, yt_list *list);

/* ---- the feeds ------------------------------------------------------ */

/* The path on www.youtube.com of a channel's feed. */
void subs_feed_path (const char *channel_id, char *out, size_t size);

/* Adds the videos in a channel's feed (Atom) to list (skipping ones there
   already), each with its published time; meta is "1.2K views • 2 days
   ago" from now, with "New • " in front if published after seen (0: none
   new). The number added, or -1 with the reason in err. */
int subs_parse_feed (const char *xml, size_t len, yt_list *list, long now, long seen, char *err, size_t errlen,
                     char *name, size_t namesize);
/* (name: the channel's name in the feed, UTF-8, or "".) */

/* "New • " in front of a meta already in the desktop's alphabet (malloc'd). */
char *subs_mark_new (const char *meta);

/* "3 days ago" (in a channel page's meta, "1.2K views • 3 days ago",
   "Streamed 2 weeks ago"): about how many seconds; -1 if there's none. */
long subs_ago_seconds (const char *text);
/* "2 days ago", "just now"... for a time t seconds before now (into out). */
void subs_ago_text (long seconds, char *out, size_t size);
/* "2026-10-20T15:00:06+00:00" as Unix time; -1 if it isn't one. */
long subs_parse_time (const char *iso);

/* Newest first (stable), at most max items (the rest freed). */
void subs_sort_newest (yt_list *list, int max);

#endif
