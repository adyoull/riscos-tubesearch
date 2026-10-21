/* ui.c - see ui.h. The window and menus are built in code (no Templates
   file), with the block layouts from the PRMs (Wimp_CreateWindow,
   Wimp_CreateIcon, Wimp_CreateMenu) and the nested Wimp's toolbar (a
   child window, Wimp_OpenWindow with "TASK").

   Coordinates: everything is in OS units. Work area y runs down from 0
   at the top; the toolbar covers the top TOOLBAR_H of the visible area,
   so the grid starts below it.

   Part of TubeSearch. MIT licence. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <kernel.h>
#include <swis.h>
#include "ui.h"
#include "api.h"
#include "http.h"
#include "text.h"
#include "thumbs.h"
#include "proxy.h"
#include "subs.h"
#include <time.h>
#include "transport.h"
#include "ejsupd.h"
#include "yt.h"

#define APP_NAME "TubeSearch"
#ifndef TUBE_VERSION
#define TUBE_VERSION "0.0.0"             /* (the Makefile sets it) */
#endif
#ifndef TUBE_DATE
#define TUBE_DATE ""
#endif
#define TASK_WORD 0x4B534154          /* "TASK" */

/* Wimp_Poll reasons and messages */
enum { Null_Reason = 0, Redraw_Window_Request = 1, Open_Window_Request = 2, Close_Window_Request = 3,
       Mouse_Click = 6, Key_Pressed = 8, Menu_Selection = 9, User_Message = 17,
       User_Message_Recorded = 18 };
enum { Button_Adjust = 1, Button_Menu = 2, Button_Select = 4 };
#define Message_Quit 0
#define Message_DataLoad 3
#define Message_DataLoadAck 4
#define Message_ModeChange 0x400C1
#define Message_FontChanged 0x400CF

/* layout (OS units) */
#define TOOLBAR_H 84
#define PAD 16
#define TILE_W 680
#define TILE_H 560
#define THUMB_W 640                   /* 320 x 180 pixels at 90 dpi */
#define THUMB_H 360
#define LINE_H 40
#define TEXT_W (TILE_W - 40)

/* toolbar icons */
enum { I_BACK, I_FIELD, I_SEARCH, I_STATUS, I_DATE, I_DATEPOP, I_REFRESH, I_SUBS };

#define MAX_PAGES 24

typedef struct {
  char line[2][192];          /* the title, wrapped into two lines */
  int wrapped;                /* for wrap_gen */
} tile_text;

typedef struct page {
  int kind;                   /* YT_LIST_* */
  char *arg;                  /* query, video ID or channel ID */
  char *heading;              /* for the title bar */
  yt_list list;
  tile_text *text;            /* one per item */
  int text_cap;
  api_op *op;                 /* fetching (the first part or more) */
  int scroll_y;
  int date;                   /* a search's upload-date filter (YT_DATE_*) */
  long seen;                  /* subscriptions: newer than this was marked new */
  api_feed_result feed_res;   /* subscriptions: how the reading went */
  char status[200];
} page;

static int task, win = -1, bar = -1, info = -1, win_open;
static int cols = 3;
static int screen_w = 3840, screen_h = 2160;
static int wrap_gen = 1;
static page *pages[MAX_PAGES];
static int depth;
static api_op *play_op;
static int play_answered;                    /* play_done has been called for the newest play */
static char play_id[40];
static int play_file_n;
static int quality = 1080;
static yt_subs subs;                         /* the subscriptions (subs.h) */
static char subs_path[300];
static int search_date;                      /* YT_DATE_*: for the next search */
static char date_buf[16] = "Any time";

static char title_buf[300] = APP_NAME;
static char field_buf[256];
static char status_buf[200] = "Type what you'd like to watch and press Return";

/* ---- small helpers ------------------------------------------------------ */

void ui_report (const char *msg)
{
  _kernel_oserror e;
  e.errnum = 0;
  snprintf (e.errmess, sizeof e.errmess, "%.250s", msg);
  _swix (Wimp_ReportError, _INR (0, 2), &e, 1 /* OK */, APP_NAME);
}

static int fail (const _kernel_oserror *e)
{
  if (e)
    ui_report (e->errmess);
  return e != NULL;
}

static page *cur (void)
{
  return depth ? pages[depth - 1] : NULL;
}

static void set_status (const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));
static void set_status (const char *fmt, ...)
{
  int b[4] = { 0, I_STATUS, 0, 0 };
  va_list ap;
  va_start (ap, fmt);
  vsnprintf (status_buf, sizeof status_buf, fmt, ap);
  va_end (ap);
  if (bar != -1)
    {
      b[0] = bar;
      _swix (Wimp_SetIconState, _IN (1), b);
    }
}

static void set_title (void)
{
  page *p = cur ();
  snprintf (title_buf, sizeof title_buf, "%s%s%s", APP_NAME, p && p->heading ? ": " : "",
            p && p->heading ? p->heading : "");
  if (win != -1)
    _swix (Wimp_ForceRedraw, _INR (0, 2), win, TASK_WORD, 3);    /* the title bar */
}

static void text_colour (unsigned fg, unsigned bg)
{
  _swix (Wimp_TextOp, _INR (0, 2), 0, fg, bg);
}

static void text_plot (const char *t, int x, int y)
{
  _swix (Wimp_TextOp, _INR (0, 5), 2, t, -1, -1, x, y);
}

static int text_width (const char *t, int n)
{
  int w = 0;
  if (_swix (Wimp_TextOp, _INR (0, 2) | _OUT (0), 1, t, n, &w))
    return n * 16;
  return w;
}

static void fill_rect (unsigned colour, int x0, int y0, int x1, int y1)
{
  _swix (ColourTrans_SetGCOL, _IN (0) | _INR (3, 4), colour, 0, 0);
  _swix (OS_Plot, _INR (0, 2), 4, x0, y0);
  _swix (OS_Plot, _INR (0, 2), 96 + 5, x1 - 1, y1 - 1);
}

/* s cut to fit maxw, at a word if it can, "..." added when cut. */
static void fit (const char *s, int maxw, char *out, size_t size, const char **rest)
{
  size_t n = strlen (s), best = 0;
  if (rest)
    *rest = s + n;
  if (text_width (s, 0) <= maxw)
    {
      snprintf (out, size, "%s", s);
      return;
    }
  /* the longest prefix that fits, by halving */
  {
    size_t lo = 0, hi = n;
    while (lo < hi)
      {
        size_t mid = (lo + hi + 1) / 2;
        if (text_width (s, (int) mid) <= maxw)
          lo = mid;
        else
          hi = mid - 1;
      }
    best = lo;
  }
  while (best > 0 && ((unsigned char) s[best] & 0xC0) == 0x80)
    best--;                                   /* (not in the middle of a UTF-8 character) */
  if (rest)
    {
      /* a line break: back to the last space */
      size_t b = best;
      while (b > 0 && s[b] != ' ')
        b--;
      if (b > best / 2)
        best = b;
      snprintf (out, size, "%.*s", (int) best, s);
      *rest = s + best;
      while (**rest == ' ')
        (*rest)++;
      return;
    }
  /* the end: make room for the dots */
  while (best > 0)
    {
      char tmp[400];
      snprintf (tmp, sizeof tmp, "%.*s...", (int) best, s);
      if (text_width (tmp, 0) <= maxw)
        break;
      best--;
      while (best > 0 && ((unsigned char) s[best] & 0xC0) == 0x80)
        best--;
    }
  snprintf (out, size, "%.*s...", (int) best, s);
}

static void wrap_title (page *p, int i)
{
  tile_text *t = &p->text[i];
  const char *rest;
  if (t->wrapped == wrap_gen)
    return;
  fit (p->list.items[i].title, TEXT_W, t->line[0], sizeof t->line[0], &rest);
  if (*rest)
    fit (rest, TEXT_W, t->line[1], sizeof t->line[1], NULL);
  else
    t->line[1][0] = 0;
  t->wrapped = wrap_gen;
}

static int ensure_text (page *p)
{
  if (p->text_cap < p->list.n)
    {
      int cap = p->list.n + 32;
      tile_text *n = realloc (p->text, (size_t) cap * sizeof *n);
      if (!n)
        return -1;
      memset (n + p->text_cap, 0, (size_t) (cap - p->text_cap) * sizeof *n);
      p->text = n;
      p->text_cap = cap;
    }
  return 0;
}

/* ---- layout ------------------------------------------------------------- */

/* The tiles on a page: its items, and a "More" tile when there are more. */
static int tiles (const page *p)
{
  return !p ? 0 : p->list.n + (p->list.continuation || (p->op && p->list.n) ? 1 : 0);
}

