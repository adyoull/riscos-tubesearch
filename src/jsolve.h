/* jsolve.h - YouTube's JavaScript challenges (the "n" and signature
   values in stream addresses), solved by yt-dlp's solver scripts
   (github.com/yt-dlp/ejs: yt.solver.lib.min.js and yt.solver.core.min.js,
   Unlicense) running in an embedded QuickJS.

   QuickJS is built without its std/os modules: the scripts can only
   compute, not read files or reach the network.

   A solver keeps the last player it was given, prepared ("preprocessed"),
   so later challenges from the same player are much quicker.

   Part of TubeSearch. MIT licence. */
#ifndef TUBE_JSOLVE_H
#define TUBE_JSOLVE_H
#include <stddef.h>

typedef struct jsolve jsolve;

/* Loads the two solver scripts. NULL with a message in err. */
jsolve *jsolve_new (const char *lib, size_t liblen, const char *core, size_t corelen,
                    char *err, size_t errlen);
void jsolve_free (jsolve *s);

/* While solving, tick is called now and then (to keep the hourglass
   going); returning non-zero stops the solve. */
void jsolve_set_tick (jsolve *s, int (*tick) (void *ctx), void *ctx);

/* 1 if the player player_id is already prepared (no player text needed). */
int jsolve_has_player (const jsolve *s, const char *player_id);

/* The prepared player, saved to path (so a later run, or another program,
   needn't prepare it again), or loaded from it for player_id. 0, or -1. */
int jsolve_save_player (jsolve *s, const char *path);
int jsolve_load_player (jsolve *s, const char *player_id, const char *path);

/* Solves the challenges. player (len bytes) may be NULL when
   jsolve_has_player. n[] and sig[] are the challenges (nn and nsig of
   them); the answers go into n_out[] and sig_out[] (malloc'd, or NULL for
   one that wasn't solved). 0 if all were solved, else -1 with err. */
int jsolve_run (jsolve *s, const char *player_id, const char *player, size_t len,
                const char *const *n, int nn, char **n_out,
                const char *const *sig, int nsig, char **sig_out,
                char *err, size_t errlen);

#endif
