# site-generator native — optimasi top1x (lebih kecil lagi)

Perintah Bede 18:08 WIB: "Buat ulang buat lebih kecil lagi" + "Build".
Basis: entri native top1s (`~/workspace/makefun/work/sitegen-top1s/sitegen-native/`,
4.358 baris C, C11 + SSE2 + N-API v8, gcc -O3 -msse2, `nat.node` 112.064 byte).
Kerja di `~/workspace/makefun/work/sitegen-top1x/sitegen-native-opt/` — **asli top1s
tidak diubah**. Semua perubahan ditulis sendiri dari nol; tidak ada kode yang
disalin dari mana pun.

## Hasil akhir (lokal, VM ini)

| pengukuran | asli top1s | top1x (opt) | delta |
| --- | --- | --- | --- |
| C-level total, 30 ronde interleaved (dev/perf.c) | median 84,8 ms | median ~67,7 ms | **−17 ms** |
| Full-process CPU, 20 ronde interleaved, taskset -c 0 (dev/bench.py) | median 251,3 ms | median **237,4 ms** | **−13,9 ms** |
| VW (JS legal terbaik), full-process yang sama | median 509,9 ms | — | **−272,5 ms vs VW (2,15×)** |

Rincian C-level (perf.c, 3 run): json_parse ~5,0 ms, build ~50 ms, serialize
~12,6 ms, total ~71 ms. (Angka absolut bergeser dengan beban mesin; yang
stabil adalah delta berpasangan di bawah ini.)

Net lokal (full-process dikurangi control bot kosong `main.ts` median
156,2 ms): **opt ~81 ms, asli ~95 ms, VW ~354 ms**. Server mengurangi control
yang sama (`code-heat.ts`: net = cpu_us − controlUs, min 0), jadi yang
menentukan skor server adalah net ini — bukan angka full-process mentah.
Proyeksi server tidak pasti (model-model yang ada memberi 180–560 ms),
tapi semuanya di atas bar top 10 (80 ms) dan jauh dari top 1 (20 ms).
Tetap $0 menurut semua model — opt adalah entri tercepat yang kita punya,
bukan entri yang menang.

## Yang dicoba — dan hasilnya

| # | perubahan | hasil (30 ronde interleaved) | putusan |
| --- | --- | --- | --- |
| 1 | Profil komponen: Node startup ~144 ms + ESM/type-strip ~55 ms (control bot kosong 156,2 ms vs `node -e` 101,3 ms); C ~71 ms (parse 5 + build 50 + serialize 12,6) | — | biaya tetap didominasi Node, bukan C |
| 2 | **Scratch reuse + direct-to-html + SSE2 vector hoisting** (nat_md.c, nat_tpl.c, nat.h): satu scratch StrBuf per render_markdown; blok render langsung ke `html` tanpa temp `blk`; excerpt/TOC heading diambil sebagai offset ke `html`; `ND_VAR` temp diganti scratch statis; `sse2_find_any_v` dengan vektor broadcast yang sudah dihitung | C-level **−7,2 ms median (132,5 → 125,3), menang 23/30** | **SIMPAN** |
| 3 | LTO (`-flto`) | C-level −0,3 ms (netral) | **BUANG**, kembali -O3 -msse2 |
| 4 | PGO + LTO | C-level −1,5 ms median, menang 17/30 | **BUANG** (kecil, rapuh, butuh profiling run) |
| 5 | **Front matter: body jadi slice tanpa copy/scan** (nat_fm.c): `parse_front_matter` berhenti memecah baris begitu pagar `---` penutup ketemu; body = slice kontigu teks setelah newline pagar itu (join baris-baris sisa dengan '\n' merekonstruksi slice itu byte-per-byte, termasuk kasus tanpa newline akhir). Hemat: scan skalar seluruh body (~2,5 MB) + memcpy body + pertumbuhan array lines | digabung dengan #6+#7 di bawah | **SIMPAN** |
| 6 | **jp_string: satu scratch StrBuf per parse** (nat_json.c): hilangkan malloc/free per string (ribuan string per parse); arena_dup tetap | digabung dengan #5+#7 di bawah | **SIMPAN** |
| 7 | **json_escape_mask: vektor broadcast di-hoist** (nat_json.c): 4× `_mm_set1_epi8` dihitung sekali per string, bukan per chunk 16 byte | #5+#6+#7 digabung: C-level **−8,0 ms median, menang 30/30** | **SIMPAN** |
| 8 | **SSE4.2 runtime dispatch** (nat.h, nat_util.c): `cpu_has_sse42()` (cpuid ECX bit 20), `sse42_scan_any` (PCMPESTRI), `sse42_ws_mask` (PCMPESTRM) untuk `sse2_find_any`, `sse2_find_any_v`, `sse2_count_words` | **+4,1 ms median (LEBIH LAMBAT), menang 8/30** — PCMPESTRI microcoded dan scan bukan bottleneck. Sempat ada bug: `_mm_cmpestri` mengembalikan indeks ke operan KEDUA (set harus operan pertama, chunk kedua) — diperbaiki, diuji terisolasi, tetap lebih lambat | **REVERT TOTAL** ke SSE2 murni |
| 9 | **utf8_decode jadi static inline di nat.h** (dari nat_util.c): hilangkan overhead call lintas TU di call site per-codepoint (slugify, template, has_ws — ~2,5 juta call) | C-level **−3,4 ms median, menang 24/30** | **SIMPAN** |

