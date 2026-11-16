# Windows 11 ARM64 安装流程设计（四阶段 / 三页面 + 会话页）

> 设计稿，2026-11-16。基于 `qibaoseven/Henge-iOS-Windows-Ver` 当前代码（HEAD `4a6cda5`）。
> 目标：iPhone/iPad 上装 Windows 11 ARM64，**全程可见、可操作、可重来**；不追求"一键装完"。

---

## 0. 目标与不变量

| 项 | 值 |
|---|---|
| 目标 guest | Windows 11 ARM64 |
| 内存 | 默认 **6 GB**（页 3 可调），装 PE 与装系统都用这个值 |
| 系统盘 | 稀疏镜像，默认 **32 GB**（页 3 的输入框决定） |
| 设备 | NVMe（系统盘）、xHCI（键鼠）、ramfb（画面）、HDA、virtio-net（装完驱动后可用） |
| 没有的东西 | 无 hypervisor、无 EL2（Hyper-V/WSL2/VBS 用不了）、**无 TPM**（Win11 必须绕过）、无 AArch32 |
| 依赖 | EDK2 ArmVirtQemu 双 pflash + ACPI（已有），ramfb 画面通路 |

---

## 1. 三个页面 + 会话页

| # | 页面 | 现状 | 改法 |
|---|---|---|---|
| 1 | **首页** `HomeView` | Mac 列表 + "Install macOS" | 保留结构；列表项加状态徽标（准备中 / 装系统中 / 可用），入口文案改成 "Install Windows 11" |
| 2 | **安装页** `InstallView` | 选 macOS 版本、内存滑杆、JIT 检查、空间预检、下载进度 | **整体重写**：只有一个"分配空间"输入框（默认 32 GB，步进 4 GB）+ 下方一个大按钮"开始安装"；再往下是**三份文件的准备清单**（见阶段 1），不出现版本下拉、不出现内存滑杆（内存挪到页 3） |
| 3 | **设置页** `MacSettingsView` | 内存 / CPU / 分辨率 / 手势 / 颜色 | 保留并改：内存默认 **6 GB**（范围 4–10），CPU 默认 4，分辨率默认跟随设备；新增"介质"分组，显示 PE / ESD / virtio 三个文件的路径与校验状态；原来 macOS 专属项（颜色匹配、Magic Trackpad 模式）隐藏 |
| 4 | **会话页** `SessionView` | macOS 桌面 + 触屏当 Magic Trackpad 2 | 保留并改：Windows 桌面；触屏 = **绝对定位鼠标**（点即左键、长按拖动、两指点右键）；底部一行虚拟键盘（Esc/Tab/Ctrl/Win/Alt + 字母区）；右上角显示当前阶段；阶段 2 时底部常驻一张"操作卡片"（DiskGenius → WinNTSetup → 关机） |

> 说明：你说"三个界面"，我把会话页单独算一类（它是运行态，不是配置态）。如果会话页也要算进去，那第 1 页可以合并进首页。

---

## 2. 四阶段状态机

### 阶段 1 — 准备（不启动任何 VM）

界面：页 2。程序**不启动虚拟机**，只做文件收集与校验。

要用户提供三份文件：

| # | 文件 | 用途 | 校验（程序做） |
|---|---|---|---|
| 1 | **Windows 11 ARM64 ESD**（`install.esd`，或从 ISO 里取出） | WinNTSetup 的安装源 | 头部 `MSCM`/`MSCW` 魔术字；`install.esd` 里应含 `Windows 11 ARM64` 映像（用 `wimlib`/自研只读解析取 image 列表亦可，最低限度只校验魔术字与大小） |
| 2 | **Windows PE（ARM64）** | 阶段 2 的启动环境 | 里面有 `EFI/BOOT/BOOTAA64.EFI`；是 **UEFI 可启动的 FAT32 镜像**（见 §4） |
| 3 | **virtio-win ISO（ARM64）** | 阶段 4 装驱动（NetKVM，将来 viosgpu） | ISO 里存在 `NetKVM/arm64/*.inf` |

交互：每份文件一行，"选择文件 / 从链接下载（可选）/ 更换"，校验通过打勾。三份齐了，"开始安装"才可点。

失败：任何一份不合格 → 只红那一行，不阻断其他两份的收集。

