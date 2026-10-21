/* text.c - see text.h. Part of TubeSearch. MIT licence. */
#include <stdlib.h>
#include <string.h>
#include "text.h"
#include "alphabets.h"

int text_utf8_desktop;
static int alphabet;                       /* index in alphabets[] (0: Latin-1) */
static int ascii_only;                     /* an alphabet not known: only ASCII is sure */

int text_set_alphabet (int number)
{
  text_utf8_desktop = number == 111;
  alphabet = 0;
  ascii_only = 0;
  if (number == 111)
    return 0;
  for (size_t i = 0; i < sizeof alphabets / sizeof alphabets[0]; i++)
    if (alphabets[i].number == number)
      {
        alphabet = (int) i;
        return 0;
      }
  ascii_only = number != 101 && number > 0;      /* (one TubeSearch doesn't know: Cyrillic2, BFont...) */
  return -1;
}

const char *text_alphabet_name (void)
{
  return text_utf8_desktop ? "UTF8" : ascii_only ? "ASCII" : alphabets[alphabet].name;
}

/* The desktop's code for Unicode character c, or 0 if it hasn't one. */
static int code_for (unsigned c)
{
  if (c < 0xA0)
    return c >= 0x20 && c < 0x7F ? (int) c : 0;
  if (ascii_only)
    return 0;
  for (int i = 0; i < 96; i++)
    if (alphabets[alphabet].hi[i] == c)
      return 0xA0 + i;
  return 0;
}

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

/* The nearest thing for a character the alphabet lacks (in ASCII), or NULL. */
static const char *latin1_for (unsigned c)
{
  switch (c)
    {
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: return "'";
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: return "\"";
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: return "-";
    case 0x2022: case 0x2027: case 0x30FB: case 0x00B7: return code_for (0xB7) ? NULL : "-";
    case 0x2026: return "...";
    case 0x00A0: case 0x2002: case 0x2003: case 0x2009: case 0x200A: case 0x202F: return " ";
    case 0x200B: case 0x200C: case 0x200D: case 0x2060: case 0xFE0F: case 0xFEFF: return "";
    case 0x200E: case 0x200F: case 0x061C: case 0x202A: case 0x202B: case 0x202C: case 0x202D: case 0x202E:
    case 0x2066: case 0x2067: case 0x2068: case 0x2069: return "";   /* (direction marks) */
    case 0x20AC: return "EUR";
    case 0x2122: return "TM";
    case 0x00A9: return "(C)";
    case 0x00AE: return "(R)";
    case 0x00AB: return "<<";
    case 0x00BB: return ">>";
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
      else if (c == 0xAD)
        continue;                                    /* (a soft hyphen) */
      else if (code_for (c))
        out[at++] = (char) code_for (c);
      else
        {
          const char *r = latin1_for (c);
          if (c == 0x2022 || c == 0x2027 || c == 0x30FB)
            {
              if (!r)
                {
                  out[at++] = (char) code_for (0xB7);  /* the middle dot */
                  continue;
                }
            }
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

char *text_to_utf8 (const char *s)
{
  size_t n = strlen (s ? s : "");
  char *o = malloc (n * 3 + 1), *w = o;
  if (!o)
    return NULL;
  for (const unsigned char *r = (const unsigned char *) (s ? s : ""); *r; r++)
    {
      unsigned c = *r;
      if (c < 0x80 || text_utf8_desktop)
        {
          *w++ = (char) c;
          continue;
        }
      c = c >= 0xA0 && !ascii_only ? alphabets[alphabet].hi[c - 0xA0] : 0;
      if (!c)
        *w++ = '?';
      else if (c < 0x800)
        {
          *w++ = (char) (0xC0 | (c >> 6));
          *w++ = (char) (0x80 | (c & 63));
        }
      else
        {
          *w++ = (char) (0xE0 | (c >> 12));
          *w++ = (char) (0x80 | ((c >> 6) & 63));
          *w++ = (char) (0x80 | (c & 63));
        }
    }
  *w = 0;
  return o;
}
