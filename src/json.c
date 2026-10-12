/* json.c - see json.h. Part of Tube. MIT licence. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "json.h"

#define BLOCK_SIZE (256 * 1024)
#define MAX_DEPTH 400

typedef struct block { struct block *next; size_t used, size; char *data; } block;

struct jdoc {
  block *blocks;
  jnode *root;
};

typedef struct {
  const char *p, *end, *base;
  jdoc *d;
  int depth;
  char *err;
  size_t errlen;
} parser;

/* Memory from the arena, aligned for jnode (pointer-sized, never unaligned). */
static void *arena_alloc (jdoc *d, size_t n)
{
  block *b = d->blocks;
  n = (n + 7) & ~(size_t) 7;
  if (!b || b->size - b->used < n)
    {
      size_t size = n > BLOCK_SIZE ? n : BLOCK_SIZE;
      block *nb = malloc (sizeof (block));
      if (!nb)
        return NULL;
      nb->data = malloc (size);
      if (!nb->data)
        {
          free (nb);
          return NULL;
        }
      nb->size = size;
      nb->used = 0;
      nb->next = d->blocks;
      d->blocks = nb;
      b = nb;
    }
  b->used += n;
  return b->data + b->used - n;
}

static int fail (parser *ps, const char *msg)
{
  if (ps->err && ps->errlen && !ps->err[0])
    snprintf (ps->err, ps->errlen, "JSON: %s (at byte %ld)", msg, (long) (ps->p - ps->base));
  return -1;
}

static void space (parser *ps)
{
  while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
    ps->p++;
}

static int hex4 (const char *p, unsigned *out)
{
  unsigned v = 0;
  for (int i = 0; i < 4; i++)
    {
      char c = p[i];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= (unsigned) (c - '0');
      else if (c >= 'a' && c <= 'f') v |= (unsigned) (c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= (unsigned) (c - 'A' + 10);
      else return -1;
    }
  *out = v;
  return 0;
}

static char *put_utf8 (char *o, unsigned c)
{
  if (c < 0x80) *o++ = (char) c;
  else if (c < 0x800) { *o++ = (char) (0xC0 | (c >> 6)); *o++ = (char) (0x80 | (c & 0x3F)); }
  else if (c < 0x10000) { *o++ = (char) (0xE0 | (c >> 12)); *o++ = (char) (0x80 | ((c >> 6) & 0x3F)); *o++ = (char) (0x80 | (c & 0x3F)); }
  else { *o++ = (char) (0xF0 | (c >> 18)); *o++ = (char) (0x80 | ((c >> 12) & 0x3F)); *o++ = (char) (0x80 | ((c >> 6) & 0x3F)); *o++ = (char) (0x80 | (c & 0x3F)); }
  return o;
}

/* At the opening quote: the string, unescaped, in the arena. */
static char *string (parser *ps, int *len)
{
  const char *q = ps->p + 1, *start = q;
  size_t raw = 0;
  char *out, *o;
  /* find the end first: the unescaped text is never longer than the raw */
  while (q < ps->end && *q != '"')
    {
      if (*q == '\\')
        q++;
      q++;
    }
  if (q >= ps->end)
    {
      fail (ps, "unterminated string");
      return NULL;
    }
  raw = (size_t) (q - start);
  out = arena_alloc (ps->d, raw + 1);
  if (!out)
    {
      fail (ps, "out of memory");
      return NULL;
    }
  o = out;
  for (const char *s = start; s < q; s++)
    {
      if (*s != '\\')
        {
          *o++ = *s;
          continue;
        }
      s++;
      switch (*s)
        {
        case 'n': *o++ = '\n'; break;
        case 't': *o++ = '\t'; break;
        case 'r': *o++ = '\r'; break;
        case 'b': *o++ = '\b'; break;
        case 'f': *o++ = '\f'; break;
        case 'u':
          {
            unsigned c, c2;
            if (q - s < 5 || hex4 (s + 1, &c))
              {
                fail (ps, "bad \\u escape");
                return NULL;
              }
            s += 4;
            if (c >= 0xD800 && c < 0xDC00 && q - s >= 7 && s[1] == '\\' && s[2] == 'u'
                && !hex4 (s + 3, &c2) && c2 >= 0xDC00 && c2 < 0xE000)
              {
                c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00);
                s += 6;
              }
            else if (c >= 0xD800 && c < 0xE000)
              c = 0xFFFD;
            o = put_utf8 (o, c);
            break;
          }
        default: *o++ = *s; break;     /* \" \\ \/ */
        }
    }
  *o = 0;
  *len = (int) (o - out);
  ps->p = q + 1;
  return out;
}

