/* solvejob.c - see solvejob.h (the files; no QuickJS here).
   Part of TubeSearch. MIT licence. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solvejob.h"
#include "json.h"

#ifdef __riscos
#define SEP "."
#else
#define SEP "/"
#endif

void solvejob_cache_path (const char *cache_dir, const char *player_id, const char *version,
                          char *out, size_t size)
{
  char v[32];
  size_t i;
  snprintf (v, sizeof v, "%s", version ? version : "0");
  for (i = 0; v[i]; i++)
    if (v[i] == '.' || v[i] == '/')
      v[i] = '-';
  snprintf (out, size, "%s" SEP "%s-%s", cache_dir, player_id, v);
}

int solvejob_cached (const char *cache_dir, const char *player_id, const char *version)
{
  char path[512];
  FILE *f;
  solvejob_cache_path (cache_dir, player_id, version, path, sizeof path);
  if (!(f = fopen (path, "rb")))
    return 0;
  fclose (f);
  return 1;
}

static void put_str (FILE *f, const char *s)
{
  size_t n = json_quote (s ? s : "", NULL, 0);
  char *q = malloc (n + 1);
  if (q)
    {
      json_quote (s ? s : "", q, n + 1);
      fputs (q, f);
      free (q);
    }
}

static void put_list (FILE *f, const char *const *v, int count)
{
  fputc ('[', f);
  for (int i = 0; i < count; i++)
    {
      if (i)
        fputc (',', f);
      put_str (f, v[i]);
    }
  fputc (']', f);
}

int solvejob_write (const char *path, const solvejob *j)
{
  FILE *f = fopen (path, "w");
  if (!f)
    return -1;
  fputs ("{\"player_id\":", f); put_str (f, j->player_id);
  fputs (",\"player_file\":", f); put_str (f, j->player_file);
  fputs (",\"cache_dir\":", f); put_str (f, j->cache_dir);
  fputs (",\"lib\":", f); put_str (f, j->lib);
  fputs (",\"core\":", f); put_str (f, j->core);
  fputs (",\"version\":", f); put_str (f, j->version);
  fputs (",\"n\":", f); put_list (f, j->n, j->nn);
  fputs (",\"sig\":", f); put_list (f, j->sig, j->nsig);
  fputs ("}\n", f);
  return fclose (f) ? -1 : 0;
}

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

int solvejob_read_result (const char *path, const solvejob *j, char **n_out, char **sig_out,
                          char *err, size_t errlen, int *kind)
{
  size_t len;
  char *t = slurp (path, &len), jerr[96];
  jdoc *d;
  const jnode *root, *n, *sig;
  int missing = 0;
  *kind = SOLVE_OTHER;
  for (int i = 0; i < j->nn; i++)
    n_out[i] = NULL;
  for (int i = 0; i < j->nsig; i++)
    sig_out[i] = NULL;
  if (!t)
    {
      snprintf (err, errlen, "the JavaScript helper gave no answer");
      return -1;
    }
  d = json_parse (t, len, jerr, sizeof jerr);
  free (t);
  if (!d)
    {
      snprintf (err, errlen, "the JavaScript helper's answer wasn't readable (%s)", jerr);
      return -1;
    }
  root = json_root (d);
  if (j_str (j_get (root, "error")))
    {
      const char *k = j_str (j_get (root, "kind"));
      *kind = !k ? SOLVE_OTHER : !strcmp (k, "script") ? SOLVE_SCRIPT : !strcmp (k, "cache") ? SOLVE_CACHE : SOLVE_OTHER;
      snprintf (err, errlen, "%s", j_str (j_get (root, "error")));
      json_free (d);
      return -1;
    }
  n = j_get (root, "n");
  sig = j_get (root, "sig");
  for (int i = 0; i < j->nn; i++)
    missing += !(n_out[i] = j_str (j_get (n, j->n[i])) ? strdup (j_str (j_get (n, j->n[i]))) : NULL);
  for (int i = 0; i < j->nsig; i++)
    missing += !(sig_out[i] = j_str (j_get (sig, j->sig[i])) ? strdup (j_str (j_get (sig, j->sig[i]))) : NULL);
  json_free (d);
  if (missing)
    {
      for (int i = 0; i < j->nn; i++)
        free (n_out[i]), n_out[i] = NULL;
      for (int i = 0; i < j->nsig; i++)
        free (sig_out[i]), sig_out[i] = NULL;
      snprintf (err, errlen, "the JavaScript helper left %d challenge%s unanswered", missing, missing == 1 ? "" : "s");
      *kind = SOLVE_SCRIPT;
      return -1;
    }
  *kind = SOLVE_OK;
  return 0;
}
