/* tscli - TubeSearch's YouTube code from the command line, for trying it out
   and finding faults (on RISC OS in a TaskWindow, or on Linux), and
   TubeSearch's JavaScript helper:

     tscli search <words...>     videos and channels found
     tscli related <video ID>    the videos YouTube suggests next to it
     tscli channel <UC... ID>    a channel's videos
     tscli play <video ID> [lines] [file]
                                   chooses the streams and writes what Reel
                                   reads (yt-dlp's JSON) to file (default:
                                   standard output); then *Reel file
     tscli update                checks for newer solver scripts now
     tscli solve <job> <answer>  answers YouTube's challenges (solvejob.h):
                                   what !TubeSearch starts as a task of its
                                   own (with -wimp) to do that
   Options before the command:
     -more N      fetch N more parts of a list
     -dump DIR    keep YouTube's raw answers in DIR
     -config FILE read settings (as !TubeSearch's Config file)
     -client C    the streams from: auto (the player client, then the TV
                  app), player, or tv
     -ejs DIR     the bundled solver scripts (lib, core, Version)
     -state DIR   where newer solver scripts are kept
     -work DIR    the TV fallback's files (default: <Wimp$ScrapDir>.TubeSearch)
     -wimp        (solve) be a desktop task, polling while it works

   Part of TubeSearch. MIT licence. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <sys/stat.h>
#include "api.h"
#include "proxy.h"
#include "ejsupd.h"
#include "http.h"
#include "solvejob.h"
#include "text.h"
#include "transport.h"
#include "yt.h"

#ifdef __riscos
#include <kernel.h>
#include <swis.h>
#include <unixlib/local.h>
/* every file name given is a RISC OS one */
int __riscosify_control = __RISCOSIFY_NO_PROCESS;
/* The solver's memory in a dynamic area: about 105-112 MB for the first
   solve of a new YouTube player, much less after. 128 MB is what RISC OS
   gives a new area anyway unless DALimit has been raised; a first solve
   that needs more gets a "TubeSearch Solver 2" from UnixLib for as long as
   the helper runs. (It reserved 256 MB on machines with a higher DALimit.) */
const char *const __dynamic_da_name = "TubeSearch Solver";
int __dynamic_da_max_size = 128 << 20;
#define SEP "."
#else
#define SEP "/"
#endif

static int finished, failed;
static char *out_json;
static char work_dir[400], cache_dir[440];

static void list_done (int added, const char *err, void *ctx)
{
  (void) ctx;
  if (added < 0)
    {
      fprintf (stderr, "tscli: %s\n", err);
      failed = 1;
    }
  finished = 1;
}

static void play_done (yt_play *p, const char *err, void *ctx)
{
  const char *id = ctx;
  if (!p)
    {
      fprintf (stderr, "tscli: %s\n", err);
      failed = 1;
    }
  else
    {
      fprintf (stderr, "%s: %dp %s%s%s", p->title ? p->title : id, p->quality, p->codec,
               p->audio_url ? " + AAC" : "", p->live ? " (live, HLS)" : p->hls ? " (HLS)" : " (DASH)");
      if (p->audio_url && p->audio.lang[0])
        fprintf (stderr, ", sound: %s%s", yt_lang_name (p->audio.lang),
                 p->audio.original ? " (original)" : p->audio.dubbed ? " (dubbed)" : "");
      fputc ('\n', stderr);
      out_json = yt_reel_json (p, id);
    }
  finished = 1;
}

static void wait_done (void)
{
  finished = 0;
  while (!finished)
    {
      if (!http_poll () && !finished)
        {
          fprintf (stderr, "tscli: nothing happening\n");
          failed = 1;
          return;
        }
      transport_wait (1);
    }
}

static void print_list (const yt_list *l, int from)
{
  if (from == 0 && l->title)
    printf ("== %s\n", l->title);
  for (int i = from; i < l->n; i++)
    {
      const yt_item *it = &l->items[i];
      if (it->kind == YT_CHANNEL)
        printf ("[channel] %s  %s  (%s)\n", it->id, it->title, it->meta);
      else
        printf ("%-11s %8s  %s\n            %s  %s\n", it->id, it->duration, it->title, it->channel, it->meta);
    }
}

static int usage (void)
{
  fprintf (stderr, "usage: tscli [-more N] [-dump DIR] [-config FILE] [-client C] [-ejs DIR] [-state DIR] [-work DIR]\n"
                   "             [-date any|today|week|month|year] [-proxy URL|none] [-sound mine|original|LANG]\n"
                   "             search WORDS... | related ID | channel ID | play ID [LINES] [FILE]\n"
                   "             | update | [-wimp] solve JOB ANSWER\n");
  return 2;
}

