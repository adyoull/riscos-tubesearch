/* Tests ui.c (with thumbs.c, api.c, http.c, yt.c...) against riscos-
   crossdev's fake RISC OS: a fake Wimp records the windows, the toolbar's
   nesting, text and sprites drawn, menus, errors, files saved and tasks
   started; a fake transport answers with the recorded YouTube fixtures.
   It drives TubeSearch the way a user would: open the window, search, see the
   pictures arrive, play, related videos, a channel, More, Back, menus,
   resizing, a pasted address, and an unplayable video.
     test_ui FIXTURE_DIR
   Part of TubeSearch. MIT licence. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "fake.h"
#define JPEG_Info 0x49980          /* (not in UnixLib's swis.h) */
#define JPEG_PlotScaled 0x49982
#include "swis.h"
#include "../src/ui.h"
#include "../src/save.h"
#include "../src/thumbs.h"
#include "../src/http.h"
#include "../src/transport.h"
#include "../src/yt.h"

static const char *fixdir;
static char scrap[300];

/* ---- the fake Wimp ------------------------------------------------------ */

typedef struct {
  int block[22 + 8 * 8];      /* as created */
  int vis[4], sx, sy, open, parent, nest_flags, opens;
  int extent[4];
} fwin;

static fwin wins[6];                       /* handle 1: main, 2: toolbar, 3: Info, 4: Save video, 5: Saving */
static int nwins, iconbar_icons, forced, carets, extents;
static char drawn[1 << 20];                /* every string Wimp_TextOp plotted */
static size_t drawn_len;
static char report[256], started[512], saved_path[256];
static char *saved_data;
static int sprite_outputs, sprite_plots, jpeg_plots, menus_made;
static void *last_menu;
static int acks_sent;
/* the smallest MP4 save_check_mp4 takes: ftyp, moov, mdat */
#define MINI_MP4 "\0\0\0\x10" "ftypisom\0\0\0\0" "\0\0\0\x08" "moov" "\0\0\0\x0C" "mdat" "data"
static int claims, dataload_to;
static char dataload_path[256];
static int drags, morites, datasave[64], datasave_to, ptr_win = -1, ptr_icon = -1, typed_as;
static char typed_path[600];
static char last_search[8192];             /* the body of the newest search request */
static int search_requests;

static const _kernel_oserror err_bad = { 1, "Bad" };

/* The JavaScript helper (tscli solve), faked: it answers the challenges
   with the answers node gives for player 854a788e. */
enum { HELPER_ANSWER, HELPER_LATER, HELPER_NEVER };
static int helper_mode, helper_starts, quit_sent_to;
static char helper_cmd[1000], helper_cache[600];
static const char helper_answer[] =
  "{\"n\":{\"IlLiA21ny7gqA2m4p37\":\"RL7twTMlYAhbWlqMF\",\"eabGFpsUKuWHXGh6FR4\":\"OtjoLvmzT_c2ob_ei\"},"
  "\"sig\":{\"NJAJEij0EwRgIhAI0KExTgjfPk-MPM9MAdzyyPRt=BM8-XO5tm5hlMCSVpAiEAv7eP3CURqZNSPow8BXXAoazVoXgeMP7gH9BdylHCwgw=gwzz\":"
  "\"zzwg=wgwCHlydB9Hg7PMegXoVzaoAXXB8woPSNZqRUC3Pe7vAEiApVSCMlh5mt5OX-NMB=tRPyyzdAM9MPM-kPfjgTxEK0IAhIgRwE0jiEJA\"}}";

static int fake_helper (const char *cmd)
{
  char job[500], out[500];
  FILE *f;
  snprintf (helper_cmd, sizeof helper_cmd, "%s", cmd);
  helper_starts++;
  if (sscanf (strstr (cmd, ".Solve ") + 7, "%499s %499s", job, out) != 2)
    return 0;
  if (helper_mode == HELPER_ANSWER && (f = fopen (out, "w")))
    {
      fputs (helper_answer, f);
      fclose (f);
      if (helper_cache[0] && (f = fopen (helper_cache, "w")))   /* the prepared player it keeps */
        {
          fputs ("prepared", f);
          fclose (f);
        }
    }
  return 77;                                         /* its task handle */
}

static const _kernel_oserror *wimp (int swi, int r[10])
{
  switch (swi)
    {
    case Wimp_Initialise: r[1] = 0x1234; return NULL;
    case Wimp_CreateIcon: iconbar_icons++; r[0] = 0; return NULL;
    case Wimp_CreateWindow:
      {
        fwin *w = &wins[++nwins];
        const int *b = (const int *) r[1];
        int n = 22 + 8 * b[21];
        memcpy (w->block, b, (size_t) n * sizeof (int));
        memcpy (w->vis, b, sizeof w->vis);
        memcpy (w->extent, &b[10], sizeof w->extent);
        r[0] = nwins;
        return NULL;
      }
    case Wimp_OpenWindow:
      {
        int *b = (int *) r[1];
        fwin *w = &wins[b[0]];
        memcpy (w->vis, &b[1], sizeof w->vis);
        w->sx = b[5];
        w->sy = b[6];
        /* the Wimp keeps the scroll within the extent */
        if (w->sy < w->extent[1] + (w->vis[3] - w->vis[1]))
          w->sy = w->extent[1] + (w->vis[3] - w->vis[1]);
        if (w->sy > 0)
          w->sy = 0;
        b[6] = w->sy;
        w->open = 1;
        w->opens++;
        if (r[2] == 0x4B534154)
          {
            w->parent = r[3];
            w->nest_flags = r[4];
          }
        return NULL;
      }
    case Wimp_GetWindowState:
      {
        int *b = (int *) r[1];
        fwin *w = &wins[b[0]];
        memcpy (&b[1], w->vis, sizeof w->vis);
        b[5] = w->sx;
        b[6] = w->sy;
        b[7] = -1;
        b[8] = w->block[7] | (w->open ? 1 << 16 : 0);
        return NULL;
      }
    case Wimp_CloseWindow: wins[((int *) r[1])[0]].open = 0; return NULL;
    case Wimp_SetExtent:
      memcpy (wins[r[0]].extent, (int *) r[1], 4 * sizeof (int));
      extents++;
      return NULL;
    case Wimp_ForceRedraw: forced++; return NULL;
    case Wimp_SetIconState: return NULL;
    case Wimp_GetIconState:
      {
        int *b = (int *) r[1];
        memcpy (&b[2], &wins[b[0]].block[22 + 8 * b[1]], 8 * sizeof (int));
        return NULL;
      }
    case Wimp_SetCaretPosition: carets++; return NULL;
    case Wimp_ProcessKey: return NULL;
    case Wimp_CloseDown: return NULL;
    case Wimp_RedrawWindow:
      {
        int *b = (int *) r[1];
        fwin *w = &wins[b[0]];
        memcpy (&b[1], w->vis, sizeof w->vis);
        b[5] = w->sx;
        b[6] = w->sy;
        memcpy (&b[7], w->vis, sizeof w->vis);     /* all of it */
        r[0] = 1;
        return NULL;
      }
    case Wimp_GetRectangle: r[0] = 0; return NULL;
    case Wimp_TextOp:
      if (r[0] == 1)
        {
          const char *t = (const char *) r[1];
          int n = 0;
          while ((unsigned char) t[n] >= 32 && (r[2] == 0 || n < r[2]))
            n++;
          r[0] = 16 * n;
        }
      else if (r[0] == 2)
        {
          size_t l = strlen ((const char *) r[1]);
          if (drawn_len + l + 2 < sizeof drawn)
            {
              memcpy (drawn + drawn_len, (const char *) r[1], l);
              drawn_len += l;
              drawn[drawn_len++] = '\n';
              drawn[drawn_len] = 0;
            }
        }
      return NULL;
    case Wimp_CreateMenu: menus_made++; last_menu = (void *) r[1]; return NULL;
    case Wimp_SpriteOp:                              /* 40: is there such a sprite? (only file_bf4) */
      return r[0] == 40 && !strcmp ((const char *) r[2], "file_bf4") ? NULL : &err_bad;
    case Wimp_DragBox: drags++; return NULL;
    case Wimp_ResizeIcon:                            /* the icon's new box, kept */
      memcpy (&wins[r[0]].block[22 + 8 * r[1]], &r[2], 4 * sizeof (int));
      return NULL;
    case Wimp_GetPointerInfo:
      ((int *) r[1])[0] = 500;
      ((int *) r[1])[1] = 400;
      ((int *) r[1])[2] = 0;
      ((int *) r[1])[3] = ptr_win;
      ((int *) r[1])[4] = ptr_icon;
      return NULL;
    case Wimp_ReportError:
      snprintf (report, sizeof report, "%s", ((const _kernel_oserror *) r[0])->errmess);
      r[1] = 1;
      return NULL;
    case Wimp_StartTask:
      if (strstr ((const char *) r[0], ".Solve "))
        r[0] = fake_helper ((const char *) r[0]);
      else
        {
          snprintf (started, sizeof started, "%s", (const char *) r[0]);
          r[0] = 0;
        }
      return NULL;
    case Wimp_SendMessage:
      if (r[0] == 17 && ((int *) r[1])[4] == 0)
        quit_sent_to = r[2];
      if (r[0] == 17 && ((int *) r[1])[4] == 4)
        acks_sent++;                                 /* Message_DataLoadAck */
      if (((int *) r[1])[4] == 1)                    /* Message_DataSave: kept, with a reference */
        {
          memcpy (datasave, (int *) r[1], sizeof datasave);
          datasave_to = r[2];
          ((int *) r[1])[2] = datasave[2] = 0x5151;
        }
      if (((int *) r[1])[4] == 0x808C4)              /* Message_TaskWindow_Morite */
        morites++;
      if (((int *) r[1])[4] == 15 && r[2] == 0)      /* Message_ClaimEntity, to everyone */
        claims = ((int *) r[1])[5];
      if (((int *) r[1])[4] == 3 && r[0] == 18)      /* Message_DataLoad (recorded) */
        {
          dataload_to = r[2];
          snprintf (dataload_path, sizeof dataload_path, "%s", (const char *) &((int *) r[1])[11]);
        }
      return NULL;
    }
  return &err_bad;
}

