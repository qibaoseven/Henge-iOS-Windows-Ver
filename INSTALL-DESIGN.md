# Windows 11 ARM64 安装流程设计（v2：驱动内置，四阶段 → 三步文件）

> 2026-11-16，第二版。基于 `qibaoseven/Henge-iOS-Windows-Ver`（HEAD `4a6cda5`）。
> v2 的变化：**驱动不再让用户下载**。显卡/网卡驱动直接打进 App，自动安装。
> 用户要准备的东西从 3 份（ESD / WinPE / virtio ISO）降到 **2 份（ESD / WinPE）**。

---

## 0. 结论先行：显卡驱动查到什么

| 问 | 答 |
|---|---|
| 仓库里有没有 virtio 显卡驱动 | **没有**。全树 0 个 `.inf/.sys/.cat`；`third_party/` 只有恢复镜像、网络、plist 那些 |
| 引擎里有没有 virtio-gpu 设备 | **没有**。显示只有 `devices/fb_display.c`（ramfb，EDK2 GOP 写 `etc/ramfb`） |
| ARM64 有没有 2D 显卡驱动 | **有**：virtio-win 里存在 `viogpudo\w11\ARM64`（`viogpudo.inf`），Windows 11 ARM64 可用；**它是 DOD（Display Only Driver），只有 2D** |
| ARM64 有没有 3D 显卡驱动 | **没有可用的**。见 §7.3 |
| UTM 能借到什么 | UTM 的 GPU 加速只对 **Linux 访客**（virgl/venus）开放；Windows 访客官方不提供 DX/GL 3D，官方也把 GPU 加速默认关掉了 |

所以"把驱动全合并进 App、用户不用下载"这件事 **2D 成立、3D 不成立**：
- **能内置**：`viogpudo`（显示，2D）、`NetKVM`（网卡）、证书；
- **不能内置**：任何能跑 3D 的 Windows ARM64 驱动 —— 市面上不存在。

---

## 1. 目标与不变量

| 项 | 值 |
|---|---|
| 目标 guest | Windows 11 ARM64 |
| 内存 | 默认 **6 GB**（页 3 可调） |
| 系统盘 | 稀疏镜像，默认 **32 GB**（页 2 的输入框） |
| 设备 | NVMe（系统盘）、xHCI（键鼠）、ramfb **+ virtio-gpu（新）**、HDA、virtio-net |
| 没有 | 无 hypervisor、无 EL2（无 Hyper-V/WSL2/VBS）、**无 TPM**、无 AArch32 |
| 用户要准备 | **2 个文件**：Win11 ARM64 ESD、WinPE（ARM64） |

---

## 2. 三个页面 + 会话页

| # | 页面 | 改法 |
|---|---|---|
| 1 | 首页 | 保留；入口改 "Install Windows 11"，列表加状态徽标 |
| 2 | **安装页** | **整体重写**：一个"分配空间"输入框（默认 32 GB）+ 一个大按钮"开始安装"；下面是**两份文件**的准备清单（原来三份，virtio 那份删掉——现在是内置的） |
| 3 | 设置页 | 保留改动：内存默认 6 GB（4–10）、CPU 4、分辨率跟随设备；新增"驱动"分组（显示"已内置：显示 2D / 网卡 / 证书"，不可选）；macOS 专属项隐藏 |
| 4 | 会话页 | 保留改动：触屏 = 绝对定位鼠标、底部虚拟键盘、右上角阶段；阶段 2 常驻操作卡片 |

---

## 3. 四阶段状态机

### 阶段 1 — 准备（不启动 VM）

| # | 文件 | 用途 | 校验 |
|---|---|---|---|
| 1 | **Windows 11 ARM64 ESD** | WinNTSetup 的安装源 | `MSCM`/`MSCW` 魔术字 + 大小 |
| 2 | **WinPE（ARM64，FAT32 镜像）** | 阶段 2 的启动环境 | 有 `EFI/BOOT/BOOTAA64.EFI` |