### 阶段 2 — 用 WinPE 装系统（用户主导，程序只看）

程序做的事只有四件：

1. 按页 3 的内存（默认 6 GB）与页 2 的空间（默认 32 GB，稀疏）建机器；
2. 挂三块盘：**系统盘（RW）** + **PE 盘（RO，启动源）** + **ESD 盘（RO）**；
3. 从 PE 盘启动，把画面/触屏/键盘直通给用户；
4. 底部常驻操作卡片，除此之外**不自动做任何事**。

用户按卡片操作：

1. 打开 **DiskGenius**：把 32 GB 盘转 GPT，建 `ESP 300 MB` + `MSR` + 主分区（或直接快速分区）。
2. 打开 **WinNTSetup**：
   - "安装源"选 ESD 盘里的 `install.esd`（选 Windows 11 ARM64 那个索引）；
   - "引导驱动器"选 ESP；
   - "安装驱动器"选主分区；
   - **"无人值守"选我们提供的 `unattend.xml`**（随 PE 盘一起放，或单独一张小 tools 盘）。
3. 点安装 → 等进度走完 → 卡片提示"关机"。
4. 关机（或点卡片的"我装好了，进入第一次开机"）。

为什么走 WinNTSetup 而不是 Setup：WinNTSetup 能直接吃 `unattend.xml`，把 OOBE、TPM 检查、账户、驱动安装全部前置配置好，于是**阶段 3 不需要触屏驱动**，自己跑完就行——这是你这条方案的核心理由，成立。

### 阶段 3 — 第一次开机（挂机）

程序：卸载 PE 盘与 ESD 盘，只留系统盘启动；进程只负责"跑着 + 显示 + 一个按钮"。

guest：`unattend.xml` 接管全部 OOBE（跳过联网、建本地账户、语言/区域/时区、绕过 TPM/SecureBoot/RAM 检查）。

用户：什么都不用做，看它自己走到桌面。底部只有一个按钮："已进入桌面，下一步"。

### 阶段 4 — 驱动收尾（自动）

程序：**重建 VM**（不是热插拔），介质换成 **系统盘 + virtio-win ISO（RO）**；启动后自动执行驱动安装，然后自动重启一次。

自动执行的载体（二选一，见 §8 决策 4）：
- 阶段 3 的 `unattend.xml` 里 `FirstLogonCommands` 已经排好了"下次开机去 `D:\` 跑 pnputil"；
- 或阶段 4 挂一张我们生成的**自动执行盘**（FAT32，内含 `install-drivers.cmd` + `unattend` 片段）。

实际命令（WinPE/Windows 都带 `pnputil`）：
```
pnputil /add-driver D:\NetKVM\arm64\*.inf /install
pnputil /add-driver D:\vioscsi\arm64\*.inf /install     :: 将来若换 vioscsi
pnputil /add-driver D:\viosgpu\arm64\*.inf /install     :: 要等 virtio-gpu 实现后才用得上
```
装完自动重启 → **阶段完成**，回到首页，条目变成"可用"，用户手动 **Start**。

---

## 3. 各阶段设备表

| 阶段 | NVMe0（系统盘） | NVMe1 | NVMe2 | 启动源 | 自动化 |
|---|---|---|---|---|---|
| 2 | RW 32 GB 稀疏 | PE 盘 RO | ESD 盘 RO | NVMe1 | 无（用户操作） |
| 3 | RW | — | — | NVMe0 | unattend |
| 4 | RW | virtio ISO RO | — | NVMe0 | FirstLogonCommands / 自动盘 |

内存全程 6 GB（页 3 可调）。分辨率跟随设备，ramfb 由 EDK2 GOP 配置。

> 现在代码只支持 **2 个盘位**（`disk0` 读写 / `disk1` 只读）：阶段 2 要 **3 个**，这是第一个要扩的地方（`HvWinVmConfig` + `winvirt` + `pcie` 槽位，槽位够，`HV_PCI_SLOTS 32`）。

---

## 4. 介质格式：三个硬约束（这一节决定成败）

我们**没有 CD-ROM / AHCI**，只有 NVMe（见 §6），NVMe 后面挂的是**原始字节流**，不是"光盘"。所以：

### 4.1 PE 盘：不要用 ISO，用 UEFI 可启动的 FAT32 镜像

