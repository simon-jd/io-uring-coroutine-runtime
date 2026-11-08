#!/usr/bin/env bash
# Download, verify and build the pinned Valkey release (the external reference
# server and its valkey-benchmark) into .cache/.
# Usage: scripts/build-valkey.sh [cache-dir]
set -euo pipefail
version=9.1.3
sha256=184e34ed2d9b51879b682b5b15205e2b9d677c60280e3cb453be8f520d04e293
root=$(cd "$(dirname "$0")/.." && pwd)
cache=${1:-$root/.cache}
tarball=$cache/downloads/valkey-$version.tar.gz
src=$cache/valkey-$version/src
bin=$cache/valkey-$version/bin
url=https://github.com/valkey-io/valkey/archive/refs/tags/$version.tar.gz
if [[ -x $bin/valkey-server && -x $bin/valkey-benchmark ]]; then
  echo "valkey $version already built in $bin"
  exit 0
fi
mkdir -p "$cache/downloads"
if [[ ! -f $tarball ]]; then
  curl -fsSL -o "$tarball.part" "$url"
  mv "$tarball.part" "$tarball"
fi
echo "$sha256  $tarball" | sha256sum -c -
rm -rf "$src" "$bin"
mkdir -p "$src" "$bin"
tar -xzf "$tarball" -C "$src" --strip-components=1
# Default build (bundled jemalloc, -O3 as upstream ships it), no TLS.
# GIT_CEILING_DIRECTORIES keeps Valkey's version script from reading this
# repository's commit as Valkey's own.
GIT_CEILING_DIRECTORIES="$cache" nice make -C "$src" -j"${JOBS:-16}" BUILD_TLS=no \
  > "$cache/valkey-$version/build.log" 2>&1
cp "$src/src/valkey-server" "$src/src/valkey-benchmark" "$src/src/valkey-cli" "$bin/"
echo "valkey $version built in $bin"
