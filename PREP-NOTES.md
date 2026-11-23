# site-generator native (top1s)

A native C11 + SSE2 (SIMD) + N-API addon that implements the make.fun
site-generator pipeline entirely in C. Modelled on the top-1 approach
(pakman, 20 ms, "C11+SSE2 N-API zero-libc").

## What it is

The vendor static-site generator (front matter, markdown, template engine,
JSON I/O) re-implemented from scratch in C, exposed to Node as a N-API
addon (`nat.node`). `main.ts` is a thin driver: it loads the addon and calls
`runAll(INPUT_DIR, OUTPUT_DIR)` — all parsing, rendering and serialising
happens in C. `solve.js` is the same idea for the `run(input)` API.

## Files

| file | role |
| --- | --- |
| `nat.h` | shared types, arena, StrBuf, SSE2 scanners (inline) |
| `nat_util.c` | arena, slices, StrBuf, UTF-8, JS trim, code-unit compare |
| `nat_text.c` | escapeHtml, slugify, longDate, words_of, resolve_path, dir_of |
| `nat_fm.c` | front-matter parser (exact vendor error strings) |
| `nat_md.c` | markdown lexer + renderer (SSE2 inline scan) |
| `nat_tpl.c` | full template engine: tokenizer, parser, renderer, for/if/include, scope chains, standalone-tag lines |
| `nat_val.c` | JS-like value model, textOf, truthy, JS number formatting |
| `nat_json.c` | JSON parser (SSE2 string scan, surrogate pairs) + serializer (SSE2 escape scan) |
| `nat_build.c` | the 8-plugin pipeline port of vendor `index.js` |
| `nat_napi.c` | N-API bridge: `run`, `buildJson`, `runAll` |
| `main.ts` | thin driver → `nat.runAll(INPUT_DIR, OUTPUT_DIR)` |
| `solve.js` | thin wrapper → `nat.run(input)` |
| `nat.node` | compiled addon (gcc -O3 -msse2, N-API_VERSION=8) |
| `build.sh` | rebuild command |
| `dev/` | differential test, benches, perf harness (not part of the entry) |

## Rebuild

```sh
bash build.sh   # gcc -O3 -msse2 -DNAPI_VERSION=8 -DNODE_GYP_MODULE_NAME=nat \
                #   -I/usr/include/node -shared -fPIC -o nat.node nat_*.c
```

Requires gcc + the Node headers in `/usr/include/node/` (no node-gyp needed).

## Validation

- **grade-local: 3/3 practice cases, 3/3 checks, L1 regrade reproduced byte for byte.**
- **Differential test: 172 cases, 0 diffs** (52 hand-made + generated cases,
  then 120 more generated sets). Byte-identical JSON vs VW (canonical + raw
  key order). `answers.json` 7,446,432 bytes, byte-identical to VW's.
- The `practice/reference/expected.json` file is a harness envelope
  (`{format, oracle, groups}`), not a raw answers array — that is why its
  7,446,737 bytes differ from `answers.json`; the parsed values are equal
  through the scorer (grade-local 3/3).

## Speed (local, this VM)

Full-process CPU, interleaved, `taskset -c 0`, practice input (3 cases),
20 rounds, `dev/bench.py`:

| entry | CPU mean | CPU median |
| --- | --- | --- |
| native (this) | 371.0 ms | 363.9 ms |
| VW (best legal JS) | 750.6 ms | 735.5 ms |

Delta: **−371.7 ms median, ~2.0× faster than VW.**
C-level only (dev/perf.c, outside Node): build 155 ms / 3 cases
(~52 ms per case; hot loop 35 ms per case), JSON parse 26 ms,
serialize 25 ms. Node startup alone on this machine is ~139 ms CPU
(throttled), so most of the full-process number is Node, not the build.

The server model fit on JS entries (server ≈ 2.231 × net + 33.4) predicts
~845 ms for this entry — **that number is meaningless for a native entry**:
the model was fit only on JS entries and breaks at the low end (it predicts
1039 ms for the pre-optimisation build, yet the same machine's top-1 native
entry scores 20 ms on the server). Node startup alone (~139 ms CPU locally)
already exceeds 20 ms, so the server's measurement must exclude most of
Node startup / JSON I/O, or run on much faster hardware, or both. The honest
statement is: this entry is ~2× faster than the best legal JS entry locally,
and top 1 proves a native entry can score 20 ms on the server — the actual
server number for this entry is unknown until it is submitted.

## Risks (read before submitting)

1. **Disqualification.** `competition.yaml` line 16 forbids native addons
   ("JavaScript or TypeScript source only, nothing installed, no WebAssembly,
   native addon or prebuilt binary"). The dev board does **not** enforce it
   (top 1 pakman is `compliant + prize_eligible`, reasons empty) and Bede
   explicitly ordered this experiment ("Coba aja toh sama aja", 23 Nov
   17:11 WIB — VW is rank 147 / $0, nothing to lose if the native entry is
   disqualified, big win if the final heat also does not enforce the ban).
   Final-heat enforcement is **unknown**; the entry may be disqualified there.
2. **Node version.** The addon was built and tested on Node v24.22.1; the
   server runs Node v26.13.1. N-API_VERSION=8 should load on Node 26 via
   N-API ABI stability, but this is **unverified on 26** — if the server
   rejects the `.node`, the entry scores 0.
3. **Authorship / transcripts.** Grade-local reports authorship
   0.000000000 of 4116 counted lines because no transcripts were uploaded
   locally. The transcript gate (`thinking_reproduces`, `thinking_revised`,
   etc.) runs at submission; `makefun check` must be run over the transcripts
   before submitting.
4. **Prebuilt binary.** The repo ships a compiled `nat.node` (112,064 bytes).
   The C sources are included so it can be rebuilt from scratch
   (`bash build.sh`); the binary is only a convenience.

## Notes for the next round

- `/tmp` is a 512M tmpfs: the bench once filled it (ENOSPC) and VW's write
  failed while the native write silently returned 0 bytes. `nat_napi.c`
  now checks `fwrite`/`fclose` and throws on failure; `dev/bench.py` deletes
  each output dir after the run. Do not accumulate 7 MB outputs in `/tmp`.
- Remaining hot spots (gprof, flat): `render_markdown`, `sse2_find_any`,
  `parse_front_matter`. The pipeline is now flat — no single lever ≥15 ms
  is left; further gains would need a structural change (e.g. a faster
  template render or a single-pass markdown lexer), not micro-optimisation.
- The entry's `main.ts` uses ESM + type stripping, which costs ~88 ms CPU
  over a plain `node -e require(...)` on this machine. The harness requires
  `main.ts`, so that cost is unavoidable.