驱动这一项**已经不是用户的事**：App 内置一张几百 KB 的 FAT32 小盘（见 §4）。

### 阶段 2 — WinPE 装系统（用户主导）

机器：6 GB 内存、32 GB 稀疏盘。挂三块盘：

| 槽 | 内容 | 属性 |
|---|---|---|
| NVMe0 | 系统盘（32 GB 稀疏） | RW |
| NVMe1 | WinPE 盘 | RO，**启动源** |
| NVMe2 | ESD 盘 | RO |

外加一张 **内置驱动盘**（只读小盘，随程序走）——里面是 `viogpudo\w11\ARM64`、`NetKVM\w11\ARM64` 和 `cert\Virtio_Win_Red_Hat_CA.cer`。

程序只做四件事：建机器、挂盘、从 PE 盘启动、把画面/触屏/键盘直通。**不做任何自动化**。

用户按卡片操作：

1. **DiskGenius**：32 GB 盘转 GPT，建 ESP 300 MB + MSR + 主分区。
2. **WinNTSetup**：安装源 = ESD 盘里的 `install.esd`（选 Win11 ARM64 索引）；引导驱动器 = ESP；安装驱动器 = 主分区；**无人值守 = 我们提供的 `unattend.xml`**。
3. 安装完成 → 关机。

### 阶段 3 — 第一次开机（挂机）

卸载 PE/ESD/驱动盘，只留系统盘启动。`unattend.xml` 接管 OOBE：
跳过联网、建本地账户、语言/区域/时区、`LabConfig` 绕过 TPM/SecureBoot/RAM 检查。

用户只需等到桌面，点"下一步"。

### 阶段 4 — 驱动收尾（全自动，**不再需要 virtio ISO**）

重建 VM，介质 = 系统盘 + **内置驱动盘**（含自动脚本）。启动后自动执行：

```
certutil -addstore    Root            D:\cert\Virtio_Win_Red_Hat_CA.cer
certutil -addstore -f TrustedPublisher D:\cert\Virtio_Win_Red_Hat_CA.cer
pnputil /add-driver D:\NetKVM\w11\ARM64\netkvm.inf /install
pnputil /add-driver D:\viogpudo\w11\ARM64\viogpudo.inf /install
```

自动重启一次 → 完成。条目变"可用"，用户手动 Start。

> 证书这两步是必须的：virtio-win 的驱动是 Red Hat **attestation 签名**，不是 WHQL；
> 不导入 `Virtio_Win_Red_Hat_CA.cer` 到 Root + TrustedPublisher，`pnputil` 会拒绝加载。

---

## 4. 内置驱动盘（App 自带的那个）

| 内容 | 来源 | 大小 | 说明 |
|---|---|---|---|
| `NetKVM\w11\ARM64\*.inf/.sys/.cat` | virtio-win | 几十 KB | 网卡，装完就能上网 |
| `viogpudo\w11\ARM64\viogpudo.inf/.sys` | virtio-win | 约 148 KB | 显示，**2D-only DOD** |
| `cert\Virtio_Win_Red_Hat_CA.cer` | virtio-win | KB 级 | 驱动签名 CA |
| `unattend.xml`、`install-drivers.cmd` | 我们生成 | KB 级 | 自动化 |

形态：一个 FAT32 小镜像（生成一次打进 App，几百 KB），挂成只读 NVMe。**不进 4 GB 单文件限制的问题**，因为最大文件才 148 KB。

许可证：virtio-win 驱动是 Red Hat 发布的开源驱动（BSD/GPL 系），可再分发，随附 LICENSE 与来源说明即可。

---

## 5. 介质格式（三个硬约束，仍然有效）

我们只有 NVMe（没有 AHCI/ATAPI CD），块设备后面是**原始字节流**，不是光盘。

