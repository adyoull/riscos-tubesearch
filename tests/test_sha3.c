/* SHA3-512 against hashlib's answers (tests/sha3-vectors.txt: length,
   sha3_512 of bytes i*7+3 mod 256) and yt-dlp's pinned hashes of the
   bundled solver scripts. Part of TubeSearch. MIT licence. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sha3.h"
static int failures;
#define CHECK(c) do { if (!(c)) { printf ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
static char *load (const char *p, size_t *l)
{
  FILE *f = fopen (p, "rb"); char *b; long n;
  if (!f) { printf ("FAIL: no %s\n", p); exit (1); }
  fseek (f, 0, SEEK_END); n = ftell (f); fseek (f, 0, SEEK_SET);
  b = malloc ((size_t) n + 1); *l = fread (b, 1, (size_t) n, f); fclose (f); return b;
}
int main (int argc, char **argv)
{
  const char *vec = argc > 1 ? argv[1] : "tests/sha3-vectors.txt", *ejs = argc > 2 ? argv[2] : "ejs";
  char line[300], got[129];
  FILE *f = fopen (vec, "r");
  int n = 0;
  unsigned char *buf = malloc (4096);
  for (int i = 0; i < 4096; i++) buf[i] = (unsigned char) (i * 7 + 3);
  CHECK (f != NULL);
  while (f && fgets (line, sizeof line, f))
    {
      unsigned len; char want[129];
      if (sscanf (line, "%u %128s", &len, want) != 2) continue;
      sha3_512_hex (buf, len, got);
      CHECK (!strcmp (got, want));
      n++;
    }
  if (f) fclose (f);
  CHECK (n >= 10);
  /* the bundled scripts: the hashes in ejs.json (from yt-dlp) */
  {
    char path[300], *json; size_t l, jl;
    snprintf (path, sizeof path, "%s/../ejs.json", ejs);
    json = load (path, &jl);
    json = realloc (json, jl + 1); json[jl] = 0;
    for (int i = 0; i < 2; i++)
      {
        const char *name = i ? "yt.solver.core.min.js" : "yt.solver.lib.min.js";
        char *data, key[64]; const char *h;
        snprintf (path, sizeof path, "%s/0.8.0/%s", ejs, name);
        data = load (path, &l);
        sha3_512_hex (data, l, got);
        snprintf (key, sizeof key, "\"%s\": \"", name);
        h = strstr (json, key);
        CHECK (h && !strncmp (h + strlen (key), got, 128));
        free (data);
      }
    free (json);
  }
  free (buf);
  printf (failures ? "test_sha3: %d FAILED\n" : "test_sha3: all passed (%d vectors)\n", failures ? failures : n);
  return failures != 0;
}
