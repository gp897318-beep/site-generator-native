#!/bin/bash
# Build the native addon (nat.node) with gcc + the system Node headers.
# No node-gyp, no network, no dependencies.
set -euo pipefail
cd "$(dirname "$0")"
gcc -O3 -msse2 -DNAPI_VERSION=8 -DNODE_GYP_MODULE_NAME=nat \
  -I/usr/include/node -shared -fPIC \
  -o nat.node \
  nat_util.c nat_text.c nat_fm.c nat_md.c nat_tpl.c nat_val.c nat_json.c nat_build.c nat_napi.c
ls -la nat.node
