/* save.c - see save.h. Part of TubeSearch. MIT licence. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "http.h"
#include "save.h"
#include "text.h"

#define TAG_SAVE 7
#define TRIES 3

long long save_range_size = 4LL * 1024 * 1024;

typedef struct {
  char *url;                  /* the playlist, or the stream itself */
  int hls;
  char *file;
  FILE *f;
  char **parts;               /* HLS: the pieces' addresses */
  int nparts;
  int part;                   /* HLS: the next piece */
  long long clen;             /* not HLS: the size (clen=), or -1 unknown */
  long long at;               /* not HLS: bytes written */
} stream;

struct save_job {
  stream s[2];
  int ns;
  int cur;                    /* the stream being fetched */
  int phase;                  /* 0 reading the playlists, 1 fetching */
  int tries;
  int done_parts, total_parts;
  long long bytes;
  http_req *req;
  char headers[600];
  save_progress_fn progress;
  save_done_fn done;
  void *ctx;
  int starting;               /* in save_start: a failure is returned, not called back */
  int start_failed;
  char start_err[300];
};

static void next (save_job *j);

static void finish (save_job *j, int ok, const char *err)
{
  save_done_fn done = j->done;
  void *ctx = j->ctx;
  if (j->starting)
    {
      j->start_failed = 1;
      snprintf (j->start_err, sizeof j->start_err, "%s", err ? err : "Out of memory");
      return;
    }
  for (int i = 0; i < j->ns; i++)
    {
      if (j->s[i].f)
        {
          if (fclose (j->s[i].f) && ok)
            {
              ok = 0;
              err = "The file couldn't be written (is the disc full?)";
            }
          j->s[i].f = NULL;
        }
    }
  {
    char e[300];
    snprintf (e, sizeof e, "%s", err ? err : "");
    for (int i = 0; i < j->ns; i++)
      {
        free (j->s[i].url);
        free (j->s[i].file);
        yt_free_strings (j->s[i].parts, j->s[i].nparts);
      }
    free (j);
    if (done)
      done (ok, e, ctx);
  }
}

static int fetch (save_job *j, const char *url, const char *range_header, http_done_fn cb)
{
  char host[256], *path = malloc (strlen (url) + 2), hdr[700];
  if (!path || yt_split_url (url, host, sizeof host, path, strlen (url) + 2))
    {
      free (path);
      return -1;                          /* (out of memory, or not an https address) */
    }
  snprintf (hdr, sizeof hdr, "%s%s", j->headers, range_header ? range_header : "");
  j->req = http_get (host, path, hdr, TAG_SAVE, cb, j);
  free (path);
  return j->req ? 0 : -1;
}

/* A failed request: again (up to TRIES times, unless the answer says it
   won't work: a 4xx other than 408 or 429), or give up. */
static void failed (save_job *j, int status, const char *why)
{
  char e[300];
  int hopeless = status >= 400 && status < 500 && status != 408 && status != 429;
  if (!hopeless && ++j->tries < TRIES)
    {
      next (j);
      return;
    }
  snprintf (e, sizeof e, "YouTube stopped sending the video (%s)", why);
  finish (j, 0, e);
}

static void playlist_done (http_req *r, void *ctx)
{
  save_job *j = ctx;
  stream *s = &j->s[j->cur];
  size_t len;
  const char *body = http_body (r, &len);
  char err[200], st[40];
  double secs;
  int n;
  j->req = NULL;
  if (http_status (r) != 200 || !body)
    {
      snprintf (st, sizeof st, "%d", http_status (r));
      failed (j, http_status (r), http_status (r) ? st : http_error (r));
      return;
    }
  if ((n = yt_hls_segments (body, len, s->url, &s->parts, &secs, err, sizeof err)) < 0)
    {
      finish (j, 0, err);
      return;
    }
  s->nparts = n;
  j->total_parts += n;
  j->tries = 0;
  j->cur++;
  next (j);
}

