// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UIKit

/// The JIT walkthrough: pick a way in, follow its numbered steps, end with JIT on.
///
/// Three ways in, all of them a separate app or the system: StikDebug (the one
/// most installs use), TrollStore, or a jailbreak's Allow JIT setting. Presented
/// from onboarding, Settings › JIT, and whenever a boot finds JIT not set up.
struct JITSetupFlow: View {
    enum Step: Hashable { case stikDebug, trollStore, jailbreak }

    @ObservedObject private var jit = JITCoordinator.shared
    @State private var path: [Step] = []
    @Environment(\.dismiss) private var dismiss

    private var device: String {
        UIDevice.current.userInterfaceIdiom == .pad ? "iPad" : "iPhone"
    }

    var body: some View {
        NavigationStack(path: $path) {
            choose
                .hengeDestination(for: Step.self) { step in
                    switch step {
                    case .stikDebug: stikDebug
                    case .trollStore: trollStore
                    case .jailbreak: jailbreak
                    }
                }
        }
        .tint(Theme.accent)
        .onAppear { HengeLog.log("ui", "JIT walkthrough opened") }
        // Swiping the sheet away, or a copy shown from onboarding, must not
        // leave a request to show it again behind.
        .onDisappear { jit.showSetup = false }
    }

    private func close() {
        jit.showSetup = false
        dismiss()
    }

    // MARK: Choose

    private var choose: some View {
        page(symbol: "bolt.fill", title: "Turn on JIT",
             subtitle: "Guest memory has to be writable and then executable, which on iOS only an attached "
                     + "debugger can grant. Choose how your \(device) gets one.") {
            VStack(spacing: 10) {
                way("Use StikDebug", symbol: "ant",
                    detail: JITBootstrap.isStikDebugInstalled ? "StikDebug is installed."
                        : "The usual way: install StikDebug and let HaloWin open it.",
                    done: jit.method == .stikDebug) { path.append(.stikDebug) }
                way("Use TrollStore", symbol: "sparkles",
                    detail: JITBootstrap.isTrollStoreInstalled ? "TrollStore is installed."
                        : "For a HaloWin installed through TrollStore.",
                    done: jit.method == .trollStore) { path.append(.trollStore) }
                way("Use a jailbreak", symbol: "lock.open",
                    detail: JITBootstrap.debuggedAtLaunch ? "JIT was already on when HaloWin opened."
                        : JITBootstrap.isJailbroken ? "A jailbreak was found. Turn on Allow JIT in Apps."
                        : "Dopamine can give every app JIT with one setting.",
                    done: jit.method == .jailbreak || JITBootstrap.debuggedAtLaunch) { path.append(.jailbreak) }
            }
            if let status = jit.status { outcome(status, ok: true) }
            if let error = jit.error { outcome(error, ok: false) }
        } actions: {
            Button("Not now") { close() }
                .font(.system(size: 15, weight: .medium))
                .foregroundStyle(Theme.textDim)
        }
    }

