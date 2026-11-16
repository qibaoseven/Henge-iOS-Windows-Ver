import Foundation

/// One Windows machine on disk: a folder with the firmware, the NVMe image and
/// whatever media is staged for it. Nothing here talks to the engine; that is
/// WinEngine's job.
struct WinMachine: Codable, Identifiable, Hashable {
    var id: UUID = UUID()
    var name: String
    var ramMB: Int = 6144
    var cpus: Int = 4
    var diskGB: Int = 32
    var stage: Int = 0
    var createdAt: Date = Date()
    var lastBootedAt: Date?
    /// Which display hardware the guest gets: 2 auto (ramfb + virtio-gpu),
    /// 0 ramfb only, 1 virtio-gpu only. Stored loosely so old files still load.
    var displayKind: Int = 2
    var panelW: Int = 1440
    var panelH: Int = 900
    /// virgl 3D on the virtio-gpu; needs an app built with VIRGL=1.
    var gpu3d: Bool = false

    var folder: URL { WinStore.root.appendingPathComponent(id.uuidString, isDirectory: true) }
    var diskPath: String { folder.appendingPathComponent("disk.img").path }
    var codePath: String { folder.appendingPathComponent("code.fd").path }
    var varsPath: String { folder.appendingPathComponent("vars.fd").path }
    /// The PE ISO the user brought, and the payload ISO holding install.wim/esd.
    var peISOPath: String { folder.appendingPathComponent("pe.iso").path }
    var payloadISOPath: String { folder.appendingPathComponent("payload.iso").path }
    /// The driver disc that ships inside the app.
    var driverISOPath: String { folder.appendingPathComponent("drivers.iso").path }

    var peISOExists: Bool { FileManager.default.fileExists(atPath: peISOPath) }
    var payloadISOExists: Bool { FileManager.default.fileExists(atPath: payloadISOPath) }
    var diskExists: Bool { FileManager.default.fileExists(atPath: diskPath) }

    /// Older machines.json files predate the display fields, so they are decoded
    /// leniently rather than failing the whole list.
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = try c.decode(UUID.self, forKey: .id)
        name = try c.decode(String.self, forKey: .name)
        ramMB = try c.decodeIfPresent(Int.self, forKey: .ramMB) ?? 6144
        cpus = try c.decodeIfPresent(Int.self, forKey: .cpus) ?? 4
        diskGB = try c.decodeIfPresent(Int.self, forKey: .diskGB) ?? 32
        stage = try c.decodeIfPresent(Int.self, forKey: .stage) ?? 0
        createdAt = try c.decodeIfPresent(Date.self, forKey: .createdAt) ?? Date()
        lastBootedAt = try c.decodeIfPresent(Date.self, forKey: .lastBootedAt)
        displayKind = try c.decodeIfPresent(Int.self, forKey: .displayKind) ?? 2
        panelW = try c.decodeIfPresent(Int.self, forKey: .panelW) ?? 1440
        panelH = try c.decodeIfPresent(Int.self, forKey: .panelH) ?? 900
        gpu3d = try c.decodeIfPresent(Bool.self, forKey: .gpu3d) ?? false
    }

    init(name: String) { self.name = name }

    var diskBytes: Int64 {
        let attrs = try? FileManager.default.attributesOfItem(atPath: diskPath)
        return (attrs?[.size] as? NSNumber)?.int64Value ?? 0
    }

    var diskIsBlank: Bool {
        guard let h = try? FileHandle(forReadingFrom: URL(fileURLWithPath: diskPath)) else { return true }
        defer { try? h.close() }
        let head = (try? h.read(upToCount: 1 << 20)) ?? Data()
        return head.allSatisfy { $0 == 0 }
    }
}

/// The stage names the installer screen shows. Index matches `WinMachine.stage`.
enum WinStage {
    static let titles = ["准备", "PE：分区与部署", "首次开机", "驱动", "完成"]

    /// The one line the install screen keeps pinned at the bottom: what the
    /// user has to do right now, and nothing else.
    static func hint(_ stage: Int) -> String {
        switch stage {
        case 0:
            return "导入 ARM64 的 PE 光盘、装着 install.wim/ESD 的数据光盘，然后点开始安装。"
        case 1:
            return "在 PE 里：① DiskGenius 格式化 32 GB 盘 ② 右键自动建 ESP/MSR ③ WinNTSetup 恢复映像，完成后关机。"
        case 2:
            return "让它自己跑，别动。装完会自动重启，看到桌面就点下一步。"
        case 3:
            return "驱动盘已挂上，正在自动安装显卡和网卡驱动，装完自动重启。"
        default:
            return "装好了。以后从这里启动就是一台完整的 Windows 11。"
        }
    }
}

enum WinStore {
    static var root: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        let dir = base.appendingPathComponent("WindowsVMs", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    private static var indexURL: URL { root.appendingPathComponent("machines.json") }

    static func load() -> [WinMachine] {
        guard let data = try? Data(contentsOf: indexURL),
              let list = try? JSONDecoder().decode([WinMachine].self, from: data) else { return [] }
        return list
    }

    static func save(_ list: [WinMachine]) {
        let enc = JSONEncoder()
        enc.outputFormatting = [.prettyPrinted, .sortedKeys]
        if let data = try? enc.encode(list) { try? data.write(to: indexURL, options: .atomic) }
    }

    /// Creates the folder, a blank NVMe image, and a private copy of the UEFI
    /// NVRAM. The firmware itself is copied from the app bundle, so every machine
    /// starts from the same known-good EDK2 build.
    static func prepare(_ m: WinMachine) throws {
        try FileManager.default.createDirectory(at: m.folder, withIntermediateDirectories: true)
        if !m.diskExists, henge_winvm_make_disk(m.diskPath, UInt64(m.diskGB)) != 0 {
            throw WinError.message("无法创建 \(m.diskGB) GB 的虚拟硬盘（空间不足？）")
        }
        if !FileManager.default.fileExists(atPath: m.codePath) {
            guard let src = Bundle.main.url(forResource: "code", withExtension: "fd") else {
                throw WinError.message("App 里没有 UEFI 固件（code.fd）。这是一个打包问题，请反馈。")
            }
            try FileManager.default.copyItem(at: src, to: URL(fileURLWithPath: m.codePath))
        }
        if !FileManager.default.fileExists(atPath: m.varsPath) {
            if let src = Bundle.main.url(forResource: "vars", withExtension: "fd") {
                try FileManager.default.copyItem(at: src, to: URL(fileURLWithPath: m.varsPath))
            } else {
                FileManager.default.createFile(atPath: m.varsPath, contents: Data(count: 64 << 20))
            }
        }
        // The driver disc is an app asset; copy it next to the machine so the boot
        // code only ever deals with one directory.
        if !FileManager.default.fileExists(atPath: m.driverISOPath),
           let src = Bundle.main.url(forResource: "drivers", withExtension: "iso") {
            try FileManager.default.copyItem(at: src, to: URL(fileURLWithPath: m.driverISOPath))
        }
    }
}

enum WinError: LocalizedError {
    case message(String)
    var errorDescription: String? {
        switch self { case .message(let s): return s }
    }
}
