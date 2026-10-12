/* ejsupd.c - see ejsupd.h. Part of TubeSearch. MIT licence. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include "ejsupd.h"
#include "http.h"
#include "json.h"
#include "sha3.h"
#include "yt.h"

#ifdef __riscos
#define SEP "."
#else
#define SEP "/"
#endif

#define TAG_EJS 60
#define DAY (24 * 60 * 60)

static char bundled[400], state[400];

static struct {
  int busy;
  char version[32];
  char hash[2][129];
  char *data[2];
  size_t len[2];
  int got;
  void (*done) (int, const char *, void *);
  void *ctx;
} job;

static const char *const remote_name[2] = { "yt.solver.lib.min.js", "yt.solver.core.min.js" };
static const char *const local_name[2] = { "lib", "core" };

void ejsupd_init (const char *bundled_dir, const char *state_dir)
{
  snprintf (bundled, sizeof bundled, "%s", bundled_dir ? bundled_dir : "");
  snprintf (state, sizeof state, "%s", state_dir ? state_dir : "");
}

static void path (const char *dir, const char *leaf, char *out, size_t size)
{
  snprintf (out, size, "%s" SEP "%s", dir, leaf);
}

static int exists (const char *p)
{
  FILE *f = fopen (p, "rb");
  if (!f)
    return 0;
  fclose (f);
  return 1;
}

/* The first line of a small file (without the newline). */
static int read_line (const char *p, char *out, size_t size)
{
  FILE *f = fopen (p, "r");
  out[0] = 0;
  if (!f)
    return -1;
  if (!fgets (out, (int) size, f))
    out[0] = 0;
  fclose (f);
  out[strcspn (out, "\r\n")] = 0;
  return out[0] ? 0 : -1;
}

/* The State file: "checked <time>" and "bad <version>" lines. */
static void read_state (long *checked, char *bad, size_t badsize)
{
  char p[450], line[80];
  FILE *f;
  *checked = 0;
  bad[0] = 0;
  path (state, "State", p, sizeof p);
  if (!(f = fopen (p, "r")))
    return;
  while (fgets (line, sizeof line, f))
    {
      line[strcspn (line, "\r\n")] = 0;
      if (!strncmp (line, "checked ", 8))
        *checked = atol (line + 8);
      else if (!strncmp (line, "bad ", 4))
        snprintf (bad, badsize, "%.31s", line + 4);
    }
  fclose (f);
}

static void write_state (long checked, const char *bad)
{
  char p[450];
  FILE *f;
  path (state, "State", p, sizeof p);
  if ((f = fopen (p, "w")))
    {
      fprintf (f, "checked %ld\n", checked);
      if (bad && *bad)
        fprintf (f, "bad %s\n", bad);
      fclose (f);
    }
}

static int downloaded_version (char *v, size_t size)
{
  char p[450], bad[32];
  long checked;
  path (state, "Version", p, sizeof p);
  if (!state[0] || read_line (p, v, size))
    return -1;
  read_state (&checked, bad, sizeof bad);
  for (int i = 0; i < 2; i++)
    {
      path (state, local_name[i], p, sizeof p);
      if (!exists (p))
        return -1;
    }
  return strcmp (v, bad) ? 0 : -1;
}

/* <0, 0, >0 as version a is older, the same, newer than b: dotted numbers,
   then anything after them compared as text ("0.8.1-test" > "0.8.1"). */
static int version_cmp (const char *a, const char *b)
{
  while (*a || *b)
    {
      if (isdigit ((unsigned char) *a) && isdigit ((unsigned char) *b))
        {
          long x = strtol (a, (char **) &a, 10), y = strtol (b, (char **) &b, 10);
          if (x != y)
            return x < y ? -1 : 1;
        }
      else if (*a == '.' && *b == '.')
        a++, b++;
      else
        return strcmp (a, b) < 0 ? -1 : strcmp (a, b) > 0;
    }
  return 0;
}