static void part_done (http_req *r, void *ctx)
{
  save_job *j = ctx;
  stream *s = &j->s[j->cur];
  int st = http_status (r);
  size_t len;
  const char *body = http_body (r, &len);
  char why[40];
  j->req = NULL;
  if ((st != 200 && st != 206) || !body)
    {
      snprintf (why, sizeof why, "%d", st);
      failed (j, st, st ? why : http_error (r));
      return;
    }
  if (!s->hls && st == 206 && !len && s->clen >= 0 && s->at < s->clen)
    {
      failed (j, 0, "an empty part");
      return;
    }
  if (!s->hls && st == 200 && s->at > 0)
    {
      finish (j, 0, "YouTube sent the whole stream again rather than the part asked for");
      return;
    }
  if (len && fwrite (body, 1, len, s->f) != len)
    {
      finish (j, 0, "The file couldn't be written (is the disc full?)");
      return;
    }
  j->bytes += (long long) len;
  j->tries = 0;
  if (s->hls)
    s->part++;
  else
    {
      s->at += (long long) len;
      /* the size not known: the end is a part shorter than asked for (or
         a 200: the whole of it at once) */
      if (s->clen < 0 && (st == 200 || (long long) len < save_range_size))
        s->clen = s->at;
    }
  j->done_parts++;
  if (j->progress)
    j->progress (j->done_parts, j->total_parts, (long) (j->bytes >> 10), j->ctx);
  next (j);
}

static void next (save_job *j)
{
  stream *s;
  if (j->phase == 0)
    {
      /* the HLS playlists first, so the pieces can be counted */
      while (j->cur < j->ns && !j->s[j->cur].hls)
        j->cur++;
      if (j->cur < j->ns)
        {
          if (fetch (j, j->s[j->cur].url, NULL, playlist_done))
            finish (j, 0, "Out of memory");
          return;
        }
      j->phase = 1;
      j->cur = 0;
      if (j->progress)
        j->progress (0, j->total_parts, 0, j->ctx);
    }
  while (j->cur < j->ns)
    {
      s = &j->s[j->cur];
      if (!s->f && !(s->f = fopen (s->file, "wb")))
        {
          char e[300];
          snprintf (e, sizeof e, "Couldn't create %s", s->file);
          finish (j, 0, e);
          return;
        }
      if (s->hls ? s->part < s->nparts : (s->clen < 0 || s->at < s->clen))
        break;
      fclose (s->f);
      s->f = NULL;
      j->cur++;
    }
  if (j->cur == j->ns)
    {
      finish (j, 1, NULL);
      return;
    }
  s = &j->s[j->cur];
  if (s->hls)
    {
      if (fetch (j, s->parts[s->part], NULL, part_done))
        finish (j, 0, "Out of memory");
    }
  else
    {
      char range[80];
      long long last = s->at + save_range_size - 1;
      if (s->clen >= 0 && last > s->clen - 1)
        last = s->clen - 1;
      snprintf (range, sizeof range, "Range: bytes=%lld-%lld\r\n", s->at, last);
      if (fetch (j, s->url, range, part_done))
        finish (j, 0, "Out of memory");
    }
}

save_job *save_start (const yt_play *p, const char *video_file, const char *audio_file,
                      save_progress_fn progress, save_done_fn done, void *ctx, char *err, size_t errlen)
{
  save_job *j;
  const char *urls[2];
  const char *files[2];
  if (p->live || (!p->video_url && p->hls_url))
    {
      snprintf (err, errlen, "It's a live stream: it can't be saved");
      return NULL;
    }
  if (!p->video_url)
    {
      snprintf (err, errlen, "YouTube gave no stream to save");
      return NULL;
    }
  if (!(j = calloc (1, sizeof *j)))
    {
      snprintf (err, errlen, "Out of memory");
      return NULL;
    }
  urls[0] = p->video_url;
  urls[1] = p->audio_url;
  files[0] = video_file;
  files[1] = audio_file;
  for (int i = 0; i < 2; i++)
    {
      stream *s = &j->s[j->ns];
      if (!urls[i])
        continue;
      s->url = strdup (urls[i]);
      s->file = strdup (files[i]);
      s->hls = p->hls;
      s->clen = p->hls ? -1 : yt_url_clen (urls[i]);
      j->ns++;
      if (!s->url || !s->file)
        {
          j->done = NULL;
          finish (j, 0, NULL);
          snprintf (err, errlen, "Out of memory");
          return NULL;
        }
      if (!s->hls)
        j->total_parts += s->clen > 0 ? (int) ((s->clen + save_range_size - 1) / save_range_size) : 1;
      if (s->clen > 0xFFFFFFFFLL)
        {
          j->done = NULL;
          finish (j, 0, NULL);
          snprintf (err, errlen, "The video is over 4 GB, more than a RISC OS file can hold: choose a lower Quality");
          return NULL;
        }
    }
  snprintf (j->headers, sizeof j->headers, "User-Agent: %s\r\n", yt_config_get ("media_user_agent"));
  j->progress = progress;
  j->done = done;
  j->ctx = ctx;
  j->starting = 1;
  next (j);
  j->starting = 0;
  if (j->start_failed)
    {
      snprintf (err, errlen, "%s", j->start_err);
      j->done = NULL;
      finish (j, 0, NULL);
      return NULL;
    }
  return j;
}

