# 交接文档 — Henge 16 KB→4 KB 页改造 / Windows on ARM64 方向

> 写给接手的 AI 或人。读完这一份就能继续，不需要翻聊天记录。
> 最后更新：2026-11-10

---

## 0. 一分钟版本

**项目**：Henge，在 iPhone/iPad 上不依赖 hypervisor、自己写 JIT + 软件 MMU 来跑 macOS 的 arm64 虚拟机。

**这一轮做了什么**：把软件 TLB 的页粒度从 **16 KB 改成 4 KB**，为将来跑 Windows 11 ARM64 铺路（Windows 强制 4 KB 页，无法配置）。

**状态**：✅ 完成并验证。`tests/test_mmu.c` 117 checks / 0 failures。5 个提交，工作区干净。

**没做的**：❌ IPA 打不出来（本机无 iOS SDK）。❌ Windows 机器模型（`devices/virt.c`、ACPI、UEFI）一行没写。

**下一步最该做的**：先出 IPA，在真机上确认 macOS guest 没被 4K 改动跑坏。理由见 §7。

---

## 1. 环境与代码位置

| 项 | 值 |
|---|---|
| 代码根目录 | `/root/henge_extract/Henge-main` |
| 原始压缩包 | `/root/Henge-main.zip`（基线来源，可随时重新解压对照） |
| Git | 已初始化，5 个提交，**无远程** |
| 基线提交 | `7635f0e Import Henge upstream (baseline, unmodified)` |
| 当前 HEAD | `5c3cb81` |
| 宿主机 | iSH / Alpine 3.21 / aarch64 / musl / gcc 14.2 |
| 内存 / 磁盘 | 4 GB / 剩约 34 GB |

**看全部改动**：`git diff 7635f0e HEAD --stat`

```
compat/TargetConditionals.h     +21   新增
compat/henge_pthread.h          +38   新增
compat/henge_ulock.h            +64   新增
compat/libkern/OSCacheControl.h +29   新增
compat/mach/mach.h              +69   新增
compat/mach/mach_time.h         +31   新增
compat/mach/vm_remap.h          +94   新增
compat/build-and-test.sh        +72   新增
core/cpu.h                      +19 -?  TLB 粒度常量
core/mmu.c                      +65 -?  tlb_fill 拆分 + 3 处硬编码修复
core/sysreg.c                   +18 -?  TLBI 粒度通用化
tests/test_mmu.c                +81 -?  按 4 KB 索引
```

---

## 2. 关键约束：iOS SDK 缺失

**这台机器装不了 IPA。** 不是 clang 版本问题，是缺 SDK：

- 没有 `iPhoneOS.sdk`、没有 Swift 工具链（`ios/Henge/*.swift` 是 SwiftUI 主体）
- 没有 iOS 链接器 / 运行时 / 代码签名工具
- Alpine 3.21 仓库最高 clang19；edge 有 clang23 但依赖 llvm23（edge 仓库没有）
- LLVM 官方 22/23 的 Linux-ARM64 完整包约 1 GB（zst），本机只有 4 GB 内存

**所以**：`core/`（C 语言）能在本机编译测试并验证；`ios/`（Swift + Metal）**一行都编译不了**。产出安装包必须走 macOS。用户已定：CI 用 `macos-latest` + 一步显式 `xcode-select` 切到最新可用 Xcode。

原仓库自带 `.github/workflows/build-henge.yml` 可以直接用，但它依赖另一个仓库 `Leviidev/Husk`（拉 StikJIT.xcframework、libslirp、glib 的 iOS 预编译库），流程较重。

---

## 3. 已完成的改动（逐条）

### 3.1 `core/cpu.h` — 粒度常量

```c
#define HV_TLB_PAGE_BITS 12   // 原 14（16 KB）
#define HV_TLB_BITS      14   // 保持不变
```

**为什么 `HV_TLB_BITS` 不跟着提高**：提到 16 的话 TLB 从 2 MB 变 8 MB，`HvCpu` 整体位移，`enter.S` 里那些硬编码偏移（`CPU_PC 0x100`、`CPU_EXIT 0x110`…）和 `_Static_assert` 全部要重算，测试里“整个 HvCpu 放栈上”会爆栈。

**代价**：TLB 覆盖范围从 256 MB 降到 64 MB（16384 项 × 4 KB）。对 Windows 够用，但不是无损替换。**如果将来发现 TLB 抖动明显，这就是第一个要调的地方**（提高 `HV_TLB_BITS` 并同步 `enter.S` 偏移）。

