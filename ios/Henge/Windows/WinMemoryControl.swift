import SwiftUI

/// The memory row: the number on the left, the ladder on the slider, a box on
/// the right for any number the ladder does not land on (1145 MB is a real
/// answer). WinMemory (the ladder and the notes) lives in its own file, without
/// SwiftUI, so it can be tested off an iOS machine.
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
