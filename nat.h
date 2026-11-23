// nat.h — shared types for the native (C11+SSE2 N-API) site-generator.
// Own code, written from the vendor (makefun-site 1.0.0) semantics. No copy.
#ifndef NAT_H
#define NAT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <emmintrin.h>

// ---------------- Arena (bump allocator; freed wholesale per run) ----------------
typedef struct ArenaChunk {
  struct ArenaChunk *next;
  size_t cap, used;
  char data[];
} ArenaChunk;

typedef struct {
  ArenaChunk *head;
} Arena;

void arena_init(Arena *a);
void *arena_alloc(Arena *a, size_t n); // 8-byte aligned
void *arena_alloc0(Arena *a, size_t n);
char *arena_dup(Arena *a, const char *s, size_t n);
char *arena_dupcstr(Arena *a, const char *s);
void arena_free(Arena *a);

// ---------------- Slice: UTF-8 bytes, length-based (may contain NUL) ----------------
typedef struct {
  const char *p;
  size_t n;
} Slice;

static inline Slice sl(const char *p, size_t n) {
  Slice s;
  s.p = p;
  s.n = n;
  return s;
}
static inline Slice sl_cstr(const char *s) {
  Slice r;
  r.p = s;
  r.n = strlen(s);
  return r;
}
int slice_cmp(Slice a, Slice b); // byte order == JS code-unit order for our data
static inline int slice_eq(Slice a, Slice b) {
  return a.n == b.n && (a.n == 0 || memcmp(a.p, b.p, a.n) == 0);
}
static inline int slice_eq_cstr(Slice a, const char *b) {
  size_t n = strlen(b);
  return a.n == n && (n == 0 || memcmp(a.p, b, n) == 0);
}
int slice_starts(Slice s, Slice pre);
int slice_ends(Slice s, Slice suf);

// ---------------- StrBuf: heap growable buffer ----------------
typedef struct {
  char *buf;
  size_t len, cap;
} StrBuf;

void sb_init(StrBuf *b);
void sb_reserve(StrBuf *b, size_t add);
static inline void sb_addn(StrBuf *b, const char *p, size_t n) {
  if (n == 0) return;
  if (b->len + n > b->cap) sb_reserve(b, n);
  memcpy(b->buf + b->len, p, n);
  b->len += n;
}
static inline void sb_add(StrBuf *b, Slice s) { sb_addn(b, s.p, s.n); }
static inline void sb_addc(StrBuf *b, char c) { sb_addn(b, &c, 1); }
void sb_addcstr(StrBuf *b, const char *s);
void sb_free(StrBuf *b);

// ---------------- UTF-8 ----------------
// Decode one code point at p (n bytes left). *adv = bytes consumed (1..4).
// Invalid bytes yield the byte value itself with *adv = 1.
// UTF-8 decode: static inline so the per-codepoint call sites (slugify,
// template rendering, has_ws) pay no cross-TU call overhead; the ASCII
// fast path is a single branch.
static inline uint32_t utf8_decode(const char *p, size_t n, size_t *adv) {
  if (n == 0) {
    *adv = 0;
    return 0;
  }
  unsigned char c = (unsigned char)p[0];
  if (c < 0x80) {
    *adv = 1;
    return c;
  }
  if ((c & 0xE0) == 0xC0 && n >= 2 && ((unsigned char)p[1] & 0xC0) == 0x80) {
    uint32_t cp = ((c & 0x1F) << 6) | ((unsigned char)p[1] & 0x3F);
    if (cp >= 0x80) {
      *adv = 2;
      return cp;
    }
  } else if ((c & 0xF0) == 0xE0 && n >= 3 && ((unsigned char)p[1] & 0xC0) == 0x80 &&
             ((unsigned char)p[2] & 0xC0) == 0x80) {
    uint32_t cp = ((c & 0x0F) << 12) | (((unsigned char)p[1] & 0x3F) << 6) | ((unsigned char)p[2] & 0x3F);
    if (cp >= 0x800 && !(cp >= 0xD800 && cp <= 0xDFFF)) {
      *adv = 3;
      return cp;
    }
  } else if ((c & 0xF8) == 0xF0 && n >= 4 && ((unsigned char)p[1] & 0xC0) == 0x80 &&
             ((unsigned char)p[2] & 0xC0) == 0x80 && ((unsigned char)p[3] & 0xC0) == 0x80) {
    uint32_t cp = ((c & 0x07) << 18) | (((unsigned char)p[1] & 0x3F) << 12) |
                  (((unsigned char)p[2] & 0x3F) << 6) | ((unsigned char)p[3] & 0x3F);
    if (cp >= 0x10000 && cp <= 0x10FFFF) {
      *adv = 4;
      return cp;
    }
  }
  *adv = 1;
  return c; // invalid byte: yield raw byte value (never a-z0-9, so a gap in slugify)
}
size_t utf8_encode(uint32_t cp, char out[4]);
Slice js_trim(Slice s); // JS String trim (WhiteSpace + LineTerminator)

