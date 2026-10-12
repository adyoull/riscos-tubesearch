/* jsolve.c - see jsolve.h. Part of TubeSearch. MIT licence. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jsolve.h"
#include "json.h"
#include "quickjs.h"

/* GCC 10 ELF programs on RISC OS get a 1 MB stack (ARMEABISupport); QuickJS
   stops with a JavaScript "stack overflow" well before that. */
#define STACK_LIMIT (640 * 1024)
#define MEMORY_LIMIT (160u * 1024 * 1024)

struct jsolve {
  JSRuntime *rt;
  JSContext *ctx;
  char player_id[64];          /* the prepared player in globalThis.__tsPre, or "" */
  int (*tick) (void *);
  void *tick_ctx;
  int stopped;
};

static int interrupt (JSRuntime *rt, void *opaque)
{
  jsolve *s = opaque;
  (void) rt;
  if (s->tick && s->tick (s->tick_ctx))
    s->stopped = 1;
  return s->stopped;
}

/* The pending exception as text. */
static void exception_text (JSContext *ctx, char *err, size_t errlen)
{
  JSValue e = JS_GetException (ctx);
  const char *t = JS_ToCString (ctx, e);
  /* ("the solver" marks it as the scripts' fault: TubeSearch then tries others) */
  snprintf (err, errlen, "the solver: %s", t ? t : "a JavaScript error");
  if (t)
    JS_FreeCString (ctx, t);
  JS_FreeValue (ctx, e);
}

/* Evaluates NUL-terminated code (QuickJS needs the NUL). 0, or -1 with err. */
static int eval_code (jsolve *s, const char *code, size_t len, const char *name, JSValue *result,
                      char *err, size_t errlen)
{
  JSValue v = JS_Eval (s->ctx, code, len, name, JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException (v))
    {
      if (s->stopped)
        snprintf (err, errlen, "stopped");
      else
        exception_text (s->ctx, err, errlen);
      return -1;
    }
  if (result)
    *result = v;
  else
    JS_FreeValue (s->ctx, v);
  return 0;
}

static int eval_copy (jsolve *s, const char *code, size_t len, const char *name, char *err, size_t errlen)
{
  char *c = malloc (len + 1);
  int r;
  if (!c)
    {
      snprintf (err, errlen, "out of memory");
      return -1;
    }
  memcpy (c, code, len);
  c[len] = 0;
  r = eval_code (s, c, len, name, NULL, err, errlen);
  free (c);
  return r;
}

jsolve *jsolve_new (const char *lib, size_t liblen, const char *core, size_t corelen,
                    char *err, size_t errlen)
{
  static const char setup[] =
    "globalThis.console = { log: function () {}, warn: function () {}, error: function () {},"
    " info: function () {}, debug: function () {} };";
  static const char assign[] = "Object.assign(globalThis, lib);";
  jsolve *s = calloc (1, sizeof *s);
  if (!s)
    {
      snprintf (err, errlen, "out of memory");
      return NULL;
    }
  if (!(s->rt = JS_NewRuntime ()) || !(s->ctx = JS_NewContext (s->rt)))
    {
      snprintf (err, errlen, "JavaScript: out of memory");
      jsolve_free (s);
      return NULL;
    }
  JS_SetMaxStackSize (s->rt, STACK_LIMIT);
  {
    const char *m = getenv ("TUBESEARCH_JS_MB");          /* (tests: find how much it needs) */
    JS_SetMemoryLimit (s->rt, m && atoi (m) > 0 ? (size_t) atoi (m) << 20 : MEMORY_LIMIT);
  }
  JS_SetInterruptHandler (s->rt, interrupt, s);
  if (eval_code (s, setup, sizeof setup - 1, "setup", NULL, err, errlen)
      || eval_copy (s, lib, liblen, "yt.solver.lib.js", err, errlen)
      || eval_code (s, assign, sizeof assign - 1, "assign", NULL, err, errlen)
      || eval_copy (s, core, corelen, "yt.solver.core.js", err, errlen))
    {
      jsolve_free (s);
      return NULL;
    }
  return s;
}

void jsolve_free (jsolve *s)
{
  if (!s)
    return;
  if (s->ctx)
    JS_FreeContext (s->ctx);
  if (s->rt)
    JS_FreeRuntime (s->rt);
  free (s);
}