static int bundled_version (char *v, size_t size)
{
  char p[450];
  path (bundled, "Version", p, sizeof p);
  return read_line (p, v, size);
}

int ejsupd_scripts (char *lib, size_t libsize, char *core, size_t coresize, char *version, size_t versize)
{
  char p[450], bv[32];
  /* the downloaded copy when it is good and newer than the bundled one */
  if (downloaded_version (version, versize) == 0
      && (bundled_version (bv, sizeof bv) || version_cmp (version, bv) > 0))
    {
      path (state, "lib", lib, libsize);
      path (state, "core", core, coresize);
      return 0;
    }
  (void) p;
  if (bundled_version (version, versize))
    return -1;
  path (bundled, "lib", lib, libsize);
  path (bundled, "core", core, coresize);
  return exists (lib) && exists (core) ? 0 : -1;
}

void ejsupd_failed (const char *version)
{
  char v[32];
  long checked, now = (long) time (NULL);
  char bad[32];
  if (downloaded_version (v, sizeof v) || strcmp (v, version))
    return;                                 /* (the bundled copy: nothing else to fall back to) */
  read_state (&checked, bad, sizeof bad);
  write_state (checked ? checked : now, version);
}

/* ---- checking --------------------------------------------------------------- */

static void finish (int updated, const char *fmt, const char *arg)
{
  char msg[300];
  void (*done) (int, const char *, void *) = job.done;
  void *ctx = job.ctx;
  snprintf (msg, sizeof msg, fmt, arg ? arg : "");
  for (int i = 0; i < 2; i++)
    {
      free (job.data[i]);
      job.data[i] = NULL;
    }
  job.busy = 0;
  if (done)
    done (updated, msg, ctx);
}

static int plain_version (const char *v)
{
  if (!*v || strlen (v) > 20)
    return 0;
  for (; *v; v++)
    if (!((*v >= '0' && *v <= '9') || (*v >= 'a' && *v <= 'z') || (*v >= 'A' && *v <= 'Z') || *v == '.' || *v == '-'))
      return 0;
  return 1;
}

/* Both files are here and checked: into state_dir. */
static void install (void)
{
  char p[450], tmp[450];
  FILE *f;
  long checked;
  char bad[32];
  for (int i = 0; i < 2; i++)
    {
      char leaf[16];
      snprintf (leaf, sizeof leaf, "%s-new", local_name[i]);
      path (state, leaf, tmp, sizeof tmp);
      if (!(f = fopen (tmp, "wb")) || fwrite (job.data[i], 1, job.len[i], f) != job.len[i] || fclose (f))
        {
          finish (0, "couldn't save the new solver scripts in %s", state);
          return;
        }
    }
  for (int i = 0; i < 2; i++)
    {
      char leaf[16];
      snprintf (leaf, sizeof leaf, "%s-new", local_name[i]);
      path (state, leaf, tmp, sizeof tmp);
      path (state, local_name[i], p, sizeof p);
      remove (p);
      if (rename (tmp, p))
        {
          finish (0, "couldn't put the new solver scripts in place in %s", state);
          return;
        }
    }
  path (state, "Version", p, sizeof p);
  if ((f = fopen (p, "w")))
    {
      fprintf (f, "%s\n", job.version);
      fclose (f);
    }
  read_state (&checked, bad, sizeof bad);
  write_state ((long) time (NULL), strcmp (bad, job.version) ? bad : "");
  finish (1, "solver scripts %s taken", job.version);
}

static void file_done (http_req *r, void *ctx)
{
  int i = (int) (long) ctx;
  char got[129];
  size_t len;
  if (!job.busy)
    return;
  if (http_status (r) != 200)
    {
      finish (0, "the solver script couldn't be downloaded (%s)", http_status (r) ? "not found" : http_error (r));
      return;
    }
  job.data[i] = http_take_body (r, &len);
  job.len[i] = len;
  sha3_512_hex (job.data[i], len, got);
  if (strcmp (got, job.hash[i]))
    {
      finish (0, "a downloaded solver script didn't match the manifest's hash (%s): not used", remote_name[i]);
      return;
    }
  if (++job.got == 2)
    install ();
}

