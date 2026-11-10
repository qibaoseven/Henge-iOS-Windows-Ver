# firmware/

The two flash images the virt machine boots from. Neither is in the repository:
both are 64 MB, and they are build products of edk2's ArmVirtQemu.

    edk2-aarch64-code.fd   the code bank (read-only at 0x0)
    edk2-arm-vars.fd       the variables bank (writable at 0x04000000)

On Debian/Ubuntu they come out of `qemu-efi-aarch64`:

    apt install qemu-efi-aarch64
    cp /usr/share/AAVMF/AAVMF_CODE.no-secboot.fd  edk2-aarch64-code.fd
    cp /usr/share/AAVMF/AAVMF_VARS.fd             edk2-arm-vars.fd

`no-secboot` matters: the secure-boot image verifies everything it loads against
keys that are not there, and stops.

Then:

    sh tools/run-uefi.sh
