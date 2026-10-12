/* Tests json.c, text.c and yt.c against real YouTube answers
   (tests/fetch-fixtures.sh) and some made-up JSON.
     test_yt FIXTURE_DIR [-v]
   Part of Tube. MIT licence. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"
#include "text.h"
#include "yt.h"

static int failures, verbose;
static const char *dir;

#define CHECK(c) do { if (!(c)) { printf ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static char *load (const char *name, size_t *len)
{
  char path[512];
  FILE *f;
  char *b;
  long n;
  snprintf (path, sizeof path, "%s/%s", dir, name);
  if (!(f = fopen (path, "rb")))
    {
      printf ("FAIL: can't open %s (run tests/fetch-fixtures.sh)\n", path);
      failures++;
      return NULL;
    }
  fseek (f, 0, SEEK_END);
  n = ftell (f);
  fseek (f, 0, SEEK_SET);
  b = malloc ((size_t) n + 1);
  if (fread (b, 1, (size_t) n, f) != (size_t) n)
    n = 0;
  b[n] = 0;
  fclose (f);
  *len = (size_t) n;
  return b;
}

static void show (const yt_list *l, int from)
{
  if (!verbose)
    return;
  printf ("  title=%s chan=%s cont=%s\n", l->title ? l->title : "-",
          l->title_channel_id ? l->title_channel_id : "-", l->continuation ? "yes" : "no");
  for (int i = from; i < l->n; i++)
    printf ("  %2d %s %-11s [%s] %s | %s (%s) | %s\n", i, l->items[i].kind == YT_VIDEO ? "V" : "C",
            l->items[i].id, l->items[i].duration, l->items[i].title, l->items[i].channel,
            l->items[i].channel_id, l->items[i].meta);
}

static void test_json (void)
{
  char err[96], buf[64];
  const char *t = "{\"a\":[1,2,{\"b\":\"x\\u00e9\\ud83d\\ude00\\n\"}],\"t\":true,\"r\":{\"runs\":[{\"text\":\"Hello \"},{\"text\":\"world\"}]}}";
  jdoc *d = json_parse (t, strlen (t), err, sizeof err);
  CHECK (d != NULL);
  if (d)
    {
      jnode *r = json_root (d);
      CHECK (j_path (r, "a.1") && !strcmp (j_path (r, "a.1")->s, "2"));
      CHECK (j_pstr (r, "a.2.b") && !strcmp (j_pstr (r, "a.2.b"), "x\xC3\xA9\xF0\x9F\x98\x80\n"));
      CHECK (j_true (j_get (r, "t")));
      CHECK (j_text (j_get (r, "r"), buf, sizeof buf) == 11 && !strcmp (buf, "Hello world"));
      CHECK (j_text (j_get (r, "r"), buf, 6) == 5 && !strcmp (buf, "Hello"));
      CHECK (j_find (r, "b") != NULL);
      json_free (d);
    }
  CHECK (!json_parse ("{\"a\":", 5, err, sizeof err) && strstr (err, "JSON"));
  CHECK (!json_parse ("[1,2", 4, err, sizeof err));
  CHECK (!json_parse ("{\"a\" 1}", 7, err, sizeof err));
  CHECK (json_quote ("a\"b\\c\n", buf, sizeof buf) == 15 && !strcmp (buf, "\"a\\\"b\\\\c\\u000a\""));
  /* deep nesting is refused, not a crash */
  {
    static char deep[3000];
    memset (deep, '[', 1500);
    memset (deep + 1500, ']', 1500);
    CHECK (!json_parse (deep, 3000, err, sizeof err));
  }
}

