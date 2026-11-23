// nat_napi.c — N-API bridge. Exports:
//   run(inputObject) -> outputObject        (the solve.js fast path: JS values in/out)
//   buildJson(inputJsonString) -> outputJsonString
//   runAll(inputDir, outputDir) -> number of cases written
// All heavy work is C; the JS side is a thin driver.
#define NAPI_VERSION 8
#include <node_api.h>
#include "nat.h"

// ---------------- JS value -> Val ----------------
static Val *js_to_val(napi_env env, napi_value v, Arena *a);

static Val *js_to_val(napi_env env, napi_value v, Arena *a) {
  napi_valuetype t;
  if (napi_typeof(env, v, &t) != napi_ok) return v_undef();
  switch (t) {
    case napi_undefined:
      return v_undef();
    case napi_null:
      return v_null();
    case napi_boolean: {
      bool b = false;
      napi_get_value_bool(env, v, &b);
      return v_bool(a, b ? 1 : 0);
    }
    case napi_number: {
      double d = 0;
      napi_get_value_double(env, v, &d);
      return v_num(a, d);
    }
    case napi_string: {
      size_t len = 0;
      napi_get_value_string_utf8(env, v, NULL, 0, &len);
      char *buf = (char *)arena_alloc(a, len + 1);
      size_t copied = 0;
      napi_get_value_string_utf8(env, v, buf, len + 1, &copied);
      return v_str(a, sl(buf, copied));
    }
    case napi_object: {
      bool is_arr = false;
      napi_is_array(env, v, &is_arr);
      if (is_arr) {
        uint32_t len = 0;
        napi_get_array_length(env, v, &len);
        Val **items = (Val **)arena_alloc(a, (len ? len : 1) * sizeof(Val *));
        for (uint32_t i = 0; i < len; i++) {
          napi_value el;
          napi_get_element(env, v, i, &el);
          items[i] = js_to_val(env, el, a);
        }
        return v_arr(a, items, len);
      }
      // plain object: own enumerable string keys, in insertion order
      napi_value keys;
      napi_get_property_names(env, v, &keys);
      uint32_t n = 0;
      napi_get_array_length(env, keys, &n);
      Val *o = v_obj(a);
      for (uint32_t i = 0; i < n; i++) {
        napi_value k, val;
        napi_get_element(env, keys, i, &k);
        size_t klen = 0;
        napi_get_value_string_utf8(env, k, NULL, 0, &klen);
        char *kbuf = (char *)arena_alloc(a, klen + 1);
        size_t kcopied = 0;
        napi_get_value_string_utf8(env, k, kbuf, klen + 1, &kcopied);
        napi_get_property(env, v, k, &val);
        v_obj_set(a, o, sl(kbuf, kcopied), js_to_val(env, val, a));
      }
      return o;
    }
    default:
      // functions, symbols, bigints: not in the input; treat as undefined
      return v_undef();
  }
}

// ---------------- Val -> JS value ----------------
static napi_value val_to_js(napi_env env, Val *v, Arena *a) {
  napi_value out;
  if (!v || v->tag == V_UNDEF) {
    napi_get_undefined(env, &out);
    return out;
  }
  switch (v->tag) {
    case V_NULL:
      napi_get_null(env, &out);
      return out;
    case V_BOOL:
      napi_get_boolean(env, v->b, &out);
      return out;
    case V_NUM:
      napi_create_double(env, v->num, &out);
      return out;
    case V_STR:
      napi_create_string_utf8(env, v->s.p, v->s.n, &out);
      return out;
    case V_ARR: {
      napi_create_array_with_length(env, v->arr.len, &out);
      for (size_t i = 0; i < v->arr.len; i++) {
        napi_value el = val_to_js(env, v->arr.items[i], a);
        napi_set_element(env, out, (uint32_t)i, el);
      }
      return out;
    }
    case V_OBJ: {
      napi_create_object(env, &out);
      for (size_t i = 0; i < v->obj.len; i++) {
        napi_value key, val;
        napi_create_string_utf8(env, v->obj.kvs[i].key.p, v->obj.kvs[i].key.n, &key);
        val = val_to_js(env, v->obj.kvs[i].val, a);
        napi_set_property(env, out, key, val);
      }
      return out;
    }
  }
  napi_get_undefined(env, &out);
  return out;
}

static void throw_err(napi_env env, const char *msg) {
  napi_throw_error(env, NULL, msg);
}

// ---------------- run(inputObject) ----------------
static napi_value Run(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1];
  if (napi_get_cb_info(env, info, &argc, args, NULL, NULL) != napi_ok || argc < 1) {
    throw_err(env, "run(input) needs one argument");
    return NULL;
  }
  Arena a;
  arena_init(&a);
  Val *input = js_to_val(env, args[0], &a);
  StrBuf err;
  sb_init(&err);
  Val *output = build_site_val(&a, input, &err);
  napi_value result = NULL;
  if (!output) {
    char *msg = arena_dup(&a, err.buf, err.len);
    sb_free(&err);
    throw_err(env, msg);
    arena_free(&a);
    return NULL;
  }
  sb_free(&err);
  result = val_to_js(env, output, &a);
  arena_free(&a);
  return result;
}