static int extent_height (void)
{
  page *p = cur ();
  int rows = (tiles (p) + cols - 1) / cols;
  return TOOLBAR_H + PAD + rows * TILE_H + PAD;
}

static void set_extent (void)
{
  int b[4];
  b[0] = 0;
  b[1] = -extent_height ();
  b[2] = screen_w;
  b[3] = 0;
  if (b[1] > -screen_h)
    b[1] = -screen_h;
  _swix (Wimp_SetExtent, _INR (0, 1), win, b);
}

static void tile_box (int i, int *x0, int *y1)
{
  *x0 = PAD + (i % cols) * TILE_W;
  *y1 = -(TOOLBAR_H + PAD + (i / cols) * TILE_H);
}

static void redraw_all (void)
{
  _swix (Wimp_ForceRedraw, _INR (0, 4), win, 0, -extent_height () - screen_h, screen_w, 0);
}

static void redraw_tile (int i)
{
  int x0, y1;
  tile_box (i, &x0, &y1);
  _swix (Wimp_ForceRedraw, _INR (0, 4), win, x0, y1 - TILE_H, x0 + TILE_W, y1);
}

static void read_screen (void)
{
  int xeig = 1, yeig = 1, xlim = 1919, ylim = 1079;
  _swix (OS_ReadModeVariable, _INR (0, 1) | _OUT (2), -1, 4, &xeig);
  _swix (OS_ReadModeVariable, _INR (0, 1) | _OUT (2), -1, 5, &yeig);
  _swix (OS_ReadModeVariable, _INR (0, 1) | _OUT (2), -1, 11, &xlim);
  _swix (OS_ReadModeVariable, _INR (0, 1) | _OUT (2), -1, 12, &ylim);
  screen_w = (xlim + 1) << xeig;
  screen_h = (ylim + 1) << yeig;
}

/* ---- drawing ------------------------------------------------------------ */

static void draw_tile (page *p, int i, int ox, int oy)
{
  int x0, y1, sx, sy, tx, ty;
  tile_box (i, &x0, &y1);
  sx = ox + x0;
  sy = oy + y1;
  tx = sx + 20;
  ty = sy - 20 - THUMB_H;                         /* the bottom of the picture */
  if (i >= p->list.n)
    {
      /* the "More" tile */
      const char *t = p->op ? "Loading..." : "More";
      int w = text_width (t, 0);
      fill_rect (0xEEEEEE00u, tx, ty, tx + THUMB_W, ty + THUMB_H);
      text_colour (0x44444400u, 0xEEEEEE00u);
      text_plot (t, tx + (THUMB_W - w) / 2, ty + THUMB_H / 2 - 10);
      return;
    }
  {
    yt_item *it = &p->list.items[i];
    int chan = it->kind == YT_CHANNEL;
    int pw = chan ? 180 : 320, ph = 180;          /* picture size, pixels */
    int px = tx + (THUMB_W - pw * 2) / 2;
    thumb *th = thumb_get (it->thumb_url, pw, ph);
    if (thumb_ready (th) == 1)
      thumb_plot (th, px, ty);
    else
      fill_rect (thumb_ready (th) < 0 ? 0xCCCCCC00u : 0xE4E4E400u, px, ty, px + pw * 2, ty + THUMB_H);
    if (it->duration[0])
      {
        int w = text_width (it->duration, 0) + 16;
        unsigned bg = !strcmp (it->duration, "LIVE") ? 0x0000CC00u : 0x10101000u;
        fill_rect (bg, tx + THUMB_W - w - 8, ty + 8, tx + THUMB_W - 8, ty + 8 + LINE_H);
        text_colour (0xFFFFFF00u, bg);
        text_plot (it->duration, tx + THUMB_W - w, ty + 8 + 10);
      }
    if (ensure_text (p) == 0)
      {
        wrap_title (p, i);
        text_colour (0x00000000u, 0xFFFFFF00u);
        text_plot (p->text[i].line[0], tx, ty - 8 - LINE_H + 10);
        text_plot (p->text[i].line[1], tx, ty - 8 - 2 * LINE_H + 10);
      }
    {
      char buf[200];
      fit (chan ? "Channel" : it->channel, TEXT_W, buf, sizeof buf, NULL);
      text_colour (chan ? 0x60606000u : 0x80300000u, 0xFFFFFF00u);   /* channel names: dark blue */
      text_plot (buf, tx, ty - 8 - 3 * LINE_H + 10);
      fit (it->meta, TEXT_W, buf, sizeof buf, NULL);
      text_colour (0x70707000u, 0xFFFFFF00u);
      text_plot (buf, tx, ty - 8 - 4 * LINE_H + 10);
    }
  }
}

static void draw (int *b, int more)
{
  page *p = cur ();
  while (more)
    {
      int ox = b[1] - b[5], oy = b[4] - b[6];   /* screen position of the work area origin */
      int cy0 = b[8] - oy, cy1 = b[10] - oy;    /* the clip box, in work area y */
      if (!p || (!p->list.n && !p->op))
        {
          const char *t = p ? p->status : "Type what you'd like to watch in the box above, then press Return.";
          text_colour (0x60606000u, 0xFFFFFF00u);
          text_plot (t, ox + PAD + 20, oy - TOOLBAR_H - PAD - 60);
          if (p && (p->kind == YT_LIST_SUBS || p->kind == YT_LIST_MYCHANNELS) && !subs.n)
            {
              text_plot ("Choose Subscribe on a channel's menu (or a video's) to add it here.",
                         ox + PAD + 20, oy - TOOLBAR_H - PAD - 120);
              text_plot ("Or drop a list of your subscriptions on this window: Google Takeout's",
                         ox + PAD + 20, oy - TOOLBAR_H - PAD - 170);
              text_plot ("subscriptions.csv, or NewPipe's or FreeTube's export.",
                         ox + PAD + 20, oy - TOOLBAR_H - PAD - 220);
            }
          if (!p)
            {
              text_plot ("Select on a video plays it in Reel. Adjust shows related videos.",
                         ox + PAD + 20, oy - TOOLBAR_H - PAD - 120);
              text_plot ("Click a channel's name for its videos. Menu has more.",
                         ox + PAD + 20, oy - TOOLBAR_H - PAD - 170);
            }
        }
      else
        {
          int n = tiles (p);
          int row0 = (-cy1 - TOOLBAR_H - PAD) / TILE_H, row1 = (-cy0 - TOOLBAR_H - PAD) / TILE_H;
          if (row0 < 0)
            row0 = 0;
          for (int row = row0; row <= row1; row++)
            for (int c = 0; c < cols; c++)
              {
                int i = row * cols + c;
                if (i < n)
                  draw_tile (p, i, ox, oy);
              }
        }
      if (_swix (Wimp_GetRectangle, _IN (1) | _OUT (0), b, &more))
        break;
    }
}

static void redraw (int *b)
{
  int more = 0;
  if (_swix (Wimp_RedrawWindow, _IN (1) | _OUT (0), b, &more))
    return;
  draw (b, more);
}

/* ---- windows ------------------------------------------------------------ */

static int create_main (void)
{
  int b[23];
  int w = 3 * TILE_W + 2 * PAD + 40, h = 2 * TILE_H + TOOLBAR_H + 200;
  memset (b, 0, sizeof b);
  if (w > screen_w - 200)
    w = screen_w - 200;
  if (h > screen_h - 300)
    h = screen_h - 300;
  b[0] = (screen_w - w) / 2;
  b[1] = (screen_h - h) / 2;
  b[2] = b[0] + w;
  b[3] = b[1] + h;
  b[6] = -1;
  /* new flags: back, close, title, toggle size, vertical scroll bar, adjust
     size; movable; the program draws the work area */
  b[7] = (int) (0x80000000u | (1u << 24) | (1u << 25) | (1u << 26) | (1u << 27) | (1u << 28) | (1u << 29) | 2u);
  ((unsigned char *) b)[32] = 7;  ((unsigned char *) b)[33] = 2;   /* title */
  ((unsigned char *) b)[34] = 7;  ((unsigned char *) b)[35] = 0;   /* work area: white */
  ((unsigned char *) b)[36] = 3;  ((unsigned char *) b)[37] = 1;   /* scroll bar */
  ((unsigned char *) b)[38] = 12;
  b[10] = 0; b[11] = -screen_h; b[12] = screen_w; b[13] = 0;      /* work area */
  b[14] = 0x13D;              /* title: text, border, centred, filled, indirected */
  b[15] = 3 << 12;            /* work area: clicks */
  b[16] = 1;
  b[17] = 0;
  b[18] = (int) title_buf; b[19] = -1; b[20] = sizeof title_buf;
  b[21] = 0;
  return fail (_swix (Wimp_CreateWindow, _IN (1) | _OUT (0), b, &win));
}