void jsolve_set_tick (jsolve *s, int (*tick) (void *ctx), void *ctx)
{
  s->tick = tick;
  s->tick_ctx = ctx;
}

int jsolve_has_player (const jsolve *s, const char *player_id)
{
  return s && s->player_id[0] && player_id && !strcmp (s->player_id, player_id);
}

/* The requests as JSON: [{"type":"n","challenges":[...]},{"type":"sig",...}] */
static char *requests_json (const char *const *n, int nn, const char *const *sig, int nsig)
{
  size_t cap = 64;
  char *b, *p;
  for (int i = 0; i < nn; i++)
    cap += json_quote (n[i], NULL, 0) + 1;
  for (int i = 0; i < nsig; i++)
    cap += json_quote (sig[i], NULL, 0) + 1;
  if (!(b = malloc (cap + 64)))
    return NULL;
  p = b;
  p += sprintf (p, "[{\"type\":\"n\",\"challenges\":[");
  for (int i = 0; i < nn; i++)
    {
      if (i)
        *p++ = ',';
      p += json_quote (n[i], p, cap - (size_t) (p - b));
    }
  p += sprintf (p, "]},{\"type\":\"sig\",\"challenges\":[");
  for (int i = 0; i < nsig; i++)
    {
      if (i)
        *p++ = ',';
      p += json_quote (sig[i], p, cap - (size_t) (p - b));
    }
  sprintf (p, "]}]");
  return b;
}

/* Takes the answers for one request out of {"type":"result","data":{in: out}}. */
static int take_answers (const jnode *resp, const char *const *in, int count, char **out,
                         char *err, size_t errlen)
{
  const jnode *data = j_get (resp, "data");
  const char *type = j_str (j_get (resp, "type"));
  int missing = 0;
  if (!type || strcmp (type, "result") || !data)
    {
      const char *e = j_str (j_get (resp, "error"));
      snprintf (err, errlen, "the solver: %s", e ? e : "no result");
      return -1;
    }
  for (int i = 0; i < count; i++)
    {
      const char *a = j_str (j_get (data, in[i]));
      out[i] = a ? strdup (a) : NULL;
      missing += !out[i];
    }
  if (missing)
    {
      snprintf (err, errlen, "the solver didn't answer %d challenge%s", missing, missing == 1 ? "" : "s");
      return -1;
    }
  return 0;
}

