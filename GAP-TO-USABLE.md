# 从当前代码到"能装能用"的完整缺口清单

> 目标定义：**编出来的 App，用户只有三个页面**（首页 → 安装页 → 设置页 + 会话页），
> 点一次就能把 Windows 11 ARM64 装进虚拟机、装完能开机进桌面、触屏和键盘能用。
> 基线：`41d642e` 之后的 `ad1313e`（工作树干净，全部已 commit，未 push）。

---

## 阶段 0：编译环境（唯一的硬阻塞）

| 缺口 | 现状 | 解法 |
|---|---|---|
| 没有 macOS / Xcode | `Makefile` 的 `ios-lib` 用 `xcrun --sdk iphoneos`，只有 Mac 能跑；本机是 x86 Linux，而 `core/translate.c` 有 aarch64 内联汇编（`mrs dczid_el0`），**x86 上连语法检查都过不了** | ① 借一台 Mac；② **GitHub Actions 的 macOS runner**（public 仓库免费）；③ 找有 Mac 的人编 IPA |
| 签名/权限 | `ios/Henge/Henge.entitlements` 需要 `get-task-allow`（开 JIT）与 `increased-memory-limit`（6 GB 内存） | 沿用现成两个 entitlements 文件；无付费账号时用侧载 + StikDebug |

| 没有固件 | 板子要两块 64 MB pflash（`code.fd`/`vars.fd`），仓库里没有 | `tools/build-firmware.sh`（`--package` 用发行版 AAVMF，`--source` 编 edk2）；`.github/workflows/firmware.yml` 在 macos-14 上产出 artifact。细节见 `tools/FIRMWARE.md` |

**验收**：`make ios-lib` 产出 `build/ios/libhengevm.a`；`xcodebuild` 产出可安装的 `.app`；
`tools/build-firmware.sh` 产出两块 64 MB 的 pflash，且 `sh tools/run-uefi.sh` 能在串口上看到 EDK2 的版本行。

---

## 阶段 1：先让链接通过（不做这一步，什么都跑不起来）

| # | 缺口 | 位置 | 规模 | 验收 |
|---|---|---|---|---|
| 1.1 | **`devices/fb_metal.m` 根本不存在** | `fb_display.h` 声明了 `hv_fb_metal_set_layer` / `hv_fb_metal_frames` / `hv_fb_save_png`，`host/winvm.c:129/147/152/218` 在调用 → **`winvm` 链接必失败** | 中 | 编译通过 + 屏幕出画面 |
| 1.2 | iOS 桥还挂在 macOS 那套 API 上 | `ios/Henge/Native/henge-vm.c` include 了 `pvg_lite.h` `vm.h` `install.h` `dns_proxy.h`（**四个文件都已删**）；`Makefile` 已把它从 `IOS_SRC` 排除 | 中 | 新桥 `henge-winvm.c` 只暴露 `hv_winvm_*`，`libhengevm.a` 无 undefined |
| 1.3 | 遗留目标引用已删源 | `Makefile` 的 `hengevm` / `hengevm-ref` 指向 `host/main.c`、`host/vm.c`、`hvf_ref.c` | 小 | 删掉或改成历史注释 |

1.1 是唯一"缺实现"级的洞：`fb_metal.m` 要做的事很窄 —— 把 `fb_display` 拿到的 RGBA 帧传到 `CAMetalLayer`（一次纹理上传 + present），外加帧计数和 PNG 截图。可以照 `devices/pvg_metal.m` 里 CAMetalLayer 的用法，但逻辑比它简单一个数量级。

---

## 阶段 2：设备层（"能装、能看、能点"）

