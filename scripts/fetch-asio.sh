#!/usr/bin/env bash
# Download and verify the pinned standalone Asio release (header-only) into .cache/.
# Usage: scripts/fetch-asio.sh [cache-dir]
set -euo pipefail
version=1-36-0
sha256=0310a76b27e1854f09f696b30de57dc490b5e1b17faed1eb8c9a2891f956e52b
root=$(cd "$(dirname "$0")/.." && pwd)
cache=${1:-$root/.cache}
tarball=$cache/downloads/asio-$version.tar.gz
dest=$cache/asio-$version
url=https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-$version.tar.gz
if [[ -f $dest/include/asio.hpp ]]; then
  echo "asio $version already in $dest"
  exit 0
fi
mkdir -p "$cache/downloads"
if [[ ! -f $tarball ]]; then
  curl -fsSL -o "$tarball.part" "$url"
  mv "$tarball.part" "$tarball"
fi
echo "$sha256  $tarball" | sha256sum -c -
rm -rf "$dest"
mkdir -p "$dest"
tar -xzf "$tarball" -C "$dest" --strip-components=2 "asio-asio-$version/asio/include"
echo "asio $version headers in $dest/include"