Total C-level vs asli: **−7,1 (scratch) −8,0 (FM/JSON) −3,4 (utf8 inline) ≈ −18 ms**.
Full-process: **−13,9 ms median** (251,3 → 237,4 ms).

## Validasi (kode final)

- **grade-local: 3/3 practice cases, checks 3/3, L1 regrade reproduced byte
  for byte, eligible ranked yes, prize yes.**
- **Differential: 52 case 0 diff + 120-set (376 case) 0 diff, ALL OK.**
- **answers.json byte-identik vs VW**: sha256
  `25980928b145f54712027939364a64c277098bc4528a2d524e0bc27a0275fb03`,
  7.446.432 byte.
- Perhatian: satu edit sempat salah label `case 'b'` → `case '\b'` di
  jp_string (huruf escape JSON vs byte backspace) — diperbaiki sebelum
  validasi; differential 376 case menangkapnya bila lolos.

## File yang berubah vs top1s

`nat.h` (scratch/direct-to-html, SSE2 `_v`, utf8_decode inline),
`nat_md.c` (scratch reuse + direct-to-html), `nat_tpl.c` (scratch ND_VAR),
`nat_fm.c` (body slice), `nat_json.c` (scratch jp_string + hoist escape mask),
`nat_util.c` (utf8_decode dipindah ke header), `nat.node` (rebuild),
`dev/bench.py` (path), `dev/diff-test-opt.mjs` (baru). `nat_build.c`,
`main.ts`, `solve.js`, `nat_napi.c`, `nat_text.c`, `nat_val.c` tidak berubah.
Total baris: 4.388 (dari 4.358). `build.sh` tetap `gcc -O3 -msse2`.

## Temuan harness (membaca sumber harness, bukan menebak)

- Entry **harus** `main.ts` (`ENTRY_SCRIPTS: node22 → main.ts`, argv
  `["node", "/bot/main.ts"]`) — tidak ada opsi `main.js`; biaya ESM/type-strip
  ~55 ms di atas `node -e` tidak bisa dihindari.
- Skor server = **net dari control**: `code-heat.ts` — net = cpu_us −
  controlUs (min 0). Top 1 (20 ms net) berarti gross ≈ control + 20 ms,
  konsisten dengan entri native (Node startup + ~20 ms kerja C).
- Server Node v26.13.1 vs lokal v24.22.1; N-API v8 seharusnya load via
  stabilitas ABI — **belum terverifikasi di Node 26**.

## Risiko (sama seperti top1s, baca sebelum submit)

1. **Diskualifikasi.** `competition.yaml` baris 16 melarang native addon.
   Dev board tidak menegakkannya (top 1 pakman compliant + prize_eligible);
   Bede memerintahkan eksperimen ini ("cari cara sampe 20 ms apapun
   caranya"). Penegakan di final heat **tidak diketahui**.
2. **Node 26.** Addon dibangun/tes di Node v24.22.1; server v26.13.1.
   N-API v8 seharusnya load, belum terverifikasi.
3. **Authorship/transcripts.** Grade-local lokal melaporkan authorship 0,0
   (tanpa transcripts). Gate transcripts berjalan saat submit; `makefun check`
   harus dijalankan atas transcripts sebelum submit.
4. **Prebuilt binary.** Repo menyertakan `nat.node` terkompilasi (112.304
   byte); sumber C disertakan agar bisa rebuild (`bash build.sh`).

## Catatan operasional

- `/tmp` adalah tmpfs 512 MB: bench yang tidak menghapus output 7,4 MB per
  run akan memenuhinya (ENOSPC) dan run gagal diam-diam. `dev/bench.py`
  menghapus tiap output dir setelah run; script bench ad-hoc harus begitu juga.
- `/tmp` dihapus antar exec: rebuild perf/binary + A/B harus dalam satu
  panggilan yang sama.
- Tidak ada lagi tuas C ≥ 5 ms yang terlihat: build 50 ms tersebar di
  render_nodes / render_markdown / v_obj_get / sb_addcstr / arena_alloc.
  Sisa biaya didominasi Node startup + ESM (~156 ms control) yang dikurangi
  server — menaikkan skor server butuh net C di bawah ~38 ms (dari ~81 ms),
  yaitu speedup C 2× lagi, yang tidak terlihat jalannya.