static const _kernel_oserror *os (int swi, int r[10])
{
  switch (swi)
    {
    case OS_ReadModeVariable:
      r[2] = r[1] == 4 || r[1] == 5 ? 1 : r[1] == 11 ? 1919 : r[1] == 12 ? 1079 : 0;
      return NULL;
    case OS_Byte: r[1] = r[1] == 255 ? 1 : 101; return NULL;   /* a UK keyboard; Latin-1 */
    case Territory_Number: r[0] = 6; return NULL;    /* France */
    case 0x50B00:                                    /* MimeMap_Translate: extension mp4 -> &BF4 */
      if (r[0] != 3 || r[2] != 0 || strcmp ((const char *) r[1], "mp4"))
        return &err_bad;
      r[3] = 0xBF4;
      return NULL;
    case OS_FSControl:                               /* 25: rename */
      if (r[0] != 25 || rename ((const char *) r[1], (const char *) r[2]))
        return &err_bad;
      return NULL;
    case 0x42400: drags++; return NULL;              /* DragASprite_Start */
    case 0x42401: return NULL;                       /* DragASprite_Stop */
    case OS_Plot: return NULL;
    case ColourTrans_SetGCOL: return NULL;
    case OS_File:
      if (r[0] == 8)                                 /* create a directory */
        {
          mkdir ((const char *) r[1], 0777);
          return NULL;
        }
      if (r[0] == 17)                                /* read catalogue: 0 none, 1 file, 2 directory */
        {
          struct stat st;
          r[0] = stat ((const char *) r[1], &st) ? 0 : S_ISDIR (st.st_mode) ? 2 : 1;
          r[4] = r[0] ? (int) st.st_size : 0;
          return NULL;
        }
      if (r[0] == 6)                                 /* delete */
        {
          unlink ((const char *) r[1]);
          return NULL;
        }
      if (r[0] == 18)                                /* set the type */
        {
          snprintf (typed_path, sizeof typed_path, "%s", (const char *) r[1]);
          typed_as = r[2];
          return NULL;
        }
      if (r[0] != 10)
        return &err_bad;
      snprintf (saved_path, sizeof saved_path, "%s", (const char *) r[1]);
      free (saved_data);
      saved_data = strndup ((const char *) r[4], (size_t) (r[5] - r[4]));
      return NULL;
    case OS_SpriteOp:
      if ((r[0] & 0xFF) == 60)
        {
          if (r[1])
            sprite_outputs++;                        /* to a sprite (not back to the screen) */
          r[0] = 60;                                 /* the old output: the screen */
          r[1] = r[2] = r[3] = 0;
          return NULL;
        }
      if ((r[0] & 0xFF) == 52)
        {
          const int *area = (const int *) r[1];
          const int *spr = (const int *) r[2];
          if (spr != area + 4 || spr[10] != (1 | (90 << 1) | (90 << 14) | (6 << 27)))
            return &err_bad;
          sprite_plots++;
          return NULL;
        }
      return &err_bad;
    case JPEG_Info:
      {
        const unsigned char *j = (const unsigned char *) r[1];
        if (r[2] < 4 || j[0] != 0xFF || j[1] != 0xD8)
          return &err_bad;
        r[2] = 320;
        r[3] = 180;
        return NULL;
      }
    case JPEG_PlotScaled: jpeg_plots++; return NULL;
    }
  return &err_bad;
}

static void install (void)
{
  static const int w[] = { Wimp_SendMessage, Wimp_Initialise, Wimp_CreateIcon, Wimp_CreateWindow, Wimp_OpenWindow,
    Wimp_GetWindowState, Wimp_CloseWindow, Wimp_SetExtent, Wimp_ForceRedraw, Wimp_SetIconState, Wimp_GetIconState,
    Wimp_SetCaretPosition, Wimp_ProcessKey, Wimp_CloseDown, Wimp_RedrawWindow, Wimp_GetRectangle,
    Wimp_TextOp, Wimp_CreateMenu, Wimp_GetPointerInfo, Wimp_ReportError, Wimp_StartTask, Wimp_SpriteOp,
    Wimp_DragBox, Wimp_ResizeIcon, 0 };
  static const int o[] = { OS_ReadModeVariable, OS_Byte, OS_Plot, ColourTrans_SetGCOL, OS_File,
    OS_SpriteOp, JPEG_Info, JPEG_PlotScaled, Territory_Number, 0x50B00, 0x42400, 0x42401, OS_FSControl, 0 };
  for (int i = 0; w[i]; i++)
    fake_swi (w[i], NULL, wimp);
  for (int i = 0; o[i]; i++)
    fake_swi (o[i], NULL, os);
}

/* ---- the fake network: recorded answers ----------------------------------- */

typedef struct { char rx[65536]; size_t rxlen; char *tx; size_t txlen, txpos; int requests; } fconn;
static int api_requests, picture_requests, player_requests, flaky_requests, gone_mq, gone_hq;
static unsigned net_clock;

static char *load (const char *name, size_t *len)
{
  char path[512];
  FILE *f;
  char *b;
  long n;
  snprintf (path, sizeof path, "%s/%s", fixdir, name);
  if (!(f = fopen (path, "rb")))
    {
      fprintf (stderr, "can't open %s (run tests/fetch-fixtures.sh)\n", path);
      exit (1);
    }
  fseek (f, 0, SEEK_END);
  n = ftell (f);
  fseek (f, 0, SEEK_SET);
  b = malloc ((size_t) n + 1);
  *len = fread (b, 1, (size_t) n, f);
  fclose (f);
  return b;
}

static int iframe_requests, playerjs_requests, tv_requests, manifest_requests, hls_requests, hls_broken;
static int feed_requests, playlist_requests, piece_requests;

static void answer (fconn *c, const char *req)
{
  char *body;
  size_t len;
  char head[128];
  int code = 200;
  const char *json = strstr (req, "\r\n\r\n") + 4;
  if (!strncmp (req, "GET /iframe_api ", 16))
    {
      iframe_requests++;
      body = load ("iframe_api.js", &len);
    }
  else if (!strncmp (req, "GET /s/player/854a788e/player_ias.vflset/en_US/base.js ", 55))
    {
      playerjs_requests++;
      body = load ("player_854a788e.js", &len);
    }
  else if (!strncmp (req, "GET /feeds/videos.xml?channel_id=", 33))
    {
      /* a channel's feed: tests/subs/feed_<id>.xml, or 404 (then its page) */
      char name[80];
      FILE *t;
      feed_requests++;
      snprintf (name, sizeof name, "../subs/feed_%.24s.xml", req + 33);
      snprintf (head, sizeof head, "%s/%s", fixdir, name);
      if ((t = fopen (head, "rb")))
        {
          fclose (t);
          body = load (name, &len);
        }
      else
        {
          body = strdup ("<html>404</html>");
          len = strlen (body);
          code = 404;
        }
    }
  else if (strstr (req, "Host: raw.githubusercontent.com"))
    {
      manifest_requests++;                           /* (the daily solver check: not here) */
      body = strdup ("404: Not Found");
      len = strlen (body);
      code = 404;
    }
  else if (strstr (req, "Host: manifest.googlevideo.com") && strstr (req, "/hls_playlist/"))
    {
      /* one stream's media playlist (saving): three pieces each */
      const char *kind = strstr (req, "/itag/233/") || strstr (req, "/itag/234/") ? "a" : "v";
      char pl[600];
      snprintf (pl, sizeof pl, "#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXTINF:2.0,\nhttps://rr1---sn-fake.googlevideo.com/seg/%s/0\n"
                "#EXTINF:2.0,\n/seg/%s/1\n#EXTINF:2.0,\nhttps://rr1---sn-fake.googlevideo.com/seg/%s/2\n#EXT-X-ENDLIST\n",
                kind, kind, kind);
      playlist_requests++;
      body = strdup (pl);
      len = strlen (body);
    }
  else if (!strncmp (req, "GET /seg/", 9))
    {
      /* a piece: its own name, 1000 times */
      char name[16];
      size_t n = strcspn (req + 9, " ");
      snprintf (name, sizeof name, "%.*s|", (int) (n < 10 ? n : 10), req + 9);
      piece_requests++;
      len = strlen (name) * 1000;
      body = malloc (len + 1);
      for (size_t i = 0; i < 1000; i++)
        memcpy (body + i * strlen (name), name, strlen (name));
      body[len] = 0;
    }
  else if (strstr (req, "Host: manifest.googlevideo.com"))
    {
      hls_requests++;                                /* YouTube's HLS master playlist */
      if (hls_broken)
        {
          body = strdup ("<html>gone</html>");
          len = strlen (body);
        }
      else
        body = load ("../hls/master.m3u8", &len);
    }
  else if (!strncmp (req, "GET ", 4))
    {
      picture_requests++;
      if (strstr (req, "/vi/flaky/") && flaky_requests++ == 0)
        {
          body = strdup ("busy");                   /* (once) */
          code = 503;
        }
      else if (strstr (req, "/vi/gone/mqdefault.jpg"))
        {
          gone_mq++;
          body = strdup ("not here");
          code = 404;
        }
      else
        {
          if (strstr (req, "/vi/gone/hqdefault.jpg"))
            gone_hq++;
          body = strdup ("\xFF\xD8\xFF\xE0 a JPEG of sorts");
        }
      len = strlen (body);
    }
  else
    {
      api_requests++;
      if (strstr (req, "/visitor_id"))
        body = load ("visitor.json", &len);
      else if (strstr (req, "/search") && (search_requests++, snprintf (last_search, sizeof last_search, "%s", json)))
        body = load (strstr (json, "\"continuation\"") ? "search_2.json" : "search_1.json", &len);
      else if (strstr (req, "/next"))
        body = load ("next.json", &len);
      else if (strstr (req, "/browse"))
        body = load (strstr (json, "\"continuation\"") ? "channel_2.json" : "channel_1.json", &len);
      else if (strstr (req, "/player") && strstr (json, "\"TVHTML5\""))
        {
          tv_requests++;
          body = load ("player_tv.json", &len);
        }
      else if (strstr (req, "/player") && strstr (json, "BotCheck001"))
        {
          player_requests++;
          body = strdup ("{\"playabilityStatus\":{\"status\":\"LOGIN_REQUIRED\",\"reason\":\"Sign in to confirm you\\u2019re not a bot\"}}");
          len = strlen (body);
        }
      else if (strstr (req, "/player"))
        {
          player_requests++;
          if (strstr (json, "BADBADBADBA"))
            {
              body = strdup ("{\"playabilityStatus\":{\"status\":\"ERROR\",\"reason\":\"Video unavailable\"}}");
              len = strlen (body);
            }
          else
            body = load ("player_visionos.json", &len);
        }
      else
        {
          body = strdup ("{}");
          len = 2;
        }
    }
  snprintf (head, sizeof head, "HTTP/1.1 %d %s\r\nContent-Length: %lu\r\n\r\n", code, code == 200 ? "OK" : "Not Found",
            (unsigned long) len);
  c->tx = realloc (c->tx, c->txlen + strlen (head) + len);
  memcpy (c->tx + c->txlen, head, strlen (head));
  memcpy (c->tx + c->txlen + strlen (head), body, len);
  c->txlen += strlen (head) + len;
  free (body);
}