static jnode *value (parser *ps);

static jnode *container (parser *ps, jnode *v, char close)
{
  jnode **tail = &v->child;
  ps->p++;
  space (ps);
  if (ps->p < ps->end && *ps->p == close)
    {
      ps->p++;
      return v;
    }
  for (;;)
    {
      const char *key = NULL;
      jnode *item;
      space (ps);
      if (close == '}')
        {
          int klen;
          if (ps->p >= ps->end || *ps->p != '"')
            {
              fail (ps, "expected a member name");
              return NULL;
            }
          if (!(key = string (ps, &klen)))
            return NULL;
          space (ps);
          if (ps->p >= ps->end || *ps->p != ':')
            {
              fail (ps, "expected ':'");
              return NULL;
            }
          ps->p++;
        }
      if (!(item = value (ps)))
        return NULL;
      item->key = key;
      *tail = item;
      tail = &item->next;
      v->n++;
      space (ps);
      if (ps->p < ps->end && *ps->p == ',')
        {
          ps->p++;
          continue;
        }
      if (ps->p < ps->end && *ps->p == close)
        {
          ps->p++;
          return v;
        }
      fail (ps, close == '}' ? "expected ',' or '}'" : "expected ',' or ']'");
      return NULL;
    }
}

static jnode *value (parser *ps)
{
  jnode *v;
  space (ps);
  if (ps->p >= ps->end)
    {
      fail (ps, "unexpected end");
      return NULL;
    }
  if (++ps->depth > MAX_DEPTH)
    {
      fail (ps, "nested too deeply");
      return NULL;
    }
  if (!(v = arena_alloc (ps->d, sizeof (jnode))))
    {
      fail (ps, "out of memory");
      return NULL;
    }
  memset (v, 0, sizeof *v);
  switch (*ps->p)
    {
    case '{':
      v->type = J_OBJ;
      if (!container (ps, v, '}'))
        return NULL;
      break;
    case '[':
      v->type = J_ARR;
      if (!container (ps, v, ']'))
        return NULL;
      break;
    case '"':
      v->type = J_STR;
      if (!(v->s = string (ps, &v->n)))
        return NULL;
      break;
    case 't':
    case 'f':
    case 'n':
      {
        const char *w = *ps->p == 't' ? "true" : *ps->p == 'f' ? "false" : "null";
        size_t wl = strlen (w);
        if ((size_t) (ps->end - ps->p) < wl || memcmp (ps->p, w, wl))
          {
            fail (ps, "bad literal");
            return NULL;
          }
        v->type = *w == 'n' ? J_NULL : J_BOOL;
        v->n = *w == 't';
        ps->p += wl;
        break;
      }
    default:
      {
        const char *s = ps->p;
        char *t;
        while (ps->p < ps->end && strchr ("+-0123456789.eE", *ps->p))
          ps->p++;
        if (ps->p == s)
          {
            fail (ps, "unexpected character");
            return NULL;
          }
        v->type = J_NUM;
        if (!(t = arena_alloc (ps->d, (size_t) (ps->p - s) + 1)))
          {
            fail (ps, "out of memory");
            return NULL;
          }
        memcpy (t, s, (size_t) (ps->p - s));
        t[ps->p - s] = 0;
        v->s = t;
        break;
      }
    }
  ps->depth--;
  return v;
}

jdoc *json_parse (const char *text, size_t len, char *err, size_t errlen)
{
  parser ps;
  jdoc *d = calloc (1, sizeof (jdoc));
  if (err && errlen)
    err[0] = 0;
  if (!d)
    {
      if (err && errlen)
        snprintf (err, errlen, "JSON: out of memory");
      return NULL;
    }
  ps.p = ps.base = text;
  ps.end = text + len;
  ps.d = d;
  ps.depth = 0;
  ps.err = err;
  ps.errlen = errlen;
  if (len >= 3 && (unsigned char) text[0] == 0xEF && (unsigned char) text[1] == 0xBB
      && (unsigned char) text[2] == 0xBF)
    ps.p += 3;                                    /* a UTF-8 byte order mark */
  d->root = value (&ps);
  if (!d->root)
    {
      if (err && errlen && !err[0])
        snprintf (err, errlen, "JSON: not valid");
      json_free (d);
      return NULL;
    }
  return d;
}