static void icon (int *ic, int x0, int y0, int x1, int y1, int flags, char *text, const char *valid, int size)
{
  ic[0] = x0; ic[1] = y0; ic[2] = x1; ic[3] = y1;
  ic[4] = flags;
  ic[5] = (int) text; ic[6] = (int) valid; ic[7] = size;
}

static int create_bar (void)
{
  static char back_text[] = "Back", search_text[] = "Search", refresh_text[] = "Refresh", refresh_valid[] = "R5,3",
    subs_text[] = "Subscriptions", subs_valid[] = "R5,3";
  static char back_valid[] = "R5,3", search_valid[] = "R6,3", field_valid[] = "Pptr_write;R7",
    status_valid[] = "";
  static char date_valid[] = "R2", pop_text[] = "", pop_valid[] = "Sgright;R5,3";
  int b[22 + 8 * 8];
  memset (b, 0, sizeof b);
  b[0] = 0; b[1] = 0; b[2] = 1000; b[3] = TOOLBAR_H;
  b[6] = -1;
  b[7] = (int) (0x80000000u | (1u << 4));          /* no furniture; the Wimp draws it */
  ((unsigned char *) b)[32] = 7;  ((unsigned char *) b)[33] = 2;
  ((unsigned char *) b)[34] = 7;  ((unsigned char *) b)[35] = 1;  /* light grey */
  ((unsigned char *) b)[36] = 3;  ((unsigned char *) b)[37] = 1;
  ((unsigned char *) b)[38] = 12;
  b[10] = 0; b[11] = -TOOLBAR_H; b[12] = screen_w; b[13] = 0;
  b[14] = 0;
  b[15] = 0;
  b[16] = 1;
  b[21] = 8;
  /* Back, the search box, Search, the status line; then the upload-date
     filter (a display field and its pop-up menu button) between Search
     and the status line */
  icon (&b[22], 12, -72, 136, -12, 0x1700313D, back_text, back_valid, sizeof back_text);
  icon (&b[30], 532, -72, 1060, -12, 0x0700F135, field_buf, field_valid, sizeof field_buf);
  icon (&b[38], 1072, -72, 1232, -12, 0x1700313D, search_text, search_valid, sizeof search_text);
  icon (&b[46], 1546, -72, 3800, -12, 0x15000131, status_buf, status_valid, sizeof status_buf);
  icon (&b[54], 1248, -68, 1476, -16, 0x1700313D, date_buf, date_valid, sizeof date_buf);
  icon (&b[62], 1480, -68, 1528, -16, 0x1700313F, pop_text, pop_valid, sizeof pop_text);
  icon (&b[70], 148, -72, 288, -12, 0x1700313D, refresh_text, refresh_valid, sizeof refresh_text);
  icon (&b[78], 300, -72, 520, -12, 0x1700313D, subs_text, subs_valid, sizeof subs_text);
  return fail (_swix (Wimp_CreateWindow, _IN (1) | _OUT (0), b, &bar));
}

/* The Info box: the usual program information window, opened by the
   Wimp as the submenu of Info on the icon bar menu. */
static int create_info (void)
{
  static char title[] = "About this program";
  static char name[] = APP_NAME;
  static char purpose[] = "Browse YouTube; play in Reel";
  static char author[] = "\xA9 Andrew Youll, 2026";
  static char version[48];
  static char label_name[] = "Name", label_purpose[] = "Purpose", label_author[] = "Author",
    label_version[] = "Version", none[] = "";
  static char *const labels[4] = { label_name, label_purpose, label_author, label_version };
  char *const values[4] = { name, purpose, author, version };
  int b[22 + 8 * 8];
  int w = 920, row = 52, h = 4 * row + 24;
  snprintf (version, sizeof version, "%s%s%s%s", TUBE_VERSION, TUBE_DATE[0] ? " (" : "", TUBE_DATE,
            TUBE_DATE[0] ? ")" : "");
  memset (b, 0, sizeof b);
  b[0] = 0; b[1] = 0; b[2] = w; b[3] = h;
  b[6] = -1;
  b[7] = (int) (0x80000000u | (1u << 26) | 2u | (1u << 4));   /* title; movable; the Wimp redraws it */
  ((unsigned char *) b)[32] = 7;  ((unsigned char *) b)[33] = 2;
  ((unsigned char *) b)[34] = 7;  ((unsigned char *) b)[35] = 1;   /* grey */
  ((unsigned char *) b)[36] = 3;  ((unsigned char *) b)[37] = 1;
  ((unsigned char *) b)[38] = 12;
  b[10] = 0; b[11] = -h; b[12] = w; b[13] = 0;
  b[14] = 0x13D;              /* title: text, border, centred, filled, indirected */
  b[15] = 0;
  b[16] = 1;
  b[18] = (int) title; b[19] = -1; b[20] = sizeof title;
  b[21] = 8;
  for (int i = 0; i < 4; i++)
    {
      int y1 = -12 - i * row, y0 = y1 - row + 8;
      /* the label, right-aligned; the value in a box beside it */
      icon (&b[22 + 16 * i], 8, y0, 196, y1, 0x17000311, labels[i], none, (int) strlen (labels[i]) + 1);
      icon (&b[30 + 16 * i], 212, y0, w - 16, y1, 0x07000135, values[i], none, (int) strlen (values[i]) + 1);
    }
  return fail (_swix (Wimp_CreateWindow, _IN (1) | _OUT (0), b, &info));
}

/* Opens the toolbar inside the window's top edge (a child window, kept
   there by the Wimp as the window moves and changes size). */
static void open_bar (const int *main_state)
{
  int b[8];
  unsigned flags = (1u << 16)      /* left edge: the parent's visible left */
                   | (2u << 18)    /* bottom: the parent's visible top */
                   | (2u << 20)    /* right: the parent's visible right */
                   | (2u << 22)    /* top: the parent's visible top */
                   | (1u << 24)    /* x scroll: fixed to the left */
                   | (2u << 26);   /* y scroll: fixed to the top */
  b[0] = bar;
  b[1] = main_state[1];
  b[2] = main_state[4] - TOOLBAR_H;
  b[3] = main_state[3];
  b[4] = main_state[4];
  b[5] = 0;
  b[6] = 0;
  b[7] = -1;
  fail (_swix (Wimp_OpenWindow, _INR (1, 4), b, TASK_WORD, win, flags));
}

static void open_main (int *b)
{
  int new_cols;
  if (fail (_swix (Wimp_OpenWindow, _IN (1), b)))
    return;
  win_open = 1;
  new_cols = (b[3] - b[1] - 2 * PAD) / TILE_W;
  if (new_cols < 1)
    new_cols = 1;
  if (new_cols != cols)
    {
      cols = new_cols;
      set_extent ();
      redraw_all ();
      _swix (Wimp_OpenWindow, _IN (1), b);      /* (the extent may have moved the scroll) */
    }
  open_bar (b);
}

static void show_window (int caret)
{
  int b[9];
  b[0] = win;
  if (fail (_swix (Wimp_GetWindowState, _IN (1), b)))
    return;
  b[7] = -1;
  open_main (b);
  if (caret)
    _swix (Wimp_SetCaretPosition, _INR (0, 5), bar, I_FIELD, -1, -1, -1, (int) strlen (field_buf));
}

/* Scrolls the window to y (from the page's remembered position). */
static void scroll_to (int y)
{
  int b[9];
  b[0] = win;
  if (_swix (Wimp_GetWindowState, _IN (1), b))
    return;
  b[6] = y;
  if (win_open)
    open_main (b);
}

/* ---- pages -------------------------------------------------------------- */

static void free_page (page *p)
{
  if (!p)
    return;
  api_cancel (p->op);
  yt_list_free (&p->list);
  free (p->text);
  free (p->arg);
  free (p->heading);
  free (p);
}

/* ---- subscriptions (subs.h) ---------------------------------------------- */

#define FEED_MAX 150                         /* the newest this many (the pictures kept) */

static void subs_store (void)
{
  if (subs_path[0] && subs_save (&subs, subs_path))
    ui_report ("The subscriptions couldn't be saved");
}

/* a name shown here (the desktop's alphabet) back into UTF-8 for the file */
static char *to_utf8 (const char *s)
{
  size_t n = strlen (s);
  char *o = malloc (n * 2 + 1), *w = o;
  if (!o)
    return NULL;
  for (const unsigned char *r = (const unsigned char *) s; *r; r++)
    if (*r < 0x80 || text_utf8_desktop)
      *w++ = (char) *r;
    else
      {
        *w++ = (char) (0xC0 | (*r >> 6));
        *w++ = (char) (0x80 | (*r & 63));
      }
  *w = 0;
  return o;
}

