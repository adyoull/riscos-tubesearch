/* Tests json.c, text.c and yt.c against real YouTube answers
   (tests/fetch-fixtures.sh) and some made-up JSON.
     test_yt FIXTURE_DIR [-v]
   Part of TubeSearch. MIT licence. */
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
  /* the other alphabets */
  {
    char *u;
    CHECK (text_set_alphabet (102) == 0 && !strcmp (text_alphabet_name (), "Latin2"));
    text_display_buf ("\xC5\x81\xC3\xB3" "d\xC5\xBA \xE2\x80\xA2 \xC2\xA9", out, sizeof out);   /* Łódź • © */
    CHECK (!strcmp (out, "\xA3\xF3" "d\xBC - (C)"));                        /* (Latin-2 has no middle dot) */
    CHECK ((u = text_to_utf8 ("\xA3\xF3" "d\xBC")) && !strcmp (u, "\xC5\x81\xC3\xB3" "d\xC5\xBA"));
    free (u);
    CHECK (text_set_alphabet (107) == 0);                                   /* Greek */
    text_display_buf ("\xCE\x91\xCE\xB8\xCE\xAE\xCE\xBD\xCE\xB1 \xE2\x80\xA2 x", out, sizeof out);  /* Αθήνα */
    CHECK (!strcmp (out, "\xC1\xE8\xDE\xED\xE1 \xB7 x"));
    CHECK (text_set_alphabet (105) == 0);                                   /* Cyrillic */
    text_display_buf ("\xD0\x9C\xD0\xBE\xD1\x81\xD0\xBA\xD0\xB2\xD0\xB0", out, sizeof out);  /* Москва */
    CHECK (!strcmp (out, "\xBC\xDE\xE1\xDA\xD2\xD0"));
    CHECK ((u = text_to_utf8 ("\xBC\xDE")) && !strcmp (u, "\xD0\x9C\xD0\xBE"));
    free (u);
    CHECK (text_set_alphabet (112) == 0);                                   /* Latin-9: the euro */
    text_display_buf ("5 \xE2\x82\xAC", out, sizeof out);
    CHECK (!strcmp (out, "5 \xA4"));
    CHECK (text_set_alphabet (110) == 0);                                   /* Welsh */
    text_display_buf ("\xC5\xB5", out, sizeof out);                         /* ŵ */
    CHECK (!strcmp (out, "\xF0"));
    CHECK (text_set_alphabet (102) == 0);                                   /* © and ® grow: "(C)" */
    text_display_buf ("\xC2\xA9" "AB \xC2\xAE" "CD", out, sizeof out);
    CHECK (!strcmp (out, "(C)AB (R)CD"));
    CHECK (text_set_alphabet (120) == -1);                                  /* Cyrillic2: not known, so ASCII only */
    text_display_buf ("Caf\xC3\xA9", out, sizeof out);
    CHECK (!strcmp (out, "Caf?"));
    CHECK ((u = text_to_utf8 ("Caf\xE9")) && !strcmp (u, "Caf?"));
    free (u);
    CHECK (text_set_alphabet (108) == 0);                                   /* Hebrew: direction marks left out */
    text_display_buf ("\xD7\xA9\xE2\x80\x8F 1", out, sizeof out);
    CHECK (!strcmp (out, "\xF9\xFE 1"));                                   /* (Hebrew has the right-to-left mark) */
    text_set_alphabet (101);
    text_display_buf ("a\xE2\x80\x8F b", out, sizeof out);
    CHECK (!strcmp (out, "a b"));
    CHECK (text_set_alphabet (99) == -1 && !strcmp (text_alphabet_name (), "ASCII"));
    CHECK (text_set_alphabet (101) == 0 && !strcmp (text_alphabet_name (), "Latin1"));
    CHECK ((u = text_to_utf8 ("Caf\xE9")) && !strcmp (u, "Caf\xC3\xA9"));
    free (u);
    CHECK (text_set_alphabet (111) == 0 && text_utf8_desktop);
    CHECK ((u = text_to_utf8 ("Caf\xC3\xA9")) && !strcmp (u, "Caf\xC3\xA9"));
    free (u);
    text_set_alphabet (101);
  }
}

