#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build the kmod with the OpenWrt SDK and stage the artifact in deploy/.
#
# Usage: tools/build.sh [path-to-sdk]
# Default SDK path: build/openwrt-sdk-* in this repository (gitignored).
# Also builds tools/ssc-pcm-test with the SDK's cross compiler.
set -e

here=$(cd "$(dirname "$0")/.." && pwd)
root=$here

sdk=${1:-$(ls -d "$root"/build/openwrt-sdk-* 2>/dev/null | head -1)}
if [ -z "$sdk" ] || [ ! -d "$sdk" ]; then
	echo "no SDK found; pass the path as an argument" >&2
	exit 1
fi

pkgdir="$sdk/package/kmod-lantiq-ssc-pcm"
mkdir -p "$pkgdir"
cp -a "$here/Makefile" "$pkgdir/"
rm -rf "$pkgdir/src"
cp -a "$here/src" "$pkgdir/"

(
	cd "$sdk"
	make package/kmod-lantiq-ssc-pcm/clean >/dev/null 2>&1 || true
	make package/kmod-lantiq-ssc-pcm/compile V=s
)

ko="$sdk/build_dir/target-mips_24kc_musl/linux-lantiq_xrx200/lantiq-ssc-pcm-0.1/.pkgdir/kmod-lantiq-ssc-pcm/lib/modules/6.12.112/lantiq_ssc_pcm.ko"
apk=$(ls "$sdk"/bin/targets/lantiq/xrx200/packages/kmod-lantiq-ssc-pcm-*.apk 2>/dev/null | head -1)

mkdir -p "$root/deploy"
cc=$(ls "$sdk"/staging_dir/toolchain-*/bin/*-openwrt-linux-musl-gcc | head -1)
STAGING_DIR="$sdk/staging_dir" "$cc" -O2 -Wall -I"$here/src/include/uapi" \
	-o "$root/deploy/ssc-pcm-test" "$here/tools/ssc-pcm-test.c"
[ -f "$ko" ] && cp -a "$ko" "$root/deploy/lantiq_ssc_pcm.ko"
[ -n "$apk" ] && cp -a "$apk" "$root/deploy/"

echo "artifacts in $root/deploy:"
ls -l "$root/deploy" 2>/dev/null || true
