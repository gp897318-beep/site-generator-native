// nat_tpl.c — template engine (exact port of vendor lib/template.js).
// Tokenizer (with standalone-tag line handling), parser to a node tree, and
// renderer over a scope chain. All error strings match the vendor.
#include "nat.h"

// ---------------- helpers ----------------
static int is_ident_start(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
static int is_ident_char(char c) {
  return is_ident_start(c) || (c >= '0' && c <= '9');
}

// JS \s over a slice (for the \S+ in FOR/IF tag regexes): true if any code point is whitespace.
static int has_js_ws(Slice s) {
  size_t i = 0;
  while (i < s.n) {
    size_t adv;
    (void)utf8_decode(s.p + i, s.n - i, &adv);
    if (adv == 0) adv = 1;
    // A single code point trims to empty iff it is JS whitespace.
    if (js_trim(sl(s.p + i, adv)).n == 0) return 1;
    i += adv;
  }
  return 0;
}

// pathOf(name, text): validate PATH, split at dots. 0 ok, 1 error (err_out).
static int path_of(Slice name, Slice text, Arena *a, Slice **path_out, size_t *npath_out, StrBuf *err_out) {
  // PATH /^[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*$/
  int ok = text.n > 0 && is_ident_start(text.p[0]);
  if (ok) {
    for (size_t i = 1; i < text.n && ok; i++) {
      char c = text.p[i];
      if (c == '.') {
        if (i + 1 >= text.n || !is_ident_start(text.p[i + 1])) ok = 0;
      } else if (!is_ident_char(c))
        ok = 0;
    }
  }
  if (!ok) {
    sb_addcstr(err_out, "template \"");
    sb_addn(err_out, name.p, name.n);
    sb_addcstr(err_out, "\": ");
    json_quote_sb(err_out, text);
    sb_addcstr(err_out, " is not a path");
    return 1;
  }
  size_t nseg = 1;
  for (size_t i = 0; i < text.n; i++)
    if (text.p[i] == '.') nseg++;
  Slice *segs = (Slice *)arena_alloc(a, nseg * sizeof(Slice));
  size_t s = 0, k = 0;
  for (size_t i = 0; i <= text.n; i++) {
    if (i == text.n || text.p[i] == '.') {
      segs[k++] = sl(text.p + s, i - s);
      s = i + 1;
    }
  }
  *path_out = segs;
  *npath_out = nseg;
  return 0;
}

static Node *new_node(Arena *a, NodeKind kind) {
  Node *n = (Node *)arena_alloc0(a, sizeof(Node));
  n->kind = kind;
  return n;
}

// ---------------- tokenizer ----------------
typedef struct {
  int kind; // 0=text 1=var 2=raw 3=tag
  Slice s; // payload: text, or trimmed inner/tag text
} Tok;

static int is_line_ws(Slice s) {
  for (size_t i = 0; i < s.n; i++)
    if (s.p[i] != ' ' && s.p[i] != '\t') return 0;
  return 1;
}

// Find needle (2 or 3 bytes) from `from`; -1 if absent.
static long find_str(const char *p, size_t n, const char *needle, size_t nlen, size_t from) {
  if (nlen == 0 || from >= n) return -1;
  size_t j = from;
  while (j + nlen <= n) {
    size_t f = sse2_find_byte(p + j, n - j, (uint8_t)needle[0]);
    if (f >= n - j) return -1;
    j += f;
    if (j + nlen <= n && memcmp(p + j, needle, nlen) == 0) return (long)j;
    j++;
  }
  return -1;
}

static long find_char(const char *p, size_t n, char c, size_t from) {
  if (from >= n) return -1;
  size_t f = sse2_find_byte(p + from, n - from, (uint8_t)c);
  if (f >= n - from) return -1;
  return (long)(from + f);
}

static int tokenize(Arena *a, Slice name, Slice src, Tok **toks_out, size_t *ntoks_out, StrBuf *err_out) {
  size_t cap = 32, nt = 0;
  Tok *toks = (Tok *)arena_alloc(a, cap * sizeof(Tok));
  size_t i = 0, tstart = 0;
  while (i < src.n) {
    long open = find_char(src.p, src.n, '{', i);
    if (open == -1 || (size_t)open == src.n - 1) {
      // text += src.slice(i); break (pending text runs to the end)
      i = src.n;
      break;
    }
    char next = src.p[open + 1];
    if (next != '{' && next != '%') {
      // lone '{' is literal text: it stays in the pending run
      i = (size_t)open + 1;
      continue;
    }
    if (next == '{') {
      int raw = (size_t)open + 2 < src.n && src.p[open + 2] == '{';
      const char *close_s = raw ? "}}}" : "}}";
      size_t clen = raw ? 3 : 2;
      long close = find_str(src.p, src.n, close_s, clen, (size_t)open + clen);
      if (close == -1) {
        sb_addcstr(err_out, "template \"");
        sb_addn(err_out, name.p, name.n);
        sb_addcstr(err_out, raw ? "\": a {{{ is not closed" : "\": a {{ is not closed");
        return 1;
      }
      // flush pending text [tstart, open)
      if ((size_t)open > tstart) {
        if (nt == cap) {
          size_t nc = cap * 2;
          Tok *np = (Tok *)arena_alloc(a, nc * sizeof(Tok));
          memcpy(np, toks, nt * sizeof(Tok));
          toks = np;
          cap = nc;
        }
        toks[nt].kind = 0;
        toks[nt].s = sl(src.p + tstart, (size_t)open - tstart);
        nt++;
      }
      if (nt == cap) {
        size_t nc = cap * 2;
        Tok *np = (Tok *)arena_alloc(a, nc * sizeof(Tok));
        memcpy(np, toks, nt * sizeof(Tok));
        toks = np;
        cap = nc;
      }
      toks[nt].kind = raw ? 2 : 1;
      toks[nt].s = js_trim(sl(src.p + (size_t)open + clen, (size_t)close - ((size_t)open + clen)));
      nt++;
      i = (size_t)close + clen;
      tstart = i;
      continue;
    }
    // {% ... %}
    long close = find_str(src.p, src.n, "%}", 2, (size_t)open + 2);
    if (close == -1) {
      sb_addcstr(err_out, "template \"");
      sb_addn(err_out, name.p, name.n);
      sb_addcstr(err_out, "\": a {% is not closed");
      return 1;
    }
    size_t end = (size_t)close + 2;
    // standalone? lineStart..open and end..lineEnd all [ \t]
    size_t lineStart = 0;
    {
      long nl = -1;
      for (long k = (long)open - 1; k >= 0; k--)
        if (src.p[k] == '\n') {
          nl = k;
          break;
        }
      lineStart = nl == -1 ? 0 : (size_t)nl + 1;
    }
    long nl = find_char(src.p, src.n, '\n', end);
    size_t lineEnd = nl == -1 ? src.n : (size_t)nl;
    int standalone = is_line_ws(sl(src.p + lineStart, (size_t)open - lineStart)) &&
                     is_line_ws(sl(src.p + end, lineEnd - end));
    size_t text_end = standalone ? lineStart : (size_t)open;
    if (text_end > tstart) {
      if (nt == cap) {
        size_t nc = cap * 2;
        Tok *np = (Tok *)arena_alloc(a, nc * sizeof(Tok));
        memcpy(np, toks, nt * sizeof(Tok));
        toks = np;
        cap = nc;
      }
      toks[nt].kind = 0;
      toks[nt].s = sl(src.p + tstart, text_end - tstart);
      nt++;
    }
    if (nt == cap) {
      size_t nc = cap * 2;
      Tok *np = (Tok *)arena_alloc(a, nc * sizeof(Tok));
      memcpy(np, toks, nt * sizeof(Tok));
      toks = np;
      cap = nc;
    }
    toks[nt].kind = 3;
    toks[nt].s = js_trim(sl(src.p + (size_t)open + 2, (size_t)close - ((size_t)open + 2)));
    nt++;
    if (standalone) {
      i = nl == -1 ? src.n : (size_t)nl + 1;
    } else {
      i = end;
    }
    tstart = i;
  }
  if (src.n > tstart) {
    if (nt == cap) {
      size_t nc = cap * 2;
      Tok *np = (Tok *)arena_alloc(a, nc * sizeof(Tok));
      memcpy(np, toks, nt * sizeof(Tok));
      toks = np;
      cap = nc;
    }
    toks[nt].kind = 0;
    toks[nt].s = sl(src.p + tstart, src.n - tstart);
    nt++;
  }
  *toks_out = toks;
  *ntoks_out = nt;
  return 0;
}

// ---------------- parser ----------------
typedef struct {
  Node **items;
  size_t len, cap;
  Arena *a;
} NodeVec;

static void nv_push(NodeVec *v, Node *n) {
  if (v->len == v->cap) {
    size_t nc = v->cap ? v->cap * 2 : 8;
    Node **np = (Node **)arena_alloc(v->a, nc * sizeof(Node *));
    if (v->items) memcpy(np, v->items, v->len * sizeof(Node *));
    v->items = np;
    v->cap = nc;
  }
  v->items[v->len++] = n;
}

static Node *nv_link(NodeVec *v) {
  for (size_t i = 0; i < v->len; i++) v->items[i]->next = i + 1 < v->len ? v->items[i + 1] : NULL;
  return v->len ? v->items[0] : NULL;
}

typedef struct {
  NodeKind kind; // ND_FOR or ND_IF (root uses ND_TEXT as marker with node==NULL)
  Node *node; // the for/if node (NULL for root)
  NodeVec body; // then-body / for-body
  NodeVec else_body; // if: else
  int in_else;
} Frame;

static void tpl_err(StrBuf *err_out, Slice name, const char *msg) {
  sb_addcstr(err_out, "template \"");
  sb_addn(err_out, name.p, name.n);
  sb_addcstr(err_out, "\": ");
  sb_addcstr(err_out, msg);
}

// Parse a FOR tag: "for X in PATH". 0 ok.
static int parse_for_tag(Slice name, Slice tag, Arena *a, Slice *var_out, Slice **path_out, size_t *npath_out,
                         StrBuf *err_out) {
  // /^for ([A-Za-z_][A-Za-z0-9_]*) in (\S+)$/
  if (tag.n < 4 || memcmp(tag.p, "for ", 4) != 0) return -1;
  size_t i = 4;
  if (i >= tag.n || !is_ident_start(tag.p[i])) return -1;
  size_t vs = i;
  while (i < tag.n && is_ident_char(tag.p[i])) i++;
  *var_out = sl(tag.p + vs, i - vs);
  if (i + 4 > tag.n || memcmp(tag.p + i, " in ", 4) != 0) return -1;
  Slice rest = sl(tag.p + i + 4, tag.n - (i + 4));
  if (rest.n == 0 || has_js_ws(rest)) return -1;
  return path_of(name, rest, a, path_out, npath_out, err_out);
}

// Parse an IF tag: "if [not ]PATH". 0 ok.
static int parse_if_tag(Slice name, Slice tag, Arena *a, int *neg_out, Slice **path_out, size_t *npath_out,
                        StrBuf *err_out) {
  // /^if (not )?(\S+)$/
  if (tag.n < 3 || memcmp(tag.p, "if ", 3) != 0) return -1;
  Slice rest = sl(tag.p + 3, tag.n - 3);
  int neg = 0;
  if (rest.n >= 4 && memcmp(rest.p, "not ", 4) == 0) {
    neg = 1;
    rest = sl(rest.p + 4, rest.n - 4);
  }
  if (rest.n == 0 || has_js_ws(rest)) return -1;
  *neg_out = neg;
  return path_of(name, rest, a, path_out, npath_out, err_out);
}

// Parse an INCLUDE tag: 'include "name"'. 0 ok, -1 no match.
static int parse_include_tag(Slice tag, Slice *name_out) {
  // /^include "([a-z0-9-]+)"$/
  if (tag.n < 11 || memcmp(tag.p, "include \"", 9) != 0) return -1;
  size_t i = 9;
  size_t s = i;
  while (i < tag.n) {
    char c = tag.p[i];
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') i++;
    else break;
  }
  if (i == s) return -1;
  if (i + 1 != tag.n || tag.p[i] != '"') return -1;
  *name_out = sl(tag.p + s, i - s);
  return 0;
}

int parse_template(Arena *a, Slice name, Slice src, Node **root_out, StrBuf *err_out) {
  Tok *toks;
  size_t ntoks;
  if (tokenize(a, name, src, &toks, &ntoks, err_out)) return 1;
  // Frame stack (arena). Depth is bounded by template size.
  size_t fcap = 16, nf = 0;
  Frame *frames = (Frame *)arena_alloc(a, fcap * sizeof(Frame));
  Frame *root_f = &frames[nf++];
  memset(root_f, 0, sizeof(*root_f));
  root_f->kind = ND_TEXT; // marker: root
  root_f->body.a = a;
  for (size_t ti = 0; ti < ntoks; ti++) {
    Tok *tok = &toks[ti];
    Frame *top = &frames[nf - 1];
    NodeVec *cur = top->in_else ? &top->else_body : &top->body;
    if (tok->kind == 0) {
      Node *n = new_node(a, ND_TEXT);
      n->text = tok->s;
      nv_push(cur, n);
      continue;
    }
    if (tok->kind == 1 || tok->kind == 2) {
      Node *n = new_node(a, tok->kind == 1 ? ND_VAR : ND_RAW);
      if (path_of(name, tok->s, a, &n->path, &n->npath, err_out)) return 1;
      nv_push(cur, n);
      continue;
    }
    // tag
    Slice tag = tok->s;
    Slice var;
    Slice *path = NULL;
    size_t npath = 0;
    int neg = 0;
    if (parse_for_tag(name, tag, a, &var, &path, &npath, err_out) == 0) {
      Node *n = new_node(a, ND_FOR);
      n->var_name = var;
      n->path = path;
      n->npath = npath;
      nv_push(cur, n);
      if (nf == fcap) {
        size_t nc = fcap * 2;
        Frame *nfr = (Frame *)arena_alloc(a, nc * sizeof(Frame));
        memcpy(nfr, frames, nf * sizeof(Frame));
        frames = nfr;
        fcap = nc;
      }
      Frame *f = &frames[nf++];
      memset(f, 0, sizeof(*f));
      f->kind = ND_FOR;
      f->node = n;
      f->body.a = a;
      f->else_body.a = a;
      continue;
    }
    if (err_out->len > 0) return 1; // pathOf error inside a for tag
    if (parse_if_tag(name, tag, a, &neg, &path, &npath, err_out) == 0) {
      Node *n = new_node(a, ND_IF);
      n->neg = neg;
      n->path = path;
      n->npath = npath;
      nv_push(cur, n);
      if (nf == fcap) {
        size_t nc = fcap * 2;
        Frame *nfr = (Frame *)arena_alloc(a, nc * sizeof(Frame));
        memcpy(nfr, frames, nf * sizeof(Frame));
        frames = nfr;
        fcap = nc;
      }
      Frame *f = &frames[nf++];
      memset(f, 0, sizeof(*f));
      f->kind = ND_IF;
      f->node = n;
      f->body.a = a;
      f->else_body.a = a;
      continue;
    }
    if (err_out->len > 0) return 1; // pathOf error inside an if tag
    if (slice_eq_cstr(tag, "else")) {
      if (top->kind != ND_IF || top->node->has_else) {
        tpl_err(err_out, name, "an else outside an if");
        return 1;
      }
      top->node->has_else = 1;
      top->in_else = 1;
      continue;
    }
    if (slice_eq_cstr(tag, "endfor") || slice_eq_cstr(tag, "endif")) {
      NodeKind want = slice_eq_cstr(tag, "endfor") ? ND_FOR : ND_IF;
      if (top->kind != want || top->node == NULL) {
        sb_addcstr(err_out, "template \"");
        sb_addn(err_out, name.p, name.n);
        sb_addcstr(err_out, "\": an ");
        sb_addn(err_out, tag.p, tag.n);
        sb_addcstr(err_out, " with no open ");
        sb_addcstr(err_out, want == ND_FOR ? "for" : "if");
        return 1;
      }
      top->node->body = nv_link(&top->body);
      if (top->kind == ND_IF && top->node->has_else) top->node->else_body = nv_link(&top->else_body);
      nf--;
      continue;
    }
    Slice inc;
    if (parse_include_tag(tag, &inc) == 0) {
      Node *n = new_node(a, ND_INCLUDE);
      n->inc_name = inc;
      nv_push(cur, n);
      continue;
    }
    sb_addcstr(err_out, "template \"");
    sb_addn(err_out, name.p, name.n);
    sb_addcstr(err_out, "\": {% ");
    sb_addn(err_out, tag.p, tag.n);
    sb_addcstr(err_out, " %} is not a tag");
    return 1;
  }
  if (nf > 1) {
    Frame *top = &frames[nf - 1];
    tpl_err(err_out, name, top->kind == ND_FOR ? "a for is not closed" : "a if is not closed");
    return 1;
  }
  *root_out = nv_link(&frames[0].body);
  return 0;
}

// ---------------- renderer ----------------
// Scratch for ND_VAR non-string values: text_of_sb + escape complete before
// any nested render_nodes call, so one reused buffer is exact (single-threaded).
static StrBuf tpl_scratch;
static int tpl_scratch_init = 0;

static int scope_has(Scope *s, Slice name) {
  for (size_t i = 0; i < s->n; i++)
    if (slice_eq(s->names[i], name)) return 1;
  return 0;
}

static Val *scope_get(Scope *s, Slice name) {
  for (size_t i = 0; i < s->n; i++)
    if (slice_eq(s->names[i], name)) return s->vals[i];
  return NULL;
}

static int is_array_index(Slice s, size_t *idx_out) {
  if (s.n == 0 || s.n > 10) return 0;
  for (size_t i = 0; i < s.n; i++)
    if (s.p[i] < '0' || s.p[i] > '9') return 0;
  // canonical: no leading zero unless single "0"
  if (s.n > 1 && s.p[0] == '0') return 0;
  unsigned long long v = 0;
  for (size_t i = 0; i < s.n; i++) v = v * 10 + (unsigned)(s.p[i] - '0');
  if (v > 4294967294ULL) return 0;
  *idx_out = (size_t)v;
  return 1;
}

// lookup(scope, path): first segment from the nearest scope holding it, then own properties.
static Val *lookup(Arena *a, Scope *scope, Slice *path, size_t npath) {
  if (npath == 0) return NULL;
  Scope *s = scope;
  while (s && !scope_has(s, path[0])) s = s->parent;
  if (!s) return NULL;
  Val *v = scope_get(s, path[0]);
  for (size_t k = 1; k < npath; k++) {
    if (!v) return NULL;
    if (v->tag == V_OBJ) {
      Val *nv = v_obj_get(v, path[k]);
      if (!nv) return NULL;
      v = nv;
    } else if (v->tag == V_ARR) {
      if (slice_eq_cstr(path[k], "length")) {
        v = v_num(a, (double)v->arr.len);
      } else {
        size_t idx;
        if (!is_array_index(path[k], &idx) || idx >= v->arr.len) return NULL;
        v = v->arr.items[idx];
      }
    } else {
      return NULL;
    }
  }
  return v;
}

static Template *find_template(Template *templates, size_t ntemplates, Slice name) {
  for (size_t i = 0; i < ntemplates; i++)
    if (slice_eq(templates[i].name, name)) return &templates[i];
  return NULL;
}

void render_nodes(StrBuf *out, Node *nodes, Scope *scope, Template *templates, size_t ntemplates, int depth, Arena *a) {
  for (Node *n = nodes; n; n = n->next) {
    switch (n->kind) {
      case ND_TEXT:
        sb_add(out, n->text);
        break;
      case ND_VAR: {
        Val *v = lookup(a, scope, n->path, n->npath);
        if (v && v->tag == V_STR) {
          escape_html_sb(out, v->s.p, v->s.n);
        } else {
          if (!tpl_scratch_init) {
            sb_init(&tpl_scratch);
            tpl_scratch_init = 1;
          }
          tpl_scratch.len = 0;
          text_of_sb(&tpl_scratch, v);
          escape_html_sb(out, tpl_scratch.buf, tpl_scratch.len);
        }
        break;
      }
      case ND_RAW: {
        Val *v = lookup(a, scope, n->path, n->npath);
        text_of_sb(out, v);
        break;
      }
      case ND_IF: {
        Val *v = lookup(a, scope, n->path, n->npath);
        int tv = val_truthy(v);
        Node *branch = (tv != n->neg) ? n->body : (n->has_else ? n->else_body : NULL);
        if (branch) render_nodes(out, branch, scope, templates, ntemplates, depth, a);
        break;
      }
      case ND_FOR: {
        Val *xs = lookup(a, scope, n->path, n->npath);
        if (!xs || xs->tag != V_ARR) break;
        // One loop object for the whole loop, mutated per iteration: the
        // template only reads it (never stores it), so reuse is exact.
        Val *loop = v_obj(a);
        Val *v_index = v_num(a, 1);
        Val *v_first = v_bool(a, 1);
        Val *v_last = v_bool(a, 0);
        Val *v_len = v_num(a, (double)xs->arr.len);
        v_obj_set(a, loop, sl("index", 5), v_index);
        v_obj_set(a, loop, sl("first", 5), v_first);
        v_obj_set(a, loop, sl("last", 4), v_last);
        v_obj_set(a, loop, sl("length", 6), v_len);
        Slice names[2];
        Val *vals[2];
        names[0] = n->var_name;
        names[1] = sl("loop", 4);
        vals[1] = loop;
        Scope child;
        child.names = names;
        child.vals = vals;
        child.n = 2;
        child.parent = scope;
        for (size_t k = 0; k < xs->arr.len; k++) {
          v_index->num = (double)(k + 1);
          v_first->b = (k == 0);
          v_last->b = (k == xs->arr.len - 1);
          vals[0] = xs->arr.items[k];
          render_nodes(out, n->body, &child, templates, ntemplates, depth, a);
        }
        break;
      }
      case ND_INCLUDE: {
        Template *t = find_template(templates, ntemplates, n->inc_name);
        if (t && t->root && depth < 8) render_nodes(out, t->root, scope, templates, ntemplates, depth + 1, a);
        break;
      }
    }
  }
}
