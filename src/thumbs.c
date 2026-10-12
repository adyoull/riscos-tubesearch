/* thumbs.c - see thumbs.h. RISC OS only (SpriteExtend, OS_SpriteOp).

   A picture becomes a sprite of its own (32bpp, 90 dpi, so 2 OS units a
   pixel: 320 x 180 pixels is 640 x 360 OS units, pixel for pixel in the
   usual desktop modes). The JPEG is drawn into it once with
   JPEG_PlotScaled, output switched to the sprite; after that redrawing is
   a PutSpriteScaled, which is quick. As in Reel, no translation table: a
   16 or 32 bit-per-pixel desktop is assumed.

   Part of TubeSearch. MIT licence. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include "thumbs.h"
#include "http.h"
#include "yt.h"

#define MAX_THUMBS 150
#define TAG_THUMB 50

#define XOS_SpriteOp     (0x2E | 0x20000)
#define XJPEG_Info       (0x49980 | 0x20000)
#define XJPEG_PlotScaled (0x49982 | 0x20000)

enum { T_FREE, T_NONE, T_LOADING, T_READY, T_FAILED };

struct thumb {
  int state;
  char *url;
  int w, h;
  int *area;                  /* sprite area with one sprite */
  unsigned stamp;
};

static thumb table[MAX_THUMBS];
static unsigned clock_stamp;
static void (*arrived_fn) (const char *url);

void thumbs_init (void (*arrived) (const char *url))
{
  arrived_fn = arrived;
}

static void forget (thumb *t)
{
  free (t->url);
  free (t->area);
  memset (t, 0, sizeof *t);
}

/* A sprite area with one w x h 32bpp sprite, black. */
static int *make_sprite (int w, int h)
{
  size_t bytes = 16 + 44 + (size_t) w * 4 * (size_t) h;
  int *a = calloc (1, bytes), *s;
  if (!a)
    return NULL;
  a[0] = (int) bytes;                /* area: size, sprites, first, free */
  a[1] = 1;
  a[2] = 16;
  a[3] = (int) bytes;
  s = a + 4;
  s[0] = (int) bytes - 16;           /* sprite: next, name, width-1 (words), height-1 */
  memcpy (&s[1], "thumb\0\0\0\0\0\0\0", 12);
  s[4] = w - 1;
  s[5] = h - 1;
  s[6] = 0;                          /* first and last bit used */
  s[7] = 31;
  s[8] = 44;                         /* image, mask (none) */
  s[9] = 44;
  s[10] = 1 | (90 << 1) | (90 << 14) | (6 << 27);   /* 32bpp, 90 x 90 dpi */
  return a;
}

/* Draws the JPEG into a new sprite of t's size. 0 if it worked. */
static int decode (thumb *t, const char *jpeg, size_t len)
{
  _kernel_swi_regs r, saved;
  _kernel_oserror *e;
  int jw, jh, scale[4];
  int *area;

  r.r[0] = 1;                        /* return the dimensions */
  r.r[1] = (int) jpeg;
  r.r[2] = (int) len;
  if (_kernel_swi (XJPEG_Info, &r, &r))
    return -1;                       /* not a JPEG SpriteExtend can draw (WebP, say) */
  jw = r.r[2];
  jh = r.r[3];
  if (jw < 1 || jh < 1)
    return -1;
  if (!(area = make_sprite (t->w, t->h)))
    return -1;

  r.r[0] = 60 + 512;                 /* output to the sprite (pointer), no save area */
  r.r[1] = (int) area;
  r.r[2] = (int) (area + 4);
  r.r[3] = 0;
  if ((e = _kernel_swi (XOS_SpriteOp, &r, &saved)))
    {
      free (area);
      return -1;
    }
  /* fill the sprite: the picture scaled to it, keeping its shape (a
     4:3 picture gets black bars at the sides) */
  {
    int sw = t->w, sh = t->h, dw, dh;
    if ((long) jw * sh > (long) jh * sw)
      {
        dw = sw;
        dh = (int) ((long) jh * sw / jw);
      }
    else
      {
        dh = sh;
        dw = (int) ((long) jw * sh / jh);
      }
    scale[0] = dw;                   /* xmul, ymul, xdiv, ydiv */
    scale[1] = dh;
    scale[2] = jw;
    scale[3] = jh;
    r.r[0] = (int) jpeg;
    r.r[1] = (sw - dw);              /* x, y in OS units (2 a pixel in the sprite): centred */
    r.r[2] = (sh - dh);
    r.r[3] = (int) scale;
    r.r[4] = (int) len;
    r.r[5] = 0;
    e = _kernel_swi (XJPEG_PlotScaled, &r, &r);
  }
  _kernel_swi (XOS_SpriteOp, &saved, &saved);   /* output back where it was */
  if (e)
    {
      free (area);
      return -1;
    }
  free (t->area);
  t->area = area;
  return 0;
}

