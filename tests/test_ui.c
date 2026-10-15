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
#include <sys/stat.h>
#include "fake.h"
#define JPEG_Info 0x49980          /* (not in UnixLib's swis.h) */
#define JPEG_PlotScaled 0x49982
#include "swis.h"
#include "../src/ui.h"
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

static fwin wins[4];                       /* handle 1: main, 2: toolbar, 3: Info */
static int nwins, iconbar_icons, forced, carets, extents;
static char drawn[1 << 20];                /* every string Wimp_TextOp plotted */
static size_t drawn_len;
static char report[256], started[512], saved_path[256];
static char *saved_data;
static int sprite_outputs, sprite_plots, jpeg_plots, menus_made;
static void *last_menu;
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
    case Wimp_GetPointerInfo: ((int *) r[1])[2] = 0; return NULL;
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
    case OS_Byte: r[1] = 101; return NULL;           /* Latin-1 */
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
    Wimp_TextOp, Wimp_CreateMenu, Wimp_GetPointerInfo, Wimp_ReportError, Wimp_StartTask, 0 };
  static const int o[] = { OS_ReadModeVariable, OS_Byte, OS_Plot, ColourTrans_SetGCOL, OS_File,
    OS_SpriteOp, JPEG_Info, JPEG_PlotScaled, 0 };
  for (int i = 0; w[i]; i++)
    fake_swi (w[i], NULL, wimp);
  for (int i = 0; o[i]; i++)
    fake_swi (o[i], NULL, os);
}

/* ---- the fake network: recorded answers ----------------------------------- */

typedef struct { char rx[65536]; size_t rxlen; char *tx; size_t txlen, txpos; int requests; } fconn;
static int api_requests, picture_requests, player_requests;
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
  else if (strstr (req, "Host: raw.githubusercontent.com"))
    {
      manifest_requests++;                           /* (the daily solver check: not here) */
      body = strdup ("404: Not Found");
      len = strlen (body);
      code = 404;
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
      body = strdup ("\xFF\xD8\xFF\xE0 a JPEG of sorts");
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
  yt_config_set ("stream_format", "dash");     /* (HLS has its own part, near the end) */
  CHECK (iconbar_icons == 1 && nwins == 3, "an icon bar icon and three windows");
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
    CHECK (m && !strcmp ((const char *) &m[7 + 3 * 6 + 3], "Quit") && (m[7 + 3 * 6] & 0x80), "Quit is last");
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
    int sel[4] = { 4, 2, -1, -1 };              /* Play, Related, Channel, Back, Quality > 720p */
    ui_event (9, sel);
    click_tile (3, 100, 100, 4);
    pump ();
    CHECK (saved_data && strstr (saved_data, "itag=136") && !strstr (saved_data, "itag=137"), "720p after choosing it");
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
    CHECK (bar[21] == 6, "the toolbar has the date filter's two icons");
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
    CHECK (saved_data && strstr (saved_data, "/itag/232/") && strstr (saved_data, "itag%3D140/playlist")
           && !strstr (saved_data, "lang%3Dde") && strstr (saved_data, "\"protocol\":\"m3u8_native\"")
           && !strstr (saved_data, "itag=136"), "HLS: the 720p variant and the default sound");
    CHECK (strstr (status (), expect_q) && started[0], "HLS: playing");
    /* a master playlist that isn't one: the DASH streams instead */
    hls_broken = 1;
    started[0] = 0;
    click_tile (1, 100, 100, 4);
    pump ();
    CHECK (hls_requests == 2 && saved_data && strstr (saved_data, "itag=136") && !strstr (saved_data, "m3u8_native")
           && started[0], "HLS unusable: DASH");
    hls_broken = 0;
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