### 5.1 PE 盘：用 UEFI 可启动的 FAT32 镜像（不要 ISO）

ISO 要靠 El Torito 从"光盘"启动，这条链路（EDK2 ArmVirtQemu + 我们的 NVMe）没验证过；
FAT32 扁平镜像挂到 NVMe 后，BDS 直接当可启动盘，零风险。

### 5.2 ESD 盘：三条路

| 方案 | 做法 | 代价 | 风险 |
|---|---|---|---|
| A | 用户把 `install.esd` 打成 UDF ISO，我们当只读 NVMe 挂 | 0 | 未验证，Windows 多半不把裸块设备上的 ISO 认成卷 |
| **B** | 主机侧解析 ISO（ISO9660/UDF），重合成 **exFAT 盘**，里面只放 `install.esd` | ~600 行 | 低 |
| **C** | 用户直接给 **exFAT 镜像**（里面一个 `install.esd`） | ~0 | 低，用户多一步 |

`install.esd` 通常 4–5 GB，**FAT32 单文件上限 4 GB** 放不下 → 必须 exFAT 或 UDF。

### 5.3 驱动盘：不用讨论

我们自己做，FAT32，几百 KB（§4）。

---

## 6. `unattend.xml` 要点

| 段 | 内容 |
|---|---|
| `windowsPE` → `RunSynchronous` | 写 `HKLM\SYSTEM\Setup\LabConfig`：`BypassTPMCheck` / `BypassSecureBootCheck` / `BypassRAMCheck` = 1（本机无 TPM，这是**必需**） |
| `specialize` | 计算机名、时区 `China Standard Time`、zh-CN |
| `oobeSystem` | 本地账户、跳过联网（`BypassNRO`）、跳过隐私页 |
| `oobeSystem` → `FirstLogonCommands` | 留一个"下次开机装驱动"的钩子（阶段 4 用） |

---

## 7. 显卡：2D 能到哪，3D 为什么不行

### 7.1 路线对比

| 路线 | guest 侧驱动 | 我们这边要做 | 结果 |
|---|---|---|---|
| ramfb（现状） | Windows 自带 BasicDisplay（继承 UEFI GOP 帧缓冲） | `fb_metal.m` 呈现器（**缺**） | 能装、能看、能办公；分辨率固定、无加速 |
| **virtio-gpu 2D + viogpudo** | `viogpudo\w11\ARM64`（virtio-win） | 写 `devices/virtio_gpu.c`（2D 命令流）+ 呈现器 | 分辨率可换、缩放更好、仍是 2D |
| virtio-gpu 3D（virgl） | Windows ARM64 **无可用驱动** | virglrenderer 移植 + 3D 驱动 | **当前不可达** |

### 7.2 virtio-gpu 2D 要写什么（`devices/virtio_gpu.c` 工作量估计）

- virtio 队列 + 控制队列：`GET_DISPLAY_INFO`、`RESOURCE_CREATE_2D`、`RESOURCE_ATTACH_BACKING`、`SET_SCANOUT`、`TRANSFER_TO_HOST_2D`、`RESOURCE_FLUSH`
- 显存就是 guest RAM 里的资源页；flush 时做一次 blit 到呈现器
- 规模：**比 `pvg_metal.m`（2883 行 Metal）小一个数量级**，大约 300–600 行 C
- 前置：`fb_metal.m` 呈现器要先有（否则导不出来）

### 7.3 3D：为什么现在没有

- Windows 的 3D 虚拟显卡需要 **guest 侧 WDDM 3D 驱动**（virgl/venus 或 DirectX 直通）。
- 目前唯一的 Windows 3D 路线是社区项目 **Triton**（virgl/DirectX in QEMU/KVM）：
  **它在 ARM64 Windows 上崩溃**，且多年未上游合并。
