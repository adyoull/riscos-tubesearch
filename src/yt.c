/* yt.c - see yt.h. Part of Tube. MIT licence. */
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

static void web_context (sb *b, int visitor_request)
{
  sb_str (b, "{\"context\":{\"client\":{\"clientName\":\"WEB\",\"clientVersion\":");
  sb_quote (b, CFG ("web_client_version"));
  if (visitor_request)
    sb_str (b, ",\"clientScreen\":\"WATCH\"");
  sb_str (b, ",\"hl\":");
  sb_quote (b, CFG ("language"));
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
             CFG ("web_user_agent"), CFG ("web_client_version"), CFG ("language"));
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

yt_req *yt_req_search (const char *query)
{
  sb b = { 0 };
  web_context (&b, 0);
  sb_str (&b, ",\"query\":");
  sb_quote (&b, query);
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

static int lines_of (const jnode *f)
{
  const char *q = j_str (j_get (f, "qualityLabel"));
  if (q && *q >= '0' && *q <= '9')
    return atoi (q);
  return (int) j_long (j_get (f, "height"), 0);
}

/* The other language tracks and the "DRC" (quieter) copies of the sound
   are left out: the default track only. */
static int default_audio (const jnode *f)
{
  const jnode *t = j_get (f, "audioTrack");
  if (j_true (j_get (f, "isDrc")))
    return 0;
  return !t || j_true (j_get (t, "audioIsDefault"));
}

int yt_parse_player (const char *json, size_t len, int maxq, yt_play *p, char *err, size_t errlen)
{
  char jerr[96], buf[512];
  jdoc *d = json_parse (json, len, jerr, sizeof jerr);
  const jnode *root, *ps, *sd, *af, *best_v = NULL, *best_a = NULL, *f;
  const char *status, *hls;
  int best_q = 0, signed_only = 0;
  long best_vb = 0, best_ab = 0;

  memset (p, 0, sizeof *p);
  if (!d)
    {
      snprintf (err, errlen, "YouTube's answer wasn't readable (%s)", jerr);
      return -1;
    }
  root = json_root (d);
  ps = j_get (root, "playabilityStatus");
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
      text_display_buf (buf, buf, sizeof buf);
      snprintf (err, errlen, "YouTube won't play this video%s%s%s",
                buf[0] ? ": " : "", buf, !strcmp (status ? status : "", "LOGIN_REQUIRED")
                && strstr (buf, "bot") ? " (try again later, or update the client settings in Tube's Config)" : "");
      json_free (d);
      return -1;
    }
  if (j_pstr (root, "videoDetails.title"))
    p->title = strdup (j_pstr (root, "videoDetails.title"));
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
          if (!j_str (j_get (f, "url")))
            {
              if (j_get (f, "signatureCipher") || j_get (f, "cipher"))
                signed_only = 1;
              continue;
            }
          if (!strncmp (mime, "video/mp4", 9) && strstr (mime, "avc1"))
            {
              int q = lines_of (f);
              /* the tallest no taller than maxq; if all are taller, the smallest */
              int better = !best_v
                || (q <= maxq && (best_q > maxq || q > best_q || (q == best_q && rate > best_vb)))
                || (q > maxq && best_q > maxq && q < best_q);
              if (better)
                {
                  best_v = f;
                  best_q = q;
                  best_vb = rate;
                }
            }
          else if (!strncmp (mime, "audio/mp4", 9) && default_audio (f))
            {
              if (!best_a || rate > best_ab)
                {
                  best_a = f;
                  best_ab = rate;
                }
            }
        }
    }
  if (best_v && best_a)
    {
      const char *mime = j_str (j_get (best_v, "mimeType"));
      const char *c = mime ? strstr (mime, "codecs=\"") : NULL;
      p->video_url = strdup (j_str (j_get (best_v, "url")));
      p->audio_url = strdup (j_str (j_get (best_a, "url")));
      p->quality = best_q;
      if (c)
        {
          size_t l = strcspn (c + 8, "\"");
          snprintf (p->codec, sizeof p->codec, "%.*s", (int) (l < sizeof p->codec ? l : sizeof p->codec - 1), c + 8);
        }
    }
  else if (hls)
    p->hls_url = strdup (hls);
  else
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
      snprintf (err, errlen, signed_only
                ? "This video's addresses are scrambled (they need YouTube's JavaScript, which Tube can't run yet)"
                : "YouTube gave no H.264 streams for this video");
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
  free (p->title);
  free (p->video_url);
  free (p->audio_url);
  free (p->hls_url);
  memset (p, 0, sizeof *p);
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
      sb_str (&b, ",\"acodec\":\"none\",\"http_headers\":{\"User-Agent\":");
      sb_quote (&b, CFG ("media_user_agent"));
      sb_str (&b, "}},{\"url\":");
      sb_quote (&b, p->audio_url);
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
