/* text.c - see text.h. Part of Tube. MIT licence. */
#include <stdlib.h>
#include <string.h>
#include "text.h"

int text_utf8_desktop;

/* The next character of UTF-8 text (bad bytes come back as themselves). */
static unsigned next_char (const unsigned char **pp)
{
  const unsigned char *p = *pp;
  unsigned c = *p++;
  int more = 0;
  if (c >= 0xF0 && c < 0xF8) { c &= 7; more = 3; }
  else if (c >= 0xE0) { c &= 15; more = 2; }
  else if (c >= 0xC0) { c &= 31; more = 1; }
  else if (c >= 0x80) { *pp = p; return 0xFFFD; }
  while (more--)
    {
      if ((*p & 0xC0) != 0x80)
        {
          *pp = p;
          return 0xFFFD;
        }
      c = (c << 6) | (*p++ & 0x3F);
    }
  *pp = p;
  return c;
}

static const char *latin1_for (unsigned c)
{
  switch (c)
    {
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: return "'";
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: return "\"";
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: return "-";
    case 0x2022: case 0x2027: case 0x30FB: return "\xB7";
    case 0x2026: return "...";
    case 0x00A0: case 0x2002: case 0x2003: case 0x2009: case 0x200A: case 0x202F: return " ";
    case 0x200B: case 0x200C: case 0x200D: case 0x2060: case 0xFE0F: case 0xFEFF: return "";
    case 0x20AC: return "EUR";
    case 0x2122: return "TM";
    }
  return NULL;
}

size_t text_display_buf (const char *s, char *out, size_t size)
{
  const unsigned char *p = (const unsigned char *) s;
  size_t at = 0;
  if (!size)
    return 0;
  while (*p && at < size - 1)
    {
      const unsigned char *start = p;
      unsigned c = next_char (&p);
      if (c < 32 || c == 127)
        c = ' ';
      if (text_utf8_desktop)
        {
          size_t l = (size_t) (p - start);
          if (c == ' ')
            out[at++] = ' ';
          else if (at + l < size)
            {
              memcpy (out + at, start, l);
              at += l;
            }
          else
            break;
        }
      else if (c < 256 && c != 0xAD)
        out[at++] = (char) c;
      else
        {
          const char *r = latin1_for (c);
          if (!r)
            {
              /* emoji and other symbols: leave them out rather than
                 filling titles with question marks */
              if (c >= 0x1F000 || (c >= 0x2600 && c < 0x2800) || (c >= 0x2B00 && c < 0x2C00))
                continue;
              r = "?";
            }
          while (*r && at < size - 1)
            out[at++] = *r++;
        }
    }
  out[at] = 0;
  /* runs of spaces (from removed symbols) become one; none at the ends */
  {
    size_t i, j = 0;
    for (i = 0; i < at; i++)
      if (!(out[i] == ' ' && (j == 0 || out[j - 1] == ' ')))
        out[j++] = out[i];
    while (j > 0 && out[j - 1] == ' ')
      j--;
    out[j] = 0;
    at = j;
  }
  return at;
}

char *text_display (const char *s)
{
  size_t l = strlen (s ? s : "") * 3 + 1;
  char *o = malloc (l);
  if (o)
    text_display_buf (s ? s : "", o, l);
  return o;
}
