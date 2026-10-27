/* Tests subs.c: the subscriptions file, importing Takeout/NewPipe/FreeTube
   lists, the channels' feeds, times, and newest-first.
     test_subs [DIR]   (DIR: tests/subs)
   Part of TubeSearch. MIT licence. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../src/subs.h"
#include "../src/text.h"

static int failed, checks;
static const char *dir = "tests/subs";
#define CHECK(c) do { checks++; if (!(c)) { failed++; printf ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define A "UCaaaaaaaaaaaaaaaaaaaaaa"
#define B "UCbbbbbbbbbbbbbbbbbbbbbb"

static char *load (const char *name, size_t *len)
{
  char path[512];
  FILE *f;
  char *b;
  long n;
  snprintf (path, sizeof path, "%s/%s", dir, name);
  if (!(f = fopen (path, "rb")))
    {
      printf ("FAIL: can't open %s\n", path);
      failed++;
      return NULL;
    }
  fseek (f, 0, SEEK_END);
  n = ftell (f);
  fseek (f, 0, SEEK_SET);
  b = malloc ((size_t) n + 1);
  *len = fread (b, 1, (size_t) n, f);
  b[*len] = 0;
  fclose (f);
  return b;
}

static void test_list (void)
{
  yt_subs s, t;
  char path[] = "/tmp/test_subs_XXXXXX";
  int fd = mkstemp (path);
  close (fd);
  memset (&s, 0, sizeof s);
  CHECK (subs_is_channel_id (A, 24) && !subs_is_channel_id ("UCshort", 7) && !subs_is_channel_id ("XXaaaaaaaaaaaaaaaaaaaaaa", 24));
  CHECK (subs_add (&s, A, "Joueur du Grenier", "https://yt3.ggpht.com/a.jpg") == 1);
  CHECK (subs_add (&s, B, "Two\tlines\nhere", "") == 1);
  CHECK (subs_add (&s, A, "", "") == 0 && !strcmp (s.v[0].name, "Joueur du Grenier"));      /* kept */
  CHECK (subs_add (&s, A, "JdG", "") == 0 && !strcmp (s.v[0].name, "JdG"));                /* renamed */
  CHECK (subs_add (&s, "@handle", "x", "") < 0 && s.n == 2);
  CHECK (subs_find (&s, B) == 1 && !strcmp (s.v[1].name, "Two lines here"));
  s.seen = 1786000000L;
  CHECK (subs_save (&s, path) == 0);
  CHECK (subs_load (&t, path) == 0 && t.n == 2 && t.seen == 1786000000L && !strcmp (t.v[0].id, A)
         && !strcmp (t.v[0].thumb, "https://yt3.ggpht.com/a.jpg") && !strcmp (t.v[1].name, "Two lines here"));
  CHECK (subs_remove (&t, A) == 1 && t.n == 1 && !strcmp (t.v[0].id, B) && subs_remove (&t, A) == 0);
  subs_free (&t);
  CHECK (subs_load (&t, "/nonexistent/subs") == 0 && t.n == 0);
  {
    yt_list l;
    memset (&l, 0, sizeof l);
    CHECK (subs_channel_list (&s, &l) == 2 && l.items[0].kind == YT_CHANNEL && !strcmp (l.items[0].id, A)
           && !strcmp (l.items[0].title, "JdG") && !strcmp (l.items[0].thumb_url, "https://yt3.ggpht.com/a.jpg"));
    yt_list_free (&l);
  }
  subs_free (&s);
  unlink (path);
}

