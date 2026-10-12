/* yt.h - YouTube's Innertube API: building requests and reading the
   answers (search results, related videos, a channel's videos, and the
   addresses of a video's streams).

   The requests copy what NewPipe and yt-dlp send (2026-08 to 2026-10):
   the WEB client for lists, and the VISIONOS client for the streams,
   which gives addresses that need no JavaScript and no proof-of-origin
   token. YouTube changes these from time to time, so every client
   string can be overridden (yt_config_set, from TubeSearch's Config file).

   Part of TubeSearch. MIT licence. */
#ifndef TUBE_YT_H
#define TUBE_YT_H
#include <stddef.h>

/* ---- settings --------------------------------------------------------- */

/* Sets one setting by name (see yt.c's table for the names and the
   defaults). Returns 0, or -1 for an unknown name. */
int yt_config_set (const char *key, const char *value);
const char *yt_config_get (const char *key);
/* Reads "key = value" lines (# starts a comment). Returns the number of
   settings taken, or -1 if the file can't be read. */
int yt_config_load (const char *path);

/* The visitor ID YouTube gave (yt_parse_visitor), sent with every request
   after it. NULL until then. */
extern char *yt_visitor;

/* ---- requests ----------------------------------------------------------- */

typedef struct yt_req {
  const char *host;         /* www.youtube.com, youtubei.googleapis.com */
  char *path;               /* /youtubei/v1/search?prettyPrint=false */
  char *headers;            /* extra header lines, each ending \r\n */
  char *body;               /* JSON */
} yt_req;

yt_req *yt_req_visitor (void);
yt_req *yt_req_search (const char *query);
yt_req *yt_req_related (const char *video_id);
yt_req *yt_req_channel (const char *channel_id);
yt_req *yt_req_player (const char *video_id);
/* The same from the TV client (TVHTML5), whose addresses need the player's
   JavaScript; sts is the player's signature timestamp (yt_player_sts). */
yt_req *yt_req_player_tv (const char *video_id, int sts);

/* ---- the player JavaScript (for the TV client) ------------------------- */

#define YT_PLAYER_HOST "www.youtube.com"
#define YT_IFRAME_API_PATH "/iframe_api"
/* The player's ID (8 hex digits) from www.youtube.com/iframe_api, into
   id[16]. 0, or -1 if it isn't there. */
int yt_player_id (const char *iframe_api, size_t len, char id[16]);
/* The path of the player's JavaScript on www.youtube.com. */
void yt_player_path (const char *id, char *path, size_t size);
/* The signature timestamp in the player's JavaScript, or 0. */
int yt_player_sts (const char *player, size_t len);
/* Headers for fetching from www.youtube.com outside the API (the web user agent). */
const char *yt_web_get_headers (void);
/* The next part of a list (kind: YT_LIST_*). */
yt_req *yt_req_more (int kind, const char *token);
void yt_req_free (yt_req *r);

/* ---- lists -------------------------------------------------------------- */

typedef enum { YT_VIDEO, YT_CHANNEL } yt_kind;

typedef struct {
  yt_kind kind;
  char id[40];              /* video ID, or channel ID (UC...) */
  char channel_id[40];      /* a video's channel, or "" */
  char duration[24];        /* "12:34", "LIVE", or "" */
  char *title;              /* in the desktop's alphabet (text.h) */
  char *channel;            /* a video's channel name, or "" */
  char *meta;               /* "1.2M views · 3 years ago", "Subscribers..." */
  char *thumb_url;          /* the picture to show */
} yt_item;

enum { YT_LIST_SEARCH, YT_LIST_RELATED, YT_LIST_CHANNEL };

typedef struct {
  yt_item *items;
  int n, cap;
  char *continuation;       /* token for more, or NULL */
  char *title;              /* channel name / video title for the window, or NULL */
  char *title_channel_id;   /* RELATED: the video's channel, or NULL */
} yt_list;

/* Adds the items in an answer to list (skipping ones already there) and
   replaces list->continuation. Returns the number added, or -1 with a
   message in err. */
int yt_parse_list (const char *json, size_t len, int kind, yt_list *list, char *err, size_t errlen);
void yt_list_free (yt_list *list);

int yt_parse_visitor (const char *json, size_t len);

/* ---- playing ------------------------------------------------------------ */

typedef struct {
  char *title;              /* UTF-8 */
  char *video_url;          /* the video stream (or video+sound), or NULL with hls_url */
  char *audio_url;          /* the sound stream, or NULL */
  char *hls_url;            /* an HLS playlist (live streams), or NULL */
  int quality;              /* the video's lines (720, 1080...), 0 unknown */
  char codec[32];
  /* The TV client's addresses carry challenges that YouTube's JavaScript
     answers (jsolve.h): a scrambled signature (s, added back as the
     parameter sp) and an "n" value. NULL when there is none. */
  char *video_s, *video_sp, *video_n;
  char *audio_s, *audio_sp, *audio_n;
} yt_play;

/* 1 if the addresses still need challenges answered (yt_play_apply). */
int yt_play_needs_solving (const yt_play *p);
/* The different challenges in p: up to 2 n values and 2 signatures
   (pointers into p). */
void yt_play_challenges (const yt_play *p, const char *n[2], int *nn, const char *sig[2], int *nsig);
/* Puts the answers (in the same order as yt_play_challenges gave the
   challenges) into the addresses. 0, or -1 out of memory. */
int yt_play_apply (yt_play *p, const char *const *n_out, const char *const *sig_out);

/* Chooses the streams: H.264 (avc1) no taller than maxq lines and AAC
   sound; HLS for live streams. 0, or -1 with the reason in err. */
int yt_parse_player (const char *json, size_t len, int maxq, yt_play *p, char *err, size_t errlen);
/* After a failed yt_parse_player: 1 if another client couldn't help either
   (the video is unavailable, private, removed...). */
extern int yt_player_final;
void yt_play_free (yt_play *p);

/* What Reel reads: yt-dlp's -j JSON (title, the addresses, and the
   headers). malloc'd. */
char *yt_reel_json (const yt_play *p, const char *video_id);

#endif
