/* text.h - UTF-8 text for the RISC OS desktop. Part of TubeSearch. MIT licence. */
#ifndef TUBE_TEXT_H
#define TUBE_TEXT_H
#include <stddef.h>

/* 1: the desktop shows UTF-8 (alphabet 111), so text is left as it is;
   0 (the default): it is converted to the alphabet text_set_alphabet
   chose (Latin-1 at first). */
extern int text_utf8_desktop;

/* The desktop's alphabet, by its RISC OS number (OS_Byte 71): 101 Latin-1
   to 116 Latin-10 (Latin-2, Cyrillic, Greek, Hebrew, Latin-9...), 111
   UTF-8. 0 if it is one TubeSearch knows; -1 if not (then only ASCII is
   shown, the rest as '?'). */
int text_set_alphabet (int number);
/* The alphabet's name ("Latin2", "UTF8", "ASCII"). */
const char *text_alphabet_name (void);

/* A copy of the UTF-8 text s in the desktop's alphabet (malloc'd), with
   control characters made spaces. Characters the alphabet lacks become
   the nearest thing (quotes, dashes, bullet = middle dot) or '?'. */
char *text_display (const char *s);

/* Text in the desktop's alphabet (typed, or shown) as UTF-8 (malloc'd). */
char *text_to_utf8 (const char *s);

/* The same into a buffer (not s itself: the text can grow); returns the length. */
size_t text_display_buf (const char *s, char *out, size_t size);

#endif
