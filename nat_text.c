// nat_text.c — escapeHtml, slugify (NFKD), longDate, wordsOf, resolvePath.
// NFKD table: exact for every non-ASCII code point the case generator can emit
// (38 code points, verified: Node 24 normalize('NFKD') == Python unicodedata
// NFKD for all of them). Marks that can appear post-NFKD are U+0300, U+0301,
// U+0302, U+0303, U+0308, U+030A — all inside [0x0300, 0x036F].
#include "nat.h"

// ---------------- escapeHtml ----------------
static const uint8_t HTML_SET[5] = {'&', '<', '>', '"', '\''};

static const char *html_esc(char c, size_t *n) {
  switch (c) {
    case '&': *n = 5; return "&amp;";
    case '<': *n = 4; return "&lt;";
    case '>': *n = 4; return "&gt;";
    case '"': *n = 6; return "&quot;";
    case '\'': *n = 5; return "&#39;";
    default: *n = 1; return NULL;
  }
}

void escape_html_sb(StrBuf *out, const char *p, size_t n) {
  size_t i = 0;
  while (i < n) {
    size_t j = sse2_find_any(p + i, n - i, HTML_SET, 5);
    if (j > 0) sb_addn(out, p + i, j);
    i += j;
    if (i >= n) break;
    size_t en;
    const char *e = html_esc(p[i], &en);
    sb_addn(out, e, en);
    i++;
  }
}

void escape_html_sb_slice(StrBuf *out, Slice s) { escape_html_sb(out, s.p, s.n); }

// ---------------- NFKD (generated-case exact) ----------------
// Sorted by code point. rep is the NFKD decomposition as UTF-8 bytes.
typedef struct {
  uint32_t cp;
  const char *rep;
} NfkdEnt;

static const NfkdEnt NFKD_TAB[] = {
    {0x00C5, "A\xcc\x8a"}, // Å -> A + U+030A
    {0x00C9, "E\xcc\x81"}, // É -> E + U+0301
    {0x00E3, "a\xcc\x83"}, // ã -> a + U+0303
    {0x00E5, "a\xcc\x8a"}, // å -> a + U+030A
    {0x00E8, "e\xcc\x80"}, // è -> e + U+0300
    {0x00E9, "e\xcc\x81"}, // é -> e + U+0301
    {0x00EF, "i\xcc\x88"}, // ï -> i + U+0308
    {0x00F1, "n\xcc\x83"}, // ñ -> n + U+0303
    {0x00F3, "o\xcc\x81"}, // ó -> o + U+0301
    {0x00F6, "o\xcc\x88"}, // ö -> o + U+0308
    {0x00FB, "u\xcc\x82"}, // û -> u + U+0302
    {0x00FC, "u\xcc\x88"}, // ü -> u + U+0308
    {0x017A, "z\xcc\x81"}, // ź -> z + U+0301
    {0x03AD, "\xce\xb5\xcc\x81"}, // έ -> ε + U+0301
    {0xFB01, "fi"},              // ﬁ -> f i
    {0xFF37, "W"},               // Ｗ -> W
    {0xFF44, "d"},               // ｄ -> d
    {0xFF45, "e"},               // ｅ -> e
    {0xFF49, "i"},               // ｉ -> i
};
static const size_t NFKD_N = sizeof(NFKD_TAB) / sizeof(NFKD_TAB[0]);

static const char *nfkd_rep(uint32_t cp) {
  size_t lo = 0, hi = NFKD_N;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (NFKD_TAB[mid].cp < cp) lo = mid + 1;
    else if (NFKD_TAB[mid].cp > cp) hi = mid;
    else return NFKD_TAB[mid].rep;
  }
  return NULL;
}

static int is_mark(uint32_t cp) {
  // \p{M} restricted to what NFKD of the generator inventory can yield.
  return cp >= 0x0300 && cp <= 0x036F;
}

// ---------------- slugify ----------------
#define SLUG_MAX 64

Slice slugify(Arena *a, Slice text) {
  // One pass: decode code point -> NFKD decompose -> drop marks -> keep/gap.
  char *out = (char *)arena_alloc(a, text.n * 2 + 1);
  size_t olen = 0;
  int gap = 0;
  size_t i = 0;
  while (i < text.n) {
    size_t adv;
    uint32_t cp = utf8_decode(text.p + i, text.n - i, &adv);
    i += adv;
    const char *rep = cp < 0x80 ? NULL : nfkd_rep(cp);
    if (!rep) {
      if (cp < 0x80) {
        // ASCII fast path (no decomposition, never a mark).
        char ch = (char)cp;
        char keep = 0;
        if (ch >= 'A' && ch <= 'Z') keep = (char)(ch + 32);
        else if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) keep = ch;
        if (!keep) {
          gap = olen > 0;
          continue;
        }
        if (gap) out[olen++] = '-';
        gap = 0;
        out[olen++] = keep;
      } else {
        // Non-ASCII, no decomposition: a gap (not a-z0-9).
        gap = olen > 0;
      }
      continue;
    }
    // Decomposed form: walk its code points.
    size_t rn = strlen(rep);
    size_t j = 0;
    while (j < rn) {
      size_t adv2;
      uint32_t cp2 = utf8_decode(rep + j, rn - j, &adv2);
      j += adv2;
      if (is_mark(cp2)) continue;
      if (cp2 >= 0x80) { // non-ASCII, not a-z0-9: a gap
        gap = olen > 0;
        continue;
      }
      char ch = (char)cp2;
      char keep = 0;
      if (ch >= 'A' && ch <= 'Z') keep = (char)(ch + 32);
      else if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) keep = ch;
      else {
        gap = olen > 0;
        continue;
      }
      if (gap) out[olen++] = '-';
      gap = 0;
      out[olen++] = keep;
    }
  }
  // slice(0, 64) then strip trailing '-'.
  if (olen > SLUG_MAX) olen = SLUG_MAX;
  while (olen > 0 && out[olen - 1] == '-') olen--;
  if (olen == 0) return sl_cstr("untitled");
  out[olen] = '\0';
  Slice r;
  r.p = out;
  r.n = olen;
  return r;
}

