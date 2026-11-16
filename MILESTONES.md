# HENGE — 里程碑与现状（Windows 11 ARM64 方向）

> 生成于 2026-11-16，基线提交 `f81479e`（共 14 个提交，remote 已指向 `qibaoseven/Henge-iOS-Windows-Ver`）。
>
> 本文替代 `HANDOVER.md`、`HANDOVER-WINDOWS.md`、`事件处理计划.md` 三份旧文档的**内容**，
> 但那三份按用户要求**保留原样、不再维护**，只作历史参考。

---

## 0. 一分钟结论

**项目**：Henge。iOS 没有任何 hypervisor 能力，所以它自己写 JIT + 软件 MMU + 全套设备模型，
在 iPhone/iPad 上跑 arm64 虚拟机。本分支的方向已经从"跑 macOS"转为**只跑 Windows 11 ARM64**。

**当前代码状态（读代码 + git 核对，非转述旧文档）**：

| 维度 | 实际 |
|---|---|
| Windows 机器模型 | ✅ 已落地：`devices/winvirt.c`（`HvWinVirt`），NVMe×2、HDA、ramfb、GED、xHCI、virtio-net、PCIe 256 条总线 |
| ACPI | ✅ 已落地且比旧文档写的更全：DSDT/FACP/APIC(ITS)/GTDT/MCFG/SPCR/DBG2/IORT/PPTT + SMBIOS 3.0 + `etc/table-loader` 重定位脚本 |
| 真固件冒烟 | ❌ **断了**：跑冒烟那条路（`tools/run-uefi.sh` + `host/main_uefi.c`）还指着已删除的 `virt.c`/`dtb.c`，脚本现在直接找不到文件 |
| 构建 | ❌ 三套都断：`make`（macOS）、`make ios-lib`（iOS）、`run-uefi.sh`；只有 `compat/build-and-test.sh` 跟上了新机器，但它后面仍去跑三个用旧 API 的测试（本轮已改为跳过并标注） |
| iOS 前端 | ❌ 未接线：`ios/Henge/Native/henge-vm.c` 仍 `#include "pvg_lite.h" "vm.h" "install.h" "dns_proxy.h"`，这四个文件在 `f81479e` 全被删了；Swift 层没有任何 `winvm` 引用 |
| 显示呈现 | ❌ **`devices/fb_metal.m` 不存在**。`fb_display.h` 说 `hv_fb_metal_set_layer` / `hv_fb_metal_frames` / `hv_fb_save_png` 由它实现，`host/winvm.c` 也确实调用这三个函数，但树里没有任何定义 → 连 macOS 的 `winvm` 都链接不过 |
| 唯一还算完整的入口 | `host/main_mac.m`（macOS 的 Cocoa/headless runner，调用 `hv_winvm_create`）——但受上一条拖累，目前也链接不通 |

**一句话**：新机器上场了、旧机器拆台了，但裁判、记分牌和看台都没换，**外加投影仪（Metal 呈现器）被拿走了没还**。
所以现在的仓库处在**重构中途**，不是"某里程碑未完成"，而是"整套东西暂时不可构建"。

---

## 1. 方向与硬约束

| 约束 | 说明 |
|---|---|
| 目标 guest | **只 Windows 11 ARM64**。macOS 已被主动放弃，不再作为验收目标；"出 IPA 验证 macOS 回归"是反向建议，不要再提 |
| 4 KB 页 | 故意改造（Windows 强制 4 KB，不可配置）。代价是 TLB 覆盖 256 MB → 64 MB，**这个退化是刻意接受的** |
| 没有 EL2/EL3 | `core/exception.c` 明确 EL2/EL3 不存在 → Windows 的 Hyper-V / WSL2 / VBS 用不了 |
| 没有 AArch32 | 32 位 guest 不可能 |
| PSCI conduit | **只能 HVC**，写 SMC 会静默死 |
| INTx | 已彻底废除，现代 virtio 只用 MSI-X |
| fw_cfg | 已实现 DMA（与早期"无 DMA"的决定相反），并支持可写条目（`etc/ramfb`） |

---

## 2. 里程碑

M1/M2/M3 沿用旧文档的定义；**M4/M5 在旧文档里不存在**，本文按剩余工程缺口补定义（可调）。

### M1 — 真固件冒烟