static int fetch (const char *url, http_done_fn fn, void *ctx)
{
  char host[96];
  const char *p, *slash;
  if (strncmp (url, "https://", 8))
    return -1;
  p = url + 8;
  slash = strchr (p, '/');
  if (!slash || (size_t) (slash - p) >= sizeof host)
    return -1;
  memcpy (host, p, (size_t) (slash - p));
  host[slash - p] = 0;
  return http_get (host, slash, "User-Agent: TubeSearch\r\nAccept: */*\r\n", TAG_EJS, fn, ctx) ? 0 : -1;
}

static void manifest_done (http_req *r, void *ctx)
{
  size_t len;
  const char *body = http_body (r, &len);
  char jerr[96], have[32], bad[32];
  const char *v;
  jdoc *d;
  long checked;
  (void) ctx;
  if (!job.busy)
    return;
  if (http_status (r) != 200)
    {
      finish (0, "the solver manifest couldn't be read (%s)", http_status (r) ? "not found" : http_error (r));
      return;
    }
  if (!(d = json_parse (body, len, jerr, sizeof jerr)))
    {
      finish (0, "the solver manifest wasn't readable (%s)", jerr);
      return;
    }
  v = j_str (j_get (json_root (d), "version"));
  for (int i = 0; i < 2; i++)
    {
      const char *h = j_str (j_get (j_get (json_root (d), "files"), remote_name[i]));
      snprintf (job.hash[i], sizeof job.hash[i], "%s", h && strlen (h) == 128 ? h : "");
    }
  if (!v || !plain_version (v) || !job.hash[0][0] || !job.hash[1][0])
    {
      json_free (d);
      finish (0, "the solver manifest is incomplete", NULL);
      return;
    }
  snprintf (job.version, sizeof job.version, "%s", v);
  json_free (d);
  read_state (&checked, bad, sizeof bad);
  /* already this version (downloaded or bundled), or known to be bad */
  if (!strcmp (job.version, bad)
      || (downloaded_version (have, sizeof have) == 0 && !strcmp (have, job.version)))
    {
      write_state ((long) time (NULL), bad);
      finish (0, "solver scripts %s are up to date", job.version);
      return;
    }
  {
    char p[450], bv[32];
    path (bundled, "Version", p, sizeof p);
    if (read_line (p, bv, sizeof bv) == 0 && !strcmp (bv, job.version))
      {
        write_state ((long) time (NULL), bad);
        finish (0, "solver scripts %s (the copy in TubeSearch) are up to date", job.version);
        return;
      }
  }
  for (int i = 0; i < 2; i++)
    {
      char url[512];
      snprintf (url, sizeof url, "%s/%s/%s", yt_config_get ("ejs_download_base"), job.version, remote_name[i]);
      if (fetch (url, file_done, (void *) (long) i))
        {
          finish (0, "the solver scripts couldn't be asked for", NULL);
          return;
        }
    }
}

void ejsupd_check (int force, void (*done) (int, const char *, void *), void *ctx)
{
  long checked, now = (long) time (NULL);
  char bad[32];
  if (job.busy || !state[0])
    return;
  if (!force && !strcmp (yt_config_get ("ejs_update"), "never"))
    return;
  read_state (&checked, bad, sizeof bad);
  if (!force && checked && now - checked < DAY && now >= checked)
    return;
  memset (&job, 0, sizeof job);
  job.busy = 1;
  job.done = done;
  job.ctx = ctx;
  if (fetch (yt_config_get ("ejs_manifest_url"), manifest_done, NULL))
    finish (0, "the solver manifest address in Config isn't https://", NULL);
}
