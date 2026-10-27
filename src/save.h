/* save.h - saving a video: fetching its picture and sound streams (the
   addresses api_play found) into two files, for !FFmpeg's ffmpeg to join
   into one MP4 (the Join Obey file; ui.c). HLS streams are fetched piece
   by piece as the playlist lists them; a single (DASH) stream in 4 MB
   ranges, as yt-dlp does, since YouTube slows long requests down. Each
   request goes through http.c (the proxy, keep-alive) with the media user
   agent, and is tried three times before giving up.

   Part of TubeSearch. MIT licence. */
#ifndef TUBE_SAVE_H
#define TUBE_SAVE_H
#include "yt.h"

typedef struct save_job save_job;

/* parts: pieces fetched of total (0 while the playlists are read);
   kbytes: written so far, in KB. (A stream of unknown size counts as one
   piece until its end shows.) */
typedef void (*save_progress_fn) (int parts, int total, long kbytes, void *ctx);
/* ok 1: both files are complete; 0: err says why (the files are left for
   the caller to delete). Called once; the job is freed after it returns. */
typedef void (*save_done_fn) (int ok, const char *err, void *ctx);

/* Starts fetching p's streams: the picture (or picture and sound) into
   video_file, the sound into audio_file (unless p has none: then it's not
   written). NULL if it can't start (err says why: a live stream...). */
save_job *save_start (const yt_play *p, const char *video_file, const char *audio_file,
                      save_progress_fn progress, save_done_fn done, void *ctx, char *err, size_t errlen);
/* Stops it: done isn't called. Safe with NULL. */
void save_cancel (save_job *j);

/* ffmpeg's arguments for joining the two files into an MP4 (without
   re-encoding), into out: "-nostdin ... -i file:V -i file:A ... -f mp4
   file:OUT" (just the picture file's streams if audio_file is NULL), for
   any ffmpeg from 2.x on, riscos-ffmpeg's or an older RISC OS port. -1 if
   it won't fit. */
int save_join_args (char *out, size_t size, const char *video_file, const char *audio_file, const char *mp4);

/* Is path a complete MP4: boxes from start to end (ftyp first), with a
   moov (the index; ffmpeg writes it last, so a join cut short has none)
   and an mdat? 0, or -1 with the reason in err. */
int save_check_mp4 (const char *path, char *err, size_t errlen);

/* A file name from a video's title (UTF-8 in, the desktop's alphabet
   out, via text_display): letters RISC OS names can't have are changed or
   left out, at most max characters; "video" if nothing is left. */
void save_leaf_name (const char *title, char *out, size_t size, int max);

/* The range a single stream is fetched in (bytes); tests make it smaller. */
extern long long save_range_size;

#endif
