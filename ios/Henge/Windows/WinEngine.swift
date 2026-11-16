import Combine
import CoreGraphics
import Foundation
import QuartzCore

/// Owns the single Windows machine that can be running, which is the engine's
/// own rule: one board at a time, because the RAM and the JIT region are not
/// shareable.
@MainActor
final class WinEngine: ObservableObject {
    static let shared = WinEngine()

    @Published private(set) var running = false
    @Published private(set) var exitReason = ""
    @Published private(set) var frames: UInt64 = 0
    @Published private(set) var displaySize = CGSize.zero
    @Published var errorText: String?

    private var pollTimer: Timer?
    private var attachedLayer: CAMetalLayer?

    private init() {}

    /// Which discs this stage needs, in tray order. The driver disc rides along
    /// from the very first boot: WinNTSetup reads answer/unattend.xml off it, and
    /// FirstLogonCommands finds the same disc later to install the drivers, so
    /// nothing has to be swapped mid-install.
    private func media(for m: WinMachine) -> (String?, String?, String?) {
        let drivers = FileManager.default.fileExists(atPath: m.driverISOPath) ? m.driverISOPath : nil
        switch m.stage {
        case 0, 1:
            return (m.peISOExists ? m.peISOPath : nil, m.payloadISOExists ? m.payloadISOPath : nil, drivers)
        case 3:
            return (drivers, nil, nil)
        default:
            return (nil, nil, nil)
        }
    }

    func start(_ m: WinMachine) {
        guard !running else { return }
        errorText = nil

        let media = media(for: m)
        if m.stage <= 1 && !m.peISOExists {
            errorText = "还没导入 PE 光盘。到设置页导入 ARM64 的 PE ISO。"
            return
        }
        if m.stage <= 1 && !m.payloadISOExists {
            errorText = "还没导入数据光盘。到设置页导入装着 install.wim 或 ESD 的 ISO。"
            return
        }

        // Which display hardware this machine gets, and how big it should think
        // the panel is -- told to the engine before it builds the board, because
        // a driver asks for a mode long before the UI can react.
        henge_winvm_set_display(Int32(m.displayKind), UInt32(m.panelW), UInt32(m.panelH))
        henge_winvm_set_gpu3d(m.gpu3d ? 1 : 0)

        let ram = UInt64(m.ramMB)
        let cpus = Int32(m.cpus)
        let code = m.codePath, vars = m.varsPath, disk = m.diskPath
        // C wants a plain pointer per optional path; an Optional<String> does not
        // convert on its own, so each disc gets a C string of its own.
        let cd0: UnsafePointer<CChar>? = media.0.flatMap { strdup($0).map { UnsafePointer($0) } }
        let cd1: UnsafePointer<CChar>? = media.1.flatMap { strdup($0).map { UnsafePointer($0) } }
        let cd2: UnsafePointer<CChar>? = media.2.flatMap { strdup($0).map { UnsafePointer($0) } }

        Task.detached(priority: .userInitiated) {
            defer {
                if let p = cd0 { free(UnsafeMutablePointer(mutating: p)) }
                if let p = cd1 { free(UnsafeMutablePointer(mutating: p)) }
                if let p = cd2 { free(UnsafeMutablePointer(mutating: p)) }
            }
            do {
                try WinStore.prepare(m)
            } catch {
                await MainActor.run { self.errorText = error.localizedDescription }
                return
            }
            var err = [CChar](repeating: 0, count: 512)
            let rc = code.withCString { codePtr in
                vars.withCString { varsPtr in
                    disk.withCString { diskPtr in
                        henge_winvm_boot(codePtr, varsPtr, diskPtr, cd0, cd1, cd2, ram, cpus, &err, 512)
                    }
                }
            }
            if rc != 0 {
                let reason = String(cString: err)
                await MainActor.run { self.errorText = reason.isEmpty ? "无法启动这台 Windows。" : reason }
                return
            }
            await MainActor.run { self.didStart(m) }
        }
    }

    private func didStart(_ m: WinMachine) {
        running = true
        exitReason = ""
        if let layer = attachedLayer { henge_winvm_set_display_layer(Unmanaged.passUnretained(layer).toOpaque()) }
        pollTimer?.invalidate()
        pollTimer = Timer.scheduledTimer(withTimeInterval: 0.35, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.poll() }
        }
    }

    private func poll() {
        frames = henge_winvm_display_frames()
        var w: UInt32 = 0, h: UInt32 = 0
        if henge_winvm_display_size(&w, &h) == 0, w > 0, h > 0 {
            let next = CGSize(width: Int(w), height: Int(h))
            if next != displaySize { displaySize = next }
        }
        let state = henge_winvm_state()
        if state == 2 && running {
            running = false
            pollTimer?.invalidate()
            pollTimer = nil
            exitReason = String(cString: henge_winvm_exit_reason())
        }
    }

    /// Attached right before the first frame is expected; the engine keeps the
    /// layer, so it must outlive the view hierarchy swap.
    func attach(_ layer: CAMetalLayer?) {
        attachedLayer = layer
        henge_winvm_set_display_layer(layer.map { Unmanaged.passUnretained($0).toOpaque() })
    }

    func stop() { henge_winvm_stop() }
    func powerButton() { henge_winvm_power_button() }
    func flush() { henge_winvm_flush_disks() }
    func ctrlAltDel() { henge_winvm_ctrl_alt_del() }

    func key(hid usage: UInt8, down: Bool) { henge_winvm_key_hid(usage, down ? 1 : 0) }

    func pointer(_ p: CGPoint, buttons: UInt8 = 0, wheel: Int8 = 0) {
        henge_winvm_tablet(Double(p.x), Double(p.y), buttons, wheel)
    }
}