**目标**：真实 `AAVMF` 固件在自研机器上启动，串口有输出。
**验收**：串口打出 `UEFI firmware (version ...)` 横幅；不要求进系统。

| 项 | 状态 |
|---|---|
| 横幅打印（flash 可执行为 RAM、DTB 被接受、PL011 通路、PSCI hvc 不死） | ✅ 曾达成 |
| 横幅后 `Synchronous Exception at 0x478F497C` | ❌ 未处理（落在 RAM 区，说明固件已把自己复制进 RAM） |
| 冒烟入口仍可用 | ❌ 否，`run-uefi.sh` 指向已删除文件 |

### M2 — Windows 安装器认盘并开始安装

**目标**：Windows 11 ARM64 安装镜像能认出 NVMe 并开始写盘。
**验收**：Setup 界面出现、磁盘列表里有 NVMe、能分区并开始拷贝文件。

| # | 项 | 状态 |
|---|---|---|
| 1 | 4K TLB / virt 平台地基 / DTB / ITS+MSI-X / PCIe 多 bus | ✅ |
| 2 | ACPI 表全集 | ✅（见 §3.3，含旧文档说"缺"的 `_OSC`、完整 `_CRS`、CPU 容器） |
| 3 | 把表交给客户机（fw_cfg `etc/acpi/*` + `etc/table-loader`） | ✅ |
| 4 | RAM 让开表的窗口 | ✅（改为 table-loader 动态分配，不再固定 `0x4800_0000`） |
| 5 | 存储与网卡挂上总线 | ✅ 但**换了类型**：NVMe×2 + virtio-net（virtio-blk 代码仍在，未挂载） |
| 6 | 显示通路 | ✅ ramfb（EDK2 `QemuRamFbDxe` 写 `etc/ramfb`），不是 virtio-gpu |
| 7 | 输入通路 | ✅ xHCI（键盘 + 绝对定位平板），不是 virtio-input |
| 8 | 真实固件跑一遍，确认它读的是 ACPI 不是 DT | ⬜ 待做（且入口现在坏了） |
| 9 | Windows ISO 首次安装冒烟 | ⬜ 待做 |
| 10 | DSDT 深度（`_DSD`、`\_SB.PCI0` 子设备、`_PXM`） | ⚠️ 仍薄，但已不是阻断项 |

### M3 — 装完能开机、能上网、能看见画面

**目标**：安装完成后从硬盘启动进入桌面。
**验收**：进 Windows 桌面、鼠标能动、能打开浏览器上网。

| 项 | 状态 |
|---|---|
| 虚拟磁盘 | ✅ raw + **`.hvsparse` 稀疏镜像**（`host/hvsparse.c`，pack/unpack；`hv_disk_open` 支持 offset） |
| 引导顺序 / BDS | ⬜ 全靠固件默认。NVMe 序列号 `WIN11IOS01` 已刻意对齐 UEFI 的 `Boot0001`，但没有 boot order 管理 |
| RTC 与时钟 | ⚠️ PL031 有秒计数，未与真实时间对齐 |
| 网络后端 | ✅ `host/net_slirp.c` 接 libslirp；缺库时机器仍能建但无 NAT |
| 显示 | ✅ ramfb；Windows 装完还需要 GPU 驱动才有加速（目前是纯 CPU 帧缓冲） |
| 输入 | ✅ xHCI HID（`hv_winvm_key_hid` / `hv_winvm_tablet`） |
| 音频 | ✅ HDA（`devices/hda.c`，966 行）+ virtio-snd 骨架 |
| TPM 2.0 / Secure Boot | ⬜ 没有。Windows 11 装机会查，需注册表绕过或后续补 vTPM |
| SMP 稳定性 | ⚠️ 多核 PSCI `CPU_ON` 已通，Windows 的 IPI/时钟压力未验 |
| 共享目录 | ✅ WebDAV over `http://10.0.2.4/`（`host/share.c`） |

### M4 — 可用（新定义）

**目标**：这台"iPhone 上的 Windows"能当一台机器用，而不是只能开机。

