import Foundation
import SwiftUI

/// How much RAM the guest gets: a slider whose notches are a ladder rather than
/// a fixed step, and a box beside it that takes any number at all.
///
/// The engine takes megabytes (`henge-winvm.c` hands `ram_mb` straight to the
/// board), so nothing is rounded to gigabytes. Two things follow from that:
/// a Tiny11 27H1 image can be started at 256 MB, and a request like 1145 MB is
/// as valid as 1024 -- which is why the number can also just be typed.
///
/// The ladder starts fine where small machines live and widens as it goes up:
/// 256, 384, 512, 768, then in 1 GB steps to 10 GB, and from there the step
/// doubles every eight steps of the previous size: 2 GB to 16 GB, 4 GB to 32 GB,
/// 8 GB to 64 GB, 16 GB to 128 GB, 32 GB to 256 GB, 64 GB to 512 GB, 128 GB to
/// 1 TB and 256 GB to 2 TB -- nothing here assumes a laptop-sized ceiling, since
/// this also runs under PlayCover on Macs, including the 2 TB and 6 TB clusters.
enum WinMemory {
    /// The floor. Tiny11 is reported to start in 256 MB; below that the firmware
    /// plus the Windows loader have nothing to stand on.
    static let floorMB = 256

    /// What this device has, in megabytes. `ProcessInfo` reports the machine's
    /// RAM whatever the app is running under, so PlayCover on a 512 GB Mac Studio
    /// offers 512 GB and a cluster node offers whatever it has -- there is no
    /// ceiling written down here.
    static var deviceMB: Int {
        let mb = Int(ProcessInfo.processInfo.physicalMemory >> 20)
        // Rounded down to a notch so the last stop is the device's own size, and
        // never below the floor: a device too small for 256 MB still gets 256.
        return max(floorMB, (mb / 128) * 128)
    }

    /// The top of the slider: this device's memory. Asking for more than the
    /// machine has does not work, so it is not offered.
    static var ceilingMB: Int { deviceMB }

    /// "256 MB", "1.5 GB", "6.0 GB": two units, because 3.5 GB is a real answer
    /// and a slider that only says "GB" hides it.
    static func label(_ mb: Int) -> String {
        if mb < 1024 { return "\(mb) MB" }
        return String(format: "%.1f GB", Double(mb) / 1024)
    }

    /// The notches, up to whatever this device has. Every value is megabytes.
    static func stops() -> [Int] {
        let cap = ceilingMB
        // Everything up to 16 GB is written out by hand, because the values that
        // matter are not evenly spaced and a generated list would lose them:
        // 256 MB is the Tiny11 floor, 1536 MB is a small full install, 5120 MB is
        // the 8 GB iPad's desktop share, 10240 MB is 10 GB, and 16 GB is where
        // doubling starts. Only above 16 GB is the ladder arithmetic.
        let handwritten = [256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096, 5120, 6144,
                           7168, 8192, 9216, 10240, 12288, 14336, 16_384]
        var out = handwritten.filter { $0 < cap }
        guard cap > 16_384 else {
            // A machine smaller than the hand-written list ends on its own size.
            if out.last != cap { out.append(cap) }
            return out
        }
        // Above 16 GB: 4 GB hops to 32 GB, and from there the step doubles once
        // it is an eighth of where it is -- 8 GB to 64 GB, 16 GB to 128 GB, 32 GB
        // to 256 GB, 64 GB to 512 GB, 128 GB to 1 TB, 256 GB to 2 TB.
        var next = 16_384
        var step = 4096
        while next < cap {
            while next >= step * 8 { step *= 2 }
            next += step
            out.append(min(next, cap))
        }
        return out
    }

    /// A typed number: kept exactly, only held inside the range.
    static func snapped(_ mb: Int) -> Int { min(max(mb, floorMB), ceilingMB) }

    /// Which notch the handle sits on: the nearest one. It snaps rather than
    /// floats, so the handle never claims a size the guest does not have -- a
    /// typed 1145 MB parks the handle on 1024 while the box keeps 1145.
    static func nearestIndex(of mb: Int, in stops: [Int]) -> Double {
        guard !stops.isEmpty else { return 0 }
        let clamped = snapped(mb)
        var best = 0
        var bestDistance = Int.max
        for (i, stop) in stops.enumerated() {
            let d = abs(stop - clamped)
            if d < bestDistance { bestDistance = d; best = i }
        }
        return Double(best)
    }

    /// What this number risks, if anything. nil when it is a sane ask.
    static func note(_ mb: Int) -> String? {
        if mb < 1024 {
            return "低于 1 GB 只适合 Tiny11 这类精简镜像。完整 Windows 11 ARM64 装不上，安装程序会直接报内存不足。"
        }
        if mb < 4096 {
            return "低于微软写的 4 GB 下限：只适合精简镜像，完整 Windows 11 能装但很慢，更新多半会失败。"
        }
        if mb > deviceMB {
            return "这台设备一共就 \(label(deviceMB))。要更多，iOS 会把 Henge 挤掉。"
        }
        if mb >= deviceMB {
            return "\(label(mb)) 已经是这台设备的全部内存了：iOS 自己要占一部分，Windows 会换页换到手软。"
        }
        if mb > deviceMB / 2 {
            return "用了这台设备一半以上的内存：切回别的 app 时 Henge 有被系统关掉的风险。"
        }
        return nil
    }
}

/// The memory row: the number on the left, the ladder on the slider, a box on the
/// right for any number the ladder does not land on (1145 MB is a real answer).
struct WinMemoryControl: View {
    @Binding var mb: Int

    private var stops: [Int] { WinMemory.stops() }

    /// The slider runs over notch indices, so dragging lands on notches and the
    /// handle snaps to the nearest one; a typed value parks it there while the
    /// box keeps the exact number.
    private var indexBinding: Binding<Double> {
        Binding(
            get: { WinMemory.nearestIndex(of: mb, in: stops) },
            set: { value in
                let i = min(max(Int(value.rounded()), 0), max(stops.count - 1, 0))
                if stops.indices.contains(i) { mb = stops[i] }
            }
        )
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 10) {
                Text(WinMemory.label(mb))
                    .monospacedDigit()
                    .font(.callout.weight(.medium))
                Slider(value: indexBinding, in: 0...Double(max(stops.count - 1, 1)))
                // The box takes megabytes: it is the same unit the engine and the
                // slider are in, so 1145 means 1145.
                TextField("MB", value: $mb, format: .number)
                    .multilineTextAlignment(.trailing)
                    .keyboardType(.numberPad)
                    .textFieldStyle(.roundedBorder)
                    .frame(width: 88)
                    .onSubmit { mb = WinMemory.snapped(mb) }
            }
            HStack {
                Text(WinMemory.label(WinMemory.floorMB))
                Spacer()
                Text(WinMemory.label(WinMemory.ceilingMB))
            }
            .font(.caption2)
            .foregroundStyle(.tertiary)
            if let note = WinMemory.note(mb) {
                Text(note).font(.footnote).foregroundStyle(.secondary)
            }
        }
    }
}
