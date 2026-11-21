// ios/tests/WinMemoryTests.swift
//
// The memory ladder, checked without an iOS device or Xcode: tools/test-swift.sh
// compiles this with WinMemory.swift and runs it. It asserts the things that
// would silently break the slider -- a lost notch, a handle that cannot reach an
// end, a typed value that parks somewhere other than the nearest notch.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}

// Every machine size the ladder is meant to serve: an iPad, a laptop, a Mac
// Studio, a 2 TB and a 6 TB cluster node.
let caps = [3008, 4096, 5120, 6144, 8192, 10240, 12288, 16384, 32768, 131072,
            524288, 2097152, 6291456]

for cap in caps {
    let s = WinMemory.stops(upTo: cap)
    check(s.first == 256, "cap \(cap): starts at the 256 MB floor")
    check(s.last == cap, "cap \(cap): ends on the machine's own size")
    check(s == s.sorted(), "cap \(cap): ascending")
    check(Set(s).count == s.count, "cap \(cap): no duplicates")
    // The notches that exist because they mean something, not because arithmetic
    // landed there: 5120 (8 GB iPad's desktop share) and 10240 (10 GB).
    if cap >= 5120 { check(s.contains(5120), "cap \(cap): keeps the 5 GB notch") }
    if cap >= 10240 { check(s.contains(10240), "cap \(cap): keeps the 10 GB notch") }
    if cap >= 16384 { check(s.contains(12288) && s.contains(14336) && s.contains(16384),
                            "cap \(cap): keeps 12, 14 and 16 GB") }
    // The handle has to be able to land on every notch.
    for (i, v) in s.enumerated() {
        check(WinMemory.nearestIndex(of: v, in: s) == Double(i), "cap \(cap): notch \(v) is its own index")
    }
}

// Snapping goes to the nearest notch, and the ends hold.
let s16 = WinMemory.stops(upTo: 16_384)
check(WinMemory.nearestIndex(of: 1145, in: s16) == Double(s16.firstIndex(of: 1024)!), "1145 parks on 1024")
check(WinMemory.nearestIndex(of: 1300, in: s16) == Double(s16.firstIndex(of: 1536)!), "1300 parks on 1536")
check(WinMemory.nearestIndex(of: 100, in: s16) == 0, "below the floor parks at the first notch")
check(WinMemory.nearestIndex(of: 999_999, in: s16) == Double(s16.count - 1), "above the top parks at the last notch")
check(WinMemory.nearestIndex(of: 4096, in: []) == 0, "an empty ladder does not crash")

// The two units.
check(WinMemory.label(256) == "256 MB", "label 256 MB")
check(WinMemory.label(1536) == "1.5 GB", "label 1.5 GB")
check(WinMemory.label(6144) == "6.0 GB", "label 6.0 GB")
check(WinMemory.label(131_072) == "128.0 GB", "label 128 GB")

// Above 16 GB the hops widen; they must not stay at 1 GB on a 2 TB machine.
let s2tb = WinMemory.stops(upTo: 2_097_152)
let gaps = zip(s2tb, s2tb.dropFirst()).map { $1 - $0 }
check(gaps.first == 128, "the first hop is 128 MB")
check(gaps.last! > 1024, "the last hop of a 2 TB ladder is wider than 1 GB (got \(gaps.last!))")

print(failures == 0 ? "WinMemory: all checks passed (\(caps.count) machine sizes)" : "WinMemory: \(failures) failures")
exit(failures == 0 ? 0 : 1)
