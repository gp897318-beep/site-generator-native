// nat_val.c — the template/build value model (JS-like values) + JS number formatting
// and UTF-16 code-unit comparison.
#include "nat.h"

static Val UNDEF_VAL = {.tag = V_UNDEF};
static Val NULL_VAL = {.tag = V_NULL};
static Val ARR_LEN_SCRATCH = {.tag = V_NUM, .num = 0};

Val *v_undef(void) { return &UNDEF_VAL; }
Val *v_null(void) { return &NULL_VAL; }

Val *v_bool(Arena *a, int b) {
  Val *v = (Val *)arena_alloc(a, sizeof(Val));
  v->tag = V_BOOL;
  v->b = !!b;
  return v;
}

Val *v_num(Arena *a, double d) {
  Val *v = (Val *)arena_alloc(a, sizeof(Val));
  v->tag = V_NUM;
  v->num = d;
  return v;
}

Val *v_str(Arena *a, Slice s) {
  Val *v = (Val *)arena_alloc(a, sizeof(Val));
  v->tag = V_STR;
  v->s = s;
  return v;
}

Val *v_arr(Arena *a, Val **items, size_t len) {
  Val *v = (Val *)arena_alloc(a, sizeof(Val));
  v->tag = V_ARR;
  v->arr.items = items;
  v->arr.len = len;
  return v;
}

Val *v_obj(Arena *a) {
  Val *v = (Val *)arena_alloc(a, sizeof(Val));
  v->tag = V_OBJ;
  v->obj.kvs = NULL;
  v->obj.len = 0;
  v->obj.cap = 0;
  return v;
}

void v_obj_set(Arena *a, Val *o, Slice key, Val *v) {
  for (size_t i = 0; i < o->obj.len; i++)
    if (slice_eq(o->obj.kvs[i].key, key)) {
      o->obj.kvs[i].val = v;
      return;
    }
  if (o->obj.len == o->obj.cap) {
    size_t nc = o->obj.cap ? o->obj.cap * 2 : 8;
    KV *np = (KV *)arena_alloc(a, nc * sizeof(KV));
    if (o->obj.kvs) memcpy(np, o->obj.kvs, o->obj.len * sizeof(KV));
    o->obj.kvs = np;
    o->obj.cap = nc;
  }
  o->obj.kvs[o->obj.len].key = key;
  o->obj.kvs[o->obj.len].val = v;
  o->obj.len++;
}

Val *v_obj_get(Val *o, Slice key) {
  if (!o || o->tag != V_OBJ) return NULL;
  for (size_t i = 0; i < o->obj.len; i++)
    if (slice_eq(o->obj.kvs[i].key, key)) return o->obj.kvs[i].val;
  return NULL;
}

Val *v_lookup_arr_len(Val *arr, Slice key) {
  if (!arr || arr->tag != V_ARR) return NULL;
  if (!slice_eq_cstr(key, "length")) return NULL;
  // Scratch singleton: valid until the next v_lookup_arr_len call. Callers in
  // the renderer use the value immediately, never store it.
  ARR_LEN_SCRATCH.num = (double)arr->arr.len;
  return &ARR_LEN_SCRATCH;
}

void js_num_sb(StrBuf *out, double d) {
  if (d != d) {
    sb_addcstr(out, "NaN");
    return;
  }
  if (d == 1.0 / 0.0) {
    sb_addcstr(out, "Infinity");
    return;
  }
  if (d == -1.0 / 0.0) {
    sb_addcstr(out, "-Infinity");
    return;
  }
  if (d == 0) { // also -0: JS String(-0) === "0"
    sb_addc(out, '0');
    return;
  }
  // Integral and |d| < 1e21: plain decimal digits (JS prints no exponent below 1e21).
  if (d == trunc(d) && fabs(d) < 1e21) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%lld", (long long)d);
    sb_addn(out, buf, (size_t)n);
    return;
  }
  // Shortest round-trip via %.*g, then normalize the exponent to JS style
  // (strip the leading zero: "1.5e-07" -> "1.5e-7").
  char buf[64];
  for (int prec = 1; prec <= 17; prec++) {
    snprintf(buf, sizeof(buf), "%.*g", prec, d);
    if (strtod(buf, NULL) == d) break;
  }
  // Normalize exponent: e+07 -> e+7, e-07 -> e-7.
  char *e = strchr(buf, 'e');
  if (e && (e[1] == '+' || e[1] == '-') && e[2] == '0') {
    memmove(e + 2, e + 3, strlen(e + 3) + 1);
  }
  sb_addcstr(out, buf);
}

void text_of_sb(StrBuf *out, Val *v) {
  if (!v) return; // undefined
  switch (v->tag) {
    case V_UNDEF:
    case V_NULL:
    case V_OBJ:
      return;
    case V_BOOL:
      sb_addcstr(out, v->b ? "true" : "false");
      return;
    case V_NUM:
      js_num_sb(out, v->num);
      return;
    case V_STR:
      sb_add(out, v->s);
      return;
    case V_ARR:
      for (size_t i = 0; i < v->arr.len; i++) {
        if (i) sb_addcstr(out, ", ");
        text_of_sb(out, v->arr.items[i]);
      }
      return;
  }
}

int val_truthy(Val *v) {
  if (!v) return 0;
  switch (v->tag) {
    case V_UNDEF:
    case V_NULL:
      return 0;
    case V_BOOL:
      return v->b;
    case V_NUM:
      return v->num != 0;
    case V_STR:
      return v->s.n > 0;
    case V_ARR:
      return v->arr.len > 0;
    case V_OBJ:
      return 1;
  }
  return 0;
}

// Next UTF-16 code unit from a UTF-8 slice; *pend holds a pending low surrogate.
static int next_unit(Slice s, size_t *i, uint32_t *pend, uint16_t *out) {
  if (*pend) {
    *out = (uint16_t)*pend;
    *pend = 0;
    return 1;
  }
  if (*i >= s.n) return 0;
  size_t adv;
  uint32_t cp = utf8_decode(s.p + *i, s.n - *i, &adv);
  *i += adv;
  if (cp >= 0x10000) {
    *out = (uint16_t)(0xD800 + ((cp - 0x10000) >> 10));
    *pend = 0xDC00 + ((cp - 0x10000) & 0x3FF);
    return 1;
  }
  *out = (uint16_t)cp;
  return 1;
}

int cmp_code_units(Slice a, Slice b) {
  size_t ia = 0, ib = 0;
  uint32_t pa = 0, pb = 0;
  while (1) {
    uint16_t ua, ub;
    int ha = next_unit(a, &ia, &pa, &ua);
    int hb = next_unit(b, &ib, &pb, &ub);
    if (!ha && !hb) return 0;
    if (!ha) return -1;
    if (!hb) return 1;
    if (ua != ub) return ua < ub ? -1 : 1;
  }
}
