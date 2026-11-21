import SwiftUI

/// The list of Windows machines, persisted next to their disk images.
@MainActor
final class WinLibrary: ObservableObject {
    static let shared = WinLibrary()
    @Published var machines: [WinMachine] = WinStore.load()

    func save() { WinStore.save(machines) }

    func add(_ m: WinMachine) {
        machines.append(m)
        save()
    }

    func update(_ m: WinMachine) {
        if let i = machines.firstIndex(where: { $0.id == m.id }) {
            machines[i] = m
            save()
        }
    }

    func delete(_ m: WinMachine) {
        machines.removeAll { $0.id == m.id }
        save()
        try? FileManager.default.removeItem(at: m.folder)
    }
}

/// Page 1: the list, unchanged in spirit from the mac machines screen -- one
/// row per machine, a start button, and nothing that needs explaining.
struct WinHomeView: View {
    @ObservedObject private var library = WinLibrary.shared
    @ObservedObject private var engine = WinEngine.shared
    @State private var creating = false
    @State private var newName = ""
    @State private var newDiskGB = 32
    @State private var newRAM = 6144

    var body: some View {
        List {
            if library.machines.isEmpty {
                ContentUnavailableView {
                    Label("还没有 Windows", systemImage: "desktopcomputer")
                } description: {
                    Text("新建一台，装一次。装完以后每次点一下就是一台完整的 Windows 11。")
                } actions: {
                    Button("新建 Windows 虚拟机") { creating = true }
                }
                .listRowBackground(Color.clear)
            }

            ForEach($library.machines) { $m in
                NavigationLink {
                    WinMachineRoot(machine: $m)
                } label: {
                    WinRow(machine: m, running: engine.running)
                }
                .swipeActions(edge: .trailing, allowsFullSwipe: false) {
                    Button(role: .destructive) {
                        library.delete(m)
                    } label: { Label("删除", systemImage: "trash") }
                }
            }
        }
        .navigationTitle("Windows")
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                Button { creating = true } label: { Image(systemName: "plus") }
            }
        }
        .sheet(isPresented: $creating) { createSheet }
    }

    private var createSheet: some View {
        NavigationStack {
            Form {
                Section("名字") {
                    TextField("Windows 11", text: $newName)
                }
                Section("虚拟硬盘") {
                    Picker("容量", selection: $newDiskGB) {
                        ForEach([32, 48, 64, 96], id: \.self) { Text("\($0) GB").tag($0) }
                    }
                    Text("Windows 11 最小要求 64 GB 才舒服；这个盘是稀疏文件，先不占满。")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }
                Section("内存") {
                    /* The same control as the settings page: a full Windows 11
                       wants 4-6 GB, a Tiny11 image is happy at 256 MB. */
                    WinMemoryControl(mb: $newRAM)
                }
            }
            .navigationTitle("新建 Windows 虚拟机")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("取消") { creating = false }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("创建") {
                        var m = WinMachine(name: newName.isEmpty ? "Windows 11" : newName)
                        m.diskGB = newDiskGB
                        m.ramMB = newRAM
                        library.add(m)
                        creating = false
                        newName = ""
                    }
                }
            }
        }
    }
}

private struct WinRow: View {
    let machine: WinMachine
    let running: Bool

    var body: some View {
        HStack(spacing: 14) {
            ZStack {
                RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .fill(machine.stage >= 4 ? Color.blue.opacity(0.16) : Color.orange.opacity(0.16))
                Image(systemName: machine.stage >= 4 ? "windows" : "wrench.and.screwdriver")
                    .font(.system(size: 20, weight: .semibold))
                    .foregroundStyle(machine.stage >= 4 ? Color.blue : Color.orange)
            }
            .frame(width: 44, height: 44)

            VStack(alignment: .leading, spacing: 3) {
                Text(machine.name).font(.headline)
                Text("\(machine.stage >= 4 ? "已安装" : WinStage.titles[min(machine.stage, 4)]) · \(machine.diskGB) GB · \(machine.ramMB / 1024) GB 内存")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Spacer()
            if running {
                Label("运行中", systemImage: "circle.fill")
                    .font(.caption)
                    .labelStyle(.titleAndIcon)
                    .foregroundStyle(.green)
            }
        }
        .padding(.vertical, 4)
    }
}

/// The machine's own page: install while it is being installed, the desktop
/// once it is done. Both are the same screen -- a display and a control strip.
struct WinMachineRoot: View {
    @Binding var machine: WinMachine

    var body: some View {
        WinInstallView(machine: $machine)
    }
}
