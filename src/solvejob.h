/* solvejob.h - a solve, handed between TubeSearch and the helper that
   runs QuickJS (tscli solve JOB OUT), as two small JSON files:

     JOB {"player_id": "854a788e", "player_file": "...", "cache_dir": "...",
          "lib": "...", "core": "...", "version": "0.8.0",
          "n": ["..."], "sig": ["..."]}
     OUT {"n": {"challenge": "answer"}, "sig": {...}}
         or  {"error": "...", "kind": "script" | "cache" | "other"}

   kind: script, the solver scripts failed (try others); cache, the
   prepared player in the cache failed (the helper has deleted it: run the
   job again with the player's code); other, anything else (memory, a
   missing file...). A stopped solve writes no OUT.

   player_file may be "" when the prepared player is in the cache
   (solvejob_cached). The helper keeps one prepared player in cache_dir
   (the newest), named for the player and the solver's version.

   Part of TubeSearch. MIT licence. */
#ifndef TUBE_SOLVEJOB_H
#define TUBE_SOLVEJOB_H
#include <stddef.h>

typedef struct {
  const char *player_id, *player_file, *cache_dir, *lib, *core, *version;
  const char *n[2], *sig[2];
  int nn, nsig;
} solvejob;

/* The cache file for a player and solver version (RISC OS names: no dots
   in the leaf). */
void solvejob_cache_path (const char *cache_dir, const char *player_id, const char *version,
                          char *out, size_t size);
/* 1 if the prepared player is cached. */
int solvejob_cached (const char *cache_dir, const char *player_id, const char *version);

/* Writes the job. 0, or -1. */
int solvejob_write (const char *path, const solvejob *j);

/* Reads the helper's answers: n_out/sig_out in the job's order (malloc'd).
   0, or -1 with the reason in err and *kind (SOLVE_*). */
enum { SOLVE_OK, SOLVE_SCRIPT, SOLVE_CACHE, SOLVE_OTHER };
int solvejob_read_result (const char *path, const solvejob *j, char **n_out, char **sig_out,
                          char *err, size_t errlen, int *kind);

/* The helper's side (solverun.c, with QuickJS): runs the job in JOB and
   writes OUT. tick (may be NULL) is called during the solve. 0 if solved. */
int solvejob_run (const char *job_path, const char *out_path, int (*tick) (void *), void *ctx);

#endif
