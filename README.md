# site-generator native (top1s) — C11 + SSE2 + N-API

A native addon that implements the whole site-generator pipeline in C.
`main.ts` loads `nat.node` and calls `runAll(INPUT_DIR, OUTPUT_DIR)`.
Rebuild with `bash build.sh` (gcc -O3 -msse2, Node headers in
`/usr/include/node/`, no node-gyp needed). See PREP-NOTES.md for validation,
speed and the disqualification risk.