/* The language and country from RISC OS's territory; languages understood */
static void test_languages (void)
{
  const char *l, *r;
  char c[16];
  yt_audio orig_en = { "en", 1, 0, 1 }, dub_fr = { "fr", 0, 1, 0 }, dub_de = { "de", 0, 1, 0 };
  CHECK (yt_territory_locale (6, &l, &r) == 0 && !strcmp (l, "fr") && !strcmp (r, "FR"));
  CHECK (yt_territory_locale (1, &l, &r) == 0 && !strcmp (l, "en-GB") && !strcmp (r, "GB"));
  CHECK (yt_territory_locale (48, &l, &r) == 0 && !strcmp (l, "en") && !strcmp (r, "US"));
  CHECK (yt_territory_locale (7, &l, &r) == 0 && !strcmp (l, "de") && !strcmp (r, "DE"));
  CHECK (yt_territory_locale (157, &l, &r) == 0 && !strcmp (l, "de") && !strcmp (r, "CH"));
  CHECK (yt_territory_locale (17, &l, &r) == 0 && !strcmp (l, "fr-CA"));                    /* Canada1: French */
  CHECK (yt_territory_locale (9, &l, &r) == -1 && yt_territory_locale (999, &l, &r) == -1);   /* Esperanto: none */
  CHECK (yt_use_territory (6) == 0 && !strcmp (yt_config_get ("language"), "fr") && !strcmp (yt_config_get ("region"), "FR"));
  CHECK (yt_use_territory (9) == -1 && !strcmp (yt_config_get ("language"), "fr"));            /* left as it was */
  yt_lang_primary ("iw", c, sizeof c);
  CHECK (!strcmp (c, "he"));
  yt_lang_primary ("pt-BR", c, sizeof c);
  CHECK (!strcmp (c, "pt"));
  /* French: a French dub before an English original... */
  CHECK (yt_audio_score (&dub_fr) > yt_audio_score (&orig_en));
  /* ...unless English is understood too: then the original as it was made */
  yt_config_set ("languages", "en, de");
  CHECK (yt_audio_score (&orig_en) > yt_audio_score (&dub_fr));
  /* a German dub (understood) isn't taken over the French one */
  CHECK (yt_audio_score (&dub_fr) > yt_audio_score (&dub_de));
  {
    /* a Japanese original, a German dub (understood), no French: the dub */
    yt_audio orig_ja = { "ja", 1, 0, 1 };
    yt_config_set ("languages", "de");
    CHECK (yt_audio_score (&dub_de) > yt_audio_score (&orig_ja));
  }
  {
    /* YouTube's default on a dub you understand doesn't beat your own */
    yt_audio dub_de_dflt = { "de", 0, 1, 1 };
    CHECK (yt_audio_score (&dub_fr) > yt_audio_score (&dub_de_dflt));
    /* a language named is that one, whatever else is understood */
    yt_config_set ("audio_language", "de");
    CHECK (yt_audio_score (&dub_de) > yt_audio_score (&orig_en));
    yt_config_set ("audio_language", "mine");
  }
  yt_config_set ("languages", "de");                                       /* English not understood */
  CHECK (yt_audio_score (&dub_fr) > yt_audio_score (&orig_en));
  /* the menus' list, and ticking languages on and off */
  CHECK (!strcmp (yt_lang_menu (0), "ar") && yt_lang_menu (29) && !yt_lang_menu (30) && !yt_lang_menu (-1));
  for (int i = 0; yt_lang_menu (i); i++)
    CHECK (strcmp (yt_lang_name (yt_lang_menu (i)), yt_lang_menu (i)) && strlen (yt_lang_name (yt_lang_menu (i))) <= 12);
  yt_config_set ("languages", "en, de");
  CHECK (yt_lang_toggle ("fr") == 0 && !strcmp (yt_config_get ("languages"), "en de fr") && yt_lang_understood ("fr-FR"));
  CHECK (yt_lang_toggle ("de-DE") == 0 && !strcmp (yt_config_get ("languages"), "en fr") && !yt_lang_understood ("de"));
  CHECK (yt_lang_toggle ("en") == 0 && yt_lang_toggle ("fr") == 0 && !strcmp (yt_config_get ("languages"), ""));
  yt_config_set ("languages", "");
  yt_config_set ("language", "en-GB");
  yt_config_set ("region", "GB");
}