// ---------------- SSE2 scanners (emmintrin.h) ----------------
// Index of the first byte in [p, p+n) that is a member of `set` (nset bytes), or n.
// Broadcast vectors are hoisted out of the chunk loop (they are constant per call).
static inline size_t sse2_find_any(const char *p, size_t n, const uint8_t *set, int nset) {
  __m128i vec[16];
  for (int i = 0; i < nset; i++) vec[i] = _mm_set1_epi8((char)set[i]);
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    __m128i v = _mm_loadu_si128((const __m128i *)(p + i));
    __m128i acc = _mm_setzero_si128();
    for (int k = 0; k < nset; k++) acc = _mm_or_si128(acc, _mm_cmpeq_epi8(v, vec[k]));
    int m = _mm_movemask_epi8(acc);
    if (m) return i + (size_t)__builtin_ctz((unsigned)m);
  }
  for (; i < n; i++) {
    unsigned char c = (unsigned char)p[i];
    for (int j = 0; j < nset; j++)
      if (c == set[j]) return i;
  }
  return n;
}
// Same as sse2_find_any but with precomputed broadcast vectors
// (vec[k] = _mm_set1_epi8(set[k])); the scalar tail uses `set`.
static inline size_t sse2_find_any_v(const char *p, size_t n, const __m128i *vec, const uint8_t *set,
                                     int nset) {
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    __m128i v = _mm_loadu_si128((const __m128i *)(p + i));
    __m128i acc = _mm_setzero_si128();
    for (int k = 0; k < nset; k++) acc = _mm_or_si128(acc, _mm_cmpeq_epi8(v, vec[k]));
    int m = _mm_movemask_epi8(acc);
    if (m) return i + (size_t)__builtin_ctz((unsigned)m);
  }
  for (; i < n; i++) {
    unsigned char c = (unsigned char)p[i];
    for (int j = 0; j < nset; j++)
      if (c == set[j]) return i;
  }
  return n;
}
// Index of the first byte equal to c, or n (memchr-equivalent, SSE2).
static inline size_t sse2_find_byte(const char *p, size_t n, uint8_t c) {
  size_t i = 0;
  __m128i needle = _mm_set1_epi8((char)c);
  for (; i + 16 <= n; i += 16) {
    __m128i v = _mm_loadu_si128((const __m128i *)(p + i));
    int m = _mm_movemask_epi8(_mm_cmpeq_epi8(v, needle));
    if (m) return i + (size_t)__builtin_ctz((unsigned)m);
  }
  for (; i < n; i++)
    if ((unsigned char)p[i] == c) return i;
  return n;
}
// Count of \S+ runs (JS wordsOf) over UTF-8 bytes. Bytes >= 0x80 are non-whitespace
// (the generator's non-ASCII inventory holds no Unicode whitespace).
size_t sse2_count_words(const char *p, size_t n);

// ---------------- Text (nat_text.c) ----------------
void escape_html_sb(StrBuf *out, const char *p, size_t n); // & < > " '
void escape_html_sb_slice(StrBuf *out, Slice s);
Slice slugify(Arena *a, Slice text); // NFKD + strip marks + a-z0-9 runs -> '-'

// Small set of slices (arena-grown, linear search; sets here are tiny).
typedef struct {
  Slice *items;
  size_t len, cap;
  Arena *a;
} StrSet;
void strset_init(StrSet *s, Arena *a);
int strset_has(StrSet *s, Slice v);
void strset_add(StrSet *s, Slice v);
Slice unique_slug(Arena *a, Slice slug, StrSet *taken); // first of slug, slug-2, ...; adds it
void long_date_sb(StrBuf *out, Slice date); // "2026-11-09" -> "9 November 2026"
size_t words_of(const char *p, size_t n);
Slice resolve_path(Arena *a, Slice dir, Slice rel, int *ok); // NULL (*ok=0) when climbs out
Slice dir_of(Slice path);

// ---------------- Values (template data model) ----------------
typedef enum { V_UNDEF, V_NULL, V_BOOL, V_NUM, V_STR, V_ARR, V_OBJ } VTag;

typedef struct Val Val;
typedef struct {
  Slice key;
  Val *val;
} KV;

struct Val {
  VTag tag;
  union {
    int b;
    double num;
    Slice s;
    struct {
      Val **items;
      size_t len;
    } arr;
    struct {
      KV *kvs;
      size_t len, cap;
    } obj;
  };
};

