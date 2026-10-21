/* subs.c - see subs.h. Part of TubeSearch. MIT licence. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "subs.h"
#include "json.h"
#include "text.h"

/* ---- the list ------------------------------------------------------------- */

int subs_is_channel_id (const char *id, size_t len)
{
  if (len != 24 || id[0] != 'U' || id[1] != 'C')
    return 0;
  for (size_t i = 2; i < len; i++)
    if (!isalnum ((unsigned char) id[i]) && id[i] != '-' && id[i] != '_')
      return 0;
  return 1;
}

void subs_free (yt_subs *s)
{
  for (int i = 0; i < s->n; i++)
    {
      free (s->v[i].name);
      free (s->v[i].thumb);
    }
  free (s->v);
  memset (s, 0, sizeof *s);
}

int subs_find (const yt_subs *s, const char *id)
{
  for (int i = 0; i < s->n; i++)
    if (!strcmp (s->v[i].id, id))
      return i;
  return -1;
}

/* a copy of t with tabs and line ends made spaces (they'd break the file) */
static char *clean (const char *t)
{
  char *c = strdup (t ? t : "");
  for (char *p = c; p && *p; p++)
    if (*p == '\t' || *p == '\n' || *p == '\r')
      *p = ' ';
  return c;
}

int subs_add (yt_subs *s, const char *id, const char *name, const char *thumb)
{
  int i;
  char *n, *t;
  if (!id || !subs_is_channel_id (id, strlen (id)))
    return -1;
  if ((i = subs_find (s, id)) >= 0)
    {
      if (name && *name && (n = clean (name)))
        {
          free (s->v[i].name);
          s->v[i].name = n;
        }
      if (thumb && *thumb && (t = clean (thumb)))
        {
          free (s->v[i].thumb);
          s->v[i].thumb = t;
        }
      return 0;
    }
  if (s->n == s->cap)
    {
      int cap = s->cap ? s->cap * 2 : 16;
      yt_sub *v = realloc (s->v, (size_t) cap * sizeof *v);
      if (!v)
        return -1;
      s->v = v;
      s->cap = cap;
    }
  n = clean (name);
  t = clean (thumb);
  if (!n || !t)
    {
      free (n);
      free (t);
      return -1;
    }
  snprintf (s->v[s->n].id, sizeof s->v[s->n].id, "%s", id);
  s->v[s->n].name = n;
  s->v[s->n].thumb = t;
  s->n++;
  return 1;
}

int subs_remove (yt_subs *s, const char *id)
{
  int i = subs_find (s, id);
  if (i < 0)
    return 0;
  free (s->v[i].name);
  free (s->v[i].thumb);
  memmove (&s->v[i], &s->v[i + 1], (size_t) (s->n - i - 1) * sizeof s->v[0]);
  s->n--;
  return 1;
}

int subs_load (yt_subs *s, const char *path)
{
  FILE *f = fopen (path, "r");
  char line[2048];
  memset (s, 0, sizeof *s);
  if (!f)
    return 0;
  while (fgets (line, sizeof line, f))
    {
      char *e = line + strlen (line), *t1, *t2;
      while (e > line && (e[-1] == '\n' || e[-1] == '\r'))
        *--e = 0;
      if (line[0] == '#' || !line[0])
        continue;
      if (!strncmp (line, "seen ", 5))
        {
          s->seen = atol (line + 5);
          continue;
        }
      t1 = strchr (line, '\t');
      if (t1)
        *t1++ = 0;
      t2 = t1 ? strchr (t1, '\t') : NULL;
      if (t2)
        *t2++ = 0;
      if (subs_add (s, line, t1 ? t1 : "", t2 ? t2 : "") < 0 && subs_is_channel_id (line, strlen (line)))
        {
          fclose (f);
          return -1;
        }
    }
  fclose (f);
  return 0;
}

int subs_save (const yt_subs *s, const char *path)
{
  char tmp[400];
  FILE *f;
  snprintf (tmp, sizeof tmp, "%s-new", path);
  if (!(f = fopen (tmp, "w")))
    return -1;
  fprintf (f, "# TubeSearch's subscriptions: a channel a line (ID, tab, name, tab, picture)\nseen %ld\n", s->seen);
  for (int i = 0; i < s->n; i++)
    fprintf (f, "%s\t%s\t%s\n", s->v[i].id, s->v[i].name, s->v[i].thumb);
  if (fclose (f))
    {
      remove (tmp);
      return -1;
    }
  remove (path);
  return rename (tmp, path) ? -1 : 0;
}