### 3.2 `core/mmu.c` — `tlb_fill` 拆成两个函数

原来 `tlb_fill` 假定“guest 页 == TLB 页”，且明确写着：

```c
if (w->block_size < (1ull << HV_TLB_PAGE_BITS)) return; /* 小页只走慢路径 */
```

现在 guest 页可能**大于** TLB 页（Linux 16K/64K 内核、Windows 的 2 MB 大页），一次 walk 要填多个 4 KB 项：

- `fill_one(cpu, va, el, w, page_pa)` — 填一个 4 KB 项（原 `tlb_fill` 的主体）
- `tlb_fill(...)` — 填 `va` 所在页，若映射覆盖更多且 ≤ `HV_FILL_AHEAD`(16)，顺带填后续页
- 1 GB block 不预填（262144 项，比 TLB 还大，填了反而把有用的挤掉）

### 3.3 `core/sysreg.c` — TLBI 粒度不再写死 16 KB

原来：

```c
if (by_range && ((xt >> 46) & 3) == 2) {   // 只认 TG=2（16 KB）
```

现在按 Xt 里的 TG 解出 shift（4K/16K/64K 都支持），并把页数换算成 TLB 页：

```c
uint32_t rshift = (tg == 1) ? 12 : (tg == 2) ? 14 : (tg == 3) ? 16 : 0;
...
npages = gpages << (rshift - HV_TLB_PAGE_BITS);
```

**意义**：4 KB 粒度的 guest（Windows）也能走“一次性远程失效”的快路径，而不是退回逐页。

### 3.4 `core/mmu.c` — 另外三处写死的 16 KB

这三个是**真实的 bug**，不是测试问题。用全量插桩一次看出来的（见 §8，那个方法很有用）：

| 位置 | 原来 | 为什么错 |
|---|---|---|
| `hv_ijc_drop_range` | `c << 11` | 32 MB chunk 换算成 TLB 页，硬编码 2048（=32MB/16KB） |
| `hv_ijc_drop_range` | `n < 4096` / `n >= 4096` | 页数预算，应随页大小走 → 改成 `max_pages = 64MB >> HV_TLB_PAGE_BITS` |
| `WALK_PAGE_BITS` | `0x00ffffffffffc000` | 16 KB 掩码。范围失效拿 4 KB 页去和 16 KB 掩码比，漏掉范围里的 walk |

**这个 bug 的表现很隐蔽**：`walks_elsewhere` 判定“没有冲突”，于是把目标 CPU 正在走的页表**当成安全的**，直接远程失效——本来应该返回“竞争、重试”。

### 3.5 `core/mmu.c` — `hv_tlb_flush_kernel_range` 阈值

```c
- if (len <= (4096ull << HV_TLB_PAGE_BITS))   // 绑 TLB 粒度
+ if (len <= (4096ull << HV_TLB_PAGE_BITS))   // 保留，但注释说清楚是“4096 个 TLB 页 = 16 MB”
```

这个当初以为要改，复查后**保留了原值**：它是“页数”预算而非跨度，4096 个 4 KB 页 = 16 MB 仍然合理。

### 3.6 `tests/test_mmu.c`

把测试里“TLB 页”的地方全部改用 `HV_TLB_PAGE_BITS` 索引。**注意区分两种常量**：

| 该改的 | 不该改的 |
|---|---|
| TLB 槽位索引 `(va >> HV_TLB_PAGE_BITS) & (HV_TLB_SIZE-1)` | TCR 寄存器的 TG0 位域（固定在 bit 14） |
| 手工构造 TLB 项时的页号 | TLBI 的 Xt 操作数里的 TG 字段（描述 **guest** 粒度） |
| `code_pages[]` / `ijc_va[]` / `xhead[]` 索引 | 测试里 `ks[]` 数组的页号（那是 guest 页） |

**这个区分是踩坑重点**：我最初用批量替换 `<< 14` → `<< HV_TLB_PAGE_BITS`，把 TCR 位域编码也改坏了，失败数从 21 涨到 33。

---

## 4. `compat/` 移植层（可随时删除）

**这不是 Henge 的代码，是为了在非 Apple 主机上编译而加的垫片。** 上 macOS 时完全用不到，`make test` 走原路径。

