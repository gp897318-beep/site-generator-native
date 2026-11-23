// nat_json.c — JSON parser (into Val) and serializer. The parser is a
// recursive-descent reader with an SSE2 fast path for string bodies; the
// serializer emits insertion-ordered keys and raw UTF-8 strings.
#include "nat.h"
#include <emmintrin.h>

typedef struct {
  const char *p;
  size_t n, i;
  Arena *a;
  StrBuf *err;
} JP;

static void jp_err(JP *j, const char *what) {
  char buf[64];
  int n = snprintf(buf, sizeof(buf), "JSON parse error at offset %zu: %s", j->i, what);
  sb_addn(j->err, buf, (size_t)n);
}

static void jp_ws(JP *j) {
  while (j->i < j->n) {
    char c = j->p[j->i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') j->i++;
    else break;
  }
}

static Val *jp_value(JP *j);

static int hex_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int parse_hex4(const char *p, uint32_t *out) {
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) {
    int h = hex_val(p[i]);
    if (h < 0) return 0;
    v = (v << 4) | (uint32_t)h;
  }
  *out = v;
  return 1;
}

// Parse a string starting at the opening quote. 0 ok.
static int jp_string(JP *j, Slice *out) {
  // j->p[j->i] == '"'
  j->i++;
  StrBuf b;
  sb_init(&b);
  static const uint8_t SET[2] = {'"', '\\'};
  while (1) {
    if (j->i >= j->n) {
      sb_free(&b);
      jp_err(j, "unterminated string");
      return 1;
    }
    size_t f = sse2_find_any(j->p + j->i, j->n - j->i, SET, 2);
    if (f >= j->n - j->i) {
      sb_free(&b);
      jp_err(j, "unterminated string");
      return 1;
    }
    sb_addn(&b, j->p + j->i, f);
    j->i += f;
    char c = j->p[j->i];
    if (c == '"') {
      j->i++;
      break;
    }
    // c == '\\'
    j->i++;
    if (j->i >= j->n) {
      sb_free(&b);
      jp_err(j, "unterminated escape");
      return 1;
    }
    char e = j->p[j->i++];
    switch (e) {
      case '"': sb_addc(&b, '"'); break;
      case '\\': sb_addc(&b, '\\'); break;
      case '/': sb_addc(&b, '/'); break;
      case 'b': sb_addc(&b, '\b'); break;
      case 'f': sb_addc(&b, '\f'); break;
      case 'n': sb_addc(&b, '\n'); break;
      case 'r': sb_addc(&b, '\r'); break;
      case 't': sb_addc(&b, '\t'); break;
      case 'u': {
        uint32_t cp;
        if (j->i + 4 > j->n || !parse_hex4(j->p + j->i, &cp)) {
          sb_free(&b);
          jp_err(j, "bad \\u escape");
          return 1;
        }
        j->i += 4;
        if (cp >= 0xD800 && cp <= 0xDBFF) {
          if (j->i + 6 <= j->n && j->p[j->i] == '\\' && j->p[j->i + 1] == 'u') {
            uint32_t lo;
            if (parse_hex4(j->p + j->i + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              j->i += 6;
            } else
              cp = 0xFFFD;
          } else
            cp = 0xFFFD;
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
          cp = 0xFFFD;
        }
        char enc[4];
        size_t el = utf8_encode(cp, enc);
        sb_addn(&b, enc, el);
        break;
      }
      default:
        sb_free(&b);
        jp_err(j, "bad escape");
        return 1;
    }
  }
  char *s = arena_dup(j->a, b.buf, b.len);
  size_t n = b.len;
  sb_free(&b);
  *out = sl(s, n);
  return 0;
}

static Val *jp_number(JP *j) {
  size_t s = j->i;
  if (j->i < j->n && j->p[j->i] == '-') j->i++;
  while (j->i < j->n) {
    char c = j->p[j->i];
    if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') j->i++;
    else break;
  }
  // strtod needs a NUL-terminated copy; numbers here are short.
  char tmp[64];
  size_t len = j->i - s;
  if (len >= sizeof(tmp)) {
    jp_err(j, "number too long");
    return NULL;
  }
  memcpy(tmp, j->p + s, len);
  tmp[len] = '\0';
  return v_num(j->a, strtod(tmp, NULL));
}

static Val *jp_array(JP *j) {
  // j->p[j->i] == '['
  j->i++;
  size_t cap = 8, len = 0;
  Val **items = (Val **)arena_alloc(j->a, cap * sizeof(Val *));
  jp_ws(j);
  if (j->i < j->n && j->p[j->i] == ']') {
    j->i++;
    return v_arr(j->a, items, 0);
  }
  while (1) {
    jp_ws(j);
    Val *v = jp_value(j);
    if (!v) return NULL;
    if (len == cap) {
      size_t nc = cap * 2;
      Val **np = (Val **)arena_alloc(j->a, nc * sizeof(Val *));
      memcpy(np, items, len * sizeof(Val *));
      items = np;
      cap = nc;
    }
    items[len++] = v;
    jp_ws(j);
    if (j->i >= j->n) {
      jp_err(j, "unterminated array");
      return NULL;
    }
    if (j->p[j->i] == ',') {
      j->i++;
      continue;
    }
    if (j->p[j->i] == ']') {
      j->i++;
      return v_arr(j->a, items, len);
    }
    jp_err(j, "expected , or ] in array");
    return NULL;
  }
}

static Val *jp_object(JP *j) {
  // j->p[j->i] == '{'
  j->i++;
  Val *o = v_obj(j->a);
  jp_ws(j);
  if (j->i < j->n && j->p[j->i] == '}') {
    j->i++;
    return o;
  }
  while (1) {
    jp_ws(j);
    if (j->i >= j->n || j->p[j->i] != '"') {
      jp_err(j, "expected string key");
      return NULL;
    }
    Slice key;
    if (jp_string(j, &key)) return NULL;
    jp_ws(j);
    if (j->i >= j->n || j->p[j->i] != ':') {
      jp_err(j, "expected : in object");
      return NULL;
    }
    j->i++;
    jp_ws(j);
    Val *v = jp_value(j);
    if (!v) return NULL;
    v_obj_set(j->a, o, key, v);
    jp_ws(j);
    if (j->i >= j->n) {
      jp_err(j, "unterminated object");
      return NULL;
    }
    if (j->p[j->i] == ',') {
      j->i++;
      continue;
    }
    if (j->p[j->i] == '}') {
      j->i++;
      return o;
    }
    jp_err(j, "expected , or } in object");
    return NULL;
  }
}

static Val *jp_value(JP *j) {
  jp_ws(j);
  if (j->i >= j->n) {
    jp_err(j, "unexpected end");
    return NULL;
  }
  char c = j->p[j->i];
  if (c == '{') return jp_object(j);
  if (c == '[') return jp_array(j);
  if (c == '"') {
    Slice s;
    if (jp_string(j, &s)) return NULL;
    return v_str(j->a, s);
  }
  if (c == 't') {
    if (j->i + 4 <= j->n && memcmp(j->p + j->i, "true", 4) == 0) {
      j->i += 4;
      return v_bool(j->a, 1);
    }
    jp_err(j, "bad literal");
    return NULL;
  }
  if (c == 'f') {
    if (j->i + 5 <= j->n && memcmp(j->p + j->i, "false", 5) == 0) {
      j->i += 5;
      return v_bool(j->a, 0);
    }
    jp_err(j, "bad literal");
    return NULL;
  }
  if (c == 'n') {
    if (j->i + 4 <= j->n && memcmp(j->p + j->i, "null", 4) == 0) {
      j->i += 4;
      return v_null();
    }
    jp_err(j, "bad literal");
    return NULL;
  }
  if (c == '-' || (c >= '0' && c <= '9')) return jp_number(j);
  jp_err(j, "unexpected character");
  return NULL;
}

Val *json_parse(Arena *a, Slice text, StrBuf *err_out) {
  JP j;
  j.p = text.p;
  j.n = text.n;
  j.i = 0;
  j.a = a;
  j.err = err_out;
  Val *v = jp_value(&j);
  if (!v) return NULL;
  jp_ws(&j);
  if (j.i != j.n) {
    jp_err(&j, "trailing data");
    return NULL;
  }
  return v;
}

// ---------------- serializer ----------------
// Mask of bytes in a 16-byte chunk that need escaping in a JSON string:
// '"' (0x22), '\\' (0x5C), or any control byte < 0x20.
static inline int json_escape_mask(const char *p) {
  __m128i v = _mm_loadu_si128((const __m128i *)p);
  __m128i flipped = _mm_xor_si128(v, _mm_set1_epi8((char)0x80));
  __m128i ctrl = _mm_cmplt_epi8(flipped, _mm_set1_epi8((char)0xA0)); // byte < 0x20
  __m128i q = _mm_cmpeq_epi8(v, _mm_set1_epi8('"'));
  __m128i bs = _mm_cmpeq_epi8(v, _mm_set1_epi8('\\'));
  return _mm_movemask_epi8(_mm_or_si128(_mm_or_si128(ctrl, q), bs));
}

static void json_escape_byte(StrBuf *out, unsigned char c) {
  char buf[8];
  switch (c) {
    case '"': sb_addcstr(out, "\\\""); return;
    case '\\': sb_addcstr(out, "\\\\"); return;
    case '\b': sb_addcstr(out, "\\b"); return;
    case '\f': sb_addcstr(out, "\\f"); return;
    case '\n': sb_addcstr(out, "\\n"); return;
    case '\r': sb_addcstr(out, "\\r"); return;
    case '\t': sb_addcstr(out, "\\t"); return;
    default:
      // other control bytes: \u00XX (lowercase hex, as JSON.stringify does)
      snprintf(buf, sizeof(buf), "\\u%04x", c);
      sb_addcstr(out, buf);
      return;
  }
}

void json_quote_sb(StrBuf *out, Slice s) {
  sb_addc(out, '"');
  size_t i = 0;
  while (i < s.n) {
    size_t j = i;
    // SSE2 chunks
    while (j + 16 <= s.n) {
      int m = json_escape_mask(s.p + j);
      if (m) {
        j += (size_t)__builtin_ctz((unsigned)m);
        break;
      }
      j += 16;
    }
    if (j == i) {
      // no escape found in the chunk loop (or scalar tail below)
    }
    // scalar tail / remainder: find the next escape byte
    if (j + 16 > s.n || j == s.n) {
      size_t k = j;
      while (k < s.n) {
        unsigned char c = (unsigned char)s.p[k];
        if (c == '"' || c == '\\' || c < 0x20) break;
        k++;
      }
      j = k;
    }
    sb_addn(out, s.p + i, j - i);
    if (j >= s.n) break;
    json_escape_byte(out, (unsigned char)s.p[j]);
    i = j + 1;
  }
  sb_addc(out, '"');
}

void json_stringify_sb(StrBuf *out, Val *v) {
  if (!v) {
    sb_addcstr(out, "null");
    return;
  }
  switch (v->tag) {
    case V_UNDEF:
    case V_NULL:
      sb_addcstr(out, "null");
      return;
    case V_BOOL:
      sb_addcstr(out, v->b ? "true" : "false");
      return;
    case V_NUM:
      js_num_sb(out, v->num);
      return;
    case V_STR:
      json_quote_sb(out, v->s);
      return;
    case V_ARR: {
      sb_addc(out, '[');
      for (size_t i = 0; i < v->arr.len; i++) {
        if (i) sb_addc(out, ',');
        Val *it = v->arr.items[i];
        if (!it || it->tag == V_UNDEF) sb_addcstr(out, "null");
        else json_stringify_sb(out, it);
      }
      sb_addc(out, ']');
      return;
    }
    case V_OBJ: {
      sb_addc(out, '{');
      int first = 1;
      for (size_t i = 0; i < v->obj.len; i++) {
        Val *val = v->obj.kvs[i].val;
        if (!val || val->tag == V_UNDEF) continue; // JSON.stringify drops undefined props
        if (!first) sb_addc(out, ',');
        first = 0;
        json_quote_sb(out, v->obj.kvs[i].key);
        sb_addc(out, ':');
        json_stringify_sb(out, val);
      }
      sb_addc(out, '}');
      return;
    }
  }
}