int subs_channel_list (const yt_subs *s, yt_list *list)
{
  int added = 0;
  for (int i = 0; i < s->n; i++)
    {
      yt_item *it;
      int dup = 0;
      for (int k = 0; k < list->n; k++)
        dup |= !strcmp (list->items[k].id, s->v[i].id);
      if (dup)
        continue;
      if (list->n == list->cap)
        {
          int cap = list->cap ? list->cap * 2 : 32;
          yt_item *v = realloc (list->items, (size_t) cap * sizeof *v);
          if (!v)
            break;
          list->items = v;
          list->cap = cap;
        }
      it = &list->items[list->n];
      memset (it, 0, sizeof *it);
      it->kind = YT_CHANNEL;
      snprintf (it->id, sizeof it->id, "%s", s->v[i].id);
      snprintf (it->channel_id, sizeof it->channel_id, "%s", s->v[i].id);
      it->title = text_display (s->v[i].name[0] ? s->v[i].name : s->v[i].id);
      it->channel = strdup ("");
      it->meta = strdup ("Subscribed");
      it->thumb_url = strdup (s->v[i].thumb);
      if (!it->title || !it->channel || !it->meta || !it->thumb_url)
        {
          free (it->title); free (it->channel); free (it->meta); free (it->thumb_url);
          break;
        }
      list->n++;
      added++;
    }
  return added;
}

/* ---- bringing a list in ------------------------------------------------------ */

/* "...channel/UC..." in an address: the ID */
static int id_in_url (const char *url, char out[40])
{
  const char *c = url ? strstr (url, "channel/") : NULL;
  if (!c || !subs_is_channel_id (c + 8, 24))
    return 0;
  memcpy (out, c + 8, 24);
  out[24] = 0;
  return 1;
}

static int import_json (yt_subs *s, const jnode *root, yt_subs *seen)
{
  int added = 0;
  const jnode *arr = j_get (root, "subscriptions");
  if (!arr && root && root->type == J_ARR)
    arr = root;
  if (!arr || arr->type != J_ARR)
    return -1;
  for (const jnode *e = arr->child; e; e = e->next)
    {
      char id[40] = "";
      const char *i = j_str (j_get (e, "id")), *name = j_str (j_get (e, "name"));
      if (i && subs_is_channel_id (i, strlen (i)))
        snprintf (id, sizeof id, "%s", i);
      else if (!id_in_url (j_str (j_get (e, "url")), id))
        continue;
      if (!name)
        name = j_str (j_get (e, "title"));
      subs_add (seen, id, "", "");                /* (the different channels in the list) */
      if (subs_add (s, id, name ? name : "", j_str (j_get (e, "thumbnail"))) == 1)
        added++;
    }
  return added;
}

/* one CSV line's fields (quoted ones unquoted), into f[] */
static int csv_fields (const char *a, const char *b, char f[][256], int max)
{
  int n = 0;
  while (a <= b && n < max)
    {
      size_t o = 0;
      if (a < b && *a == '"')
        {
          a++;
          while (a < b)
            {
              if (*a == '"' && a + 1 < b && a[1] == '"')
                a++;
              else if (*a == '"')
                {
                  a++;
                  break;
                }
              if (o + 1 < 256)
                f[n][o++] = *a;
              a++;
            }
          while (a < b && *a != ',')
            a++;
        }
      else
        while (a < b && *a != ',')
          {
            if (o + 1 < 256)
              f[n][o++] = *a;
            a++;
          }
      f[n][o] = 0;
      n++;
      if (a >= b)
        break;
      a++;                                  /* (the comma) */
    }
  return n;
}

static int import_lines (yt_subs *s, const char *p, const char *end, yt_subs *seen);