static void test_import (void)
{
  static const char *const files[] = { "takeout_subscriptions.csv", "newpipe_subscriptions.json", "freetube_profiles.db" };
  size_t len;
  char *t;
  int found;
  yt_subs s;
  for (int i = 0; i < 3; i++)
    {
      memset (&s, 0, sizeof s);
      if (!(t = load (files[i], &len)))
        continue;
      int k = subs_import (&s, t, len, &found);
      CHECK (k == (i == 0 ? 3 : 2) && found == k && subs_find (&s, A) == 0 && subs_find (&s, B) == 1);
      CHECK (!strcmp (s.v[0].name, "Joueur du Grenier"));
      if (i == 0)
        CHECK (!strcmp (s.v[1].name, "Manchester Airport, live"));          /* (a quoted comma) */
      if (i == 2)
        CHECK (!strcmp (s.v[0].thumb, "https://yt3.ggpht.com/a.jpg"));
      /* again: nothing new */
      CHECK (subs_import (&s, t, len, &found) == 0 && found == k);
      free (t);
      subs_free (&s);
    }
  memset (&s, 0, sizeof s);
  if ((t = load ("takeout_fr.csv", &len)))
    {
      CHECK (subs_import (&s, t, len, &found) == 1 && !strcmp (s.v[0].name, "Joueur du Grenier"));   /* any language */
      free (t);
    }
  CHECK (subs_import (&s, "hello world\nnothing here\n", 25, &found) == -1 && found == 0);
  subs_free (&s);
  memset (&s, 0, sizeof s);
  {
    /* a JSON array laid out an object a line */
    const char *arr = "[\n{\"id\": \"" A "\", \"name\": \"One\"},\n{\"id\": \"" B "\", \"name\": \"Two\"}\n]\n";
    CHECK (subs_import (&s, arr, strlen (arr), &found) == 2 && found == 2 && !strcmp (s.v[1].name, "Two"));
    subs_free (&s);
    memset (&s, 0, sizeof s);
    /* FreeTube's All Channels repeats the other profiles' channels: counted once */
    const char *ft = "{\"name\":\"All Channels\",\"subscriptions\":[{\"id\":\"" A "\",\"name\":\"One\"}]}\n"
                     "{\"name\":\"Mine\",\"subscriptions\":[{\"id\":\"" A "\",\"name\":\"One\"}]}\n";
    CHECK (subs_import (&s, ft, strlen (ft), &found) == 1 && found == 1);
    subs_free (&s);
    memset (&s, 0, sizeof s);
    /* TubeSearch's own file (another machine's) */
    const char *own = "# TubeSearch's subscriptions\nseen 5\n" A "\tOne\thttps://x/a.jpg\n" B "\tTwo\t\n";
    CHECK (subs_import (&s, own, strlen (own), &found) == 2 && found == 2 && !strcmp (s.v[0].name, "One"));
    subs_free (&s);
    memset (&s, 0, sizeof s);
    /* two columns: the name is the one that isn't an address */
    const char *two = A ",Joueur\n";
    CHECK (subs_import (&s, two, strlen (two), &found) == 1 && !strcmp (s.v[0].name, "Joueur"));
  }
  CHECK (subs_import (&s, "{\"other\":1}", 11, &found) == -1);
  subs_free (&s);
}

static void test_times (void)
{
  char t[40];
  CHECK (subs_parse_time ("1970-01-01T00:00:00+00:00") == 0);
  CHECK (subs_parse_time ("2026-10-20T15:00:06+00:00") == 1792508406L);
  CHECK (subs_parse_time ("2026-10-10T09:30:00+02:00") == subs_parse_time ("2026-10-10T07:30:00+00:00"));
  CHECK (subs_parse_time ("2024-02-29T12:00:00.123Z") == subs_parse_time ("2024-02-29T12:00:00+00:00"));
  CHECK (subs_parse_time ("yesterday") == -1 && subs_parse_time (NULL) == -1);
  CHECK (subs_parse_time ("2026-10-10T12:30:00+0530") == subs_parse_time ("2026-10-10T07:00:00+00:00"));
  CHECK (subs_parse_time ("2026-10-10T12:30:00-05:00") == subs_parse_time ("2026-10-10T17:30:00+00:00"));
  CHECK (subs_ago_seconds ("1.2M views \xB7 3 days ago") == 3 * 86400L);
  CHECK (subs_ago_seconds ("Streamed 2 weeks ago") == 14 * 86400L);
  CHECK (subs_ago_seconds ("1 year ago") == 365 * 86400L && subs_ago_seconds ("10 minutes ago") == 600);
  CHECK (subs_ago_seconds ("Scheduled for 21/10/2026") == -1);
  subs_ago_text (30, t, sizeof t);
  CHECK (!strcmp (t, "just now"));
  subs_ago_text (3600, t, sizeof t);
  CHECK (!strcmp (t, "1 hour ago"));
  subs_ago_text (2 * 86400L + 5, t, sizeof t);
  CHECK (!strcmp (t, "2 days ago"));
  subs_ago_text (400 * 86400L, t, sizeof t);
  CHECK (!strcmp (t, "1 year ago"));
}