Val *v_undef(void);
Val *v_null(void);
Val *v_bool(Arena *a, int b);
Val *v_num(Arena *a, double d);
Val *v_str(Arena *a, Slice s);
Val *v_arr(Arena *a, Val **items, size_t len);
Val *v_obj(Arena *a);
void v_obj_set(Arena *a, Val *o, Slice key, Val *v);
Val *v_obj_get(Val *o, Slice key); // NULL when absent (does NOT follow 'length')
Val *v_lookup_arr_len(Val *arr, Slice key); // 'length' on arrays, else NULL

// JS textOf(v): string itself; number -> digits; bool -> true/false;
// array -> items joined by ", "; else "".
void text_of_sb(StrBuf *out, Val *v);
// JS truthy(v): arrays -> length>0; else not undefined/null/false/""/0.
int val_truthy(Val *v);
// JS String(number) for a double (shortest round-trip; integers as plain digits).
void js_num_sb(StrBuf *out, double d);
// Code-unit (UTF-16) order: -1/0/1. Used for every JS string sort/compare.
int cmp_code_units(Slice a, Slice b);

// ---------------- Front matter (nat_fm.c) ----------------
// Returns 0 ok (data_out filled, body_out = body slice), 1 error (err_out = message).
typedef struct {
  int has_title, has_date, has_tags, has_draft, has_layout, has_slug, has_summary;
  Slice title, date, layout, slug, summary;
  int draft; // 0/1
  Slice *tags;
  size_t ntags;
} FMData;

int parse_front_matter(const char *text, size_t n, Arena *a, FMData *data_out, Slice *body_out, StrBuf *err_out);

// ---------------- Markdown (nat_md.c) ----------------
typedef struct {
  void *ctx;
  // Returns href slice for target (arena-owned). May record broken links in ctx.
  Slice (*link)(void *ctx, Slice target);
} MdEnv;

typedef struct {
  Slice html; // rendered HTML (arena-owned)
  struct {
    int level; // 2 or 3
    Slice id;
    Slice html;
  } *toc;
  size_t ntoc, captoc;
  Slice excerpt; // first paragraph's inline HTML, or ""
} MdOut;

void render_markdown(Arena *a, Slice src, MdEnv *env, MdOut *out);

// ---------------- Template (nat_tpl.c) ----------------
typedef enum { ND_TEXT, ND_VAR, ND_RAW, ND_FOR, ND_IF, ND_INCLUDE } NodeKind;

typedef struct Node Node;
struct Node {
  NodeKind kind;
  Node *next; // next sibling (NULL at end of a body list)
  Slice text; // ND_TEXT
  Slice *path; // ND_VAR, ND_RAW, ND_FOR, ND_IF (dot-split names)
  size_t npath;
  Slice var_name; // ND_FOR: loop variable name
  Node *body; // ND_FOR body / ND_IF then-branch (head of sibling list)
  Node *else_body; // ND_IF else-branch (NULL when no else)
  int has_else; // ND_IF
  int neg; // ND_IF: 'not'
  Slice inc_name; // ND_INCLUDE
};

typedef struct {
  Slice name;
  Slice source; // raw template source (set by the bridge; parsed by build_site)
  Node *root; // head of sibling list (NULL when absent/failed)
  int failed;
  StrBuf err; // error message when failed
} Template;

typedef struct Scope Scope;
struct Scope {
  Slice *names;
  Val **vals;
  size_t n;
  Scope *parent;
};

// Parse src into a node list. Returns 0 ok, 1 TemplateError (err_out = message).
int parse_template(Arena *a, Slice name, Slice src, Node **root_out, StrBuf *err_out);
void render_nodes(StrBuf *out, Node *nodes, Scope *scope, Template *templates, size_t ntemplates, int depth, Arena *a);

// ---------------- JSON (nat_json.c) ----------------
// Parse a JSON document into Val (arena). Returns NULL on error (err_out = message).
Val *json_parse(Arena *a, Slice text, StrBuf *err_out);
// Serialize Val to JSON (insertion order for objects; strings raw UTF-8).
void json_stringify_sb(StrBuf *out, Val *v);
// JSON.stringify of a plain string (with quotes).
void json_quote_sb(StrBuf *out, Slice s);

// ---------------- Build (nat_build.c) ----------------
// Full pipeline over already-converted C input; appends output entries (path, content).
typedef struct {
  Slice path;
  Slice content;
} OutEntry;

typedef struct {
  Slice title, base_url;
  long per_page, recent;
  Template *templates; // sorted by name; failed ones have failed=1 and root=NULL
  size_t ntemplates;
  Slice *file_paths;
  Slice *file_contents;
  size_t nfiles;
} BuildInput;

int build_site(Arena *a, BuildInput *in, OutEntry **outs_out, size_t *nouts_out, StrBuf *err_out);
// Val-level wrapper: input Val -> output object Val (or NULL on error).
Val *build_site_val(Arena *a, Val *input, StrBuf *err_out);

#endif