int subs_import (yt_subs *s, const char *text, size_t len, int *found)
{
  const char *p = text, *end = text + len;
  int added = -1, any = 0;
  yt_subs seen;
  memset (&seen, 0, sizeof seen);
  *found = 0;
  while (p < end && (isspace ((unsigned char) *p) || (unsigned char) *p == 0xEF || (unsigned char) *p == 0xBB
                     || (unsigned char) *p == 0xBF))
    p++;                                    /* (and a UTF-8 byte order mark) */
  if (p < end && (*p == '{' || *p == '['))
    {
      /* a JSON document a line (FreeTube), or one document (NewPipe; or an
         array, however it's laid out) */
      int lines = 0, k;
      jdoc *d;
      for (const char *q = p; q < end; q++)
        if (*q == '{' && (q == p || q[-1] == '\n'))
          lines++;
      if (lines > 1)
        for (const char *q = p; q < end;)
          {
            const char *e = memchr (q, '\n', (size_t) (end - q));
            if (!e)
              e = end;
            if (e > q && *q == '{' && (d = json_parse (q, (size_t) (e - q), NULL, 0)))
              {
                k = import_json (s, json_root (d), &seen);
                json_free (d);
                if (k >= 0)
                  {
                    added = (any ? added : 0) + k;
                    any = 1;
                  }
              }
            q = e < end ? e + 1 : end;
          }
      if (!any && (d = json_parse (p, (size_t) (end - p), NULL, 0)))
        {
          added = import_json (s, json_root (d), &seen);
          json_free (d);
        }
    }
  else
    added = import_lines (s, p, end, &seen);
  *found = seen.n;
  subs_free (&seen);
  return added;
}

/* CSV (Google Takeout, in any language: a field that is a channel ID, and
   the title in the last field that isn't an address), or TubeSearch's own
   file (ID, tab, name, tab, picture) */
static int import_lines (yt_subs *s, const char *p, const char *end, yt_subs *seen)
{
  int added = 0, any = 0;
  while (p < end)
    {
      const char *e = memchr (p, '\n', (size_t) (end - p)), *b, *tab;
      char f[4][256];
      int n, idf = -1, namef = -1;
      if (!e)
        e = end;
      b = e;
      while (b > p && (b[-1] == '\r'))
        b--;
      if ((tab = memchr (p, '\t', (size_t) (b - p))) && subs_is_channel_id (p, (size_t) (tab - p)))
        {
          /* a TubeSearch Subscriptions file */
          const char *t2 = memchr (tab + 1, '\t', (size_t) (b - tab - 1));
          char id[40], name[256];
          snprintf (id, sizeof id, "%.24s", p);
          snprintf (name, sizeof name, "%.*s", (int) ((t2 ? t2 : b) - tab - 1), tab + 1);
          any = 1;
          subs_add (seen, id, "", "");
          if (subs_add (s, id, name, "") == 1)
            added++;
          p = e < end ? e + 1 : end;
          continue;
        }
      n = csv_fields (p, b, f, 4);
      for (int i = 0; i < n && idf < 0; i++)
        if (subs_is_channel_id (f[i], strlen (f[i])))
          idf = i;
      for (int i = n - 1; i >= 0 && namef < 0; i--)
        if (i != idf && f[i][0] && strncmp (f[i], "http", 4))
          namef = i;
      if (idf >= 0)
        {
          any = 1;
          subs_add (seen, f[idf], "", "");
          if (subs_add (s, f[idf], namef >= 0 ? f[namef] : "", "") == 1)
            added++;
        }
      p = e < end ? e + 1 : end;
    }
  return any ? added : -1;
}

/* ---- times ---------------------------------------------------------------------- */

/* days since 1970-01-01 of a civil date (Howard Hinnant's algorithm) */
static long days_from_civil (long y, int m, int d)
{
  long era, yoe, doy, doe;
  y -= m <= 2;
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = y - era * 400;
  doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

long subs_parse_time (const char *iso)
{
  int y, mo, d, h, mi, sec = 0, oh = 0, om = 0, n = 0;
  char sign = 0;
  long t;
  if (!iso || sscanf (iso, "%4d-%2d-%2dT%2d:%2d:%2d%n", &y, &mo, &d, &h, &mi, &sec, &n) < 6 || !n)
    return -1;
  iso += n;
  while (*iso == '.' || isdigit ((unsigned char) *iso))
    iso++;                                  /* (fractions of a second) */
  if (*iso == '+' || *iso == '-')
    {
      sign = *iso;
      if (sscanf (iso + 1, iso[3] == ':' ? "%2d:%2d" : "%2d%2d", &oh, &om) < 1)    /* +05:30, +0530, +05 */
        return -1;
    }
  if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || sec > 60)
    return -1;
  t = days_from_civil (y, mo, d) * 86400L + h * 3600L + mi * 60L + sec;
  if (sign == '+')
    t -= oh * 3600L + om * 60L;
  else if (sign == '-')
    t += oh * 3600L + om * 60L;
  return t;
}

