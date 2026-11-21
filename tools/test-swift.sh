#!/bin/sh
# Checks the Swift here without a device or Xcode:
#   1. parses every .swift file under ios/Henge (catches syntax, needs no SDK)
#   2. compiles and runs the WinMemory ladder tests (Foundation only, so it runs
#      on Linux too -- that is why the ladder sits in its own file)
#
#   sh tools/test-swift.sh              # swift from PATH
#   SWIFTC=/opt/swift/usr/bin/swiftc sh tools/test-swift.sh
set -e
SWIFTC=${SWIFTC:-swiftc}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/swift-test"
mkdir -p "$OUT"

echo "==> parsing every Swift file under ios/Henge"
"$SWIFTC" -parse $(find "$ROOT/ios/Henge" -name '*.swift')

echo "==> running the WinMemory ladder tests"
cp "$ROOT/ios/Henge/Windows/WinMemory.swift" "$OUT/WinMemory.swift"
cp "$ROOT/ios/tests/WinMemoryTests.swift" "$OUT/main.swift"
"$SWIFTC" -O -o "$OUT/winmemery" "$OUT/WinMemory.swift" "$OUT/main.swift"
"$OUT/winmemery"
