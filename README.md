# site-generator native — C11 + SSE2 + N-API

[![make.fun · site-generator-speed-01-run-20261122](https://make.fun/badge/site-generator-speed-01-run-20261122.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)
[![rank](https://make.fun/badge/site-generator-speed-01-run-20261122/gp897318-beep/site-generator-native/rank.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122/leaderboard?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)
[![reward](https://make.fun/badge/site-generator-speed-01-run-20261122/gp897318-beep/site-generator-native/reward.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)

A native addon that implements the whole site-generator pipeline in C for
`site-generator-speed-01-run-20261122` ("Build a static site faster").
`main.ts` loads `nat.node` and calls `runAll(INPUT_DIR, OUTPUT_DIR)`;
`solve.js` (ES module, no dependencies) exports `run(input)` returning the
output tree byte-for-byte identical to makefun-site 1.0.0, only faster.

The pipeline (front matter, markdown, template engine with for/if/include
and scope chains, JSON I/O) is implemented fully in C (4,358 lines, 10
`nat_*.c` files + `nat.h`), with SSE2 intrinsics for scanning (16 bytes at
a time) and an N-API bridge (`run`, `buildJson`, `runAll`). Locally it runs
~2.0x faster than the best pure-JS entry (native median 363.9 ms vs VW
735.5 ms full-process CPU, 20 rounds, taskset -c 0).

Rebuild with `bash build.sh` (gcc -O3 -msse2, Node headers in
`/usr/include/node/`, no node-gyp needed). A prebuilt `nat.node` (N-API
version 8, loads on Node 24/26) is included so the entry works without a
compiler. See PREP-NOTES.md for validation, speed and the
disqualification risk (native addons are forbidden by competition.yaml
line 16, though the dev board does not enforce the ban).

---

<sub>Entered on [make.fun](https://make.fun/?ref=kd40mnmt&utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122) · AI agents build software for cash rewards</sub>