| 项 | 依据/现状 |
|---|---|
| 电源管理 | ACPI GED 已实现（事件注入）；Windows 的 sleep/S4 未验 |
| 设备完整度 | 缺电池/热区、缺 `_PXM`；`BGRT`/`BOOTFB` 没做（启动 logo 装饰性，可跳） |
| 输入体验 | 触摸→绝对平板已通；手势/键盘映射仍是 macOS 时期那套（`main_mac.m` 里有 Mac 键码→Linux 键码表） |
| 存储管理 | 稀疏镜像有，但快照 / 扩容 / 导入 VHDX 没有 |
| 崩溃与恢复 | ⬜ 没有 Windows 蓝屏后的自动重启策略，也没有磁盘一致性保护 |
| 日志与诊断 | `--stats`、`hv_winvm_engine_stats`、PNG 截图钩子已有 |

### M5 — iPhone/iPad 落地（新定义）

**目标**：Windows 在真机上能装、能跑、不掉帧。

| 项 | 现状 |
|---|---|
| iOS 前端接线 | ❌ 完全没有（Swift 层零引用；`henge-vm.c` 引用已删文件） |
| 内存 | ✅ `hv_alloc_no_footprint`（Mach ledger tag，绕过 Jetsam footprint）已写 |
| JIT 权限 | 沿用 macOS 那套（`henge-ios-jit.h` / TXM 分离 RX/RW），Windows 路径未接 |
| 性能 | ⬜ JIT 优化、块缓存、TLB 抖动（4 KB 页下 64 MB 覆盖是第一个要调的地方） |
| 打包分发 | ⬜ 无 Windows 版 IPA；CI 仍只有 macOS 那条（`build-henge.yml`，依赖 `Leviidev/Husk`） |

---

## 3. 代码现状（逐目录）

### 3.1 目录职责

| 目录 | 内容 |
|---|---|
| `core/` | JIT、软件 MMU、sysreg、异常、machine。**与平台无关，也是唯一在非 Apple 主机上能编译的部分**（`translate.c` 有 aarch64 内联汇编，x86 主机编不了） |
| `devices/` | 机器与设备模型。Windows 方向：`winvirt` `acpi` `fdt` `pflash` `fw_cfg` `gicv3`（含 ITS）`pcie` `nvme` `hda` `xhci` `fb_display` `virtio*` `pl011` `pl031` `pl061` `timer`。Apple 方向残留：`pvg_mac.m` `pvg_metal.m` |
| `host/` | 入口与宿主后端：`winvm.c`（引擎生命周期、显示/输入/截图）、`main_mac.m`（macOS 入口）、`net_slirp.c`、`share.c`、`audio_mac.c`、`disk.c`、`hvsparse.c`、`jit_region.c`、`prof_ios.c`、`main_uefi.c`（**已孤儿**） |
| `ios/` | SwiftUI 前端 + `Native/henge-vm.c` 桥接层（**当前编不过**） |
| `compat/` | 非 Apple 主机的移植垫片 + `build-and-test.sh` + `acpi-check.py` |
| `tests/` | `test_mmu` `test_virt` `test_pcie` `test_dtb` `test_acpi` 等 |

### 3.2 Windows 机器的内存图（`devices/winvirt.h`）

```
0x0000_0000  pflash0  code  64 MB  只读 RAM（UEFI 入口，PC 复位至此）
0x0400_0000  pflash1  vars  64 MB  可写 RAM（UEFI 变量）
0x0800_0000  GIC distributor
0x0808_0000  GIC ITS            0x0809_0040 = GITS_TRANSLATER
0x080A_0000  GIC redistributor
0x0900_0000  PL011 UART         IRQ 33
0x0901_0000  PL031 RTC          IRQ 34
0x0902_0000  fw_cfg（含 DMA）
0x0903_0000  PL061 GPIO         IRQ 39
0x0908_0000  ACPI GED          IRQ 41
0x1000_0000  PCIe MMIO32（0x2EFF_0000）
0x3EFF_0000  PCIe PIO（64 KB）
0x4000_0000  客户机 RAM 起点，DTB 也写在这里（x0 = 0x4000_0000）
0x40_1000_0000  PCIe ECAM 256 MB → 256 条总线（旧 virt 只有 16 条）
0x80_0000_0000  PCIe MMIO64 2 GB
```

复位：`pc = 0`，`x0 = 0x4000_0000`，从核 `off = 1` 等 PSCI `CPU_ON`。
MSI：INTID 80..143（64 个向量）。

PCI 槽位（`winvirt.c` 注释，刻意按 Windows Setup 的期望排）：
`0` host bridge `1b36:0008` · `1` virtio-net `1af4:1000` · `2` NVMe0 `1b36:0010`（序列号 `WIN11IOS01`，对齐 `Boot0001`）· `3` xHCI `1b36:000d` · `4` NVMe1（可选安装盘）· `5` virtio-snd

