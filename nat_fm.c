// nat_fm.c — front matter parser (strict subset of YAML block mappings).
// Exact port of vendor lib/frontmatter.js, including error strings.
#include "nat.h"

static int is_blank_line(Slice l) {
  for (size_t i = 0; i < l.n; i++)
    if (l.p[i] != ' ' && l.p[i] != '\t') return 0;
  return 1;
}

// JS String trim (WhiteSpace + LineTerminator) over a slice -> trimmed slice.

// Trim trailing [ \t] only (raw value trim in parseFrontMatter).
static Slice rtrim_st(Slice s) {
  while (s.n > 0 && (s.p[s.n - 1] == ' ' || s.p[s.n - 1] == '\t')) s.n--;
  return s;
}

static int days_in(int y, int m) {
  if (m == 2) return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0 ? 29 : 28;
  return (m == 4 || m == 6 || m == 9 || m == 11) ? 30 : 31;
}

// stringOf(raw) -> 0 ok (value_out), 1 error (err_out = message without key prefix).
static int string_of(Slice raw, Arena *a, Slice *value_out, StrBuf *err_out) {
  if (raw.n > 0 && raw.p[0] == '"') {
    if (raw.n < 2 || raw.p[raw.n - 1] != '"') {
      sb_addcstr(err_out, "a quoted string is not closed");
      return 1;
    }
    StrBuf out;
    sb_init(&out);
    for (size_t i = 1; i + 1 < raw.n; i++) {
      char c = raw.p[i];
      if (c == '\\') {
        char n = raw.p[i + 1];
        if ((n != '"' && n != '\\') || i + 1 == raw.n - 1) {
          sb_addcstr(err_out, "a quoted string holds an escape other than \\\" or \\\\");
          sb_free(&out);
          return 1;
        }
        sb_addc(&out, n);
        i++;
      } else if (c == '"') {
        sb_addcstr(err_out, "a quoted string holds a stray quote");
        sb_free(&out);
        return 1;
      } else {
        sb_addc(&out, c);
      }
    }
    char *s = arena_dup(a, out.buf, out.len);
    size_t n = out.len;
    sb_free(&out);
    *value_out = sl(s, n);
    return 0;
  }
  if (raw.n > 0 && raw.p[0] == '[') {
    sb_addcstr(err_out, "a bare string opens on [");
    return 1;
  }
  *value_out = raw;
  return 0;
}

static int is_name_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
}