| # | 缺口 | 现状 | 规模 | 验收 |
|---|---|---|---|---|
| 2.1 | **USB 只读 CD-ROM** | 没有。`devices/xhci.c`（1001 行）是 HID 专用：`HV_XHCI_PORTS=2` 写死键盘/平板、报告是固定数组、**无 endpoint context / bulk transfer / 设备插槽模型**；全树 0 处 BOT/SCSI | **+800～1200 行** | 固件能从 ISO 走 El Torito 启动；PE 里 `dir E:` 能看到盘 |
| 2.2 | 三介质槽 | `HvWinVmConfig` 只有 `disk0`/`disk1`；阶段 2 需要 **系统盘 + PE 盘 + payload 盘** 三块 | 小 | 三块同时出现在 PE 的磁盘管理器 |
| 2.3 | 启动源选择 | 现在没有"这一阶段从哪块盘启动"的机制 | 小 | 阶段 2 从 PE 起、阶段 3 从系统盘起，全程不需要进 UEFI 菜单 |
| 2.4 | 触屏在 PE 里能用 | `xhci` 已实现绝对坐标平板（`tab_reports`，6 字节） | 小（验证为主） | PE 里鼠标点得准、能拖动 |
| 2.5 | ~~`virtio-gpu` 设备不存在~~ **已写出 2D**（`devices/virtio_gpu.c`，约 480 行） | 板子上原来只有 `ramfb`，而 `viogpudo.inf` 的 HardwareID 写死 `PCI\VEN_1AF4&DEV_1050`。现在是 1af4:1050、modern-only、两个队列（control+cursor），实现了 `GET_DISPLAY_INFO` / `RESOURCE_CREATE_2D` / `ATTACH_BACKING` / `TRANSFER_TO_HOST_2D` / `SET_SCANOUT` / `RESOURCE_FLUSH` / `UNREF`，scanout 直接接 `HvFbDisplay`（ramfb 与它共用一个呈现路径） | 结构体尺寸用 `_Static_assert` 钉住；**未在真机验证** | 设备管理器出现显示适配器；分辨率跟着 SET_SCANOUT 变 |
| 2.6 | virtio-vga / 3D（virgl） | 都不做：virtio-vga 只是多一套 legacy VGA 寄存器 + Option ROM，UEFI 引导用不到；3D 要 virglrenderer + `viogpu3d`，是 AppleVM 那条路，等 2D 跑通再说 | — | — |

### 显示设备模型：QEMU 有一堆，ARM64 Windows 只认两个

| 模型 | PCI / 位置 | 谁绑它 | 结论 |
|---|---|---|---|
| `ramfb` | fw_cfg `etc/ramfb`，无 PCI | **只有 GOP 和 Linux**；Windows 无驱动 | 保留：固件出画面要用 |
| `virtio-gpu-pci`（= "GPU PCII"） | `1af4:1050`，class `03/80` | `viogpudo`（DOD，仅 2D） | **已实现**，`devices/virtio_gpu.c` |
| `virtio-vga` | `1af4:1050` + VGA 寄存器 + Option ROM | 同上，但只为 BIOS 引导补 VGA 兼容 | 不做：UEFI 客人不碰 VGA 寄存器 |
| `virtio-gpu-gl-pci` | 同上 + `VIRTIO_GPU_F_VIRGL` | `viogpu3d`（3D） | 后续；要先有 2D 基线 |
| `bochs-display` | `1234:1111` | Windows 无 ARM64 驱动（VBEMP 是 x86） | 不做 |
| `qxl` / `stdvga` / `vmware-svga` | — | 全是 x86/x64 驱动或 BIOS 路径 | 不做 |

### 3D 那一半：协议写完了，后端还是空的

`virtio_gpu.c` 现在实现了 3D 的**全部协议面**：`CTX_CREATE` / `CTX_DESTROY` /
`CTX_ATTACH_RESOURCE` / `RESOURCE_CREATE_3D` / `TRANSFER_TO_HOST_3D` /
`TRANSFER_FROM_HOST_3D` / `SUBMIT_3D`，`GET_CAPSET_INFO` / `GET_CAPSET` 也从
"回零"改成了转给后端。真正把 virgl 命令流翻译成 Metal 的部分在
`devices/gpu_renderer.h` 的 vtable 里：

```c
g->rend->ctx_create(ud, ctx_id, name, init);
g->rend->res_create(ud, &info);            // PIPE_TEXTURE_2D ...
g->rend->transfer_to_host(ud, id, off, level, stride, layer_stride, src, len, box);
g->rend->submit(ud, ctx_id, cmds, len, flags);
```

要接 virglrenderer（UTM 的 Neptune 构建里就有），就是把这几个函数填成
`virgl_renderer_resource_create` / `virgl_renderer_ctx_create` /
`virgl_renderer_submit_cmd` / `virgl_renderer_transfer_*`，然后
`hv_virtio_gpu_set_renderer(g, &r)`。**没接的时候** `VIRTIO_GPU_F_VIRGL` 不 advertise，
所有 3D 命令回 `ERR_UNSPEC`，设备退化成纯 2D —— 驱动不会挂到 error 43 上。

`hv_gpu_renderer_none()` 是现在的占位后端，它一律拒绝。

板子现在有 `HvDisplayKind`：`RAMFB` / `VIRTIO_GPU` / `AUTO`（默认，两者都建）。UI 设置页可选，命令行也可以：

```sh
HV_DISPLAY=gpu HV_PANEL=1440x900 sh tools/run-uefi.sh
```

`GET_DISPLAY_INFO` 现在报的是**面板 hint**（`WinMachine.panelW/H` → `hv_fb_display_set_hint`），而不是固件留下的 1024×768 —— 否则驱动会按 1024×768 建 framebuffer 再拉伸。