static int count_new (const page *p)
{
  int n = 0;
  for (int i = 0; i < p->list.n; i++)
    n += p->seen && p->list.items[i].published > p->seen;
  return n;
}

static void list_done (int added, const char *err, void *ctx)
{
  page *p = ctx;
  p->op = NULL;
  if (added < 0)
    snprintf (p->status, sizeof p->status, "%s", err);
  else if (p->kind == YT_LIST_SUBS || p->kind == YT_LIST_MYCHANNELS)
    {
      wrap_gen++;                                         /* (the list is new: lay its titles out again) */
      free (p->heading);
      p->heading = strdup (p->kind == YT_LIST_SUBS ? "Subscriptions" : "My channels");
      if (!subs.n)
        snprintf (p->status, sizeof p->status, "No subscriptions yet");
      else if (p->kind == YT_LIST_MYCHANNELS)
        snprintf (p->status, sizeof p->status, "%d channel%s", subs.n, subs.n == 1 ? "" : "s");
      else
        {
          int n = count_new (p);
          snprintf (p->status, sizeof p->status, "%d video%s from %d channel%s", p->list.n, p->list.n == 1 ? "" : "s",
                    subs.n, subs.n == 1 ? "" : "s");
          if (n)
            snprintf (p->status + strlen (p->status), sizeof p->status - strlen (p->status), ", %d new", n);
          if (p->feed_res.failed)
            snprintf (p->status + strlen (p->status), sizeof p->status - strlen (p->status),
                      " (%d couldn't be read)", p->feed_res.failed);
          /* these are seen now: next time, only newer ones are new (unless
             some channels couldn't be read, whose new videos aren't seen yet) */
          if (!p->feed_res.failed && p->feed_res.newest > subs.seen)
            subs.seen = p->feed_res.newest;
          subs_store ();                                 /* (and the names the feeds brought up to date) */
        }
    }
  else
    {
      if (!p->heading || p->kind != YT_LIST_SEARCH)
        {
          char h[256];
          if (p->kind == YT_LIST_RELATED)
            snprintf (h, sizeof h, "Related to %s", p->list.title ? p->list.title : p->arg);
          else if (p->kind == YT_LIST_CHANNEL)
            snprintf (h, sizeof h, "%s", p->list.title ? p->list.title : p->arg);
          else
            snprintf (h, sizeof h, "%s", p->arg);
          free (p->heading);
          p->heading = strdup (h);
        }
      if (!p->list.n)
        snprintf (p->status, sizeof p->status, "Nothing found");
      else
        snprintf (p->status, sizeof p->status, "%d %s%s", p->list.n, p->list.n == 1 ? "video" : "videos",
                  p->list.continuation ? " (More at the end)" : "");
    }
  if (p == cur ())
    {
      /* (a page's status is its list's; playing only shows for a while) */
      set_status ("%s", p->status);
      set_title ();
      ensure_text (p);
      set_extent ();
      redraw_all ();
      /* the pictures, in order (the ones on screen are asked for as they're drawn) */
      for (int i = 0; i < p->list.n; i++)
        thumb_get (p->list.items[i].thumb_url, p->list.items[i].kind == YT_CHANNEL ? 180 : 320, 180);
    }
}

/* The upload-date filter for the next search, shown in the toolbar. */
static void show_search_date (int date)
{
  int b[4] = { bar, I_DATE, 0, 0 };
  search_date = date >= 0 && date < YT_DATE_COUNT ? date : YT_DATE_ANY;
  snprintf (date_buf, sizeof date_buf, "%s", yt_date_name (search_date));
  if (bar != -1)
    _swix (Wimp_SetIconState, _IN (1), b);
}

/* http_proxy (and no_proxy) as they are now, which is what Reel will use:
   read before each thing fetched; when they change, connections kept
   from before are closed, and a bad value is reported (once). */
static void check_proxy (void)
{
  char err[200];
  int k = proxy_refresh (NULL, err, sizeof err);
  if (k)
    http_close_idle ();
  if (k < 0)
    ui_report (err);
}

static void feed_progress (int done, int total, void *ctx)
{
  page *p = ctx;
  snprintf (p->status, sizeof p->status, "Reading the channels: %d of %d...", done, total);
  if (p == cur ())
    set_status ("%s", p->status);
}

/* Starts fetching p's list from the start (the page's own way): p->op,
   and p->status says so. A list made here (My channels) is done at once. */
static void fetch (page *p, const char *doing)
{
  snprintf (p->status, sizeof p->status, "%s", doing);
  if (p->kind == YT_LIST_MYCHANNELS)
    {
      p->op = NULL;
      list_done (subs_channel_list (&subs, &p->list), NULL, p);
      return;
    }
  if (p->kind == YT_LIST_SUBS)
    {
      const char **ids = malloc ((size_t) (subs.n + 1) * sizeof *ids);
      if (!ids)
        {
          snprintf (p->status, sizeof p->status, "Out of memory");
          return;
        }
      for (int i = 0; i < subs.n; i++)
        ids[i] = subs.v[i].id;
      p->seen = subs.seen;
      snprintf (p->status, sizeof p->status, "Reading the channels: 0 of %d...", subs.n);
      memset (&p->feed_res, 0, sizeof p->feed_res);
      p->op = api_feed (ids, subs.n, p->seen, FEED_MAX, &p->list, list_done, feed_progress, p, &subs, &p->feed_res);
      free (ids);
      if (!p->op && !strncmp (p->status, "Reading the channels: 0", 23))
        snprintf (p->status, sizeof p->status, "Out of memory");
      return;
    }
  p->op = api_list (p->kind, p->arg, 0, p->date, &p->list, list_done, p);
  if (!p->op && !p->list.n && !strcmp (p->status, doing))
    snprintf (p->status, sizeof p->status, "Out of memory");
}

/* Shows a new page, fetching its list. */
static void go (int kind, const char *arg, const char *heading)
{
  page *p = calloc (1, sizeof *p);
  int b[9];
  check_proxy ();
  if (!p || !(p->arg = strdup (arg)))
    {
      free (p);
      ui_report ("Out of memory");
      return;
    }
  p->kind = kind;
  p->date = kind == YT_LIST_SEARCH ? search_date : 0;
  if (heading && p->date)
    {
      char h[300];
      snprintf (h, sizeof h, "%s (%s)", heading, yt_date_name (p->date));
      p->heading = strdup (h);
    }
  else
    p->heading = heading ? strdup (heading) : NULL;
  /* remember where the old page was scrolled to */
  if (cur ())
    {
      b[0] = win;
      if (!_swix (Wimp_GetWindowState, _IN (1), b))
        cur ()->scroll_y = b[6];
      api_cancel (cur ()->op);
      cur ()->op = NULL;
    }
  if (depth == MAX_PAGES)
    {
      free_page (pages[0]);
      memmove (pages, pages + 1, (MAX_PAGES - 1) * sizeof pages[0]);
      depth--;
    }
  pages[depth++] = p;
  thumbs_cancel_pending ();
  fetch (p, kind == YT_LIST_SEARCH ? "Searching..." : "Loading...");
  set_status ("%s", p->status);
  set_title ();
  set_extent ();
  if (!win_open)
    show_window (0);
  scroll_to (0);
  redraw_all ();
}

static void go_back (void)
{
  page *p;
  if (depth <= 1)
    return;
  free_page (pages[--depth]);
  p = cur ();
  thumbs_cancel_pending ();
  if (p->kind == YT_LIST_SEARCH)
    show_search_date (p->date);           /* the filter goes with the results shown */
  set_status ("%s", p->status);
  set_title ();
  set_extent ();
  /* a page left before its list arrived: ask again */
  if (!p->list.n && !p->op)
    {
      check_proxy ();
      fetch (p, "Loading...");
      set_status ("%s", p->status);
    }
  scroll_to (p->scroll_y);
  redraw_all ();
}

/* Refresh: the page on screen fetched again from the start (a live
   stream that has begun, new videos on a channel, new results). */
static void refresh (void)
{
  page *p = cur ();
  if (!p)
    return;
  check_proxy ();
  api_cancel (p->op);
  p->op = NULL;
  yt_list_free (&p->list);
  memset (&p->list, 0, sizeof p->list);
  free (p->text);
  p->text = NULL;
  p->text_cap = 0;
  wrap_gen++;
  thumbs_cancel_pending ();
  fetch (p, "Refreshing...");
  set_status ("%s", p->status);
  set_extent ();
  scroll_to (0);
  redraw_all ();
}

