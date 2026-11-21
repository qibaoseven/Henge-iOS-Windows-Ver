# Licensing

Henge is GPL-2.0-or-later ([LICENSE](../LICENSE)). The shipped binary is a combined
GPL work because usbmuxd and code from Husk are compiled into it, so it cannot go on
the App Store.

| Code | Licence | Where |
| --- | --- | --- |
| Henge | GPL-2.0-or-later | `core/`, `devices/`, `host/`, `ios/`, `tools/` |
| Modifications by qibaoseven | GPL-2.0-or-later | `core/`, `devices/`, `host/`, `tests/`, `tools/`, `compat/`, `docs/` — see [LICENSE-qibaoseven](../LICENSE-qibaoseven) |
| Code from Husk | GPL-2.0-or-later | `ios/Henge/JIT/`, `HengeLog.swift`, `Native/henge-ios-jit.*`, `henge-brk.S` |
| WindowsiOS (ammaarreshi/WindowsiOS), as the source of the Windows side | GPL-2.0 | the firmware identity and ACPI tables in `devices/acpi.c`, the machine description, and part of the Windows UI scaffolding — see [INSTALL-DESIGN.md](../INSTALL-DESIGN.md) |
| UTM (utmapp/UTM), its keyboard accessory as a model | Apache-2.0 | `ios/Henge/Windows/WinKeyboard.swift` |
| virglrenderer, UTM's `neptune` branch | MIT | the 3D path around `devices/gpu_renderer.h` |
| UTM's QEMU fork, as a design reference | GPL-2.0 | the virtio-gpu device, the ISO-as-bytes boot and the Windows-on-ARM machine layout |
| QEMU's vmapple AES device, as a reference | GPL-2.0-or-later | `devices/aes.c` |
| QEMU's `tcg-apple-jit.h` | LGPL-2.1-or-later | `ios/Henge/Native/` |
| usbmuxd | GPL-2.0 or GPL-3.0 | `third_party/usbmuxd` |
| idevicerestore | LGPL (see below) | `third_party/idevicerestore` |
| libimobiledevice, libplist | LGPL-2.1, with GPL-2.0 for their tools | `third_party/` |
| libimobiledevice-glue, libirecovery, libtatsu, libusbmuxd | LGPL-2.1 | `third_party/` |
| libzip | BSD | `third_party/libzip` |
| Mbed TLS | Apache-2.0 or GPL-2.0-or-later | `third_party/mbedtls` |
| StikJIT (the Bitrise build applies `ci/stikjit-high-address.patch`) | MPL-2.0 | `ios/Henge/Resources/legal/` |
| idevice | MIT | `ios/Henge/Resources/legal/` |
| Pairing library's Rust crates | Listed per crate | `ios/Henge/Resources/legal/LICENSES-rppairing-crates.txt` |
| libslirp, pcre2 | BSD | Prebuilt from Husk |
| glib, libintl | LGPL-2.1-or-later | Prebuilt from Husk |

idevicerestore's `COPYING` file is LGPL-3.0, while its source headers say LGPL-2.1
or later. If the `COPYING` file governs, the combined binary is GPL-3.0, which
GPL-2.0-or-later allows.

## Modifications

This tree is a modified version of upstream Henge. Changes made by **qibaoseven**
(core memory/MMU/TLB, PCIe and virtio with MSI-X, GICv3 and ITS, the ACPI tables,
fw_cfg and pflash, the test suite and the build scripts) are also licensed
GPL-2.0-or-later, the same terms as the upstream project. This tree also follows
[WindowsiOS](https://github.com/ammaarreshi/WindowsiOS), a Windows-targeted fork of Henge
(GPL-2.0), for the Windows side of the boot; that project's licence and notices are
untouched as well. The upstream Henge copyright and `LICENSE` are untouched; the
additional notice lives in [LICENSE-qibaoseven](../LICENSE-qibaoseven). Distributing a binary built from this
tree therefore still means meeting GPL-2.0-or-later in full.

## Not covered

These are Apple's, and none of the licences above apply to them:

- **macOS**, which Henge downloads from Apple when you install it.
- **The boot ROM**, `AVPBooter.vmapple2.bin`. Henge fetches it on the device, from Apple's
  macOS 14.6.1 restore image, when you install. It isn't in this repository or in the app.

An exported Mac is a copy of that Mac, so its folder contains macOS and the ROM, as does
one made by `tools/export_ios.sh`. Those folders are for your own devices, not for sharing.