static void *n_open (const char *host, int port, int tls, char *err, size_t errlen)
{
  return calloc (1, sizeof (fconn));
}
static int n_ready (void *p, char *err, size_t errlen) { return 1; }
static int n_send (void *p, const char *b, int len)
{
  fconn *c = p;
  memcpy (c->rx + c->rxlen, b, (size_t) len);
  c->rxlen += (size_t) len;
  c->rx[c->rxlen] = 0;
  {
    char *end = strstr (c->rx, "\r\n\r\n");
    const char *cl = strstr (c->rx, "Content-Length: ");
    size_t need = end ? (size_t) (end + 4 - c->rx) + (cl && cl < end ? (size_t) atol (cl + 16) : 0) : 0;
    if (end && c->rxlen >= need)
      {
        answer (c, c->rx);
        memmove (c->rx, c->rx + need, c->rxlen - need);
        c->rxlen -= need;
      }
  }
  return len;
}
static int n_recv (void *p, char *b, int len)
{
  fconn *c = p;
  int n = (int) (c->txlen - c->txpos);
  if (!n)
    return HTTP_AGAIN;
  if (n > len)
    n = len;
  memcpy (b, c->tx + c->txpos, (size_t) n);
  c->txpos += (size_t) n;
  return n;
}
static void n_close (void *p) { fconn *c = p; free (c->tx); free (c); }
static unsigned n_now (void) { return net_clock; }

const http_transport transport_system = { n_open, n_ready, n_recv, n_send, n_close, n_now };
int transport_check (char *err, size_t errlen) { return 0; }
void transport_wait (int cs) { }

/* ---- driving it --------------------------------------------------------- */

static int block[64];

static void pump (void)
{
  int quit = 0;
  for (int i = 0; i < 10000 && ui_busy (); i++)
    {
      net_clock++;
      quit |= ui_event (0, block);
    }
  CHECK (!quit, "a null event doesn't quit");
}

static void redraw (void)
{
  drawn_len = 0;
  drawn[0] = 0;
  block[0] = 1;
  ui_event (1, block);
}

static void click (int win, int icon, int x, int y, int buttons)
{
  block[0] = x; block[1] = y; block[2] = buttons; block[3] = win; block[4] = icon;
  ui_event (6, block);
}

/* A click on tile i at (dx, dy) from its top left, in the work area. */
static void menu_open_none (void)
{
  /* (a menu left open: the next one replaces it, as the Wimp does) */
}

static void click_tile (int i, int dx, int dy, int buttons)
{
  fwin *w = &wins[1];
  int cols = (w->vis[2] - w->vis[0] - 32) / 680;
  int x0 = 16 + (i % cols) * 680, y1 = -(84 + 16 + (i / cols) * 560);
  click (1, -1, w->vis[0] - w->sx + x0 + dx, w->vis[3] - w->sy + y1 - dy, buttons);
}

static const char *title (void)  { return (const char *) wins[1].block[18]; }
static char *field (void)         { return (char *) wins[2].block[22 + 8 + 5]; }
static const char *status (void) { return (const char *) wins[2].block[22 + 24 + 5]; }

static void type_and_search (const char *text)
{
  strcpy (field (), text);
  block[0] = 2; block[1] = 1; block[6] = 13;
  ui_event (8, block);
}