static void test_text (void)
{
  char out[64];
  text_utf8_desktop = 0;
  text_display_buf ("Caf\xC3\xA9 \xE2\x80\x9CHi\xE2\x80\x9D \xE2\x80\x94 x \xE2\x80\xA2 y\xE2\x80\xA6", out, sizeof out);
  CHECK (!strcmp (out, "Caf\xE9 \"Hi\" - x \xB7 y..."));
  text_display_buf ("A \xF0\x9F\x98\x80 B\t\xE4\xB8\xAD", out, sizeof out);
  CHECK (!strcmp (out, "A B ?"));
  text_display_buf ("abcdefghij", out, 5);
  CHECK (!strcmp (out, "abcd"));
  text_utf8_desktop = 1;
  text_display_buf ("Caf\xC3\xA9", out, sizeof out);
  CHECK (!strcmp (out, "Caf\xC3\xA9"));
  text_utf8_desktop = 0;
}

static void test_requests (void)
{
  char err[96];
  yt_req *r = yt_req_search ("cats \"and\" dogs");
  CHECK (r && !strcmp (r->host, "www.youtube.com") && !strcmp (r->path, "/youtubei/v1/search?prettyPrint=false"));
  if (r)
    {
      jdoc *d = json_parse (r->body, strlen (r->body), err, sizeof err);
      CHECK (d && !strcmp (j_pstr (json_root (d), "query"), "cats \"and\" dogs"));
      CHECK (d && !strcmp (j_pstr (json_root (d), "context.client.clientName"), "WEB"));
      CHECK (strstr (r->headers, "X-YouTube-Client-Name: 1\r\n") && strstr (r->headers, "Cookie: SOCS=CAE=\r\n"));
      json_free (d);
      yt_req_free (r);
    }
  r = yt_req_player ("7PIMiDcwNvc");
  CHECK (r && !strcmp (r->host, "youtubei.googleapis.com"));
  if (r)
    {
      jdoc *d = json_parse (r->body, strlen (r->body), err, sizeof err);
      CHECK (strstr (r->path, "/youtubei/v1/player?prettyPrint=false&t=") && strstr (r->path, "&id=7PIMiDcwNvc"));
      CHECK (d && !strcmp (j_pstr (json_root (d), "context.client.clientName"), "VISIONOS"));
      CHECK (d && strlen (j_pstr (json_root (d), "cpn")) == 16);
      CHECK (strstr (r->headers, "User-Agent: com.google.visionos.youtube/"));
      json_free (d);
      yt_req_free (r);
    }
  r = yt_req_more (YT_LIST_CHANNEL, "TOKEN");
  CHECK (r && strstr (r->path, "/browse?") && strstr (r->body, "\"continuation\":\"TOKEN\""));
  yt_req_free (r);
  CHECK (yt_config_set ("player_client_version", "9.99") == 0);
  CHECK (!strcmp (yt_config_get ("player_client_version"), "9.99"));
  CHECK (yt_config_set ("no_such_thing", "x") == -1);
  yt_config_set ("player_client_version", "1.04");
}

static int count_kind (const yt_list *l, yt_kind k, int from)
{
  int n = 0;
  for (int i = from; i < l->n; i++)
    n += l->items[i].kind == k;
  return n;
}

