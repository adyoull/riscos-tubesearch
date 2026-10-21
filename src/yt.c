/* yt.c - see yt.h. Part of TubeSearch. MIT licence. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "yt.h"
#include "json.h"
#include "text.h"

/* ---- settings --------------------------------------------------------- */

typedef struct { const char *key; const char *dflt; char *value; } setting;

/* The defaults: NewPipe 2026-08 (VISIONOS 1.04, WEB 2.20260805) and
   yt-dlp 2026-10-10 (the VISIONOS user agent for the media servers). */
static setting settings[] = {
  { "web_client_version", "2.20260805.01.00", NULL },
  { "web_user_agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/141.0.0.0 Safari/537.36", NULL },
  { "web_host", "www.youtube.com", NULL },
  { "player_host", "youtubei.googleapis.com", NULL },
  { "player_client_name", "VISIONOS", NULL },
  { "player_client_version", "1.04", NULL },
  { "player_user_agent", "com.google.visionos.youtube/1.04(RealityDevice17,1; U; CPU visionOS 26_6_0 like Mac OS X; GB)", NULL },
  { "player_device_make", "Apple", NULL },
  { "player_device_model", "RealityDevice17,1", NULL },
  { "player_os_name", "visionOS", NULL },
  { "player_os_version", "26.6.0.23O770", NULL },
  { "media_user_agent", "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15", NULL },
  { "language", "en-GB", NULL },
  { "region", "GB", NULL },
  { "consent_cookie", "SOCS=CAE=", NULL },
  { "channel_videos_params", "EgZ2aWRlb3PyBgQKAjoA", NULL },
  { "max_quality", "1080", NULL },
  { "max_fps", "30", NULL },                  /* the most frames a second: 30 or 60 */
  { "stream_format", "hls", NULL },           /* hls, dash (yt.h) */
  { "audio_language", "mine", NULL },         /* mine, original, or a language code (yt.h) */
  { "languages", "", NULL },                  /* others you understand: "en de" (yt.h) */
  { "search_date", "any", NULL },             /* any, today, week, month, year */
  /* the fallback when the player client gives nothing playable: yt-dlp
     2026-10-10's default client, tv_samsung (needs the player's JavaScript) */
  { "stream_client", "auto", NULL },          /* auto (player, then tv), player, tv */
  { "tv_host", "www.youtube.com", NULL },
  { "tv_client_version", "5.20260707", NULL },
  { "tv_user_agent", "Mozilla/5.0 (SMART-TV; Linux; Tizen 2.4.0) AppleWebKit/538.1 (KHTML, like Gecko) Version/2.4.0 TV Safari/538.1", NULL },
  { "tv_device_make", "Samsung", NULL },
  { "tv_device_model", "UKS9800", NULL },
  { "tv_os_name", "Tizen", NULL },
  { "tv_os_version", "2.4.0", NULL },
  { "player_js_variant", "player_ias.vflset/en_US/base.js", NULL },
  { "thumb_user_agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/141.0.0.0 Safari/537.36", NULL },
  /* the solver scripts: which version to use comes from this manifest */
  { "ejs_manifest_url", "https://raw.githubusercontent.com/adyoull/riscos-tubesearch/main/ejs.json", NULL },
  { "ejs_update", "daily", NULL },            /* daily, never */
  { "ejs_download_base", "https://github.com/yt-dlp/ejs/releases/download", NULL },
  { NULL, NULL, NULL }
};

char *yt_visitor;

static setting *find_setting (const char *key)
{
  for (setting *s = settings; s->key; s++)
    if (!strcmp (s->key, key))
      return s;
  return NULL;
}

const char *yt_config_get (const char *key)
{
  setting *s = find_setting (key);
  return !s ? "" : s->value ? s->value : s->dflt;
}

int yt_config_set (const char *key, const char *value)
{
  setting *s = find_setting (key);
  char *v;
  if (!s)
    return -1;
  if (!(v = strdup (value)))
    return -1;
  free (s->value);
  s->value = v;
  return 0;
}