static void test_requests (void)
{
  char err[96];
  yt_req *r = yt_req_search ("cats \"and\" dogs", YT_DATE_ANY);
  CHECK (r && !strcmp (r->host, "www.youtube.com") && !strcmp (r->path, "/youtubei/v1/search?prettyPrint=false"));
  if (r)
    {
      jdoc *d = json_parse (r->body, strlen (r->body), err, sizeof err);
      CHECK (d && !strcmp (j_pstr (json_root (d), "query"), "cats \"and\" dogs"));
      CHECK (d && !strcmp (j_pstr (json_root (d), "context.client.clientName"), "WEB"));
      CHECK (strstr (r->headers, "X-YouTube-Client-Name: 1\r\n") && strstr (r->headers, "Cookie: SOCS=CAE=\r\n"));
      CHECK (d && !j_get (json_root (d), "params"));
      json_free (d);
      yt_req_free (r);
    }
  /* the upload-date filter: params = base64 of protobuf {2: {1: date}},
     i.e. 12 02 08 <date> (Today 2, week 3, month 4, year 5) */
  for (int date = YT_DATE_TODAY; date < YT_DATE_COUNT; date++)
    {
      static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
      const char *p = yt_date_params (date);
      unsigned char bytes[8];
      int n = 0;
      unsigned acc = 0, bits = 0;
      for (; *p && *p != '='; p++)
        {
          acc = acc << 6 | (unsigned) (strchr (b64, *p) - b64);
          if ((bits += 6) >= 8)
            bytes[n++] = (unsigned char) (acc >> (bits -= 8)), acc &= (1u << bits) - 1;
        }
      CHECK (n == 4 && bytes[0] == 0x12 && bytes[1] == 2 && bytes[2] == 8 && bytes[3] == date + 1);
      r = yt_req_search ("cats", date);
      if (r)
        {
          jdoc *d = json_parse (r->body, strlen (r->body), err, sizeof err);
          CHECK (d && !strcmp (j_pstr (json_root (d), "params"), yt_date_params (date)));
          json_free (d);
          yt_req_free (r);
        }
      CHECK (yt_date_parse (yt_date_word (date)) == date);
    }
  CHECK (!strcmp (yt_date_name (YT_DATE_WEEK), "This week") && yt_date_parse ("fortnight") < 0);
  CHECK (!*yt_date_params (YT_DATE_ANY) && !*yt_date_params (99));
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
    /* a ciphered format with no address in its cipher: not taken */
    const char *sig = "{\"playabilityStatus\":{\"status\":\"OK\"},\"streamingData\":{\"adaptiveFormats\":[{\"itag\":137,\"mimeType\":\"video/mp4; codecs=\\\"avc1\\\"\",\"signatureCipher\":\"s=x\"}]}}";
    CHECK (yt_parse_player (sig, strlen (sig), 720, &p, err, sizeof err) == -1 && strstr (err, "no H.264"));
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

/* The TV client: ciphered signatures and n values, answered and put back. */
static void test_tv (void)
{
  size_t len;
  char err[200], *t;
  yt_play p;
  const char *n[2], *sig[2];
  int nn, nsig;
  static const char *const n_out_v[] = { "RL7twTMlYAhbWlqMF", "OtjoLvmzT_c2ob_ei" };
  static const char sig_out[] =
    "zzwg=wgwCHlydB9Hg7PMegXoVzaoAXXB8woPSNZqRUC3Pe7vAEiApVSCMlh5mt5OX-NMB=tRPyyzdAM9MPM-kPfjgTxEK0IAhIgRwE0jiEJA";

  /* the player's ID and signature timestamp */
  if ((t = load ("iframe_api.js", &len)))
    {
      char id[16], path[128];
      CHECK (yt_player_id (t, len, id) == 0 && !strcmp (id, "854a788e"));
      yt_player_path (id, path, sizeof path);
      CHECK (!strcmp (path, "/s/player/854a788e/player_ias.vflset/en_US/base.js"));
      free (t);
    }
  CHECK (yt_player_id ("nothing here", 12, (char[16]) { 0 }) == -1);
  if ((t = load ("player_854a788e.js", &len)))
    {
      CHECK (yt_player_sts (t, len) == 20668);
      free (t);
    }
  CHECK (yt_player_sts ("var a={sts:19123}", 17) == 19123);
  CHECK (yt_player_sts ("hosts:12345", 11) == 0);

  /* the TV request */
  {
    yt_req *r = yt_req_player_tv ("7PIMiDcwNvc", 20668);
    CHECK (r && !strcmp (r->host, "www.youtube.com") && strstr (r->path, "/youtubei/v1/player"));
    CHECK (r && strstr (r->body, "\"clientName\":\"TVHTML5\"") && strstr (r->body, "\"signatureTimestamp\":20668"));
    CHECK (r && strstr (r->headers, "X-YouTube-Client-Name: 7\r\n") && strstr (r->headers, "SMART-TV"));
    yt_req_free (r);
  }

  if (!(t = load ("player_tv.json", &len)))
    return;
  CHECK (yt_parse_player (t, len, 720, &p, err, sizeof err) == 0);
  CHECK (yt_play_needs_solving (&p));
  CHECK (p.quality == 720 && strstr (p.video_url, "itag=136") && strstr (p.audio_url, "itag=140"));
  CHECK (p.video_sp && !strcmp (p.video_sp, "sig") && p.video_s && !strncmp (p.video_s, "NJAJEij0Ew", 10));
  CHECK (p.video_n && !strcmp (p.video_n, "IlLiA21ny7gqA2m4p37") && p.audio_n && !strcmp (p.audio_n, "eabGFpsUKuWHXGh6FR4"));
  yt_play_challenges (&p, n, &nn, sig, &nsig);
  CHECK (nn == 2 && nsig == 1);                   /* (the same signature on both) */
  CHECK (yt_play_apply (&p, n_out_v, (const char *const[]) { sig_out }) == 0);
  CHECK (!yt_play_needs_solving (&p));
  CHECK (strstr (p.video_url, "&n=RL7twTMlYAhbWlqMF&") || strstr (p.video_url, "&n=RL7twTMlYAhbWlqMF"));
  CHECK (strstr (p.audio_url, "n=OtjoLvmzT_c2ob_ei"));
  CHECK (!strstr (p.video_url, "IlLiA21ny7gqA2m4p37"));
  {
    /* the signature, URL-encoded, as the last parameter */
    const char *e = strstr (p.video_url, "&sig=zzwg%3Dwgw");
    CHECK (e && !strchr (e + 1, '&') && strstr (e, "jiEJA"));
  }
  if (verbose)
    printf ("  tv video: ...%s\n", p.video_url + strlen (p.video_url) - 160);
  yt_play_free (&p);
  free (t);
}

/* HLS: choosing a variant (and its sound) from a master playlist. */
static void test_hls (void)
{
  size_t len, llen;
  char *m = load ("../hls/master.m3u8", &len), *live = load ("../hls/live.m3u8", &llen);
  char *v, *a, codec[32], err[200], host[128], path[512];
  int q;
  yt_audio ai;
  const char *base = "https://manifest.googlevideo.com/api/manifest/hls_variant/expire/1/id/x/file/index.m3u8";
  if (!m || !live)
    return;
  CHECK (yt_hls_choose (m, len, base, 1080, &v, &a, &q, codec, sizeof codec, &ai, err, sizeof err) == 0);
  CHECK (q == 1080 && strstr (v, "/itag/270/") && !strcmp (codec, "avc1.640028"));
  /* the sound in my language (en-GB): the English original, though
     YouTube marks the German dub as the default */
  CHECK (a && strstr (a, "itag%3D140;xtags%3Dacont%253Doriginal%253Alang%253Den-US") && !strcmp (ai.lang, "en") && ai.original && !ai.dubbed);
  free (v); free (a);
  yt_config_set ("language", "de-DE");                                       /* German: the dub */
  CHECK (yt_hls_choose (m, len, base, 1080, &v, &a, &q, codec, sizeof codec, &ai, err, sizeof err) == 0
         && strstr (a, "lang%253Dde-DE") && !strcmp (ai.lang, "de") && ai.dubbed);
  free (v); free (a);
  yt_config_set ("audio_language", "original");                              /* German, but the original */
  CHECK (yt_hls_choose (m, len, base, 1080, &v, &a, &q, codec, sizeof codec, &ai, err, sizeof err) == 0
         && strstr (a, "itag%3D140;xtags%3Dacont%253Doriginal%253Alang%253Den-US") && ai.original);
  free (v); free (a);
  yt_config_set ("audio_language", "fr");                                    /* a language named */
  CHECK (yt_hls_choose (m, len, base, 1080, &v, &a, &q, codec, sizeof codec, &ai, err, sizeof err) == 0
         && strstr (a, "lang%253Dfr-FR") && !strcmp (yt_lang_name (ai.lang), "French"));
  free (v); free (a);
  yt_config_set ("audio_language", "es");                                    /* not there: the original */
  CHECK (yt_hls_choose (m, len, base, 1080, &v, &a, &q, codec, sizeof codec, &ai, err, sizeof err) == 0
         && strstr (a, "itag%3D140;xtags%3Dacont%253Doriginal%253Alang%253Den-US"));
  yt_config_set ("audio_language", "mine");
  yt_config_set ("language", "en-GB");
  free (v); free (a);
  CHECK (yt_hls_choose (m, len, base, 720, &v, &a, &q, codec, sizeof codec, &ai, err, sizeof err) == 0);
  CHECK (q == 720 && strstr (v, "/itag/232/"));                                /* 30 a second, not 60 */
  free (v); free (a);
  CHECK (yt_hls_choose (m, len, base, 480, &v, &a, &q, codec, sizeof codec, &ai, err, sizeof err) == 0);
  CHECK (q == 360 && strstr (v, "/itag/230/"));
  free (v); free (a);
  CHECK (yt_hls_choose (m, len, base, 144, &v, &a, &q, codec, sizeof codec, &ai, err, sizeof err) == 0);
  CHECK (q == 144 && strstr (v, "/itag/269/") && a && strstr (a, "/itag/233/"));
  free (v); free (a);
  CHECK (yt_hls_choose (m, len, base, 100, &v, &a, &q, codec, sizeof codec, &ai, err, sizeof err) == 0 && q == 144);
  free (v); free (a);
  CHECK (yt_hls_choose (m, len, base, 4000, &v, &a, &q, codec, sizeof codec, &ai, err, sizeof err) == 0 && q == 1080);
  free (v); free (a);                                                         /* (never VP9) */
  /* a live stream's: the sound in the video; names from the server's root */
  /* (its 1080p is 60 a second: at most 30 gives the 720p) */
  CHECK (yt_hls_choose (live, llen, base, 1080, &v, &a, &q, codec, sizeof codec, &ai, err, sizeof err) == 0
         && !a && q == 720 && strstr (v, "/itag/95/"));
  free (v);
  yt_config_set ("max_fps", "60");
  CHECK (yt_hls_choose (live, llen, base, 1080, &v, &a, &q, codec, sizeof codec, &ai, err, sizeof err) == 0);
  CHECK (!a && q == 1080 && !strcmp (v, "https://manifest.googlevideo.com/api/manifest/hls_playlist/id/live/itag/301/playlist/index.m3u8"));
  free (v);
  /* up to 60: the 720p60 (more bits) rather than the 720p30 */
  CHECK (yt_hls_choose (m, len, base, 720, &v, &a, &q, codec, sizeof codec, &ai, err, sizeof err) == 0 && strstr (v, "/itag/311/"));
  free (v); free (a);
  yt_config_set ("max_fps", "30");
  CHECK (yt_hls_choose (live, llen, base, 720, &v, &a, &q, codec, sizeof codec, &ai, err, sizeof err) == 0 && strstr (v, "/itag/95/"));
  free (v);
  {
    /* only 60 a second, and a cap of 30: the lowest there is */
    const char *all60 = "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1,CODECS=\"avc1.4d401f\",RESOLUTION=1280x720,FRAME-RATE=60\nhttps://x/60.m3u8\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=1,CODECS=\"avc1.4d401f\",RESOLUTION=1280x720,FRAME-RATE=50\nhttps://x/50.m3u8\n";
    CHECK (yt_hls_choose (all60, strlen (all60), base, 1080, &v, &a, &q, codec, sizeof codec, &ai, err, sizeof err) == 0 && strstr (v, "/50."));
    free (v);
  }
  {
    /* DASH: frame rates from the formats' fps */
    yt_play p;
    const char *j = "{\"playabilityStatus\":{\"status\":\"OK\"},\"streamingData\":{\"adaptiveFormats\":["
      "{\"itag\":299,\"mimeType\":\"video/mp4; codecs=\\\"avc1.64002a\\\"\",\"qualityLabel\":\"1080p60\",\"fps\":60,\"bitrate\":9,\"url\":\"https://v/299\"},"
      "{\"itag\":137,\"mimeType\":\"video/mp4; codecs=\\\"avc1.640028\\\"\",\"qualityLabel\":\"1080p\",\"fps\":30,\"bitrate\":5,\"url\":\"https://v/137\"},"
      "{\"itag\":298,\"mimeType\":\"video/mp4; codecs=\\\"avc1.4d4020\\\"\",\"qualityLabel\":\"720p60\",\"fps\":60,\"bitrate\":4,\"url\":\"https://v/298\"},"
      "{\"itag\":140,\"mimeType\":\"audio/mp4; codecs=\\\"mp4a.40.2\\\"\",\"bitrate\":1,\"url\":\"https://a/140\"}]}}";
    CHECK (yt_parse_player (j, strlen (j), 1080, &p, err, sizeof err) == 0 && strstr (p.video_url, "/137"));
    yt_play_free (&p);
    yt_config_set ("max_fps", "60");
    CHECK (yt_parse_player (j, strlen (j), 1080, &p, err, sizeof err) == 0 && strstr (p.video_url, "/299"));
    yt_play_free (&p);
    CHECK (yt_parse_player (j, strlen (j), 720, &p, err, sizeof err) == 0 && strstr (p.video_url, "/298"));
    yt_play_free (&p);
    yt_config_set ("max_fps", "30");
    CHECK (yt_parse_player (j, strlen (j), 720, &p, err, sizeof err) == 0 && strstr (p.video_url, "/298"));  /* (no 720p30: 720p60) */
    yt_play_free (&p);
  }
  {
    const char *rel = "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1,CODECS=\"avc1.4d401f,mp4a.40.2\",RESOLUTION=1280x720\r\nsub/index.m3u8\r\n";
    CHECK (yt_hls_choose (rel, strlen (rel), "https://h.example/a/b/master.m3u8?x=1", 1080, &v, &a, &q, codec, sizeof codec, &ai, err, sizeof err) == 0
           && !strcmp (v, "https://h.example/a/b/sub/index.m3u8") && !a);
    free (v);
    /* only the xtags say which is the original */
    const char *xt = "#EXTM3U\n"
      "#EXT-X-MEDIA:URI=\"https://x/a/xtags%3Dacont%253Ddubbed-auto%253Alang%253Dit/i.m3u8\",TYPE=AUDIO,GROUP-ID=\"1\",NAME=\"A\",DEFAULT=YES\n"
      "#EXT-X-MEDIA:URI=\"https://x/b/xtags%3Dacont%253Doriginal%253Alang%253Dja/i.m3u8\",TYPE=AUDIO,GROUP-ID=\"1\",NAME=\"B\"\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=1,CODECS=\"avc1.4d401f\",RESOLUTION=1280x720,AUDIO=\"1\"\nhttps://x/v.m3u8\n";
    CHECK (yt_hls_choose (xt, strlen (xt), base, 1080, &v, &a, &q, codec, sizeof codec, &ai, err, sizeof err) == 0
           && strstr (a, "/b/") && !strcmp (ai.lang, "ja") && ai.original);
    free (v); free (a);
    const char *vp9 = "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1,CODECS=\"vp09.00.40.08\",RESOLUTION=1920x1080\nhttps://x/v.m3u8\n";
    CHECK (yt_hls_choose (vp9, strlen (vp9), base, 1080, &v, &a, &q, codec, sizeof codec, &ai, err, sizeof err) == -1 && strstr (err, "H.264") && !v && !a);
    const char *nosound = "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1,CODECS=\"avc1.4d401f\",RESOLUTION=1280x720,AUDIO=\"9\"\nhttps://x/v.m3u8\n";
    CHECK (yt_hls_choose (nosound, strlen (nosound), base, 1080, &v, &a, &q, codec, sizeof codec, &ai, err, sizeof err) == -1 && strstr (err, "sound"));
    CHECK (yt_hls_choose ("<html>", 6, base, 1080, &v, &a, &q, codec, sizeof codec, &ai, err, sizeof err) == -1);
  }
  CHECK (yt_split_url (base, host, sizeof host, path, sizeof path) == 0 && !strcmp (host, "manifest.googlevideo.com")
         && !strncmp (path, "/api/manifest/hls_variant/", 26));
  CHECK (yt_split_url ("https://h.example?x", host, sizeof host, path, sizeof path) == 0 && !strcmp (path, "/?x"));
  CHECK (yt_split_url ("http://h.example/", host, sizeof host, path, sizeof path) < 0);
  free (m);
  free (live);
  /* DASH: the same choice of sound, from a real answer with 35 tracks
     (the English original, marked default, and 34 dubs) */
  {
    yt_play p;
    char *t = load ("player_multiaudio.json", &len);
    if (t && yt_parse_player (t, len, 720, &p, err, sizeof err) == 0)
      {
        CHECK (strstr (p.audio_url, "xtags=acont%3Doriginal%3Alang%3Den-US") && !strcmp (p.audio.lang, "en") && p.audio.original);
        yt_play_free (&p);
        yt_config_set ("language", "de");
        CHECK (yt_parse_player (t, len, 720, &p, err, sizeof err) == 0
               && strstr (p.audio_url, "xtags=acont%3Ddubbed%3Alang%3Dde&") && p.audio.dubbed && !strcmp (p.audio.lang, "de"));
        yt_play_free (&p);
        yt_config_set ("audio_language", "original");
        CHECK (yt_parse_player (t, len, 720, &p, err, sizeof err) == 0 && strstr (p.audio_url, "acont%3Doriginal"));
        yt_play_free (&p);
        yt_config_set ("audio_language", "pt");
        CHECK (yt_parse_player (t, len, 720, &p, err, sizeof err) == 0 && strstr (p.audio_url, "lang%3Dpt-BR"));
        yt_play_free (&p);
        yt_config_set ("audio_language", "mine");
        yt_config_set ("language", "en-GB");
      }
    else
      CHECK (0);
    free (t);
  }
  /* the visionOS answer has both: HLS by default, DASH when Config says so */
  {
    yt_play p;
    char *t = load ("player_visionos.json", &len);
    if (t && yt_parse_player (t, len, 720, &p, err, sizeof err) == 0)
      {
        CHECK (p.hls_url && p.video_url && p.audio_url && !p.live && !p.hls);
        CHECK (yt_want_hls (&p));
        yt_config_set ("stream_format", "dash");
        CHECK (!yt_want_hls (&p));
        p.live = 1;
        CHECK (yt_want_hls (&p));                                              /* (live: always) */
        yt_config_set ("stream_format", "hls");
        p.hls = 1;
        p.live = 0;
        {
          char *rj = yt_reel_json (&p, "x");
          CHECK (rj && strstr (rj, "\"protocol\":\"m3u8_native\""));
          free (rj);
        }
        yt_play_free (&p);
      }
    else
      CHECK (0);
    free (t);
  }
}

int main (int argc, char **argv)
{
  dir = argc > 1 ? argv[1] : "tests/fixtures";
  verbose = argc > 2 && !strcmp (argv[2], "-v");
  test_json ();
  test_text ();
  test_languages ();
  test_requests ();
  test_lists ();
  test_player ();
  test_tv ();
  test_hls ();
  printf (failures ? "test_yt: %d FAILED\n" : "test_yt: all passed\n", failures);
  return failures != 0;
}
