# 交接文档 — Henge：转向 Windows 11 ARM64

> 写给接手的 AI 或人。读完这一份就能继续，不需要翻聊天记录。
> 最后更新：2026-11-10

---

## 0. 一分钟版本

**项目**：Henge，在 iPhone/iPad 上不依赖 hypervisor、自己写 JIT + 软件 MMU 来跑 arm64 虚拟机。

**这一轮做了什么**：把项目从"跑 macOS"彻底转向"跑 Windows 11 ARM64"。macOS 已被主动放弃，不再是验收目标。

**当前状态**：真机 UEFI 已经跑起来，串口打印出 ArmVirtQemu 的横幅；PCIe 与 virtio 的 MSI-X 打通（Windows 的硬前提）。下一步是 ACPI 表。

| 项 | 值 |
|---|---|
| 代码根目录 | `/root/henge_extract/Henge-main` |
| Git | 11 个提交，**无远程**，工作区干净 |
| 基线提交 | `7635f0e Import Henge upstream (baseline, unmodified)` |
| 当前 HEAD | `1d6978e` |
| 构建 | `sh compat/build-and-test.sh` |
| 测试 | 117 / 62 / 26 / 34，全 0 failures |

---

## 1. 方向：为什么抛弃 macOS

**user 明确的要求**：全面抛弃 macOS，专供 Windows 11 ARM64。

- 4K TLB 改造是**故意的**，不是兼容性副作用。
- macOS guest 强制 16 KB 页，所以 4K 改造对 macOS 是净负收益（16384 项 × 4 KB = 64 MB，
  而基线注释写着「xnu 的工作集曾溢出 64 MB」）。**这个退化是刻意接受的。**
- **不要再提「出 IPA 验证 macOS 回归」**——那是反向建议。

### 真实固件冒烟结果

用 Debian 的 AAVMF（ArmVirtQemu 2025.02）跑通了：

```
UEFI firmware (version 2025.02-8+deb13u1 built at 18:06:17 on Nov  8 2025)

Synchronous Exception at 0x00000000478F497C
```

横幅能打印，说明：flash 作为 RAM 可执行、DTB 被接受、PL011 通路正常、PSCI hvc 没静默死。
之后那条异常**尚未处理**，但它落在 `0x478F497C`（RAM 区），说明固件已把自己复制到 RAM。
`--verbose` 可打开 `m->log = 1` 的设备访问日志。

---

## 2. 环境与构建

| 项 | 值 |
|---|---|
| 宿主机 | iSH / Alpine 3.21 / aarch64 / musl / gcc 14.2 |
| 内存 / 磁盘 | 4 GB / 约 34 GB |
| **JIT** | **这台机器有 JIT**（实测能执行真实 arm64 指令），早先的判断是错的 |

```sh
cd /root/henge_extract/Henge-main
sh compat/build-and-test.sh        # 四套测试
sh tools/run-uefi.sh               # 真实固件（需先放 firmware/*.fd）
```

**编译要点**（脚本已封装）：
- `-D_GNU_SOURCE`、`-include compat/henge_pthread.h -include compat/henge_ulock.h`
- `-Icompat -Icore -Ihost -Idevices`（**`jit_region.h` 在 `host/` 下**，漏了编不过）
- `-latomic`、`core/mmu.c` 要加 `-mgeneral-regs-only`
- `enter.S` 要 `objcopy --redefine-sym _hv_xxx=hv_xxx`

### 固件从哪来

```sh
# Debian/Ubuntu
apt install qemu-efi-aarch64
cp /usr/share/AAVMF/AAVMF_CODE.no-secboot.fd  firmware/edk2-aarch64-code.fd
cp /usr/share/AAVMF/AAVMF_VARS.fd             firmware/edk2-arm-vars.fd
```

**必须用 `no-secboot`**：secboot 版会验证不存在的密钥然后停下。
两个文件在 `firmware/` 下，已被 `.gitignore` 排除（各 64 MB，不进仓库）。

---

## 3. 已完成的改动（按提交顺序）

### 3.1 `c1a708e` → `0aa6442`：4K TLB

软件 TLB 页粒度 16 KB → 4 KB。

