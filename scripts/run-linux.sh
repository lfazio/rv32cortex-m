#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Build and run a 32-bit Linux kernel on the emulator, under OpenSBI.
#
# What this proves that run-opensbi.sh does not
# ---------------------------------------------
# OpenSBI exercises M-mode: the CLINT, PMP, medeleg/mideleg and an
# NS16550. It stops where a payload would start. Linux is the payload,
# and it exercises the half nothing else here reaches -- **Sv32**, the
# S-mode trap path, and the virtio devices behind the APLIC. A kernel
# that reaches userspace has walked page tables for every access it has
# made since early boot.
#
# The pieces, and why each is where it is
# ---------------------------------------
#   boot/rv32-emu.dts        the machine. OpenSBI and Linux both read
#                            their world out of this, so it is the one
#                            description and the emulator must agree
#                            with it -- see the warning below about
#                            devices.
#   boot/initramfs/init.c    userspace. -nostdlib, because there is no
#                            rv32 libc in this toolchain.
#   boot/initramfs.list      the cpio manifest, generated here.
#
# **The device nodes are why there is a manifest.** Pointing
# CONFIG_INITRAMFS_SOURCE at a *directory* archives what is in it, and a
# device node cannot be created without root -- so /dev/console was
# simply absent, the kernel said "unable to open an initial console",
# and everything userspace printed went to a file descriptor that was
# never opened. It ran correctly and proved nothing. gen_init_cpio
# writes the node as a cpio record instead, which needs no privilege.
#
# **Every virtio node in the device tree needs a device behind it.**
# Nothing checks that the two agree, and the failure is not a driver
# quietly finding nothing: virtio_mmio_probe reads the magic, the bus
# refuses an unmapped address, and the kernel takes a load access fault
# and panics inside driver_attach. That is why this script passes
# --disk, --net and --virtio-console -- one per node.
#
#   scripts/run-linux.sh [emu-binary]
#
# Needs gcc-riscv64-linux-gnu (the bare-metal riscv64-unknown-elf- one
# will not do for OpenSBI: its linker cannot make PIEs), dtc and cpio.

set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
EMU=${1:-$ROOT/build/host/emu-host}
WORK=${LINUX_WORK:-$ROOT/build/linux}
SRC=${LINUX_SRC:-$WORK/linux}
OPENSBI_SRC=${OPENSBI_SRC:-$ROOT/build/opensbi/opensbi}
CROSS=riscv64-linux-gnu-

# How long to let it run. A kernel reaches userspace in about 1.5e9
# instructions interpreted; the default is generous rather than tight,
# because the failure this guards against is an emulator that never
# stops, not one that is slightly slow.
MAXINSN=${LINUX_MAXINSN:-20000000000}

if [ ! -x "$EMU" ]; then
    echo "error: no emulator at $EMU" >&2
    echo "       cmake -B build/host && cmake --build build/host" >&2
    exit 1
fi

#
# ls -la the runner before believing a result: this script defaults to a
# path no other workflow rebuilds, which is how a whole session's
# figures once came from a three-day-old binary -- and how the kernel
# below came to be run with an init binary six hours newer than the
# image it was supposedly inside.
#
echo "runner: $(ls -la "$EMU" | tr -s ' ' | cut -d' ' -f6-)"

for t in "${CROSS}gcc" dtc cpio; do
    command -v "$t" >/dev/null 2>&1 || {
        echo "error: $t not found" >&2
        case "$t" in
        "${CROSS}gcc")
            echo "       sudo apt install gcc-riscv64-linux-gnu" >&2 ;;
        esac
        exit 1
    }
done

[ -d "$SRC" ] || {
    echo "error: no kernel source at $SRC" >&2
    echo "       set LINUX_SRC, or clone one there" >&2
    exit 1
}
[ -d "$OPENSBI_SRC" ] || {
    echo "error: no OpenSBI at $OPENSBI_SRC; run scripts/run-opensbi.sh first" >&2
    exit 1
}

mkdir -p "$WORK"

# ---------------------------------------------------------------------
# userspace
# ---------------------------------------------------------------------
#
# **The Linux-targeting compiler, not the bare-metal one.** Both can
# generate rv32 code and init is -nostdlib either way, so the bare-metal
# riscv64-unknown-elf- looks equivalent -- and is not. Its default
# linker script is for bare metal: it packs .rodata, .data and .bss into
# one segment, which strip then refuses to rewrite and which the kernel
# loads into a process that immediately takes SIGSEGV. The kernel
# reports that as `exitcode=0x0000000b` from a panic about killing init,
# with no other clue.
#
# riscv64-linux-gnu- ships no rv32 runtime, which is why this is
# -nostdlib, but its linker script is the one that produces a loadable
# Linux binary.
#
echo "==> init"
"${CROSS}gcc" -march=rv32ima -mabi=ilp32 -static -nostdlib -s \
    -Os -ffreestanding -fno-stack-protector \
    -o "$WORK/init" "$ROOT/boot/initramfs/init.c"