static void test_feed (void)
{
  size_t la, lb;
  char *fa = load ("feed_" A ".xml", &la), *fb = load ("feed_" B ".xml", &lb), err[100], path[100];
  yt_list l;
  long now = subs_parse_time ("2026-10-21T12:00:00+00:00"), seen = subs_parse_time ("2026-10-16T00:00:00+00:00");
  if (!fa || !fb)
    return;
  text_utf8_desktop = 0;                                      /* (Latin-1 desktop) */
  memset (&l, 0, sizeof l);
  CHECK (subs_parse_feed (fa, la, &l, now, seen, err, sizeof err, NULL, 0) == 3);
  CHECK (!strcmp (l.items[0].id, "aaaaaaaaaa1") && !strcmp (l.items[0].channel_id, A)
         && !strcmp (l.items[0].title, "Les jeux d'horreur & moi") && !strcmp (l.items[0].channel, "Joueur du Grenier"));
  CHECK (l.items[0].published == subs_parse_time ("2026-10-20T15:00:06+00:00"));
  CHECK (!strcmp (l.items[0].meta, "New \xB7 1.2M views \xB7 20 hours ago"));
  CHECK (!strcmp (l.items[1].meta, "999 views \xB7 1 week ago") && !strcmp (l.items[1].title, "Episode - deux"));
  CHECK (!strcmp (l.items[2].meta, "1 view \xB7 1 year ago"));
  CHECK (!strcmp (l.items[0].thumb_url, "https://i.ytimg.com/vi/aaaaaaaaaa1/mqdefault.jpg"));   /* (i.ytimg.com, as the lists) */
  CHECK (subs_parse_feed (fa, la, &l, now, seen, err, sizeof err, NULL, 0) == 0);                  /* (no doubles) */
  CHECK (subs_parse_feed (fb, lb, &l, now, seen, err, sizeof err, NULL, 0) == 2 && l.n == 5);
  CHECK (!strcmp (l.items[4].meta, "10M views \xB7 6 days ago") && !strcmp (l.items[3].channel, "Manchester Airport"));
  subs_sort_newest (&l, 4);
  CHECK (l.n == 4 && !strcmp (l.items[0].id, "bbbbbbbbbb1") && !strcmp (l.items[1].id, "aaaaaaaaaa1")
         && !strcmp (l.items[2].id, "bbbbbbbbbb2") && !strcmp (l.items[3].id, "aaaaaaaaaa2"));
  yt_list_free (&l);
  memset (&l, 0, sizeof l);
  CHECK (subs_parse_feed ("<html>404</html>", 16, &l, now, 0, err, sizeof err, NULL, 0) == -1 && l.n == 0);
  CHECK (subs_parse_feed (fa, la, &l, now, 0, err, sizeof err, NULL, 0) == 3 && !strstr (l.items[0].meta, "New"));   /* never seen: none new */
  yt_list_free (&l);
  {
    char name[100];
    memset (&l, 0, sizeof l);
    CHECK (subs_parse_feed (fa, la, &l, now, 0, err, sizeof err, name, sizeof name) == 3 && !strcmp (name, "Joueur du Grenier"));
    yt_list_free (&l);
    /* an entry with only media:title; a video ID that isn't one; HTML that mentions <feed */
    const char *odd = "<feed xmlns=\"http://www.w3.org/2005/Atom\"><entry><yt:videoId>abcdefghijk</yt:videoId>"
      "<media:group><media:title>Only here</media:title></media:group></entry>"
      "<entry><yt:videoId>../../etc</yt:videoId><title>Bad</title></entry></feed>";
    memset (&l, 0, sizeof l);
    CHECK (subs_parse_feed (odd, strlen (odd), &l, now, 0, err, sizeof err, NULL, 0) == 1 && !strcmp (l.items[0].title, "Only here"));
    yt_list_free (&l);
    const char *html = "<html><body>the <feed of the day></body></html>";
    memset (&l, 0, sizeof l);
    CHECK (subs_parse_feed (html, strlen (html), &l, now, 0, err, sizeof err, NULL, 0) == -1);
    yt_list_free (&l);
  }
  {
    char *m = subs_mark_new ("1.2K views \xB7 3 days ago");
    CHECK (m && !strcmp (m, "New \xB7 1.2K views \xB7 3 days ago"));
    free (m);
  }
  subs_feed_path (A, path, sizeof path);
  CHECK (!strcmp (path, "/feeds/videos.xml?channel_id=" A));
  free (fa);
  free (fb);
}

int main (int argc, char **argv)
{
  if (argc > 1)
    dir = argv[1];
  test_list ();
  test_import ();
  test_times ();
  test_feed ();
  printf ("test_subs: %d checks, %d failed\n", checks, failed);
  return failed != 0;
}
