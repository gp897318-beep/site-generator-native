// nat_util.c — arena, slices, StrBuf, UTF-8, SSE2 scanners.
#include "nat.h"
#include <emmintrin.h>

// ---------------- Arena ----------------
#define ARENA_CHUNK 65536

void arena_init(Arena *a) { a->head = NULL; }

static ArenaChunk *arena_new_chunk(size_t need) {
  size_t cap = need > ARENA_CHUNK ? need : ARENA_CHUNK;
  ArenaChunk *c = (ArenaChunk *)malloc(sizeof(ArenaChunk) + cap);
  if (!c) abort();
  c->next = NULL;
  c->cap = cap;
  c->used = 0;
  return c;
}

void *arena_alloc(Arena *a, size_t n) {
  n = (n + 7) & ~(size_t)7;
  ArenaChunk *c = a->head;
  if (!c || c->used + n > c->cap) {
    ArenaChunk *nc = arena_new_chunk(n);
    nc->next = c;
    a->head = nc;
    c = nc;
  }
  void *p = c->data + c->used;
  c->used += n;
  return p;
}

void *arena_alloc0(Arena *a, size_t n) {
  void *p = arena_alloc(a, n);
  memset(p, 0, n);
  return p;
}

char *arena_dup(Arena *a, const char *s, size_t n) {
  char *p = (char *)arena_alloc(a, n + 1);
  if (n) memcpy(p, s, n);
  p[n] = '\0';
  return p;
}

char *arena_dupcstr(Arena *a, const char *s) { return arena_dup(a, s, strlen(s)); }

void arena_free(Arena *a) {
  ArenaChunk *c = a->head;
  while (c) {
    ArenaChunk *nx = c->next;
    free(c);
    c = nx;
  }
  a->head = NULL;
}

// ---------------- Slices ----------------
int slice_cmp(Slice a, Slice b) {
  size_t n = a.n < b.n ? a.n : b.n;
  int c = n ? memcmp(a.p, b.p, n) : 0;
  if (c) return c < 0 ? -1 : 1;
  if (a.n == b.n) return 0;
  return a.n < b.n ? -1 : 1;
}

int slice_starts(Slice s, Slice pre) {
  return s.n >= pre.n && (pre.n == 0 || memcmp(s.p, pre.p, pre.n) == 0);
}

int slice_ends(Slice s, Slice suf) {
  return s.n >= suf.n && (suf.n == 0 || memcmp(s.p + s.n - suf.n, suf.p, suf.n) == 0);
}

// ---------------- StrBuf ----------------
void sb_init(StrBuf *b) {
  b->cap = 64;
  b->len = 0;
  b->buf = (char *)malloc(b->cap);
  if (!b->buf) abort();
}

void sb_reserve(StrBuf *b, size_t add) {
  if (b->len + add <= b->cap) return;
  size_t nc = b->cap;
  while (nc < b->len + add) nc *= 2;
  b->buf = (char *)realloc(b->buf, nc);
  if (!b->buf) abort();
  b->cap = nc;
}

void sb_addcstr(StrBuf *b, const char *s) { sb_addn(b, s, strlen(s)); }

void sb_free(StrBuf *b) {
  free(b->buf);
  b->buf = NULL;
  b->len = b->cap = 0;
}

// ---------------- UTF-8 ----------------
// (utf8_decode is static inline in nat.h; see there.)
size_t utf8_encode(uint32_t cp, char out[4]) {
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

// ---------------- SSE2 scanners ----------------
// (sse2_find_any / sse2_find_byte live in nat.h as static inlines.)

// Count \S+ runs. Whitespace bytes (JS \s restricted to what can appear):
// ASCII space \t \n \v \f \r. Bytes >= 0x80 are non-whitespace (generator inventory
// holds no Unicode whitespace).
// A word starts at a non-whitespace byte whose previous byte is whitespace
// (or at the start). Per 16-byte chunk: starts = ~ws & (ws<<1 | prev_ws),
// counted with popcount — a few ops per 16 bytes, no per-bit loop.
size_t sse2_count_words(const char *p, size_t n) {
  static const uint8_t WS[6] = {' ', '\t', '\n', '\v', '\f', '\r'};
  __m128i vec[6];
  for (int i = 0; i < 6; i++) vec[i] = _mm_set1_epi8((char)WS[i]);
  size_t count = 0;
  int prev_ws = 1; // the byte before the text counts as whitespace
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    __m128i v = _mm_loadu_si128((const __m128i *)(p + i));
    __m128i acc = _mm_setzero_si128();
    for (int k = 0; k < 6; k++) acc = _mm_or_si128(acc, _mm_cmpeq_epi8(v, vec[k]));
    int m = _mm_movemask_epi8(acc); // 1 bits = whitespace
    unsigned starts = (unsigned)(~m) & (unsigned)((m << 1) | (prev_ws ? 1 : 0));
    starts &= 0xFFFFu;
    count += (size_t)__builtin_popcount(starts);
    prev_ws = (m >> 15) & 1;
  }
  for (; i < n; i++) {
    unsigned char c = (unsigned char)p[i];
    int is_ws = (c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r');
    if (!is_ws && prev_ws) count++;
    prev_ws = is_ws;
  }
  return count;
}

// ---------------- JS trim ----------------
static int is_js_ws_cp(uint32_t cp) {
  if (cp == 0x09 || cp == 0x0A || cp == 0x0B || cp == 0x0C || cp == 0x0D || cp == 0x20) return 1;
  if (cp == 0xA0 || cp == 0x1680 || cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F ||
      cp == 0x3000 || cp == 0xFEFF)
    return 1;
  if (cp >= 0x2000 && cp <= 0x200A) return 1;
  return 0;
}

Slice js_trim(Slice s) {
  size_t a = 0, b = s.n;
  while (a < b) {
    size_t adv;
    uint32_t cp = utf8_decode(s.p + a, b - a, &adv);
    if (!is_js_ws_cp(cp)) break;
    a += adv;
  }
  while (b > a) {
    size_t j = b - 1;
    while (j > a && ((unsigned char)s.p[j] & 0xC0) == 0x80) j--;
    size_t adv;
    uint32_t cp = utf8_decode(s.p + j, b - j, &adv);
    if (!is_js_ws_cp(cp)) break;
    b = j;
  }
  return sl(s.p + a, b - a);
}