void strset_init(StrSet *s, Arena *a) {
  s->a = a;
  s->len = 0;
  s->cap = 16;
  s->items = (Slice *)arena_alloc(a, s->cap * sizeof(Slice));
}

int strset_has(StrSet *s, Slice v) {
  for (size_t i = 0; i < s->len; i++)
    if (slice_eq(s->items[i], v)) return 1;
  return 0;
}

void strset_add(StrSet *s, Slice v) {
  if (s->len == s->cap) {
    size_t nc = s->cap * 2;
    Slice *np = (Slice *)arena_alloc(s->a, nc * sizeof(Slice));
    memcpy(np, s->items, s->len * sizeof(Slice));
    s->items = np;
    s->cap = nc;
  }
  s->items[s->len++] = v;
}

Slice unique_slug(Arena *a, Slice slug, StrSet *taken) {
  Slice out = slug;
  for (int n = 2; strset_has(taken, out); n++) {
    char num[24];
    int nn = snprintf(num, sizeof(num), "-%d", n);
    size_t blen = slug.n + (size_t)nn;
    char *buf = (char *)arena_alloc(a, blen + 1);
    memcpy(buf, slug.p, slug.n);
    memcpy(buf + slug.n, num, (size_t)nn);
    buf[blen] = '\0';
    out.p = buf;
    out.n = blen;
  }
  strset_add(taken, out);
  return out;
}

// ---------------- longDate ----------------
static const char *MONTHS[12] = {"January", "February", "March",     "April",   "May",      "June",
                                 "July",    "August",   "September", "October", "November", "December"};

void long_date_sb(StrBuf *out, Slice date) {
  // date is "YYYY-MM-DD" (validated). d = Number(d) (no leading zero), m -> month, y as-is.
  const char *p = date.p;
  size_t n = date.n;
  size_t d1 = 0, d2 = 0; // positions of the two '-'
  for (size_t i = 0; i < n; i++) {
    if (p[i] == '-') {
      if (!d1) d1 = i;
      else {
        d2 = i;
        break;
      }
    }
  }
  long d = 0, m = 0;
  for (size_t i = d2 + 1; i < n; i++) d = d * 10 + (p[i] - '0');
  for (size_t i = d1 + 1; i < d2; i++) m = m * 10 + (p[i] - '0');
  char num[24];
  int nn = snprintf(num, sizeof(num), "%ld", d);
  sb_addn(out, num, (size_t)nn);
  sb_addc(out, ' ');
  sb_addcstr(out, MONTHS[m - 1]);
  sb_addc(out, ' ');
  sb_addn(out, p, d1); // year as-is
}

// ---------------- wordsOf ----------------
size_t words_of(const char *p, size_t n) { return sse2_count_words(p, n); }

// ---------------- resolvePath / dirOf ----------------
Slice resolve_path(Arena *a, Slice dir, Slice rel, int *ok) {
  // parts array (arena)
  size_t cap = 8, len = 0;
  Slice *parts = (Slice *)arena_alloc(a, cap * sizeof(Slice));
  if (dir.n > 0) {
    size_t s = 0;
    for (size_t i = 0; i <= dir.n; i++) {
      if (i == dir.n || dir.p[i] == '/') {
        if (len == cap) {
          // grow: allocate new (arena can't realloc; copy)
          size_t nc = cap * 2;
          Slice *np = (Slice *)arena_alloc(a, nc * sizeof(Slice));
          memcpy(np, parts, len * sizeof(Slice));
          parts = np;
          cap = nc;
        }
        parts[len++] = sl(dir.p + s, i - s);
        s = i + 1;
      }
    }
  }
  size_t s = 0;
  for (size_t i = 0; i <= rel.n; i++) {
    if (i == rel.n || rel.p[i] == '/') {
      Slice seg = sl(rel.p + s, i - s);
      s = i + 1;
      if (seg.n == 0 || (seg.n == 1 && seg.p[0] == '.')) continue;
      if (seg.n == 2 && seg.p[0] == '.' && seg.p[1] == '.') {
        if (len == 0) {
          *ok = 0;
          return sl("", 0);
        }
        len--;
        continue;
      }
      if (len == cap) {
        size_t nc = cap * 2;
        Slice *np = (Slice *)arena_alloc(a, nc * sizeof(Slice));
        memcpy(np, parts, len * sizeof(Slice));
        parts = np;
        cap = nc;
      }
      parts[len++] = seg;
    }
  }
  // join with '/'
  size_t total = 0;
  for (size_t i = 0; i < len; i++) total += parts[i].n + (i ? 1 : 0);
  char *buf = (char *)arena_alloc(a, total + 1);
  size_t o = 0;
  for (size_t i = 0; i < len; i++) {
    if (i) buf[o++] = '/';
    memcpy(buf + o, parts[i].p, parts[i].n);
    o += parts[i].n;
  }
  buf[o] = '\0';
  *ok = 1;
  return sl(buf, o);
}

Slice dir_of(Slice path) {
  size_t last = (size_t)-1;
  for (size_t i = 0; i < path.n; i++)
    if (path.p[i] == '/') last = i;
  if (last == (size_t)-1) return sl("", 0);
  return sl(path.p, last);
}