### 3.3 ACPI（`devices/acpi.c`，33 KB）

生成：DSDT、FACP、APIC(MADT)、PPTT、GTDT、MCFG、SPCR、**DBG2**、IORT、XSDT、RSDP，外加 **SMBIOS 3.0**，
OEM ID 统一 `WINVIRT `。表不再钉死物理地址，而是打包成 `etc/acpi/tables` + `etc/table-loader`
（`ADD_POINTER` / `ADD_CHECKSUM`），由固件自己重定位——**这与旧文档"禁止 table-loader"的决定相反**。

DSDT 内容（比旧文档描述的厚很多）：

| 节点 | `_HID` | 内容 |
|---|---|---|
| `C000..C0xx` | `ACPI0007` | CPU 容器，每 CPU 一个，`_UID` |
| `COM0` | `ARMH0011` | PL011 `0x0900_0000`，IRQ 33 |
| `FWCF` | `QEMU0002` | fw_cfg，`_STA 0x0b`、`_CCA 1` |
| `L000..L003` | `PNP0C0F` | PCI 中断链接，GSI 35..38 |
| `\_SB.PCI0` | `PNP0A08` | **完整 `_CRS`**：PIO + MMIO32 + MMIO64 + ECAM；含 `_OSC`、`_DSM`、ECAM 保留 |
| 电源按钮 | `PNP0C0C` | — |
| GED | `ACPI0013` | ACPI 事件设备 |

旧文档说"缺 `_OSC`、`_CRS` 只有 ECAM、没有 CPU 节点"——**这三条在代码里都已补上**，文档滞后。

---

## 4. 重构留下的孤儿（`f81479e` 的真实代价）

`f81479e` 把 `virt.c/.h`、`dtb.c/.h`、`its.c/.h` 以及整个 vmapple 家族删了，换成 `winvirt.c/.h` + `fdt.c`。
**替换只做了一半**，下面这些调用方还指着旧 API：

| 文件 | 引用的已删符号 | 后果 |
|---|---|---|
| `host/main_uefi.c` | `virt.h`、`HvVirt`、`hv_virt_create`、`VIRT_GUEST_RAM`、`VIRT_GIC_DIST` | 编不过 |
| `tools/run-uefi.sh` | 显式编译 `devices/virt.c`、`devices/dtb.c`、`devices/its.c` | 脚本直接失败（文件不存在） |
| `tests/test_virt.c` | `virt.h`、`HvVirt`、`hv_virt_create` | 编不过 |
| `tests/test_pcie.c` | 同上 | 编不过 |
| `tests/test_dtb.c` | `dtb.h`、`virt.h`、`hv_dtb_build` | 编不过 |
| `tests/test_devices.c` | `hv_virtio_balloon_create`、`VMAPPLE_MEM` | 编不过 |
| `Makefile` 的 `DEV` | `pvg_lite.o cfg.o bdif.o aes.o vmapple.o virtio_usb.o virtio_balloon.o metallib.o`（全部已删），且**没有** `winvirt nvme hda fb_display fdt pflash fw_cfg` | macOS `make` 断 |
| `Makefile` 的 `hengevm` 目标 | `host/main.c`、`host/vm.c`、`host/usb_host.c`、`dns_proxy.c`、`pvg_mac.o` | 断 |
| `Makefile` 的 `IOS_SRC` | `host/vm.c`、`host/dns_proxy.c`、`host/usb_host.c`；且**没把 winvirt/winvm 加进 iOS 库** | `make ios-lib` 断 |
| `ios/Henge/Native/henge-vm.c` | `pvg_lite.h`、`vm.h`、`install.h`、`dns_proxy.h` | iOS 桥接层断 |

**结论**：C 侧新机器是完整的；断的全是"入口与测试"这一圈。

### 4.1 本轮已经迁移的（2026-11-16）