static const struct { const char *word; long secs; } units[] = {
  { "second", 1 }, { "minute", 60 }, { "hour", 3600 }, { "day", 86400 },
  { "week", 7 * 86400L }, { "month", 30 * 86400L }, { "year", 365 * 86400L },
};

long subs_ago_seconds (const char *text)
{
  const char *a = text ? strstr (text, " ago") : NULL, *p;
  long n;
  if (!a)
    return -1;
  /* back over the unit, then the number */
  p = a;
  while (p > text && isalpha ((unsigned char) p[-1]))
    p--;
  for (size_t u = 0; u < sizeof units / sizeof units[0]; u++)
    if (!strncmp (p, units[u].word, strlen (units[u].word)))
      {
        const char *q = p;
        while (q > text && q[-1] == ' ')
          q--;
        while (q > text && isdigit ((unsigned char) q[-1]))
          q--;
        n = atol (q);
        return (n > 0 ? n : 1) * units[u].secs;
      }
  return -1;
}

void subs_ago_text (long s, char *out, size_t size)
{
  int u = 6;
  long n;
  if (s < 60)
    {
      snprintf (out, size, "just now");
      return;
    }
  while (u > 0 && s < units[u].secs)
    u--;
  n = s / units[u].secs;
  snprintf (out, size, "%ld %s%s ago", n, units[u].word, n == 1 ? "" : "s");
}

/* ---- the feeds ---------------------------------------------------------------------- */

void subs_feed_path (const char *channel_id, char *out, size_t size)
{
  snprintf (out, size, "/feeds/videos.xml?channel_id=%s", channel_id);
}

/* The text between <tag ...> and </tag> in [a, b) (entities decoded) into
   out; *next after it. 1 if found. */