static void more (void)
{
  page *p = cur ();
  if (!p || p->op || !p->list.continuation)
    return;
  check_proxy ();
  p->op = api_list (p->kind, p->list.continuation, 1, p->date, &p->list, list_done, p);
  if (p->op)
    {
      set_status ("Loading more...");
      redraw_tile (p->list.n);
    }
}

/* A YouTube address typed or pasted in: its video ID (11 characters). */
static int video_id_in (const char *q, char *id)
{
  static const char *const marks[] = { "v=", "youtu.be/", "/shorts/", "/live/", "/embed/", NULL };
  for (int m = 0; marks[m]; m++)
    {
      const char *s = strstr (q, marks[m]);
      int n = 0;
      if (!s)
        continue;
      s += strlen (marks[m]);
      while (n < 11 && ((s[n] >= 'A' && s[n] <= 'Z') || (s[n] >= 'a' && s[n] <= 'z')
                        || (s[n] >= '0' && s[n] <= '9') || s[n] == '-' || s[n] == '_'))
        n++;
      if (n == 11)
        {
          memcpy (id, s, 11);
          id[11] = 0;
          return 1;
        }
    }
  return 0;
}

static void play (const yt_item *it);

static void search (void)
{
  char *q = field_buf, id[12];
  while (*q == ' ')
    q++;
  if (!*q)
    return;
  if (video_id_in (q, id))
    {
      /* an address: play that video, and show what's related to it */
      yt_item it;
      memset (&it, 0, sizeof it);
      it.kind = YT_VIDEO;
      strcpy (it.id, id);
      go (YT_LIST_RELATED, id, NULL);
      play (&it);
      return;
    }
  go (YT_LIST_SEARCH, q, q);
}

/* ---- playing ------------------------------------------------------------ */

/* Wimp_StartTask, with UnixLib's VFP context switched off around it (as
   riscos-ffmpeg's ffplay does): otherwise VFPSupport may later save it
   lazily into the new task's memory, which is at the same address. */
static _kernel_oserror *start_task (const char *cmd, int *handle)
{
#ifdef __arm__
  _kernel_oserror *err;
  int h = 0;
  __asm__ volatile (
    "mov    r0, #0\n\t"
    "mov    r1, #0\n\t"
    "swi    0x78EC3\n\t"          /* XVFPSupport_ChangeContext 0: off */
    "movvs  r0, #0\n\t"
    "mov    r4, r0\n\t"           /* the old context */
    "mov    r0, %2\n\t"
    "swi    0x600DE\n\t"          /* XWimp_StartTask */
    "movvs  %0, r0\n\t"
    "movvc  %0, #0\n\t"
    "movvc  %1, r0\n\t"           /* the new task's handle (0: it has finished) */
    "movs   r0, r4\n\t"
    "mov    r1, #0\n\t"
    "swine  0x78EC3\n\t"          /* back on */
    : "=&r" (err), "=&r" (h) : "r" (cmd) : "r0", "r1", "r2", "r3", "r4", "r14", "cc", "memory");
  if (handle)
    *handle = h;
  return err;
#else   /* the tests on Linux (fake RISC OS) */
  int h = 0;
  _kernel_oserror *e = _swix (Wimp_StartTask, _IN (0) | _OUT (0), cmd, &h);
  if (handle)
    *handle = h;
  return e;
#endif
}

/* ---- the JavaScript helper (the TV fallback's challenges) ------------------

   The solve runs in tscli, started as a task of its own (the Solve Obey
   file sets its WimpSlot): its memory (about 110 MB the first time for a
   new YouTube player) is its own and freed when it ends, and it polls the
   Wimp while it works. One at a time; it ends when the answer is written. */

#define Message_TaskCloseDown 0x400C3

static char ts_work[400], ts_cache[440], ejs_bundled[400], ejs_state[400];

static struct {
  int running;                /* the task is there (until Message_TaskCloseDown) */
  int task;
  int answered;               /* done has been called for this job */
  void (*done) (void *);
  void *ctx;
  char out[400];
} helper;

static struct { int set; char job[400], out[400]; void (*done) (void *); void *ctx; } pending;

static int file_exists (const char *path)
{
  int type = 0;
  if (_swix (OS_File, _INR (0, 1) | _OUT (0), 17, path, &type))
    return 0;
  return type == 1;
}

static void helper_answer (void)
{
  void (*done) (void *) = helper.done;
  if (helper.answered)
    return;
  helper.answered = 1;
  helper.done = NULL;
  if (done)
    done (helper.ctx);
}

static int helper_start (const char *job, const char *out, void (*done) (void *), void *ctx)
{
  char cmd[1000];
  const char *dir = getenv ("TubeSearch$Dir");
  int handle = 0;
  if (!dir)
    return -1;
  snprintf (cmd, sizeof cmd, "/%s.Solve %s %s", dir, job, out);
  memset (&helper, 0, sizeof helper);
  helper.done = done;
  helper.ctx = ctx;
  snprintf (helper.out, sizeof helper.out, "%s", out);
  if (start_task (cmd, &handle))
    return -1;
  helper.task = handle;
  helper.running = handle != 0;
  if (!helper.running)
    helper_answer ();                       /* (it has finished already) */
  return 0;
}

static void helper_closed (void)
{
  helper.running = 0;
  helper_answer ();                         /* (without an answer file: solve_done says so) */
  if (pending.set)
    {
      pending.set = 0;
      if (helper_start (pending.job, pending.out, pending.done, pending.ctx))
        pending.done (pending.ctx);
    }
}

static int tv_solve (const char *job, const char *out, void (*done) (void *), void *ctx)
{
  if (helper.running)
    {
      /* one helper at a time (each may need 110 MB): this one next */
      pending.set = 1;
      snprintf (pending.job, sizeof pending.job, "%s", job);
      snprintf (pending.out, sizeof pending.out, "%s", out);
      pending.done = done;
      pending.ctx = ctx;
      return 0;
    }
  return helper_start (job, out, done, ctx);
}

static void tv_solve_cancel (void *ctx)
{
  if (pending.set && pending.ctx == ctx)
    pending.set = 0;
  if (helper.ctx == ctx && !helper.answered)
    {
      int msg[5] = { 20, 0, 0, 0, Message_Quit };
      helper.answered = 1;                  /* (its answer isn't wanted) */
      helper.done = NULL;
      if (helper.running)
        _swix (Wimp_SendMessage, _INR (0, 2), 17, msg, helper.task);
    }
}

static void tv_progress (const char *what, void *ctx)
{
  (void) ctx;
  set_status ("%s", what);
}

static const api_tv_host tv_host = {
  ts_work, ts_cache, ejsupd_scripts, tv_solve, tv_solve_cancel, ejsupd_failed, tv_progress
};

/* Null events while the helper works: its answer may be there. */
static void helper_poll (void)
{
  if (helper.running && !helper.answered && file_exists (helper.out))
    helper_answer ();
}

static void make_dir (const char *path)
{
  _swix (OS_File, _INR (0, 1) | _IN (4), 8, path, 0);
}

static void tv_init (void)
{
  const char *scrap = getenv ("Wimp$ScrapDir"), *dir = getenv ("TubeSearch$Dir"),
    *choices = getenv ("Choices$Write");
  if (!scrap || !dir)
    return;                                 /* (no TV fallback) */
  snprintf (ts_work, sizeof ts_work, "%s.TubeSearch", scrap);
  snprintf (ts_cache, sizeof ts_cache, "%s.Players", ts_work);
  make_dir (ts_work);
  make_dir (ts_cache);
  snprintf (ejs_bundled, sizeof ejs_bundled, "%s.Resources.ejs", dir);
  if (choices)
    {
      char p[400];
      snprintf (p, sizeof p, "%s.TubeSearch", choices);
      make_dir (p);
      snprintf (ejs_state, sizeof ejs_state, "%s.TubeSearch.ejs", choices);
      make_dir (ejs_state);
    }
  ejsupd_init (ejs_bundled, ejs_state[0] ? ejs_state : NULL);
  api_set_tv_host (&tv_host);
  ejsupd_check (0, NULL, NULL);             /* (once a day) */
}

