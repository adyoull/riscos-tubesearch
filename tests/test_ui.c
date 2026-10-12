/* Tests ui.c (with thumbs.c, api.c, http.c, yt.c...) against riscos-
   crossdev's fake RISC OS: a fake Wimp records the windows, the toolbar's
   nesting, text and sprites drawn, menus, errors, files saved and tasks
   started; a fake transport answers with the recorded YouTube fixtures.
   It drives Tube the way a user would: open the window, search, see the
   pictures arrive, play, related videos, a channel, More, Back, menus,
   resizing, a pasted address, and an unplayable video.
     test_ui FIXTURE_DIR
   Part of Tube. MIT licence. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fake.h"
#define JPEG_Info 0x49980          /* (not in UnixLib's swis.h) */
#define JPEG_PlotScaled 0x49982
#include "swis.h"
#include "../src/ui.h"
#include "../src/http.h"
#include "../src/transport.h"

static const char *fixdir;

/* ---- the fake Wimp ------------------------------------------------------ */

typedef struct {
  int block[22 + 4 * 8];      /* as created */
  int vis[4], sx, sy, open, parent, nest_flags, opens;
  int extent[4];
} fwin;

static fwin wins[3];                       /* handle 1: main, 2: toolbar */
static int nwins, iconbar_icons, forced, carets, extents;
static char drawn[1 << 20];                /* every string Wimp_TextOp plotted */
static size_t drawn_len;
static char report[256], started[512], saved_path[256];
static char *saved_data;
static int sprite_outputs, sprite_plots, jpeg_plots, menus_made;
static void *last_menu;

static const _kernel_oserror err_bad = { 1, "Bad" };

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
    case Wimp_StartTask: snprintf (started, sizeof started, "%s", (const char *) r[0]); return NULL;
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
  static const int w[] = { Wimp_Initialise, Wimp_CreateIcon, Wimp_CreateWindow, Wimp_OpenWindow,
    Wimp_GetWindowState, Wimp_CloseWindow, Wimp_SetExtent, Wimp_ForceRedraw, Wimp_SetIconState,
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

static void answer (fconn *c, const char *req)
{
  char *body;
  size_t len;
  char head[128];
  const char *json = strstr (req, "\r\n\r\n") + 4;
  if (!strncmp (req, "GET ", 4))
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
      else if (strstr (req, "/search"))
        body = load (strstr (json, "\"continuation\"") ? "search_2.json" : "search_1.json", &len);
      else if (strstr (req, "/next"))
        body = load ("next.json", &len);
      else if (strstr (req, "/browse"))
        body = load (strstr (json, "\"continuation\"") ? "channel_2.json" : "channel_1.json", &len);
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
  snprintf (head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Length: %lu\r\n\r\n", (unsigned long) len);
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
  for (int i = 0; i < 10000 && ui_busy (); i++)
    {
      net_clock++;
      CHECK (ui_event (0, block) == 0, "a null event doesn't quit");
    }
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
  setenv ("Wimp$ScrapDir", "RAM::RamDisc0.$.Scrap", 1);
  setenv ("Alias$Reel", "/<Reel$Dir> %*0", 1);
  unsetenv ("Tube$Dir");

  CHECK (ui_init () == 0, "starts");
  CHECK (iconbar_icons == 1 && nwins == 2, "an icon bar icon and two windows");
  CHECK (!strcmp (title (), "Tube"), "the window's title");

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
  CHECK (!strcmp (title (), "Tube: test"), "the title says what was searched for");
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
  CHECK (!strcmp (saved_path, "RAM::RamDisc0.$.Scrap.TubePlay1"), "Reel's file in the scrap directory");
  CHECK (saved_data && strstr (saved_data, "\"requested_formats\"") && strstr (saved_data, "itag=137")
         && strstr (saved_data, "itag=140"), "the 1080p video and the AAC sound");
  CHECK (!strcmp (started, "Reel RAM::RamDisc0.$.Scrap.TubePlay1"), "Reel started with it");
  CHECK (strstr (status (), "Playing 1080p") != NULL, "the status says it's playing");

  /* Adjust: related videos */
  click_tile (0, 100, 100, 1);
  pump ();
  CHECK (!strcmp (title (), "Tube: Related to Marzia & Felix - Wedding 19.08.2019"), "the related page's title");
  CHECK (!strncmp (status (), "20 videos", 9), "20 related videos");

  /* the channel's name on a tile: that channel */
  click_tile (0, 100, 490, 4);
  pump ();
  CHECK (!strcmp (title (), "Tube: Creative Commons"), "the channel page");
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
    CHECK (!strcmp (title (), "Tube: Related to Marzia & Felix - Wedding 19.08.2019"), "Back: related");
    click (2, 0, 0, 0, 4);
    CHECK (!strcmp (title (), "Tube: test"), "Back again: the search");
    CHECK (!strncmp (status (), "24 videos", 9), "its status back");
    CHECK (api_requests == before, "nothing fetched again");
    click (2, 0, 0, 0, 4);
    CHECK (!strcmp (title (), "Tube: test"), "Back at the first page does nothing");
  }

  /* Menu on a tile: Play */
  click_tile (3, 100, 100, 2);
  CHECK (menus_made == 1 && last_menu != NULL, "the tile menu opens");
  {
    int sel[4] = { 0, -1, -1, -1 };
    int before = player_requests;
    CHECK (ui_event (9, sel) == 0, "choosing Play");
    pump ();
    CHECK (player_requests == before + 1 && strstr (started, "TubePlay2"), "Play from the menu");
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
  CHECK (!strcmp (title (), "Tube: Related to Marzia & Felix - Wedding 19.08.2019"), "an address: related");
  CHECK (saved_data && strstr (saved_data, "Marzia & Felix"), "and it plays");

  /* a video YouTube won't play */
  type_and_search ("youtu.be/BADBADBADBA");
  pump ();
  CHECK (strstr (report, "Video unavailable") != NULL, "the reason is reported");

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
