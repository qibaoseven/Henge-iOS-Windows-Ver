#!/bin/zsh
# Copies the prebuilt iOS pieces HaloWin links into ios/deps (not in git):
#   libslirp, glib, intl, pcre2 (arm64 iOS static libraries, Husk's scripts/build_ios.sh)
# The on-device pairing library (libhusk_rppairing.a) and StikJIT.xcframework are
# not copied any more: both served the built-in JIT helper, which is gone.
set -e
HUSK=${HUSK:-$HOME/Documents/Coding/Husk}
DEPS="$(cd "$(dirname "$0")/.." && pwd)/deps"
mkdir -p "$DEPS/lib"
rm -rf "$DEPS/StikJIT.xcframework"
for l in libslirp libglib-2.0 libintl libpcre2-8; do cp "$HUSK/build/ios-arm64/sysroot/lib/$l.a" "$DEPS/lib/"; done
echo "deps in $DEPS"
