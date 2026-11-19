import SwiftUI

/// Page 2: the machine while it is being installed. Deliberately not fullscreen
/// and deliberately not a wizard: the picture is the real machine, and one
/// permanent line at the bottom says what the user's hands have to do next.
struct WinInstallView: View {
    @Binding var machine: WinMachine
    @ObservedObject private var engine = WinEngine.shared
    @State private var showingSettings = false

    private var stage: Int { min(machine.stage, WinStage.titles.count - 1) }

    var body: some View {
        VStack(spacing: 0) {
            ZStack {
                Color.black
                WinDisplayView()
                if !engine.running {
                    idleOverlay
                }
            }
            .aspectRatio(16.0 / 10.0, contentMode: .fit)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .clipped()

            controls
            hintBar
        }
        .background(Color(.systemBackground))
        .navigationTitle(machine.name)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                Button { showingSettings = true } label: { Image(systemName: "gearshape") }
            }
        }
        .sheet(isPresented: $showingSettings) { WinSettingsView(machine: $machine) }
    }

    private var idleOverlay: some View {
        VStack(spacing: 10) {
            Image(systemName: machine.stage >= 4 ? "windows" : "opticaldiscdrive")
                .font(.system(size: 34, weight: .light))
                .foregroundStyle(.white.opacity(0.5))
            if let error = engine.errorText {
                Text(error)
                    .font(.footnote)
                    .multilineTextAlignment(.center)
                    .foregroundStyle(.orange)
                    .padding(.horizontal, 24)
            } else if !engine.exitReason.isEmpty {
                Text(engine.exitReason)
                    .font(.footnote)
                    .foregroundStyle(.white.opacity(0.6))
            }
        }
    }

    private var controls: some View {
        HStack(spacing: 12) {
            Button {
                if engine.running { engine.stop() } else { engine.start(machine) }
            } label: {
                Label(engine.running ? "关机" : (machine.stage >= 4 ? "启动" : "开始安装"),
                      systemImage: engine.running ? "stop.fill" : "play.fill")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .controlSize(.large)

            Button {
                WinDisplayView.toggleKeyboard()
            } label: {
                Image(systemName: "keyboard")
            }
            .buttonStyle(.bordered)
            .controlSize(.large)
            .disabled(!engine.running)

            Button {
                engine.powerButton()
            } label: {
                Image(systemName: "power")
            }
            .buttonStyle(.bordered)
            .controlSize(.large)
            .disabled(!engine.running)

            if stage >= 1 && !engine.running {
                Button {
                    machine.stage = min(machine.stage + 1, 4)
                    WinLibrary.shared.update(machine)
                } label: {
                    Label("下一步", systemImage: "chevron.right")
                }
                .buttonStyle(.bordered)
                .controlSize(.large)
            }
        }
        .padding(.horizontal, 14)
        .padding(.top, 10)
    }

    /// The line the whole page exists for: one sentence, never scrolled away.
    private var hintBar: some View {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            Text("\(stage + 1)/5")
                .font(.caption2.monospacedDigit())
                .foregroundStyle(.secondary)
            Text(WinStage.hint(stage))
                .font(.caption)
                .lineLimit(2)
                .foregroundStyle(.primary)
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 8)
        .background(.thinMaterial)
    }
}
