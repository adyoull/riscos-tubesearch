/* ui.h - Tube's desktop front end: an icon bar icon, a window with a
   toolbar (Back, the search box, Search, and a status line) over a grid
   of videos with their pictures. Select plays a video in Reel; Adjust
   shows the videos related to it; clicking a channel's name opens its
   videos; Menu has all of these.

   main.c only polls; everything else is here. Part of Tube. MIT. */
#ifndef TUBE_UI_H
#define TUBE_UI_H

/* Wimp_Initialise and the windows. 0 if all went well. */
int ui_init (void);
/* One event from Wimp_Poll. Returns 1 when the program should end. */
int ui_event (int reason, int *block);
/* 1 while network work is going on (null events wanted). */
int ui_busy (void);
void ui_close (void);
void ui_report (const char *text);

#endif