| 文件 | 替代 |
|---|---|
| `mach/mach_time.h` | `mach_absolute_time` → `gettimeofday` × 24 MHz（保持 24 MHz，因为 `sysreg.c` 和 `dispatch.c` 都假设这个频率） |
| `mach/mach.h` | `mach_port_t`、`thread_basic_info_data_t`、`time_value_t`（只给 `--stats` 用） |
| `mach/vm_remap.h` | `vm_remap` → 临时文件 + 双 `mmap`（**memfd_create 在 iSH 上运行失败**，实测过） |
| `TargetConditionals.h` | 全 0，走通用分支 |
| `libkern/OSCacheControl.h` | `sys_icache_invalidate` → `__clear_cache` |
| `henge_pthread.h` | `pthread_cond_timedwait_relative_np`（Darwin 扩展） |
| `henge_ulock.h` | `__ulock_wait/__ulock_wake` → futex（**功能代码，不是诊断**：跨 CPU 同步用） |
| `build-and-test.sh` | 一键构建 + 跑 `test_mmu` |

### 编译要点（脚本已封装，手敲时容易漏）

```sh
-D_GNU_SOURCE                        # musl 才暴露 Dl_info
-include compat/henge_pthread.h      # 必须前置，core/ 不#include
-include compat/henge_ulock.h        # 同上，machine.c 是 extern 声明
-Icompat                             # 让 <mach/...> 命中垫片
-latomic                             # memop.c 有 128 位 CAS（CASP）
-mgeneral-regs-only                  # 只给 mmu.c，refill 时 guest 向量还活着

# enter.S：Darwin 符号带下划线，ELF 不带
objcopy --redefine-sym _hv_enter=hv_enter ... enter.raw.o enter.o
```

**已知环境坑**：
- `memfd_create` 编译过、运行返回失败（iSH 内核限制）→ 用临时文件替代
- 在 `/root` 下创建名为 `port` 的目录会触发 iSH 幽灵目录 bug（`mkdir` 说存在、`ls` 说没有）→ 换名，这次用的 `compat/`
- `cd tests/` 之类的名字也踩过同类 bug

### 跑测试

```sh
cd /root/henge_extract/Henge-main
sh compat/build-and-test.sh        # → 117 checks, 0 failures
```

---

## 5. 验证状态：哪些可信，哪些不可信

| 测试 | 本机结果 | 可信吗 |
|---|---|---|
| **`test_mmu`** | **117 checks, 0 failures** | ✅ **可信**，纯 C，无 Apple 依赖 |
| `test_mem` | 2 个 FAIL（`pairs`、`exclusive`） | ❌ **不可信**。用**原版代码**跑出一模一样的 FAIL，是本机无 JIT 导致只走了 C 慢路径 |
| `test_classify` / `test_operands` / `test_jit` / `test_exc` / `test_devices` / `test_shutdown` | 未跑 | ❌ 依赖 `tests/*.S`，用 Mach-O 语法（`@PAGE`/`@PAGEOFF`）和符号前缀，GNU as 编不过 |
| Swift 测试（`test-display-routing` 等） | 未跑 | ❌ 无 Swift 工具链 |

**转换 `*.S` 到 ELF 的方法是可行的**（我试通过：`@PAGEOFF` → `:lo12:`、`@PAGE` → `:pg_hi21:`，修饰符要在符号**前面**；再 `objcopy` 去前缀；`tests/harness.h` 里的宏也要改）。但 `test_mem` 即使编出来也需要 JIT 才能验 JIT 路径，本机没有。

**所以：`test_mmu` 是唯一能作为回归依据的测试。** 其余必须到 macOS 上跑 `make test`。

---

## 6. 为什么改 4 KB（背景，接手必读）

Windows on ARM64 **强制 4 KB 页，无法配置**。而原实现：

```c
// docs/architecture.md 原话
// "Guest and host both use 16 KB pages, so guest RAM maps onto host pages directly."
```

**好消息**：`hv_mmu_walk()` 本来就完整支持 4K/16K/64K（粒度从 guest TCR 读，`granule_bits()`），所以页表遍历一行没改。真正卡住的只有 TLB 缓存层。

**另一个关键发现**：virtio-win 有 ARM64 驱动（`viostor`、`NetKVM`、`vio-gpu`、`vioscsi`），所以 Windows 的图形不是死路——之前我判断错了。

---

## 7. 下一步：两条路，建议先走 B

### A. 继续推进 Windows guest（大工程，数月起）

按顺序：

