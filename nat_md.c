// nat_md.c — Markdown lex + render (exact port of vendor lib/markdown.js).
// Inline scanning uses SSE2 for the next-special search.
#include "nat.h"

static int is_blank(Slice l) {
  for (size_t i = 0; i < l.n; i++)
    if (l.p[i] != ' ' && l.p[i] != '\t') return 0;
  return 1;
}

static Slice trim_line(Slice l) {
  size_t a = 0, b = l.n;
  while (a < b && (l.p[a] == ' ' || l.p[a] == '\t')) a++;
  while (b > a && (l.p[b - 1] == ' ' || l.p[b - 1] == '\t')) b--;
  return sl(l.p + a, b - a);
}

// FENCE_OPEN /^```([A-Za-z0-9_+-]*)[ \t]*$/ -> 1 + lang; 0 no match.
static int fence_open(Slice l, Slice *lang) {
  if (l.n < 3 || l.p[0] != '`' || l.p[1] != '`' || l.p[2] != '`') return 0;
  size_t i = 3;
  while (i < l.n) {
    char c = l.p[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '+' || c == '-')
      i++;
    else
      break;
  }
  *lang = sl(l.p + 3, i - 3);
  while (i < l.n && (l.p[i] == ' ' || l.p[i] == '\t')) i++;
  return i == l.n;
}

static int fence_close(Slice l) {
  if (l.n < 3 || l.p[0] != '`' || l.p[1] != '`' || l.p[2] != '`') return 0;
  size_t i = 3;
  while (i < l.n && (l.p[i] == ' ' || l.p[i] == '\t')) i++;
  return i == l.n;
}

// HEADING /^(#{1,6})[ \t]+(.*?)[ \t]*$/ with non-empty text. -> 1 + depth + text.
static int heading_of(Slice l, int *depth, Slice *text) {
  size_t i = 0;
  while (i < l.n && l.p[i] == '#') i++;
  if (i == 0 || i > 6) return 0;
  size_t j = i;
  if (j >= l.n || (l.p[j] != ' ' && l.p[j] != '\t')) return 0;
  while (j < l.n && (l.p[j] == ' ' || l.p[j] == '\t')) j++;
  // text = rest with trailing [ \t] stripped (lazy (.*?) + [ \t]*$)
  size_t e = l.n;
  while (e > j && (l.p[e - 1] == ' ' || l.p[e - 1] == '\t')) e--;
  if (e == j) return 0;
  *depth = (int)i;
  *text = sl(l.p + j, e - j);
  return 1;
}

static int is_hr(Slice l) {
  // /^(?:---|\*\*\*)[ \t]*$/
  size_t need = 0;
  if (l.n >= 3 && l.p[0] == '-' && l.p[1] == '-' && l.p[2] == '-') need = 3;
  else if (l.n >= 3 && l.p[0] == '*' && l.p[1] == '*' && l.p[2] == '*')
    need = 3;
  else
    return 0;
  size_t i = need;
  while (i < l.n && (l.p[i] == ' ' || l.p[i] == '\t')) i++;
  return i == l.n;
}

static int ends_paragraph(Slice l) {
  int d;
  Slice t;
  return is_blank(l) || fence_open(l, &t) || heading_of(l, &d, &t) || is_hr(l);
}

// BULLET /^[-*] (.*)$/ -> 1 + captured (may be empty)
static int bullet_of(Slice l, Slice *cap) {
  if (l.n >= 2 && (l.p[0] == '-' || l.p[0] == '*') && l.p[1] == ' ') {
    *cap = sl(l.p + 2, l.n - 2);
    return 1;
  }
  return 0;
}

// ORDERED /^(\d{1,9})\. (.*)$/ -> 1 + number + captured
static int ordered_of(Slice l, long *num, Slice *cap) {
  size_t i = 0;
  while (i < l.n && l.p[i] >= '0' && l.p[i] <= '9' && i < 9) i++;
  if (i == 0 || i >= l.n || l.p[i] != '.' || i + 1 >= l.n || l.p[i + 1] != ' ') return 0;
  long v = 0;
  for (size_t k = 0; k < i; k++) v = v * 10 + (l.p[k] - '0');
  *num = v;
  *cap = sl(l.p + i + 2, l.n - (i + 2));
  return 1;
}