void save_cancel (save_job *j)
{
  if (!j)
    return;
  if (j->req)
    http_cancel (j->req);
  j->req = NULL;
  j->done = NULL;
  finish (j, 0, NULL);
}

int save_join_args (char *out, size_t size, const char *video_file, const char *audio_file, const char *mp4)
{
  int n;
  /* (No -movflags +faststart: its second pass rewrites the whole file to
     move the index to the front, which isn't needed to play a file here,
     doubles the writing, and was where a broken MP4 could come from.)
     file: in front, so that a RISC OS name (SDFS::Pi.$.x) isn't taken
     for a protocol (SDFS:) by ffmpegs without riscos-ffmpeg's patch for
     it; aac_adtstoasc named, as ffmpegs before 3.x don't add it themselves
     (HLS pieces' sound is ADTS; it passes other AAC through). Nothing here
     newer than FFmpeg 2.x. */
  if (audio_file)
    n = snprintf (out, size, "-nostdin -hide_banner -v error -y -i file:%s -i file:%s -map 0:v:0 -map 1:a:0 -c copy "
                  "-bsf:a aac_adtstoasc -f mp4 file:%s", video_file, audio_file, mp4);
  else
    n = snprintf (out, size, "-nostdin -hide_banner -v error -y -i file:%s -c copy -f mp4 file:%s",
                  video_file, mp4);
  return n < 0 || (size_t) n >= size ? -1 : 0;
}

void save_leaf_name (const char *title, char *out, size_t size, int max)
{
  char *d = text_display (title ? title : "");
  size_t o = 0;
  int sp = 0;
  if (!size)
    return;
  for (const unsigned char *p = (const unsigned char *) (d ? d : ""); *p && o + 1 < size && (int) o < max; p++)
    {
      unsigned c = *p;
      /* RISC OS: no . : * # $ & @ ^ % \\ " | < > or space (a hard space,
         &A0, is the usual stand-in; _ here, as Filer users read it) */
      if (c == ' ' || c == 0xA0)
        {
          if (o && !sp)
            out[o++] = '_', sp = 1;
          continue;
        }
      if (strchr (".:*#$&@^%\\\"|<>,;'`?!/()[]{}=+~", (int) c) || c < 32 || c == 127)
        continue;
      out[o++] = (char) c;
      sp = 0;
    }
  while (o && out[o - 1] == '_')
    o--;
  out[o] = 0;
  if (!o)
    snprintf (out, size, "video");
  free (d);
}

static unsigned long long be (const unsigned char *p, int n)
{
  unsigned long long v = 0;
  for (int i = 0; i < n; i++)
    v = v << 8 | p[i];
  return v;
}

int save_check_mp4 (const char *path, char *err, size_t errlen)
{
  FILE *f = fopen (path, "rb");
  unsigned char h[16];
  unsigned long long at = 0, end;
  int moov = 0, mdat = 0, n = 0;
  if (!f)
    {
      snprintf (err, errlen, "it isn't there");
      return -1;
    }
  {
    long e;
    if (fseek (f, 0, SEEK_END) || (e = ftell (f)) < 0)
      {
        fclose (f);
        return 0;                              /* (over 2 GB: not checked here) */
      }
    end = (unsigned long long) e;
  }
  while (at < end)
    {
      unsigned long long size;
      int hl = 8;
      if (end - at < 8 || fseek (f, (long) at, SEEK_SET) || fread (h, 1, 8, f) != 8)
        break;
      size = be (h, 4);
      if (size == 1)
        {
          if (fread (h + 8, 1, 8, f) != 8)
            break;
          size = be (h + 8, 8);
          hl = 16;
        }
      else if (size == 0)
        size = end - at;                       /* (to the end) */
      if (size < (unsigned long long) hl || size > end - at)
        break;
      if (n == 0 && memcmp (h + 4, "ftyp", 4))
        break;
      moov |= !memcmp (h + 4, "moov", 4);
      mdat |= !memcmp (h + 4, "mdat", 4);
      at += size;
      n++;
    }
  fclose (f);
  if (at != end || !n)
    snprintf (err, errlen, "it isn't a whole MP4 (it ends part way through, at byte %llu of %llu)", at, end);
  else if (!moov)
    snprintf (err, errlen, "ffmpeg didn't finish it (it has no index, the moov box)");
  else if (!mdat)
    snprintf (err, errlen, "it has no picture or sound in it (no mdat box)");
  else
    return 0;
  return -1;
}