int main (int argc, char **argv)
{
  fixdir = argc > 1 ? argv[1] : "tests/fixtures";
  fake_reset ();
  install ();
  /* RISC OS names over a Linux directory: "<tmp>/scrap.TubeSearch..." */
  {
    static char tmp[] = "/tmp/test_ui.XXXXXX";
    char p[600];
    FILE *f;
    if (!mkdtemp (tmp))
      return 1;
    snprintf (scrap, sizeof scrap, "%s/scrap", tmp);
    setenv ("Wimp$ScrapDir", scrap, 1);
    snprintf (p, sizeof p, "%s/choices", tmp);
    setenv ("Choices$Write", p, 1);
    snprintf (p, sizeof p, "%s/ts", tmp);
    setenv ("TubeSearch$Dir", p, 1);
    /* the bundled solver scripts (the fake helper doesn't run them) */
    snprintf (p, sizeof p, "%s/ts.Resources.ejs.Version", tmp);
    if ((f = fopen (p, "w"))) { fputs ("0.8.0\n", f); fclose (f); }
    snprintf (p, sizeof p, "%s/ts.Resources.ejs.lib", tmp);
    if ((f = fopen (p, "w"))) { fputs ("var lib = {};", f); fclose (f); }
    snprintf (p, sizeof p, "%s/ts.Resources.ejs.core", tmp);
    if ((f = fopen (p, "w"))) { fputs ("var jsc;", f); fclose (f); }
    snprintf (helper_cache, sizeof helper_cache, "%s.TubeSearch.Players.854a788e-0-8-0", scrap);
  }
  setenv ("Alias$Reel", "/<Reel$Dir> %*0", 1);

  CHECK (ui_init () == 0, "starts");
  CHECK (!strcmp (yt_config_get ("language"), "fr") && !strcmp (yt_config_get ("region"), "FR"),
         "the language and country from the territory (France)");
  yt_config_set ("language", "en-GB");         /* (the rest expects the defaults) */
  yt_config_set ("region", "GB");
  yt_config_set ("stream_format", "dash");     /* (HLS has its own part, near the end) */
  CHECK (iconbar_icons == 1 && nwins == 5, "an icon bar icon and five windows (main, toolbar, Info, Save video, Saving)");
  /* the Info box: four labels and four values; Version says which */
  {
    const int *b = wins[3].block;
    CHECK (b[21] == 8, "the Info box has eight icons");
    CHECK (!strcmp ((const char *) b[18], "About this program"), "its title");
    CHECK (!strcmp ((const char *) b[22 + 16 * 3 + 5], "Version"), "a Version label");
    CHECK (!strcmp ((const char *) b[30 + 16 * 3 + 5], TUBE_VERSION " (" TUBE_DATE ")"), "the version and date");
    CHECK (!strcmp ((const char *) b[30 + 5], "TubeSearch"), "the name");
  }
  CHECK (!strcmp (title (), "TubeSearch"), "the window's title");

  /* Menu on the icon bar icon: Info leads to the Info box */
  click (-2, 0, 100, 20, 2);
  {
    const int *m = (const int *) last_menu;          /* title 3 words, colours, width, height, gap; items of 6 words */
    CHECK (m && !strcmp ((const char *) &m[7 + 3], "Info") && m[7 + 1] == 3, "Info's submenu is the Info box");
    CHECK (m && !strcmp ((const char *) &m[7 + 7 * 6 + 3], "Quit") && (m[7 + 7 * 6] & 0x80), "Quit is last");
    CHECK (m && !strcmp ((const char *) &m[7 + 6 * 6 + 3], "Sound"), "Sound before it");
    CHECK (m && !strcmp ((const char *) &m[7 + 5 * 6 + 3], "Frame rate"), "Frame rate before that");
    CHECK (m && (m[7 + 2 * 6 + 2] & 0x100) && !strcmp ((const char *) m[7 + 2 * 6 + 3], "Subscriptions"), "Subscriptions");
    menus_made = 0;
  }

  /* the icon bar icon opens the window, with the toolbar nested inside its top */
  click (-2, 0, 100, 20, 4);
  CHECK (wins[1].open && wins[2].open, "both windows open");
  CHECK (wins[2].parent == 1, "the toolbar is a child of the window");
  CHECK (wins[2].nest_flags == ((1 << 16) | (2 << 18) | (2 << 20) | (2 << 22) | (1 << 24) | (2 << 26)),
         "the toolbar is fixed to the top edge");
  CHECK (wins[2].vis[3] == wins[1].vis[3] && wins[2].vis[1] == wins[1].vis[3] - 84, "the toolbar's place");
  CHECK (carets == 1, "the caret goes in the search box");
  redraw ();
  CHECK (strstr (drawn, "Type what you'd like to watch") != NULL, "an empty window says what to do");

  /* search */
  type_and_search ("test");
  CHECK (strstr (status (), "Searching") != NULL, "searching...");
  pump ();
  CHECK (api_requests == 2, "the visitor ID, then the search");
  CHECK (!strncmp (status (), "24 videos", 9), "the status says how many");
  CHECK (!strcmp (title (), "TubeSearch: test"), "the title says what was searched for");
  CHECK (wins[1].extent[1] <= -(84 + 16 + 9 * 560 + 16), "the extent holds 9 rows (24 + More, 3 across)");
  redraw ();
  CHECK (strstr (drawn, "50 QUESTIONS pour TESTER TON SAVOIR |\nQuiz Culture G\xE9n\xE9rale\n") != NULL,
         "the first title, wrapped at a word (16 OS units a letter here)");
  CHECK (strstr (drawn, "\nDevine le LOGO") != NULL, "no space before a title");
  CHECK (strstr (drawn, "Hero Quiz\n") != NULL, "a channel name");
  CHECK (strstr (drawn, "14:22\n") != NULL, "a duration");
  CHECK (strstr (drawn, "2.3m views \xB7 2 years ago\n") != NULL, "views and age (bullet as Latin-1 middle dot)");
  pump ();
  CHECK (picture_requests == 24, "a picture for every result");
  CHECK (jpeg_plots == 24 && sprite_outputs == 24, "each drawn into a sprite");
  sprite_plots = 0;
  redraw ();
  CHECK (sprite_plots >= 9, "the pictures on screen plotted");
  {
    /* scrolled to the end: the More tile */
    int b[9];
    b[0] = 1;
    memcpy (&b[1], wins[1].vis, 4 * sizeof (int));
    b[5] = 0; b[6] = -100000; b[7] = -1;
    ui_event (2, b);
    redraw ();
    CHECK (strstr (drawn, "More\n") != NULL, "the More tile at the end");
    b[6] = 0;
    ui_event (2, b);
  }

  /* Select plays: the streams are found and Reel is started with them */
  click_tile (0, 100, 100, 4);
  pump ();
  CHECK (player_requests == 1, "the player asked for the streams");
  {
    char want[400];
    snprintf (want, sizeof want, "%s.TubeSearchPlay1", scrap);
    CHECK (!strcmp (saved_path, want), "Reel's file in the scrap directory");
    snprintf (want, sizeof want, "Reel %s.TubeSearchPlay1", scrap);
    CHECK (!strcmp (started, want), "Reel started with it");
  }
  CHECK (saved_data && strstr (saved_data, "\"requested_formats\"") && strstr (saved_data, "itag=137")
         && strstr (saved_data, "itag=140"), "the 1080p video and the AAC sound");
  CHECK (strstr (status (), "Playing 1080p") != NULL, "the status says it's playing");

  /* Adjust: related videos */
  click_tile (0, 100, 100, 1);
  pump ();
  CHECK (!strcmp (title (), "TubeSearch: Related to Marzia & Felix - Wedding 19.08.2019"), "the related page's title");
  CHECK (!strncmp (status (), "20 videos", 9), "20 related videos");

  /* the channel's name on a tile: that channel */
  click_tile (0, 100, 490, 4);
  pump ();
  CHECK (!strcmp (title (), "TubeSearch: Creative Commons"), "the channel page");
  CHECK (!strncmp (status (), "30 videos", 9), "30 of its videos");

  /* More (the 31st tile) */
  click_tile (30, 100, 100, 4);
  CHECK (strstr (status (), "Loading more") != NULL, "loading more...");
  pump ();
  CHECK (!strncmp (status (), "60 videos", 9), "60 after More");

  /* Back, Back: the search results again, without asking again */
  {
    int before = api_requests;
    click (2, 0, 0, 0, 4);
    CHECK (!strcmp (title (), "TubeSearch: Related to Marzia & Felix - Wedding 19.08.2019"), "Back: related");
    click (2, 0, 0, 0, 4);
    CHECK (!strcmp (title (), "TubeSearch: test"), "Back again: the search");
    CHECK (!strncmp (status (), "24 videos", 9), "its status back");
    CHECK (api_requests == before, "nothing fetched again");
    click (2, 0, 0, 0, 4);
    CHECK (!strcmp (title (), "TubeSearch: test"), "Back at the first page does nothing");
  }

  /* Menu on a tile: Play */
  click_tile (3, 100, 100, 2);
  CHECK (menus_made == 1 && last_menu != NULL, "the tile menu opens");
  {
    int sel[4] = { 0, -1, -1, -1 };
    int before = player_requests;
    CHECK (ui_event (9, sel) == 0, "choosing Play");
    pump ();
    CHECK (player_requests == before + 1 && strstr (started, "TubeSearchPlay2"), "Play from the menu");
  }
  /* Menu: Quality > 720p, then play again */
  click_tile (3, 100, 100, 2);
  {
    int sel[4] = { 8, 2, -1, -1 };              /* Play, Related, Channel, Save video, Copy, Subscribe, Back, Refresh, Quality > 720p */
    ui_event (9, sel);
    click_tile (3, 100, 100, 4);
    pump ();
    CHECK (saved_data && strstr (saved_data, "itag=136") && !strstr (saved_data, "itag=137"), "720p after choosing it");
  }
  /* Refresh (the toolbar button, F5 and the menu): the page fetched again */
  {
    int before = api_requests;
    const int *bar = wins[2].block;
    CHECK (bar[21] == 8 && !strcmp ((const char *) bar[22 + 8 * 6 + 5], "Refresh"), "a Refresh button");
    click (2, 6, 0, 0, 4);
    CHECK (strstr (status (), "Refreshing") != NULL, "refreshing...");
    pump ();
    CHECK (api_requests == before + 1 && !strncmp (status (), "24 videos", 9), "the search again, the same 24");
    redraw ();
    CHECK (strstr (drawn, "Hero Quiz\n") != NULL, "drawn again");
    block[0] = 2; block[1] = 1; block[6] = 0x185;
    ui_event (8, block);
    pump ();
    CHECK (api_requests == before + 2, "F5 refreshes too");
    click_tile (3, 100, 100, 2);
    {
      const int *m = (const int *) last_menu;
      int sel[4] = { 7, -1, -1, -1 };          /* Play, Related, Channel, Save video, Copy, Subscribe, Back, Refresh */
      CHECK (m && !strcmp ((const char *) &m[7 + 7 * 6 + 3], "Refresh"), "Refresh on the menu");
      ui_event (9, sel);
      pump ();
      CHECK (api_requests == before + 3, "and from the menu");
    }
  }

  /* the choices are kept: written to Choices:TubeSearch.Choices, and read
     back (as at the next start) over Config's */
  {
    char path[600], text[600] = "";
    FILE *f;
    int sel[4] = { 9, 1, -1, -1 };              /* ... Quality, Frame rate > 60 a second */
    size_t n;
    click_tile (3, 100, 100, 2);
    ui_event (9, sel);
    CHECK (!strcmp (yt_config_get ("max_fps"), "60"), "Frame rate > 60 a second");
    snprintf (path, sizeof path, "%s.TubeSearch.Choices", getenv ("Choices$Write"));
    if ((f = fopen (path, "r")))
      {
        n = fread (text, 1, sizeof text - 1, f);
        text[n] = 0;
        fclose (f);
      }
    CHECK (strstr (text, "max_quality = 720\n") && strstr (text, "max_fps = 60\n") && strstr (text, "audio_language = mine\n")
           && strstr (text, "languages = \n") && strstr (text, "search_date = any\n"), "the choices file");
    yt_config_set ("max_quality", "1080");
    yt_config_set ("max_fps", "30");
    CHECK (yt_config_load (path) == 6 && !strcmp (yt_config_get ("max_quality"), "720")
           && !strcmp (yt_config_get ("max_fps"), "60"), "read back at the next start");
    click_tile (3, 100, 100, 2);
    sel[1] = 0;                                  /* Frame rate > 30 a second */
    ui_event (9, sel);
    CHECK (!strcmp (yt_config_get ("max_fps"), "30"), "and back to 30");
  }

  /* a narrower window: two across, a taller extent */
  {
    int b[9];
    int ext = wins[1].extent[1];
    b[0] = 1;
    b[1] = wins[1].vis[0]; b[2] = wins[1].vis[1]; b[3] = wins[1].vis[0] + 1500; b[4] = wins[1].vis[3];
    b[5] = 0; b[6] = 0; b[7] = -1;
    ui_event (2, b);
    CHECK (wins[1].extent[1] < ext, "fewer across: more rows");
    CHECK (wins[2].vis[2] == wins[1].vis[2], "the toolbar follows");
  }

  /* a pasted address: related videos, and that one plays */
  type_and_search ("https://www.youtube.com/watch?v=7PIMiDcwNvc&t=10s");
  pump ();
  CHECK (!strcmp (title (), "TubeSearch: Related to Marzia & Felix - Wedding 19.08.2019"), "an address: related");
  CHECK (saved_data && strstr (saved_data, "Marzia & Felix"), "and it plays");

  /* a video YouTube won't play */
  type_and_search ("youtu.be/BADBADBADBA");
  pump ();
  CHECK (strstr (report, "Video unavailable") != NULL, "the reason is reported");

  CHECK (iframe_requests == 0 && tv_requests == 0, "an unavailable video doesn't try the TV app");
  CHECK (manifest_requests == 1, "the solver manifest is checked once at start-up");

  /* ---- the TV fallback: YouTube refuses the player client ("not a bot") ---- */
  helper_mode = HELPER_ANSWER;
  started[0] = 0;
  type_and_search ("youtu.be/BotCheck001");
  pump ();
  CHECK (iframe_requests == 1 && playerjs_requests == 1 && tv_requests == 1, "the player's ID, its code, the TV answer");
  CHECK (helper_starts == 1 && strstr (helper_cmd, "/ts.Solve ") && strstr (helper_cmd, ".TubeSearchJob")
         && strstr (helper_cmd, ".TubeSearchAns"), "the helper started through the Solve Obey file");
  CHECK (saved_data && strstr (saved_data, "n=RL7twTMlYAhbWlqMF") && strstr (saved_data, "&sig=zzwg%3Dwgw")
         && strstr (saved_data, "n=OtjoLvmzT_c2ob_ei"), "Reel gets the answered addresses");
  CHECK (started[0] && strstr (status (), "Playing"), "and plays them");
  CHECK (ui_busy (), "busy until the helper has gone");
  {
    int msg[6] = { 24, 77, 0, 0, 0x400C3, 0 };     /* Message_TaskCloseDown from the helper */
    ui_event (17, msg);
  }
  CHECK (!ui_busy (), "then idle");
  {
    char p[600];
    struct stat st;
    snprintf (p, sizeof p, "%s.TubeSearch.TubeSearchJob1", scrap);
    CHECK (stat (p, &st) != 0, "the job file is removed");
  }
  /* again: the player is prepared (the helper keeps it), so no player code */
  type_and_search ("youtu.be/BotCheck001");
  pump ();
  CHECK (playerjs_requests == 1 && tv_requests == 2 && helper_starts == 2, "the prepared player saves fetching the code");
  {
    int msg[6] = { 24, 77, 0, 0, 0x400C3, 0 };
    ui_event (17, msg);
  }

  /* a solve given up: another video chosen while the helper works */
  helper_mode = HELPER_LATER;
  type_and_search ("youtu.be/BotCheck001");
  for (int i = 0; i < 2000; i++)
    ui_event (0, block);
  CHECK (strstr (status (), "Answering YouTube's challenge") != NULL, "the status says what's happening");
  started[0] = 0;
  quit_sent_to = 0;
  type_and_search ("youtu.be/7PIMiDcwNvc");
  CHECK (quit_sent_to == 77, "the helper is told to stop");
  for (int i = 0; i < 2000; i++)
    ui_event (0, block);
  CHECK (started[0] != 0, "the new video plays (the player client gave addresses)");
  {
    int msg[6] = { 24, 77, 0, 0, 0x400C3, 0 };
    ui_event (17, msg);
  }
  CHECK (!ui_busy (), "and the helper's end is noticed");

  /* a helper that ends without answering */
  helper_mode = HELPER_NEVER;
  report[0] = 0;
  type_and_search ("youtu.be/BotCheck001");
  for (int i = 0; i < 2000; i++)
    ui_event (0, block);
  {
    int msg[6] = { 24, 77, 0, 0, 0x400C3, 0 };
    ui_event (17, msg);
  }
  pump ();
  CHECK (strstr (report, "JavaScript helper gave no answer") != NULL, "reported");

  /* TV only, with the solver scripts gone: the play ends at once, said so,
     and nothing is left pointing at it */
  {
    char p[600], q[600];
    snprintf (p, sizeof p, "%s.Resources.ejs.Version", getenv ("TubeSearch$Dir"));
    snprintf (q, sizeof q, "%s.Resources.ejs.Version-away", getenv ("TubeSearch$Dir"));
    rename (p, q);
    yt_config_set ("stream_client", "tv");
    report[0] = 0;
    type_and_search ("youtu.be/7PIMiDcwNvc");
    CHECK (strstr (status (), "scripts are missing") && strstr (report, "scripts are missing"),
           "the reason stays on the status line");
    type_and_search ("youtu.be/7PIMiDcwNvc");    /* (a second play cancels nothing stale) */
    pump ();
    yt_config_set ("stream_client", "auto");
    rename (q, p);
  }

  /* the upload-date filter: its pop-up menu, a search done again with it,
     Back to the one before, and later searches keeping it */
  {
    const int *bar = wins[2].block;
    const char *date_icon = (const char *) bar[22 + 8 * 4 + 5];
    const int *m;
    int sel[4] = { 2, -1, -1, -1 };              /* Any time, Today, This week */
    int before;
    CHECK (bar[21] == 8, "the toolbar has the date filter's two icons (and Refresh, Subscriptions)");
    CHECK (!strcmp (date_icon, "Any time"), "it starts at Any time");
    CHECK (bar[22 + 8 * 5 + 4] & 2 && strstr ((const char *) bar[22 + 8 * 5 + 6], "Sgright"),
           "a pop-up menu button");
    type_and_search ("test");
    pump ();
    CHECK (!strstr (last_search, "\"params\""), "Any time: no filter sent");
    menus_made = 0;
    click (2, 5, 0, 0, 4);
    m = (const int *) last_menu;
    CHECK (menus_made == 1 && m && !strcmp ((const char *) m, "Uploaded"), "the Uploaded menu opens");
    CHECK (m && !strcmp ((const char *) &m[7 + 2 * 6 + 3], "This week") && (m[7] & 1) && !(m[7 + 2 * 6] & 1),
           "Any time is ticked");
    CHECK (m && !strcmp ((const char *) &m[7 + 4 * 6 + 3], "This year") && (m[7 + 4 * 6] & 0x80), "five choices");
    before = search_requests;
    ui_event (9, sel);
    pump ();
    CHECK (search_requests == before + 1 && strstr (last_search, "\"params\":\"EgIIAw==\""),
           "the search again, for this week");
    CHECK (strstr (last_search, "\"query\":\"test\"") != NULL, "the same words");
    {
      /* words typed in the desktop's alphabet go to YouTube as UTF-8 */
      int before2 = search_requests;
      type_and_search ("caf\xE9");
      pump ();
      CHECK (search_requests == before2 + 1 && strstr (last_search, "\"query\":\"caf\xC3\xA9\""), "caf\xE9 sent as UTF-8");
      CHECK (!strncmp (title (), "TubeSearch: caf\xE9", 16), "and shown as typed");
      click (2, 0, 0, 0, 4);                     /* Back */
    }
    CHECK (!strcmp (date_icon, "This week"), "the toolbar shows it");
    CHECK (!strcmp (title (), "TubeSearch: test (This week)"), "and the title");
    click (2, 4, 0, 0, 4);                       /* the display field opens the menu too */
    m = (const int *) last_menu;
    CHECK (m && (m[7 + 2 * 6] & 1) && !(m[7] & 1), "This week is ticked now");
    before = search_requests;
    click (2, 0, 0, 0, 4);
    CHECK (!strcmp (title (), "TubeSearch: test"), "Back: the unfiltered search");
    CHECK (search_requests == before, "not asked again");
    CHECK (!strcmp (date_icon, "Any time"), "and the toolbar shows its filter");
    click (2, 5, 0, 0, 4);
    ui_event (9, sel);                           /* This week again */
    pump ();
    CHECK (search_requests == before + 1 && strstr (last_search, "EgIIAw=="), "This week again");
    type_and_search ("cats");
    pump ();
    CHECK (strstr (last_search, "\"query\":\"cats\"") && strstr (last_search, "EgIIAw=="),
           "a new search keeps the filter");
    click_tile (24, 100, 100, 4);                /* More: the continuation carries the filter */
    pump ();
    CHECK (strstr (last_search, "\"continuation\"") && !strstr (last_search, "\"params\""),
           "More sends YouTube's continuation, not the filter again");
    sel[0] = 4;
    click (2, 5, 0, 0, 4);
    ui_event (9, sel);
    pump ();
    CHECK (strstr (last_search, "EgIIBQ==") && !strcmp (date_icon, "This year"), "This year");
    sel[0] = 0;
    click (2, 5, 0, 0, 4);
    ui_event (9, sel);
    pump ();
    CHECK (!strstr (last_search, "\"params\"") && !strcmp (date_icon, "Any time"), "back to Any time");
    /* a related page isn't searched again when the filter changes */
    click_tile (0, 100, 100, 1);
    pump ();
    before = search_requests;
    sel[0] = 3;
    click (2, 5, 0, 0, 4);
    ui_event (9, sel);
    pump ();
    CHECK (search_requests == before && !strcmp (date_icon, "This month"), "related: just the setting");
    sel[0] = 0;
    click (2, 5, 0, 0, 4);
    ui_event (9, sel);
  }

  /* HLS (the default): the master playlist, then the 720p H.264 variant
     (30 a second) and its default sound, given to Reel like DASH's pair */
  {
    const char *expect_q = "720p";
    yt_config_set ("stream_format", "hls");
    hls_requests = 0;
    started[0] = 0;
    click_tile (1, 100, 100, 4);
    pump ();
    CHECK (hls_requests == 1, "HLS: the master playlist fetched");
    CHECK (saved_data && strstr (saved_data, "/itag/232/") && strstr (saved_data, "itag%3D140;xtags%3Dacont%253Doriginal%253Alang%253Den-US")
           && !strstr (saved_data, "dubbed") && strstr (saved_data, "\"protocol\":\"m3u8_native\"")
           && !strstr (saved_data, "itag=136"), "HLS: the 720p variant and the default sound");
    CHECK (strstr (status (), expect_q) && strstr (status (), "sound: English (original)") && started[0], "HLS: playing");
    /* German as my language: the German dub; then Sound > Original */
    yt_config_set ("language", "de-DE");
    click_tile (1, 100, 100, 4);
    pump ();
    CHECK (saved_data && strstr (saved_data, "dubbed-auto%253Alang%253Dde-DE") && strstr (status (), "sound: German (dubbed)"),
           "HLS: my language's dub");
    click_tile (1, 100, 100, 2);
    {
      const int *m = (const int *) last_menu;
      int sel[4] = { 10, 1, -1, -1 };            /* Play, Related, Channel, Save video, Copy, Subscribe, Back, Refresh, Quality, Frame rate, Sound > Original */
      CHECK (m && !strcmp ((const char *) &m[7 + 10 * 6 + 3], "Sound") && (m[7 + 10 * 6] & 0x80), "the Sound menu, last");
      ui_event (9, sel);
    }
    CHECK (!strcmp (yt_config_get ("audio_language"), "original"), "Sound > Original chosen");
    {
      /* Sound > Language > French; Sound > I also understand > English (and off again) */
      const int *m, *sm, *lm, *um;
      int fr = -1, en = -1;
      for (int i = 0; yt_lang_menu (i); i++)
        {
          if (!strcmp (yt_lang_menu (i), "fr"))
            fr = i;
          if (!strcmp (yt_lang_menu (i), "en"))
            en = i;
        }
      click_tile (1, 100, 100, 2);
      m = (const int *) last_menu;
      sm = m ? (const int *) m[7 + 10 * 6 + 1] : NULL;
      CHECK (sm && !strcmp ((const char *) &sm[7 + 2 * 6 + 3], "Language") && (sm[7 + 1 * 6] & 2)
             && (sm[7 + 3 * 6] & 0x80) && !strcmp ((const char *) sm[7 + 3 * 6 + 3], "I also understand"),
             "Sound: My language, Original, Language, I also understand");
      lm = sm ? (const int *) sm[7 + 2 * 6 + 1] : NULL;
      um = sm ? (const int *) sm[7 + 3 * 6 + 1] : NULL;
      CHECK (lm && um && fr >= 0 && !strncmp ((const char *) &lm[7 + fr * 6 + 3], "French", 6)
             && !(lm[7 + fr * 6] & 1) && !(um[7 + en * 6] & 1), "the languages, none ticked");
      {
        int sel[4] = { 10, 2, fr, -1 };
        ui_event (9, sel);
      }
      CHECK (!strcmp (yt_config_get ("audio_language"), "fr"), "Sound > Language > French");
      click_tile (1, 100, 100, 2);
      m = (const int *) last_menu;
      sm = (const int *) m[7 + 10 * 6 + 1];
      lm = (const int *) sm[7 + 2 * 6 + 1];
      CHECK ((sm[7 + 2 * 6] & 1) && (lm[7 + fr * 6] & 1) && !(sm[7] & 1), "French ticked (and Language)");
      {
        int sel[4] = { 10, 3, en, -1 };
        ui_event (9, sel);
      }
      CHECK (!strcmp (yt_config_get ("languages"), "en"), "I also understand > English");
      click_tile (1, 100, 100, 2);
      m = (const int *) last_menu;
      sm = (const int *) m[7 + 10 * 6 + 1];
      um = (const int *) sm[7 + 3 * 6 + 1];
      CHECK (um[7 + en * 6] & 1, "English ticked");
      {
        int sel[4] = { 10, 3, en, -1 };
        ui_event (9, sel);
      }
      CHECK (!strcmp (yt_config_get ("languages"), ""), "and off again");
      /* Sound > My language > French: the language and country YouTube is told; then Automatic */
      click_tile (1, 100, 100, 2);
      m = (const int *) last_menu;
      sm = (const int *) m[7 + 10 * 6 + 1];
      {
        const int *mm = (const int *) sm[7 + 1];
        CHECK (mm && !strcmp ((const char *) &mm[7 + 3], "Automatic") && (mm[7] & 1) && (mm[7] & 2)
               && !strncmp ((const char *) &mm[7 + (fr + 1) * 6 + 3], "French", 6), "My language: Automatic (ticked), then the languages");
        int sel[4] = { 10, 0, fr + 1, -1 };
        ui_event (9, sel);
      }
      CHECK (!strcmp (yt_config_get ("language"), "fr") && !strcmp (yt_config_get ("region"), "FR")
             && !strcmp (yt_config_get ("audio_language"), "mine") && !strcmp (yt_config_get ("my_language"), "fr"),
             "My language > French");
      {
        char path[600], text[600] = "";
        FILE *f;
        snprintf (path, sizeof path, "%s.TubeSearch.Choices", getenv ("Choices$Write"));
        if ((f = fopen (path, "r")))
          {
            text[fread (text, 1, sizeof text - 1, f)] = 0;
            fclose (f);
          }
        CHECK (strstr (text, "my_language = fr\n") != NULL, "kept in Choices");
      }
      click_tile (1, 100, 100, 2);
      m = (const int *) last_menu;
      sm = (const int *) m[7 + 10 * 6 + 1];
      CHECK (!strcmp ((const char *) sm[7 + 3], "My language (French)"), "and named");
      {
        int sel[4] = { 10, 0, 0, -1 };             /* Automatic */
        ui_event (9, sel);
      }
      CHECK (!strcmp (yt_config_get ("language"), "fr") && !strcmp (yt_config_get ("region"), "FR")
             && !strcmp (yt_config_get ("my_language"), ""), "Automatic: back to what the start found (France)");
      yt_config_set ("language", "de-DE");         /* (as this part had it) */
      yt_config_set ("region", "GB");
      yt_config_set ("audio_language", "original");
    }
    click_tile (1, 100, 100, 2);
    {
      const int *m = (const int *) last_menu, *sm = m ? (const int *) m[7 + 10 * 6 + 1] : NULL;
      CHECK (sm && (sm[7 + 2] & 0x100) && !strcmp ((const char *) sm[7 + 3], "My language (German)"),
             "Sound's first item names my language");
    }
    click_tile (1, 100, 100, 4);
    pump ();
    CHECK (saved_data && strstr (saved_data, "acont%253Doriginal%253Alang%253Den-US") && !strstr (saved_data, "dubbed"),
           "HLS: the original, though my language is German");
    yt_config_set ("audio_language", "mine");
    yt_config_set ("language", "en-GB");
    /* a master playlist that isn't one: the DASH streams instead */
    hls_broken = 1;
    hls_requests = 0;
    started[0] = 0;
    click_tile (1, 100, 100, 4);
    pump ();
    CHECK (hls_requests == 1 && saved_data && strstr (saved_data, "itag=136") && !strstr (saved_data, "m3u8_native")
           && started[0], "HLS unusable: DASH");
    hls_broken = 0;
  }

  /* subscriptions: none yet; a Takeout list dropped on the window; the
     feeds (one channel's gone: its page instead); Unsubscribe; Subscribe */
  {
    int sel[4] = { 2, -1, -1, -1 };              /* icon bar menu > Subscriptions */
    char path[600], text[2000] = "";
    FILE *f;
    size_t n;
    click (-2, 0, 100, 20, 2);
    ui_event (9, sel);
    CHECK (!strcmp (title (), "TubeSearch: Subscriptions") && !strcmp (status (), "No subscriptions yet"), "Subscriptions: none yet");
    redraw ();
    CHECK (strstr (drawn, "Google Takeout") != NULL, "it says how to add some");
    {
      int msg[64];
      memset (msg, 0, sizeof msg);
      msg[0] = 256; msg[1] = 99; msg[2] = 1234; msg[4] = 3;       /* Message_DataLoad from task 99 */
      msg[5] = 1; msg[6] = -1; msg[10] = 0xDFE;
      snprintf ((char *) &msg[11], 200, "%s/../subs/takeout_subscriptions.csv", fixdir);
      acks_sent = 0;
      ui_event (17, msg);
      CHECK (acks_sent == 1, "the drop is acknowledged");
      CHECK (!strcmp (title (), "TubeSearch: My channels") && strstr (status (), "3 channels added from the 3"),
             "3 channels brought in, shown");
      redraw ();
      CHECK (strstr (drawn, "Joueur du Grenier\n") && strstr (drawn, "Gone Channel\n"), "the channels' tiles");
      msg[2] = 1235; msg[3] = 0; msg[4] = 3;     /* (the ack was written over it) */
      snprintf ((char *) &msg[11], 200, "%s/../hls/master.m3u8", fixdir);
      report[0] = 0;
      acks_sent = 0;
      ui_event (17, msg);
      CHECK (acks_sent == 0 && strstr (report, "list of subscriptions"), "another file: refused, not taken");
    }
    snprintf (path, sizeof path, "%s.TubeSearch.Subscriptions", getenv ("Choices$Write"));
    if ((f = fopen (path, "r")))
      {
        n = fread (text, 1, sizeof text - 1, f);
        text[n] = 0;
        fclose (f);
      }
    CHECK (strstr (text, "UCaaaaaaaaaaaaaaaaaaaaaa\tJoueur du Grenier\t") && strstr (text, "seen 0\n"), "kept in Choices");
    /* the toolbar's Subscriptions: the three channels' feeds */
    feed_requests = 0;
    {
      int before = api_requests;
      click (2, 7, 0, 0, 4);
      CHECK (strstr (status (), "Reading the channels") != NULL, "reading...");
      redraw ();
      CHECK (!strstr (drawn, "Les jeux d'horreur & moi"), "nothing shown till they're all read (and sorted)");
      pump ();
      CHECK (feed_requests == 3 && api_requests == before + 1, "three feeds, and the gone one's page");
    }
    CHECK (!strcmp (title (), "TubeSearch: Subscriptions") && strstr (status (), "videos from 3 channels"), "the subscriptions' videos");
    CHECK (atoi (status ()) == 5 + 30, "5 from the feeds, 30 from the page");
    redraw ();
    CHECK (strstr (drawn, "Les jeux d'horreur & moi") != NULL, "the feeds' videos shown");
    if ((f = fopen (path, "r")))
      {
        n = fread (text, 1, sizeof text - 1, f);
        text[n] = 0;
        fclose (f);
      }
    CHECK (!strstr (text, "seen 0\n") && strstr (text, "seen "), "the time they were seen, kept");
    /* left while reading, then Back: read again, not stuck */
    click (2, 7, 0, 0, 4);
    type_and_search ("test");
    pump ();
    click (2, 0, 0, 0, 4);
    CHECK (!strcmp (title (), "TubeSearch: Subscriptions") && strstr (status (), "Reading the channels"), "Back to it: reading again");
    pump ();
    CHECK (strstr (status (), "videos from 3 channels") != NULL, "and read");
    CHECK (!strstr (status (), " new"), "the ones seen before aren't new now");
    /* My channels (Adjust on the button) > Unsubscribe the first */
    click (2, 7, 0, 0, 1);
    CHECK (!strcmp (title (), "TubeSearch: My channels") && !strcmp (status (), "3 channels"), "My channels");
    click_tile (0, 100, 100, 2);
    {
      const int *m = (const int *) last_menu;
      int us[4] = { 1, -1, -1, -1 };              /* Open, Unsubscribe */
      CHECK (m && !strcmp ((const char *) &m[7 + 6 + 3], "Unsubscribe"), "a subscribed channel: Unsubscribe");
      ui_event (9, us);
    }
    CHECK (!strcmp (status (), "2 channels"), "unsubscribed: 2 left");
    if ((f = fopen (path, "r")))
      {
        n = fread (text, 1, sizeof text - 1, f);
        text[n] = 0;
        fclose (f);
      }
    CHECK (!strstr (text, "UCaaaaaaaaaaaaaaaaaaaaaa") && strstr (text, "UCbbbbbbbbbbbbbbbbbbbbbb"), "and in the file");
    /* Subscribe from a video's menu (a search result's channel) */
    type_and_search ("test");
    pump ();
    click_tile (0, 100, 100, 2);
    {
      const int *m = (const int *) last_menu;
      int sub[4] = { 5, -1, -1, -1 };             /* Play, Related, Channel, Save video, Copy, Subscribe */
      CHECK (m && !strcmp ((const char *) &m[7 + 5 * 6 + 3], "Subscribe"), "a video's channel: Subscribe");
      ui_event (9, sub);
    }
    CHECK (strstr (status (), "Subscribed to ") && strstr (status (), "(3 channels)"), "subscribed");
    click_tile (0, 100, 100, 2);
    {
      const int *m = (const int *) last_menu;
      CHECK (m && !strcmp ((const char *) &m[7 + 5 * 6 + 3], "Unsubscribe"), "now Unsubscribe");
    }
  }

  /* Save video...: the save box (a drag, or a full name), the streams
     fetched beside the MP4, then !FFmpeg's ffmpeg in a TaskWindow joins them */
  {
    char out[600], v[620], a[620], obey[600];
    static char text[16000];
    const int *m, *box;
    FILE *f;
    size_t n;
    int msg[64];
    mkdir (scrap, 0777);                          /* (where the saved files go here) */
    type_and_search ("test");
    pump ();
    click_tile (0, 100, 100, 2);
    m = (const int *) last_menu;
    CHECK (m && (m[7 + 3 * 6 + 2] & 0x100) && !strcmp ((const char *) m[7 + 3 * 6 + 3], "Save video...") && m[7 + 3 * 6 + 1] == 4,
           "Save video... after Channel, leading to the save box");
    box = wins[4].block;
    CHECK (!strcmp ((const char *) box[18], "Save video") && strstr ((const char *) box[22 + 8 + 5], "/mp4"),
           "its name: the title, made a RISC OS name");
    snprintf (text, sizeof text, "%s", (const char *) box[22 + 8 + 5]);
    CHECK (!strchr (text, ' ') && !strchr (text, '.') && !strchr (text, ':') && strlen (text) > 4, "(no spaces, dots or colons)");
    {
      char leaf[64];
      save_leaf_name ("A: B. c/d \xC3\xA9t\xC3\xA9 *#$&@^%|<>\"", leaf, sizeof leaf, 40);
      CHECK (!strcmp (leaf, "A_B_cd_\xE9t\xE9"), "names: RISC OS's special letters out, spaces as _, accents kept");
      save_leaf_name ("...", leaf, sizeof leaf, 40);
      CHECK (!strcmp (leaf, "video"), "nothing left: video");
      save_leaf_name ("abcdefghijklmnopqrstuvwxyz", leaf, sizeof leaf, 10);
      CHECK (!strcmp (leaf, "abcdefghij"), "at most the length asked for");
    }
    CHECK (!strcmp ((const char *) box[22 + 5], "file_bf4"), "the file icon: MimeMap's MP4 type's sprite");
    /* without !FFmpeg: said so */
    unsetenv ("Alias$ffmpeg");
    report[0] = 0;
    snprintf ((char *) box[22 + 8 + 5], 256, "%.240s/x_mp4", scrap);
    {
      int k[7] = { 4, 1, 0, 0, -1, 0, 13 };
      ui_event (8, k);
    }
    CHECK (strstr (report, "needs ffmpeg") != NULL, "no ffmpeg: said so, nothing fetched");
    /* another port's ffmpeg, on Run$Path (no alias): fine */
    {
      FILE *rf = fopen ("Run:ffmpeg", "w");
      if (rf)
        fclose (rf);
      report[0] = 0;
      snprintf ((char *) box[22 + 8 + 5], 256, "%.240s/y_mp4", scrap);
      {
        int k[7] = { 4, 1, 0, 0, -1, 0, 13 };
        ui_event (8, k);
      }
      CHECK (!strstr (report, "needs ffmpeg") && strstr (status (), "finding the streams"), "an ffmpeg on Run$Path (another port's): fine");
      {
        int stop[4] = { 3, -1, -1, -1 };
        click_tile (0, 100, 100, 2);
        ui_event (9, stop);
      }
      unlink ("Run:ffmpeg");
    }
    setenv ("Alias$ffmpeg", "WimpSlot -min 44032K|MRun <FFmpeg$Dir>.ffmpeg %*0", 1);
    /* a bare name: drag it, or type a full one */
    report[0] = 0;
    snprintf ((char *) box[22 + 8 + 5], 256, "justaname");
    {
      int c[5] = { 0, 0, 4 /* Select */, 4, 2 };
      ui_event (6, c);
    }
    CHECK (strstr (report, "drag the file icon") != NULL, "a bare name: drag it or type a full one");
    snprintf ((char *) box[22 + 8 + 5], 256, "My_video/mp4");
    /* dragged to a Filer window (window 99): Message_DataSave there, with the leaf and the type */
    drags = 0;
    {
      int c[5] = { 0, 0, 64, 4, 0 };
      ui_event (6, c);
    }
    CHECK (drags == 1, "dragging the file icon");
    ptr_win = 99;
    ptr_icon = 0;
    datasave[0] = 0;
    ui_event (7, msg);
    CHECK (datasave_to == 99 && datasave[4] == 1 && datasave[5] == 99 && datasave[10] == 0xBF4
           && !strcmp ((const char *) &datasave[11], "My_video/mp4"), "dropped: DataSave to the Filer, leaf and type");
    /* the Filer's answer: the full name. Then the streams, then the join */
    snprintf (out, sizeof out, "%s/saved_mp4", scrap);
    if ((f = fopen (out, "wb")))
      {
        fputs ("an older file of that name", f);
        fclose (f);
      }
    memset (msg, 0, sizeof msg);
    msg[1] = 99; msg[3] = 0x5151; msg[4] = 2; msg[9] = 1000;
    snprintf ((char *) &msg[11], 200, "%.190s", out);
    playlist_requests = piece_requests = 0;
    started[0] = 0;
    ui_event (17, msg);
    CHECK (strstr (status (), "finding the streams") != NULL, "saving: finding the streams");
    CHECK (wins[5].open && !strcmp ((const char *) wins[5].block[22 + 5], (const char *) wins[5].block[22 + 5])
           && !strcmp ((const char *) wins[5].block[22 + 8 * 3 + 5], "Finding the streams..."),
           "the Saving window opens: Finding the streams...");
    CHECK (wins[5].block[22 + 8 * 2 + 2] == wins[5].block[22 + 8 * 2 + 0], "its bar empty");
    click_tile (0, 100, 100, 2);
    m = (const int *) last_menu;
    CHECK (m && !strcmp ((const char *) &m[7 + 3 * 6 + 3], "Stop saving"), "the menu says Stop saving meanwhile");
    {
      int pq[6] = { 24, 0x99, 0x777, 0, 8, 0 };
      report[0] = 0;
      ui_event (18, pq);
      CHECK (strstr (report, "being saved") && pq[3] == 0x777, "quitting meanwhile: objected to (PreQuit acknowledged), said why");
    }
    menu_open_none ();
    pump ();
    snprintf (v, sizeof v, "%s_v", out);
    snprintf (a, sizeof a, "%s_a", out);
    CHECK (playlist_requests == 2 && piece_requests == 6, "both playlists, then their six pieces");
    if ((f = fopen (v, "rb")))
      {
        text[fread (text, 1, sizeof text - 1, f)] = 0;
        fclose (f);
      }
    CHECK (!strncmp (text, "v/0|v/0|", 8) && strstr (text, "v/0|v/1|") && strstr (text, "v/1|v/2|") && strlen (text) == 3 * 4000,
           "the picture's pieces, in order, in <name>_v");
    if ((f = fopen (a, "rb")))
      {
        text[fread (text, 1, sizeof text - 1, f)] = 0;
        fclose (f);
      }
    CHECK (!strncmp (text, "a/0|", 4) && strlen (text) == 3 * 4000, "the sound's in <name>_a");
    CHECK (strstr (started, "TaskWindow \"Obey ") && strstr (started, "-task &1234") && strstr (started, "-txt &54535631")
           && strstr (started, "-quit"), "then the join, in a TaskWindow that reports to us");
    CHECK (strstr (status (), "joining") != NULL, "status: joining");
    {
      const int *bar = &wins[5].block[22 + 8 * 2], *back = &wins[5].block[22 + 8 * 1];
      int full = back[2] - back[0] - 8, got = bar[2] - bar[0];
      CHECK (wins[5].open && got * 100 >= full * 89 && got * 100 <= full * 91, "the bar at 90% when the pieces are in");
      CHECK (strstr (started, "") && strstr ((const char *) wins[5].block[22 + 8 * 3 + 5], "Joining"), "and Joining...");
    }
    snprintf (obey, sizeof obey, "%s", strstr (started, "Obey ") + 5);
    *strchr (obey, '"') = 0;
    /* (written with OS_File 10, as the file for Reel is: the fake keeps it) */
    CHECK (!strcmp (saved_path, obey) && strstr (obey, ".TubeSearchJoin"), "the Obey file beside Reel's, in the scrap directory");
    snprintf (text, sizeof text, "%s", saved_data ? saved_data : "");
    (void) n;
    CHECK (strstr (text, "\nffmpeg -progress pipe:1 -nostats -nostdin") && strstr (text, v) && strstr (text, a) && strstr (text, "-i file:") && strstr (text, "-c copy -bsf:a aac_adtstoasc -f mp4")
           && strstr (text, "Echo TubeSearch-rc <Sys$ReturnCode>"), "the Obey file: ffmpeg joining them, then its return code");
    CHECK (strstr (text, "-f mp4 file:") && strstr (text, "saved_mp4_t\n") && !strstr (text, "faststart"),
           "ffmpeg writes <name>_t (in one pass: no faststart)");
    /* the TaskWindow: Ego, the output, then Morio; ffmpeg made <name>_t */
    {
      char t[620];
      snprintf (t, sizeof t, "%s_t", out);
      if ((f = fopen (t, "wb")))
        {
          fwrite (MINI_MP4, 1, sizeof MINI_MP4 - 1, f);
          fclose (f);
        }
      CHECK (!access (out, F_OK), "(the old file of that name still there meanwhile)");
    }
    memset (msg, 0, sizeof msg);
    msg[1] = 55; msg[4] = 0x808C2; msg[5] = 0x54535631;
    ui_event (17, msg);
    {
      /* ffmpeg's -progress lines (split across messages) move the bar; they aren't kept as output */
      const char *p1 = "frame=1\nout_time_ms=", *p2 = "60000000\nprogress=continue\n";   /* a minute in */
      const int *bar = &wins[5].block[22 + 8 * 2], *back = &wins[5].block[22 + 8 * 1];
      msg[4] = 0x808C1;
      msg[5] = (int) strlen (p1);
      memcpy (&msg[6], p1, strlen (p1));
      ui_event (17, msg);
      msg[5] = (int) strlen (p2);
      memcpy (&msg[6], p2, strlen (p2));
      ui_event (17, msg);
      CHECK (bar[2] - bar[0] > (back[2] - back[0] - 8) * 90 / 100 && strstr ((const char *) wins[5].block[22 + 8 * 3 + 5], "%"),
             "ffmpeg's progress moves the bar past 90%");
    }
    {
      /* what ffmpeg 5.1's -progress really prints, many times over (as on
         the Pi: "stream_0_0_q=-1.0" was once taken for an error, and the
         rc lost behind it) */
      static const char block[] = "frame=0\nfps=0.00\nstream_0_0_q=-1.0\nbitrate=N/A\ntotal_size=48\n"
                                  "out_time_us=0\nout_time_ms=0\nout_time=00:00:00.000000\ndup_frames=0\n"
                                  "drop_frames=0\nspeed=N/A\nprogress=continue\n";
      msg[4] = 0x808C1;
      for (int k = 0; k < 20; k++)
        for (size_t at = 0; at < sizeof block - 1; at += 100)
          {
            size_t n = sizeof block - 1 - at < 100 ? sizeof block - 1 - at : 100;
            msg[5] = (int) n;
            memcpy (&msg[6], block + at, n);
            ui_event (17, msg);
          }
    }
    msg[4] = 0x808C1; msg[5] = 16;
    memcpy (&msg[6], "TubeSearch-rc 0\n", 16);
    ui_event (17, msg);
    msg[4] = 0x808C3;
    ui_event (17, msg);
    CHECK (strstr (status (), "Saved ") && strstr (status (), "saved_mp4"), "Saved, said so");
    CHECK (strstr (typed_path, "saved_mp4_t") && typed_as == 0xBF4, "typed as MP4");
    CHECK (!wins[5].open, "the Saving window closes when it's saved");
    text[0] = 0;
    if ((f = fopen (out, "rb")))
      {
        text[fread (text, 1, sizeof text - 1, f)] = 0;
        fclose (f);
      }
    CHECK (access (v, F_OK) && access (a, F_OK) && !memcmp (text, MINI_MP4, sizeof MINI_MP4 - 1),
           "the pieces deleted; the MP4 took the name (over the old file)");
    click_tile (0, 100, 100, 2);
    m = (const int *) last_menu;
    CHECK (m && !strcmp ((const char *) m[7 + 3 * 6 + 3], "Save video..."), "and Save video... again");
    menu_open_none ();
    /* rc 0, but an MP4 ffmpeg didn't finish (no moov): not taken */
    {
      char o5[600], t5[620];
      snprintf (o5, sizeof o5, "%s/nomoov_mp4", scrap);
      snprintf ((char *) box[22 + 8 + 5], 256, "%.250s", o5);
      report[0] = 0;
      {
        int c[5] = { 0, 0, 4, 4, 2 };
        ui_event (6, c);
      }
      pump ();
      snprintf (t5, sizeof t5, "%s_t", o5);
      if ((f = fopen (t5, "wb")))
        {
          fwrite ("\0\0\0\x10" "ftypisom\0\0\0\0" "\0\0\x10\0" "mdat", 1, 24, f);   /* mdat cut short */
          fclose (f);
        }
      memset (msg, 0, sizeof msg);
      msg[1] = 59; msg[4] = 0x808C2; msg[5] = 0x54535631;
      ui_event (17, msg);
      msg[4] = 0x808C1; msg[5] = 16;
      memcpy (&msg[6], "TubeSearch-rc 0\n", 16);
      ui_event (17, msg);
      msg[4] = 0x808C3;
      ui_event (17, msg);
      CHECK (strstr (report, "isn't right") && strstr (report, "isn't a whole MP4") && access (o5, F_OK) && access (t5, F_OK),
             "rc 0 but an MP4 cut short: not taken, said why, nothing left");
    }
    /* ffmpeg failing: said so (its words), nothing left behind */
    {
      char out2[600];
      snprintf (out2, sizeof out2, "%s/fails_mp4", scrap);
      if ((f = fopen (out2, "wb")))
        {
          fputs ("keep me", f);
          fclose (f);
        }
      snprintf ((char *) box[22 + 8 + 5], 256, "%.250s", out2);
      report[0] = 0;
      {
        int c[5] = { 0, 0, 4 /* Select */, 4, 2 };
        ui_event (6, c);
      }
      pump ();
      memset (msg, 0, sizeof msg);
      msg[1] = 56; msg[4] = 0x808C2; msg[5] = 0x54535631;
      ui_event (17, msg);
      msg[4] = 0x808C1; msg[5] = 45;
      memcpy (&msg[6], "Could not write header for output\nTubeSearch-rc 1\n", 45);
      msg[5] = (int) strlen ("Could not write header for output\nTubeSearch-rc 1\n");
      memcpy (&msg[6], "Could not write header for output\nTubeSearch-rc 1\n", (size_t) msg[5]);
      ui_event (17, msg);
      msg[4] = 0x808C3;
      ui_event (17, msg);
      snprintf (v, sizeof v, "%s_v", out2);
      CHECK (strstr (report, "couldn't join") && strstr (report, "Could not write header"), "ffmpeg failing: said so, in its words");
      CHECK (access (v, F_OK) && !access (out2, F_OK), "and nothing left behind (the file already there kept)");
      unlink (out2);
    }
    /* Stop saving, while the pieces are being fetched */
    {
      char out3[600];
      int stop[4] = { 3, -1, -1, -1 };
      snprintf (out3, sizeof out3, "%s/stopped_mp4", scrap);
      snprintf ((char *) box[22 + 8 + 5], 256, "%.250s", out3);
      {
        int c[5] = { 0, 0, 4 /* Select */, 4, 2 };
        ui_event (6, c);
      }
      for (int i = 0; i < 30 && !strstr (status (), "pieces"); i++)
        ui_event (0, msg);
      click_tile (0, 100, 100, 2);
      ui_event (9, stop);
      pump ();
      snprintf (v, sizeof v, "%s_v", out3);
      CHECK (strstr (status (), "Stopped saving") && access (v, F_OK) && access (out3, F_OK), "Stop saving: stopped, nothing left");
    }
    /* stopping the join: Morite to the TaskWindow */
    {
      char out4[600];
      int stop[4] = { 3, -1, -1, -1 };
      snprintf (out4, sizeof out4, "%s/stopjoin_mp4", scrap);
      snprintf ((char *) box[22 + 8 + 5], 256, "%.250s", out4);
      {
        int c[5] = { 0, 0, 4 /* Select */, 4, 2 };
        ui_event (6, c);
      }
      pump ();
      memset (msg, 0, sizeof msg);
      msg[1] = 57; msg[4] = 0x808C2; msg[5] = 0x54535631;
      ui_event (17, msg);
      morites = 0;
      {
        int c[5] = { 0, 0, 4, 5, 4 };             /* the Saving window's Stop */
        ui_event (6, c);
      }
      (void) stop;
      CHECK (morites == 1, "Stop (the Saving window's) while joining: Morite to the TaskWindow");
      ui_event (17, msg);                       /* (a second Ego: nothing more) */
      msg[4] = 0x808C3;
      ui_event (17, msg);
      snprintf (v, sizeof v, "%s_v", out4);
      CHECK (strstr (status (), "Stopped saving") && access (v, F_OK), "then stopped, nothing left");
    }
  }

  /* Stop before the TaskWindow has said who it is: Morite once it does */
  {
    char o[600], v[620];
    int msg[64], stop[4] = { 3, -1, -1, -1 };
    const int *box = wins[4].block;
    snprintf (o, sizeof o, "%s/early_mp4", scrap);
    snprintf ((char *) box[22 + 8 + 5], 256, "%.250s", o);
    {
      int c[5] = { 0, 0, 4, 4, 2 };
      ui_event (6, c);
    }
    pump ();
    morites = 0;
    click_tile (0, 100, 100, 2);
    ui_event (9, stop);
    CHECK (morites == 0 && strstr (status (), "stopping"), "Stop before Ego: stopping...");
    memset (msg, 0, sizeof msg);
    msg[1] = 58; msg[4] = 0x808C2; msg[5] = 0x54535631;
    ui_event (17, msg);
    CHECK (morites == 1, "then Morite as soon as it says who it is");
    msg[4] = 0x400C3;                           /* TaskCloseDown (as from the Task Manager) */
    ui_event (17, msg);
    snprintf (v, sizeof v, "%s_v", o);
    CHECK (strstr (status (), "Stopped saving") && access (v, F_OK), "and its end (TaskCloseDown) finishes it");
  }

  /* Copy > Embed code: the clipboard claimed; a paste (DataRequest) gets the
     text by the save protocol (DataSave, DataSaveAck, the file, DataLoad) */
  {
    int sel[4] = { 4, 0, -1, -1 }, msg[64];
    const int *m;
    type_and_search ("test");
    pump ();
    click_tile (0, 100, 100, 2);
    m = (const int *) last_menu;
    CHECK (m && !strcmp ((const char *) &m[7 + 4 * 6 + 3], "Copy") && m[7 + 4 * 6 + 1], "Copy after Save video, with its submenu");
    {
      const int *cm = (const int *) m[7 + 4 * 6 + 1];
      CHECK (!strcmp ((const char *) &cm[7 + 3], "Embed code") && !strcmp ((const char *) cm[7 + 6 + 3], "Embed code (no cookies)")
             && !strcmp ((const char *) &cm[7 + 12 + 3], "Link") && !strcmp ((const char *) &cm[7 + 18 + 3], "Short link"),
             "Copy: Embed code, Embed code (no cookies), Link, Short link");
    }
    claims = 0;
    ui_event (9, sel);
    CHECK (claims == 4 && strstr (status (), "Copied the embed code"), "the clipboard claimed");
    memset (msg, 0, sizeof msg);
    msg[0] = 52; msg[1] = 0x77; msg[2] = 0x4242; msg[4] = 16;          /* DataRequest from task &77 */
    msg[5] = 9; msg[6] = -1; msg[7] = 10; msg[8] = 20; msg[9] = 4; msg[10] = 0xFFF; msg[11] = -1;
    datasave[0] = 0;
    ui_event (17, msg);
    CHECK (datasave_to == 0x77 && datasave[3] == 0x4242 && datasave[5] == 9 && datasave[10] == 0xFFF,
           "a paste: DataSave of text to it");
    memset (msg, 0, sizeof msg);
    msg[0] = 64; msg[1] = 0x77; msg[2] = 0x4343; msg[3] = 0x5151; msg[4] = 2; msg[9] = -1;
    snprintf ((char *) &msg[11], 200, "<Wimp$Scrap>");
    if (saved_data)
      saved_data[0] = 0;
    ui_event (17, msg);
    CHECK (!strcmp (saved_path, "<Wimp$Scrap>") && saved_data && !strncmp (saved_data, "<iframe width=\"560\" height=\"315\" src=\"https://www.youtube.com/embed/", 68)
           && strstr (saved_data, "allowfullscreen></iframe>"), "the embed code written where it said");
    CHECK (dataload_to == 0x77 && !strcmp (dataload_path, "<Wimp$Scrap>"), "then DataLoad to it");
    /* another task claims the clipboard: a paste then isn't ours to answer */
    memset (msg, 0, sizeof msg);
    msg[0] = 24; msg[1] = 0x88; msg[4] = 15; msg[5] = 4;
    ui_event (17, msg);
    memset (msg, 0, sizeof msg);
    msg[0] = 52; msg[1] = 0x77; msg[4] = 16; msg[9] = 4; msg[10] = -1;
    datasave_to = 0;
    ui_event (17, msg);
    CHECK (datasave_to == 0, "someone else's clipboard: not answered");
    /* no cookies, and the short link */
    click_tile (0, 100, 100, 2);
    sel[1] = 1;
    ui_event (9, sel);
    msg[1] = 0x77; msg[2] = 0x4444; msg[5] = 9; msg[10] = 0xFFF; msg[11] = -1;
    ui_event (17, msg);
    memset (msg, 0, sizeof msg);
    msg[0] = 64; msg[1] = 0x77; msg[3] = 0x5151; msg[4] = 2;
    snprintf ((char *) &msg[11], 200, "<Wimp$Scrap>");
    ui_event (17, msg);
    CHECK (saved_data && strstr (saved_data, "https://www.youtube-nocookie.com/embed/"), "Embed code (no cookies)");
  }

  /* a picture that fails once is fetched again; a 320x180 one that isn't
     there (404) is fetched at 480x360 instead */
  {
    thumb *t1 = thumb_get ("https://i.ytimg.com/vi/flaky/mqdefault.jpg", 320, 180);
    thumb *t2 = thumb_get ("https://i.ytimg.com/vi/gone/mqdefault.jpg", 320, 180);
    pump ();
    CHECK (thumb_ready (t1) == 1 && flaky_requests == 2, "a picture that failed once: fetched again, shown");
    CHECK (thumb_ready (t2) == 1 && gone_mq == 1 && gone_hq == 1, "no mqdefault: hqdefault instead");
  }

  /* Reel not known: said so, nothing started */
  unsetenv ("Alias$Reel");
  started[0] = 0;
  click_tile (1, 100, 100, 4);
  pump ();
  CHECK (!started[0] && strstr (report, "Reel isn't known"), "no Reel: said so");

  /* quit */
  {
    int msg[6] = { 20, 0, 0, 0, 0, 0 };
    CHECK (ui_event (17, msg) == 1, "Message_Quit ends it");
  }
  ui_close ();
  return fake_done ("test_ui");
}