- UTM 对 Windows 访客**不提供** DirectX/OpenGL 3D（GPU 加速只给 Linux 访客，官方还默认关掉）。
- 结论：**3D 不是"难"，是"没有驱动"**。要等有人把 virgl 的 WDDM 驱动编出 ARM64 版；在此之前，
  唯一能做的是把 2D 做到"顺滑、可缩放、不掉帧"，让 Windows 用起来像台机器。

---

## 8. 与现有代码的映射

**已有**：双 NVMe 槽、xHCI 键盘 + 绝对平板、ramfb、稀疏镜像、Jetsam 豁免内存、截图/停止/统计钩子。

**缺**（按依赖排）：

| # | 缺什么 | 规模 |
|---|---|---|
| 1 | `devices/fb_metal.m` 呈现器 | 中（现在 `winvm` 链接不过） |
| 2 | 第三个只读槽（阶段 2 要 3 块盘） | 小 |
| 3 | 启动源选择（阶段 2 从 NVMe1 启动，写 `BootNext` 到 vars flash） | 小 |
| 4 | `devices/virtio_gpu.c`（2D） | 中（300–600 行） |
| 5 | Swift 侧四阶段状态机 + 两份文件校验 + `unattend.xml` 生成 | 中 |
| 6 | 内置驱动盘生成（从 virtio-win ISO 抽 5 个文件 → FAT32） | 小 |
| 7 | ESD 盘（按 §5.2 选型） | 0–600 行 |

**没有热插拔**：设备在 `hv_winvirt_create` 时一次建好。阶段 3→4 换介质**靠重建 VM**——正好对应"自动重启"。

---

## 9. 失败与回滚

| 阶段 | 失败 | 处理 |
|---|---|---|
| 1 | 文件不合格 | 只标红那一行 |
| 2 | DiskGenius/WinNTSetup 出错 | 不动镜像，可重来 |
| 2 | App 被杀 | 稀疏盘留着，下次接着用 |
| 3 | OOBE 卡住 | "回阶段 2 重装"，保留 `unattend.xml` 便于对比 |
| 3 | 黑屏 | 判为 ramfb/呈现器问题，落日志 + 串口最后一行 |
| 4 | 驱动没装上 | 不影响开机；可再挂驱动盘重跑，条目仍算"可用" |

---

## 10. 要你拍板的（已简化）

1. **ESD 怎么进 guest**：B（我们解析 ISO 再合成 exFAT，稳）还是 C（用户给 exFAT 镜像，省）？
   建议 **C 起步、B 作为后续自动化**。
2. **PE 给 FAT32 镜像**（我的强建议）——你确认就行。
3. **32 GB / 6 GB 是默认还是硬性**？6 GB 在 8 GB 机型上要防 Jetsam，要不要按机型自动降？
4. **阶段 4 的自动装驱动载体**：`unattend` 的 `FirstLogonCommands`，还是独立自动执行盘？建议后者。
5. **要不要现在就做 virtio-gpu 2D**？我的建议：**先做 `fb_metal.m` + ramfb 把画面跑顺，virtio-gpu 放进 M4**。
   因为 2D 的收益是"分辨率可换"，而代价是一个新设备 + 新驱动加载链；ramfb 已经能装能看。

---

## 11. 落地顺序

1. `devices/fb_metal.m`（不解决什么都看不见）
2. 三介质槽 + 启动源选择（阶段 2 能起来）
3. Swift 四阶段状态机 + 页 2/3/4 改造 + 内置驱动盘
4. `unattend.xml` 与自动装驱动脚本
5. ESD 盘（按决策 1）→ 真机冒烟
6. （M4）`devices/virtio_gpu.c` + `viogpudo` 自动装 → 2D 升级

---

## 12. 参考：Apple VM（zktfjcksxk-spec/AppleVM）的显卡与安装路径

> 同源项目：WindowsiOS（`ammaarreshi/WindowsiOS`，基于 Henge）的修改版，GPL-2.0。它跑通了
> **Windows 11 ARM64 的 DirectX 11**，所以它的做法可以直接照搬。

