import SwiftUI
import UniformTypeIdentifiers

/// Page 3: the settings that must exist, and the two files the user owns.
/// Everything else -- firmware, drivers, the ISO builder -- is inside the app,
/// which is the whole point of this design.
struct WinSettingsView: View {
    @Binding var machine: WinMachine
    @Environment(\.dismiss) private var dismiss
    @ObservedObject private var engine = WinEngine.shared

    @State private var importingPE = false
    @State private var importingPayload = false
    @State private var importError: String?

    private let ramChoices = [4096, 6144, 8192, 12288]
    private let cpuChoices = [2, 4, 6, 8]

    var body: some View {
        NavigationStack {
            Form {
                Section("名字") {
                    TextField("Windows 11", text: $machine.name)
                }

                Section("内存（板子就这么大，别贪）") {
                    Picker("内存", selection: $machine.ramMB) {
                        ForEach(ramChoices, id: \.self) { Text("\($0 / 1024) GB").tag($0) }
                    }
                    Picker("CPU", selection: $machine.cpus) {
                        ForEach(cpuChoices, id: \.self) { Text("\($0) 核").tag($0) }
                    }
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