static void fetched (http_req *r, void *ctx)
{
  thumb *t = ctx;
  size_t len;
  const char *body = http_body (r, &len);
  if (http_status (r) == 200 && len > 0 && decode (t, body, len) == 0)
    t->state = T_READY;
  else
    t->state = T_FAILED;
  if (arrived_fn && t->state == T_READY)
    arrived_fn (t->url);
}

static int start_fetch (thumb *t)
{
  char host[96], headers[400];
  const char *p = t->url, *slash;
  if (strncmp (p, "https://", 8))
    return -1;
  p += 8;
  slash = strchr (p, '/');
  if (!slash || (size_t) (slash - p) >= sizeof host)
    return -1;
  memcpy (host, p, (size_t) (slash - p));
  host[slash - p] = 0;
  snprintf (headers, sizeof headers, "User-Agent: %s\r\nAccept: image/jpeg,image/*;q=0.5\r\n",
            yt_config_get ("thumb_user_agent"));
  if (!http_get (host, slash, headers, TAG_THUMB, fetched, t))
    return -1;
  t->state = T_LOADING;
  return 0;
}

thumb *thumb_get (const char *url, int w, int h)
{
  thumb *t = NULL, *oldest = NULL;
  if (!url || !*url)
    return NULL;
  for (int i = 0; i < MAX_THUMBS; i++)
    {
      thumb *c = &table[i];
      if (c->state != T_FREE && c->w == w && c->h == h && !strcmp (c->url, url))
        {
          t = c;
          break;
        }
    }
  if (!t)
    {
      for (int i = 0; i < MAX_THUMBS && !t; i++)
        if (table[i].state == T_FREE)
          t = &table[i];
      if (!t)
        {
          /* full: drop the one shown longest ago (never one on its way) */
          for (int i = 0; i < MAX_THUMBS; i++)
            if (table[i].state != T_LOADING && (!oldest || table[i].stamp < oldest->stamp))
              oldest = &table[i];
          if (!oldest)
            return NULL;
          forget (oldest);
          t = oldest;
        }
      if (!(t->url = strdup (url)))
        return NULL;
      t->w = w;
      t->h = h;
      t->state = T_NONE;
    }
  t->stamp = ++clock_stamp;
  if (t->state == T_NONE && start_fetch (t))
    t->state = T_FAILED;
  return t;
}

int thumb_ready (const thumb *t)
{
  return !t ? -1 : t->state == T_READY ? 1 : t->state == T_FAILED ? -1 : 0;
}

void thumb_plot (thumb *t, int x, int y)
{
  _kernel_swi_regs r;
  if (!t || t->state != T_READY)
    return;
  t->stamp = ++clock_stamp;
  r.r[0] = 52 + 512;                 /* PutSpriteScaled, by pointer */
  r.r[1] = (int) t->area;
  r.r[2] = (int) (t->area + 4);
  r.r[3] = x;
  r.r[4] = y;
  r.r[5] = 0;                        /* overwrite */
  r.r[6] = 0;                        /* no scaling: its dpi says its size */
  r.r[7] = 0;                        /* no translation table (16/32bpp desktop) */
  _kernel_swi (XOS_SpriteOp, &r, &r);
}

void thumbs_cancel_pending (void)
{
  http_cancel_tag (TAG_THUMB);
  for (int i = 0; i < MAX_THUMBS; i++)
    if (table[i].state == T_LOADING)
      table[i].state = T_NONE;       /* asked for again when it's shown */
}

void thumbs_free_all (void)
{
  http_cancel_tag (TAG_THUMB);
  for (int i = 0; i < MAX_THUMBS; i++)
    if (table[i].state != T_FREE)
      forget (&table[i]);
}