// ---------------- buildJson(inputJsonString) ----------------
static napi_value BuildJson(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1];
  if (napi_get_cb_info(env, info, &argc, args, NULL, NULL) != napi_ok || argc < 1) {
    throw_err(env, "buildJson(json) needs one argument");
    return NULL;
  }
  size_t len = 0;
  napi_get_value_string_utf8(env, args[0], NULL, 0, &len);
  char *buf = (char *)malloc(len + 1);
  if (!buf) {
    throw_err(env, "out of memory");
    return NULL;
  }
  size_t copied = 0;
  napi_get_value_string_utf8(env, args[0], buf, len + 1, &copied);
  Arena a;
  arena_init(&a);
  StrBuf err;
  sb_init(&err);
  Val *input = json_parse(&a, sl(buf, copied), &err);
  napi_value result = NULL;
  if (!input) {
    char *msg = arena_dup(&a, err.buf, err.len);
    sb_free(&err);
    free(buf);
    throw_err(env, msg);
    arena_free(&a);
    return NULL;
  }
  sb_free(&err);
  Val *output = build_site_val(&a, input, &err);
  if (!output) {
    char *msg = arena_dup(&a, err.buf, err.len);
    sb_free(&err);
    free(buf);
    throw_err(env, msg);
    arena_free(&a);
    return NULL;
  }
  sb_free(&err);
  StrBuf out;
  sb_init(&out);
  json_stringify_sb(&out, output);
  napi_create_string_utf8(env, out.buf, out.len, &result);
  sb_free(&out);
  free(buf);
  arena_free(&a);
  return result;
}

// ---------------- runAll(inputDir, outputDir) ----------------
// Reads <inputDir>/cases.json ([{id, input}]), builds each case, writes
// <outputDir>/answers.json ([answer, ...]). Returns the case count.
static char *read_file(const char *path, size_t *n_out) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n < 0) {
    fclose(f);
    return NULL;
  }
  char *buf = (char *)malloc((size_t)n + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  size_t rd = fread(buf, 1, (size_t)n, f);
  fclose(f);
  buf[rd] = '\0';
  *n_out = rd;
  return buf;
}

static napi_value RunAll(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2];
  if (napi_get_cb_info(env, info, &argc, args, NULL, NULL) != napi_ok || argc < 2) {
    throw_err(env, "runAll(inputDir, outputDir) needs two arguments");
    return NULL;
  }
  char in_dir[4096], out_dir[4096];
  size_t l1 = 0, l2 = 0;
  napi_get_value_string_utf8(env, args[0], in_dir, sizeof(in_dir), &l1);
  napi_get_value_string_utf8(env, args[1], out_dir, sizeof(out_dir), &l2);
  in_dir[l1] = '\0';
  out_dir[l2] = '\0';
  char in_path[8192], out_path[8192];
  snprintf(in_path, sizeof(in_path), "%s/cases.json", in_dir);
  snprintf(out_path, sizeof(out_path), "%s/answers.json", out_dir);
  size_t cases_len = 0;
  char *cases_json = read_file(in_path, &cases_len);
  if (!cases_json) {
    throw_err(env, "cannot read cases.json");
    return NULL;
  }
  Arena a;
  arena_init(&a);
  StrBuf err;
  sb_init(&err);
  Val *cases = json_parse(&a, sl(cases_json, cases_len), &err);
  free(cases_json);
  if (!cases || cases->tag != V_ARR) {
    char *msg = arena_dup(&a, err.buf, err.len);
    sb_free(&err);
    throw_err(env, msg[0] ? msg : "cases.json is not an array");
    arena_free(&a);
    return NULL;
  }
  sb_free(&err);
  // answers array, built case by case; each case gets a fresh arena slice of
  // work, but the answers Val must outlive the per-case arena, so serialize
  // each answer immediately into one output StrBuf (streaming, no giant Val).
  StrBuf out;
  sb_init(&out);
  sb_addc(&out, '[');
  size_t ncases = cases->arr.len;
  for (size_t ci = 0; ci < ncases; ci++) {
    Val *c = cases->arr.items[ci];
    Val *input = v_obj_get(c, sl("input", 5));
    if (!input) {
      sb_free(&out);
      throw_err(env, "a case has no input");
      arena_free(&a);
      return NULL;
    }
    Arena ca;
    arena_init(&ca);
    StrBuf cerr;
    sb_init(&cerr);
    Val *answer = build_site_val(&ca, input, &cerr);
    if (!answer) {
      char *msg = arena_dup(&ca, cerr.buf, cerr.len);
      sb_free(&cerr);
      sb_free(&out);
      throw_err(env, msg);
      arena_free(&ca);
      arena_free(&a);
      return NULL;
    }
    sb_free(&cerr);
    if (ci) sb_addc(&out, ',');
    json_stringify_sb(&out, answer);
    arena_free(&ca);
  }
  sb_addc(&out, ']');
  FILE *f = fopen(out_path, "wb");
  if (!f) {
    sb_free(&out);
    throw_err(env, "cannot write answers.json");
    arena_free(&a);
    return NULL;
  }
  size_t written = fwrite(out.buf, 1, out.len, f);
  int closed = fclose(f);
  if (written != out.len || closed != 0) {
    sb_free(&out);
    throw_err(env, "failed writing answers.json");
    arena_free(&a);
    return NULL;
  }
  sb_free(&out);
  arena_free(&a);
  napi_value result;
  napi_create_uint32(env, (uint32_t)ncases, &result);
  return result;
}

// ---------------- module init ----------------
static napi_value Init(napi_env env, napi_value exports) {
  napi_value fn;
  napi_create_function(env, "run", NAPI_AUTO_LENGTH, Run, NULL, &fn);
  napi_set_named_property(env, exports, "run", fn);
  napi_create_function(env, "buildJson", NAPI_AUTO_LENGTH, BuildJson, NULL, &fn);
  napi_set_named_property(env, exports, "buildJson", fn);
  napi_create_function(env, "runAll", NAPI_AUTO_LENGTH, RunAll, NULL, &fn);
  napi_set_named_property(env, exports, "runAll", fn);
  return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
