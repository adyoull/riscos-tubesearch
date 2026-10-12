/* thumbs.h - the pictures beside the videos: fetched (http.h), turned
   from JPEG into 32bpp sprites by SpriteExtend, kept for a while (the
   least recently shown are dropped first) and plotted.
   Part of Tube. MIT licence. */
#ifndef TUBE_THUMBS_H
#define TUBE_THUMBS_H

typedef struct thumb thumb;

/* Called when a picture has arrived (url is the one asked for). */
void thumbs_init (void (*arrived) (const char *url));

/* The picture at url, made w x h pixels (90 dpi: 2 OS units a pixel);
   fetched if it isn't here yet. NULL if url is empty or nothing is free. */
thumb *thumb_get (const char *url, int w, int h);
int thumb_ready (const thumb *t);         /* 1 here, 0 coming, -1 failed */
/* Plots it with its bottom left at screen (x, y). */
void thumb_plot (thumb *t, int x, int y);

/* A new page: forget the fetches not started yet. */
void thumbs_cancel_pending (void);

void thumbs_free_all (void);

#endif
