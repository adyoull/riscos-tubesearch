/* tscli - TubeSearch's YouTube code from the command line, for trying it out
   and finding faults (on RISC OS in a TaskWindow, or on Linux):

     tscli search <words...>     videos and channels found
     tscli related <video ID>    the videos YouTube suggests next to it
     tscli channel <UC... ID>    a channel's videos
     tscli play <video ID> [lines] [file]
                                   chooses the streams and writes what Reel
                                   reads (yt-dlp's JSON) to file (default:
                                   standard output); then *Reel file
   Options before the command:
     -more N      fetch N more parts of a list
     -dump DIR    keep YouTube's raw answers in DIR
     -config FILE read settings (as !TubeSearch's Config file)

   Part of TubeSearch. MIT licence. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "api.h"
#include "http.h"
#include "text.h"
#include "transport.h"
#include "yt.h"

static int finished, failed;
static char *out_json;

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
      fprintf (stderr, "%s: %dp %s%s%s\n", p->title ? p->title : id, p->quality, p->codec,
               p->audio_url ? " + AAC" : "", p->hls_url ? " (HLS)" : "");
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
  fprintf (stderr, "usage: tscli [-more N] [-dump DIR] [-config FILE]\n"
                   "               search WORDS... | related ID | channel ID | play ID [LINES] [FILE]\n");
  return 2;
}

int main (int argc, char **argv)
{
  char err[200], query[512];
  int more = 0, a = 1;
  srand ((unsigned) time (NULL));
  while (a < argc && argv[a][0] == '-')
    {
      if (!strcmp (argv[a], "-more") && a + 1 < argc)
        more = atoi (argv[a + 1]);
      else if (!strcmp (argv[a], "-dump") && a + 1 < argc)
        api_dump_dir = argv[a + 1];
      else if (!strcmp (argv[a], "-config") && a + 1 < argc)
        {
          if (yt_config_load (argv[a + 1]) < 0)
            fprintf (stderr, "tscli: can't read %s\n", argv[a + 1]);
        }
      else
        return usage ();
      a += 2;
    }
  if (a + 1 >= argc)
    return usage ();
  if (transport_check (err, sizeof err))
    {
      fprintf (stderr, "tscli: %s\n", err);
      return 1;
    }
  http_init (&transport_system);

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
      if (!api_list (kind, query, 0, &l, list_done, NULL))
        return 1;
      wait_done ();
      print_list (&l, 0);
      while (!failed && more-- > 0 && l.continuation)
        {
          int from = l.n;
          char *tok = strdup (l.continuation);
          printf ("-- more\n");
          if (!api_list (kind, tok, 1, &l, list_done, NULL))
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