### 12.1 显卡驱动是怎么拿到的（不是自研）

| 层 | 内容 | 来源 | 许可 |
|---|---|---|---|
| Windows 侧驱动 | **viogpu3d v0.5** + **Neptune UMD** | `osy/kvm-guest-drivers-windows`、`osy/virtio-win-mesa`，**原样二进制**打进 `winkit/viogpu3d/` | BSD-3-Clause / MIT |
| 网卡驱动 | NetKVM ARM64 | Red Hat `virtio-win/kvm-guest-drivers-windows`，原样 | BSD-3-Clause |
| 宿主渲染器 | **virglrenderer（Neptune）**，UTM 的 `neptune` 分支 | `utmapp/virglrenderer`（MIT）；预编译 framework 取自 UTM iOS v5.0.8 | MIT |
| D3D11 → Metal | **DXMT** | `3Shain/dxmt` | LGPL（独立可替换 framework） |
| GLES → Metal | ANGLE | chromium angle | BSD-3 |
| Vulkan → Metal | MoltenVK + **KosmicKrisp**（Mesa 的 Vulkan-on-Metal，**二进制打补丁**：`kk_image_layout_init` 两条指令） | `gitlab.freedesktop.org/osy/mesa` | MIT |
| 引擎侧设备 | **`devices/virtio_gpu3d.c`**，照 QEMU `hw/display/virtio-gpu-virgl.c` 的设计；`devices/virglrenderer.h` 是 virglrenderer 的公开头 | `utmapp/qemu` fork | GPL-2.0 |

也就是说：**驱动是"拿现成的"，不是编译出来的**；真正的工作量在宿主侧的 `virtio_gpu3d.c` + virgl 接线 + Metal 呈现。
它整套源码在 `source/windowsios-full.patch`（GPL-2.0，基于 WindowsiOS `0071d69`），我们同源，可以直接读来移植。

### 12.2 它怎么装 Windows（和我们的方案根本不同）

- **不在设备上装**：在 Mac 上编译同一个模拟器的 `build/winvm`，跑 `scripts/provision_windows_arm64.py`，
  两阶段无人值守（阶段 1 ≤150 分钟：WinPE 分区 + **DISM** 写盘 + 写启动项 + 关机；阶段 2 ≤240 分钟：OOBE 自动登录 + 关机），
  产出 `code.fd` / `vars.fd` / `windows11.img` → `hvsparse pack` 成 `windows11.hvsparse`。
- 设备上只做：导入三个文件 → 开 JIT → 启动 → 在 Windows 里双击 `START.bat` 装驱动。
- 它自己写明：**"能不能不用 Mac？制作这一步不行"**。
- 两个可直接借鉴的点：
  1. `vars.fd` 必须带上 Windows 的启动项，换空白 NVRAM 就进不了系统 —— 对应我们要处理的 boot order。
  2. 它的阶段 1/2 全自动、还带截图进度 —— 说明"WinPE + DISM + bcdboot + unattend"这条链可以完全脚本化。
     我们的阶段 2 现在是手工 DiskGenius/WinNTSetup，**可以改成自动**（自解 ESD 里的 WinPE 镜像 + 自带 autorun 脚本），
     那样用户只需要给一个 ESD 文件。

### 12.3 已知限制（照搬时要一起接受）

- 最高 **DirectX 11（feature level 11_0）**，无 DX12；DXMT 被刻意限制在 11.0，否则 Windows 拒绝驱动。
- 显存窗口 **512 MB**（受 App 地址空间限制）。
- 8 GB 内存需要 **Increased Memory Limit** 能力，否则自动降到 4–5 GB，3D 可能加载失败。
- 开 3D 后可能黑屏 → 用 `Shared/gpu3d.on` 开关回退到基本显示；**不要禁用 Microsoft Basic Display Driver**。
- Windows 自带"重启"偶发卡黑屏，关机正常。