static void test_lists (void)
{
  size_t len;
  char err[160], *t;
  yt_list l;
  int n;

  /* search, then its next part (a continuation answer) */
  memset (&l, 0, sizeof l);
  if ((t = load ("search_1.json", &len)))
    {
      n = yt_parse_list (t, len, YT_LIST_SEARCH, &l, err, sizeof err);
      printf ("search_1: %d items (%d videos)\n", n, count_kind (&l, YT_VIDEO, 0));
      show (&l, 0);
      CHECK (n >= 15);
      CHECK (l.continuation != NULL);
      for (int i = 0; i < l.n; i++)
        {
          CHECK (l.items[i].title[0]);
          CHECK (l.items[i].thumb_url[0]);
          if (l.items[i].kind == YT_VIDEO)
            CHECK (l.items[i].channel[0] && !strncmp (l.items[i].channel_id, "UC", 2));
        }
      free (t);
    }
  if ((t = load ("search_2.json", &len)))
    {
      int before = l.n;
      n = yt_parse_list (t, len, YT_LIST_SEARCH, &l, err, sizeof err);
      printf ("search_2 (more): %d items\n", n);
      show (&l, before);
      CHECK (n >= 8);
      CHECK (l.continuation != NULL);
      free (t);
    }
  yt_list_free (&l);

  if ((t = load ("search_channel.json", &len)))
    {
      memset (&l, 0, sizeof l);
      n = yt_parse_list (t, len, YT_LIST_SEARCH, &l, err, sizeof err);
      printf ("search_channel: %d items (%d channels)\n", n, count_kind (&l, YT_CHANNEL, 0));
      show (&l, 0);
      CHECK (count_kind (&l, YT_CHANNEL, 0) >= 1);
      for (int i = 0; i < l.n; i++)
        if (l.items[i].kind == YT_CHANNEL)
          CHECK (!strncmp (l.items[i].thumb_url, "https://", 8) && !strncmp (l.items[i].id, "UC", 2));
      yt_list_free (&l);
      free (t);
    }

  /* related videos */
  if ((t = load ("next.json", &len)))
    {
      memset (&l, 0, sizeof l);
      n = yt_parse_list (t, len, YT_LIST_RELATED, &l, err, sizeof err);
      printf ("next: %d related\n", n);
      show (&l, 0);
      CHECK (n >= 15);
      CHECK (l.title && !strcmp (l.title, "Marzia & Felix - Wedding 19.08.2019"));
      CHECK (l.title_channel_id && !strcmp (l.title_channel_id, "UC-lHJZR3Gqxm24_Vd_AJ5Yw"));
      CHECK (l.continuation != NULL);
      for (int i = 0; i < l.n; i++)
        CHECK (l.items[i].channel[0] && l.items[i].meta[0] && l.items[i].duration[0]);
      yt_list_free (&l);
      free (t);
    }

  /* a channel's videos, and the next part */
  memset (&l, 0, sizeof l);
  if ((t = load ("channel_1.json", &len)))
    {
      n = yt_parse_list (t, len, YT_LIST_CHANNEL, &l, err, sizeof err);
      printf ("channel_1: %d videos\n", n);
      show (&l, 0);
      CHECK (n == 30);
      CHECK (l.title && !strcmp (l.title, "Creative Commons"));
      CHECK (l.continuation != NULL);
      for (int i = 0; i < l.n; i++)
        CHECK (!strcmp (l.items[i].channel, "Creative Commons") && l.items[i].duration[0]
               && !strcmp (l.items[i].channel_id, l.title_channel_id));
      free (t);
    }
  if ((t = load ("channel_2.json", &len)))
    {
      n = yt_parse_list (t, len, YT_LIST_CHANNEL, &l, err, sizeof err);
      printf ("channel_2 (more): %d videos\n", n);
      CHECK (n == 30);
      CHECK (l.n == 60);
      CHECK (!strcmp (l.items[59].channel, "Creative Commons"));
      free (t);
    }
  yt_list_free (&l);

  /* errors */
  memset (&l, 0, sizeof l);
  CHECK (yt_parse_list ("<html>", 6, YT_LIST_SEARCH, &l, err, sizeof err) == -1 && strstr (err, "readable"));
  {
    const char *e = "{\"error\":{\"code\":400,\"message\":\"Request contains an invalid argument.\"}}";
    CHECK (yt_parse_list (e, strlen (e), YT_LIST_SEARCH, &l, err, sizeof err) == -1 && strstr (err, "invalid argument"));
  }
  yt_list_free (&l);
}

