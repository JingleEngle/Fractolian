#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
case "$(uname -m)" in
  arm64) arch=arm64; macho=arm64 ;;
  x86_64) arch=x64; macho=x86_64 ;;
  *) echo "This build requires a Mac with an Intel or Apple Silicon processor."; exit 1 ;;
esac
if ! xcrun --find clang++ >/dev/null 2>&1; then
  echo "Install Apple's command line tools with xcode-select --install, then run this file again."
  exit 1
fi
for tool in jq zstd; do
  if ! command -v "$tool" >/dev/null; then
    echo "Install jq and zstd with Homebrew (brew install jq zstd), then run this file again."
    exit 1
  fi
done
tmp=$(mktemp -d /tmp/fractolian.XXXXXXXX)
trap 'rm -rf "$tmp"' EXIT
curl -fsSL --retry 3 "https://vcvrack.com/downloads/Rack-SDK-2.6.6-mac-$arch.zip" -o "$tmp/sdk.zip"
unzip -q "$tmp/sdk.zip" -d "$tmp"
mkdir "$tmp/project"
cp -R src res LICENSE Makefile plugin.json "$tmp/project/"
make -C "$tmp/project" -j2 RACK_DIR="$tmp/Rack-SDK" dist
slug=$(jq -er .slug plugin.json)
version=$(jq -er .version plugin.json)
package="$tmp/project/dist/$slug-$version-mac-$arch.vcvplugin"
mkdir "$tmp/check"
zstd -dc "$package" | tar -xf - -C "$tmp/check"
plugin="$tmp/check/$slug/plugin.dylib"
test "$(lipo -archs "$plugin")" = "$macho"
codesign --verify --strict "$plugin"
otool -L "$plugin" | grep -F '/tmp/Rack2/libRack.dylib'
nm -gjU "$plugin" | grep -x '_init'
cmp plugin.json "$tmp/check/$slug/plugin.json"
cmp res/FractalXY.svg "$tmp/check/$slug/res/FractalXY.svg"
cp "$package" "Fractolian-$version-mac-$arch.vcvplugin"
echo "Built $PWD/Fractolian-$version-mac-$arch.vcvplugin"