| 位置 | 改动 |
|---|---|
| `core/cpu.h` | `HV_TLB_PAGE_BITS 14→12`；`HV_TLB_BITS` **保持 14**（避免 `HvCpu` 布局位移，`enter.S` 的硬编码偏移不用重算） |
| `core/mmu.c` | `tlb_fill` 拆成 `fill_one` + `tlb_fill`，支持 guest 页 > 4K 时填多个项 |
| `core/mmu.c` | 修 3 处写死的 16 KB：`hv_ijc_drop_range` 的 `c << 11`、页数预算 4096、`WALK_PAGE_BITS` 掩码 |
| `core/sysreg.c` | TLBI 范围失效支持 4K/16K/64K 三种 guest 粒度 |

**代价（已接受）**：覆盖范围 256 MB → 64 MB。

### 3.2 `5c3cb81`：`compat/` 移植层

七个垫片 + `build-and-test.sh`。**现在不是临时的，是唯一构建路径**（macOS 目标已放弃）。

| 文件 | 替代 |
|---|---|
| `mach/mach_time.h` | `mach_absolute_time` → `gettimeofday` × 24 MHz |
| `mach/mach.h` | `mach_port_t`、线程信息（只给 `--stats` 用） |
| `mach/vm_remap.h` | 临时文件 + 双 `mmap`（memfd 在 iSH 上失败） |
| `TargetConditionals.h` | 全 0 |
| `libkern/OSCacheControl.h` | `__clear_cache` |
| `henge_pthread.h` / `henge_ulock.h` | pthread 扩展 / futex |

### 3.3 `fcfb8fb`：virt 平台地基

- **`devices/pflash.c`**：双片 nor flash @ `0x0`，各 64 MB，code 只读 / vars 可写。
- **`devices/fw_cfg.c`**：`0x0902_0000`，**大端** selector + data，签名/id/file-dir/file 条目。**无 DMA**（user 拍板）。
- **`devices/virt.c`**：`hw/arm/virt.c` 的 `base_memmap[]` 逐字照搬。

### 3.4 `d620d1c`：DTB + 让 flash 能执行

- **`devices/dtb.c`**：手写 FDT（header + 空 rsvmap + 结构块 + 字符串表）。`dtc` 反编译**零警告**。
- **架构修正**：pflash 必须是 **RAM**，不能做成 MMIO 设备——JIT 只从 `m->ram[]` 取指。
  `vmapple.c` 早就写对了：`/* QEMU maps the firmware as RAM, and so do we. */`
- **`host/main_uefi.c`** + **`tools/run-uefi.sh`**。

### 3.5 `508ee1c`：脚本修复与固件说明

### 3.6 `5a170c3`：ITS + RET 快速路径默认关闭

- **`devices/its.c`**：命令队列（MAPD/MAPC/MAPTI/MOVI/DISCARD）、`GITS_TRANSLATER`。
- **`gicv3.c`**：LPI 复用 SPI 表（`8192 + 256`），新增 `hv_gicv3_pend_cpu`。
- **RET 快速路径默认关闭**（`HV_RAS=1` 可开回）。见 §6。

### 3.7 `1d6978e`：PCIe 多 bus + virtio MSI-X

- **`pcie.c`**：ECAM **16 条总线**（16 MB ÷ 1 MB/bus）、type-1 host bridge、BAR 探测与重定位、
  MSI-X(0x11) 与 PCIe(0x10) 能力。
- **`virtio.c`**：MSI-X，config 用 vector 0、队列 q 用 vector q+1，**INTx 彻底废除**。

---

## 4. 当前内存布局

```
0x0000_0000  pflash code   64 MB  只读 RAM（UEFI 入口，PC 复位至此）
0x0400_0000  pflash vars   64 MB  可写 RAM（UEFI 变量，可存盘回读）
0x0800_0000  GIC dist
0x0808_0000  GIC ITS       （M2 已实现）
0x080A_0000  GIC redist
0x0900_0000  PL011
0x0901_0000  PL031
0x0902_0000  fw_cfg        0x18
0x0903_0000  PL061
0x1000_0000  PCIe MMIO     0x2EFF_0000
0x3F00_0000  PCIe ECAM     16 MB = 16 条总线
0x4000_0000  DTB           1 MB 预留
0x4010_0000  客户机 RAM 起点（= VIRT_GUEST_RAM）
0x4800_0000  ACPI 预留     1 MB（M2 用）
```

复位：`pc = 0x0`，`x0 = 0x4000_0000`（DTB），从核 `off=1` 等 PSCI `CPU_ON`。

---

## 5. 测试：哪些可信

全部在非 Apple 主机上跑，`sh compat/build-and-test.sh` 一键。