- ISO 要靠 El Torito 从"光盘"启动，而这条链路（EDK2 ArmVirtQemu + 我们的 NVMe）**没验证过**；
- FAT32 镜像（`.img`，扁平的）挂到 NVMe 后，BDS 直接把它当普通可启动盘，零风险。
- 制作方式：用户自己用 Rufus 做"WinPE ISO → 写进 VHD/IMG"，或我们以后做工具帮转。

### 4.2 ESD 盘：三条路，按代价排

| 方案 | 做法 | 代价 | 风险 |
|---|---|---|---|
| **A** | 用户把 `install.esd` 打成 **UDF ISO**，我们当只读 NVMe 挂 | 0 行代码 | **未验证**：Windows 会不会把裸块设备上的 ISO 认成卷。大概率不行（Windows 的 ISO 挂载是按文件走的） |
| **B** | 主机侧解析用户的 ISO（ISO9660/UDF），**重新合成一个 exFAT 盘**，里面只放 `install.esd` | ~600 行（ISO/UDF 只读 + exFAT 写） | 低。guest 100% 可读 |
| **C** | 用户直接给一个 **exFAT 镜像**（`.img`），里面就一个 `install.esd` | ~0 行代码 | 低。但要用户会用 diskpart/挂 VHD 造镜像 |
| **D** | 放弃 ESD，直接用完整 Win11 ARM64 ISO 走 Setup | 0 行代码 | 中：Setup 交互多，且 OOBE 自动化没 WinNTSetup 顺手 |

> 补充：`install.esd` 通常 **4–5 GB**，FAT32 单文件上限 4 GB 放不下 → 用 **exFAT**（WinPE 自带 exFAT 驱动）或 UDF。

### 4.3 virtio ISO：和 4.2 同一个问题

阶段 4 要 `pnputil` 读里面 `.inf`，所以同样不能是"裸块设备上的 ISO"。最省事：**方案 C** —— 让用户给一个 exFAT 镜像，里面放 `NetKVM/arm64` 那几个文件（几百 KB），或者我们直接从 ISO 里抽出这几个文件合成小 FAT32 盘（**这条最划算**：virtio 驱动总共几百 KB，完全放得进 FAT32，不受 4 GB 限制）。

**结论建议**：virtio 驱动盘由**我们生成**（从 ISO 抽 4 个文件合成 FAT32 小盘，几十行代码）；PE 要用户给 FAT32 镜像；ESD 用方案 C 或 B。

---

## 5. `unattend.xml` 要写什么

| 段 | 内容 | 作用 |
|---|---|---|
| `windowsPE` → `Microsoft-Windows-Setup` → `RunSynchronous` | 写 `HKLM\SYSTEM\Setup\LabConfig`：`BypassTPMCheck` / `BypassSecureBootCheck` / `BypassRAMCheck` = 1 | 绕过 Win11 硬门槛（本机无 TPM） |
| `windowsPE` → `UserData` | `AcceptEula`、产品密钥（可留空/通用密钥） | 免交互 |
| `specialize` | 计算机名、时区 `China Standard Time`、语言 zh-CN | 免 OOBE 询问 |
| `oobeSystem` | 建本地账户、`BypassNRO`（跳过联网）、跳过隐私页 | 阶段 3 无人值守 |
| `oobeSystem` → `FirstLogonCommands` | 下次开机挂 virtio 盘后执行 `pnputil` 的脚本 | 阶段 4 自动装驱动 |

注意：本机 **无 TPM 也无 Secure Boot**，所以 `BypassTPMCheck` 是必需的，不是可选。

---

## 6. 与现有代码的映射

**已经有的（不用做）**

| 能力 | 位置 |
|---|---|
| 双 NVMe（一个 RW、一个 RO） | `winvirt.c`、`host/winvm.c`（`disk0`/`disk1`） |
| 绝对定位平板 + HID 键盘（触屏当鼠标、虚拟键盘） | `hv_winvm_tablet()`、`hv_winvm_key_hid()`、`devices/xhci.c` |
| ramfb 画面 | `devices/fb_display.c`（`etc/ramfb`，EDK2 GOP 配） |
| 稀疏磁盘 | `host/hvsparse.c`、`Installer` 里 `truncate` 的用法 |
| 不占 Jetsam footprint 的客户机 RAM | `hv_alloc_no_footprint()` |
| 断电/停止、截图、状态 | `hv_winvm_stop()`、`hv_winvm_save_png()`、`hv_winvm_engine_stats()` |