int yt_config_load (const char *path)
{
  FILE *f = fopen (path, "r");
  char line[512];
  int n = 0;
  if (!f)
    return -1;
  while (fgets (line, sizeof line, f))
    {
      char *k = line, *v, *e;
      while (*k == ' ' || *k == '\t')
        k++;
      if (*k == '#' || *k == '|' || !*k || *k == '\n' || *k == '\r')
        continue;
      if (!(v = strchr (k, '=')))
        continue;
      for (e = v; e > k && (e[-1] == ' ' || e[-1] == '\t'); e--)
        ;
      *e = 0;
      v++;
      while (*v == ' ' || *v == '\t')
        v++;
      for (e = v + strlen (v); e > v && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'); e--)
        ;
      *e = 0;
      if (!yt_config_set (k, v))
        n++;
    }
  fclose (f);
  return n;
}

#define CFG(k) yt_config_get (k)

/* ---- a growing string ---------------------------------------------------- */

typedef struct { char *s; size_t len, cap; int oom; } sb;

static void sb_add (sb *b, const char *t, size_t l)
{
  if (b->oom)
    return;
  if (b->len + l + 1 > b->cap)
    {
      size_t cap = b->cap ? b->cap * 2 : 256;
      char *n;
      while (cap < b->len + l + 1)
        cap *= 2;
      if (!(n = realloc (b->s, cap)))
        {
          b->oom = 1;
          return;
        }
      b->s = n;
      b->cap = cap;
    }
  memcpy (b->s + b->len, t, l);
  b->len += l;
  b->s[b->len] = 0;
}

static void sb_str (sb *b, const char *t)
{
  sb_add (b, t, strlen (t));
}

static void sb_printf (sb *b, const char *fmt, ...)
{
  char tmp[1024];
  va_list ap;
  int n;
  va_start (ap, fmt);
  n = vsnprintf (tmp, sizeof tmp, fmt, ap);
  va_end (ap);
  if (n < 0)
    return;
  if ((size_t) n < sizeof tmp)
    sb_add (b, tmp, (size_t) n);
  else
    {
      char *big = malloc ((size_t) n + 1);
      if (!big)
        {
          b->oom = 1;
          return;
        }
      va_start (ap, fmt);
      vsnprintf (big, (size_t) n + 1, fmt, ap);
      va_end (ap);
      sb_add (b, big, (size_t) n);
      free (big);
    }
}

/* s as a JSON string literal */
static void sb_quote (sb *b, const char *s)
{
  size_t need = json_quote (s, NULL, 0);
  char *q = malloc (need + 1);
  if (!q)
    {
      b->oom = 1;
      return;
    }
  json_quote (s, q, need + 1);
  sb_add (b, q, need);
  free (q);
}

static char *sb_done (sb *b)
{
  if (b->oom)
    {
      free (b->s);
      return NULL;
    }
  if (!b->s)
    return strdup ("");
  return b->s;
}

/* ---- requests ----------------------------------------------------------- */

static void random_id (char *out, int n)
{
  static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  for (int i = 0; i < n; i++)
    out[i] = chars[rand () % 64];
  out[n] = 0;
}

/* the language a request is asked for in, when not Config's (yt_req_channel_lang) */
static const char *hl_override;
#define WEB_HL (hl_override ? hl_override : CFG ("language"))

static void web_context (sb *b, int visitor_request)
{
  sb_str (b, "{\"context\":{\"client\":{\"clientName\":\"WEB\",\"clientVersion\":");
  sb_quote (b, CFG ("web_client_version"));
  if (visitor_request)
    sb_str (b, ",\"clientScreen\":\"WATCH\"");
  sb_str (b, ",\"hl\":");
  sb_quote (b, WEB_HL);
  sb_str (b, ",\"gl\":");
  sb_quote (b, CFG ("region"));
  sb_str (b, ",\"originalUrl\":\"https://www.youtube.com\",\"platform\":\"DESKTOP\",\"utcOffsetMinutes\":0");
  if (yt_visitor && !visitor_request)
    {
      sb_str (b, ",\"visitorData\":");
      sb_quote (b, yt_visitor);
    }
  sb_str (b, "},\"request\":{\"internalExperimentFlags\":[],\"useSsl\":true},"
             "\"user\":{\"lockedSafetyMode\":false}}");
}

static char *web_headers (void)
{
  sb b = { 0 };
  sb_printf (&b, "Content-Type: application/json\r\n"
                 "Origin: https://www.youtube.com\r\n"
                 "Referer: https://www.youtube.com/\r\n"
                 "User-Agent: %s\r\n"
                 "X-YouTube-Client-Name: 1\r\n"
                 "X-YouTube-Client-Version: %s\r\n"
                 "Accept-Language: %s, en;q=0.9\r\n",
             CFG ("web_user_agent"), CFG ("web_client_version"), WEB_HL);
  if (*CFG ("consent_cookie"))
    sb_printf (&b, "Cookie: %s\r\n", CFG ("consent_cookie"));
  if (yt_visitor)
    sb_printf (&b, "X-Goog-Visitor-Id: %s\r\n", yt_visitor);
  return sb_done (&b);
}

static yt_req *new_req (const char *host, const char *endpoint, char *body, char *headers)
{
  yt_req *r = calloc (1, sizeof *r);
  sb p = { 0 };
  if (!r || !body || !headers)
    {
      free (r);
      free (body);
      free (headers);
      return NULL;
    }
  sb_printf (&p, "/youtubei/v1/%s?prettyPrint=false", endpoint);
  r->host = host;
  r->path = sb_done (&p);
  r->body = body;
  r->headers = headers;
  if (!r->path)
    {
      yt_req_free (r);
      return NULL;
    }
  return r;
}

void yt_req_free (yt_req *r)
{
  if (!r)
    return;
  free (r->path);
  free (r->headers);
  free (r->body);
  free (r);
}

yt_req *yt_req_visitor (void)
{
  sb b = { 0 };
  web_context (&b, 1);
  sb_str (&b, "}");
  return new_req (CFG ("web_host"), "visitor_id", sb_done (&b), web_headers ());
}

static const struct { const char *word, *name, *params; } dates[YT_DATE_COUNT] = {
  { "any", "Any time", "" },
  { "today", "Today", "EgIIAg==" },
  { "week", "This week", "EgIIAw==" },
  { "month", "This month", "EgIIBA==" },
  { "year", "This year", "EgIIBQ==" },
};

const char *yt_date_params (int date)
{
  return date > 0 && date < YT_DATE_COUNT ? dates[date].params : "";
}

const char *yt_date_name (int date)
{
  return dates[date > 0 && date < YT_DATE_COUNT ? date : 0].name;
}

const char *yt_date_word (int date)
{
  return dates[date > 0 && date < YT_DATE_COUNT ? date : 0].word;
}

int yt_date_parse (const char *word)
{
  for (int i = 0; i < YT_DATE_COUNT; i++)
    if (word && !strcmp (word, dates[i].word))
      return i;
  return -1;
}

yt_req *yt_req_search (const char *query, int date)
{
  sb b = { 0 };
  web_context (&b, 0);
  sb_str (&b, ",\"query\":");
  sb_quote (&b, query);
  if (*yt_date_params (date))
    {
      sb_str (&b, ",\"params\":");
      sb_quote (&b, yt_date_params (date));
    }
  sb_str (&b, "}");
  return new_req (CFG ("web_host"), "search", sb_done (&b), web_headers ());
}

yt_req *yt_req_related (const char *video_id)
{
  sb b = { 0 };
  web_context (&b, 0);
  sb_str (&b, ",\"videoId\":");
  sb_quote (&b, video_id);
  sb_str (&b, ",\"contentCheckOk\":true,\"racyCheckOk\":true}");
  return new_req (CFG ("web_host"), "next", sb_done (&b), web_headers ());
}

yt_req *yt_req_channel (const char *channel_id)
{
  sb b = { 0 };
  web_context (&b, 0);
  sb_str (&b, ",\"browseId\":");
  sb_quote (&b, channel_id);
  if (*CFG ("channel_videos_params"))
    {
      sb_str (&b, ",\"params\":");
      sb_quote (&b, CFG ("channel_videos_params"));
    }
  sb_str (&b, "}");
  return new_req (CFG ("web_host"), "browse", sb_done (&b), web_headers ());
}

yt_req *yt_req_channel_lang (const char *channel_id, const char *language)
{
  yt_req *q;
  hl_override = language;
  q = yt_req_channel (channel_id);
  hl_override = NULL;
  return q;
}

yt_req *yt_req_more (int kind, const char *token)
{
  sb b = { 0 };
  web_context (&b, 0);
  sb_str (&b, ",\"continuation\":");
  sb_quote (&b, token);
  sb_str (&b, "}");
  return new_req (CFG ("web_host"),
                  kind == YT_LIST_SEARCH ? "search" : kind == YT_LIST_RELATED ? "next" : "browse",
                  sb_done (&b), web_headers ());
}

yt_req *yt_req_player (const char *video_id)
{
  sb b = { 0 }, h = { 0 }, p = { 0 };
  char cpn[17], t[13];
  yt_req *r;
  random_id (cpn, 16);
  random_id (t, 12);
  sb_str (&b, "{\"context\":{\"client\":{\"clientName\":");
  sb_quote (&b, CFG ("player_client_name"));
  sb_str (&b, ",\"clientVersion\":");
  sb_quote (&b, CFG ("player_client_version"));
  sb_str (&b, ",\"clientScreen\":\"WATCH\",\"platform\":\"MOBILE\"");
  if (yt_visitor)
    {
      sb_str (&b, ",\"visitorData\":");
      sb_quote (&b, yt_visitor);
    }
  sb_str (&b, ",\"deviceMake\":");
  sb_quote (&b, CFG ("player_device_make"));
  sb_str (&b, ",\"deviceModel\":");
  sb_quote (&b, CFG ("player_device_model"));
  sb_str (&b, ",\"osName\":");
  sb_quote (&b, CFG ("player_os_name"));
  sb_str (&b, ",\"osVersion\":");
  sb_quote (&b, CFG ("player_os_version"));
  sb_str (&b, ",\"hl\":");
  sb_quote (&b, CFG ("language"));
  sb_str (&b, ",\"gl\":");
  sb_quote (&b, CFG ("region"));
  sb_str (&b, ",\"utcOffsetMinutes\":0},\"request\":{\"internalExperimentFlags\":[],\"useSsl\":true},"
              "\"user\":{\"lockedSafetyMode\":false}},\"videoId\":");
  sb_quote (&b, video_id);
  sb_printf (&b, ",\"cpn\":\"%s\",\"contentCheckOk\":true,\"racyCheckOk\":true}", cpn);

  sb_printf (&h, "Content-Type: application/json\r\n"
                 "User-Agent: %s\r\n"
                 "X-Goog-Api-Format-Version: 2\r\n"
                 "Accept-Language: %s, en;q=0.9\r\n",
             CFG ("player_user_agent"), CFG ("language"));
  if (yt_visitor)
    sb_printf (&h, "X-Goog-Visitor-Id: %s\r\n", yt_visitor);

  r = new_req (CFG ("player_host"), "player", sb_done (&b), sb_done (&h));
  if (!r)
    return NULL;
  sb_printf (&p, "/youtubei/v1/player?prettyPrint=false&t=%s&id=", t);
  for (const char *c = video_id; *c; c++)            /* IDs are [A-Za-z0-9_-] */
    if ((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_')
      sb_add (&p, c, 1);
  free (r->path);
  r->path = sb_done (&p);
  if (!r->path)
    {
      yt_req_free (r);
      return NULL;
    }
  return r;
}

yt_req *yt_req_player_tv (const char *video_id, int sts)
{
  sb b = { 0 }, h = { 0 };
  char cpn[17];
  random_id (cpn, 16);
  sb_str (&b, "{\"context\":{\"client\":{\"clientName\":\"TVHTML5\",\"clientVersion\":");
  sb_quote (&b, CFG ("tv_client_version"));
  sb_str (&b, ",\"deviceMake\":");
  sb_quote (&b, CFG ("tv_device_make"));
  sb_str (&b, ",\"deviceModel\":");
  sb_quote (&b, CFG ("tv_device_model"));
  sb_str (&b, ",\"osName\":");
  sb_quote (&b, CFG ("tv_os_name"));
  sb_str (&b, ",\"osVersion\":");
  sb_quote (&b, CFG ("tv_os_version"));
  sb_str (&b, ",\"userAgent\":");
  sb_quote (&b, CFG ("tv_user_agent"));
  sb_str (&b, ",\"hl\":");
  sb_quote (&b, CFG ("language"));
  sb_str (&b, ",\"gl\":");
  sb_quote (&b, CFG ("region"));
  if (yt_visitor)
    {
      sb_str (&b, ",\"visitorData\":");
      sb_quote (&b, yt_visitor);
    }
  sb_str (&b, ",\"utcOffsetMinutes\":0},\"request\":{\"internalExperimentFlags\":[],\"useSsl\":true},"
              "\"user\":{\"lockedSafetyMode\":false}},\"videoId\":");
  sb_quote (&b, video_id);
  sb_printf (&b, ",\"cpn\":\"%s\",\"playbackContext\":{\"contentPlaybackContext\":{\"html5Preference\":\"HTML5_PREF_WANTS\"", cpn);
  if (sts > 0)
    sb_printf (&b, ",\"signatureTimestamp\":%d", sts);
  sb_str (&b, "}},\"contentCheckOk\":true,\"racyCheckOk\":true}");
  sb_printf (&h, "Content-Type: application/json\r\n"
                 "Origin: https://www.youtube.com\r\n"
                 "User-Agent: %s\r\n"
                 "X-YouTube-Client-Name: 7\r\n"
                 "X-YouTube-Client-Version: %s\r\n"
                 "Accept-Language: %s, en;q=0.9\r\n",
             CFG ("tv_user_agent"), CFG ("tv_client_version"), CFG ("language"));
  if (*CFG ("consent_cookie"))
    sb_printf (&h, "Cookie: %s\r\n", CFG ("consent_cookie"));
  if (yt_visitor)
    sb_printf (&h, "X-Goog-Visitor-Id: %s\r\n", yt_visitor);
  return new_req (CFG ("tv_host"), "player", sb_done (&b), sb_done (&h));
}

/* ---- the player JavaScript ------------------------------------------------- */

static int is_hex (int c)
{
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int yt_player_id (const char *t, size_t len, char id[16])
{
  /* ".../s/player/854a788e/..." or, escaped in the iframe API's JS, "player\/854a788e\/" */
  for (size_t i = 0; i + 7 < len; i++)
    if (!strncmp (t + i, "player", 6))
      {
        size_t j = i + 6, k = 0;
        if (j < len && t[j] == '\\')
          j++;
        if (j >= len || t[j] != '/')
          continue;
        j++;
        while (j + k < len && k < 15 && is_hex ((unsigned char) t[j + k]))
          k++;
        if (k >= 8 && j + k < len && (t[j + k] == '/' || t[j + k] == '\\'))
          {
            memcpy (id, t + j, k);
            id[k] = 0;
            return 0;
          }
      }
  return -1;
}

void yt_player_path (const char *id, char *path, size_t size)
{
  snprintf (path, size, "/s/player/%s/%s", id, CFG ("player_js_variant"));
}

int yt_player_sts (const char *t, size_t len)
{
  /* (?:signatureTimestamp|sts)\s*:\s*([0-9]{5}), as yt-dlp looks for it */
  static const char *const keys[2] = { "signatureTimestamp", "sts" };
  for (int k = 0; k < 2; k++)
    {
      size_t kl = strlen (keys[k]);
      for (size_t i = 0; i + kl + 6 < len; i++)
        {
          size_t j;
          int digits = 0, v = 0;
          if (memcmp (t + i, keys[k], kl))
            continue;
          if (k == 1 && i > 0 && ((t[i - 1] >= 'a' && t[i - 1] <= 'z') || (t[i - 1] >= 'A' && t[i - 1] <= 'Z')))
            continue;
          j = i + kl;
          while (j < len && (t[j] == ' ' || t[j] == '\t' || t[j] == '\n'))
            j++;
          if (j >= len || t[j] != ':')
            continue;
          j++;
          while (j < len && (t[j] == ' ' || t[j] == '\t' || t[j] == '\n'))
            j++;
          while (j < len && t[j] >= '0' && t[j] <= '9' && digits < 6)
            {
              v = v * 10 + (t[j++] - '0');
              digits++;
            }
          if (digits == 5)
            return v;
        }
    }
  return 0;
}

const char *yt_web_get_headers (void)
{
  static char h[512];
  snprintf (h, sizeof h, "User-Agent: %s\r\nAccept-Language: %s, en;q=0.9\r\n%s%s%s",
            CFG ("web_user_agent"), CFG ("language"),
            *CFG ("consent_cookie") ? "Cookie: " : "", CFG ("consent_cookie"), *CFG ("consent_cookie") ? "\r\n" : "");
  return h;
}

/* ---- lists -------------------------------------------------------------- */

typedef struct {
  yt_list *list;
  int added;
  const char *cont;          /* the newest continuation found beside items */
  const char *channel;       /* default channel name (a channel's own page) */
  const char *channel_id;
} walker;

static char *disp (const char *utf8)
{
  return text_display (utf8 ? utf8 : "");
}

static int have_item (const yt_list *l, const char *id)
{
  for (int i = 0; i < l->n; i++)
    if (!strcmp (l->items[i].id, id))
      return 1;
  return 0;
}

static yt_item *new_item (walker *w, yt_kind kind, const char *id)
{
  yt_list *l = w->list;
  yt_item *it;
  if (!id || !*id || strlen (id) >= sizeof it->id || have_item (l, id))
    return NULL;
  if (l->n == l->cap)
    {
      int cap = l->cap ? l->cap * 2 : 32;
      yt_item *n = realloc (l->items, (size_t) cap * sizeof (yt_item));
      if (!n)
        return NULL;
      l->items = n;
      l->cap = cap;
    }
  it = &l->items[l->n];
  memset (it, 0, sizeof *it);
  it->kind = kind;
  strcpy (it->id, id);
  return it;
}

static void item_done (walker *w, yt_item *it)
{
  if (!it->title)
    it->title = strdup ("");
  if (!it->channel)
    it->channel = disp (w->channel);
  if (!it->meta)
    it->meta = strdup ("");
  if (!it->thumb_url)
    it->thumb_url = strdup ("");
  if (!it->channel_id[0] && w->channel_id && strlen (w->channel_id) < sizeof it->channel_id)
    strcpy (it->channel_id, w->channel_id);
  if (!it->title || !it->channel || !it->meta || !it->thumb_url)
    {
      free (it->title); free (it->channel); free (it->meta); free (it->thumb_url);
      return;                                 /* (out of memory: not added) */
    }
  w->list->n++;
  w->added++;
}

static void copy_id (char *dst, size_t size, const char *src)
{
  if (src && strlen (src) < size)
    strcpy (dst, src);
}

/* The first browseEndpoint below n whose browseId is a channel (UC...). */
static const char *find_channel_id (const jnode *n)
{
  if (!n || (n->type != J_OBJ && n->type != J_ARR))
    return NULL;
  for (jnode *c = n->child; c; c = c->next)
    {
      const char *id;
      if (n->type == J_OBJ && c->key && !strcmp (c->key, "browseEndpoint"))
        {
          id = j_str (j_get (c, "browseId"));
          if (id && !strncmp (id, "UC", 2))
            return id;
        }
      if ((id = find_channel_id (c)))
        return id;
    }
  return NULL;
}

static void video_thumb (yt_item *it)
{
  char url[96];
  snprintf (url, sizeof url, "https://i.ytimg.com/vi/%s/mqdefault.jpg", it->id);
  it->thumb_url = strdup (url);
}

static void add_video_renderer (walker *w, const jnode *v)
{
  char buf[512], views[128], when[128];
  const jnode *ov;
  yt_item *it = new_item (w, YT_VIDEO, j_str (j_get (v, "videoId")));
  if (!it)
    return;
  j_text (j_get (v, "title"), buf, sizeof buf);
  if (!buf[0])
    j_text (j_get (v, "headline"), buf, sizeof buf);
  it->title = disp (buf);
  if (!j_text (j_get (v, "ownerText"), buf, sizeof buf)
      && !j_text (j_get (v, "shortBylineText"), buf, sizeof buf))
    j_text (j_get (v, "longBylineText"), buf, sizeof buf);
  if (buf[0])
    it->channel = disp (buf);
  copy_id (it->channel_id, sizeof it->channel_id,
           find_channel_id (j_get (v, "ownerText")) ? find_channel_id (j_get (v, "ownerText"))
           : find_channel_id (j_get (v, "shortBylineText")) ? find_channel_id (j_get (v, "shortBylineText"))
           : find_channel_id (j_get (v, "longBylineText")));
  j_text (j_get (v, "lengthText"), buf, sizeof buf);
  ov = j_get (v, "thumbnailOverlays");
  if (ov && ov->type == J_ARR)
    for (jnode *o = ov->child; o; o = o->next)
      {
        const jnode *ts = j_get (o, "thumbnailOverlayTimeStatusRenderer");
        const char *style = j_str (j_get (ts, "style"));
        if (!ts)
          continue;
        if (style && !strcmp (style, "LIVE"))
          strcpy (buf, "LIVE");
        else if (!buf[0])
          j_text (j_get (ts, "text"), buf, sizeof buf);
      }
  if (!buf[0])
    {
      const char *style = j_pstr (j_find (j_get (v, "badges"), "metadataBadgeRenderer"), "style");
      if (style && strstr (style, "LIVE"))
        strcpy (buf, "LIVE");
    }
  text_display_buf (buf, it->duration, sizeof it->duration);
  if (!j_text (j_get (v, "shortViewCountText"), views, sizeof views))
    j_text (j_get (v, "viewCountText"), views, sizeof views);
  j_text (j_get (v, "publishedTimeText"), when, sizeof when);
  snprintf (buf, sizeof buf, "%s%s%s", views, views[0] && when[0] ? " \xE2\x80\xA2 " : "", when);
  it->meta = disp (buf);
  video_thumb (it);
  item_done (w, it);
}

/* The text of one of a lockup's metadata rows: its parts joined by " • ". */
static void row_text (const jnode *row, char *buf, size_t size)
{
  const jnode *parts = j_get (row, "metadataParts");
  size_t at = 0;
  buf[0] = 0;
  if (!parts || parts->type != J_ARR)
    return;
  for (jnode *p = parts->child; p; p = p->next)
    {
      char t[256];
      if (!j_text (j_get (p, "text"), t, sizeof t))
        continue;
      at += (size_t) snprintf (buf + at, at < size ? size - at : 0, "%s%s", at ? " \xE2\x80\xA2 " : "", t);
      if (at >= size)
        {
          buf[size - 1] = 0;
          return;
        }
    }
}

static void add_lockup (walker *w, const jnode *v)
{
  const char *type = j_str (j_get (v, "contentType"));
  const jnode *md, *rows, *badge;
  char buf[512];
  int nrows = 0;
  yt_item *it;
  if (!type || strcmp (type, "LOCKUP_CONTENT_TYPE_VIDEO"))
    return;                                   /* playlists, podcasts: not yet */
  if (!(it = new_item (w, YT_VIDEO, j_str (j_get (v, "contentId")))))
    return;
  md = j_path (v, "metadata.lockupMetadataViewModel");
  j_text (j_get (md, "title"), buf, sizeof buf);
  it->title = disp (buf);
  rows = j_path (md, "metadata.contentMetadataViewModel.metadataRows");
  if (rows && rows->type == J_ARR)
    nrows = rows->n;
  if (nrows >= 2)
    {
      row_text (j_idx (rows, 0), buf, sizeof buf);
      it->channel = disp (buf);
      row_text (j_idx (rows, 1), buf, sizeof buf);
      it->meta = disp (buf);
    }
  else if (nrows == 1)
    {
      row_text (j_idx (rows, 0), buf, sizeof buf);
      it->meta = disp (buf);
    }
  copy_id (it->channel_id, sizeof it->channel_id, find_channel_id (md));
  badge = j_find (j_get (v, "contentImage"), "thumbnailBadgeViewModel");
  if (badge)
    {
      const char *style = j_str (j_get (badge, "badgeStyle"));
      if (style && strstr (style, "LIVE"))
        strcpy (it->duration, "LIVE");
      else
        {
          j_text (j_get (badge, "text"), buf, sizeof buf);
          text_display_buf (buf, it->duration, sizeof it->duration);
        }
    }
  video_thumb (it);
  item_done (w, it);
}

static void add_channel (walker *w, const jnode *v)
{
  char buf[512], subs[128], vids[128];
  const jnode *th;
  const char *url = NULL;
  yt_item *it = new_item (w, YT_CHANNEL, j_str (j_get (v, "channelId")));
  if (!it)
    return;
  copy_id (it->channel_id, sizeof it->channel_id, it->id);
  j_text (j_get (v, "title"), buf, sizeof buf);
  it->title = disp (buf);
  it->channel = disp (buf);
  j_text (j_get (v, "subscriberCountText"), subs, sizeof subs);
  j_text (j_get (v, "videoCountText"), vids, sizeof vids);
  snprintf (buf, sizeof buf, "%s%s%s", subs, subs[0] && vids[0] ? " \xE2\x80\xA2 " : "", vids);
  it->meta = disp (buf);
  th = j_path (v, "thumbnail.thumbnails");
  if (th && th->type == J_ARR)
    for (jnode *t = th->child; t; t = t->next)
      if (j_str (j_get (t, "url")))
        url = j_str (j_get (t, "url"));          /* the last is the biggest */
  if (url)
    {
      snprintf (buf, sizeof buf, "%s%s", !strncmp (url, "//", 2) ? "https:" : "", url);
      it->thumb_url = strdup (buf);
    }
  item_done (w, it);
}

static const char *continuation_token (const jnode *e)
{
  return j_pstr (j_find (e, "continuationCommand"), "token");
}

static int skipped (const char *key)
{
  static const char *const skip[] = {
    "engagementPanels", "topbar", "frameworkUpdates", "microformat", "responseContext",
    "header", "menu", "menuButton", "reelShelfRenderer", "shortsLockupViewModel",
    "gridShelfViewModel", "endScreenVideoRenderer", "playerOverlays", "trackingParams",
    NULL
  };
  for (int i = 0; skip[i]; i++)
    if (!strcmp (key, skip[i]))
      return 1;
  return 0;
}

static void walk (walker *w, const jnode *n)
{
  if (!n)
    return;
  if (n->type == J_OBJ)
    {
      for (jnode *c = n->child; c; c = c->next)
        {
          const char *k = c->key;
          if (!k || skipped (k))
            continue;
          if (!strcmp (k, "videoRenderer") || !strcmp (k, "compactVideoRenderer")
              || !strcmp (k, "gridVideoRenderer") || !strcmp (k, "playlistVideoRenderer"))
            add_video_renderer (w, c);
          else if (!strcmp (k, "lockupViewModel"))
            add_lockup (w, c);
          else if (!strcmp (k, "channelRenderer"))
            add_channel (w, c);
          else
            walk (w, c);
        }
    }
  else if (n->type == J_ARR)
    {
      int before = w->added;
      const char *tok = NULL;
      for (jnode *c = n->child; c; c = c->next)
        {
          if (j_get (c, "continuationItemRenderer"))
            {
              const char *t = continuation_token (c);
              if (t)
                tok = t;
            }
          else
            walk (w, c);
        }
      if (tok && w->added > before)
        w->cont = tok;
    }
}

int yt_parse_list (const char *json, size_t len, int kind, yt_list *list, char *err, size_t errlen)
{
  char jerr[96], buf[512];
  jdoc *d = json_parse (json, len, jerr, sizeof jerr);
  const jnode *root, *start;
  walker w;
  if (!d)
    {
      snprintf (err, errlen, "YouTube's answer wasn't readable (%s)", jerr);
      return -1;
    }
  root = json_root (d);
  if (j_get (root, "error"))
    {
      const char *msg = j_pstr (root, "error.message");
      snprintf (err, errlen, "YouTube: %s", msg ? msg : "error");
      json_free (d);
      return -1;
    }
  memset (&w, 0, sizeof w);
  w.list = list;
  start = root;
  if (kind == YT_LIST_RELATED)
    {
      const jnode *primary = j_find (root, "videoPrimaryInfoRenderer");
      const jnode *owner = j_find (root, "videoOwnerRenderer");
      const jnode *sec = j_path (root, "contents.twoColumnWatchNextResults.secondaryResults");
      if (primary && j_text (j_get (primary, "title"), buf, sizeof buf) && !list->title)
        list->title = disp (buf);
      if (owner && !list->title_channel_id && find_channel_id (owner))
        list->title_channel_id = strdup (find_channel_id (owner));
      if (sec)
        start = sec;
    }
  else if (kind == YT_LIST_CHANNEL)
    {
      const char *t = j_pstr (root, "metadata.channelMetadataRenderer.title");
      w.channel_id = j_pstr (root, "metadata.channelMetadataRenderer.externalId");
      if (t && !list->title)
        list->title = disp (t);
      if (!w.channel_id && list->title_channel_id)
        w.channel_id = list->title_channel_id;
      if (!list->title_channel_id && w.channel_id)
        list->title_channel_id = strdup (w.channel_id);
      /* a channel page's own videos carry no channel name: its title */
      w.channel = t;
    }
  walk (&w, start);
  /* a channel page's later parts (no metadata): the name from before */
  if (kind == YT_LIST_CHANNEL && list->title)
    for (int i = list->n - w.added; i < list->n; i++)
      if (!list->items[i].channel[0])
        {
          char *c = strdup (list->title);
          if (c)
            {
              free (list->items[i].channel);
              list->items[i].channel = c;
            }
        }
  free (list->continuation);
  list->continuation = w.cont ? strdup (w.cont) : NULL;
  json_free (d);
  return w.added;
}

void yt_list_free (yt_list *l)
{
  for (int i = 0; i < l->n; i++)
    {
      free (l->items[i].title);
      free (l->items[i].channel);
      free (l->items[i].meta);
      free (l->items[i].thumb_url);
    }
  free (l->items);
  free (l->continuation);
  free (l->title);
  free (l->title_channel_id);
  memset (l, 0, sizeof *l);
}

int yt_parse_visitor (const char *json, size_t len)
{
  jdoc *d = json_parse (json, len, NULL, 0);
  const char *v = d ? j_pstr (json_root (d), "responseContext.visitorData") : NULL;
  int ok = 0;
  if (v && *v)
    {
      char *c = strdup (v);
      if (c)
        {
          free (yt_visitor);
          yt_visitor = c;
          ok = 1;
        }
    }
  json_free (d);
  return ok ? 0 : -1;
}

/* ---- playing ------------------------------------------------------------ */

/* Is a video (q lines, fps frames a second, rate bits a second) better
   than the best so far? Best of all: no taller than maxq and no more than
   Config's max_fps frames a second (the tallest, then the most bits).
   Failing that: no taller but faster (the slowest), then no faster but
   taller (the smallest), then the smallest and slowest. */
static int video_tier (int q, int fps, int maxq, int maxfps)
{
  return (q <= maxq) * 2 + (fps <= maxfps) + (q <= maxq && fps <= maxfps);
}

static int better_video (int have, int q, int fps, long rate, int bq, int bfps, long brate, int maxq)
{
  int maxfps = atoi (CFG ("max_fps")), t, bt;
  if (maxfps < 1)
    maxfps = 30;
  if (!have)
    return 1;
  t = video_tier (q, fps, maxq, maxfps);
  bt = video_tier (bq, bfps, maxq, maxfps);
  if (t != bt)
    return t > bt;
  if (t == 4)                                   /* both fit: the tallest */
    return q != bq ? q > bq : rate > brate;
  if (t == 2)                                   /* too fast: the slowest, then the tallest */
    return fps != bfps ? fps < bfps : q != bq ? q > bq : rate > brate;
  /* too tall: the smallest, then the slowest */
  return q != bq ? q < bq : fps != bfps ? fps < bfps : rate > brate;
}

static int lines_of (const jnode *f)
{
  const char *q = j_str (j_get (f, "qualityLabel"));
  if (q && *q >= '0' && *q <= '9')
    return atoi (q);
  return (int) j_long (j_get (f, "height"), 0);
}

/* ---- which sound track ---------------------------------------------------

   Many videos have dubbed tracks besides the original (YouTube's own
   auto-dubbing, Arabic to Vietnamese). A track's language and whether it
   is the original are in its address's xtags (acont=original|dubbed...:
   lang=de), %-encoded once or more, and in DASH's audioTrack (id "de.3",
   displayName "English (US) original") or HLS's LANGUAGE and NAME. */

static int hexval (int c);

/* s with %XX decoded, up to three times over, into out. */
static void pct_decode3 (const char *s, size_t n, char *out, size_t size)
{
  char tmp[2048];
  size_t o = 0;
  for (size_t i = 0; i < n && o + 1 < sizeof tmp; i++)
    tmp[o++] = s[i];
  tmp[o] = 0;
  for (int round = 0; round < 3; round++)
    {
      char *r = tmp, *w = tmp;
      int changed = 0;
      while (*r)
        {
          int h1, h2;
          if (r[0] == '%' && r[1] && r[2] && (h1 = hexval (r[1])) >= 0 && (h2 = hexval (r[2])) >= 0)
            {
              *w++ = (char) (h1 * 16 + h2);
              r += 3;
              changed = 1;
            }
          else
            *w++ = *r++;
        }
      *w = 0;
      if (!changed)
        break;
    }
  snprintf (out, size, "%s", tmp);
}

/* What an address says about its sound: lang (primary subtag, lower case)
   and whether it's the original. */
static void xtags_of (const char *url, size_t n, yt_audio *a)
{
  char d[2048];
  const char *l;
  pct_decode3 (url, n, d, sizeof d);
  if (strstr (d, "acont=original"))
    a->original = 1;
  if (strstr (d, "acont=dubbed"))
    a->dubbed = 1;
  if ((l = strstr (d, "lang=")) && !a->lang[0])
    yt_lang_primary (l + 5, a->lang, sizeof a->lang);
}

void yt_lang_primary (const char *code, char *out, size_t size)
{
  static const char *const old[][2] = { { "iw", "he" }, { "nb", "no" }, { "in", "id" }, { "ji", "yi" } };
  size_t i = 0;
  while (code[i] && i + 1 < size && ((code[i] >= 'a' && code[i] <= 'z') || (code[i] >= 'A' && code[i] <= 'Z')))
    {
      out[i] = (char) (code[i] >= 'A' && code[i] <= 'Z' ? code[i] + 32 : code[i]);
      i++;
    }
  if (size)
    out[i < size ? i : size - 1] = 0;
  for (size_t k = 0; k < sizeof old / sizeof old[0]; k++)
    if (size > 2 && !strcmp (out, old[k][0]))
      strcpy (out, old[k][1]);
}

/* Is lang one of Config's languages (a list split by spaces or commas)? */
static int understood (const char *lang)
{
  const char *p = CFG ("languages");
  char one[16];
  if (!lang[0])
    return 0;
  while (*p)
    {
      size_t n = 0;
      char word[32];
      while (*p == ' ' || *p == ',' || *p == '\t')
        p++;
      while (p[n] && p[n] != ' ' && p[n] != ',' && p[n] != '\t')
        n++;
      if (!n)
        break;
      snprintf (word, sizeof word, "%.*s", (int) (n < 31 ? n : 31), p);
      yt_lang_primary (word, one, sizeof one);
      if (!strcmp (one, lang))
        return 1;
      p += n;
    }
  return 0;
}

int yt_audio_score (const yt_audio *a)
{
  const char *want = CFG ("audio_language");
  char mine[16];
  int original = a->original || (!a->dubbed && a->dflt);
  int is_mine, other;
  if (!strcmp (want, "original"))
    return (original ? 8 : 0) + (a->dflt ? 2 : 0);
  yt_lang_primary (strcmp (want, "mine") ? want : CFG ("language"), mine, sizeof mine);
  is_mine = mine[0] && !strcmp (a->lang, mine);
  other = !is_mine && !strcmp (want, "mine") && understood (a->lang);   /* (a language named is just that) */
  /* an original you understand, as it was made; then your language's
     track; then another you understand; then the original */
  return (original && (is_mine || other) ? 32 : 0) + (is_mine ? 16 : other ? 8 : 0) + (original ? 4 : 0)
         + (a->dflt ? 1 : 0);                      /* (YouTube's default: only between equals) */
}

/* RISC OS's territory numbers (Countries in HdrSrc). Canada1 is French
   Canadian, Canada2 English. Where a territory doesn't say which language
   (Canada, Belgium, Switzerland), the main one;
   Config's language changes it. */
static const struct { short territory; const char *language, *region; } territories[] = {
  { 0, "en-GB", "GB" }, { 1, "en-GB", "GB" }, { 2, "en-GB", "GB" }, { 3, "en-GB", "GB" },
  { 4, "it", "IT" }, { 5, "es", "ES" }, { 6, "fr", "FR" }, { 7, "de", "DE" }, { 8, "pt-PT", "PT" },
  { 10, "el", "GR" }, { 11, "sv", "SE" }, { 12, "fi", "FI" }, { 14, "da", "DK" }, { 15, "no", "NO" },
  { 16, "is", "IS" }, { 17, "fr-CA", "CA" }, { 18, "en", "CA" }, { 19, "en", "CA" }, { 20, "tr", "TR" },
  { 21, "ar", "SA" }, { 22, "en-GB", "IE" }, { 23, "zh-HK", "HK" }, { 24, "ru", "RU" }, { 25, "ru", "RU" },
  { 26, "iw", "IL" }, { 27, "es-419", "MX" }, { 28, "es-419", "AR" }, { 29, "en-GB", "AU" },
  { 30, "de", "AT" }, { 31, "nl", "BE" }, { 32, "ja", "JP" }, { 33, "ar", "AE" }, { 34, "nl", "NL" },
  { 35, "de", "CH" }, { 36, "en-GB", "GB" }, { 37, "en-GB", "NZ" }, { 48, "en", "US" },
  { 49, "en-GB", "GB" }, { 50, "zh-CN", "CN" }, { 51, "pt", "BR" }, { 52, "en-GB", "ZA" },
  { 53, "ko", "KR" }, { 54, "zh-TW", "TW" }, { 129, "sq", "AL" }, { 130, "en-GB", "ZA" },
  { 131, "bn", "IN" }, { 132, "bg", "BG" }, { 133, "be", "BY" }, { 134, "cs", "CZ" }, { 135, "hi", "IN" },
  { 136, "fa", "IR" }, { 137, "gu", "IN" }, { 138, "et", "EE" }, { 140, "el", "GR" }, { 141, "da", "GL" },
  { 142, "hu", "HU" }, { 144, "lv", "LV" }, { 145, "lt", "LT" }, { 146, "mk", "MK" }, { 147, "en-GB", "MT" },
  { 148, "pl", "PL" }, { 149, "pa", "IN" }, { 150, "ro", "RO" }, { 151, "sr", "RS" }, { 152, "sk", "SK" },
  { 153, "sl", "SI" }, { 154, "ta", "IN" }, { 155, "uk", "UA" }, { 156, "fr", "CH" }, { 157, "de", "CH" },
  { 158, "it", "CH" }, { 159, "de", "CH" },      /* Swiss1 French, 2 German, 3 Italian, 4 Romansh */
};

int yt_territory_locale (int territory, const char **language, const char **region)
{
  for (size_t i = 0; i < sizeof territories / sizeof territories[0]; i++)
    if (territories[i].territory == territory)
      {
        *language = territories[i].language;
        *region = territories[i].region;
        return 0;
      }
  return -1;
}

int yt_use_territory (int territory)
{
  const char *l, *r;
  if (yt_territory_locale (territory, &l, &r))
    return -1;
  return yt_config_set ("language", l) || yt_config_set ("region", r) ? -1 : 0;
}

const char *yt_lang_name (const char *code)
{
  static const char *const names[][2] = {
    { "en", "English" }, { "de", "German" }, { "fr", "French" }, { "es", "Spanish" }, { "it", "Italian" },
    { "nl", "Dutch" }, { "pt", "Portuguese" }, { "sv", "Swedish" }, { "da", "Danish" }, { "no", "Norwegian" },
{ "fi", "Finnish" }, { "pl", "Polish" }, { "cs", "Czech" }, { "sk", "Slovak" },
    { "hu", "Hungarian" }, { "el", "Greek" }, { "tr", "Turkish" }, { "ru", "Russian" }, { "uk", "Ukrainian" },
    { "ja", "Japanese" }, { "ko", "Korean" }, { "zh", "Chinese" }, { "hi", "Hindi" }, { "ar", "Arabic" },
    { "id", "Indonesian" }, { "vi", "Vietnamese" }, { "th", "Thai" }, { "ga", "Irish" }, { "cy", "Welsh" },
    { "he", "Hebrew" }, { "is", "Icelandic" }, { "ro", "Romanian" }, { "bg", "Bulgarian" }, { "sr", "Serbian" },
    { "hr", "Croatian" }, { "sl", "Slovenian" }, { "et", "Estonian" }, { "lv", "Latvian" }, { "lt", "Lithuanian" },
    { "mk", "Macedonian" }, { "sq", "Albanian" }, { "be", "Belarusian" }, { "fa", "Persian" }, { "bn", "Bengali" },
    { "gu", "Gujarati" }, { "pa", "Punjabi" }, { "ta", "Tamil" }, { "ca", "Catalan" }, { "ms", "Malay" },
  };
  for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
    if (!strcmp (code, names[i][0]))
      return names[i][1];
  return code;
}

static int says_bot (const char *reason)
{
  for (const char *p = reason; *p; p++)
    if ((*p == 'b' || *p == 'B') && (p[1] == 'o' || p[1] == 'O') && (p[2] == 't' || p[2] == 'T'))
      return 1;
  return 0;
}

/* Does a track's name say it's the original ("English (US) original",
   "Deutsch (Original)", "français (original)")? (The xtags say so too.) */
static int says_original (const char *name)
{
  for (const char *p = name; *p; p++)
    if ((*p == 'o' || *p == 'O') && !strncmp (p + 1, "riginal", 7))
      return 1;
  return 0;
}

/* A DASH format's sound: the "DRC" (quieter) copies are left out (-1). */
static int dash_audio (const jnode *f, yt_audio *a)
{
  const jnode *t = j_get (f, "audioTrack");
  const char *u = j_str (j_get (f, "url")) ? j_str (j_get (f, "url")) : j_str (j_get (f, "signatureCipher"));
  memset (a, 0, sizeof *a);
  if (j_true (j_get (f, "isDrc")))
    return -1;
  if (!t)
    {
      a->dflt = a->original = 1;                 /* (one track only) */
      return 0;
    }
  a->dflt = j_true (j_get (t, "audioIsDefault"));
  if (j_str (j_get (t, "id")))
    yt_lang_primary (j_str (j_get (t, "id")), a->lang, sizeof a->lang);
  if (j_str (j_get (t, "displayName")) && says_original (j_str (j_get (t, "displayName"))))
    a->original = 1;
  if (u)
    xtags_of (u, strlen (u), a);
  return 0;
}

/* ---- addresses and their challenges ---------------------------------------- */

static int hexval (int c)
{
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* n bytes of x-www-form-urlencoded text, decoded (malloc'd). */
static char *url_decode (const char *t, size_t n)
{
  char *o = malloc (n + 1), *q = o;
  if (!o)
    return NULL;
  for (size_t i = 0; i < n; i++)
    {
      if (t[i] == '%' && i + 2 < n && hexval (t[i + 1]) >= 0 && hexval (t[i + 2]) >= 0)
        {
          *q++ = (char) (hexval (t[i + 1]) * 16 + hexval (t[i + 2]));
          i += 2;
        }
      else
        *q++ = t[i] == '+' ? ' ' : t[i];
    }
  *q = 0;
  return o;
}

/* s percent-encoded for a query value, appended to b. */
static void sb_url_encode (sb *b, const char *t)
{
  static const char hex[] = "0123456789ABCDEF";
  for (; *t; t++)
    {
      unsigned char c = (unsigned char) *t;
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
        sb_add (b, (const char *) &c, 1);
      else
        {
          char e[3] = { '%', hex[c >> 4], hex[c & 15] };
          sb_add (b, e, 3);
        }
    }
}

/* The value of key in a query string ("a=1&b=2", or after the '?' of an
   address), decoded and malloc'd; NULL if absent. */
static char *query_get (const char *q, const char *key)
{
  size_t kl = strlen (key);
  const char *p = strchr (q, '?');
  p = p ? p + 1 : q;
  while (*p)
    {
      size_t l = strcspn (p, "&");
      if (l > kl && !strncmp (p, key, kl) && p[kl] == '=')
        return url_decode (p + kl + 1, l - kl - 1);
      p += l;
      if (*p == '&')
        p++;
    }
  return NULL;
}

/* A format's address, and its challenges (each malloc'd, or NULL). */
static int format_address (const jnode *f, char **url, char **s_out, char **sp_out, char **n_out)
{
  const char *direct = j_str (j_get (f, "url"));
  const char *cipher = j_str (j_get (f, "signatureCipher"));
  *url = *s_out = *sp_out = *n_out = NULL;
  if (!cipher)
    cipher = j_str (j_get (f, "cipher"));
  if (direct)
    *url = strdup (direct);
  else if (cipher)
    {
      *url = query_get (cipher, "url");
      *s_out = query_get (cipher, "s");
      *sp_out = query_get (cipher, "sp");
      if (*s_out && !*sp_out)
        *sp_out = strdup ("signature");
    }
  if (!*url || (cipher && !direct && !*s_out))
    {
      free (*url); free (*s_out); free (*sp_out);
      *url = *s_out = *sp_out = NULL;
      return -1;
    }
  *n_out = query_get (*url, "n");
  return 0;
}

int yt_play_needs_solving (const yt_play *p)
{
  return p->video_s || p->video_n || p->audio_s || p->audio_n;
}

static void add_unique (const char **list, int *count, const char *v)
{
  if (!v)
    return;
  for (int i = 0; i < *count; i++)
    if (!strcmp (list[i], v))
      return;
  list[(*count)++] = v;
}

void yt_play_challenges (const yt_play *p, const char *n[2], int *nn, const char *sig[2], int *nsig)
{
  *nn = *nsig = 0;
  add_unique (n, nn, p->video_n);
  add_unique (n, nn, p->audio_n);
  add_unique (sig, nsig, p->video_s);
  add_unique (sig, nsig, p->audio_s);
}

static const char *answer_for (const char *v, const char *const *in, const char *const *out, int count)
{
  for (int i = 0; i < count; i++)
    if (!strcmp (in[i], v))
      return out[i];
  return NULL;
}

/* url with n= replaced by n_new and &sp=sig added (malloc'd). */
static char *rebuild (const char *url, const char *n_new, const char *sp, const char *sig)
{
  sb b = { 0 };
  const char *q = strchr (url, '?');
  if (!q)
    return NULL;
  int first = 1;
  sb_add (&b, url, (size_t) (q - url) + 1);
  for (const char *p = q + 1; *p;)
    {
      size_t l = strcspn (p, "&");
      if (n_new && l >= 2 && p[0] == 'n' && p[1] == '=')
        {
          sb_str (&b, first ? "n=" : "&n=");
          sb_url_encode (&b, n_new);
        }
      else
        {
          if (!first)
            sb_add (&b, "&", 1);
          sb_add (&b, p, l);
        }
      first = 0;
      p += l;
      if (*p == '&')
        p++;
    }
  if (sp && sig)
    {
      sb_str (&b, "&");
      sb_url_encode (&b, sp);
      sb_str (&b, "=");
      sb_url_encode (&b, sig);
    }
  return sb_done (&b);
}

int yt_play_apply (yt_play *p, const char *const *n_out, const char *const *sig_out)
{
  const char *n_in[2], *sig_in[2];
  int nn, nsig;
  char *v, *a = NULL;
  yt_play_challenges (p, n_in, &nn, sig_in, &nsig);
  v = rebuild (p->video_url,
               p->video_n ? answer_for (p->video_n, n_in, n_out, nn) : NULL, p->video_sp,
               p->video_s ? answer_for (p->video_s, sig_in, sig_out, nsig) : NULL);
  if (p->audio_url)
    a = rebuild (p->audio_url,
                 p->audio_n ? answer_for (p->audio_n, n_in, n_out, nn) : NULL, p->audio_sp,
                 p->audio_s ? answer_for (p->audio_s, sig_in, sig_out, nsig) : NULL);
  if (!v || (p->audio_url && !a))
    {
      free (v);
      free (a);
      return -1;
    }
  free (p->video_url);
  p->video_url = v;
  if (a)
    {
      free (p->audio_url);
      p->audio_url = a;
    }
  free (p->video_s); free (p->video_sp); free (p->video_n);
  free (p->audio_s); free (p->audio_sp); free (p->audio_n);
  p->video_s = p->video_sp = p->video_n = p->audio_s = p->audio_sp = p->audio_n = NULL;
  return 0;
}

int yt_player_final;

int yt_parse_player (const char *json, size_t len, int maxq, yt_play *p, char *err, size_t errlen)
{
  char jerr[96], buf[512];
  jdoc *d = json_parse (json, len, jerr, sizeof jerr);
  const jnode *root, *ps, *sd, *af, *best_v = NULL, *best_a = NULL, *f;
  const char *status, *hls;
  int best_q = 0, best_as = 0, best_fps = 0;
  long best_vb = 0, best_ab = 0;
  yt_audio aud, best_aud;

  memset (p, 0, sizeof *p);
  yt_player_final = 0;
  if (!d)
    {
      snprintf (err, errlen, "YouTube's answer wasn't readable (%s)", jerr);
      return -1;
    }
  root = json_root (d);
  ps = j_get (root, "playabilityStatus");
  /* ERROR: unavailable, removed, private; another client won't help.
     (LOGIN_REQUIRED, UNPLAYABLE...: it might.) */
  yt_player_final = j_str (j_get (ps, "status")) && !strcmp (j_str (j_get (ps, "status")), "ERROR");
  status = j_str (j_get (ps, "status"));
  if (j_get (root, "error"))
    {
      const char *m = j_pstr (root, "error.message");
      snprintf (err, errlen, "YouTube: %s", m ? m : "error");
      json_free (d);
      return -1;
    }
  if (!status || strcmp (status, "OK"))
    {
      buf[0] = 0;
      if (!j_text (j_get (ps, "reason"), buf, sizeof buf))
        if (!j_text (j_path (ps, "errorScreen.playerErrorMessageRenderer.reason"), buf, sizeof buf))
          j_text (j_path (ps, "messages.0"), buf, sizeof buf);
      {
        char shown[sizeof buf];                    /* (not in place: it can grow) */
        text_display_buf (buf, shown, sizeof shown);
        /* "...not a bot", in English or not ("Bot", "robot"); not an age check */
        snprintf (err, errlen, "YouTube won't play this video%s%s%s",
                  shown[0] ? ": " : "", shown, !strcmp (status ? status : "", "LOGIN_REQUIRED")
                  && says_bot (buf) ? " (try again later, or update the client settings in TubeSearch's Config)" : "");
      }
      json_free (d);
      return -1;
    }
  if (j_pstr (root, "videoDetails.title"))
    p->title = strdup (j_pstr (root, "videoDetails.title"));
  p->live = j_true (j_path (root, "videoDetails.isLive"));
  sd = j_get (root, "streamingData");
  hls = j_str (j_get (sd, "hlsManifestUrl"));
  af = j_get (sd, "adaptiveFormats");

  if (!j_true (j_path (root, "videoDetails.isLive")) && af && af->type == J_ARR)
    {
      for (f = af->child; f; f = f->next)
        {
          const char *mime = j_str (j_get (f, "mimeType"));
          long rate = j_long (j_get (f, "bitrate"), 0);
          if (!mime)
            continue;
          if (!j_str (j_get (f, "url")) && !j_str (j_get (f, "signatureCipher")) && !j_str (j_get (f, "cipher")))
            continue;
          if (!strncmp (mime, "video/mp4", 9) && strstr (mime, "avc1"))
            {
              int q = lines_of (f), fps = (int) j_long (j_get (f, "fps"), 30);
              if (better_video (best_v != NULL, q, fps, rate, best_q, best_fps, best_vb, maxq))
                {
                  best_v = f;
                  best_q = q;
                  best_fps = fps;
                  best_vb = rate;
                }
            }
          else if (!strncmp (mime, "audio/mp4", 9) && !dash_audio (f, &aud))
            {
              int sc = yt_audio_score (&aud);
              if (!best_a || sc > best_as || (sc == best_as && rate > best_ab))
                {
                  best_a = f;
                  best_ab = rate;
                  best_as = sc;
                  best_aud = aud;
                }
            }
        }
    }
  if (best_v && best_a)
    {
      const char *mime = j_str (j_get (best_v, "mimeType"));
      const char *c = mime ? strstr (mime, "codecs=\"") : NULL;
      if (format_address (best_v, &p->video_url, &p->video_s, &p->video_sp, &p->video_n)
          || format_address (best_a, &p->audio_url, &p->audio_s, &p->audio_sp, &p->audio_n))
        {
          free (p->video_url);
          p->video_url = NULL;
        }
      p->quality = best_q;
      p->audio = best_aud;
      if (c)
        {
          size_t l = strcspn (c + 8, "\"");
          snprintf (p->codec, sizeof p->codec, "%.*s", (int) (l < sizeof p->codec ? l : sizeof p->codec - 1), c + 8);
        }
    }
  if (hls)
    p->hls_url = strdup (hls);                /* (as well: yt_want_hls decides) */
  if (!p->video_url && !hls)
    {
      /* the old muxed formats (360p H.264 + AAC in one file) */
      const jnode *fm = j_get (sd, "formats");
      if (fm && fm->type == J_ARR)
        for (f = fm->child; f; f = f->next)
          if (j_str (j_get (f, "url")) && j_str (j_get (f, "mimeType"))
              && strstr (j_str (j_get (f, "mimeType")), "avc1"))
            {
              p->video_url = strdup (j_str (j_get (f, "url")));
              p->quality = lines_of (f);
              strcpy (p->codec, "avc1");
              break;
            }
    }
  json_free (d);
  if (!p->video_url && !p->hls_url)
    {
      snprintf (err, errlen, "YouTube gave no H.264 streams for this video");
      yt_play_free (p);
      return -1;
    }
  if ((p->video_url && !p->video_url[0]) || (p->audio_url && !p->audio_url[0]))
    {
      snprintf (err, errlen, "out of memory");
      yt_play_free (p);
      return -1;
    }
  return 0;
}

void yt_play_free (yt_play *p)
{
  free (p->video_s); free (p->video_sp); free (p->video_n);
  free (p->audio_s); free (p->audio_sp); free (p->audio_n);
  free (p->title);
  free (p->video_url);
  free (p->audio_url);
  free (p->hls_url);
  memset (p, 0, sizeof *p);
}

/* ---- HLS ---------------------------------------------------------------- */

int yt_want_hls (const yt_play *p)
{
  return p->hls_url && (p->live || !p->video_url || !strcmp (CFG ("stream_format"), "hls"));
}

int yt_split_url (const char *url, char *host, size_t hostsize, char *path, size_t pathsize)
{
  const char *h, *s;
  if (strncmp (url, "https://", 8))
    return -1;
  h = url + 8;
  s = h + strcspn (h, "/?#");
  if (s == h || (size_t) (s - h) >= hostsize || strlen (s) + 2 > pathsize)
    return -1;
  snprintf (host, hostsize, "%.*s", (int) (s - h), h);
  snprintf (path, pathsize, "%s%s", *s == '/' ? "" : "/", s);
  return 0;
}

/* The value of attribute name in an EXT-X tag's attribute list (quotes
   removed), into out. 1 if it's there. */
static int hls_attr (const char *list, size_t n, const char *name, char *out, size_t size)
{
  size_t nl = strlen (name), i = 0;
  while (i < n)
    {
      size_t k = i, v, e;
      while (k < n && list[k] != '=' && list[k] != ',')
        k++;
      if (k >= n || list[k] != '=')
        {
          i = k + 1;
          continue;
        }
      v = k + 1;
      if (v < n && list[v] == '"')
        {
          e = v + 1;
          while (e < n && list[e] != '"')
            e++;
          if (k - i == nl && !strncmp (list + i, name, nl))
            {
              snprintf (out, size, "%.*s", (int) (e - v - 1), list + v + 1);
              return 1;
            }
          i = e < n ? e + 1 : n;
        }
      else
        {
          e = v;
          while (e < n && list[e] != ',')
            e++;
          if (k - i == nl && !strncmp (list + i, name, nl))
            {
              snprintf (out, size, "%.*s", (int) (e - v), list + v);
              return 1;
            }
          i = e;
        }
      if (i < n && list[i] == ',')
        i++;
    }
  return 0;
}

/* name (n bytes) resolved against base (malloc'd). */
static char *hls_resolve (const char *base, const char *name, size_t n)
{
  sb b = { 0 };
  if (n >= 8 && (!strncmp (name, "https://", 8) || !strncmp (name, "http://", 7)))
    sb_add (&b, name, n);
  else if (base && !strncmp (base, "https://", 8))
    {
      const char *host_end = base + 8 + strcspn (base + 8, "/?#");
      if (*name == '/')
        sb_add (&b, base, (size_t) (host_end - base));
      else
        {
          const char *q = host_end + strcspn (host_end, "?#"), *slash = q;
          while (slash > host_end && slash[-1] != '/')
            slash--;
          if (slash == host_end)
            {
              sb_add (&b, base, (size_t) (host_end - base));
              sb_str (&b, "/");
            }
          else
            sb_add (&b, base, (size_t) (slash - base));
        }
      sb_add (&b, name, n);
    }
  else
    return NULL;
  return sb_done (&b);
}

int yt_hls_choose (const char *m3u8, size_t len, const char *base_url, int maxq,
                   char **video, char **audio, int *quality, char *codec, size_t codecsize,
                   yt_audio *audio_info, char *err, size_t errlen)
{
  /* the audio renditions, and the best variant so far */
  struct { char group[64]; const char *uri; size_t urilen; yt_audio a; } aud[64];
  int na = 0, have = 0, best_q = 0, best_fps = 0;
  long best_bw = 0;
  const char *best_uri = NULL;
  size_t best_urilen = 0;
  char best_group[64] = "", best_codec[64] = "";
  const char *p = m3u8, *end = m3u8 + len;
  const char *pending = NULL;                   /* an EXT-X-STREAM-INF waiting for its line */
  size_t pending_len = 0;

  *video = *audio = NULL;
  memset (audio_info, 0, sizeof *audio_info);
  audio_info->original = audio_info->dflt = 1;  /* (the sound in the video: its only track) */
  if (len < 7 || strncmp (m3u8, "#EXTM3U", 7))
    {
      snprintf (err, errlen, "YouTube's HLS playlist wasn't one");
      return -1;
    }
  while (p < end)
    {
      const char *a = p, *e = memchr (p, '\n', (size_t) (end - p)), *b;
      if (!e)
        e = end;
      b = e;
      p = e < end ? e + 1 : end;
      while (b > a && (b[-1] == '\r' || b[-1] == ' '))
        b--;
      if (a == b)
        continue;
      if (b - a > 13 && !strncmp (a, "#EXT-X-MEDIA:", 13))
        {
          const char *l = a + 13;
          size_t n = (size_t) (b - l);
          char t[16], g[64], u[8], d[8], lang[32], name[128];
          const char *uq;
          if (na < 64 && hls_attr (l, n, "TYPE", t, sizeof t) && !strcmp (t, "AUDIO")
              && hls_attr (l, n, "GROUP-ID", g, sizeof g) && hls_attr (l, n, "URI", u, sizeof u)
              && (uq = strstr (l, "URI=\"")) && uq < b)
            {
              const char *q2 = memchr (uq + 5, '"', (size_t) (b - uq - 5));
              if (q2)
                {
                  snprintf (aud[na].group, sizeof aud[na].group, "%s", g);
                  aud[na].uri = uq + 5;
                  aud[na].urilen = (size_t) (q2 - uq - 5);
                  memset (&aud[na].a, 0, sizeof aud[na].a);
                  aud[na].a.dflt = hls_attr (l, n, "DEFAULT", d, sizeof d) && !strcmp (d, "YES");
                  if (hls_attr (l, n, "LANGUAGE", lang, sizeof lang))
                    yt_lang_primary (lang, aud[na].a.lang, sizeof aud[na].a.lang);
                  if (hls_attr (l, n, "NAME", name, sizeof name) && says_original (name))
                    aud[na].a.original = 1;
                  xtags_of (aud[na].uri, aud[na].urilen, &aud[na].a);
                  na++;
                }
            }
          continue;
        }
      if (b - a > 18 && !strncmp (a, "#EXT-X-STREAM-INF:", 18))
        {
          pending = a + 18;
          pending_len = (size_t) (b - pending);
          continue;
        }
      if (*a == '#')
        continue;
      if (pending)
        {
          char codecs[160], res[32], fr[16], bw[24], g[64] = "";
          const char *c;
          int w = 0, h = 0, q, fps;
          long rate;
          if (hls_attr (pending, pending_len, "CODECS", codecs, sizeof codecs) && (c = strstr (codecs, "avc1"))
              && hls_attr (pending, pending_len, "RESOLUTION", res, sizeof res) && sscanf (res, "%dx%d", &w, &h) == 2)
            {
              q = w < h ? w : h;
              fps = hls_attr (pending, pending_len, "FRAME-RATE", fr, sizeof fr) ? (int) (atof (fr) + 0.5) : 30;
              rate = hls_attr (pending, pending_len, "BANDWIDTH", bw, sizeof bw) ? atol (bw) : 0;
              hls_attr (pending, pending_len, "AUDIO", g, sizeof g);
              if (better_video (have, q, fps, rate, best_q, best_fps, best_bw, maxq))
                {
                  size_t cl = strcspn (c, ",");
                  have = 1;
                  best_q = q;
                  best_fps = fps;
                  best_bw = rate;
                  best_uri = a;
                  best_urilen = (size_t) (b - a);
                  snprintf (best_group, sizeof best_group, "%s", g);
                  snprintf (best_codec, sizeof best_codec, "%.*s", (int) (cl < sizeof best_codec ? cl : sizeof best_codec - 1), c);
                }
            }
          pending = NULL;
        }
    }
  if (!have)
    {
      snprintf (err, errlen, "YouTube's HLS playlist has no H.264 video");
      return -1;
    }
  if (best_group[0])
    {
      int pick = -1, best_sc = -1;
      for (int i = 0; i < na; i++)
        if (!strcmp (aud[i].group, best_group))
          {
            int sc = yt_audio_score (&aud[i].a);
            if (sc > best_sc)
              {
                pick = i;
                best_sc = sc;
              }
          }
      if (pick < 0)
        {
          snprintf (err, errlen, "YouTube's HLS playlist has no sound for its video");
          return -1;
        }
      *audio_info = aud[pick].a;
      if (!(*audio = hls_resolve (base_url, aud[pick].uri, aud[pick].urilen)))
        {
          snprintf (err, errlen, "YouTube's HLS playlist has an address TubeSearch can't use");
          return -1;
        }
    }
  if (!(*video = hls_resolve (base_url, best_uri, best_urilen)))
    {
      free (*audio);
      *audio = NULL;
      snprintf (err, errlen, "YouTube's HLS playlist has an address TubeSearch can't use");
      return -1;
    }
  *quality = best_q;
  snprintf (codec, codecsize, "%s", best_codec);
  return 0;
}

char *yt_reel_json (const yt_play *p, const char *video_id)
{
  sb b = { 0 };
  sb_str (&b, "{\"title\":");
  sb_quote (&b, p->title ? p->title : video_id);
  sb_printf (&b, ",\"webpage_url\":\"https://www.youtube.com/watch?v=%s\"", video_id);
  sb_str (&b, ",\"http_headers\":{\"User-Agent\":");
  sb_quote (&b, CFG ("media_user_agent"));
  sb_str (&b, "}");
  if (p->video_url && p->audio_url)
    {
      sb_str (&b, ",\"requested_formats\":[{\"url\":");
      sb_quote (&b, p->video_url);
      sb_str (&b, ",\"vcodec\":");
      sb_quote (&b, p->codec[0] ? p->codec : "avc1");
      sb_str (&b, p->hls ? ",\"protocol\":\"m3u8_native\"" : "");
      sb_str (&b, ",\"acodec\":\"none\",\"http_headers\":{\"User-Agent\":");
      sb_quote (&b, CFG ("media_user_agent"));
      sb_str (&b, "}},{\"url\":");
      sb_quote (&b, p->audio_url);
      sb_str (&b, p->hls ? ",\"protocol\":\"m3u8_native\"" : "");
      sb_str (&b, ",\"vcodec\":\"none\",\"acodec\":\"mp4a.40.2\",\"http_headers\":{\"User-Agent\":");
      sb_quote (&b, CFG ("media_user_agent"));
      sb_str (&b, "}}]");
    }
  else
    {
      sb_str (&b, ",\"url\":");
      sb_quote (&b, p->video_url ? p->video_url : p->hls_url);
    }
  sb_str (&b, "}\n");
  return sb_done (&b);
}
