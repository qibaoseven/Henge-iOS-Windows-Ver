// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import SwiftUI
import UIKit

/// How HaloWin gets a debugger attached. Three ways, all of them outside the app:
/// StikDebug, TrollStore, or a jailbreak. (There used to be a fourth -- a debugger
/// shipped inside the app as an extension -- and it is gone on purpose: StikDebug
/// and a LiveContainer install cover it, and an extension that debugs its own host
/// was one more thing to sign, strip and keep alive.)
enum JITMethod: String, CaseIterable, Identifiable {
    /// TrollStore or jailbreak first, then StikDebug.
    case automatic
    case stikDebug
    case trollStore
    case jailbreak

    var id: String { rawValue }
    var title: String {
        switch self {
        case .automatic: return "Automatic"
        case .stikDebug: return "StikDebug"
        case .trollStore: return "TrollStore"
        case .jailbreak: return "Jailbreak (Dopamine)"
        }
    }
}

@MainActor
final class JITCoordinator: ObservableObject {
    static let shared = JITCoordinator()

    @Published var method: JITMethod {
        didSet { UserDefaults.standard.set(method.rawValue, forKey: "henge.jitMethod") }
    }
    @Published var showSetup = false
    @Published private(set) var busy = false
    @Published private(set) var status: String?
    @Published private(set) var error: String?
    /// Bumped each time a debugger attaches. Nothing else tells ContentView to
    /// claim the code region, so this is what it watches.
    @Published private(set) var attachGeneration = 0

    private init() {
        // A stored "builtIn" from an older install maps to nil, which lands on automatic.
        method = UserDefaults.standard.string(forKey: "henge.jitMethod")
            .flatMap(JITMethod.init(rawValue:)) ?? .automatic
    }

    var resolvedMethod: JITMethod {
        guard method == .automatic else { return method }
        if JITBootstrap.isTrollStoreInstalled { return .trollStore }
        if JITBootstrap.isJailbroken { return .jailbreak }
        return .stikDebug
    }

    var automaticDescription: String {
        switch resolvedMethod {
        case .stikDebug: return "StikDebug is installed, or is the one to install, so HaloWin will open it."
        case .trollStore: return "TrollStore is installed, so HaloWin will ask it to enable JIT."
        case .jailbreak: return "This device is jailbroken, so JIT comes from the jailbreak's Allow JIT in Apps setting."
        case .automatic: return "Automatic picks StikDebug, TrollStore or the jailbreak."
        }
    }

    private func log(_ line: String) { HengeLog.log("jit", line) }

    /// Ask TrollStore for JIT each time HaloWin opens without it (#135), unless
    /// turned off in Settings: TrollStore attaches to the running app for a moment,
    /// with nothing to tap.
    static let trollStoreAutoKey = "henge.jit.trollStoreAuto"
    static var trollStoreAuto: Bool { UserDefaults.standard.object(forKey: trollStoreAutoKey) as? Bool ?? true }
    private var autoTried = false

    func enableAtLaunchIfAsked() {
        if !autoTried, JITBootstrap.isInstalledWithTrollStore, Self.trollStoreAuto {
            autoTried = true
            guard !JITBootstrap.ready else { return }
            log("installed by TrollStore: asking it for JIT at launch")
            method = .trollStore
            enable()
        }
    }

    func enable() {
        guard !JITBootstrap.ready else { return }
        // Asking twice started a second attach, which left a debugger on the process
        // and made every trap stop the app (2026-11-09).
        guard !busy else {
            log("Turn On JIT: JIT is being turned on already; nothing more to do")
            return
        }
        error = nil
        switch resolvedMethod {
        case .automatic:
            assertionFailure("Automatic must resolve to a concrete JIT method")
        case .stikDebug:
            // requestAttach() calls open() on stikjit:// and cannot report failure
            // synchronously: an unclaimed scheme fails open() silently. Wait for
            // CS_DEBUGGED so a missing StikDebug still ends in a clear message.
            if JITBootstrap.requestAttach() {
                awaitStikDebug()
            } else if JITBootstrap.requestTrollStoreAttach() {
                awaitTrollStore()
            } else {
                error = "StikDebug is not installed. Install it, then try again."
                showSetup = true
            }
        case .trollStore:
            if JITBootstrap.requestTrollStoreAttach() {
                awaitTrollStore()
            } else {
                error = "TrollStore could not be opened. Install HaloWin through TrollStore, or choose another method."
                showSetup = true
            }
        case .jailbreak:
            // Nothing to ask: a jailbreak marks an app as debugged as it opens, if it has been told to.
            error = "Turn on Allow JIT in Apps in Dopamine's settings, then open HaloWin again."
            showSetup = true
        }
    }

    /// After TrollStore is asked: it opens HaloWin again and attaches for a moment,
    /// which marks the process as debugged.
    private func awaitTrollStore() {
        busy = true
        status = "Waiting for TrollStore to enable JIT…"
        log("waiting for TrollStore's attach")
        _ = Self.waitForDebugger(timeout: 60) { [weak self] attached in
            guard let self else { return }
            busy = false
            if attached {
                status = "JIT is on."
                error = nil
                attachGeneration += 1
                log("TrollStore enabled JIT")
            } else {
                status = nil
                error = "TrollStore did not enable JIT. In TrollStore's Settings turn on URL Scheme, and make sure HaloWin was "
                      + "installed through TrollStore (or is signed with get-task-allow)."
                log("TrollStore did not enable JIT within a minute")
                showSetup = true
            }
        }
    }

    /// After StikDebug is asked to attach: open() cannot say whether it found
    /// StikDebug, so watch CS_DEBUGGED the same way awaitTrollStore does.
    private func awaitStikDebug() {
        busy = true
        status = "Waiting for StikDebug to attach…"
        log("waiting for StikDebug's attach")
        _ = Self.waitForDebugger(timeout: 60) { [weak self] attached in
            guard let self else { return }
            busy = false
            if attached {
                status = "JIT is on."
                error = nil
                attachGeneration += 1
                log("StikDebug attached")
            } else {
                status = nil
                error = "StikDebug did not attach within a minute. Make sure it's installed and try again."
                log("StikDebug did not attach within a minute")
                showSetup = true
            }
        }
    }

    /// Polls CS_DEBUGGED until it is set or `timeout` passes.
    private static func waitForDebugger(timeout: TimeInterval = 90,
                                        completion: @escaping (Bool) -> Void) -> Timer {
        let deadline = Date().addingTimeInterval(timeout)
        let timer = Timer(timeInterval: 0.5, repeats: true) { timer in
            if JITBootstrap.debuggedFlag {
                timer.invalidate()
                completion(true)
            } else if Date() >= deadline {
                timer.invalidate()
                completion(false)
            }
        }
        RunLoop.main.add(timer, forMode: .common)
        return timer
    }
}