1. **`devices/virt.c`** — 照 QEMU `hw/arm/virt.c` 的内存图重摆现有设备。`gicv3.c`、`pl011.c`、`pcie.c`、`virtio_blk.c`、`virtio_net.c`、`virtio_balloon.c` **一行不用改**，因为它们按 QEMU 的 PCI 约定写，正好是 virtio-win 驱动认的那套（`devices/virtio.c:150` 生成的是现代 virtio 设备 `0x1040+id`）。
2. **ACPI 表生成** — Windows 不认 DTB，必须 `DSDT`/`MADT`/`GTDT`/`SPCR`/`IORT`。照 QEMU 抄约 800–1500 行。
3. **UEFI** — `ArmVirtQemu`（`edk2-aarch64-code.fd`）+ 两片 pflash（CODE 只读 / VARS 可写）。需要加 flash 设备模型，约 300 行。
4. **`devices/virtio_gpu.c`** — Linux 和 Windows **共用同一份**。virtio-gpu 是 2D 命令流，比 Apple PVGPU 简单一个数量级。`pvg_metal.m`（2883 行 Metal）整体废弃，`pvg_lite.c` 只保留 `display_*` 那约 80 行。
5. **删除 Apple 专属**：`ios/Henge/BootROM.swift`、`third_party/idevicerestore*`、`devices/cfg.c`、`devices/bdif.c`。

**已知限制**：
- 没有 EL2（`core/exception.c:38` 明确 `EL2/EL3 do not exist`）→ Windows 的 Hyper-V / WSL2 / VBS 用不了
- 不支持 AArch32 → 32 位 guest 不可能
- 4 KB 页下内联 TLB 仍然工作（`HV_TLB_ADJ 15` 的技巧在 4 KB 页里依然成立），**这次改造没损失快路径**

### B. 先出 IPA 验证回归（推荐先做）

**理由**：TLB 粒度改得很深（尤其 `WALK_PAGE_BITS` 那处影响远程失效的正确性），必须在真机上确认 macOS guest 没跑坏。Windows 还远，macOS 是现在就能验证的。

做法：写 `.github/workflows/`，`runs-on: macos-latest`，加一步 `xcode-select` 切到最新 Xcode，然后复用仓库自带的 `.github/workflows/build-henge.yml` 流程（需要 `Leviidev/Husk` 仓库拉预编译依赖）。产出未签名 IPA，用 SideStore/AltStore 侧载。

---

## 8. 调试方法论（这次最有效的经验，值得沿用）

**用户建议的“全量插桩”是这次解决问题的关键。** 具体做法：

1. 在**原版代码**（基线提交）上做全量插桩：函数入口打印所有参数、每个分支打印判定、每个返回点打印返回值
2. 跑测试，把输出重定向到文件
3. **同一套测试分别跑原版和改动版，`diff` 两次的调用序列**

这次就是靠 `diff` 一眼看出：

```
原版:  npages=2 → span=32768   (2 × 16 KB)
新版:  npages=2 → span=8192    (2 × 4 KB)
```

**反面教训**：一开始我靠“改测试让测试通过”，结果失败数在 21 → 33 → 15 → 25 之间反复横跳，因为改了测试就无法区分“代码错”还是“测试错”。插桩把两个变量彻底分开了。

**另一个关键操作**：先提交基线（`7635f0e` 是未改动的 upstream），这样任何时候都能 `git stash` / `git checkout <基线> -- <文件>` 做 A/B 对照。

---

## 9. 交接检查单

接手后第一件事：

```sh
cd /root/henge_extract/Henge-main
git log --oneline          # 应该 5 个提交，HEAD = 5c3cb81
git status --short         # 应该为空
sh compat/build-and-test.sh  # 应该输出 117 checks, 0 failures
```

如果 `117 checks, 0 failures`，说明环境正常，可以直接继续。

如果编译报错，检查：
- `compat/` 七个文件是否都在（`git ls-files compat/` 应有 8 个含脚本）
- 是否漏了 `-include compat/henge_pthread.h -include compat/henge_ulock.h`
- `enter.S` 是否经过 `objcopy --redefine-sym`

---

## 10. 用户偏好（此前会话中明确过的）

- **改完就 commit，不要问**；但**绝不 push**，除非明确说
- 不要空谈方案，**直接改代码**
- 节省 token：不要反复试探、不要无谓的探索；先看文件再动手
- CI 用 `macos-latest` + 显式 Xcode 切换
- 遇到不确定的地方，宁可问一句（用 `user_ask`），也不要白跑一次 CI
