import SwiftUI
import UniformTypeIdentifiers

/// Page 3: the settings that must exist, and the two files the user owns.
/// Everything else -- firmware, drivers, the ISO builder -- is inside the app,
/// which is the whole point of this design.
struct WinSettingsView: View {
    @Binding var machine: WinMachine
    @Environment(\.dismiss) private var dismiss
    @ObservedObject private var engine = WinEngine.shared
    @ObservedObject private var jit = JITCoordinator.shared

    @State private var importingPE = false
    @State private var importingPayload = false
    @State private var importError: String?

    private let ramChoices = [4096, 6144, 8192, 12288]
    private let panelChoices: [(Int, Int)] = [(1024, 768), (1280, 800), (1440, 900), (1920, 1200)]
    private var panelBinding: Binding<String> {
        Binding(
            get: { "\(machine.panelW)x\(machine.panelH)" },
            set: { tag in
                let parts = tag.split(separator: "x")
                if parts.count == 2, let w = Int(parts[0]), let h = Int(parts[1]) {
                    machine.panelW = w
                    machine.panelH = h
                }
            }
        )
    }
    var body: some View {
        NavigationStack {
            Form {
                Section("JIT") {
                    LabeledContent("状态", value: JITBootstrap.ready ? "就绠" : "未就绠")
                    Button("设置 JIT…") { JITCoordinator.shared.showSetup = true }
                }

                Section("名字") {
                    TextField("Windows 11", text: $machine.name)
                }

                Section("内存（板子就这么大，别贪）") {
                    Picker("内存", selection: $machine.ramMB) {
                        ForEach(ramChoices, id: \.self) { Text("\($0 / 1024) GB").tag($0) }
                    }
                    /* A text field, not a picker: 1 core is the useful value here.
                       A single vCPU is how you tell "the guest is waiting for
                       another CPU that never runs" from "the guest is stuck in
                       its own code", and the firmware runs fine on one. */
                    LabeledContent("CPU") {
                        TextField("1–8", value: $machine.cpus, format: .number)
                            .keyboardType(.numberPad)
                            .multilineTextAlignment(.trailing)
                            .onChange(of: machine.cpus) { value in
                                machine.cpus = min(max(value, 1), 8)
                            }
                    }
                }
                .disabled(engine.running)

                Section {
                    Picker("显示设备", selection: $machine.displayKind) {
                        Text("自动（ramfb + virtio-gpu）").tag(2)
                        Text("仅 virtio-gpu").tag(1)
                        Text("仅 ramfb").tag(0)
                    }
                    Toggle("3D 加速（virgl）", isOn: $machine.gpu3d)
                        .disabled(machine.displayKind == 0)
                    Picker("面板分辨率", selection: panelBinding) {
                        ForEach(panelChoices.indices, id: \.self) { i in
                            Text("\(panelChoices[i].0) × \(panelChoices[i].1)")
                                .tag("\(panelChoices[i].0)x\(panelChoices[i].1)")
                        }
                    }
                } header: {
                    Text("显示")
                } footer: {
                    Text("Windows 只认 virtio-gpu（1af4:1050，viogpudo 驱动）。ramfb 只有固件和 Linux 认，选“仅 ramfb”时 Windows 装完会没有显示驱动。3D 需要 App 以 VIRGL=1 构建，否则自动退回 2D。")
                }
                .disabled(engine.running)

                Section("虚拟硬盘") {
                    LabeledContent("容量", value: "\(machine.diskGB) GB")
                    LabeledContent("占用", value: ByteCountFormatter.string(fromByteCount: machine.diskBytes, countStyle: .file))
                    LabeledContent("状态", value: machine.diskIsBlank ? "空白" : "已有分区")
                }

                Section {
                    mediaRow(title: "PE 光盘（ARM64）",
                             detail: machine.peISOExists ? "已导入" : "未导入",
                             ok: machine.peISOExists) { importingPE = true }
                    mediaRow(title: "数据光盘（install.wim / ESD）",
                             detail: machine.payloadISOExists ? "已导入" : "未导入",
                             ok: machine.payloadISOExists) { importingPayload = true }
                } header: {
                    Text("这两个文件由你提供")
                } footer: {
                    Text("PE 必须是 ARM64 的，x64 的 PE 在这台机器上起不来。数据光盘就是装着 install.wim 或 install.esd 的那张 ISO。")
                }

                Section("内置") {
                    LabeledContent("UEFI 固件", value: FileManager.default.fileExists(atPath: machine.codePath) ? "就位" : "缺失")
                    LabeledContent("驱动光盘", value: FileManager.default.fileExists(atPath: machine.driverISOPath) ? "就位" : "缺失")
                    LabeledContent("阶段", value: "\(min(machine.stage, 4) + 1)/5 · \(WinStage.titles[min(machine.stage, 4)])")
                }

                Section {
                    Button(role: .destructive) {
                        WinLibrary.shared.delete(machine)
                        dismiss()
                    } label: {
                        Label("删除这台虚拟机", systemImage: "trash")
                    }
                    .disabled(engine.running)
                }
            }
            .navigationTitle("设置")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("完成") {
                        WinLibrary.shared.update(machine)
                        dismiss()
                    }
                }
            }
            .fileImporter(isPresented: $importingPE, allowedContentTypes: [.data], allowsMultipleSelection: false) { result in
                importMedia(result, to: machine.peISOPath)
            }
            .fileImporter(isPresented: $importingPayload, allowedContentTypes: [.data], allowsMultipleSelection: false) { result in
                importMedia(result, to: machine.payloadISOPath)
            }
            .alert("导入失败", isPresented: Binding(get: { importError != nil }, set: { if !$0 { importError = nil } })) {
                Button("好") { importError = nil }
            } message: {
                Text(importError ?? "")
            }
            .sheet(isPresented: $jit.showSetup) { JITSetupFlow() }
        }
    }

    private func mediaRow(title: String, detail: String, ok: Bool, action: @escaping () -> Void) -> some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                Text(title)
                Text(detail).font(.caption).foregroundStyle(ok ? .green : .secondary)
            }
            Spacer()
            Button(ok ? "替换" : "导入", action: action)
                .buttonStyle(.bordered)
        }
    }

    private func importMedia(_ result: Result<[URL], Error>, to destination: String) {
        do {
            guard let url = try result.get().first else { return }
            let scoped = url.startAccessingSecurityScopedResource()
            defer { if scoped { url.stopAccessingSecurityScopedResource() } }
            try FileManager.default.createDirectory(at: machine.folder, withIntermediateDirectories: true)
            if FileManager.default.fileExists(atPath: destination) {
                try FileManager.default.removeItem(atPath: destination)
            }
            try FileManager.default.copyItem(at: url, to: URL(fileURLWithPath: destination))
            WinLibrary.shared.update(machine)
        } catch {
            importError = error.localizedDescription
        }
    }
}
