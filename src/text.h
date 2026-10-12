/* text.h - UTF-8 text for the RISC OS desktop. Part of TubeSearch. MIT licence. */
#ifndef TUBE_TEXT_H
#define TUBE_TEXT_H
#include <stddef.h>

/* 1: the desktop shows UTF-8 (alphabet 111), so text is left as it is;
   0 (the default): it is converted to Latin-1. */
extern int text_utf8_desktop;

/* A copy of the UTF-8 text s in the desktop's alphabet (malloc'd), with
   control characters made spaces. Characters Latin-1 lacks become the
   nearest thing (quotes, dashes, bullet = middle dot) or '?'. */
char *text_display (const char *s);

/* The same into a buffer; returns the length. */
size_t text_display_buf (const char *s, char *out, size_t size);

#endif
