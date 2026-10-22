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
/* date: YT_DATE_* (YouTube's "Upload date" filter). */
yt_req *yt_req_search (const char *query, int date);

/* The upload-date filter: YouTube's search params ({2: {1: date}} in
   protobuf, base64; the values Invidious uses). */
enum { YT_DATE_ANY, YT_DATE_TODAY, YT_DATE_WEEK, YT_DATE_MONTH, YT_DATE_YEAR, YT_DATE_COUNT };
const char *yt_date_params (int date);       /* "" for any */
const char *yt_date_name (int date);         /* "Any time", "Today", "This week"... */
const char *yt_date_word (int date);         /* "any", "today", "week", "month", "year" */
int yt_date_parse (const char *word);        /* YT_DATE_*, or -1 */
yt_req *yt_req_related (const char *video_id);
yt_req *yt_req_channel (const char *channel_id);
/* The same, asked for in that language rather than Config's ("en"). */
yt_req *yt_req_channel_lang (const char *channel_id, const char *language);
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
  long published;           /* Unix time, when known (subscriptions' feeds), else 0 */
} yt_item;

enum { YT_LIST_SEARCH, YT_LIST_RELATED, YT_LIST_CHANNEL,
       YT_LIST_SUBS,          /* the subscribed channels' newest videos (subs.h; not api_list's) */
       YT_LIST_MYCHANNELS };  /* the subscribed channels (made here, not fetched) */

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

/* A sound track: its language ("en", "" unknown), and whether it's the
   original, a dub, or the one YouTube marks as the default. */
typedef struct {
  char lang[16];
  int original, dubbed, dflt;
} yt_audio;

/* How well a track suits Config's audio_language: "mine" (the default: the
   language of Config's language setting, then the original), "original"
   (what the video was made with), or a language code ("de"). With "mine",
   Config's languages (others you understand, "en de") count too: an
   original in one of them is played as it is, before a dub in your
   language. YouTube's default only decides between equals. Higher is
   better. */
int yt_audio_score (const yt_audio *a);
/* "en-GB" -> "en" (into out); YouTube's old codes as the tracks give
   them ("iw" -> "he", "nb" -> "no", "in" -> "id"). */
void yt_lang_primary (const char *code, char *out, size_t size);
/* The language ("fr", "en-GB", "es-419") and country ("FR") YouTube is
   told, for a RISC OS territory number (Territory_Number: 1 UK, 6 France,
   48 USA...). 0 with both set; -1 for a territory without one (the
   settings are then left as they are). */
int yt_territory_locale (int territory, const char **language, const char **region);
/* Sets Config's language and region from the territory (before the
   Config files are read, so they can still say otherwise). 0, or -1. */
int yt_use_territory (int territory);
/* The same from the territory and the keyboard (OS_Byte 71, 255: the
   same numbers). RISC OS 5 has territory modules for few countries, so a
   French user is on the UK territory with a French keyboard: when the
   territory is the UK's (or has no language), the keyboard's language is
   used, if it has one. 0 if either gave one, or -1. */
int yt_use_locale (int territory, int keyboard);
/* The language and country for one of the Sound menu's languages
   ("fr" -> "fr", "FR"); 0, or -1 if it isn't one of them. */
int yt_lang_locale (const char *code, const char **language, const char **region);
/* "en" -> "English" (or the code itself). */
const char *yt_lang_name (const char *code);
/* The languages the Sound menus offer, in the order of their English
   names: the i'th one's code ("ar", "bn"... "vi"), or NULL past the end. */
const char *yt_lang_menu (int i);
/* Is code one of Config's languages? */
int yt_lang_understood (const char *code);
/* Adds code to Config's languages, or takes it off if it's there.
   0, or -1 out of memory. */
int yt_lang_toggle (const char *code);

typedef struct {
  char *title;              /* UTF-8 */
  char *video_url;          /* the video stream (or video+sound), or NULL with hls_url */
  char *audio_url;          /* the sound stream, or NULL */
  char *hls_url;            /* an HLS playlist (live streams), or NULL */
  int quality;              /* the video's lines (720, 1080...), 0 unknown */
  char codec[32];
  int live;                 /* a live stream */
  yt_audio audio;           /* the sound track chosen */
  int hls;                  /* video_url (and audio_url) are HLS playlists (yt_hls_choose) */
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

/* ---- HLS ---------------------------------------------------------------

   YouTube's player answer also gives an HLS master playlist (hls_url).
   Played as HLS, a video comes in short pieces, each its own request,
   where the two long DASH streams can be slowed down or refused part way
   (Chris, ROOL forum, 2026-10-15: yt-dlp's HLS formats play smoothly when
   its DASH ones don't). Config: stream_format = hls (the default) | dash. */

/* 1 if hls is to be used for p (Config's stream_format, or a live stream). */
int yt_want_hls (const yt_play *p);

/* From a master playlist (its text, and its own address for relative
   names): the H.264 variant no taller than maxq lines (the tallest; at the
   same height 30 frames a second or less, then the most bits), and its
   sound when the variant has a separate AUDIO group (the track that suits
   audio_language best: yt_audio_score; then the first; *audio_info says
   which).
   *video and *audio (NULL when the sound is in the video) are malloc'd
   media-playlist addresses; *quality the lines; codec the variant's H.264
   codec. 0, or -1 with the reason in err. */
int yt_hls_choose (const char *m3u8, size_t len, const char *base_url, int maxq,
                   char **video, char **audio, int *quality, char *codec, size_t codecsize,
                   yt_audio *audio_info, char *err, size_t errlen);

/* Splits an https address into host and path (for http_get). 0, or -1. */
int yt_split_url (const char *url, char *host, size_t hostsize, char *path, size_t pathsize);

/* What Reel reads: yt-dlp's -j JSON (title, the addresses, and the
   headers). malloc'd. */
char *yt_reel_json (const yt_play *p, const char *video_id);

#endif