**2.5 是这个项目目前最硬的一个漏洞**：ramfb 只有 Linux 认，Windows 侧没有任何驱动会绑定它；WinPE 也许靠 UEFI GOP 还能出画面（未验证），但安装完的系统一定要 `virtio-gpu` + `viogpudo`。驱动盘（`tools/make-driver-iso.sh`，2.3 MB）已经把 `viogpudo` 备好了，缺的是**给它一个能绑的设备**。

2.1 的规格已经写成契约：`INSTALL-DESIGN.md` §15（设备类型必须报 5 = CD-ROM、逻辑块 2048、`READ CAPACITY` 返回最后一个 LBA、`MODE SENSE` 报写保护，以及 9 条必须应答的 SCSI 命令）。

---

## 阶段 3：介质与下载（**已被 INSTALL-DESIGN.md §16 简化**：改下官方完整 ISO + 自建 autounattend 盘，拆 SWM/打 ISO/自定义 PE 全部不需要）

| # | 缺口 | 说明 | 规模 |
|---|---|---|---|
| 3.1 | 驱动盘 ISO（~200 KB） | NetKVM + viogpudo(w11/ARM64) + `Virtio_Win_Red_Hat_CA.cer` + `unattend.xml` + `install-drivers.cmd`，**构建时生成一次当资源**；二进制要先拿到（virtio-win ISO / osy 仓库） | 小 |
| 3.2 | payload 下载 | 从你的服务器下 ISO。最低要求：断点续传 + SHA256 校验 + 磁盘空间预检。QEMU 的 http 后端（`readahead` 默认 256 KB、512 对齐、只读）可当"边下边读"的参考 | 中 |
| 3.3 | `unattend.xml` | 扒 TPM/SecureBoot/RAM 检查（`LabConfig`）、跳联网、本地账户、时区 zh-CN、`FirstLogonCommands` 挂驱动安装 | 小 |

---

## 阶段 4：三页面 UI（现在全是 macOS 的页面，要换掉）

| # | 页面 | 现状 | 要做 |
|---|---|---|---|
| 4.1 | 页 1 首页 | `HomeView.swift` 是 macOS 机器列表 | 加一个 "Install Windows 11" 入口 + 状态徽标 |
| 4.2 | 页 2 安装页 | 不存在（现有 `InstallView.swift` 是 macOS 恢复安装的进度页） | **重写**：一个"分配空间"输入框（默认 32 GB）+ 一个大按钮"开始安装" + 一行状态。**不再让用户选文件** |
| 4.3 | 页 3 设置页 | `SettingsView.swift` 是 macOS 设置 | 改：内存默认 6 GB（4–10）、CPU、分辨率；新增"驱动"分组（显示"已内置：显示 2D / 网卡"）；macOS 专属项隐藏 |
| 4.4 | 会话页 | `SessionView.swift` 是 macOS 会话 | 保留并改：Metal layer 绑定、触屏当鼠标、底部虚拟键盘、右上角阶段、关机/返回 |
| 4.5 | Swift ↔ C 桥 | `HengeBridge.h` 现在**没有任何 `hv_*` 导出** | 新增：create / start / stop / set_display_layer / input / screenshot / flush_disks + 状态回调 |

---

## 阶段 5：真机联调（"能用"的最后一段）

| # | 项 | 验收 |
|---|---|---|
| 5.1 | 阶段 2 走通 | PE 起来 → 分区 → `dism /apply-image` → `bcdboot` → 关机 |
| 5.2 | 阶段 3 走通 | 改启动项 → unattend 跑完 OOBE → 进桌面 |
| 5.3 | 阶段 4 走通 | 挂驱动盘 → 装 viogpudo + NetKVM → 重启 → 分辨率生效 |
| 5.4 | 内存与稳定性 | 6 GB 下不触发 Jetsam（`hv_alloc_no_footprint` 已在）；JIT 缓存不失控 |
| 5.5 | 黑屏兜底 | 显示失败要能回退 + 落日志，不能白屏死住 |

---

## 阶段 6：收尾

| # | 项 |
|---|---|
| 6.1 | 三个孤儿测试（`test_virt.c` / `test_pcie.c` / `test_dtb.c`）按新 API 重写（需要能编译的 arm64 环境） |
| 6.2 | `MILESTONES.md` 的 M4/M5 与实际对齐 |
| 6.3 | 打包 IPA + JIT 引导文案（StikDebug）+ 首次启动说明 |

---

## 一句话总结

- **必须做、且没有替代方案**：`fb_metal.m`（1.1）、USB CD-ROM + xHCI 通用化（2.1）、iOS 桥（1.2）、三页面 UI（4.x）。
- **可以推后**：virtio-gpu 2D/3D（§12）、孤儿测试、驱动盘里的 3D 部分。
- **唯一的硬阻塞**：没有 macOS 编译环境（阶段 0）。这一条不解决，后面全部无法验证。
