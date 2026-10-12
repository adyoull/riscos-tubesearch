/* solverun.c - the helper's side of solvejob.h: QuickJS runs yt-dlp's
   solver on the job and the answers are written out.
   Part of TubeSearch. MIT licence. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "solvejob.h"
#include "jsolve.h"
#include "json.h"

#ifdef __riscos
#define SEP "."
#else
#define SEP "/"
#endif

static char *slurp (const char *path, size_t *len)
{
  FILE *f = fopen (path, "rb");
  char *b;
  long n;
  if (!f)
    return NULL;
  if (fseek (f, 0, SEEK_END) || (n = ftell (f)) < 0 || fseek (f, 0, SEEK_SET) || !(b = malloc ((size_t) n + 1)))
    {
      fclose (f);
      return NULL;
    }
  *len = fread (b, 1, (size_t) n, f);
  b[*len] = 0;
  fclose (f);
  return b;
}

static void put_str (FILE *f, const char *s)
{
  size_t n = json_quote (s, NULL, 0);
  char *q = malloc (n + 1);
  if (q)
    {
      json_quote (s, q, n + 1);
      fputs (q, f);
      free (q);
    }
}

/* Writes OUT through a temporary name, so the reader never sees half of it. */
static int write_out (const char *out_path, const char *error, const char *kind, const solvejob *j,
                      char **n_out, char **sig_out)
{
  char tmp[600];
  FILE *f;
  snprintf (tmp, sizeof tmp, "%s-tmp", out_path);
  if (!(f = fopen (tmp, "w")))
    return -1;
  if (error)
    {
      fputs ("{\"error\":", f);
      put_str (f, error);
      fputs (",\"kind\":", f);
      put_str (f, kind);
      fputs ("}\n", f);
    }
  else
    {
      fputs ("{\"n\":{", f);
      for (int i = 0; i < j->nn; i++)
        {
          if (i)
            fputc (',', f);
          put_str (f, j->n[i]);
          fputc (':', f);
          put_str (f, n_out[i]);
        }
      fputs ("},\"sig\":{", f);
      for (int i = 0; i < j->nsig; i++)
        {
          if (i)
            fputc (',', f);
          put_str (f, j->sig[i]);
          fputc (':', f);
          put_str (f, sig_out[i]);
        }
      fputs ("}}\n", f);
    }
  if (fclose (f))
    return -1;
  remove (out_path);
  return rename (tmp, out_path);
}

/* Only the newest prepared player is kept: the others in the cache go. */
static void prune_cache (const char *cache_dir, const char *keep_leaf)
{
  DIR *d = opendir (cache_dir);
  struct dirent *e;
  char path[1200];
  if (!d)
    return;
  while ((e = readdir (d)))
    {
      size_t l = strlen (e->d_name);
      if (e->d_name[0] == '.' || !strcmp (e->d_name, keep_leaf))
        continue;
      /* prepared players only: 8+ hex digits, '-', the version */
      if (l < 10 || !strchr (e->d_name, '-') || (l > 4 && !strcmp (e->d_name + l - 4, "-sts")))
        continue;
      snprintf (path, sizeof path, "%.600s" SEP "%.500s", cache_dir, e->d_name);
      remove (path);
    }
  closedir (d);
}

/* Failures that aren't the scripts' (or the cache's) fault. */
static int environment (const char *err)
{
  return !strcmp (err, "stopped") || strstr (err, "out of memory") || strstr (err, "stack overflow")
         || strstr (err, "couldn't be read");
}

