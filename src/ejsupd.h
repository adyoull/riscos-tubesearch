/* ejsupd.h - keeps yt-dlp's solver scripts up to date.

   TubeSearch carries a copy (bundled_dir: lib, core, Version). Once a day
   (Config: ejs_update) it reads a small manifest (Config:
   ejs_manifest_url, by default ejs.json in TubeSearch's own GitHub
   repository, which a weekly job keeps at the version yt-dlp itself
   pins): {"version": "0.8.0", "files": {"yt.solver.lib.min.js": SHA3-512,
   "yt.solver.core.min.js": SHA3-512}}. A newer version is downloaded from
   yt-dlp/ejs's GitHub releases, checked against those hashes, and kept in
   state_dir (lib, core, Version). If it then fails, it is marked bad and
   the bundled copy is used until the manifest names another version.

   Part of TubeSearch. MIT licence. */
#ifndef TUBE_EJSUPD_H
#define TUBE_EJSUPD_H
#include <stddef.h>

void ejsupd_init (const char *bundled_dir, const char *state_dir);

/* The scripts to use now (the downloaded ones when they're good, else the
   bundled ones). 0, or -1 if there are none. */
int ejsupd_scripts (char *lib, size_t libsize, char *core, size_t coresize, char *version, size_t versize);

/* The scripts of this version failed to solve: don't use them again. */
void ejsupd_failed (const char *version);

/* Checks for a newer version (if a day has passed since the last check,
   or force). done (may be NULL) is called at the end with what happened;
   updated is 1 if new scripts were taken. Driven by http_poll(). */
void ejsupd_check (int force, void (*done) (int updated, const char *msg, void *ctx), void *ctx);

#endif