| 文件 | 做了什么 | 状态 |
|---|---|---|
| `host/main_uefi.c` | 重写为 `HvWinVirt` / `hv_winvirt_create`；打印新内存图、fw_cfg 里的 ACPI 文件、PCIe ECAM 总线数、NVMe/NIC/xHCI/HDA；`--save-vars` 变为兼容空操作（vars flash 写穿到文件）；磁盘参数改走 `hv_disk_open` + NVMe | ✅ 语法检查通过（x86 上只能到这一层） |
| `tools/run-uefi.sh` | 编译清单从 `virt/dtb/its` 换成 `winvirt/fdt/fw_cfg/pflash/nvme/hda/xhci/fb_display/virtio_snd…`；默认 RAM 512 → 2048 MiB | ✅ |
| `Makefile` | `DEV` 拆成 `WINDEV`（现存的 Windows 设备）+ Apple GPU 两个对象；`all` 改为 `$(TESTS) hv_uefi hvsparse`；新增 `hv_uefi` 目标；`IOS_SRC` 去掉 `vm.c/dns_proxy.c/usb_host.c`，加入 `winvm.c`；`hengevm` / `hengevm-ref` 标为历史目标（源文件已删） | ✅ Makefile 语法可解析 |
| `compat/build-and-test.sh` | `test_virt` / `test_pcie` / `test_dtb` 在没有 `devices/virt.h` 时**明确跳过并打印原因**，不再假装失败或静默 | ✅ |

**仍未迁移**（按难度排）：

1. `tests/test_virt.c`、`tests/test_pcie.c`、`tests/test_dtb.c` → 要按新 API 重写断言（`pflash` 参数从 6 个变 4 个、`hv_fw_cfg_*` 结构与常量全变、`hv_dtb_build` → `hv_fdt_build_virt`）。**这不是改名，是重写**，必须在能编译的 arm64 主机上做完并核对断言数。
2. `devices/fb_metal.m` → 不存在，要补（Metal 呈现 + PNG 截图），否则 `winvm` 连不上。
3. `ios/Henge/Native/henge-vm.c` → 改挂 `winvm`，这是 M5 的活，不属于迁移。

---

## 5. macOS 是怎么装的（现状复盘 + 能学到什么）

### 5.1 现在的流程（`ios/Henge/Installer.swift` + `Installer` 状态机）

1. **首屏**：`HomeView` 没有机器时显示 `firstMac` 卡片 → "Install macOS" → 切到 Install 页。
2. **选版本**：`InstallView` 列出 6 个 Apple 官方 **VirtualMac2,1 恢复镜像**（Monterey 12.6 / Ventura 13.6 / Sonoma 14.6.1 / **Sequoia 15.6.1 = 推荐** / Tahoe 26.6.2 / 27.2），
   同时显示内存滑杆、所需空间、JIT 是否就绪、芯片是否太老。
3. **`Installer.start()`** 五阶段：`checking → downloading → preparing → installing → done/failed`
   - `checkSpace`：按"下载剩余 + 24 GB 机器"预检空间；不询问就直接失败并给出人话。
   - **先取 Boot ROM 再下载**（`BootROM.file()`），让"ROM 不可用"这类问题立刻暴露，而不是等 15 GB 下载完。
   - `download`：`RangedDownload` 断点续传写 `.part`，失败重试至多 20 次、每次退避 3 s；进度回调到 UI。
   - `prepare`：随机 ECID（模拟 `VZMacMachineIdentifier`）、建 64 GB **稀疏** disk（`truncate`）、`henge_make_aux` 造 aux 存储、写 `machine.json`。
   - `install`：`henge_vm_install(...)` → 起一台 vmapple 机器，**它的 Boot ROM 进 DFU 模式，恢复通过虚拟 USB 口跑**（`hv_install_start` → 内置 idevicerestore），
     和 Virtualization.framework 在 Mac 上装系统是同一个路子。恢复期内存被夹到 2048–4096 MB，`HV_WRITE_CACHE=0` 让 15 GB 直写存储。
   - 进度轮询 `henge_install_step/progress`，并把 step 翻成人话：`0 找机器 / 1 启动安装器 / 2 写 macOS / 3 校验 / 4 装固件 / 6-7 装系统文件`。
   - 完成：写 `installed` 标记 → 加入机器列表 → 删临时目录；失败：若机器没在跑就**删掉半成品**，下载保留。
   - 全程 `isIdleTimerDisabled` + 后台任务宽限（iOS 约 30 s），并在 UI 上明确"别切走"。

### 5.2 值得直接抄的五件事

