// nat_build.c — the full build pipeline (exact port of vendor index.js).
// build_site runs the 8 plugins in order over a BuildInput and returns the
// ordered output entries (path -> content). build_site_val is the Val-level
// wrapper used by the N-API bridge and the JSON entry points.
#include "nat.h"

#define REPORT_FILE "build-report.txt"

typedef struct Source Source;
typedef struct Tag Tag;

struct Source {
  Slice path;
  Slice text; // CRLF-normalized content
  FMData data;
  Slice body;
  int post;
  size_t idx; // position in the kept-sources list (stable-sort tiebreak)
  Slice layout;
  Slice slug, outDir, url;
  Slice html, excerpt;
  MdOut md; // toc
  Source *newer, *older;
  Tag **tagRefs;
  size_t ntagRefs;
  Val *card; // cached card (posts only, built in layouts)
  long words; // cached wordsOf(body), for reading_minutes
  int has_words;
};

struct Tag {
  Slice name, slug, url;
  Source **posts;
  size_t nposts, capposts;
};

typedef struct {
  Arena *a;
  Slice base; // stripped base_url
  Template *templates;
  size_t ntemplates;
  Source **sources;
  size_t nsources, capsources;
  struct {
    Slice path;
    Slice content;
  } *statics;
  size_t nstatics, capstatics;
  StrSet broken;
  Slice *report;
  size_t nreport, capreport;
  Source **posts;
  size_t nposts;
  Tag **tags;
  size_t ntags;
  OutEntry *out;
  size_t nout, capout;
  Source *cur_source; // source whose links are being resolved
} Build;

// ---------------- small helpers ----------------
static void report_push(Build *b, Slice line) {
  if (b->nreport == b->capreport) {
    size_t nc = b->capreport ? b->capreport * 2 : 32;
    Slice *np = (Slice *)arena_alloc(b->a, nc * sizeof(Slice));
    if (b->report) memcpy(np, b->report, b->nreport * sizeof(Slice));
    b->report = np;
    b->capreport = nc;
  }
  b->report[b->nreport++] = line;
}

static void report_pushf(Build *b, const char *fmt, Slice a, Slice c) {
  // fmt has two %s slots (a, c) — used for "refused %s: %s" style lines.
  StrBuf sb;
  sb_init(&sb);
  for (const char *p = fmt; *p; p++) {
    if (p[0] == '%' && p[1] == 's') {
      sb_add(&sb, a);
      p++;
      // second %s -> c (handled by swapping after first use)
      a = c;
    } else {
      sb_addc(&sb, *p);
    }
  }
  char *s = arena_dup(b->a, sb.buf, sb.len);
  size_t n = sb.len;
  sb_free(&sb);
  report_push(b, sl(s, n));
}

// Output assignment: JS object semantics — an existing key keeps its position,
// its value replaced.
static void out_set(Build *b, Slice path, Slice content) {
  for (size_t i = 0; i < b->nout; i++)
    if (slice_eq(b->out[i].path, path)) {
      b->out[i].content = content;
      return;
    }
  if (b->nout == b->capout) {
    size_t nc = b->capout ? b->capout * 2 : 64;
    OutEntry *np = (OutEntry *)arena_alloc(b->a, nc * sizeof(OutEntry));
    if (b->out) memcpy(np, b->out, b->nout * sizeof(OutEntry));
    b->out = np;
    b->capout = nc;
  }
  b->out[b->nout].path = path;
  b->out[b->nout].content = content;
  b->nout++;
}

static int out_has(Build *b, Slice path) {
  for (size_t i = 0; i < b->nout; i++)
    if (slice_eq(b->out[i].path, path)) return 1;
  return 0;
}

static Source *by_source(Build *b, Slice path) {
  for (size_t i = 0; i < b->nsources; i++)
    if (slice_eq(b->sources[i]->path, path)) return b->sources[i];
  return NULL;
}

static int static_has(Build *b, Slice path) {
  for (size_t i = 0; i < b->nstatics; i++)
    if (slice_eq(b->statics[i].path, path)) return 1;
  return 0;
}

static Template *find_template(Build *b, Slice name) {
  for (size_t i = 0; i < b->ntemplates; i++)
    if (slice_eq(b->templates[i].name, name) && b->templates[i].root) return &b->templates[i];
  return NULL;
}

// content.replace(/\r\n?/g, '\n')
static Slice normalize_crlf(Arena *a, Slice s) {
  size_t r = sse2_find_byte(s.p, s.n, (uint8_t)'\r');
  if (r >= s.n) return s; // fast path: no CR at all
  StrBuf out;
  sb_init(&out);
  size_t i = 0;
  while (i < s.n) {
    size_t f = sse2_find_byte(s.p + i, s.n - i, (uint8_t)'\r');
    if (f >= s.n - i) {
      sb_addn(&out, s.p + i, s.n - i);
      break;
    }
    sb_addn(&out, s.p + i, f);
    i += f;
    // \r\n -> \n ; lone \r -> \n
    if (i + 1 < s.n && s.p[i + 1] == '\n') i += 2;
    else i += 1;
    sb_addc(&out, '\n');
  }
  char *d = arena_dup(a, out.buf, out.len);
  size_t n = out.len;
  sb_free(&out);
  return sl(d, n);
}

