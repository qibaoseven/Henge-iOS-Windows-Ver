# 固件：那两块 pflash 从哪来

板子（`devices/winvirt.c`）是按 QEMU 的 `virt` 机器搭的，所以固件也是那套：
**EDK2 ArmVirtQemu，AARCH64，两块 64 MB 的 pflash。**

| 文件 | 内容 | 板子里 |
|---|---|---|
| `code.fd` | UEFI 代码，只读，64 MB | `WINVIRT_FLASH0_PA` = `0x0`，CPU 复位从 `0x0` 开始跑 |
| `vars.fd` | NVRAM（BootOrder、secure boot 变量都在里面），可写，64 MB | `WINVIRT_FLASH1_PA` = `0x04000000` |

`devices/pflash.h` 里一个 bank 就是 64 MB，和 QEMU 一样；EDK2 的变量驱动按
`PcdFlashNvStorage*` 的相对偏移找 varstore，大小不对会在打印任何东西之前就 assert。

## 怎么得到

```sh
./tools/build-firmware.sh --package   # 用发行版编译好的（秒级）
./tools/build-firmware.sh --source    # 从 edk2 源码编（分钟级，可复现）
```

产物在 `out/firmware/{code.fd,vars.fd}`，脚本会把它们补齐/截断到正好 64 MB，并做两件
检查：代码银行头部有没有 `_FVH`（固件卷头），变量银行是全 `0xFF`、全 `0x00` 还是
已经有 varstore 模板（全 `0x00` 是坏的——EDK2 没法格式化它）。

发行版权宜：

| 系统 | 命令 | 产物目录 |
|---|---|---|
| Debian / Ubuntu | `sudo apt install qemu-efi-aarch64` | `/usr/share/AAVMF/` |
| Fedora | `sudo dnf install edk2-aarch64` | `/usr/share/edk2/aarch64/` |
| macOS | `brew install qemu` | `share/qemu/edk2-aarch64-code.fd` |
| 都没有 | GitHub Actions 的 `macos-latest` runner 上跑 `--source` | 见下 |

## 让 CI 编好自己推上去（不用配任何密钥）

`.github/workflows/firmware.yml` 用 `macos-latest`（Apple Silicon，AArch64 原生 XCODE5，
没有交叉编译器），每次手动触发或用 `tools/build-firmware.sh` 有改动时自动跑：

1. 缓存 `third_party/edk2`，避免每轮重拉 200 MB；
2. `./tools/build-firmware.sh --source` 编 ArmVirtQemu；
3. 校验两个 bank 都正好 64 MB、`code.fd` 开头有 `_FVH`，算 SHA256；
4. **发布到固定的 `firmware-latest` Release**（`--clobber` 覆盖，不会每轮堆 128 MB 历史）。

不需要手工密钥：固件本身不签名（Secure Boot 关着），推 Release 用的是每个工作流都带的
`secrets.GITHUB_TOKEN`，只需要文件里那句 `permissions: contents: write`。

拉回来：

```sh
gh release download firmware-latest -p '*.fd' -D out/firmware
gh release download firmware-latest -p 'SHA256SUMS' -D out/firmware
shasum -a 256 -c out/firmware/SHA256SUMS
./tools/stage-app-resources.sh      # 拷进 ios/Henge/Resources/
```

没有 `gh` 也行：Release 页面上直接下 `code.fd` / `vars.fd`，放进 `out/firmware/`。

## 固件怎么找到设备

两条路都要给，少一条就会有一半的固件启动不了：

1. **x0**：`hv_winvirt_reset_cpu` 把 DTB 拷到 `0x40000000`，`x0` 指向它。ArmVirtQemu 的
   PrePi 从这里拿内存映射。
2. **fw_cfg `etc/fdt`**：`devices/winvirt.c` 里新加的，EDK2 的 `FdtQemuFwCfgLib` 也认这条。
   QEMU 自己也是两份都给。

ACPI 走的是 QEMU 那套 **fw_cfg + table-loader**：`hv_acpi_build_and_register()` 把
DSDT/MADT/PPTT/GTDT/MCFG/SPCR/DBG2/IORT 放进 `etc/acpi/tables`，再用
`etc/acpi/table-loader` 做重定位和校验和。EDK2 拿到就会把它们装进配置表，Windows
启动时读的是 ACPI，不是 DTB——所以这条链路必须通，DTB 只是给固件自己用的。

## 现在验证到哪一步

- 固件二进制**没有**：这台机器上没有 EDK2 构建产物，脚本也没跑过（要 aarch64 工具链）。
- 交接代码是**看过并补过**的：DTB 两条路都有，ACPI 用 loader，`etc/fdt` 是这轮加的。
- 第一次真正跑起来，串口（`b.uart->output = serial_out`）会打印 EDK2 的版本行；
  如果没有，先查 pflash 大小，再查 `etc/fdt` 里的内存节点和 `WINVIRT_RAM_PA` 对不对。
