#!/usr/bin/env bash
# Download, verify and build the pinned liburing release into .cache/.
# Usage: scripts/build-liburing.sh [cache-dir]
set -euo pipefail
version=2.14
sha256=5f80964108981c6ad979c735f0b4877d5f49914c2a062f8e88282f26bf61de0c
root=$(cd "$(dirname "$0")/.." && pwd)
cache=${1:-$root/.cache}
tarball=$cache/downloads/liburing-$version.tar.gz
src=$cache/liburing-$version/src
prefix=$cache/liburing-$version/install
url=https://github.com/axboe/liburing/archive/refs/tags/liburing-$version.tar.gz

if [[ -f $prefix/lib/liburing.a && -f $prefix/include/liburing.h ]]; then
  echo "liburing $version already built in $prefix"
  exit 0
fi
mkdir -p "$cache/downloads"
if [[ ! -f $tarball ]]; then
  curl -fsSL -o "$tarball.part" "$url"
  mv "$tarball.part" "$tarball"
fi
echo "$sha256  $tarball" | sha256sum -c -
rm -rf "$src" "$prefix"
mkdir -p "$src"
tar -xzf "$tarball" -C "$src" --strip-components=1
cd "$src"
# Static library only; -fPIC so sanitizer and fuzzer builds can link it.
CFLAGS="-O2 -g -fPIC" ./configure --prefix="$prefix" >/dev/null
make -C src -j"$(nproc 2>/dev/null || echo 4)" liburing.a >/dev/null
mkdir -p "$prefix/lib" "$prefix/include"
cp src/liburing.a "$prefix/lib/"
cp -R src/include/. "$prefix/include/"
echo "liburing $version built in $prefix"