| 测试 | 结果 | 覆盖 |
|---|---|---|
| **`test_mmu`** | 117 checks | TLB 粒度、walk、TLBI |
| **`test_virt`** | 62 checks | pflash（RAM 语义）、fw_cfg 大端、virt 复位 |
| **`test_pcie`** | 26 checks | 能力链、BAR 重定位、MSI-X → LPI 全链路 |
| **`test_dtb`** | 34 checks | 反向解析 DTB，验证节点与 `/psci` = hvc |

**不可信**：`test_mem`（本机跑出一模一样的 FAIL，与代码无关）、其余依赖 `tests/*.S`
（Mach-O 语法）或 Apple 框架的测试。

**外部校验手段**（很有用，别丢）：
```sh
dtc -I dtb -O dts /tmp/v.dtb     # 校验 DTB，应零警告
iasl -d dsdt.aml                  # 校验 ACPI（M2 用，本机已装：apk add iasl）
```

---

## 6. 未解决的问题：RET 快速路径

**现象**：`HV_RAS` 开着时，AAVMF 在自己的 MMU 初始化里死循环
（`0x520C → 0x5274 → 0x5184 → ...`）；关掉后同一份固件跑过 MMU bring-up 进入 RAM 副本。

**已确认不是**：中断没注入（手动 pend SPI 后 `irq_line=1`）、GICR RWP、TBI 未开
（`TTBR0=0xff000000006000`，低 48 位 `0x6000` 合法）。

**理论分析说它应该是安全的**：tag = `salt ^ x30`，salt 高 48 位是计数器，tag 的高 16 位
编码了 x30 的高位，碰撞应被 tag 挡住。**但实测就是不安全**——推理与实测矛盾时以实测为准。

**为什么不能简单放大表**：`HV_RET_BITS=20` 会撑爆 `HvCpu` 的静态断言（和 `HV_TLB_BITS` 同类问题）。

**建议的下一步**：加一个不改布局的 `RET` 校验（在 `hv_ret_clear` 里记录 x30 高位并比对），
抓出真实的碰撞现场，再做方案选择（哈希索引 / 校验失败回退 / 保持禁用）。

代码位置：`core/translate.c:put_return` / `put_call`，`core/machine.c:knobs_init`。

---

## 7. 踩过的坑（都是真 bug，别再犯）

### 平台 / 工具链
1. **短 `fread()` 会把缓冲区剩余部分清零**（musl 行为）。加载 flash 镜像时会把 erased 的
   `0xff` 变成 `0x00`。**改用 `read(2)` 循环。**
2. **iSH 的 `ar x` 解 deb 会损坏成员**（解出的 `data.tar.xz` md5 与包内数据不符）。
   用 Python 按 ar 格式手工切：8 字节 magic + 每成员 60 字节头（`name[16]`、`size[48..58]`）。
3. `HvPciDev` 加字段必须追加到**结构体末尾**，插在 `wmask` 后面会把 `config[]` 偏移搞乱
   （现象：设备 ID 变成 `0x10011`）。

### MMU / JIT
4. **MMIO 不能作为代码**：`hv_mmu_fetch` 只从 `m->ram[]` 取指。固件必须映射为 RAM。
5. 采样 PC 时注意：`dd` 的 `skip` 偏移与 PC 一致，但手工解码 MRS/MSR 容易错位，
   **用 `objdump -D -b binary -m aarch64`** 才可靠。

### GICv3 ITS
6. **`GITS_CBASER`：valid 在 bit63，size 在 `[7:0]`**（pages−1）。写成 bit0=valid 的话
   会读成"1 页、地址 0"，**ITS 一条命令都不跑**。
7. **命令的 DeviceID 在 word0 的高 32 位**。取低 32 位会导致 MAPD 建错设备、MAPTI 找不到、
   deliver MISSING。
8. **EventID 是 8 位，在 `[23:16]`**。声明成 16 位会让 EventID 与 opcode `[31:24]` 重叠，
   读出的 event 变成 `0xb01`。正确：`typer |= 7ull << 16`。
9. 寄存器解码按 **4 字节**粒度（`off & 0xfff`），不是按页。

### PCIe / virtio
10. **ECAM 16 MB 只装得下 16 条总线**（32 dev × 8 fn × 4 KB = 1 MB/bus）。
    `bus-range` 写 0–255 会让 OS 去读不存在的地址。