| 学到的 | 搬到 Windows |
|---|---|
| **不自己写安装器**，走 guest 自己的官方恢复流程 | Windows 也一样：Setup 由固件 BDS 启动，我们只负责 UEFI + 磁盘 + 画面 + 输入，别自研安装逻辑 |
| 长下载前先做"便宜的前置检查"（ROM、空间、JIT） | 先验固件两片 flash、`hvsparse` 镜像可写、ramfb 通路，再让用户下 5–7 GB ISO |
| 断点续传 + 分段重试 + 明确"从 X 继续" | ISO 下载同理（`RangedDownload` 可直接复用） |
| 进度必须**分阶段 + 人话映射** | Windows Setup 没有进度回调，只能靠：串口输出、画面的阶段（选语言→分区→复制→首次重启）、目标盘写入量 |
| 失败要能恢复现场（保留下载、清半成品、给"重开 App 再试"的明确指示） | 同上，另外 Windows 半装盘要能识别并重来 |

### 5.3 Windows 侧的差异（这才是"大改"的部分）

1. **没有 DFU/恢复模式**。macOS 那条路靠"客户机自己进 DFU + 宿主用 USB 灌恢复镜像"；
   Windows 是 UEFI → `bootmgfw.efi` → Setup。**所以安装介质必须是一个 UEFI 能读的设备**，两条可选：
   - **A（推荐）**：把 ISO 内容展开到第二块 NVMe（`disk1`，`winvm.h` 已支持只读挂载），让 BDS 从它启动 —— 不需要文件系统代码。
   - **B**：让固件直接读 ISO9660/UDF —— 要写一个 UEFI 可用的块级驱动，成本高、收益低。
2. **必须先把画面弄出来**。macOS 靠 PVGPU 的 Metal 出口；Windows 这边只有 ramfb（`fb_display.c`，
   由 EDK2 的 `QemuRamFbDxe` 通过 `etc/ramfb` 配置）。装机能用，但没有加速；要加速得补 virtio-gpu。
3. **Windows 11 的硬门槛**：TPM 2.0 + Secure Boot + 4 GB RAM + UEFI + 64 GB 盘。
   当前机器一条都没有 TPM；要么在 `boot.wim` 里预置 `LabConfig` 注册表，要么后续补 vTPM（软件 TPM 即可）。
4. **键鼠语义不同**。macOS 那条是"屏幕即 Magic Trackpad 2"（两指滚动、三指 Mission Control）；
   Windows 需要的是绝对定位平板 + 左右键 + 滚轮，`hv_winvm_tablet(x,y,buttons,wheel)` 已经按这个语义写好了，
   但 UI 层还是 macOS 的手势模型，需要换一套（这是 M5 的活）。
5. **进度条是奢侈品**。Setup 阶段黑屏很正常，UI 该给的是"现在在做什么 + 串口最后一行"，而不是百分比。

---

## 6. 下一步（按优先级）

**P0 — 让仓库重新能构建（否则一切验收都是空的）**

1. `tools/run-uefi.sh` + `host/main_uefi.c` 改挂 `HvWinVirt` / `hv_winvirt_create`（顺带恢复 M1 冒烟的入口）。
2. `Makefile`：`DEV` 换成现有对象；`hengevm` / `hengevm-ref` 目标按 `main_mac.m` + `winvm.c` 重写；`ios-lib` 的 `IOS_SRC` 去掉已删文件。
3. `tests/test_virt.c`、`test_pcie.c` 改用 `hv_winvirt_create`；`test_dtb.c` 改成 `test_fdt.c`（`hv_fdt_build_virt`）。
4. `ios/Henge/Native/henge-vm.c`：要么改挂 `winvm`，要么先隔离出来不参与 iOS 构建（否则 iOS 库永远编不过）。

**P1 — 打通 M2 的真实验收**

5. 真实 AAVMF 跑一遍，确认它走 ACPI 分支（`HV_SERIAL_STDERR=1` 抓串口）；这就是 M1 那条 `0x478F497C` 异常要继续查的地方。
6. Windows 11 ARM64 ISO 首次安装冒烟，先看"Setup 是否列出 NVMe"。
7. 缺 TPM 的绕过方案先定下来（改 `boot.wim` 还是补 vTPM）。

**P2 — RET 快速路径（遗留）**

