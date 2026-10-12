/* json.h - a small JSON reader for YouTube's Innertube responses.

   The whole text is parsed into a tree held in one arena (freed in one
   go). Strings are unescaped into UTF-8. Lookups are linear, which is
   fine for responses of a megabyte or two.

   Part of Tube. MIT licence (see COPYING). */
#ifndef TUBE_JSON_H
#define TUBE_JSON_H
#include <stddef.h>

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype;

typedef struct jnode jnode;
struct jnode {
  jtype type;
  int n;               /* J_ARR/J_OBJ: children; J_STR: length; J_BOOL: 0/1 */
  const char *key;     /* the member name when the parent is an object */
  const char *s;       /* J_STR: the text (UTF-8, NUL-terminated); J_NUM: the number as written */
  jnode *child;        /* first child (J_ARR/J_OBJ) */
  jnode *next;         /* next sibling */
};

typedef struct jdoc jdoc;

/* Parses text (len bytes). NULL on failure, with a message in err. */
jdoc *json_parse (const char *text, size_t len, char *err, size_t errlen);
jnode *json_root (jdoc *d);
void json_free (jdoc *d);

/* Lookups: NULL when absent or of another type. */
jnode *j_get (const jnode *obj, const char *key);
jnode *j_idx (const jnode *arr, int i);
const char *j_str (const jnode *n);                 /* J_STR's text */
/* A dotted path: object keys and array indexes, e.g.
   "contents.twoColumnWatchNextResults.secondaryResults" or "runs.0.text". */
jnode *j_path (const jnode *n, const char *path);
const char *j_pstr (const jnode *n, const char *path);
int j_true (const jnode *n);                        /* J_BOOL true, or the string "true" */
long j_long (const jnode *n, long dflt);            /* J_NUM or a string of digits */

/* YouTube's text objects: a string, {"simpleText": ...},
   {"runs": [{"text": ...}, ...]} or {"content": ...}. The text (UTF-8)
   goes into buf; returns its length (0 if there is none). */
size_t j_text (const jnode *n, char *buf, size_t size);

/* Depth-first search below n for the first member called key. */
jnode *j_find (const jnode *n, const char *key);

/* Writes s as a JSON string literal (with quotes) into buf. Returns the
   length it needs (excluding the NUL), like snprintf. */
size_t json_quote (const char *s, char *buf, size_t size);

#endif