    private func way(_ title: String, symbol: String, detail: String, done: Bool,
                     enabled: Bool = true, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            HStack(spacing: 14) {
                Image(systemName: symbol)
                    .font(.system(size: 17, weight: .semibold))
                    .foregroundStyle(Theme.accent)
                    .frame(width: 34, height: 34)
                    .background(Theme.accentSoft, in: RoundedRectangle(cornerRadius: 10, style: .continuous))
                VStack(alignment: .leading, spacing: 3) {
                    Text(title).font(.system(size: 16, weight: .semibold)).foregroundStyle(Theme.text)
                    Text(detail).font(.system(size: 13)).foregroundStyle(Theme.textDim)
                        .fixedSize(horizontal: false, vertical: true)
                        .multilineTextAlignment(.leading)
                }
                Spacer(minLength: 8)
                Image(systemName: done ? "checkmark.circle.fill" : "chevron.right")
                    .font(.system(size: done ? 18 : 13, weight: .semibold))
                    .foregroundStyle(done ? .green : Theme.textDim.opacity(0.7))
            }
            .padding(14)
            .contentShape(Rectangle())
            .hengeCard()
        }
        .buttonStyle(CardButtonStyle())
        .disabled(!enabled)
        .opacity(enabled ? 1 : 0.5)
    }

    // MARK: StikDebug

    private var stikDebug: some View {
        page(symbol: "ant", title: "Use StikDebug",
             subtitle: "StikDebug is a separate app that attaches to HaloWin. HaloWin sends it the JIT script "
                     + "itself, so nothing needs configuring for HaloWin inside StikDebug.") {
            VStack(alignment: .leading, spacing: 14) {
                point(1, "Install [StikDebug](https://github.com/StikDebug/StikDebug/releases/latest).",
                      done: JITBootstrap.isStikDebugInstalled)
                point(2, "Import this \(device)'s pairing file into StikDebug.")
                point(3, "Whenever you start a machine, HaloWin opens StikDebug, which attaches and comes back.")
            }
            .padding(16).hengeCard()
        } actions: {
            Button("Use StikDebug") {
                jit.method = .stikDebug
                HengeLog.log("ui", "JIT method set to StikDebug")
                close()
            }
            .buttonStyle(PrimaryButtonStyle())
        }
    }

    // MARK: TrollStore

    private var trollStore: some View {
        page(symbol: "sparkles", title: "Use TrollStore",
             subtitle: "TrollStore can enable JIT for apps it installed, with no pairing file, "
                     + "VPN or computer.") {
            VStack(alignment: .leading, spacing: 14) {
                point(1, "Install HaloWin.ipa through TrollStore, on an iOS version TrollStore supports. It carries the "
                       + "entitlements TrollStore keeps, so the same file works there and in a sideloader.",
                      done: JITBootstrap.isInstalledWithTrollStore)
                point(2, "In TrollStore's Settings, turn on URL Scheme. TrollStore ignores enable-jit requests without it.")
                point(3, "Whenever a machine starts, HaloWin asks TrollStore to enable JIT. TrollStore opens HaloWin, "
                       + "attaches to it for a moment and lets go, which leaves it allowed to run code it wrote.")
            }
            .padding(16).hengeCard()
            if !JITBootstrap.isTrollStoreInstalled {
                Label("TrollStore was not found on this \(device). It installs on iOS 14.0 to 17.0.", systemImage: "info.circle")
                    .font(.system(size: 13)).foregroundStyle(Theme.textDim)
                    .fixedSize(horizontal: false, vertical: true)
            }
        } actions: {
            Button("Use TrollStore") {
                jit.method = .trollStore
                HengeLog.log("ui", "JIT method set to TrollStore")
                close()
            }
            .buttonStyle(PrimaryButtonStyle())
        }
    }

    // MARK: Jailbreak

    private var jailbreak: some View {
        page(symbol: "lock.open", title: "Use a jailbreak",
             subtitle: "On a device jailbroken with Dopamine, JIT is a setting: the jailbreak marks every app as "
                     + "debugged when it opens, and a debugged app may run code it wrote. HaloWin needs nothing else.") {
            VStack(alignment: .leading, spacing: 14) {
                point(1, "Open Dopamine and go to its Settings.", done: JITBootstrap.isJailbroken)
                point(2, "Turn on Allow JIT in Apps.")
                point(3, "Close HaloWin completely and open it again. The setting applies to apps as they start.",
                      done: JITBootstrap.debuggedAtLaunch)
            }
            .padding(16).hengeCard()
            if JITBootstrap.debuggedAtLaunch {
                Label("HaloWin was marked as debugged when it opened, so JIT is on.", systemImage: "checkmark.circle.fill")
                    .font(.system(size: 13)).foregroundStyle(.green)
            } else if !JITBootstrap.isJailbroken {
                Label("No jailbreak was found on this \(device).", systemImage: "info.circle")
                    .font(.system(size: 13)).foregroundStyle(Theme.textDim)
            }
        } actions: {
            Button("Use the jailbreak") {
                jit.method = .jailbreak
                HengeLog.log("ui", "JIT method set to jailbreak")
                close()
            }
            .buttonStyle(PrimaryButtonStyle())
        }
    }

    // MARK: Pieces

    private func page<Content: View, Actions: View>(
        symbol: String, title: String, subtitle: String,
        @ViewBuilder content: () -> Content,
        @ViewBuilder actions: () -> Actions) -> some View {
        ZStack {
            Theme.backdrop
            ScrollView {
                VStack(alignment: .leading, spacing: 16) {
                    Image(systemName: symbol)
                        .font(.system(size: 26, weight: .semibold))
                        .foregroundStyle(Theme.accent)
                        .frame(width: 56, height: 56)
                        .background(Theme.accentSoft, in: RoundedRectangle(cornerRadius: 16, style: .continuous))
                    Text(title).font(.system(size: 28, weight: .semibold)).foregroundStyle(Theme.text)
                    Text(subtitle).font(.system(size: 15)).foregroundStyle(Theme.textDim)
                        .fixedSize(horizontal: false, vertical: true)
                    content()
                }
                .padding(.horizontal, 22).padding(.top, 8).padding(.bottom, 24)
            }
        }
        .safeAreaInset(edge: .bottom, spacing: 0) {
            VStack(spacing: 12) { actions() }
                .padding(.horizontal, 22).padding(.top, 10).padding(.bottom, 16)
                .background(Theme.backdrop)
        }
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .confirmationAction) {
                Button("Close") { close() }
            }
        }
    }

    private func point(_ number: Int, _ text: String, done: Bool = false) -> some View {
        HStack(alignment: .top, spacing: 12) {
            ZStack {
                Circle().fill(done ? Color.green.opacity(0.18) : Theme.accentSoft)
                if done {
                    Image(systemName: "checkmark").font(.system(size: 12, weight: .bold)).foregroundStyle(.green)
                } else {
                    Text("\(number)").font(.system(size: 13, weight: .bold)).foregroundStyle(Theme.accent)
                }
            }
            .frame(width: 26, height: 26)
            Text(.init(text))
                .font(.system(size: 15))
                .foregroundStyle(Theme.text)
                .fixedSize(horizontal: false, vertical: true)
            Spacer(minLength: 0)
        }
    }

    private func outcome(_ text: String, ok: Bool) -> some View {
        Label(text, systemImage: ok ? "checkmark.circle.fill" : "exclamationmark.triangle.fill")
            .font(.system(size: 14, weight: .medium))
            .foregroundStyle(ok ? .green : .red)
            .fixedSize(horizontal: false, vertical: true)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(14).hengeCard()
    }
}
