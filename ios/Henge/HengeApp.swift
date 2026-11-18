import SwiftUI
import AVFAudio

@main
struct HengeApp: App {
    /// Answers for external display scenes before iOS 27 (LegacyExternalDisplay.swift).
    @UIApplicationDelegateAdaptor(HengeAppDelegate.self) private var appDelegate
    @Environment(\.scenePhase) private var scenePhase

    init() {
        // Order matters: HengeLog takes over stderr, so the engine's and the JIT
        // allocator's first lines land in Documents/henge.log too.
        HengeLog.start()
        HengeLog.log("boot", "address space: \(henge_largest_reservation_gb()) GB can be reserved in one piece")
        EngineTuning.apply()
        HengeLog.logFootprint("app-launch")
        JITBootstrap.noteLaunchState()
        HengeLog.log("app", "increased memory limit: \(Entitlements.increasedMemory ? "yes" : "no"); extended addressing: \(Entitlements.extendedAddressing ? "yes" : "no"); limit \(Entitlements.memoryLimitAtLaunch >> 20) MB")
        // Without the guard, a brk no debugger answers kills the app instead of failing.
        JITBootstrap.installTrapGuard()
        try? AVAudioSession.sharedInstance().setCategory(.playback, options: [.mixWithOthers])
        Machines.shared.collectImports()
        // The display question: does this iOS have Apple's paravirtualized graphics at all?
        for path in ["/System/Library/Frameworks/ParavirtualizedGraphics.framework/ParavirtualizedGraphics",
                     "/System/Library/PrivateFrameworks/ParavirtualizedGraphics.framework/ParavirtualizedGraphics"] {
            HengeLog.log("app", "\(path): \(dlopen(path, RTLD_LAZY) != nil ? "present" : "absent")")
        }
    }

    var body: some Scene {
        WindowGroup {
            RootView()
                .onOpenURL { url in HengeLog.log("app", "opened with \(url.absoluteString)") } // henge:// back from LocalDevVPN
        }
    }
}

struct RootView: View {
    // Windows only: the macOS machines screen belonged to the guest bridge that
    // went away with the Windows direction. The three Windows pages already have
    // their own list/install/settings flow, so the app opens straight onto it.
    @ObservedObject private var jit = JITCoordinator.shared
    @Environment(\.scenePhase) private var scenePhase

    var body: some View {
        NavigationStack {
            WinHomeView()
        }
        .preferredColorScheme(.dark)
        .sheet(isPresented: $jit.showSetup) { JITSetupFlow() }
        .task(id: scenePhase) {
            guard scenePhase == .active else { return }
            jit.enableAtLaunchIfAsked()
            // StikDebug can launch Henge before it attaches. A task also runs
            // on the first appearance, when there may be no phase change.
            for _ in 0..<100 {
                guard !Task.isCancelled else { return }
                if JITBootstrap.prewarmed || JITBootstrap.isLive {
                    jit.objectWillChange.send()
                    return
                }
                if JITBootstrap.debuggedFlag || JITBootstrap.canGrantOwnJIT {
                    JITBootstrap.prewarm()
                    // Readiness lives outside the observable coordinator.
                    jit.objectWillChange.send()
                    return
                }
                do { try await Task.sleep(nanoseconds: 100_000_000) }
                catch { return } // going into the background cancels this wait
            }
        }
        .onChange(of: jit.attachGeneration) { _ in JITBootstrap.prewarm() } // the built-in helper attached
    }
}

extension View {
    /// Husk's screens are written against an iOS 15 shim; Henge needs iOS 17, so this is the real thing.
    func hengeDestination<D: Hashable, C: View>(for data: D.Type, @ViewBuilder destination: @escaping (D) -> C) -> some View {
        navigationDestination(for: data, destination: destination)
    }
}