jnode *json_root (jdoc *d)
{
  return d ? d->root : NULL;
}

void json_free (jdoc *d)
{
  if (!d)
    return;
  while (d->blocks)
    {
      block *b = d->blocks;
      d->blocks = b->next;
      free (b->data);
      free (b);
    }
  free (d);
}

jnode *j_get (const jnode *o, const char *key)
{
  if (!o || o->type != J_OBJ)
    return NULL;
  for (jnode *c = o->child; c; c = c->next)
    if (c->key && !strcmp (c->key, key))
      return c;
  return NULL;
}

jnode *j_idx (const jnode *a, int i)
{
  jnode *c;
  if (!a || a->type != J_ARR || i < 0)
    return NULL;
  for (c = a->child; c && i; c = c->next)
    i--;
  return c;
}

const char *j_str (const jnode *n)
{
  return n && n->type == J_STR ? n->s : NULL;
}

jnode *j_path (const jnode *n, const char *path)
{
  char part[96];
  const char *p = path;
  jnode *cur = (jnode *) n;
  while (cur && *p)
    {
      size_t l = strcspn (p, ".");
      if (l >= sizeof part)
        return NULL;
      memcpy (part, p, l);
      part[l] = 0;
      p += l;
      if (*p == '.')
        p++;
      if (cur->type == J_ARR && part[0] >= '0' && part[0] <= '9')
        cur = j_idx (cur, atoi (part));
      else
        cur = j_get (cur, part);
    }
  return cur;
}

const char *j_pstr (const jnode *n, const char *path)
{
  return j_str (j_path (n, path));
}

int j_true (const jnode *n)
{
  if (!n)
    return 0;
  if (n->type == J_BOOL)
    return n->n;
  return n->type == J_STR && !strcmp (n->s, "true");
}

long j_long (const jnode *n, long dflt)
{
  if (n && (n->type == J_NUM || (n->type == J_STR && n->s[0] >= '0' && n->s[0] <= '9')))
    return strtol (n->s, NULL, 10);
  return dflt;
}

static size_t append (char *buf, size_t size, size_t at, const char *s)
{
  size_t l = strlen (s);
  if (size && at < size - 1)
    {
      size_t room = size - 1 - at;
      memcpy (buf + at, s, l < room ? l : room);
      buf[at + (l < room ? l : room)] = 0;
    }
  return at + l;
}

size_t j_text (const jnode *n, char *buf, size_t size)
{
  const jnode *runs;
  size_t at = 0;
  if (size)
    buf[0] = 0;
  if (!n)
    return 0;
  if (n->type == J_STR)
    at = append (buf, size, 0, n->s);
  else if (j_str (j_get (n, "simpleText")))
    at = append (buf, size, 0, j_str (j_get (n, "simpleText")));
  else if (j_str (j_get (n, "content")))
    at = append (buf, size, 0, j_str (j_get (n, "content")));
  else if ((runs = j_get (n, "runs")) && runs->type == J_ARR)
    for (jnode *r = runs->child; r; r = r->next)
      if (j_str (j_get (r, "text")))
        at = append (buf, size, at, j_str (j_get (r, "text")));
  return size && at >= size ? size - 1 : at;
}

jnode *j_find (const jnode *n, const char *key)
{
  if (!n || (n->type != J_OBJ && n->type != J_ARR))
    return NULL;
  for (jnode *c = n->child; c; c = c->next)
    {
      jnode *f;
      if (n->type == J_OBJ && c->key && !strcmp (c->key, key))
        return c;
      if ((f = j_find (c, key)))
        return f;
    }
  return NULL;
}

size_t json_quote (const char *s, char *buf, size_t size)
{
  size_t at = 0;
#define PUT(ch) do { if (size && at < size - 1) buf[at] = (ch); at++; } while (0)
  PUT ('"');
  for (; *s; s++)
    {
      unsigned char c = (unsigned char) *s;
      if (c == '"' || c == '\\')
        {
          PUT ('\\');
          PUT ((char) c);
        }
      else if (c < 0x20)
        {
          static const char hexd[] = "0123456789abcdef";
          PUT ('\\'); PUT ('u'); PUT ('0'); PUT ('0');
          PUT (hexd[c >> 4]); PUT (hexd[c & 15]);
        }
      else
        PUT ((char) c);
    }
  PUT ('"');
  if (size)
    buf[at < size ? at : size - 1] = 0;
#undef PUT
  return at;
}