/* ---- the TV fallback, with the solve done here ---------------------------- */

static int inline_solve (const char *job, const char *out, void (*done) (void *), void *ctx)
{
  solvejob_run (job, out, NULL, NULL);
  done (ctx);
  return 0;
}

static void solve_cancel (void *ctx)
{
  (void) ctx;
}

static void progress (const char *what, void *ctx)
{
  (void) ctx;
  fprintf (stderr, "tscli: %s\n", what);
}

static const api_tv_host tv_host = {
  work_dir, cache_dir, ejsupd_scripts, inline_solve, solve_cancel, ejsupd_failed, progress
};

static void update_done (int updated, const char *msg, void *ctx)
{
  (void) ctx;
  fprintf (stderr, "tscli: %s\n", msg);
  finished = 1;
  (void) updated;
}

/* ---- the helper: a desktop task polling while QuickJS works ----------------- */

#ifdef __riscos
static int wimp_task;

static int wimp_tick (void *ctx)
{
  static int last;
  int now = 0, block[64], reason;
  (void) ctx;
  _swix (OS_ReadMonotonicTime, _OUT (0), &now);
  if (now - last < 4)
    return 0;
  last = now;
  /* null events and messages only: the helper has no windows */
  if (_swix (Wimp_Poll, _INR (0, 1) | _OUT (0), 0x3832, block, &reason))
    return 0;
  if ((reason == 17 || reason == 18) && block[4] == 0)
    return 1;                               /* Message_Quit: stop */
  return 0;
}

static int helper_solve (const char *job, const char *out)
{
  static const int messages[] = { 0 };
  int rc;
  if (_swix (Wimp_Initialise, _INR (0, 3) | _OUT (1), 310, 0x4B534154, "TubeSearch solver", messages, &wimp_task))
    return solvejob_run (job, out, NULL, NULL) ? 1 : 0;
  rc = solvejob_run (job, out, wimp_tick, NULL);
  _swix (Wimp_CloseDown, _INR (0, 1), wimp_task, 0x4B534154);
  return rc ? 1 : 0;
}
#endif

static void default_dirs (const char *work)
{
#ifdef __riscos
  const char *scrap = getenv ("Wimp$ScrapDir");
  snprintf (work_dir, sizeof work_dir, "%s", work ? work : "");
  if (!work_dir[0])
    snprintf (work_dir, sizeof work_dir, "%s.TubeSearch", scrap && *scrap ? scrap : "<Wimp$ScrapDir>");
#else
  snprintf (work_dir, sizeof work_dir, "%s", work ? work : getenv ("TUBESEARCH_WORK") ? getenv ("TUBESEARCH_WORK") : "/tmp/tubesearch");
#endif
  snprintf (cache_dir, sizeof cache_dir, "%s" SEP "Players", work_dir);
  mkdir (work_dir, 0777);
  mkdir (cache_dir, 0777);
}