# ---------------------------------------------------------------------
# the initramfs manifest
# ---------------------------------------------------------------------
#
# Generated rather than committed, because the `file` lines carry
# absolute paths. See the note at the top about why the device nodes
# cannot come from a directory.
#
cat >"$WORK/initramfs.list" <<EOF
dir /dev 0755 0 0
nod /dev/console 0600 0 0 c 5 1
nod /dev/null 0666 0 0 c 1 3
nod /dev/tty 0666 0 0 c 5 0
nod /dev/ttyS0 0600 0 0 c 4 64
nod /dev/kmsg 0600 0 0 c 1 11
file /init $WORK/init 0755 0 0
EOF

# ---------------------------------------------------------------------
# the kernel
# ---------------------------------------------------------------------
echo "==> kernel"
make -C "$SRC" ARCH=riscv CROSS_COMPILE="$CROSS" \
    CONFIG_INITRAMFS_SOURCE="$WORK/initramfs.list" \
    -j"$(nproc)" olddefconfig >"$WORK/kbuild.log" 2>&1 ||
    { echo "error: kernel configure failed; see $WORK/kbuild.log" >&2; exit 1; }

#
# The manifest has to be in .config as well as on the command line: the
# kernel records it, and a stale value there silently rebuilds the old
# initramfs -- which is exactly the failure this script exists to stop.
#
"$SRC/scripts/config" --file "$SRC/.config" \
    --set-str CONFIG_INITRAMFS_SOURCE "$WORK/initramfs.list"

make -C "$SRC" ARCH=riscv CROSS_COMPILE="$CROSS" -j"$(nproc)" \
    >>"$WORK/kbuild.log" 2>&1 ||
    { echo "error: kernel build failed; see $WORK/kbuild.log" >&2; exit 1; }

IMAGE=$SRC/arch/riscv/boot/Image
echo "kernel: $IMAGE ($(wc -c <"$IMAGE") bytes, $(date -r "$IMAGE" '+%F %T'))"

# ---------------------------------------------------------------------
# OpenSBI, with the kernel as its payload
# ---------------------------------------------------------------------
echo "==> opensbi"
make -C "$OPENSBI_SRC" -j"$(nproc)" PLATFORM=generic \
    CROSS_COMPILE="$CROSS" \
    PLATFORM_RISCV_XLEN=32 \
    PLATFORM_RISCV_ISA=rv32imafdc_zicsr_zifencei \
    PLATFORM_RISCV_ABI=ilp32d \
    FW_TEXT_START=0x80000000 \
    FW_PAYLOAD_PATH="$IMAGE" >"$WORK/sbi.log" 2>&1 ||
    { echo "error: OpenSBI build failed; see $WORK/sbi.log" >&2; exit 1; }

FW=$OPENSBI_SRC/build/platform/generic/firmware/fw_payload.bin
DTB=$WORK/rv32-emu.dtb

#
# The kernel command line lives in the device tree's chosen/bootargs,
# because that is where a kernel reads it from -- so overriding it means
# rewriting the tree, not passing another option to the emulator.
#
# LINUX_BOOTARGS is what makes one script serve two very different
# guests: the initramfs above, which needs no root device, and a real
# distribution on virtio-blk, which needs `root=/dev/vda`.
#
if [ -n "${LINUX_BOOTARGS:-}" ]; then
    sed -e "s|bootargs = \"[^\"]*\";|bootargs = \"$LINUX_BOOTARGS\";|" \
        "$ROOT/boot/rv32-emu.dts" > "$WORK/rv32-emu.dts"
    echo "bootargs: $LINUX_BOOTARGS"
    dtc -I dts -O dtb -o "$DTB" "$WORK/rv32-emu.dts" 2>/dev/null
else
    dtc -I dts -O dtb -o "$DTB" "$ROOT/boot/rv32-emu.dts" 2>/dev/null
fi

echo "payload: $FW ($(wc -c <"$FW") bytes)"
echo

# ---------------------------------------------------------------------
# run
# ---------------------------------------------------------------------
#
# A disk image, so the third virtio node has something behind it and so
# vda exists. Created if absent; never overwritten, because a guest that
# wrote to it should find what it wrote.
#
DISK=${LINUX_DISK:-$WORK/disk.img}
[ -f "$DISK" ] || dd if=/dev/zero of="$DISK" bs=1M count=16 2>/dev/null

#
# --supervisor is not optional and fails silently without: the APLIC
# raises MEIP by default, which is right for every bare-metal guest here
# and invisible to a kernel running in S-mode under OpenSBI. Without it
# every virtio driver waits for ever on a queue that already completed.
#
# 256 MiB, matching the device tree's memory node.
exec "$EMU" --ram 0x10000000 --load 0x80000000 --dtb "$DTB" \
     --supervisor \
     --disk "$DISK" --net loop --virtio-console \
     --max-insn "$MAXINSN" "$FW"