static void play_done (yt_play *pl, const char *err, void *ctx)
{
  char path[256], cmd[320], msg[300];
  const char *scrap = getenv ("Wimp$ScrapDir");
  char *json;
  (void) ctx;
  play_op = NULL;
  play_answered = 1;
  if (!pl)
    {
      set_status ("%s", err);
      ui_report (err);
      return;
    }
  if (!(json = yt_reel_json (pl, play_id)))
    {
      ui_report ("Out of memory");
      return;
    }
  play_file_n = play_file_n % 4 + 1;
  snprintf (path, sizeof path, "%s.TubeSearchPlay%d", scrap && *scrap ? scrap : "<Wimp$ScrapDir>", play_file_n);
  if (_swix (OS_File, _INR (0, 2) | _INR (4, 5), 10, path, 0xFFF, json, json + strlen (json)))
    {
      snprintf (msg, sizeof msg, "Couldn't write %s", path);
      ui_report (msg);
      free (json);
      return;
    }
  free (json);
  if (getenv ("Alias$Reel"))
    snprintf (cmd, sizeof cmd, "Reel %s", path);
  else
    {
      ui_report ("Reel isn't known yet: open the directory that has !Reel in it "
                 "(so the Filer sees it), then try again.");
      set_status ("Reel isn't known yet");
      return;
    }
  if (fail (start_task (cmd, NULL)))
    return;
  if (pl->audio_url && pl->audio.lang[0])
    set_status ("Playing %dp%s in Reel, sound: %s%s", pl->quality, pl->live ? " (live)" : "",
                yt_lang_name (pl->audio.lang), pl->audio.original ? " (original)" : pl->audio.dubbed ? " (dubbed)" : "");
  else
    set_status ("Playing %dp%s in Reel", pl->quality, pl->live ? " (live)" : "");
}

static void play (const yt_item *it)
{
  if (it->kind != YT_VIDEO)
    return;
  api_op *op;
  api_cancel (play_op);
  play_op = NULL;
  check_proxy ();
  snprintf (play_id, sizeof play_id, "%s", it->id);
  set_status ("Finding the video's streams...");
  play_answered = 0;
  op = api_play (it->id, quality, play_done, NULL);
  if (!op && !play_answered)
    set_status ("Out of memory");
  play_op = op;
}

/* ---- menus -------------------------------------------------------------- */

typedef struct { int flags, submenu, icon_flags; int data[3]; } menu_item;
typedef struct { char title[12]; unsigned char colours[4]; int width, height, gap; menu_item item[10]; } menu_block;

static menu_block quality_menu, bar_menu, tile_menu, date_menu, sound_menu, fps_menu;

static void data_load (int *b);
static void menu_init (menu_block *m, const char *title, int width);
static void menu_add (menu_block *m, int i, const char *text, int submenu, int flags);

/* ---- the menus' choices, kept ---------------------------------------------

   Quality, Frame rate, Sound and Uploaded are written to
   <Choices$Write>.TubeSearch.Choices whenever one changes, and read after
   the Config files when TubeSearch starts, so they win over them. */
static void choices_path (char *out, size_t size)
{
  const char *c = getenv ("Choices$Write");
  if (!c)
    {
      out[0] = 0;
      return;
    }
  snprintf (out, size, "%s.TubeSearch", c);
  mkdir (out, 0777);
  snprintf (out, size, "%s.TubeSearch.Choices", c);
}

static void save_choices (void)
{
  char path[300];
  FILE *f;
  choices_path (path, sizeof path);
  if (!path[0] || !(f = fopen (path, "w")))
    return;
  fprintf (f, "# TubeSearch's menu choices, saved as you make them (read after Config)\n"
           "max_quality = %d\nmax_fps = %s\naudio_language = %s\nsearch_date = %s\n",
           quality, yt_config_get ("max_fps"), yt_config_get ("audio_language"), yt_date_word (search_date));
  fclose (f);
}

static void set_quality (int i)
{
  static const int q[4] = { 360, 480, 720, 1080 };
  char v[8];
  if (i < 0 || i > 3)
    return;
  quality = q[i];
  snprintf (v, sizeof v, "%d", quality);
  yt_config_set ("max_quality", v);
  save_choices ();
}

/* Frame rate: the most frames a second to ask for */
static void build_fps_menu (void)
{
  int f = atoi (yt_config_get ("max_fps"));
  menu_init (&fps_menu, "Frame rate", 260);
  menu_add (&fps_menu, 0, "30 a second", 0, f <= 30 ? 1 : 0);
  menu_add (&fps_menu, 1, "60 a second", 0, (f > 30 ? 1 : 0) | 0x80);
}

static void choose_fps (int i)
{
  if (i == 0 || i == 1)
    {
      yt_config_set ("max_fps", i == 0 ? "30" : "60");
      save_choices ();
    }
}
static enum { M_NONE, M_ICONBAR, M_TILE, M_DATE } menu_open;
static int menu_tile = -1;
static char tile_menu_channel[200];

static void menu_init (menu_block *m, const char *title, int width)
{
  memset (m, 0, sizeof *m);
  snprintf (m->title, sizeof m->title, "%s", title);
  m->colours[0] = 7; m->colours[1] = 2; m->colours[2] = 7; m->colours[3] = 0;
  m->width = width;
  m->height = 44;
  m->gap = 0;
}

static void menu_add (menu_block *m, int i, const char *text, int submenu, int flags)
{
  menu_item *it = &m->item[i];
  it->flags = flags;
  it->submenu = submenu ? submenu : -1;
  it->icon_flags = 0x07000021;                      /* text, filled; black on white */
  memset (it->data, 0, sizeof it->data);
  strncpy ((char *) it->data, text, 12);
}

static void menu_add_ind (menu_block *m, int i, char *text, int size, int flags)
{
  menu_item *it = &m->item[i];
  it->flags = flags;
  it->submenu = -1;
  it->icon_flags = 0x07000121;                      /* indirected */
  it->data[0] = (int) text;
  it->data[1] = -1;
  it->data[2] = size;
}

static void build_quality_menu (void)
{
  static const int q[4] = { 360, 480, 720, 1080 };
  static const char *const names[4] = { "360p", "480p", "720p", "1080p" };
  menu_init (&quality_menu, "Quality", 200);
  for (int i = 0; i < 4; i++)
    menu_add (&quality_menu, i, names[i], 0, (q[i] == quality ? 1 : 0) | (i == 3 ? 0x80 : 0));
}

/* Sound: the track in my language (Config's language), or the original */
static void build_sound_menu (void)
{
  const char *a = yt_config_get ("audio_language");
  menu_init (&sound_menu, "Sound", 300);
  menu_add (&sound_menu, 0, "My language", 0, !strcmp (a, "mine") ? 1 : 0);
  menu_add (&sound_menu, 1, "Original", 0, (!strcmp (a, "original") ? 1 : 0) | 0x80);
}

static void choose_sound (int i)
{
  if (i == 0 || i == 1)
    {
      yt_config_set ("audio_language", i == 0 ? "mine" : "original");
      save_choices ();
    }
}

static void build_date_menu (void)
{
  menu_init (&date_menu, "Uploaded", 260);
  for (int i = 0; i < YT_DATE_COUNT; i++)
    menu_add (&date_menu, i, yt_date_name (i), 0, (i == search_date ? 1 : 0) | (i == YT_DATE_COUNT - 1 ? 0x80 : 0));
}

/* The upload-date filter's menu, beside its pop-up button (as RISC OS's
   pop-up menus open). */
static void open_date_menu (void)
{
  int st[9], ic[10];
  st[0] = bar;
  ic[0] = bar; ic[1] = I_DATEPOP;
  if (_swix (Wimp_GetWindowState, _IN (1), st) || _swix (Wimp_GetIconState, _IN (1), ic))
    return;
  build_date_menu ();
  menu_open = M_DATE;
  fail (_swix (Wimp_CreateMenu, _INR (1, 3), &date_menu,
               st[1] - st[5] + ic[4] + 4, st[4] - st[6] + ic[5]));
}

/* A new upload-date filter: shown in the toolbar, and the search on
   screen is done again with it (Back returns to the one before). */
static void set_search_date (int date)
{
  page *p = cur ();
  if (date < 0 || date >= YT_DATE_COUNT)
    return;
  show_search_date (date);
  yt_config_set ("search_date", yt_date_word (date));
  save_choices ();
  if (p && p->kind == YT_LIST_SEARCH && p->date != date)
    go (YT_LIST_SEARCH, p->arg, p->arg);
}

