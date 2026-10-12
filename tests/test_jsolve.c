/* Tests jsolve.c (QuickJS running yt-dlp's solver) on a real YouTube
   player (854a788e, recorded by NewPipe's tests): the answers must be the
   ones node gives for the same scripts and player (tests/jsolve-expected.txt,
   made by tests/jsolve-reference.sh). Also: the prepared player is reused,
   a stop from the tick works, and bad scripts fail cleanly.
     test_jsolve FIXTURE_DIR EJS_DIR [-time]
   Part of TubeSearch. MIT licence. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "jsolve.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static char *load (const char *dir, const char *name, size_t *len)
{
  char path[512];
  FILE *f;
  char *b;
  long n;
  snprintf (path, sizeof path, "%s/%s", dir, name);
  if (!(f = fopen (path, "rb")))
    {
      printf ("FAIL: can't open %s\n", path);
      exit (1);
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

static double now (void)
{
  struct timespec t;
  clock_gettime (CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}

static int ticks, stop_after;
static int tick (void *ctx)
{
  (void) ctx;
  ticks++;
  return stop_after && ticks >= stop_after;
}

/* The challenges and node's answers (the same as in tests/jsolve-reference.sh). */
static const char *const n_in[] = { "IlLiA21ny7gqA2m4p37", "eabGFpsUKuWHXGh6FR4" };
static const char *const n_want[] = { "RL7twTMlYAhbWlqMF", "OtjoLvmzT_c2ob_ei" };
static const char *const sig_in[] = {
  "NJAJEij0EwRgIhAI0KExTgjfPk-MPM9MAdzyyPRt=BM8-XO5tm5hlMCSVpAiEAv7eP3CURqZNSPow8BXXAoazVoXgeMP7gH9BdylHCwgw=gwzz" };
static const char *const sig_want[] = {
  "zzwg=wgwCHlydB9Hg7PMegXoVzaoAXXB8woPSNZqRUC3Pe7vAEiApVSCMlh5mt5OX-NMB=tRPyyzdAM9MPM-kPfjgTxEK0IAhIgRwE0jiEJA" };

int main (int argc, char **argv)
{
  const char *fix = argc > 1 ? argv[1] : "tests/fixtures", *ejs = argc > 2 ? argv[2] : "ejs/0.8.0";
  int timing = argc > 3 && !strcmp (argv[3], "-time");
  size_t liblen, corelen, plen;
  char *lib = load (ejs, "yt.solver.lib.min.js", &liblen);
  char *core = load (ejs, "yt.solver.core.min.js", &corelen);
  char *player = load (fix, "player_854a788e.js", &plen);
  char err[300], *n_out[2], *sig_out[1];
  double t0, t1, t2, t3;
  jsolve *s;

  t0 = now ();
  s = jsolve_new (lib, liblen, core, corelen, err, sizeof err);
  t1 = now ();
  CHECK (s != NULL);
  if (!s)
    {
      printf ("  %s\n", err);
      return 1;
    }
  jsolve_set_tick (s, tick, NULL);
  CHECK (!jsolve_has_player (s, "854a788e"));
  CHECK (jsolve_run (s, "854a788e", player, plen, n_in, 2, n_out, sig_in, 1, sig_out, err, sizeof err) == 0);
  t2 = now ();
  if (failures)
    printf ("  %s\n", err);
  for (int i = 0; i < 2; i++)
    CHECK (n_out[i] && !strcmp (n_out[i], n_want[i]));
  CHECK (sig_out[0] && !strcmp (sig_out[0], sig_want[0]));
  for (int i = 0; i < 2; i++)
    free (n_out[i]);
  free (sig_out[0]);
  CHECK (ticks > 0);

  /* the same player again: prepared already, no player text needed */
  CHECK (jsolve_has_player (s, "854a788e"));
  CHECK (jsolve_run (s, "854a788e", NULL, 0, n_in + 1, 1, n_out, sig_in, 1, sig_out, err, sizeof err) == 0);
  t3 = now ();
  CHECK (n_out[0] && !strcmp (n_out[0], n_want[1]) && sig_out[0] && !strcmp (sig_out[0], sig_want[0]));
  free (n_out[0]);
  free (sig_out[0]);

  /* only n; and a different player id without code is refused */
  CHECK (jsolve_run (s, "854a788e", NULL, 0, n_in, 1, n_out, NULL, 0, sig_out, err, sizeof err) == 0
         && n_out[0] && !strcmp (n_out[0], n_want[0]));
  free (n_out[0]);
  CHECK (jsolve_run (s, "deadbeef", NULL, 0, n_in, 1, n_out, NULL, 0, sig_out, err, sizeof err) == -1);

  /* stopped by the tick: a clean failure, and it still works afterwards */
  ticks = 0;
  stop_after = 3;
  CHECK (jsolve_run (s, "854a788f", player, plen, n_in, 1, n_out, NULL, 0, sig_out, err, sizeof err) == -1
         && strstr (err, "stopped") && !n_out[0]);
  stop_after = 0;
  CHECK (!jsolve_has_player (s, "854a788f") && !jsolve_has_player (s, "854a788e"));
  CHECK (jsolve_run (s, "854a788e", player, plen, n_in, 1, n_out, NULL, 0, sig_out, err, sizeof err) == 0
         && n_out[0] && !strcmp (n_out[0], n_want[0]));
  free (n_out[0]);

  /* a player that isn't one: the solver's error comes back */
  CHECK (jsolve_run (s, "bad", "var x = 1;", 10, n_in, 1, n_out, NULL, 0, sig_out, err, sizeof err) == -1);
  if (timing)
    printf ("  (not a player: %s)\n", err);
  jsolve_free (s);

  /* scripts that don't load */
  CHECK (!jsolve_new ("this is not javascript", 22, core, corelen, err, sizeof err));
  CHECK (!jsolve_new (lib, liblen, "jsc = ", 6, err, sizeof err));

  if (timing)
    printf ("load scripts %.2fs, first solve %.2fs, again (prepared) %.3fs\n", t1 - t0, t2 - t1, t3 - t2);
  free (lib);
  free (core);
  free (player);
  printf (failures ? "test_jsolve: %d FAILED\n" : "test_jsolve: all passed\n", failures);
  return failures != 0;
}