static void test_player (void)
{
  size_t len;
  char err[200], *t, *rj;
  yt_play p;
  if ((t = load ("visitor.json", &len)))
    {
      CHECK (yt_parse_visitor (t, len) == 0 && yt_visitor && strlen (yt_visitor) > 20);
      free (t);
    }
  if (!(t = load ("player_visionos.json", &len)))
    return;
  CHECK (yt_parse_player (t, len, 720, &p, err, sizeof err) == 0);
  printf ("player 720: %dp %s, sound %s\n", p.quality, p.codec, p.audio_url ? "yes" : "no");
  CHECK (p.quality == 720 && p.video_url && strstr (p.video_url, "itag=136") && p.audio_url && strstr (p.audio_url, "itag=140"));
  CHECK (p.title && !strcmp (p.title, "Marzia & Felix - Wedding 19.08.2019"));
  rj = yt_reel_json (&p, "7PIMiDcwNvc");
  CHECK (rj != NULL);
  if (rj)
    {
      jdoc *d = json_parse (rj, strlen (rj), err, sizeof err);
      jnode *r = json_root (d);
      CHECK (d && j_path (r, "requested_formats.1.url") && !strcmp (j_pstr (r, "requested_formats.1.vcodec"), "none"));
      CHECK (d && !strcmp (j_pstr (r, "requested_formats.0.acodec"), "none"));
      CHECK (d && strstr (j_pstr (r, "http_headers.User-Agent"), "Mozilla/5.0"));
      CHECK (d && !strcmp (j_pstr (r, "webpage_url"), "https://www.youtube.com/watch?v=7PIMiDcwNvc"));
      if (verbose)
        printf ("%.300s...\n", rj);
      json_free (d);
      free (rj);
    }
  yt_play_free (&p);
  CHECK (yt_parse_player (t, len, 1080, &p, err, sizeof err) == 0 && p.quality == 1080 && strstr (p.video_url, "itag=137"));
  yt_play_free (&p);
  CHECK (yt_parse_player (t, len, 100, &p, err, sizeof err) == 0 && p.quality == 144);
  yt_play_free (&p);
  free (t);
  {
    const char *bot = "{\"playabilityStatus\":{\"status\":\"LOGIN_REQUIRED\",\"reason\":\"Sign in to confirm you\\u2019re not a bot\"}}";
    CHECK (yt_parse_player (bot, strlen (bot), 720, &p, err, sizeof err) == -1 && strstr (err, "not a bot") && strstr (err, "Config"));
    if (verbose)
      printf ("  %s\n", err);
  }
  {
    const char *sig = "{\"playabilityStatus\":{\"status\":\"OK\"},\"streamingData\":{\"adaptiveFormats\":[{\"itag\":137,\"mimeType\":\"video/mp4; codecs=\\\"avc1\\\"\",\"signatureCipher\":\"s=x\"}]}}";
    CHECK (yt_parse_player (sig, strlen (sig), 720, &p, err, sizeof err) == -1 && strstr (err, "scrambled"));
  }
  {
    const char *live = "{\"playabilityStatus\":{\"status\":\"OK\"},\"videoDetails\":{\"isLive\":true,\"title\":\"L\"},\"streamingData\":{\"hlsManifestUrl\":\"https://x/hls.m3u8\",\"adaptiveFormats\":[{\"itag\":137,\"mimeType\":\"video/mp4; codecs=\\\"avc1\\\"\",\"url\":\"https://v\"}]}}";
    CHECK (yt_parse_player (live, strlen (live), 720, &p, err, sizeof err) == 0 && p.hls_url && !p.video_url);
    rj = yt_reel_json (&p, "LIVE1");
    CHECK (rj && strstr (rj, "\"url\":\"https://x/hls.m3u8\""));
    free (rj);
    yt_play_free (&p);
  }
}

int main (int argc, char **argv)
{
  dir = argc > 1 ? argv[1] : "tests/fixtures";
  verbose = argc > 2 && !strcmp (argv[2], "-v");
  test_json ();
  test_text ();
  test_requests ();
  test_lists ();
  test_player ();
  printf (failures ? "test_yt: %d FAILED\n" : "test_yt: all passed\n", failures);
  return failures != 0;
}