int main (int argc, char **argv)
{
  char err[200], query[512];
  const char *ejs_dir = NULL, *state_dir = NULL, *work = NULL;
  int more = 0, a = 1, wimp = 0, date = -1;
  const char *proxy_override = NULL;
  srand ((unsigned) time (NULL));
#ifndef __riscos
  signal (SIGPIPE, SIG_IGN);           /* a connection closed under us is an error, not the end */
#endif
  while (a < argc && argv[a][0] == '-')
    {
      if (!strcmp (argv[a], "-wimp"))
        {
          wimp = 1;
          a++;
          continue;
        }
      if (a + 1 >= argc)
        return usage ();
      if (!strcmp (argv[a], "-more"))
        more = atoi (argv[a + 1]);
      else if (!strcmp (argv[a], "-dump"))
        api_dump_dir = argv[a + 1];
      else if (!strcmp (argv[a], "-config"))
        {
          if (yt_config_load (argv[a + 1]) < 0)
            fprintf (stderr, "tscli: can't read %s\n", argv[a + 1]);
        }
      else if (!strcmp (argv[a], "-client"))
        yt_config_set ("stream_client", argv[a + 1]);
      else if (!strcmp (argv[a], "-ejs"))
        ejs_dir = argv[a + 1];
      else if (!strcmp (argv[a], "-state"))
        state_dir = argv[a + 1];
      else if (!strcmp (argv[a], "-work"))
        work = argv[a + 1];
      else if (!strcmp (argv[a], "-date"))
        {
          if ((date = yt_date_parse (argv[a + 1])) < 0)
            return usage ();
        }
      else if (!strcmp (argv[a], "-proxy"))
        proxy_override = argv[a + 1];
      else if (!strcmp (argv[a], "-sound"))
        yt_config_set ("audio_language", argv[a + 1]);
      else
        return usage ();
      a += 2;
    }
  if (a >= argc)
    return usage ();

  if (!strcmp (argv[a], "solve"))
    {
      if (a + 2 >= argc)
        return usage ();
#ifdef __riscos
      if (wimp)
        return helper_solve (argv[a + 1], argv[a + 2]);
#endif
      (void) wimp;
      if (solvejob_run (argv[a + 1], argv[a + 2], NULL, NULL))
        {
          fprintf (stderr, "tscli: the solve failed (the answer file says why)\n");
          return 1;
        }
      return 0;
    }

  if (transport_check (err, sizeof err))
    {
      fprintf (stderr, "tscli: %s\n", err);
      return 1;
    }
  http_init (&transport_system);
  if (proxy_refresh (proxy_override, err, sizeof err) < 0)
    {
      fprintf (stderr, "tscli: %s\n", err);
      return 1;
    }
  if (*proxy_name ())
    fprintf (stderr, "tscli: through the proxy %s\n", proxy_name ());
  if (date < 0)
    date = yt_date_parse (yt_config_get ("search_date"));
  default_dirs (work);
#ifdef __riscos
  {
    const char *dir = getenv ("TubeSearch$Dir"), *choices = getenv ("Choices$Write");
    static char b[400], st[400];
    if (!ejs_dir && dir)
      snprintf (b, sizeof b, "%s.Resources.ejs", dir), ejs_dir = b;
    if (!state_dir && choices)
      {
        snprintf (st, sizeof st, "%s.TubeSearch", choices);
        mkdir (st, 0777);
        snprintf (st, sizeof st, "%s.TubeSearch.ejs", choices);
        mkdir (st, 0777);
        state_dir = st;
      }
  }
#endif
  ejsupd_init (ejs_dir ? ejs_dir : "ejs-bundle", state_dir);
  api_set_tv_host (&tv_host);

  if (!strcmp (argv[a], "update"))
    {
      if (!state_dir)
        {
          fprintf (stderr, "tscli: update needs -state DIR\n");
          return 2;
        }
      finished = 0;
      ejsupd_check (1, update_done, NULL);
      while (!finished && http_poll ())
        transport_wait (1);
      if (!finished)
        fprintf (stderr, "tscli: the update check didn't finish\n");
      http_shutdown ();
      return finished ? 0 : 1;
    }
  if (a + 1 >= argc)
    return usage ();

  if (!strcmp (argv[a], "play"))
    {
      const char *id = argv[a + 1];
      int maxq = a + 2 < argc ? atoi (argv[a + 2]) : 1080;
      if (!api_play (id, maxq ? maxq : 1080, play_done, (void *) id))
        return 1;
      wait_done ();
      if (!failed && out_json)
        {
          if (a + 3 < argc)
            {
              FILE *f = fopen (argv[a + 3], "w");
              if (!f || fputs (out_json, f) < 0)
                {
                  fprintf (stderr, "tscli: can't write %s\n", argv[a + 3]);
                  failed = 1;
                }
              if (f)
                fclose (f);
            }
          else
            fputs (out_json, stdout);
        }
      free (out_json);
    }
  else
    {
      yt_list l;
      int kind;
      memset (&l, 0, sizeof l);
      if (!strcmp (argv[a], "search"))
        kind = YT_LIST_SEARCH;
      else if (!strcmp (argv[a], "related"))
        kind = YT_LIST_RELATED;
      else if (!strcmp (argv[a], "channel"))
        kind = YT_LIST_CHANNEL;
      else
        return usage ();
      query[0] = 0;
      for (int i = a + 1; i < argc; i++)
        {
          if (strlen (query) + strlen (argv[i]) + 2 >= sizeof query)
            break;
          if (query[0])
            strcat (query, " ");
          strcat (query, argv[i]);
        }
      if (!api_list (kind, query, 0, date < 0 ? 0 : date, &l, list_done, NULL))
        return 1;
      wait_done ();
      print_list (&l, 0);
      while (!failed && more-- > 0 && l.continuation)
        {
          int from = l.n;
          char *tok = strdup (l.continuation);
          printf ("-- more\n");
          if (!api_list (kind, tok, 1, 0, &l, list_done, NULL))
            failed = 1;
          else
            wait_done ();
          free (tok);
          print_list (&l, from);
        }
      yt_list_free (&l);
    }
  http_shutdown ();
  return failed;
}