11. **能力链追加**必须记住 `cap_next` 并让前一节点的 next 指向新位置。
    复用空位 / 线性扫描两版都会覆盖邻近能力，OS 看到的就是"没有 MSI-X"。
12. **MSI-X 表的 data/ctrl 是 32 位字段**，内部偏移要用 `(w-8)*8` / `(w-12)*8`，
    不能用 `(w&7)*8`，否则 `<<32` 把 32 位值全截断成 0。

### 测试写法
13. MMIO read 返回 `uint64`，断言要显式 `(uint32_t)` 截断，否则 `~0ull` 永远不等于 `0xffffffffu`。
14. 崩溃时 `checks` 计数会归零（BSS 被踩）。**把计数器放堆上**才能区分"崩溃"和"断言失败"。
15. `cpus[0].pc_state` 只有 `hv_virt_reset_cpu()` 之后才有效，之前是 NULL。

---

## 8. 下一步：M2 的 ACPI

**目标**：Windows 安装镜像能识别 virtio-blk 并开始安装。

### 需要生成的表

| 表 | 关键内容 |
|---|---|
| RSDP / XSDT | 根指针，放 `0x4800_0000` |
| **FADT** | **PSCI conduit 必须是 HVC**（没有 EL3，写 SMC 会静默死） |
| **MADT** | GICD / GICR / **GIC ITS 的 MMIO 地址与 GSI 基址** |
| GTDT | 系统计数器频率、EL1 定时器中断号 |
| **IORT** | PCIe root complex → ITS 的映射（Windows 靠它配 MSI） |
| SPCR | PL011 地址 |
| DSDT | CPU、PCIe host bridge、`\_SB.PCI0` |

### 方案（user 已拍板）

- **自己写极简 AML Builder**（`build_append_int_noprefix`、`build_append_namestring`、
  `build_package` 等十来个基础函数），照 QEMU 的 `aml-build.c`。
  **否决**手写固定字节数组，**否决**移植完整 ACPICA。
- **不做 `table-loader` 重定位**：表放固定物理地址，通过普通 fw_cfg 条目传地址。
- 生成后用 `iasl -d` 反编译，确保零错误。

### 已知约束

- 没有 EL2（`core/exception.c:12`）→ Windows 的 Hyper-V / WSL2 / VBS 用不了。
- 不支持 AArch32。
- 16 条总线、256 个 LPI 是当前上限，够用但不是无限制。

---

## 9. 用户偏好（重要）

- **改完就 commit，不问**；但**绝不 push**，除非明确说。
- **不要重定向输出**，直接打到命令行；需要进度条就用 `curl --progress-bar`。
- **不要 `head` / `tail` 截断**，要看实时全量日志。
- 不要空谈方案，**直接改代码**；能精简则精简，能快则快。
- 遇到不确定的地方，宁可问一句（用 `user_ask`），也不要白跑一次。
- 测试断言**不许放宽**来"通过"回归。当前 `test_mmu` 的断言增删为 0 处，这条底线保持。

---

## 10. 交接检查单

```sh
cd /root/henge_extract/Henge-main
git log --oneline        # 11 个提交，HEAD = 1d6978e
git status --short       # 应为空
sh compat/build-and-test.sh
# 期望：117 / 62 / 26 / 34，全 0 failures
```

如果编译报错，检查：
- `-Ihost`（`jit_region.h` 在那）
- `-include compat/henge_pthread.h -include compat/henge_ulock.h`
- `enter.S` 是否经过 `objcopy --redefine-sym`
- `-latomic`、`core/mmu.c` 的 `-mgeneral-regs-only`

---

## 11. 待清理（尚未删）

`compat/` 是移植层，保留。以下文件与 Windows 方向无关，**尚未删除**，删前确认没有残留引用：

```
devices/vmapple.c .h     Apple 虚拟 Mac 机器模型
devices/cfg.c .h         vmapple 配置区
devices/bdif.c .h        vmapple 后门块设备
devices/aes.c .h         Apple GID 密钥流
devices/pvg_lite.c .h    Apple 半虚拟化 GPU（其 display 出口可借鉴）
devices/pvg_metal.m      Metal 渲染（2883 行）
devices/pvg.h .pvg_colorprobe.h
devices/virtio_snd.c     Apple 声卡路径
devices/virtio_usb.c .h  Apple USB 设备口
devices/xhci.c .h        USB 控制器
ios/Henge/BootROM.swift  AVPBooter 下载器
```