static int xml_text (const char *a, const char *b, const char *tag, char *out, size_t size, const char **next)
{
  char open[64], close[64];
  const char *s, *e, *t;
  size_t o = 0;
  snprintf (open, sizeof open, "<%s", tag);
  snprintf (close, sizeof close, "</%s>", tag);
  for (s = a; (s = strstr (s, open)) && s < b; s++)
    if (s[strlen (open)] == '>' || s[strlen (open)] == ' ')
      break;
  if (!s || s >= b || !(t = strchr (s, '>')) || t >= b)
    return 0;
  t++;
  if (!(e = strstr (t, close)) || e > b)
    return 0;
  if (next)
    *next = e + strlen (close);
  if (e - t >= 9 && !strncmp (t, "<![CDATA[", 9))
    {
      const char *c = strstr (t, "]]>");
      if (c && c < e)
        {
          snprintf (out, size, "%.*s", (int) (c - t - 9), t + 9);
          return 1;
        }
    }
  while (t < e && o + 1 < size)
    {
      if (*t == '&')
        {
          static const struct { const char *n; char c; } ent[] = {
            { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&apos;", '\'' } };
          int done = 0;
          for (size_t k = 0; k < sizeof ent / sizeof ent[0] && !done; k++)
            if (!strncmp (t, ent[k].n, strlen (ent[k].n)))
              {
                out[o++] = ent[k].c;
                t += strlen (ent[k].n);
                done = 1;
              }
          if (!done && t[1] == '#')
            {
              char *x;
              unsigned long cp = t[2] == 'x' ? strtoul (t + 3, &x, 16) : strtoul (t + 2, &x, 10);
              if (*x == ';' && cp > 0 && cp < 0x110000 && o + 5 < size)
                {
                  /* as UTF-8 */
                  if (cp < 0x80)
                    out[o++] = (char) cp;
                  else if (cp < 0x800)
                    out[o++] = (char) (0xC0 | (cp >> 6)), out[o++] = (char) (0x80 | (cp & 63));
                  else if (cp < 0x10000)
                    out[o++] = (char) (0xE0 | (cp >> 12)), out[o++] = (char) (0x80 | ((cp >> 6) & 63)),
                    out[o++] = (char) (0x80 | (cp & 63));
                  else
                    out[o++] = (char) (0xF0 | (cp >> 18)), out[o++] = (char) (0x80 | ((cp >> 12) & 63)),
                    out[o++] = (char) (0x80 | ((cp >> 6) & 63)), out[o++] = (char) (0x80 | (cp & 63));
                  t = x + 1;
                  done = 1;
                }
            }
          if (!done)
            out[o++] = *t++;
        }
      else
        out[o++] = *t++;
    }
  out[o] = 0;
  return 1;
}

/* the value of attribute name in the first <tag ...> in [a, b) */
static int xml_attr (const char *a, const char *b, const char *tag, const char *name, char *out, size_t size)
{
  char open[64], key[64];
  const char *s, *e, *v, *q;
  snprintf (open, sizeof open, "<%s ", tag);
  snprintf (key, sizeof key, " %s=\"", name);
  if (!(s = strstr (a, open)) || s >= b || !(e = strchr (s, '>')) || e > b)
    return 0;
  for (v = s; (v = strstr (v, key)) && v < e; v++)
    {
      v += strlen (key);
      if (!(q = strchr (v, '"')) || q > e)
        return 0;
      snprintf (out, size, "%.*s", (int) (q - v), v);
      return 1;
    }
  return 0;
}

static void views_text (long v, char *out, size_t size)
{
  if (v >= 1000000000L)
    snprintf (out, size, "%.1fB views", v / 1e9);
  else if (v >= 1000000L)
    snprintf (out, size, v >= 10000000L ? "%.0fM views" : "%.1fM views", v / 1e6);
  else if (v >= 1000)
    snprintf (out, size, v >= 10000 ? "%.0fK views" : "%.1fK views", v / 1e3);
  else
    snprintf (out, size, "%ld view%s", v, v == 1 ? "" : "s");
  /* "1.0K" reads oddly: "1K" */
  {
    char *z = strstr (out, ".0");
    if (z && (z[2] == 'K' || z[2] == 'M' || z[2] == 'B'))
      memmove (z, z + 2, strlen (z + 2) + 1);
  }
}

char *subs_mark_new (const char *meta)
{
  char *mark = text_display ("New \xE2\x80\xA2"), *m = NULL;    /* (text_display drops a space at the end) */
  if (mark && (m = malloc (strlen (mark) + strlen (meta) + 2)))
    sprintf (m, "%s %s", mark, meta);
  free (mark);
  return m;
}

static int is_video_id (const char *id)
{
  size_t n = strlen (id);
  if (n != 11)
    return 0;
  for (size_t i = 0; i < n; i++)
    if (!isalnum ((unsigned char) id[i]) && id[i] != '-' && id[i] != '_')
      return 0;
  return 1;
}

int subs_parse_feed (const char *xml, size_t len, yt_list *list, long now, long seen, char *err, size_t errlen,
                     char *name_out, size_t namesize)
{
  const char *p, *end = xml + len, *e;
  char chan[400] = "", chan_id[40] = "";
  int added = 0;
  if (name_out && namesize)
    name_out[0] = 0;
  /* (the text must end: strstr below) */
  if (len < 5 || !memchr (xml, '<', len < 200 ? len : 200) || !strstr (xml, "<feed")
      || !strstr (xml, "http://www.w3.org/2005/Atom"))
    {
      snprintf (err, errlen, "the channel's feed wasn't one");
      return -1;
    }
  p = strstr (xml, "<feed");
  {
    const char *first = strstr (p, "<entry");
    const char *head_end = first && first < end ? first : end;
    char t[400];
    if (xml_text (p, head_end, "yt:channelId", t, sizeof t, NULL) && subs_is_channel_id (t, strlen (t)))
      snprintf (chan_id, sizeof chan_id, "%.39s", t);
    if (!xml_text (p, head_end, "name", chan, sizeof chan, NULL))
      xml_text (p, head_end, "title", chan, sizeof chan, NULL);
    if (name_out && namesize)
      snprintf (name_out, namesize, "%s", chan);
  }
  while ((p = strstr (p, "<entry")) && p < end && (e = strstr (p, "</entry>")) && e < end)
    {
      char id[40] = "", title[600] = "", when[64] = "", views[24] = "", thumb[400] = "", name[400] = "";
      char meta[200], ago[64], vtext[48];
      long pub, v = -1;
      int dup = 0;
      yt_item *it;
      e += 8;
      if (!xml_text (p, e, "yt:videoId", id, sizeof id, NULL) || !is_video_id (id))
        {
          p = e;
          continue;
        }
      {
        /* the entry's own title, not media:title (the same, but be sure) */
        const char *tt = strstr (p, "<title");
        if (tt && tt < e)
          xml_text (tt, e, "title", title, sizeof title, NULL);
        if (!title[0])
          xml_text (p, e, "media:title", title, sizeof title, NULL);
      }
      xml_text (p, e, "published", when, sizeof when, NULL);
      if (xml_attr (p, e, "media:statistics", "views", views, sizeof views))
        v = atol (views);
      xml_attr (p, e, "media:thumbnail", "url", thumb, sizeof thumb);
      if (!xml_text (p, e, "name", name, sizeof name, NULL))
        snprintf (name, sizeof name, "%s", chan);
      for (int k = 0; k < list->n; k++)
        dup |= !strcmp (list->items[k].id, id);
      p = e;
      if (dup)
        continue;
      if (list->n == list->cap)
        {
          int cap = list->cap ? list->cap * 2 : 32;
          yt_item *nv = realloc (list->items, (size_t) cap * sizeof *nv);
          if (!nv)
            break;
          list->items = nv;
          list->cap = cap;
        }
      pub = subs_parse_time (when);
      it = &list->items[list->n];
      memset (it, 0, sizeof *it);
      it->kind = YT_VIDEO;
      snprintf (it->id, sizeof it->id, "%s", id);
      snprintf (it->channel_id, sizeof it->channel_id, "%s", chan_id);
      it->published = pub > 0 ? pub : 0;
      if (pub > 0)
        subs_ago_text (now - pub, ago, sizeof ago);
      else
        ago[0] = 0;
      vtext[0] = 0;
      if (v >= 0)
        views_text (v, vtext, sizeof vtext);
      snprintf (meta, sizeof meta, "%s%s%s%s", seen && pub > seen ? "New \xE2\x80\xA2 " : "", vtext,
                vtext[0] && ago[0] ? " \xE2\x80\xA2 " : "", ago);
      it->title = text_display (title);
      it->channel = text_display (name);
      it->meta = text_display (meta);
      /* the 320x180 picture, as the lists use (the feed names the 480x360 hqdefault) */
      if (thumb[0])
        {
          char *h = strstr (thumb, "/hqdefault.jpg");
          char t2[420];
          if (h)
            snprintf (t2, sizeof t2, "%.*s/mqdefault.jpg", (int) (h - thumb), thumb);
          else
            snprintf (t2, sizeof t2, "%s", thumb);
          it->thumb_url = strdup (t2);
        }
      else
        {
          char t2[100];
          snprintf (t2, sizeof t2, "https://i.ytimg.com/vi/%s/mqdefault.jpg", id);
          it->thumb_url = strdup (t2);
        }
      if (!it->title || !it->channel || !it->meta || !it->thumb_url)
        {
          free (it->title); free (it->channel); free (it->meta); free (it->thumb_url);
          break;
        }
      list->n++;
      added++;
    }
  return added;
}

/* ---- newest first ------------------------------------------------------------------- */

static int newer (const void *a, const void *b)
{
  const yt_item *x = a, *y = b;
  if (x->published != y->published)
    return x->published > y->published ? -1 : 1;
  return 0;
}

void subs_sort_newest (yt_list *list, int max)
{
  /* stable: insertion sort (lists of a few hundred) */
  for (int i = 1; i < list->n; i++)
    {
      yt_item t = list->items[i];
      int j = i;
      while (j > 0 && newer (&list->items[j - 1], &t) > 0)
        {
          list->items[j] = list->items[j - 1];
          j--;
        }
      list->items[j] = t;
    }
  while (max >= 0 && list->n > max)
    {
      yt_item *it = &list->items[--list->n];
      free (it->title); free (it->channel); free (it->meta); free (it->thumb_url);
    }
}