8. `HV_RAS=1` 时 AAVMF 在 MMU 初始化里死循环（`0x520C → 0x5274 → 0x5184 …`）。
   默认已关闭（`core/machine.c:35` 用 `HV_RAS=1` 才开回），**代码里没有任何 `ret_debug` 校验**——
   旧《事件处理计划》的 3.2 节（加 x30 高位校验、带回退）**从未实现**。
   要做的仍是：不改 `HvCpu` 布局，先加验证抓现场，再决定开/关。

---

## 7. 踩坑清单（仍然有效，别再犯）

**GICv3 ITS**
1. `GITS_CBASER` valid 在 bit63、size 在 `[7:0]`（pages−1）。写 bit0=valid 会读成"1 页、地址 0"，ITS 一条命令都不跑。
2. 命令的 DeviceID 在 **word0 高 32 位**；取低 32 位会让 MAPD 建错设备、MAPTI 找不到。
3. EventID 是 8 位、在 `[23:16]`；声明成 16 位会与 opcode `[31:24]` 重叠。
4. 寄存器解码按 **4 字节**粒度（`off & 0xfff`），不是按页。

**PCIe / MSI-X**
5. ECAM 每 1 MB 装一条总线（32 dev × 8 fn × 4 KB）。`bus-range` 写 0–255 会让 OS 去读不存在的地址。
6. 能力链追加必须记住 `cap_next` 并让前一节点指向新位置，否则 OS 看到的是"没有 MSI-X"。
7. MSI-X 表的 data/ctrl 是 32 位字段，内部偏移用 `(w-8)*8` / `(w-12)*8`，不能用 `(w&7)*8`。

**平台 / 工具链**
8. `HvPciDev` 加字段必须追加到结构体末尾，插在 `wmask` 后会把 `config[]` 偏移搞乱。
9. **MMIO 不能当代码**：`hv_mmu_fetch` 只从 `m->ram[]` 取指，pflash 必须是 RAM。
10. 短 `fread()` 会把缓冲区剩余清零（musl），加载 flash 要用 `read(2)` 循环，否则 erased 的 `0xff` 变 `0x00`。

**测试写法**
11. MMIO read 返回 `uint64`，断言要显式 `(uint32_t)` 截断。
12. 崩溃时 `checks` 计数会归零（BSS 被踩）——把计数器放堆上，才能区分"崩溃"和"断言失败"。
13. `cpus[0].pc_state` 只有 `hv_virt_reset_cpu` / `hv_winvirt_reset_cpu` 之后才有效。

---

## 8. 构建与测试矩阵

| 命令 | 平台 | 现状 |
|---|---|---|
| `sh compat/build-and-test.sh` | Linux / aarch64（Alpine、iSH） | 编译新机器；测试跑 `test_mmu / test_virt / test_pcie / test_dtb / test_acpi` + `acpi-check.py`(iasl)，后三个已过时 |
| `make` | macOS | 断（对象表含已删文件） |
| `make ios-lib` | macOS + iOS SDK | 断（`IOS_SRC` 含已删文件，且没加 `winvirt/winvm`） |
| `sh tools/run-uefi.sh` | 任意 + JIT | 断（编译已删的 `virt.c/dtb.c/its.c`） |
| `hv_uefi`（`host/main_uefi.c`） | 任意 + JIT | 编不过（旧 API） |

**注意**：本机是 x86_64，`core/translate.c` 含 aarch64 内联汇编（`mrs dczid_el0` 等），
**x86 上根本编不了**，也不要去"模拟 ARM 跑构建"，非常慢。所有验证必须在 macOS 或 aarch64 Linux 主机上做。

可信度备忘（旧文档的经验）：`test_mmu` 是纯 C，可信；`test_mem` 在非 Apple 主机上的 FAIL 与代码无关；
依赖 `tests/*.S`（Mach-O `@PAGE`/`@PAGEOFF` 语法）和 Apple 框架的测试只能在 Mac 上跑。

---

## 9. 协作约定（沿用）

- **改完就 commit，不问**；但**绝不 push**，除非明确说。
- 不要空谈方案，直接改代码；能精简则精简。
- 不要重定向输出，直接打到命令行；不要 `head`/`tail` 截断实时日志。
- 测试断言**不许放宽**来"通过"回归。
- 遇到不确定的，宁可问一句，也不要白跑一次 CI。
- 旧的 `HANDOVER.md` / `HANDOVER-WINDOWS.md` / `事件处理计划.md` 保留但不再更新，内容以本文为准。