**缺的（要做的，按依赖排）**

| # | 缺什么 | 规模 | 说明 |
|---|---|---|---|
| 1 | **`devices/fb_metal.m`** | 中 | 呈现器根本不存在，`winvm` 现在链接不过。没有它什么都看不见 |
| 2 | **第三个只读介质槽** | 小 | `HvWinVmConfig` 加 `disk2`/`media[]`，`hv_winvirt_create` 加一个 slot |
| 3 | 启动源选择 | 小 | 阶段 2 要从 NVMe1 启动。BDS 走 NVRAM：直接预置 `Boot####`/`BootOrder`，或最土的办法——**PE 盘占 NVMe0 槽位**（但我们又要求系统盘在 NVMe0…）。建议写 `BootNext` 到 vars flash |
| 4 | Swift 侧阶段机 + 文件校验 | 中 | 页 2/3/4 改造、三份文件校验、unattend 生成 |
| 5 | virtio 驱动小盘生成 | 小 | 从 ISO 抽 `.inf/.sys/.cat` 合成 FAT32 镜像（~80 行） |
| 6 | ESD 盘（按 §4.2 选型） | 小–中 | C 几乎为 0，B 约 600 行 |

**没有热插拔**：所有设备在 `hv_winvirt_create` 时一次性建好（`pcie.c` 有 `hv_pcie_attach`，但没有运行时插拔逻辑）。所以阶段 3→4 的"卸载 ISO、换 virtio 盘"**靠重建 VM**——正好你设计里就是"自动重启"，两者天然对齐，不用做热插拔。

---

## 7. 失败与回滚

| 阶段 | 失败 | 处理 |
|---|---|---|
| 1 | 文件不合格 | 只标红那一行，可换 |
| 2 | DiskGenius/WinNTSetup 出错 | 不动镜像，可重来；"回到准备"按钮 |
| 2 | 装到一半用户走了/App 被杀 | 盘是稀疏镜像，下次进来接着用；不自动删 |
| 3 | OOBE 卡住（unattend 有误） | 提供"回阶段 2 重装"，并保留上一次的 `unattend.xml` 便于对比 |
| 3 | 黑屏/无画面 | 判定为 ramfb 或呈现器问题，落日志 + 保留串口最后一行 |
| 4 | 驱动没装上 | 不影响开机；可手动再挂 virtio 盘，条目仍算"可用" |

---

## 8. 需要你拍板的四件事

1. **ESD 怎么进 guest**：A（用户打 UDF ISO，风险未验）/ **B（我们解析 ISO 再合成 exFAT，稳但~600 行）** / C（用户给 exFAT 镜像，最省）/ D（放弃 ESD 走 Setup）。
   我的建议：**C 起步，留 B 作为"以后帮用户自动做"的升级**。
2. **PE 给 ISO 还是 FAT32 镜像**：我强烈建议 **FAT32 镜像**（`.img`），理由见 §4.1。
3. **32 GB / 6 GB 是不是定值**：32 GB 是默认还是最小？6 GB 内存在 8 GB 机型上会被 iOS 压得很紧（有 `hv_alloc_no_footprint` 兜底，但 Jetsam 仍可能杀），要不要按机型上限自动降？
4. **驱动自动安装的载体**：`unattend` 的 `FirstLogonCommands`（少一张盘，但要求阶段 3 就能看到 D 盘）还是**独立的自动执行盘**（阶段 4 挂进去，逻辑更清楚）？
   我的建议：**独立自动执行盘**，阶段 4 的边界最干净——挂盘 = 开始干活，拔盘 = 结束。

---

## 9. 落地顺序（建议）

1. `devices/fb_metal.m`（不解决就什么都看不见）
2. 三介质槽 + 启动源选择（阶段 2 能起来）
3. Swift 侧四阶段状态机 + 页 2/3/4 改造（能走完全流程）
4. virtio 驱动小盘生成（阶段 4 自动化）
5. ESD 盘（按决策 1）→ 之后才是真机冒烟