int jsolve_run (jsolve *s, const char *player_id, const char *player, size_t len,
                const char *const *n, int nn, char **n_out,
                const char *const *sig, int nsig, char **sig_out,
                char *err, size_t errlen)
{
  static const char fresh[] =
    "(function () {"
    "  var r = jsc({ type: 'player', player: globalThis.__tsPlayer,"
    "                requests: JSON.parse(globalThis.__tsRequests), output_preprocessed: true });"
    "  globalThis.__tsPlayer = undefined;"
    "  if (r && r.preprocessed_player) { globalThis.__tsPre = r.preprocessed_player; delete r.preprocessed_player; }"
    "  return JSON.stringify(r);"
    "})()";
  static const char again[] =
    "(function () {"
    "  var r = jsc({ type: 'preprocessed', preprocessed_player: globalThis.__tsPre,"
    "                requests: JSON.parse(globalThis.__tsRequests) });"
    "  if (r) delete r.preprocessed_player;"
    "  return JSON.stringify(r);"
    "})()";
  JSValue global, result;
  char *req, jerr[96];
  const char *text;
  jdoc *d;
  const jnode *root, *responses;
  int use_cached = jsolve_has_player (s, player_id), rc = -1;

  for (int i = 0; i < nn; i++)
    n_out[i] = NULL;
  for (int i = 0; i < nsig; i++)
    sig_out[i] = NULL;
  if (!use_cached && !player)
    {
      snprintf (err, errlen, "no player code");
      return -1;
    }
  if (!(req = requests_json (n, nn, sig, nsig)))
    {
      snprintf (err, errlen, "out of memory");
      return -1;
    }
  global = JS_GetGlobalObject (s->ctx);
  JS_SetPropertyStr (s->ctx, global, "__tsRequests", JS_NewString (s->ctx, req));
  free (req);
  if (!use_cached)
    {
      s->player_id[0] = 0;
      JS_SetPropertyStr (s->ctx, global, "__tsPre", JS_UNDEFINED);
      JS_SetPropertyStr (s->ctx, global, "__tsPlayer", JS_NewStringLen (s->ctx, player, len));
    }
  JS_FreeValue (s->ctx, global);
  s->stopped = 0;
  if (eval_code (s, use_cached ? again : fresh, use_cached ? sizeof again - 1 : sizeof fresh - 1,
                 "solve", &result, err, errlen))
    {
      s->player_id[0] = 0;                  /* (a prepared player that failed isn't trusted again) */
      JS_RunGC (s->rt);
      return -1;
    }
  if (!use_cached)
    snprintf (s->player_id, sizeof s->player_id, "%s", player_id ? player_id : "");
  text = JS_ToCString (s->ctx, result);
  JS_FreeValue (s->ctx, result);
  if (!text)
    {
      snprintf (err, errlen, "the solver gave nothing back");
      s->player_id[0] = 0;
      JS_RunGC (s->rt);
      return -1;
    }
  d = json_parse (text, strlen (text), jerr, sizeof jerr);
  JS_FreeCString (s->ctx, text);
  JS_RunGC (s->rt);
  if (!d)
    {
      snprintf (err, errlen, "the solver's answer wasn't readable (%s)", jerr);
      s->player_id[0] = 0;
      return -1;
    }
  root = json_root (d);
  responses = j_get (root, "responses");
  if (!j_str (j_get (root, "type")) || strcmp (j_str (j_get (root, "type")), "result") || !responses)
    {
      const char *e = j_str (j_get (root, "error"));
      snprintf (err, errlen, "the solver: %s", e ? e : "failed");
      s->player_id[0] = 0;
    }
  else if (take_answers (j_idx (responses, 0), n, nn, n_out, err, errlen) == 0
           && take_answers (j_idx (responses, 1), sig, nsig, sig_out, err, errlen) == 0)
    rc = 0;
  json_free (d);
  if (rc)
    {
      s->player_id[0] = 0;                  /* (any failure: the prepared player isn't trusted again) */
      for (int i = 0; i < nn; i++)
        {
          free (n_out[i]);
          n_out[i] = NULL;
        }
      for (int i = 0; i < nsig; i++)
        {
          free (sig_out[i]);
          sig_out[i] = NULL;
        }
    }
  return rc;
}

int jsolve_save_player (jsolve *s, const char *path)
{
  JSValue g, v;
  const char *t;
  size_t len;
  FILE *f;
  int rc = -1;
  if (!s->player_id[0])
    return -1;
  g = JS_GetGlobalObject (s->ctx);
  v = JS_GetPropertyStr (s->ctx, g, "__tsPre");
  JS_FreeValue (s->ctx, g);
  t = JS_IsString (v) ? JS_ToCStringLen (s->ctx, &len, v) : NULL;
  JS_FreeValue (s->ctx, v);
  if (!t)
    return -1;
  {
    /* through a temporary name: a half-written prepared player is never seen */
    char tmp[600];
    snprintf (tmp, sizeof tmp, "%s-tmp", path);
    if ((f = fopen (tmp, "wb")))
      {
        rc = fwrite (t, 1, len, f) == len ? 0 : -1;
        if (fclose (f))
          rc = -1;
        if (rc == 0)
          {
            remove (path);
            rc = rename (tmp, path) ? -1 : 0;
          }
        if (rc)
          remove (tmp);
      }
  }
  JS_FreeCString (s->ctx, t);
  return rc;
}

int jsolve_load_player (jsolve *s, const char *player_id, const char *path)
{
  FILE *f = fopen (path, "rb");
  char *b;
  long n;
  JSValue g;
  if (!f)
    return -1;
  if (fseek (f, 0, SEEK_END) || (n = ftell (f)) <= 0 || fseek (f, 0, SEEK_SET) || !(b = malloc ((size_t) n)))
    {
      fclose (f);
      return -1;
    }
  if (fread (b, 1, (size_t) n, f) != (size_t) n)
    {
      fclose (f);
      free (b);
      return -1;
    }
  fclose (f);
  g = JS_GetGlobalObject (s->ctx);
  JS_SetPropertyStr (s->ctx, g, "__tsPre", JS_NewStringLen (s->ctx, b, (size_t) n));
  JS_FreeValue (s->ctx, g);
  free (b);
  snprintf (s->player_id, sizeof s->player_id, "%s", player_id);
  return 0;
}