// valueOf(key, raw). Returns 0 ok, 1 error (err_out = full message).
static int value_of(Slice key, Slice raw, Arena *a, FMData *d, StrBuf *err_out) {
  char keybuf[32];
  size_t klen = key.n < sizeof(keybuf) - 1 ? key.n : sizeof(keybuf) - 1;
  memcpy(keybuf, key.p, klen);
  keybuf[klen] = '\0';

  if (slice_eq_cstr(key, "title") || slice_eq_cstr(key, "summary")) {
    Slice v;
    StrBuf e;
    sb_init(&e);
    if (string_of(raw, a, &v, &e)) {
      sb_addcstr(err_out, "key \"");
      sb_addn(err_out, key.p, key.n);
      sb_addcstr(err_out, "\": ");
      sb_addn(err_out, e.buf, e.len);
      sb_free(&e);
      return 1;
    }
    sb_free(&e);
    if (slice_eq_cstr(key, "title")) {
      d->title = v;
      d->has_title = 1;
    } else {
      d->summary = v;
      d->has_summary = 1;
    }
    return 0;
  }
  if (slice_eq_cstr(key, "date")) {
    // DATE /^(\d{4})-(\d{2})-(\d{2})$/
    int ok = 0, y = 0, mo = 0, dd = 0;
    if (raw.n == 10 && raw.p[4] == '-' && raw.p[7] == '-') {
      int dig = 1;
      for (size_t i = 0; i < 10; i++) {
        if (i == 4 || i == 7) continue;
        if (raw.p[i] < '0' || raw.p[i] > '9') dig = 0;
      }
      if (dig) {
        y = (raw.p[0] - '0') * 1000 + (raw.p[1] - '0') * 100 + (raw.p[2] - '0') * 10 + (raw.p[3] - '0');
        mo = (raw.p[5] - '0') * 10 + (raw.p[6] - '0');
        dd = (raw.p[8] - '0') * 10 + (raw.p[9] - '0');
        if (y >= 1970 && y <= 2099 && mo >= 1 && mo <= 12 && dd >= 1 && dd <= days_in(y, mo)) ok = 1;
      }
    }
    if (!ok) {
      sb_addcstr(err_out, "key \"date\": not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)");
      return 1;
    }
    d->date = raw;
    d->has_date = 1;
    return 0;
  }
  if (slice_eq_cstr(key, "tags")) {
    if (raw.n < 2 || raw.p[0] != '[' || raw.p[raw.n - 1] != ']') {
      sb_addcstr(err_out, "key \"tags\": not a list [a, b]");
      return 1;
    }
    Slice inner = sl(raw.p + 1, raw.n - 2);
    if (js_trim(inner).n == 0) {
      d->tags = NULL;
      d->ntags = 0;
      d->has_tags = 1;
      return 0;
    }
    // split ',' then trim each
    size_t cap = 4, cnt = 0;
    Slice *tags = (Slice *)arena_alloc(a, cap * sizeof(Slice));
    size_t s = 0;
    for (size_t i = 0; i <= inner.n; i++) {
      if (i == inner.n || inner.p[i] == ',') {
        Slice t = js_trim(sl(inner.p + s, i - s));
        s = i + 1;
        if (t.n == 0) {
          sb_addcstr(err_out, "key \"tags\": an empty tag, or one holding \" [ or ]");
          return 1;
        }
        // TAG /^[^"[\]]+$/
        for (size_t j = 0; j < t.n; j++) {
          char c = t.p[j];
          if (c == '"' || c == '[' || c == ']') {
            sb_addcstr(err_out, "key \"tags\": an empty tag, or one holding \" [ or ]");
            return 1;
          }
        }
        if (cnt == cap) {
          size_t nc = cap * 2;
          Slice *np = (Slice *)arena_alloc(a, nc * sizeof(Slice));
          memcpy(np, tags, cnt * sizeof(Slice));
          tags = np;
          cap = nc;
        }
        tags[cnt++] = t;
      }
    }
    d->tags = tags;
    d->ntags = cnt;
    d->has_tags = 1;
    return 0;
  }
  if (slice_eq_cstr(key, "draft")) {
    if (slice_eq_cstr(raw, "true")) {
      d->draft = 1;
      d->has_draft = 1;
      return 0;
    }
    if (slice_eq_cstr(raw, "false")) {
      d->draft = 0;
      d->has_draft = 1;
      return 0;
    }
    sb_addcstr(err_out, "key \"draft\": true or false");
    return 1;
  }
  if (slice_eq_cstr(key, "layout")) {
    if (raw.n == 0) {
      sb_addcstr(err_out, "key \"layout\": not a template name");
      return 1;
    }
    for (size_t i = 0; i < raw.n; i++)
      if (!is_name_char(raw.p[i])) {
        sb_addcstr(err_out, "key \"layout\": not a template name");
        return 1;
      }
    d->layout = raw;
    d->has_layout = 1;
    return 0;
  }
  if (slice_eq_cstr(key, "slug")) {
    // SLUG /^[a-z0-9]+(?:-[a-z0-9]+)*$/ and length <= 64
    int ok = raw.n > 0 && raw.n <= 64;
    if (ok) {
      // segments of [a-z0-9]+ joined by single '-'
      size_t i = 0;
      if (raw.p[0] == '-' || raw.p[raw.n - 1] == '-') ok = 0;
      while (ok && i < raw.n) {
        char c = raw.p[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
          i++;
        } else if (c == '-') {
          if (i + 1 >= raw.n || raw.p[i + 1] == '-') ok = 0;
          i++;
        } else
          ok = 0;
      }
    }
    if (!ok) {
      sb_addcstr(err_out, "key \"slug\": not a slug");
      return 1;
    }
    d->slug = raw;
    d->has_slug = 1;
    return 0;
  }
  // unreachable (caller checks KEYS), but keep the message shape
  sb_addcstr(err_out, "unknown key \"");
  sb_addn(err_out, key.p, key.n);
  sb_addcstr(err_out, "\"");
  return 1;
}

static const char *KEYS[7] = {"title", "date", "tags", "draft", "layout", "slug", "summary"};