// ---------------- link resolution (markdown env) ----------------
static int has_scheme(Slice t) {
  // /^[A-Za-z][A-Za-z0-9+.-]*:/
  if (t.n == 0) return 0;
  char c0 = t.p[0];
  if (!((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z'))) return 0;
  size_t i = 1;
  while (i < t.n) {
    char c = t.p[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '.' || c == '-')
      i++;
    else
      break;
  }
  return i < t.n && t.p[i] == ':';
}

static Slice build_link(void *ctx, Slice target) {
  Build *b = (Build *)ctx;
  Source *s = b->cur_source;
  Arena *a = b->a;
  if (has_scheme(target) || (target.n > 0 && (target.p[0] == '/' || target.p[0] == '#'))) return target;
  long hash = -1;
  {
    size_t f = sse2_find_byte(target.p, target.n, (uint8_t)'#');
    if (f < target.n) hash = (long)f;
  }
  Slice rel = hash == -1 ? target : sl(target.p, (size_t)hash);
  Slice frag = hash == -1 ? sl("", 0) : sl(target.p + hash, target.n - (size_t)hash);
  Slice dir = dir_of(s->path);
  int ok = 1;
  Slice path = sl("", 0);
  if (rel.n == 0) {
    ok = 0;
  } else {
    path = resolve_path(a, dir, rel, &ok);
  }
  if (ok) {
    Source *hit = by_source(b, path);
    if (hit) {
      StrBuf sb;
      sb_init(&sb);
      sb_add(&sb, hit->url);
      sb_add(&sb, frag);
      char *d = arena_dup(a, sb.buf, sb.len);
      size_t n = sb.len;
      sb_free(&sb);
      return sl(d, n);
    }
    if (static_has(b, path)) {
      StrBuf sb;
      sb_init(&sb);
      sb_add(&sb, b->base);
      sb_addc(&sb, '/');
      sb_add(&sb, path);
      sb_add(&sb, frag);
      char *d = arena_dup(a, sb.buf, sb.len);
      size_t n = sb.len;
      sb_free(&sb);
      return sl(d, n);
    }
  }
  // broken
  {
    StrBuf sb;
    sb_init(&sb);
    sb_addcstr(&sb, "broken-link ");
    sb_add(&sb, s->path);
    sb_addcstr(&sb, " -> ");
    sb_add(&sb, target);
    char *d = arena_dup(a, sb.buf, sb.len);
    size_t n = sb.len;
    sb_free(&sb);
    Slice line = sl(d, n);
    if (!strset_has(&b->broken, line)) strset_add(&b->broken, line);
  }
  return target;
}

// ---------------- card / page / tag values ----------------
static Slice long_date_val(Arena *a, Slice date) {
  StrBuf sb;
  sb_init(&sb);
  long_date_sb(&sb, date);
  char *d = arena_dup(a, sb.buf, sb.len);
  size_t n = sb.len;
  sb_free(&sb);
  return sl(d, n);
}

static Val *link_of(Arena *a, Source *s) {
  if (!s) return v_null();
  Val *o = v_obj(a);
  v_obj_set(a, o, sl("title", 5), v_str(a, s->data.title));
  v_obj_set(a, o, sl("url", 3), v_str(a, s->url));
  v_obj_set(a, o, sl("date", 4), v_str(a, s->data.has_date ? s->data.date : sl("", 0)));
  v_obj_set(a, o, sl("date_long", 9),
            v_str(a, s->data.has_date ? long_date_val(a, s->data.date) : sl("", 0)));
  return o;
}

static Val *card_of(Build *b, Source *s) {
  Arena *a = b->a;
  Val *o = v_obj(a);
  v_obj_set(a, o, sl("title", 5), v_str(a, s->data.title));
  v_obj_set(a, o, sl("url", 3), v_str(a, s->url));
  v_obj_set(a, o, sl("date", 4), v_str(a, s->data.has_date ? s->data.date : sl("", 0)));
  v_obj_set(a, o, sl("date_long", 9),
            v_str(a, s->data.has_date ? long_date_val(a, s->data.date) : sl("", 0)));
  v_obj_set(a, o, sl("summary", 7), v_str(a, s->data.has_summary ? s->data.summary : sl("", 0)));
  v_obj_set(a, o, sl("excerpt", 7), v_str(a, s->excerpt));
  if (!s->has_words) {
    s->words = (long)words_of(s->body.p, s->body.n);
    s->has_words = 1;
  }
  long mins = (s->words + 199) / 200;
  if (mins < 1) mins = 1;
  v_obj_set(a, o, sl("reading_minutes", 15), v_num(a, (double)mins));
  Val **tags = (Val **)arena_alloc(a, (s->ntagRefs ? s->ntagRefs : 1) * sizeof(Val *));
  for (size_t i = 0; i < s->ntagRefs; i++) {
    Tag *t = s->tagRefs[i];
    Val *tv = v_obj(a);
    v_obj_set(a, tv, sl("name", 4), v_str(a, t->name));
    v_obj_set(a, tv, sl("slug", 4), v_str(a, t->slug));
    v_obj_set(a, tv, sl("url", 3), v_str(a, t->url));
    tags[i] = tv;
  }
  v_obj_set(a, o, sl("tags", 4), v_arr(a, tags, s->ntagRefs));
  return o;
}

static Val *page_of(Build *b, Source *s) {
  Arena *a = b->a;
  // {...cardOf(s), ...}: for a post the cached card is the same value.
  Val *card = (s->post && s->card) ? s->card : card_of(b, s);
  // {...card, slug, content, toc, newer, older, is_post}
  Val *o = v_obj(a);
  for (size_t i = 0; i < card->obj.len; i++) v_obj_set(a, o, card->obj.kvs[i].key, card->obj.kvs[i].val);
  v_obj_set(a, o, sl("slug", 4), v_str(a, s->slug));
  v_obj_set(a, o, sl("content", 7), v_str(a, s->html));
  Val **toc = (Val **)arena_alloc(a, (s->md.ntoc ? s->md.ntoc : 1) * sizeof(Val *));
  for (size_t i = 0; i < s->md.ntoc; i++) {
    Val *h = v_obj(a);
    v_obj_set(a, h, sl("level", 5), v_num(a, (double)s->md.toc[i].level));
    v_obj_set(a, h, sl("id", 2), v_str(a, s->md.toc[i].id));
    v_obj_set(a, h, sl("html", 4), v_str(a, s->md.toc[i].html));
    toc[i] = h;
  }
  v_obj_set(a, o, sl("toc", 3), v_arr(a, toc, s->md.ntoc));
  v_obj_set(a, o, sl("newer", 5), link_of(a, s->newer));
  v_obj_set(a, o, sl("older", 5), link_of(a, s->older));
  v_obj_set(a, o, sl("is_post", 7), v_bool(a, s->post));
  return o;
}

static Val *tag_of(Build *b, Tag *t) {
  Arena *a = b->a;
  Val *o = v_obj(a);
  v_obj_set(a, o, sl("name", 4), v_str(a, t->name));
  v_obj_set(a, o, sl("slug", 4), v_str(a, t->slug));
  v_obj_set(a, o, sl("url", 3), v_str(a, t->url));
  v_obj_set(a, o, sl("count", 5), v_num(a, (double)t->nposts));
  Val **posts = (Val **)arena_alloc(a, (t->nposts ? t->nposts : 1) * sizeof(Val *));
  for (size_t i = 0; i < t->nposts; i++) posts[i] = t->posts[i]->card;
  v_obj_set(a, o, sl("posts", 5), v_arr(a, posts, t->nposts));
  return o;
}

// ---------------- template rendering ----------------
typedef struct {
  Build *b;
  Val *site_val;
  Val *all_tags; // array of {name, slug, url, count}
  Val *recent_arr; // array of card Vals
} RCtx;

static Scope *scope_from_obj(Arena *a, Val *obj, Scope *parent) {
  Scope *s = (Scope *)arena_alloc(a, sizeof(Scope));
  s->n = obj->obj.len;
  s->names = (Slice *)arena_alloc(a, (s->n ? s->n : 1) * sizeof(Slice));
  s->vals = (Val **)arena_alloc(a, (s->n ? s->n : 1) * sizeof(Val *));
  for (size_t i = 0; i < s->n; i++) {
    s->names[i] = obj->obj.kvs[i].key;
    s->vals[i] = obj->obj.kvs[i].val;
  }
  s->parent = parent;
  return s;
}

// Render template `name` with extra vars (an object Val, or NULL for {}).
// Returns the rendered slice, or NULL when the template is absent/failed.
static Slice render_tpl(RCtx *rc, Slice name, Val *extra, int *is_null) {
  Build *b = rc->b;
  Arena *a = b->a;
  *is_null = 0;
  Template *t = find_template(b, name);
  if (!t) {
    *is_null = 1;
    return sl("", 0);
  }
  Val *vars = v_obj(a);
  v_obj_set(a, vars, sl("site", 4), rc->site_val);
  v_obj_set(a, vars, sl("tags", 4), rc->all_tags);
  v_obj_set(a, vars, sl("recent", 6), rc->recent_arr);
  if (extra)
    for (size_t i = 0; i < extra->obj.len; i++) v_obj_set(a, vars, extra->obj.kvs[i].key, extra->obj.kvs[i].val);
  Scope *scope = scope_from_obj(a, vars, NULL);
  StrBuf out;
  sb_init(&out);
  render_nodes(&out, t->root, scope, b->templates, b->ntemplates, 0, a);
  char *d = arena_dup(a, out.buf, out.len);
  size_t n = out.len;
  sb_free(&out);
  return sl(d, n);
}

// ---------------- comparators (qsort) ----------------
typedef struct {
  Slice path, content;
} FileEnt;

static int cmp_file_path(const void *pa, const void *pb) {
  const FileEnt *a = (const FileEnt *)pa, *b = (const FileEnt *)pb;
  return cmp_code_units(a->path, b->path);
}

static int cmp_slice_qsort(const void *pa, const void *pb) {
  const Slice *a = (const Slice *)pa, *b = (const Slice *)pb;
  return cmp_code_units(*a, *b);
}

static int cmp_post_qsort(const void *pa, const void *pb) {
  Source *const *a = (Source *const *)pa, *const *b = (Source *const *)pb;
  int c = cmp_code_units((*b)->data.date, (*a)->data.date); // date desc
  if (c) return c;
  c = cmp_code_units((*a)->slug, (*b)->slug); // slug asc
  if (c) return c;
  return (*a)->idx < (*b)->idx ? -1 : (*a)->idx > (*b)->idx ? 1 : 0;
}

static int cmp_tag_qsort(const void *pa, const void *pb) {
  Tag *const *a = (Tag *const *)pa, *const *b = (Tag *const *)pb;
  if ((*a)->nposts != (*b)->nposts) return (*a)->nposts > (*b)->nposts ? -1 : 1; // count desc
  return cmp_code_units((*a)->slug, (*b)->slug);
}

static int cmp_template_name(const void *pa, const void *pb) {
  const Template *a = (const Template *)pa, *b = (const Template *)pb;
  return cmp_code_units(a->name, b->name);
}

// ---------------- tags ----------------
typedef struct {
  Slice name, slug;
} NameSlug;

// A source's tags, each slug once, in its own order.
static NameSlug *own_tags(Arena *a, Source *s, size_t *nout) {
  if (!s->data.has_tags || s->data.ntags == 0) {
    *nout = 0;
    return NULL;
  }
  NameSlug *out = (NameSlug *)arena_alloc(a, s->data.ntags * sizeof(NameSlug));
  StrSet seen;
  strset_init(&seen, a);
  size_t n = 0;
  for (size_t i = 0; i < s->data.ntags; i++) {
    Slice name = s->data.tags[i];
    Slice slug = slugify(a, name);
    if (strset_has(&seen, slug)) continue;
    strset_add(&seen, slug);
    out[n].name = name;
    out[n].slug = slug;
    n++;
  }
  *nout = n;
  return out;
}

static void tag_add_post(Arena *a, Tag *t, Source *p) {
  if (t->nposts == t->capposts) {
    size_t nc = t->capposts ? t->capposts * 2 : 8;
    Source **np = (Source **)arena_alloc(a, nc * sizeof(Source *));
    if (t->posts) memcpy(np, t->posts, t->nposts * sizeof(Source *));
    t->posts = np;
    t->capposts = nc;
  }
  t->posts[t->nposts++] = p;
}

static Tag *find_tag_by_slug(Tag **tags, size_t ntags, Slice slug) {
  for (size_t i = 0; i < ntags; i++)
    if (slice_eq(tags[i]->slug, slug)) return tags[i];
  return NULL;
}

// ---------------- the pipeline ----------------
int build_site(Arena *a, BuildInput *in, OutEntry **outs_out, size_t *nouts_out, StrBuf *err_out) {
  (void)err_out;
  Build b;
  memset(&b, 0, sizeof(b));
  b.a = a;
  b.templates = in->templates;
  b.ntemplates = in->ntemplates;
  strset_init(&b.broken, a);

  // base = base_url with trailing slashes stripped
  Slice base = in->base_url;
  while (base.n > 0 && base.p[base.n - 1] == '/') base.n--;
  b.base = base;

  // 0. Parse templates (sorted by name); failures are reported, template absent.
  for (size_t i = 0; i < in->ntemplates; i++) {
    Template *t = &in->templates[i];
    StrBuf e;
    sb_init(&e);
    Node *root = NULL;
    if (parse_template(a, t->name, t->source, &root, &e)) {
      t->failed = 1;
      t->root = NULL;
      t->err = e;
      char *d = arena_dup(a, e.buf, e.len);
      report_push(&b, sl(d, e.len));
      sb_free(&e);
    } else {
      t->root = root;
      sb_free(&e);
    }
  }

  // 1. readSources: sort by path, split .md vs static, normalize line ends.
  size_t nfiles = in->nfiles;
  FileEnt *files = (FileEnt *)arena_alloc(a, (nfiles ? nfiles : 1) * sizeof(FileEnt));
  for (size_t i = 0; i < nfiles; i++) {
    files[i].path = in->file_paths[i];
    files[i].content = in->file_contents[i];
  }
  qsort(files, nfiles, sizeof(FileEnt), cmp_file_path);
  Source **all = (Source **)arena_alloc(a, (nfiles ? nfiles : 1) * sizeof(Source *));
  size_t nall = 0;
  for (size_t i = 0; i < nfiles; i++) {
    if (slice_ends(files[i].path, sl(".md", 3))) {
      Source *s = (Source *)arena_alloc0(a, sizeof(Source));
      s->path = files[i].path;
      s->text = normalize_crlf(a, files[i].content);
      all[nall++] = s;
    } else {
      if (b.nstatics == b.capstatics) {
        size_t nc = b.capstatics ? b.capstatics * 2 : 32;
        void *np = arena_alloc(a, nc * sizeof(*b.statics));
        if (b.statics) memcpy(np, b.statics, b.nstatics * sizeof(*b.statics));
        b.statics = (void *)np;
        b.capstatics = nc;
      }
      b.statics[b.nstatics].path = files[i].path;
      b.statics[b.nstatics].content = files[i].content;
      b.nstatics++;
    }
  }

  // 2. frontMatter
  Source **kept = (Source **)arena_alloc(a, (nall ? nall : 1) * sizeof(Source *));
  size_t nkept = 0;
  for (size_t i = 0; i < nall; i++) {
    Source *s = all[i];
    StrBuf e;
    sb_init(&e);
    FMData data;
    Slice body;
    if (parse_front_matter(s->text.p, s->text.n, a, &data, &body, &e)) {
      StrBuf r;
      sb_init(&r);
      sb_addcstr(&r, "refused ");
      sb_add(&r, s->path);
      sb_addcstr(&r, ": ");
      sb_addn(&r, e.buf, e.len);
      char *d = arena_dup(a, r.buf, r.len);
      report_push(&b, sl(d, r.len));
      sb_free(&r);
      sb_free(&e);
      continue;
    }
    sb_free(&e);
    s->data = data;
    s->body = body;
    s->post = slice_starts(s->path, sl("posts/", 6));
    Slice own = sl("", 0);
    if (slice_starts(s->path, sl("tags/", 5))) own = sl("tags", 4);
    else if (slice_starts(s->path, sl("page/", 5))) own = sl("page", 4);
    if (own.n > 0) {
      StrBuf r;
      sb_init(&r);
      sb_addcstr(&r, "refused ");
      sb_add(&r, s->path);
      sb_addcstr(&r, ": ");
      sb_add(&r, own);
      sb_addcstr(&r, "/ is the generator's own");
      char *d = arena_dup(a, r.buf, r.len);
      report_push(&b, sl(d, r.len));
      sb_free(&r);
      continue;
    }
    if (s->post && !data.has_date) {
      StrBuf r;
      sb_init(&r);
      sb_addcstr(&r, "refused ");
      sb_add(&r, s->path);
      sb_addcstr(&r, ": a post needs a date");
      char *d = arena_dup(a, r.buf, r.len);
      report_push(&b, sl(d, r.len));
      sb_free(&r);
      continue;
    }
    Slice layout = data.has_layout ? data.layout : (s->post ? sl("post", 4) : sl("page", 4));
    if (!find_template(&b, layout)) {
      StrBuf r;
      sb_init(&r);
      sb_addcstr(&r, "refused ");
      sb_add(&r, s->path);
      sb_addcstr(&r, ": layout \"");
      sb_add(&r, layout);
      sb_addcstr(&r, "\" is no template that parses");
      char *d = arena_dup(a, r.buf, r.len);
      report_push(&b, sl(d, r.len));
      sb_free(&r);
      continue;
    }
    s->layout = layout;
    if (data.has_draft && data.draft) {
      StrBuf r;
      sb_init(&r);
      sb_addcstr(&r, "draft ");
      sb_add(&r, s->path);
      char *d = arena_dup(a, r.buf, r.len);
      report_push(&b, sl(d, r.len));
      sb_free(&r);
      continue;
    }
    s->idx = nkept;
    kept[nkept++] = s;
  }
  b.sources = kept;
  b.nsources = nkept;

  // 3. permalinks
  typedef struct {
    Slice dir;
    StrSet set;
  } TakenDir;
  TakenDir *taken = (TakenDir *)arena_alloc(a, (nkept ? nkept : 1) * sizeof(TakenDir));
  size_t ntaken = 0;
  for (size_t i = 0; i < nkept; i++) {
    Source *s = kept[i];
    Slice dir = s->post ? sl("posts", 5) : dir_of(s->path);
    TakenDir *td = NULL;
    for (size_t k = 0; k < ntaken; k++)
      if (slice_eq(taken[k].dir, dir)) {
        td = &taken[k];
        break;
      }
    if (!td) {
      td = &taken[ntaken++];
      td->dir = dir;
      strset_init(&td->set, a);
      if (dir.n == 0) {
        strset_add(&td->set, sl("posts", 5));
        strset_add(&td->set, sl("tags", 4));
        strset_add(&td->set, sl("page", 4));
      }
    }
    Slice slug = s->data.has_slug ? s->data.slug : slugify(a, s->data.title);
    slug = unique_slug(a, slug, &td->set);
    s->slug = slug;
    StrBuf ob;
    sb_init(&ob);
    if (dir.n > 0) {
      sb_add(&ob, dir);
      sb_addc(&ob, '/');
    }
    sb_add(&ob, slug);
    char *od = arena_dup(a, ob.buf, ob.len);
    s->outDir = sl(od, ob.len);
    sb_free(&ob);
    StrBuf ub;
    sb_init(&ub);
    sb_add(&ub, b.base);
    sb_addc(&ub, '/');
    sb_add(&ub, s->outDir);
    sb_addc(&ub, '/');
    char *ud = arena_dup(a, ub.buf, ub.len);
    s->url = sl(ud, ub.len);
    sb_free(&ub);
  }

  // 4. markdown
  for (size_t i = 0; i < nkept; i++) {
    Source *s = kept[i];
    b.cur_source = s;
    MdEnv env;
    env.ctx = &b;
    env.link = build_link;
    render_markdown(a, s->body, &env, &s->md);
    s->html = s->md.html;
    s->excerpt = s->md.excerpt;
  }
  b.cur_source = NULL;

  // 5. collections
  Source **posts = (Source **)arena_alloc(a, (nkept ? nkept : 1) * sizeof(Source *));
  size_t nposts = 0;
  for (size_t i = 0; i < nkept; i++)
    if (kept[i]->post) posts[nposts++] = kept[i];
  qsort(posts, nposts, sizeof(Source *), cmp_post_qsort);
  b.posts = posts;
  b.nposts = nposts;
  for (size_t k = 0; k < nposts; k++) {
    posts[k]->newer = k == 0 ? NULL : posts[k - 1];
    posts[k]->older = k == nposts - 1 ? NULL : posts[k + 1];
  }

  // 6. tags
  Tag **bySlug = (Tag **)arena_alloc(a, (nposts ? nposts : 1) * sizeof(Tag *));
  size_t nbySlug = 0;
  for (size_t pi = 0; pi < nposts; pi++) {
    Source *p = posts[pi];
    size_t nt;
    NameSlug *own = own_tags(a, p, &nt);
    Tag **refs = (Tag **)arena_alloc(a, (nt ? nt : 1) * sizeof(Tag *));
    for (size_t k = 0; k < nt; k++) {
      Tag *t = find_tag_by_slug(bySlug, nbySlug, own[k].slug);
      if (!t) {
        t = (Tag *)arena_alloc0(a, sizeof(Tag));
        t->name = own[k].name;
        t->slug = own[k].slug;
        StrBuf ub;
        sb_init(&ub);
        sb_add(&ub, b.base);
        sb_addcstr(&ub, "/tags/");
        sb_add(&ub, own[k].slug);
        sb_addc(&ub, '/');
        char *ud = arena_dup(a, ub.buf, ub.len);
        t->url = sl(ud, ub.len);
        sb_free(&ub);
        bySlug[nbySlug++] = t;
      }
      tag_add_post(a, t, p);
      refs[k] = t;
    }
    p->tagRefs = refs;
    p->ntagRefs = nt;
  }
  for (size_t si = 0; si < nkept; si++) {
    Source *s = kept[si];
    if (s->post) continue;
    size_t nt;
    NameSlug *own = own_tags(a, s, &nt);
    Tag **refs = (Tag **)arena_alloc(a, (nt ? nt : 1) * sizeof(Tag *));
    for (size_t k = 0; k < nt; k++) {
      Tag *t = find_tag_by_slug(bySlug, nbySlug, own[k].slug);
      if (!t) {
        t = (Tag *)arena_alloc0(a, sizeof(Tag));
        t->name = own[k].name;
        t->slug = own[k].slug;
        t->url = sl("", 0);
      }
      refs[k] = t;
    }
    s->tagRefs = refs;
    s->ntagRefs = nt;
  }
  qsort(bySlug, nbySlug, sizeof(Tag *), cmp_tag_qsort);
  b.tags = bySlug;
  b.ntags = nbySlug;

  // 7. layouts
  for (size_t k = 0; k < nposts; k++) posts[k]->card = card_of(&b, posts[k]);
  Val *site_val = v_obj(a);
  v_obj_set(a, site_val, sl("title", 5), v_str(a, in->title));
  v_obj_set(a, site_val, sl("base_url", 8), v_str(a, b.base));
  Val **allTagVals = (Val **)arena_alloc(a, (nbySlug ? nbySlug : 1) * sizeof(Val *));
  for (size_t k = 0; k < nbySlug; k++) {
    Tag *t = bySlug[k];
    Val *tv = v_obj(a);
    v_obj_set(a, tv, sl("name", 4), v_str(a, t->name));
    v_obj_set(a, tv, sl("slug", 4), v_str(a, t->slug));
    v_obj_set(a, tv, sl("url", 3), v_str(a, t->url));
    v_obj_set(a, tv, sl("count", 5), v_num(a, (double)t->nposts));
    allTagVals[k] = tv;
  }
  Val *all_tags = v_arr(a, allTagVals, nbySlug);
  long recent_n = in->recent;
  if (recent_n < 0) recent_n = 0;
  if (recent_n > (long)nposts) recent_n = (long)nposts;
  Val **recentVals = (Val **)arena_alloc(a, ((size_t)recent_n ? (size_t)recent_n : 1) * sizeof(Val *));
  for (long k = 0; k < recent_n; k++) recentVals[k] = posts[k]->card;
  Val *recent_arr = v_arr(a, recentVals, (size_t)recent_n);
  RCtx rc;
  rc.b = &b;
  rc.site_val = site_val;
  rc.all_tags = all_tags;
  rc.recent_arr = recent_arr;

  for (size_t i = 0; i < nkept; i++) {
    Source *s = kept[i];
    Val *extra = v_obj(a);
    v_obj_set(a, extra, sl("page", 4), page_of(&b, s));
    int is_null;
    Slice html = render_tpl(&rc, s->layout, extra, &is_null);
    if (is_null) continue;
    StrBuf pb;
    sb_init(&pb);
    sb_add(&pb, s->outDir);
    sb_addcstr(&pb, "/index.html");
    char *pd = arena_dup(a, pb.buf, pb.len);
    out_set(&b, sl(pd, pb.len), html);
    sb_free(&pb);
  }
  long per = in->per_page;
  if (per < 1) per = 1;
  long npages = (long)((nposts + (size_t)per - 1) / (size_t)per);
  if (npages < 1) npages = 1;
  for (long k = 1; k <= npages; k++) {
    size_t start = (size_t)(k - 1) * (size_t)per;
    size_t end = start + (size_t)per;
    if (end > nposts) end = nposts;
    Val **plist = (Val **)arena_alloc(a, ((end > start ? end - start : 0) ? (end - start) : 1) * sizeof(Val *));
    for (size_t i = start; i < end; i++) plist[i - start] = posts[i]->card;
    Val *pager = v_obj(a);
    v_obj_set(a, pager, sl("number", 6), v_num(a, (double)k));
    v_obj_set(a, pager, sl("total", 5), v_num(a, (double)npages));
    // indexUrl(k) = k === 1 ? base + '/' : base + '/page/' + k + '/'
    Slice url_k;
    {
      StrBuf ub;
      sb_init(&ub);
      sb_add(&ub, b.base);
      if (k == 1) {
        sb_addc(&ub, '/');
      } else {
        sb_addcstr(&ub, "/page/");
        char num[32];
        int nn = snprintf(num, sizeof(num), "%ld", k);
        sb_addn(&ub, num, (size_t)nn);
        sb_addc(&ub, '/');
      }
      url_k = sl(arena_dup(a, ub.buf, ub.len), ub.len);
      sb_free(&ub);
    }
    v_obj_set(a, pager, sl("url", 3), v_str(a, url_k));
    Slice newer_url = sl("", 0), older_url = sl("", 0);
    if (k != 1) {
      long kk = k - 1;
      StrBuf nb;
      sb_init(&nb);
      sb_add(&nb, b.base);
      if (kk == 1) {
        sb_addc(&nb, '/');
      } else {
        sb_addcstr(&nb, "/page/");
        char num[32];
        int nn = snprintf(num, sizeof(num), "%ld", kk);
        sb_addn(&nb, num, (size_t)nn);
        sb_addc(&nb, '/');
      }
      newer_url = sl(arena_dup(a, nb.buf, nb.len), nb.len);
      sb_free(&nb);
    }
    if (k != npages) {
      long kk = k + 1;
      StrBuf ob2;
      sb_init(&ob2);
      sb_add(&ob2, b.base);
      sb_addcstr(&ob2, "/page/");
      char num[32];
      int nn = snprintf(num, sizeof(num), "%ld", kk);
      sb_addn(&ob2, num, (size_t)nn);
      sb_addc(&ob2, '/');
      older_url = sl(arena_dup(a, ob2.buf, ob2.len), ob2.len);
      sb_free(&ob2);
    }
    v_obj_set(a, pager, sl("newer_url", 9), v_str(a, newer_url));
    v_obj_set(a, pager, sl("older_url", 9), v_str(a, older_url));
    Val *extra = v_obj(a);
    v_obj_set(a, extra, sl("posts", 5), v_arr(a, plist, end - start));
    v_obj_set(a, extra, sl("pager", 5), pager);
    int is_null;
    Slice html = render_tpl(&rc, sl("index", 5), extra, &is_null);
    if (is_null) continue;
    if (k == 1) {
      out_set(&b, sl("index.html", 10), html);
    } else {
      StrBuf pb;
      sb_init(&pb);
      sb_addcstr(&pb, "page/");
      char num[32];
      int nn = snprintf(num, sizeof(num), "%ld", k);
      sb_addn(&pb, num, (size_t)nn);
      sb_addcstr(&pb, "/index.html");
      char *pd = arena_dup(a, pb.buf, pb.len);
      out_set(&b, sl(pd, pb.len), html);
      sb_free(&pb);
    }
  }
  for (size_t ti = 0; ti < nbySlug; ti++) {
    Tag *t = bySlug[ti];
    Val *extra = v_obj(a);
    v_obj_set(a, extra, sl("tag", 3), tag_of(&b, t));
    int is_null;
    Slice html = render_tpl(&rc, sl("tag", 3), extra, &is_null);
    if (is_null) continue;
    StrBuf pb;
    sb_init(&pb);
    sb_addcstr(&pb, "tags/");
    sb_add(&pb, t->slug);
    sb_addcstr(&pb, "/index.html");
    char *pd = arena_dup(a, pb.buf, pb.len);
    out_set(&b, sl(pd, pb.len), html);
    sb_free(&pb);
  }
  {
    int is_null;
    Slice html = render_tpl(&rc, sl("tags", 4), NULL, &is_null);
    if (!is_null) out_set(&b, sl("tags/index.html", 15), html);
  }
  for (size_t i = 0; i < b.nstatics; i++) {
    if (out_has(&b, b.statics[i].path)) {
      StrBuf r;
      sb_init(&r);
      sb_addcstr(&r, "shadowed ");
      sb_add(&r, b.statics[i].path);
      sb_addcstr(&r, ": a built page has its path");
      char *d = arena_dup(a, r.buf, r.len);
      report_push(&b, sl(d, r.len));
      sb_free(&r);
    } else {
      out_set(&b, b.statics[i].path, b.statics[i].content);
    }
  }

  // 8. report
  size_t nlines = b.nreport + b.broken.len;
  Slice *lines = (Slice *)arena_alloc(a, (nlines + 1) * sizeof(Slice));
  size_t li = 0;
  for (size_t i = 0; i < b.nreport; i++) lines[li++] = b.report[i];
  for (size_t i = 0; i < b.broken.len; i++) lines[li++] = b.broken.items[i];
  qsort(lines, nlines, sizeof(Slice), cmp_slice_qsort);
  size_t npages_count = 0;
  for (size_t i = 0; i < nkept; i++)
    if (!kept[i]->post) npages_count++;
  StrBuf lb;
  sb_init(&lb);
  sb_addcstr(&lb, "built ");
  {
    char num[32];
    int nn = snprintf(num, sizeof(num), "%zu", npages_count);
    sb_addn(&lb, num, (size_t)nn);
  }
  sb_addcstr(&lb, " pages, ");
  {
    char num[32];
    int nn = snprintf(num, sizeof(num), "%zu", nposts);
    sb_addn(&lb, num, (size_t)nn);
  }
  sb_addcstr(&lb, " posts, ");
  {
    char num[32];
    int nn = snprintf(num, sizeof(num), "%zu", nbySlug);
    sb_addn(&lb, num, (size_t)nn);
  }
  sb_addcstr(&lb, " tags and ");
  {
    char num[32];
    int nn = snprintf(num, sizeof(num), "%zu", b.nstatics);
    sb_addn(&lb, num, (size_t)nn);
  }
  sb_addcstr(&lb, " static files");
  char *ld = arena_dup(a, lb.buf, lb.len);
  lines[li++] = sl(ld, lb.len);
  sb_free(&lb);
  StrBuf rb;
  sb_init(&rb);
  for (size_t i = 0; i < li; i++) {
    sb_add(&rb, lines[i]);
    sb_addc(&rb, '\n');
  }
  char *rd = arena_dup(a, rb.buf, rb.len);
  out_set(&b, sl(REPORT_FILE, 16), sl(rd, rb.len));
  sb_free(&rb);

  *outs_out = b.out;
  *nouts_out = b.nout;
  return 0;
}

// ---------------- Val-level wrapper ----------------
static void build_input_from_val(Arena *a, Val *input, BuildInput *out) {
  memset(out, 0, sizeof(*out));
  Val *site = v_obj_get(input, sl("site", 4));
  Val *site_title = site ? v_obj_get(site, sl("title", 5)) : NULL;
  Val *site_base = site ? v_obj_get(site, sl("base_url", 8)) : NULL;
  Val *site_per = site ? v_obj_get(site, sl("per_page", 8)) : NULL;
  Val *site_recent = site ? v_obj_get(site, sl("recent", 6)) : NULL;
  out->title = (site_title && site_title->tag == V_STR) ? site_title->s : sl("", 0);
  out->base_url = (site_base && site_base->tag == V_STR) ? site_base->s : sl("", 0);
  out->per_page = (site_per && site_per->tag == V_NUM) ? (long)site_per->num : 1;
  out->recent = (site_recent && site_recent->tag == V_NUM) ? (long)site_recent->num : 0;
  Val *tpls = v_obj_get(input, sl("templates", 9));
  size_t nt = (tpls && tpls->tag == V_OBJ) ? tpls->obj.len : 0;
  Template *arr = (Template *)arena_alloc(a, (nt ? nt : 1) * sizeof(Template));
  for (size_t i = 0; i < nt; i++) {
    arr[i].name = tpls->obj.kvs[i].key;
    Val *v = tpls->obj.kvs[i].val;
    arr[i].source = (v && v->tag == V_STR) ? v->s : sl("", 0);
    arr[i].root = NULL;
    arr[i].failed = 0;
  }
  qsort(arr, nt, sizeof(Template), cmp_template_name);
  out->templates = arr;
  out->ntemplates = nt;
  Val *files = v_obj_get(input, sl("files", 5));
  size_t nf = (files && files->tag == V_ARR) ? files->arr.len : 0;
  Slice *paths = (Slice *)arena_alloc(a, (nf ? nf : 1) * sizeof(Slice));
  Slice *contents = (Slice *)arena_alloc(a, (nf ? nf : 1) * sizeof(Slice));
  for (size_t i = 0; i < nf; i++) {
    Val *f = files->arr.items[i];
    Val *p = f ? v_obj_get(f, sl("path", 4)) : NULL;
    Val *c = f ? v_obj_get(f, sl("content", 7)) : NULL;
    paths[i] = (p && p->tag == V_STR) ? p->s : sl("", 0);
    contents[i] = (c && c->tag == V_STR) ? c->s : sl("", 0);
  }
  out->file_paths = paths;
  out->file_contents = contents;
  out->nfiles = nf;
}

Val *build_site_val(Arena *a, Val *input, StrBuf *err_out) {
  BuildInput in;
  build_input_from_val(a, input, &in);
  OutEntry *outs;
  size_t nouts;
  if (build_site(a, &in, &outs, &nouts, err_out)) return NULL;
  Val *o = v_obj(a);
  for (size_t i = 0; i < nouts; i++) v_obj_set(a, o, outs[i].path, v_str(a, outs[i].content));
  return o;
}