int solvejob_run (const char *job_path, const char *out_path, int (*tick) (void *), void *ctx)
{
  char err[400], jerr[96], cache[512];
  size_t len, liblen, corelen, plen = 0;
  char *text = slurp (job_path, &len), *lib = NULL, *core = NULL, *player = NULL;
  char *n_out[2] = { 0 }, *sig_out[2] = { 0 };
  jdoc *d = NULL;
  const jnode *root;
  jsolve *s = NULL;
  solvejob j;
  const char *kind = "other";
  int rc = -1, cached;

  memset (&j, 0, sizeof j);
  err[0] = 0;
  if (!text || !(d = json_parse (text, len, jerr, sizeof jerr)))
    {
      snprintf (err, sizeof err, "the solve job couldn't be read (%s)", text ? jerr : job_path);
      goto out;
    }
  root = json_root (d);
  j.player_id = j_str (j_get (root, "player_id"));
  j.player_file = j_str (j_get (root, "player_file"));
  j.cache_dir = j_str (j_get (root, "cache_dir"));
  j.lib = j_str (j_get (root, "lib"));
  j.core = j_str (j_get (root, "core"));
  j.version = j_str (j_get (root, "version"));
  for (int i = 0; i < 2; i++)
    {
      if (j_str (j_idx (j_get (root, "n"), i)))
        j.n[j.nn++] = j_str (j_idx (j_get (root, "n"), i));
      if (j_str (j_idx (j_get (root, "sig"), i)))
        j.sig[j.nsig++] = j_str (j_idx (j_get (root, "sig"), i));
    }
  if (!j.player_id || !j.lib || !j.core || !j.cache_dir)
    {
      snprintf (err, sizeof err, "the solve job is incomplete");
      goto out;
    }
  if (!(lib = slurp (j.lib, &liblen)) || !(core = slurp (j.core, &corelen)))
    {
      snprintf (err, sizeof err, "the solver scripts couldn't be read (%s)", lib ? j.core : j.lib);
      goto out;
    }
  if (!(s = jsolve_new (lib, liblen, core, corelen, err, sizeof err)))
    {
      kind = "script";
      goto out;
    }
  jsolve_set_tick (s, tick, ctx);
  solvejob_cache_path (j.cache_dir, j.player_id, j.version, cache, sizeof cache);
  cached = jsolve_load_player (s, j.player_id, cache) == 0;
  if (cached)
    {
      rc = jsolve_run (s, j.player_id, NULL, 0, j.n, j.nn, n_out, j.sig, j.nsig, sig_out, err, sizeof err);
      if (rc && !environment (err))
        {
          /* the prepared player didn't work: never again; from the player itself if we have it */
          remove (cache);
          kind = "cache";
        }
    }
  if (rc && j.player_file && *j.player_file && !environment (err))
    {
      /* not cached (or the cached copy didn't work): from the player itself */
      kind = "script";
      if (!(player = slurp (j.player_file, &plen)))
        {
          snprintf (err, sizeof err, "the player's JavaScript couldn't be read");
          goto out;
        }
      rc = jsolve_run (s, j.player_id, player, plen, j.n, j.nn, n_out, j.sig, j.nsig, sig_out, err, sizeof err);
      if (rc == 0)
        {
          const char *leaf = strrchr (cache, SEP[0]);
          if (jsolve_save_player (s, cache) == 0)
            prune_cache (j.cache_dir, leaf ? leaf + 1 : cache);
        }
    }
  else if (rc && !cached)
    {
      /* (the job thought it was cached, but it has gone since: as a bad cache) */
      snprintf (err, sizeof err, "the prepared player has gone and its JavaScript wasn't given");
      kind = "cache";
    }
out:
  if (rc && !strcmp (kind, "script") && environment (err))
    kind = "other";
  /* (a stopped solve isn't wanted: no answer) */
  if (!(rc && !strcmp (err, "stopped")))
    write_out (out_path, rc ? (err[0] ? err : "the solve failed") : NULL, kind, &j, n_out, sig_out);
  for (int i = 0; i < 2; i++)
    {
      free (n_out[i]);
      free (sig_out[i]);
    }
  jsolve_free (s);
  json_free (d);
  free (text);
  free (lib);
  free (core);
  free (player);
  return rc;
}