int parse_front_matter(const char *text, size_t n, Arena *a, FMData *data_out, Slice *body_out, StrBuf *err_out) {
  memset(data_out, 0, sizeof(*data_out));
  // Split into lines, but STOP as soon as the closing "---" is found.
  // The body is never split: it is exactly the contiguous slice of the
  // text after the closing fence's newline (joining the remaining lines
  // with '\n' reconstructs that slice byte-for-byte, including the
  // trailing-newline and no-trailing-newline cases), so no copy is needed.
  // This avoids a scalar scan of the whole body plus a full body memcpy.
  size_t cap = 32, nlines = 0;
  Slice *lines = (Slice *)arena_alloc(a, cap * sizeof(Slice));
  size_t s = 0;
  long close = -1;
  size_t body_start = n;
  for (size_t i = 0; i <= n; i++) {
    if (i == n || text[i] == '\n') {
      if (nlines == cap) {
        size_t nc = cap * 2;
        Slice *np = (Slice *)arena_alloc(a, nc * sizeof(Slice));
        memcpy(np, lines, nlines * sizeof(Slice));
        lines = np;
        cap = nc;
      }
      lines[nlines++] = sl(text + s, i - s);
      if (nlines > 1 && slice_eq_cstr(lines[nlines - 1], "---")) {
        close = (long)(nlines - 1);
        body_start = i + 1;
        if (body_start > n) body_start = n;
        break;
      }
      s = i + 1;
    }
  }
  if (nlines == 0 || !slice_eq_cstr(lines[0], "---")) {
    sb_addcstr(err_out, "no front matter");
    return 1;
  }
  if (close == -1) {
    sb_addcstr(err_out, "the front matter is not closed");
    return 1;
  }
  for (long i = 1; i < close; i++) {
    Slice line = lines[i];
    if (is_blank_line(line)) continue;
    if (line.n > 0 && line.p[0] == '#') continue;
    // LINE /^([a-z_]+):(?: (.*))?$/
    size_t k = 0;
    while (k < line.n && ((line.p[k] >= 'a' && line.p[k] <= 'z') || line.p[k] == '_')) k++;
    if (k == 0 || k >= line.n || line.p[k] != ':') {
      char num[32];
      int nn = snprintf(num, sizeof(num), "line %ld is not \"key: value\"", i + 1);
      sb_addn(err_out, num, (size_t)nn);
      return 1;
    }
    Slice key = sl(line.p, k);
    Slice raw;
    if (k + 1 == line.n) {
      raw = sl("", 0);
    } else if (line.p[k + 1] == ' ') {
      raw = rtrim_st(sl(line.p + k + 2, line.n - (k + 2)));
    } else {
      char num[32];
      int nn = snprintf(num, sizeof(num), "line %ld is not \"key: value\"", i + 1);
      sb_addn(err_out, num, (size_t)nn);
      return 1;
    }
    int known = 0;
    for (int q = 0; q < 7; q++)
      if (slice_eq_cstr(key, KEYS[q])) known = 1;
    if (!known) {
      sb_addcstr(err_out, "unknown key \"");
      sb_addn(err_out, key.p, key.n);
      sb_addcstr(err_out, "\"");
      return 1;
    }
    // duplicate check
    int dup = 0;
    if (slice_eq_cstr(key, "title") && data_out->has_title) dup = 1;
    if (slice_eq_cstr(key, "date") && data_out->has_date) dup = 1;
    if (slice_eq_cstr(key, "tags") && data_out->has_tags) dup = 1;
    if (slice_eq_cstr(key, "draft") && data_out->has_draft) dup = 1;
    if (slice_eq_cstr(key, "layout") && data_out->has_layout) dup = 1;
    if (slice_eq_cstr(key, "slug") && data_out->has_slug) dup = 1;
    if (slice_eq_cstr(key, "summary") && data_out->has_summary) dup = 1;
    if (dup) {
      sb_addcstr(err_out, "key \"");
      sb_addn(err_out, key.p, key.n);
      sb_addcstr(err_out, "\" is given twice");
      return 1;
    }
    if (raw.n == 0) {
      sb_addcstr(err_out, "key \"");
      sb_addn(err_out, key.p, key.n);
      sb_addcstr(err_out, "\" has no value");
      return 1;
    }
    if (value_of(key, raw, a, data_out, err_out)) return 1;
  }
  if (!data_out->has_title) {
    sb_addcstr(err_out, "no title");
    return 1;
  }
  // body = text[body_start..n] (see the note at the top of this function:
  // the join of the remaining lines is exactly this slice, no copy needed).
  *body_out = sl(text + body_start, n - body_start);
  return 0;
}