static void open_iconbar_menu (int x)
{
  build_quality_menu ();
  build_fps_menu ();
  build_sound_menu ();
  menu_init (&bar_menu, APP_NAME, 240);
  menu_add (&bar_menu, 0, "Info", info, 0);             /* (a window handle: the Wimp opens it) */
  menu_add (&bar_menu, 1, "Search...", 0, 0);
  {
    static char subs_item[] = "Subscriptions";               /* (over 12 letters: indirected) */
    menu_add_ind (&bar_menu, 2, subs_item, sizeof subs_item, 0);
  }
  menu_add (&bar_menu, 3, "My channels", 0, 2);
  menu_add (&bar_menu, 4, "Quality", (int) &quality_menu, 0);
  menu_add (&bar_menu, 5, "Frame rate", (int) &fps_menu, 0);
  menu_add (&bar_menu, 6, "Sound", (int) &sound_menu, 2 /* dotted line */);
  menu_add (&bar_menu, 7, "Quit", 0, 0x80);
  menu_open = M_ICONBAR;
  fail (_swix (Wimp_CreateMenu, _INR (1, 3), &bar_menu, x - 64, 96 + 8 * 44 + 2 * 24));
}

/* The window menu's Subscribe/Unsubscribe: which channel, and where things are */
static struct { char id[40]; char name[200]; char thumb[400]; } menu_sub;
static struct { int sub, back, refresh, quality; } menu_at;

static void sub_choice (const char *id, const char *name, const char *thumb)
{
  if (!id || !subs_is_channel_id (id, strlen (id)))
    {
      menu_sub.id[0] = 0;
      return;
    }
  snprintf (menu_sub.id, sizeof menu_sub.id, "%s", id);
  snprintf (menu_sub.name, sizeof menu_sub.name, "%s", name ? name : "");
  snprintf (menu_sub.thumb, sizeof menu_sub.thumb, "%s", thumb ? thumb : "");
}

/* Subscribe or Unsubscribe from the menu */
static void toggle_sub (void)
{
  page *p = cur ();
  if (!menu_sub.id[0])
    return;
  if (subs_find (&subs, menu_sub.id) >= 0)
    {
      subs_remove (&subs, menu_sub.id);
      set_status ("Unsubscribed from %s", menu_sub.name);
    }
  else
    {
      char *u = to_utf8 (menu_sub.name);
      if (!u || subs_add (&subs, menu_sub.id, u, menu_sub.thumb) < 0)
        ui_report ("Out of memory");
      else
        set_status ("Subscribed to %s (%d channel%s)", menu_sub.name, subs.n, subs.n == 1 ? "" : "s");
      free (u);
    }
  subs_store ();
  if (p && p->kind == YT_LIST_MYCHANNELS)
    refresh ();                                          /* (the list shown changes) */
}

static void open_tile_menu (int i, int x, int y)
{
  page *p = cur ();
  const yt_item *it;
  int n = 0;
  build_quality_menu ();
  build_fps_menu ();
  build_sound_menu ();
  menu_tile = i;
  if (p && i >= 0 && i < p->list.n)
    {
      it = &p->list.items[i];
      menu_init (&tile_menu, it->kind == YT_CHANNEL ? "Channel" : "Video", 640);
      if (it->kind == YT_VIDEO)
        {
          menu_add (&tile_menu, n++, "Play", 0, 0);
          menu_add (&tile_menu, n++, "Related", 0, 0);
          snprintf (tile_menu_channel, sizeof tile_menu_channel, "Channel: %s", it->channel);
          menu_add_ind (&tile_menu, n++, tile_menu_channel, sizeof tile_menu_channel, 0);
          if (!it->channel_id[0])
            tile_menu.item[n - 1].icon_flags |= 1 << 22;      /* shaded */
          sub_choice (it->channel_id, it->channel, "");
        }
      else
        {
          menu_add (&tile_menu, n++, "Open", 0, 0);
          sub_choice (it->id, it->title, it->thumb_url);
        }
    }
  else
    {
      menu_tile = -1;
      menu_init (&tile_menu, APP_NAME, 300);
      /* a channel's own page: that channel */
      if (p && p->kind == YT_LIST_CHANNEL)
        sub_choice (p->arg, p->list.title ? p->list.title : p->heading ? p->heading : "", "");
      else
        menu_sub.id[0] = 0;
    }
  menu_at.sub = -1;
  if (menu_sub.id[0])
    {
      menu_at.sub = n;
      menu_add (&tile_menu, n++, subs_find (&subs, menu_sub.id) >= 0 ? "Unsubscribe" : "Subscribe", 0, 2);
    }
  else if (n)
    tile_menu.item[n - 1].flags |= 2;                      /* (the dotted line) */
  menu_at.back = n;
  menu_add (&tile_menu, n++, "Back", 0, 0);
  if (depth <= 1)
    tile_menu.item[n - 1].icon_flags |= 1 << 22;
  menu_at.refresh = n;
  menu_add (&tile_menu, n++, "Refresh", 0, 0);
  if (!depth)
    tile_menu.item[n - 1].icon_flags |= 1 << 22;
  menu_at.quality = n;
  menu_add (&tile_menu, n++, "Quality", (int) &quality_menu, 0);
  menu_add (&tile_menu, n++, "Frame rate", (int) &fps_menu, 0);
  menu_add (&tile_menu, n++, "Sound", (int) &sound_menu, 0x80);
  menu_open = M_TILE;
  fail (_swix (Wimp_CreateMenu, _INR (1, 3), &tile_menu, x - 64, y));
}

static void open_channel (const yt_item *it)
{
  if (it->channel_id[0])
    go (YT_LIST_CHANNEL, it->channel_id, it->kind == YT_CHANNEL ? it->title : it->channel);
}

static int menu_selection (const int *sel)
{
  int adjust = 0;
  {
    int ptr[5];
    if (!_swix (Wimp_GetPointerInfo, _IN (1), ptr))
      adjust = ptr[2] & Button_Adjust;
  }
  if (menu_open == M_DATE)
    {
      if (sel[0] >= 0)
        set_search_date (sel[0]);
      if (adjust)
        {
          build_date_menu ();
          _swix (Wimp_CreateMenu, _INR (1, 3), &date_menu, 0, 0);
          return 0;
        }
    }
  else if (menu_open == M_ICONBAR)
    {
      if (sel[0] == 1)
        show_window (1);
      else if (sel[0] == 2)
        go (YT_LIST_SUBS, "subscriptions", "Subscriptions");
      else if (sel[0] == 3)
        go (YT_LIST_MYCHANNELS, "channels", "My channels");
      else if (sel[0] == 4 && sel[1] >= 0)
        set_quality (sel[1]);
      else if (sel[0] == 5 && sel[1] >= 0)
        choose_fps (sel[1]);
      else if (sel[0] == 6 && sel[1] >= 0)
        choose_sound (sel[1]);
      else if (sel[0] == 7)
        return 1;
      if (adjust)
        {
          build_quality_menu ();
          build_fps_menu ();
          build_sound_menu ();
          _swix (Wimp_CreateMenu, _INR (1, 3), &bar_menu, 0, 0);
          return 0;
        }
    }
  else if (menu_open == M_TILE)
    {
      page *p = cur ();
      const yt_item *it = p && menu_tile >= 0 && menu_tile < p->list.n ? &p->list.items[menu_tile] : NULL;
      int i = sel[0], q = menu_at.quality;
      if (it && it->kind == YT_VIDEO)
        {
          if (i == 0)
            play (it);
          else if (i == 1)
            go (YT_LIST_RELATED, it->id, NULL);
          else if (i == 2)
            open_channel (it);
        }
      else if (it && i == 0)
        open_channel (it);
      if (i == menu_at.sub && i >= 0)
        toggle_sub ();
      else if (i == menu_at.back)
        go_back ();
      else if (i == menu_at.refresh)
        refresh ();
      else if (i == q && sel[1] >= 0)
        set_quality (sel[1]);
      else if (i == q + 1 && sel[1] >= 0)
        choose_fps (sel[1]);
      else if (i == q + 2 && sel[1] >= 0)
        choose_sound (sel[1]);
      if (adjust && i >= q && i <= q + 2)
        {
          build_quality_menu ();
          build_fps_menu ();
          build_sound_menu ();
          _swix (Wimp_CreateMenu, _INR (1, 3), &tile_menu, 0, 0);
          return 0;
        }
    }
  menu_open = M_NONE;
  return 0;
}

/* ---- clicks ------------------------------------------------------------- */

/* The tile under screen (x, y), and which part: 0 picture/title, 1 the
   channel line. -1 for none. */