// CONTINUED /^ {2,}\S/ (space run then a non-whitespace char)
static int is_continued(Slice l) {
  size_t i = 0;
  while (i < l.n && l.p[i] == ' ') i++;
  if (i < 2) return 0;
  if (i >= l.n) return 0;
  unsigned char c = (unsigned char)l.p[i];
  if (c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r') return 0;
  return 1; // bytes >= 0x80 are non-whitespace in the generator inventory
}

// ---------------- inline ----------------
static int is_escapable(char c) {
  switch (c) {
    case '\\':
    case '`':
    case '*':
    case '_':
    case '[':
    case ']':
    case '(':
    case ')':
    case '#':
    case '+':
    case '-':
    case '.':
    case '!':
    case '>':
      return 1;
    default:
      return 0;
  }
}

static long find_unescaped(const char *s, size_t n, const char *what, size_t wlen, size_t from) {
  // SSE2: jump to the next '\\' or what[0], then check the escape / match.
  uint8_t set[2];
  set[0] = '\\';
  set[1] = (uint8_t)what[0];
  size_t j = from;
  while (j + wlen <= n) {
    size_t f = sse2_find_any(s + j, n - j, set, 2);
    if (f >= n - j) return -1;
    j += f;
    // No wlen bytes left from here: no match can start at or after j
    // (a match must start on what[0], and the only such bytes are >= j).
    if (j + wlen > n) return -1;
    if (s[j] == '\\' && j + 1 < n && is_escapable(s[j + 1])) {
      j += 2;
      continue;
    }
    if (s[j] == what[0] && memcmp(s + j, what, wlen) == 0) return (long)j;
    j++;
  }
  return -1;
}

static long find_single_star(const char *s, size_t n, size_t from) {
  static const uint8_t SET[2] = {'\\', '*'};
  static __m128i vec[2];
  static int vec_ready = 0;
  if (!vec_ready) {
    for (int k = 0; k < 2; k++) vec[k] = _mm_set1_epi8((char)SET[k]);
    vec_ready = 1;
  }
  size_t j = from;
  while (j < n) {
    size_t f = sse2_find_any_v(s + j, n - j, vec, SET, 2);
    if (f >= n - j) return -1;
    j += f;
    char c = s[j];
    if (c == '\\' && j + 1 < n && is_escapable(s[j + 1])) {
      j += 2;
      continue;
    }
    if (c != '*') {
      j++;
      continue;
    }
    if (j + 1 < n && s[j + 1] == '*') {
      j += 2;
      continue;
    }
    return (long)j;
  }
  return -1;
}

static long find_byte_from(const char *s, size_t n, char c, size_t from) {
  size_t j = sse2_find_byte(s + from, n - from, (uint8_t)c);
  if (j >= n - from) return -1;
  return (long)(from + j);
}

static int has_ws(Slice t) {
  size_t i = 0;
  while (i < t.n) {
    size_t adv;
    uint32_t cp = utf8_decode(t.p + i, t.n - i, &adv);
    if (cp == 0x09 || cp == 0x0A || cp == 0x0B || cp == 0x0C || cp == 0x0D || cp == 0x20 || cp == 0xA0 ||
        cp == 0x1680 || cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000 ||
        cp == 0xFEFF || (cp >= 0x2000 && cp <= 0x200A))
      return 1;
    i += adv;
  }
  return 0;
}

static void md_inline(Arena *a, StrBuf *out, const char *s, size_t n, MdEnv *env) {
  static const uint8_t SPEC[9] = {'\\', '`', '*', '[', '&', '<', '>', '"', '\''};
  static __m128i spec_vec[9];
  static int spec_vec_ready = 0;
  if (!spec_vec_ready) {
    for (int k = 0; k < 9; k++) spec_vec[k] = _mm_set1_epi8((char)SPEC[k]);
    spec_vec_ready = 1;
  }
  size_t i = 0;
  while (i < n) {
    size_t j = sse2_find_any_v(s + i, n - i, spec_vec, SPEC, 9);
    if (j > 0) sb_addn(out, s + i, j);
    i += j;
    if (i >= n) break;
    char c = s[i];
    if (c == '\\' && i + 1 < n && is_escapable(s[i + 1])) {
      char nx = s[i + 1];
      if (nx == '>') sb_addcstr(out, "&gt;");
      else sb_addc(out, nx);
      i += 2;
      continue;
    }
    if (c == '`') {
      long k = find_byte_from(s, n, '`', i + 1);
      if (k > (long)(i + 1)) {
        sb_addcstr(out, "<code>");
        escape_html_sb(out, s + i + 1, (size_t)(k - (long)(i + 1)));
        sb_addcstr(out, "</code>");
        i = (size_t)k + 1;
      } else {
        sb_addc(out, '`');
        i++;
      }
      continue;
    }
    if (c == '*') {
      if (i + 1 < n && s[i + 1] == '*') {
        long k = find_unescaped(s, n, "**", 2, i + 2);
        if (k > (long)(i + 2)) {
          sb_addcstr(out, "<strong>");
          md_inline(a, out, s + i + 2, (size_t)(k - (long)(i + 2)), env);
          sb_addcstr(out, "</strong>");
          i = (size_t)k + 2;
        } else {
          sb_addcstr(out, "**");
          i += 2;
        }
        continue;
      }
      long k = find_single_star(s, n, i + 1);
      if (k > (long)(i + 1)) {
        sb_addcstr(out, "<em>");
        md_inline(a, out, s + i + 1, (size_t)(k - (long)(i + 1)), env);
        sb_addcstr(out, "</em>");
        i = (size_t)k + 1;
      } else {
        sb_addc(out, '*');
        i++;
      }
      continue;
    }
    if (c == '[') {
      long k = find_unescaped(s, n, "]", 1, i + 1);
      long m = -1;
      if (k != -1 && (size_t)(k + 1) < n && s[k + 1] == '(') m = find_byte_from(s, n, ')', (size_t)k + 2);
      Slice target = sl("", 0);
      if (m != -1) target = sl(s + k + 2, (size_t)(m - (k + 2)));
      if (m != -1 && target.n > 0 && !has_ws(target)) {
        Slice href = env->link(env->ctx, target);
        sb_addcstr(out, "<a href=\"");
        escape_html_sb_slice(out, href);
        sb_addcstr(out, "\">");
        md_inline(a, out, s + i + 1, (size_t)(k - (long)(i + 1)), env);
        sb_addcstr(out, "</a>");
        i = (size_t)m + 1;
        continue;
      }
      sb_addc(out, '[');
      i++;
      continue;
    }
    // escape chars &, <, >, ", '
    size_t en;
    switch (c) {
      case '&': sb_addcstr(out, "&amp;"); break;
      case '<': sb_addcstr(out, "&lt;"); break;
      case '>': sb_addcstr(out, "&gt;"); break;
      case '"': sb_addcstr(out, "&quot;"); break;
      case '\'': sb_addcstr(out, "&#39;"); break;
      default: sb_addc(out, c); break;
    }
    (void)en;
    i++;
  }
}

// ---------------- lex + render ----------------
typedef enum { B_HEADING, B_CODE, B_HR, B_QUOTE, B_LIST, B_PARA } BKind;

typedef struct {
  BKind kind;
  int depth;
  Slice text; // heading text / paragraph text
  Slice lang; // code
  Slice *lines; // code body lines / blockquote lines (trimmed, non-empty)
  size_t nlines;
  int ordered, start; // list
  Slice *items;
  size_t nitems;
} Block;

void render_markdown(Arena *a, Slice src, MdEnv *env, MdOut *out) {
  // split into lines (views, no copy)
  size_t cap = 64, nlines = 0;
  Slice *lines = (Slice *)arena_alloc(a, cap * sizeof(Slice));
  size_t s = 0;
  for (size_t i = 0; i <= src.n; i++) {
    if (i == src.n || src.p[i] == '\n') {
      if (nlines == cap) {
        size_t nc = cap * 2;
        Slice *np = (Slice *)arena_alloc(a, nc * sizeof(Slice));
        memcpy(np, lines, nlines * sizeof(Slice));
        lines = np;
        cap = nc;
      }
      lines[nlines++] = sl(src.p + s, i - s);
      s = i + 1;
    }
  }
  // lex into blocks (arena)
  // One scratch StrBuf for the whole function (list-item / paragraph
  // accumulation in lex, quote join in render): sequential uses only, so
  // reuse is exact and avoids a malloc/free per block, item and paragraph.
  StrBuf scratch;
  sb_init(&scratch);
  size_t bcap = 32, nblocks = 0;
  Block *blocks = (Block *)arena_alloc(a, bcap * sizeof(Block));
  size_t i = 0;
  while (i < nlines) {
    Slice line = lines[i];
    if (is_blank(line)) {
      i++;
      continue;
    }
    Slice lang;
    if (fence_open(line, &lang)) {
      Block b;
      memset(&b, 0, sizeof(b));
      b.kind = B_CODE;
      b.lang = lang;
      size_t lcap = 8, ln = 0;
      Slice *body = (Slice *)arena_alloc(a, lcap * sizeof(Slice));
      i++;
      while (i < nlines && !fence_close(lines[i])) {
        if (ln == lcap) {
          size_t nc = lcap * 2;
          Slice *np = (Slice *)arena_alloc(a, nc * sizeof(Slice));
          memcpy(np, body, ln * sizeof(Slice));
          body = np;
          lcap = nc;
        }
        body[ln++] = lines[i++];
      }
      i++; // skip close (or past end)
      b.lines = body;
      b.nlines = ln;
      if (nblocks == bcap) {
        size_t nc = bcap * 2;
        Block *np = (Block *)arena_alloc(a, nc * sizeof(Block));
        memcpy(np, blocks, nblocks * sizeof(Block));
        blocks = np;
        bcap = nc;
      }
      blocks[nblocks++] = b;
      continue;
    }
    int depth;
    Slice htext;
    if (heading_of(line, &depth, &htext)) {
      Block b;
      memset(&b, 0, sizeof(b));
      b.kind = B_HEADING;
      b.depth = depth;
      b.text = htext;
      if (nblocks == bcap) {
        size_t nc = bcap * 2;
        Block *np = (Block *)arena_alloc(a, nc * sizeof(Block));
        memcpy(np, blocks, nblocks * sizeof(Block));
        blocks = np;
        bcap = nc;
      }
      blocks[nblocks++] = b;
      i++;
      continue;
    }
    if (is_hr(line)) {
      Block b;
      memset(&b, 0, sizeof(b));
      b.kind = B_HR;
      if (nblocks == bcap) {
        size_t nc = bcap * 2;
        Block *np = (Block *)arena_alloc(a, nc * sizeof(Block));
        memcpy(np, blocks, nblocks * sizeof(Block));
        blocks = np;
        bcap = nc;
      }
      blocks[nblocks++] = b;
      i++;
      continue;
    }
    if (line.n > 0 && line.p[0] == '>') {
      Block b;
      memset(&b, 0, sizeof(b));
      b.kind = B_QUOTE;
      size_t lcap = 8, ln = 0;
      Slice *body = (Slice *)arena_alloc(a, lcap * sizeof(Slice));
      while (i < nlines && lines[i].n > 0 && lines[i].p[0] == '>') {
        Slice l = lines[i++];
        // strip /^> ?/
        if (l.n >= 2 && l.p[1] == ' ') l = sl(l.p + 2, l.n - 2);
        else l = sl(l.p + 1, l.n - 1);
        Slice t = trim_line(l);
        if (t.n == 0) continue;
        if (ln == lcap) {
          size_t nc = lcap * 2;
          Slice *np = (Slice *)arena_alloc(a, nc * sizeof(Slice));
          memcpy(np, body, ln * sizeof(Slice));
          body = np;
          lcap = nc;
        }
        body[ln++] = t;
      }
      b.lines = body;
      b.nlines = ln;
      if (nblocks == bcap) {
        size_t nc = bcap * 2;
        Block *np = (Block *)arena_alloc(a, nc * sizeof(Block));
        memcpy(np, blocks, nblocks * sizeof(Block));
        blocks = np;
        bcap = nc;
      }
      blocks[nblocks++] = b;
      continue;
    }
    Slice bcap_s;
    long onum;
    Slice ocap;
    int is_bullet = bullet_of(line, &bcap_s);
    int is_ordered = !is_bullet && ordered_of(line, &onum, &ocap);
    if (is_bullet || is_ordered) {
      Block b;
      memset(&b, 0, sizeof(b));
      b.kind = B_LIST;
      b.ordered = is_ordered;
      b.start = is_ordered ? (int)onum : 1;
      size_t icap = 8, ni = 0;
      Slice *items = (Slice *)arena_alloc(a, icap * sizeof(Slice));
      // each item is a list of trimmed lines joined with '\n'; build in a scratch StrBuf per item
      while (i < nlines) {
        Slice cap_s;
        long num2;
        Slice ocap2;
        int m_b = bullet_of(lines[i], &cap_s);
        int m_o = 0;
        if (is_ordered) m_o = ordered_of(lines[i], &num2, &ocap2);
        // marker is BULLET for bullet lists, ORDERED for ordered lists
        int hit = is_ordered ? m_o : m_b;
        if (hit) {
          Slice first = trim_line(is_ordered ? ocap2 : cap_s);
          // start new item: accumulate the item text in the scratch buffer.
          scratch.len = 0;
          sb_add(&scratch, first);
          i++;
          // continued lines for this item
          while (i < nlines && !((is_ordered ? ordered_of(lines[i], &num2, &ocap2)
                                             : bullet_of(lines[i], &cap_s)))) {
            if (!is_continued(lines[i])) break;
            sb_addc(&scratch, '\n');
            sb_add(&scratch, trim_line(lines[i]));
            i++;
          }
          if (ni == icap) {
            size_t nc = icap * 2;
            Slice *np = (Slice *)arena_alloc(a, nc * sizeof(Slice));
            memcpy(np, items, ni * sizeof(Slice));
            items = np;
            icap = nc;
          }
          char *ip = arena_dup(a, scratch.buf, scratch.len);
          items[ni++] = sl(ip, scratch.len);
        } else if (is_continued(lines[i])) {
          // continued line with no current item: cannot happen (an item always exists here
          // because we only enter this branch after at least one hit), but mirror vendor:
          // vendor appends to items[items.length-1]; if items is empty this would throw.
          // In practice unreachable: the first line matched, so items is non-empty.
          // Treat as break to stay safe (unreachable path).
          break;
        } else {
          break;
        }
      }
      b.items = items;
      b.nitems = ni;
      if (nblocks == bcap) {
        size_t nc = bcap * 2;
        Block *np = (Block *)arena_alloc(a, nc * sizeof(Block));
        memcpy(np, blocks, nblocks * sizeof(Block));
        blocks = np;
        bcap = nc;
      }
      blocks[nblocks++] = b;
      continue;
    }
    // paragraph
    {
      scratch.len = 0;
      sb_add(&scratch, trim_line(line));
      i++;
      while (i < nlines && !ends_paragraph(lines[i])) {
        sb_addc(&scratch, '\n');
        sb_add(&scratch, trim_line(lines[i]));
        i++;
      }
      Block b;
      memset(&b, 0, sizeof(b));
      b.kind = B_PARA;
      char *tp = arena_dup(a, scratch.buf, scratch.len);
      b.text = sl(tp, scratch.len);
      if (nblocks == bcap) {
        size_t nc = bcap * 2;
        Block *np = (Block *)arena_alloc(a, nc * sizeof(Block));
        memcpy(np, blocks, nblocks * sizeof(Block));
        blocks = np;
        bcap = nc;
      }
      blocks[nblocks++] = b;
    }
  }

  // render — blocks go straight into `html` (no per-block temp buffer);
  // the shared scratch serves the quote join. Excerpt + heading TOC html
  // are captured as offsets into html and copied out at the end.
  StrSet ids;
  strset_init(&ids, a);
  StrBuf html;
  sb_init(&html);
  size_t ex_start = 0, ex_end = 0;
  int have_excerpt = 0;
  out->toc = NULL;
  out->ntoc = 0;
  out->captoc = 0;
  for (size_t bi = 0; bi < nblocks; bi++) {
    Block *t = &blocks[bi];
    switch (t->kind) {
      case B_HEADING: {
        Slice id = unique_slug(a, slugify(a, t->text), &ids);
        char num[16];
        int nn = snprintf(num, sizeof(num), "<h%d id=\"", t->depth);
        sb_addn(&html, num, (size_t)nn);
        sb_add(&html, id);
        sb_addcstr(&html, "\">");
        size_t h_start = html.len;
        md_inline(a, &html, t->text.p, t->text.n, env);
        size_t h_end = html.len;
        nn = snprintf(num, sizeof(num), "</h%d>", t->depth);
        sb_addn(&html, num, (size_t)nn);
        if (t->depth == 2 || t->depth == 3) {
          if (out->ntoc == out->captoc) {
            size_t nc = out->captoc ? out->captoc * 2 : 8;
            void *np = arena_alloc(a, nc * sizeof(*out->toc));
            if (out->toc) memcpy(np, out->toc, out->ntoc * sizeof(*out->toc));
            out->toc = (void *)np;
            out->captoc = nc;
          }
          char *hp = arena_dup(a, html.buf + h_start, h_end - h_start);
          char *ip = arena_dup(a, id.p, id.n);
          out->toc[out->ntoc].level = t->depth;
          out->toc[out->ntoc].id = sl(ip, id.n);
          out->toc[out->ntoc].html = sl(hp, h_end - h_start);
          out->ntoc++;
        }
        break;
      }
      case B_CODE: {
        sb_addcstr(&html, "<pre><code");
        if (t->lang.n > 0) {
          sb_addcstr(&html, " class=\"language-");
          escape_html_sb_slice(&html, t->lang);
          sb_addc(&html, '"');
        }
        sb_addc(&html, '>');
        for (size_t k = 0; k < t->nlines; k++) {
          escape_html_sb_slice(&html, t->lines[k]);
          sb_addc(&html, '\n');
        }
        sb_addcstr(&html, "</code></pre>");
        break;
      }
      case B_HR:
        sb_addcstr(&html, "<hr>");
        break;
      case B_QUOTE: {
        if (t->nlines == 0) {
          sb_addcstr(&html, "<blockquote>\n</blockquote>");
        } else {
          sb_addcstr(&html, "<blockquote>\n<p>");
          // join lines with '\n' then inline (join must precede inline:
          // markup may span lines)
          scratch.len = 0;
          for (size_t k = 0; k < t->nlines; k++) {
            if (k) sb_addc(&scratch, '\n');
            sb_add(&scratch, t->lines[k]);
          }
          md_inline(a, &html, scratch.buf, scratch.len, env);
          sb_addcstr(&html, "</p>\n</blockquote>");
        }
        break;
      }
      case B_LIST: {
        if (t->ordered && t->start != 1) {
          char num[32];
          int nn = snprintf(num, sizeof(num), "<ol start=\"%d\">", t->start);
          sb_addn(&html, num, (size_t)nn);
        } else {
          sb_addcstr(&html, t->ordered ? "<ol>" : "<ul>");
        }
        sb_addc(&html, '\n');
        for (size_t k = 0; k < t->nitems; k++) {
          sb_addcstr(&html, "<li>");
          md_inline(a, &html, t->items[k].p, t->items[k].n, env);
          sb_addcstr(&html, "</li>\n");
        }
        sb_addcstr(&html, t->ordered ? "</ol>" : "</ul>");
        break;
      }
      case B_PARA: {
        sb_addcstr(&html, "<p>");
        if (!have_excerpt) {
          ex_start = html.len;
          md_inline(a, &html, t->text.p, t->text.n, env);
          ex_end = html.len;
          have_excerpt = 1;
        } else {
          md_inline(a, &html, t->text.p, t->text.n, env);
        }
        sb_addcstr(&html, "</p>");
        break;
      }
    }
    sb_addc(&html, '\n');
  }
  char *hp = arena_dup(a, html.buf, html.len);
  out->html = sl(hp, html.len);
  if (have_excerpt) {
    char *ep = arena_dup(a, html.buf + ex_start, ex_end - ex_start);
    out->excerpt = sl(ep, ex_end - ex_start);
  } else {
    out->excerpt = sl("", 0);
  }
  sb_free(&html);
  sb_free(&scratch);
}
