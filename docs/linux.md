# Linux on RV32

The RV32 frontend boots OpenSBI, then Linux 6.12 with Sv32, then a
Yocto root filesystem to a login prompt that can be typed at -- on the
host runner, interpreted or translated. This is the record of getting
there: what had to exist, what each step found, and what the JIT had to
learn to run an operating system. What is still open is in
[TODO.md](TODO.md#linux).

Everything is driven by `scripts/run-linux.sh` (it builds init, the
initramfs manifest, the kernel and OpenSBI, then runs) and by two CMake
targets, `linux` and `linux-shell`. The device tree is
`boot/rv32-emu.dts`.

## OpenSBI

[OpenSBI](https://github.com/riscv-software-src/opensbi) runs in M-mode
under `scripts/run-opensbi.sh`: v1.9 `generic` boots on the
device tree in `boot/rv32-emu.dts`, finds the uart8250, the
aclint-mswi and aclint-mtimer, sets up its PMP domains and jumps to
S-mode at 0x80400000. That address is where a kernel goes, and one
goes there now: `scripts/run-linux.sh` builds fw_payload with a
kernel inside it. Run bare, with no payload, it still spins to the
instruction cap, which is the expected outcome rather than a
failure.

## Powering off

**A run ends when Linux powers off**, and says how:

    RV32-LINUX-EXT: 0x00000011 checks, 0x00000000 failures -- PASS
    RV32-LINUX-INIT: done
    [    3.045533] reboot: Power down
    emu-result exit=0 exited=1 capped=0 retired=128762098

`scripts/run-linux.sh`, initramfs, 33 s with the rebuilds. Until this
the test init exited, the kernel panicked about killing init, and the
run went on to its instruction cap -- 20 billion by default -- before
it printed its figures.

Three pieces:

- **A test finisher** (`include/emu/emu_finisher.h`), the SiFive test
  device's semantics at QEMU virt's address, 0x0010_0000: 0x5555 stops
  with a pass, 0x3333 with a failure coded in the high half, 0x7777
  resets. It ends the run through the platform's exit, as an M-mode
  `exit(code)` does, so the summary line is the same; a reset is
  reported as `emu: core 0 stopped: system reset by test finisher`.
  `guest-finisher-{pass,fail,reset}` hold each to stopping right after
  the guest's line. Translated, the stop lands where the block holding
  the store ends -- 99 instructions retired against 89 interpreted, the
  same on the host and on all three boards -- and nothing after it is
  printed.
- **A `syscon` in the device tree**, with `syscon-poweroff` and
  `syscon-reboot` naming it. OpenSBI 1.9's generic platform has no
  SiFive test driver; it resets through those two, writing the value to
  the register -- read-modify-write, which is why the finisher reads as
  zero rather than faulting -- and offers Linux the SBI system-reset
  extension once it has found one.
- **Init powers off** when its checks pass and **restarts** when one
  fails, so the two end differently: `exit=0 exited=1`, or the reset
  line. Forced to restart, it ended `reboot: Restarting system` and
  `system reset by test finisher`. If `reboot` returns, init exits as it
  used to, and the panic is still an ending.

A distribution's `poweroff` goes the same way.

## Benchmarks

`LINUX_BENCH=coremark scripts/run-linux.sh` builds CoreMark as a Linux
process and puts it in the initramfs as `/bench/coremark`; init runs it
after its checks -- `clone`, `execve`, `waitid` -- and powers off, so the
run ends with CoreMark's figures. It is the bare-metal guest's port with
a `COREMARK_LINUX` branch: `clock_gettime64` for the clock, `write` for
the output, and a `_start` that sets `gp`. No libc, because the cross
toolchain has none for rv32, and the bare-metal guest's `-march`.

Host JIT, the same CoreMark, `crcfinal` equal where the iteration counts
are, every row long enough for CoreMark to call valid:

| | iterations/s | |
|---|---|---|
| bare metal, x30000 | **1578** | 19 s, 7.50e9 instructions |
| a Linux process, x20000, before the data TLB | **246** | 81 s |
| a Linux process, x20000, with it | **1538** | 13 s, 97% of bare metal |

**It was 6.4x slower under Linux, and not for the reasons that come
first to mind.** Block entries were as frequent as on bare metal -- 33
guest instructions a block entry against 30 -- so not the dispatcher;
the kernel did little meanwhile -- 2.5% of the instructions were
everything else, tick and scheduler included; and FENCE.I was not it --
3,033 flushes at 2,000 iterations and 3,035 at 3,000. What changed was
every load and store. Below M-mode `pmp_active` is set and paging is
on, so the inlined RAM path was off and each access was a call.

**What the call cost was measured, not argued.** Callgrind over
sv32bench's paged build -- the same kernel as its bare build, one `-D`
apart -- said 3.37M accesses cost about 190 host instructions each:
`rv_pmp_check` 31.6% of the run, `rv_mmu_translate` 19.5%, the load and
store helpers 36.5%, translated code 10%. So the data TLB
(`src/frontend/rv32/rv_dtlb.c`, the probe in both backends, the rules in
[jit/staleness.md](jit/staleness.md#the-data-tlb)) asks all three
questions once per page. sv32bench paged went 89 ms to 21.4 against
bare's 21.5 on the host -- 725.7M host instructions to 121.5M -- and on
the boards [15x on the F746 and 7.8x on the N6](backend/thumb2.md#the-data-tlb-probe).

The rest of Linux, same runner against the parent commit:

| | parent | data TLB |
|---|---|---|
| initramfs boot, process wall time | 4.39 / 4.40 s | **2.25 / 2.25 s** |
| Yocto to the login prompt | 64.0 s | **48.0 s** |

**The login prompt was slower before it was faster: 91 s.** Inlining
the probe grew Yocto's average block from 645 bytes to 977, and udev's
working set, about 50,000 blocks, stopped fitting the 32 MB code buffer
it had fitted with eight compactions: 282 compactions evicting 4.1M
blocks, and the guest *retiring 55% more instructions* -- its clock is
the wall clock, so the longer the host spent retranslating, the more
timer-driven work it did. Every other figure here had improved, and
sv32bench and CoreMark cannot fill 32 MB, so only the largest workload
could show it. At 64 MB, now the host default, it is 2 compactions and
48.0 s; the parent at the same 64 MB is 62.9 s, so the TLB's own share
of the prompt is 1.29x.

### FENCE.I: what it costs, and where it comes from

FENCE.I has no address, so it flushes every translation, and docs/TODO.md
had proposed tracking writes to code pages to make it local. Measured
first, on the initramfs boot, against a scratch runner whose FENCE.I
discards nothing -- unsafe, and a ceiling rather than a design:

| | flushes | translations | `reboot: Power down` at |
|---|---|---|---|
| FENCE.I flushes | 3,027 / 3,028 | 154,316 / 154,438 | 3.153 / 3.167 s |
| FENCE.I does nothing | 76 / 76 | 46,546 / 46,333 | 2.867 / 2.833 s |

**About 10% of a three-second boot, and the obvious design would not
recover it.** 2,945 of the 2,951 FENCE.Is are S-mode at two places:
`__patch_insn_write` (1,473) and the `flush_icache_all` after it
(1,467) -- the kernel patching its own text, alternatives and static
keys, at boot. Every one follows a write to a page that does hold
translations, so "flush only if a code page was written" changes
nothing; it would take invalidating by *physical* page -- the patch is
written through a different alias than the one the code runs from --
and seeing every write to RAM, including inlined M-mode stores, which
OpenSBI makes on every SBI call, and virtio's DMA. At about 0.1 ms a
flush, the distribution boot's ~3,770 would be some 0.4 s of 109 (an
extrapolation, not a measurement). The memory path below M-mode was
the 6.4x above. **Both figures predate the data TLB**, which halved the
initramfs boot, so FENCE.I's share of what is left is larger than 10%
and has not been measured again.

The compilers differ -- the Linux binary is GCC 16.2, the bare-metal
guest GCC 15.3 -- and the 2.5% above is the most the instruction count
an iteration can differ by, so that is not the difference either. Building the bare-metal guest with the Linux compiler through
the guest flow did not give a working image, and was not pursued.

## A distribution to a login prompt

**A full distribution boots to a login prompt.** Yocto
`core-image-minimal` for `qemuriscv32` (poky scarthgap), root
filesystem on virtio-blk:

    EXT4-fs (vda): mounted filesystem ... r/w, ordered data mode
    Run /sbin/init as init process
    INIT: Entering runlevel: 5
    Poky (Yocto Project Reference Distro) 5.0.20 qemuriscv32 /dev/ttyS0
    qemuriscv32 login:

udev, syslogd, network interfaces configured, getty on ttyS0 and
on the virtio console. Built with `MACHINE=qemuriscv32`, which is
a first-class machine in poky and needs none of the config
fragments the usual write-ups describe. Run it with:

    LINUX_INITRAMFS=0 \
    LINUX_BOOTARGS="earlycon=sbi console=ttyS0 root=/dev/vda rw rootwait" \
    LINUX_DISK=<rootfs.ext4> scripts/run-linux.sh

Three things that cost time and would cost it again:

**A built-in initramfs containing /init wins over `root=`.** The
kernel runs it and never looks at the root device. No warning.
`LINUX_INITRAMFS=0` builds without one.

**Yocto's own kernel does not work on this machine.** It builds
6.6 for qemuriscv32 and the APLIC driver landed in 6.10, so every
device needing an interrupt defers for ever. Its *rootfs* on our
6.12 kernel is the pairing, and the right one: a rootfs is
userspace and only needs virtio-blk and ext4 in whatever kernel
runs it.

**The host build needs two Debian packages Yocto assumes**,
`libcrypt-dev` and `rpcsvc-proto`. Both fail late and obscurely --
`crypt() not found` inside shadow-native, and a HOSTTOOLS error
naming `rpcgen` -- and `dpkg -s` reports "ok" for a package in
`deinstall ok config-files`, which is how both were missed on the
first check. Use `dpkg-query -W -f='${Status}'`.

**The prompt can be typed at now**, and two CMake targets stand
the whole thing up:

    cmake -B build/host -DEMU_LINUX_ROOTFS=<rootfs.ext4>
    cmake --build build/host --target linux-shell

`linux` is the same without a root filesystem, on the built-in
initramfs.

**Output had a fallback and input did not.** `board_console_putc`
drops through to stdout when no pty is configured, so every guest
in this tree printed; `board_console_getc` returned -1
unconditionally. Every bare-metal guest here only ever prints,
which is why nothing noticed for the life of the project -- and
then a distribution booted to a login prompt that could not be
answered. stdin is pumped into a ring from `board_poll`, once per
slice rather than per LSR read: a guest polls that register far
more often than it retires an instruction, and a `poll(2)` there
would put a syscall on the hottest path a device has.

**`USES_TERMINAL` is what makes the target work, not a tidiness
flag.** Without it the build tool hands the command a pipe, raw
mode is skipped for anything that is not a terminal, and the line
arrives assembled and echoed instead of keystroke by keystroke.

Ctrl-C ends the emulator rather than reaching the guest: ISIG is
left on deliberately. That costs a guest shell its interrupt key,
which is a real limitation; an emulator a person cannot escape
from is a worse one. QEMU's answer is an escape sequence, which
is a state machine rather than a flag.

## The udev stall

**The udev stall: fixed, and it was three things, two of them
the JIT's.** This entry used to say the guest was "waiting on
something that never completes rather than grinding -- which
makes it a device question, not a JIT one". Half right. Boot to
a login prompt was **109 s** after it, from over 900 s (a 15-minute
run did not reach it) -- and is 48 s now, with the data TLB and
a 64 MB buffer ([Benchmarks](#benchmarks)):

| | 8192 blocks | 65536 blocks |
|---|---|---|
| FS out of the generation only | 202 s | 188 s |
| ...and GET_ID answered | 202 s | **109 s** |

- **The JIT flushed on every system call.** Linux clears
  `sstatus.FS` on each trap into the kernel and restores it at
  the return, and FS off-ness was in `rv_ir_gen_key` -- so every
  trap from a process that had touched a float threw the whole
  cache away. 21,949 of 22,126 flushes; a median of 438 resident
  blocks against ~7,000 while the kernel booted; guest 2.4 M
  instructions/s with 1,776 host instructions each (medians over
  the run, first and last lines dropped). FS and frm are part of a block's *identity*
  now, in a context widened to 64 bits. The telemetry showed it
  from the start -- `jit` (resident) collapsing while `blk`
  (translations) climbed -- once the columns were read the right
  way round.
- **The block table filled first.** Blocks are keyed on the
  address space, so every process udev starts translates its
  code afresh. At 8192 blocks: 2,578 compactions, 13.7M blocks
  evicted, translation 81% of host time, with the 32 MiB code
  buffer a fifth used. At 65536 the buffer fills first, as it
  should.
- **virtio-blk dropped GET_ID.** Reading
  `/sys/block/vda/serial` sends `VIRTIO_BLK_T_GET_ID`, and
  TinyEMU lets every type but IN and OUT fall through a
  `default: break;`. The worker blocked in the kernel and
  `udevadm settle` waited out its 120 s. One dropped request
  per boot, measured: type 8, 16 bytes out, 21 back. Its
  signature in the console log is udevd's own, 180 s after the
  worker started:

      [   81.562662] udevd[81]: worker [88] .../block/vda is taking a long time
      [  199.198842] udevd[81]: worker [88] .../block/vda timeout
      [  199.217723] udevd[81]: seq 1280 '.../block/vda' killed

  That is the control -- every fix but this one. With it, none of
  the three lines appears and the root filesystem is remounted at
  guest 65.8 s. The `killed` at guest 1153 s or 1276 s in slower
  boots is the same event, dispatched late because udev itself
  was crawling.

**The last two hid each other**, which is why each looked
worthless alone. With the request dropped, udev ends when the
settle timeout does however fast the CPU work runs; with 8192
blocks the CPU work takes about as long as the timeout. Both
single fixes measure 202 s; both together, 109 s. An A/B that
shows nothing is a result about the workload -- here, about a
second bottleneck the same size as the first. What separated
them was the telemetry's guest rate: **executing at 4.8 M/s and
idle at 0.1 M/s are different stalls**, and one boot had both.

## Kernel boot to userspace

The first milestone, before the distribution: Linux 6.12 rv32 boots on
OpenSBI, reaches `Run /init as init process`, and the init
process makes user-mode `ecall`s that land in `do_trap_ecall_u`
-- so Sv32, the S-mode trap path and the U-mode boundary all
work. ~1.5e9 instructions interpreted, about 10 seconds of guest
time. Rootfs is an initramfs built into the image.

**`scripts/run-linux.sh` builds and runs the whole thing**, and
that is most of what this entry was worth. It had been assembled
by hand, which is how the tree came to be running a kernel image
six hours *older* than the init binary supposedly inside it --
every run was exercising a previous userspace, and nothing said
so. The script rebuilds init, regenerates the cpio manifest,
rebuilds the kernel and OpenSBI, and runs, in one command.

Two things it had to fix to get this far:

**The initramfs had no `/dev/console`.** A directory handed to
CONFIG_INITRAMFS_SOURCE cannot carry a device node -- creating
one needs root -- so the kernel reported "unable to open an
initial console" and userspace had no file descriptors. The
manifest form of INITRAMFS_SOURCE lets gen_init_cpio write the
node as a cpio record, which needs no privilege.

**`--max-insn` could not express the budget this needs.** Its
field is uint64_t and it was parsed through a uint32_t, so
anything over 4,294,967,295 was rejected as a *usage error* --
printing the help, which reads as a typo in the command line.
A kernel needs more than 3G instructions to reach init, so the
first budget anyone would want was the first one refused.

## Userspace output, and the semihosting that stole it

**Userspace output reaches the console. Done**, and the cause
was the emulator stealing the guest kernel's syscalls.

`emu_guest_syscall` is semihosting: it answers `write` and
`exit` so a bare-metal guest can print and terminate with no
kernel underneath it. The interpreter consulted it for *every*
ECALL, at any privilege -- so a Linux process calling write(2),
which traps from U-mode, was answered by the emulator and the
kernel never saw the call. It is M-mode only now, which is where
every bare-metal guest here runs. An S-mode ECALL is an SBI call
and belongs to the firmware.

**It imitated success almost perfectly**, which is why it took
so long. The hook returns the length it was given, so write()
reported every byte written; it ignores the descriptor, so
`write(-1, ...)` reported success too; and it reads the buffer
with a *physical* bus access, which under Sv32 is not where a
user pointer points -- so the read faults on the first byte and
nothing is printed. Userspace was mute while every call
succeeded.

Four things were ruled out first, and all four were sound
conclusions from a broken instrument: the transmit interrupt
(requested 3 times a boot, all start-up probe), the tty layer
(/dev/kmsg bypasses it and behaved identically), the process
exiting too early (a busy loop bought 85 seconds of guest time),
and printk from process context (a `pr_emerg` in `do_group_exit`
printed perfectly).

**What broke the deadlock was testing the instrument**, which
this file already tells you to do: `write(-1, buf, 29)` must
return -EBADF and returned 29. Everything measured with that
wrapper was worthless, and the contradiction it had produced --
"the write succeeds" against "the kernel function it must go
through is never entered" -- was the shape of a lying
instrument rather than a strange kernel.

`tests/unit/test_ecall.c` pins all three privileges. Reverting
the fix gives 6 failures.

## The interrupt controller

**It does not have to be a PLIC.** Established by booting without one: the kernel reaches driver
init with *no* interrupt controller in the device tree, because
the timer comes from the SBI TIME extension, IPIs from SBI IPI
and the console from SBI DBCN. So this blocks devices, not boot.
Use the **APLIC the emulator already has** rather than writing a
PLIC: a 6.12 kernel has `CONFIG_RISCV_APLIC=y` as well as
`CONFIG_SIFIVE_PLIC=y`. It must be wired in **direct mode** --
`interrupts-extended` to the cpu intc and no `msi-parent` --
because MSI mode needs an IMSIC and the AIA CSRs, which this
emulator does not implement.

**Done, and the part that was missing was not the controller.**
The APLIC drove MEIP unconditionally, which is right for every
bare-metal guest here and invisible to Linux: it runs in S-mode
under OpenSBI and never sees MEIP. `--supervisor` moves
delivery, carried as emu_boot_info_t::supervisor because it is a
property of the machine. The device tree now describes the
APLIC in direct mode and the virtio nodes.

**Seen firing under Linux now**, which is what was missing: the
unit test only proved delivery moves between privileges, and
nothing had driven a real queue completion through the whole
path. `/proc/interrupts`, from a distribution at its shell:

    10:   554825  RISC-V INTC   5 Edge    riscv-timer
    12:      124  APLIC-DIRECT 10 Level   ttyS0
    13:        0  APLIC-DIRECT  3 Level   virtio2
    14:      824  APLIC-DIRECT  1 Level   virtio0
    15:        0  APLIC-DIRECT  2 Level   virtio1

824 completions on virtio-blk, delivered to S-mode. The
controller reports **APLIC-DIRECT**, so the direct-mode wiring
above is what the kernel actually bound to rather than what the
device tree merely offered. The two zeros are right: nothing
used the console or sent a frame at the loopback net device.

The 124 on ttyS0 are a second thing confirmed for free -- the
NS16550 receive path is **interrupt-driven end to end**, not
polled. Those are keystrokes typed from the host reaching a
getty through `emu_uart_poll`'s line.

## virtio

**virtio-mmio transport**, done by importing rather than
writing: TinyEMU's `virtio.c` is vendored byte-identical under
`third_party/tinyemu/` (MIT), and the porting layer is four
functions -- `cpu_register_device` to `emu_bus_add_mmio`,
`phys_mem_get_ram_ptr` to `emu_bus_host_ptr`, `set_irq` to
`emu_raise_irq`, and a dozen inline helpers. That brings block,
console, net, input and 9p with it. The PCI transport is refused
loudly rather than stubbed, because a stub lets `virtio_pci_init`
appear to succeed and return a device that never answers.

### The devices on it

**The devices on it.** `--9p [TAG:]DIR`, `--virtio-input` and
`--disk FILE` / `--disk-ro FILE` work. 9p was first on purpose:
it needs no image to build and no partition table to get right,
so the host directory *is* the filesystem. The input pair is a
keyboard and a mouse, fed from the same SDL events as the simple
polled devices and from the same converted evdev codes, so the
two families cannot disagree about what a key is.

virtio-blk is a file-backed `BlockDevice`: 512-byte sectors,
synchronous, because the completion path already handles it --
`read_async` returning 0 means "done" and the request ends
inline. `guest-virtiotest-blk` reads sector 0 and asserts its
*content*, since a device that signalled and filled nothing
would pass every other check in the guest.

virtio-net is `--net loop` and `--net tap:NAME`. The loopback
backend exists because a tap cannot be tested: it needs a device
node, a persistent interface and an address on it, none of which
a suite can assume, so a device offered only over tap would ship
having never moved a packet. `guest-virtiotest-net` is also the
first test to drive two queues at once.

**All three probe under Linux**, which is the check that matters
and which no unit test can make: `virtio_blk virtio0: [vda]
32768 512-byte logical blocks (16.8 MB/16.0 MiB)`.

Next is the display. And note what a *missing* device does:
every virtio node in the device tree needs one behind it, or
virtio_mmio_probe reads the magic, the bus refuses an unmapped
address, and the kernel takes a load access fault and panics
inside driver_attach. Not a driver quietly finding nothing.

**What virtio-blk cost was a barrier in the guest, not a bug in
the device.** A volatile access does not order the ordinary
stores around it, so GCC sank the descriptor ring and
`avail->idx` past the volatile write to QueueNotify. The device
read an avail ring still holding zero and did nothing; every
register was right and nothing faulted. The console queue had
the identical defect and passed anyway, because its one
descriptor happened to be scheduled first -- one weak test is
worse than none, again.

**The interrupt path is proven.** `tests/guest/virtiotest.c`
drives the console queue to completion from guest code and takes
the interrupt: descriptor ring in guest RAM, QueueNotify, the
device printing the bytes and advancing the used ring, the APLIC
delivering, and the handler acknowledging both device and
controller. It runs under `ctest -L fast`.

Its first version failed in the way this whole entry was written
to expect: it spoke the *legacy* transport at a device reporting
version 2, every write was accepted, and QueueNotify did nothing
at all. No register was wrong.

And the device tree has to name them: 0x1000_1000 upwards,
0x1000 apart, interrupts from 1. Nothing checks that the tree
and the emulator agree -- they are two descriptions of one
machine, and the usual failure is a driver finding nothing.

## The descriptions of the machine

**The device tree understates the machine, and the cost is
coverage.** The guest reports

    isa : rv32imafc_zicntr_zicsr_zifencei_zca_zcf
    mmu : sv32

while the emulator's own banner says
`RV32IMAFDCB_zicsr_zifencei_zicntr_zbc`: `boot/rv32-emu.dts`
declares `riscv,isa = "rv32imafc_zicsr_zifencei"` and omits B and
D. Nothing is wrong at run time -- the kernel is being
conservative about instructions it is told do not exist -- but it
means **no Linux guest can reach the Zba/Zbb/Zbc/Zbs lowerings in
either backend**, so the largest guest in the tree gives those
paths no coverage at all. This is the "two descriptions of one
machine, and disagreeing is silent" rule in its quiet direction:
the disagreement costs testing rather than correctness, so
nothing will ever report it. **Resolved in 6cd92f5:** the device
tree declares all of it, and the kernel executes Zbb -- counted
below, and translated since the B lowering.

**The banner itself was understating the core, and finding that
took a third description to notice.** It read `RV32IMAFCB_zbc`:
no D, though `rv_fpu.c` implements it; and no Zicsr, Zifencei or
Zicntr, though all three are implemented and the 20191213 spec
requires them to be *named* -- a string without Zicsr describes a
core that cannot execute `csrr`. CMake's configure summary had
been printing `_zicsr_zicntr_zifencei` and `D` all along, so the
two descriptions this tree generates disagreed with each other
and the one a user sees at run time was the wrong one. Both then
read `RV32IMAFDCB_zicsr_zifencei_zicntr_zbc` -- which still named
three multi-letter extensions out of a dozen implemented. They
name all of them now, and the `rv32-isa-string` test fails if the
two differ. The device tree is the third description and nothing
holds it to the other two.

`mmu : sv32` is the kernel's own confirmation of the paging work.

**Whether the Zbb alternatives actually patch: they do.** This
was open for as long as the device tree has declared B, because a
boot that never executes a Zbb instruction cannot fail on one and
nothing counted them. A `-DEMU_PAIR_STATS=ON` runner does now:
**331,550 Zbb instructions in one boot**, with 36,225 `cbo.zero`,
4,096 cache-block maintenance operations and 3,270 `clmul`. That
was counted under `--jit`, which then gave every one of them in
twenty seconds because the translator declined all of Zbb but
`rori`. **It does not any more**, so the same run now reports no
B at all: the kernel's Zbb executes translated. Counting it again
means `LINUX_JIT=0`, which takes several minutes to userspace.

## The JIT under an operating system

**Where it ended: a boot to a 2e9-instruction cap went from 210 s to
38 s**, with the interpreter at 175 s -- the negative cache, page-
granular `SFENCE.VMA`, and lowering `time` and `div` (the table under
"Then page-granular SFENCE.VMA" below). What follows is in the order it
happened, so its early figures are the starting point rather than the
state: the JIT began *slower* than no JIT.

**The JIT translates under paging.** It used to decline the moment Sdtrig, PMP or
paging was armed, because `blocked` pointed at `fetch_guard`,
which folds all three -- so under Linux it translated nothing
after the first page table and was 12% *slower* than no JIT.

`ir_fetch16` walks the page tables and checks execute permission
per halfword now, as the interpreter's fetch does, and `blocked`
is Sdtrig alone. Same 600M budget, same image:

| | wall | interpreted | translations |
|---|---|---|---|
| originally | 52.5 s | 580,180,660 (96.7%) | 4,786 |
| translating | 61.9 s | 228,272,019 (38.0%) | 951,343 |
| + batched fallback | 53.1 s | 236,069,284 (39.3%) | 931,905 |
| + FENCE lowered | **49.4 s** | 161,193,429 (26.9%) | 931,160 |
| interpreter | 46.8 s | all | -- |

Coverage is transformed and the clock is nearly level. **What
remains is measured rather than guessed.**

**Re-measured after the fast path and chaining, and the JIT is
still behind.** Same image, same 2e9 instruction cap both ways
so the guest work is identical, two interleaved rounds:

| | round 1 | round 2 |
|---|---|---|
| `LINUX_JIT=1` | 190 s | 189 s |
| `LINUX_JIT=0` | 177 s | 175 s |

About 8% *slower* translated. The headline above therefore still
holds, and it is worth saying plainly that none of the work in
the entries below moved it: Dhrystone went 2.0x to 5.0x over the
same period and the Linux boot did not follow, because what
costs here is not what costs there.

**What costs here, from the same run's counters:**

    flushes  129285
    interp   638699763 instructions fell back
    declined 636822916
    blocks   1764 resident, 957612 optimised

**636.8M declined translations against 638.7M interpreted
instructions is 99.7%**: essentially every instruction the
interpreter runs was preceded by a translation attempt that
produced nothing. The note further down guessed "~230M attempts
for 951k successes"; at this budget it is 637M, and it is the
largest single term in the run.

**Built and measured.** A translator that declines at a pc
declines there again, so the refusals are remembered, keyed on
(pc, context) and invalidated by an epoch the flush bumps:

|  | with | without |
|---|---|---|
| wall, 2e9 cap | **148 / 146 s** | 210 / 207 s |
| declined | 258,596 | 624,309,893 |
| cached | 668,408,667 | -- |
| translations | 1,025,340 | 1,024,706 |

**1.42x**, two interleaved rounds, both binaries against the
same kernel. Translations unchanged across 134,860 flushes is
what says nothing legitimate was suppressed; arch-test is
378/378 with `--jit` and riscv-tests 77/77.

So the JIT is no longer behind the interpreter here -- 148s
against the 175/177s an interpreted boot took, though note that
comparison crosses a kernel change and has not been re-run.

**Then page-granular SFENCE.VMA, and then `time`.** The first
collapsed flushes 134,857 to 2,968 and translations 1,023,251 to
174,903 -- and moved the clock 2%, which is what said
translation was never the cost. The second is where it was:

| | wall | interp | block entries |
|---|---|---|---|
| before | 210 s | 626.4M | 641.7M |
| negative cache | 148 s | 673.7M | 682.0M |
| + page-granular | 137 s | 674.6M | 685.8M |
| + `time` lowered | 80 s | 358.1M | 366.6M |
| + `div` lowered | **38 s** | — | — |

**210 s to 38 s, 5.5x**, and each step was found by asking the
same question and getting a different answer:

`rdtime` was **335,195,899** of the boot's 2e9 instructions,
polled in kernel delay loops; `DIV` was **356,462,035** --
99.4% of everything still interpreted after `time` -- declined
because open-coding it means reproducing RISC-V's divide-by-zero
and overflow rules per host. That is an argument against
open-coding and none at all against a call.

**Native divide was then built, measured, and does not pay**:
39 s against the helper's 38 s, two interleaved rounds. A
hardware `idiv` is 20-40 cycles, so the call was never the
dominant term, while the normalisation adds ~8 IR instructions
per divide. It is kept -- correct, suite-clean, and the balance
differs on Thumb-2 where an M7 `SDIV` is 2-12 cycles against a
call with register save/restore -- but **that is unmeasured**,
and the x86 figure is the only one in hand. See
`EMU_IR_DIVS` in emu_ir.h for the precondition it carries.

What is left: **`interp` is still 358M of 2e9 after `time`, and
the divide work removed most of the rest.** The question to ask
next is the same one, a third time -- *which instruction, not
which class* -- against a current boot, because the last two
answers were each a single instruction and neither was
guessable from the class histogram.

**SFENCE.VMA flushes the whole code cache** -- 129,286 times in
a boot, and **99.994% of those requests were not global**:

    SFENCE   global 8   page 131890   asid 2

Eight whole-address-space flushes in an entire boot, against
131,890 carrying a virtual address in rs1. Every one of the
131,890 discards all 1,750 resident blocks, which is what makes
the run translate 1,026,399 times -- roughly 585 retranslations
per block. So the operands are not a detail to be conservative
about here; they are almost the whole population.

The design that follows, and the thing that makes it feasible:
a chained exit is patched to `chain_entry`, which is *past* the
prologue and shares the caller's frame, so an invalidated block
can be made harmless by writing a jump from its own
`chain_entry` to its own `tail`. Every inbound link then falls
into the dispatcher and the pc is retranslated -- **no reverse
index from target back to the blocks that jump into it**, which
is the part that would otherwise make this cost more than it
saves: `unlink_all` is O(blocks x links) and would run 131,890
times.

What it needs that does not exist: a hook that *writes* an
unconditional jump. Both `patch_link` implementations only
rewrite the displacement of a branch already there
(`t2_patch_branch`, `x86_patch_rel32`), which is the wrong
primitive for turning ordinary code into a jump. An A/B with the invalidation simply removed (incorrect,
for measurement only) runs 55.7 s with 97,646 translations, so
this is worth about 6 s. Fixing it properly means page-granular
invalidation: a block would record the guest page it was
translated from, and a targeted SFENCE would invalidate only
matching blocks. The handler currently ignores its operands,
which is legal and maximally coarse.

**The fallback interpreted one instruction per call. Fixed.**
Every declined instruction paid the interpreter's whole run-loop
entry, which the interpreter proper amortises over thousands.
It now interprets a run in one call, with an adaptive size:
doubling while translation keeps failing, reset to one on block
entry, so an isolated declined instruction still costs exactly
one. 61.9 s to 53.1 s with coverage unchanged.

Resetting that size in the wrong place is worth knowing about.
The first version reset only on a *fresh* translation, which in
steady state never happens -- a warm cache answers from the
lookup -- so the batch pegged at its maximum and ran past
translatable code. Interpreted rose to 443M of 600M, block
entries fell 99M to 22M, and it was **faster**: 45.5 s, because
an efficient interpreter beat a JIT that had stopped
translating. A speed-up is not evidence.

**Plain FENCE was not lowered. Fixed.** It is a no-op on this
machine and the translator declined it, so every barrier ended a
block and took a fallback -- and kernel code is full of them.
53.1 s to 49.4 s, interpreted 39% to 27%.

**Since then: the memory fast path and block chaining.** What
the ratio measurement said, and what each step was worth on
Dhrystone against an interpreter at 0.231 s:

| | wall | ratio | block entries |
|---|---|---|---|
| before | 0.104 s | 44.21 | 1,665,337 |
| + self-loop chaining | 0.084 s | -- | 741,085 |
| + inlined RAM access | 0.057 s | 33.02 | 741,085 |
| + cross-block linking | **0.046 s** | **29.65** | **320,738** |

2.0x to **5.0x**. The order came from measuring rather than
guessing: 40% of guest instructions were a memory access and
each was a C call (3,728,397 of them), which was far larger than
the dispatcher; only after that did linking exits become the
biggest remaining term. 134 links removed 420,000 dispatches.

**A minimum block length was tried and is much worse.**
Refusing to keep a block below N instructions, so those go to
the (now efficient) fallback instead of paying a dispatch:

| minimum | Dhrystone | block entries |
|---|---|---|
| **1 (kept)** | **0.044 s** | 320,735 |
| 2 | 0.071 s | 300,725 |
| 3 | 0.138 s | 280,713 |
| 4 | 0.305 s | 320,639 |
| 6 | 0.643 s | 160,426 |

Monotonic, and by a lot. The reasoning behind it -- "entering a
block to run one no-op costs 80 cycles and is a loss" --
compared a short block against *free* rather than against what
actually happens instead, which is interpreting those
instructions. Even a one-instruction block beats the
interpreter by enough to pay for its own dispatch. Entries fall
as intended and the clock rises anyway, which is the shape of
an optimisation measuring the wrong thing.

Also worth knowing: the JIT attempts a full translation before
almost every interpreted instruction and gets nothing back,
which is ~230M attempts for 951k successes at a 600M budget --
counted since at a 2e9 budget as **636.8M declined against
638.7M interpreted, 99.7%**, and it is the largest term in the
run. See the re-measurement above.

Guests that never page are unaffected and still twice the
interpreter: bench 0.028 s against 0.057, dhrystone 0.104
against 0.203.

**What made any of this checkable is `EMU_EXTRA_ARGS` on
run-arch-test.sh.** Nothing validated a translating backend
under the privileged features before -- the suite runs the
interpreter, and the JIT passed the Sv32 and PMP tests by
declining to execute them. It is 378/378 both ways, and with
`--jit` those tests now translate for the first time.

**The inlined memory path is off under every operating system,
and for two reasons rather than one.** `rv_ir_fast_mem`
declines on `vm_active`, `pmp_active` or `trig_active`, and
`rv_pmp_refresh` sets `pmp_active` **from the privilege alone**
-- before it looks at a single entry:

    h->pmp_active = false;
    if (h->priv != RV_PRIV_M || rv_hart_data_priv(h) != RV_PRIV_M) {
        h->pmp_active = true;

So a kernel gets no inlined access even with no PMP region
configured, and "add an inline TLB probe" -- which is how this
item was written -- would leave the second gate shut and change
nothing. Anything built here has to answer both questions
inline: virtual to physical, *and* whether the access is
permitted.

**What it is worth, measured.** `tests/guest/sv32bench.c` is
one source built twice, differing in one `-D`: the same
memory-bound kernel in M-mode under Bare, and in S-mode under
an identity-mapped Sv32 with a background PMP entry. Retired
counts agree exactly (8,579,610), block entries agree to 0.2%,
so the difference is the per-access helper call and not
dispatch:

| | interpreter | JIT | JIT speedup |
|---|---|---|---|
| bare, fast path on | 129 ms | **21 ms** | 6.1x |
| paged, fast path off | 351 ms | **85 ms** | 4.1x |

Three interleaved rounds, 21/21/22 against 84/85/85 -- far
outside the +/-3% this host is noisy by.

**This said 4.0x was the ceiling and the prize smaller, and it
was the whole of it.** The argument was that the interpreter
pays 2.7x for the same move while inlining nothing, so most of
the gap must be translation and permission work a probe would
still do. A probe does not do it per access, it does it per
page: built, the paged JIT run is 21.4 ms against bare's 21.5
([Benchmarks](#benchmarks)). And the interpreter's 2.7x is
mostly its *fetch* path, which the same table does not serve:
data alone took it from 370 ms to 331. The argument compared
two backends' totals; callgrind, which splits a total by
function, would have said so before the estimate was written.

**And this configuration now has a test, which it did not.**
isatest arms PMP early and from then on everything interprets,
so the JIT's behaviour under Sv32 and PMP was covered by
nothing. `guest-sv32bench` and `guest-sv32bench-jit` run the
paged guest on both backends and assert `SV32BENCH-END`, the
line only a completed run prints. Confirmed by breaking it:
both fail with the trap reported.

### A trap taken inside a block

Found on the way, and **fixed**: a trap taken inside a block left
`jit_ctx` stale. `rv_ir_load` raises a fault from inside the
block and the interrupt hook raises one between blocks, and the
context -- which carries the privilege -- was refreshed only on
the interpreter fallback. So the dispatch looked the handler up
under the context it had trapped *from*, and filed the handler's
first block there. `rv_hart_trap` refreshes it now, beside the PMP
and MMU refreshes it already did. isatest's `jit-ctx-fetch-tval`
shows the consequence: S-mode jumps to a machine-mode trap stub on
a page PMP denies it, and with the fix reverted the fetch fault
arrives at stub+8 -- S-mode had run the stub's first block -- where
it must arrive at stub+0. Interpreter unaffected, as it should be.