static int tile_at (int x, int y, int *part)
{
  int b[9], wx, wy, col, row, i, x0, y1;
  page *p = cur ();
  b[0] = win;
  if (!p || _swix (Wimp_GetWindowState, _IN (1), b))
    return -1;
  wx = x - (b[1] - b[5]);
  wy = y - (b[4] - b[6]);
  if (wy > -TOOLBAR_H - PAD || wx < PAD)
    return -1;
  col = (wx - PAD) / TILE_W;
  row = (-wy - TOOLBAR_H - PAD) / TILE_H;
  if (col >= cols)
    return -1;
  i = row * cols + col;
  if (i >= tiles (p))
    return -1;
  tile_box (i, &x0, &y1);
  if (wx > x0 + TILE_W - 20 || wy < y1 - TILE_H + 12)
    return -1;
  *part = wy < y1 - 20 - THUMB_H - 8 - 2 * LINE_H && wy >= y1 - 20 - THUMB_H - 8 - 3 * LINE_H;
  return i;
}

static void click_main (const int *b)
{
  int part = 0, i = tile_at (b[0], b[1], &part);
  page *p = cur ();
  if (b[2] == Button_Menu)
    {
      open_tile_menu (i, b[0], b[1]);
      return;
    }
  if (i < 0 || !p)
    return;
  if (i >= p->list.n)
    {
      more ();
      return;
    }
  {
    const yt_item *it = &p->list.items[i];
    if (it->kind == YT_CHANNEL || (part == 1 && it->channel_id[0]))
      open_channel (it);
    else if (b[2] == Button_Select)
      play (it);
    else if (b[2] == Button_Adjust)
      go (YT_LIST_RELATED, it->id, NULL);
  }
}

static void click_bar (const int *b)
{
  if (b[2] == Button_Menu)
    {
      open_tile_menu (-1, b[0], b[1]);
      return;
    }
  if (b[4] == I_BACK)
    go_back ();
  else if (b[4] == I_REFRESH)
    refresh ();
  else if (b[4] == I_SUBS)
    go (b[2] == Button_Adjust ? YT_LIST_MYCHANNELS : YT_LIST_SUBS, b[2] == Button_Adjust ? "channels" : "subscriptions",
        b[2] == Button_Adjust ? "My channels" : "Subscriptions");
  else if (b[4] == I_SEARCH)
    search ();
  else if (b[4] == I_DATE || b[4] == I_DATEPOP)
    open_date_menu ();
}

/* ---- set up, events ----------------------------------------------------- */

static void picture_arrived (const char *url)
{
  page *p = cur ();
  if (!p || !win_open)
    return;
  for (int i = 0; i < p->list.n; i++)
    if (!strcmp (p->list.items[i].thumb_url, url))
      redraw_tile (i);
}

static int create_iconbar_icon (void)
{
  int b[9];
  memset (b, 0, sizeof b);
  b[0] = -1;
  b[1] = 0; b[2] = 0; b[3] = 68; b[4] = 68;
  b[5] = 0x301A;
  memcpy (&b[6], "!tubesearch", 12);
  return fail (_swix (Wimp_CreateIcon, _INR (0, 1), 0, b));
}

int ui_init (void)
{
  static const int messages[] = { Message_DataLoad, Message_ModeChange, Message_FontChanged, Message_TaskCloseDown, 0 };
  char err[200];
  int alphabet = 0;
  const char *dir = getenv ("TubeSearch$Dir");
  if (fail (_swix (Wimp_Initialise, _INR (0, 3) | _OUT (1), 380, TASK_WORD, APP_NAME, messages, &task)))
    return 1;
  if (transport_check (err, sizeof err))
    {
      ui_report (err);
      return 1;
    }
  /* the settings: !TubeSearch's own Config, then the user's in Choices */
  {
    char path[300];
    const char *choices = getenv ("Choices$Write");
    if (dir)
      {
        snprintf (path, sizeof path, "%s.Config", dir);
        yt_config_load (path);
      }
    if (choices)
      {
        snprintf (path, sizeof path, "%s.TubeSearch.Config", choices);
        yt_config_load (path);
      }
    choices_path (path, sizeof path);           /* the menus' choices, last */
    if (path[0])
      yt_config_load (path);
    if (choices)
      {
        snprintf (subs_path, sizeof subs_path, "%s.TubeSearch.Subscriptions", choices);
        if (subs_load (&subs, subs_path))
          ui_report ("The subscriptions couldn't be read");
      }
  }
  quality = atoi (yt_config_get ("max_quality"));
  if (quality < 144)
    quality = 1080;
  search_date = yt_date_parse (yt_config_get ("search_date"));
  if (search_date < 0)
    search_date = YT_DATE_ANY;
  snprintf (date_buf, sizeof date_buf, "%s", yt_date_name (search_date));
  check_proxy ();
  if (!_swix (OS_Byte, _INR (0, 1) | _OUT (1), 71, 127, &alphabet))
    text_utf8_desktop = alphabet == 111;
  read_screen ();
  http_init (&transport_system);
  thumbs_init (picture_arrived);
  tv_init ();
  return create_iconbar_icon () || create_main () || create_bar () || create_info ();
}

int ui_busy (void)
{
  return http_busy () || helper.running;
}

int ui_event (int reason, int *b)
{
  switch (reason)
    {
    case Null_Reason:
      http_poll ();
      helper_poll ();
      break;
    case Redraw_Window_Request:
      if (b[0] == win)
        redraw (b);
      break;
    case Open_Window_Request:
      if (b[0] == win)
        open_main (b);
      else
        _swix (Wimp_OpenWindow, _IN (1), b);
      break;
    case Close_Window_Request:
      if (b[0] == win)
        {
          _swix (Wimp_CloseWindow, _IN (1), b);
          win_open = 0;
        }
      break;
    case Mouse_Click:
      if (b[3] == -2)
        {
          if (b[2] == Button_Menu)
            open_iconbar_menu (b[0]);
          else
            show_window (1);
        }
      else if (b[3] == win)
        click_main (b);
      else if (b[3] == bar)
        click_bar (b);
      break;
    case Key_Pressed:
      if (b[0] == bar && b[1] == I_FIELD && b[6] == 13)
        search ();
      else if (b[6] == 0x185)               /* F5: Refresh */
        refresh ();
      else if (b[6] == 0x1B && b[0] == bar)
        ;                                   /* Escape in the box: nothing */
      else
        _swix (Wimp_ProcessKey, _IN (0), b[6]);
      break;
    case Menu_Selection:
      return menu_selection (b);
    case User_Message:
    case User_Message_Recorded:
      if (b[4] == Message_Quit)
        return 1;
      if (b[4] == Message_TaskCloseDown && helper.running && b[1] == helper.task)
        helper_closed ();
      if (b[4] == Message_DataLoad && (b[5] == win || b[5] == bar || b[5] == -2))
        data_load (b);
      if (b[4] == Message_ModeChange || b[4] == Message_FontChanged)
        {
          read_screen ();
          wrap_gen++;
          if (win_open)
            {
              set_extent ();
              redraw_all ();
            }
        }
      break;
    }
  return 0;
}

/* A file dropped on the window or the icon bar icon: a list of
   subscriptions to bring in (subs_import). */
static void data_load (int *b)
{
  const char *name = (const char *) &b[11];
  FILE *f = fopen (name, "rb");
  char *text = NULL, msg[300];
  long n = 0;
  int found = 0, added;
  if (f && !fseek (f, 0, SEEK_END) && (n = ftell (f)) >= 0 && n < 8 * 1024 * 1024 && !fseek (f, 0, SEEK_SET)
      && (text = malloc ((size_t) n + 1)) && fread (text, 1, (size_t) n, f) == (size_t) n)
    text[n] = 0;
  else
    {
      free (text);
      text = NULL;
    }
  if (f)
    fclose (f);
  if (!text)
    {
      ui_report ("That file couldn't be read");
      return;
    }
  added = subs_import (&subs, text, (size_t) n, &found);
  free (text);
  if (added < 0)
    {
      ui_report ("That isn't a list of subscriptions TubeSearch knows: it reads Google Takeout's "
                 "subscriptions.csv, and NewPipe's and FreeTube's exports");
      return;
    }
  b[3] = b[2];                                    /* DataLoadAck: taken */
  b[4] = Message_DataLoadAck;
  _swix (Wimp_SendMessage, _INR (0, 2), 17, b, b[1]);
  subs_store ();
  snprintf (msg, sizeof msg, "%d channel%s added from the %d in that list (%d subscriptions now)", added,
            added == 1 ? "" : "s", found, subs.n);
  if (cur () && cur ()->kind == YT_LIST_MYCHANNELS)
    refresh ();                                          /* (showing them already) */
  else
    go (YT_LIST_MYCHANNELS, "channels", "My channels");
  set_status ("%s", msg);
}

void ui_close (void)
{
  while (depth)
    free_page (pages[--depth]);
  api_cancel (play_op);
  thumbs_free_all ();
  http_shutdown ();
  _swix (Wimp_CloseDown, _INR (0, 1), task, TASK_WORD);
}
