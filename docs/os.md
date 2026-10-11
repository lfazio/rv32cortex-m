# FreeRTOS: one task per emulated core

Every platform runs the emulator under FreeRTOS. `main()` brings up the
hardware a scheduler needs, starts the scheduler, and the runner becomes
its first task; that task is also emulated core 0, and each further core
of a multicore guest runs on a task of its own. The code is
[`src/platform/common/emu_os.c`](../src/platform/common/emu_os.c), the
configuration
[`src/platform/common/FreeRTOSConfig.h`](../src/platform/common/FreeRTOSConfig.h),
and the kernel is fetched by [`cmake/freertos.cmake`](../cmake/freertos.cmake).

| platform | FreeRTOS port | parallel? |
|---|---|---|
| host | `portable/ThirdParty/GCC/Posix` -- tasks are pthreads | no: the POSIX port runs one task at a time |
| `stm32f746` | `GCC/ARM_CM7/r0p1` | no: one core |
| `stm32f446` | `GCC/ARM_CM4F` | no: one core |
| `stm32n6` | `GCC/ARM_CM55_NTZ/non_secure`, secure-only | no: one core |

So nothing runs in parallel yet. The structure is what an SMP-capable
FreeRTOS host would run in parallel, and the last section says what that
would still need. Kernel: FreeRTOS-Kernel `V11.1.0`, MIT, pinned by
`FREERTOS_KERNEL_TAG` and fetched into `FREERTOS_KERNEL_DIR` at configure
time; point the latter at a checkout to build offline.

## The round

```
coordinator (core 0's task)            core k's task, k = 1..N-1
---------------------------            -------------------------
notify cores 1..N-1          ───────▶  wake
run core 0 for one quantum             run core k for one quantum
wait for N-1 notifications   ◀───────  notify the coordinator, sleep
time, polling, uploads, the cap, the idle rule
next round
```

The cores meet at every quantum boundary, and everything that is not a
core's own execution -- guest time, the platform's polling, uploads, the
instruction cap, deciding the run is over -- happens between rounds,
exactly as it did in the single-threaded round-robin. A guest with one
core runs the round inline on the first task, with no task switch at
all; that is every guest but a multicore G4MH build.

### Determinism, and how it is checked

The scheduler is **cooperative** (`configUSE_PREEMPTION 0`, no time
slicing) and every task has the same priority. A task gives up the CPU
only where it blocks, and released tasks run in the order they were
released. So a round is core 0, then 1, then 2 -- the order
`emu_system_step` has always used -- and a multicore run is as
reproducible as it was, at every quantum.

**That order is asserted every round, because nothing a guest prints can
hold it.** Each core records its position as it starts; the coordinator
checks core *k* ran *k*-th. Two mutants show why the check has to exist:

| mutant | `barrier3` output and state dump, q 1/4/64 | instruction trace | the assertion |
|---|---|---|---|
| core 0 runs last | identical | identical | **fires** |
| cores 1 and 2 swapped | identical | identical | **fires** |

Multicore tests are written to give the same answer at any interleaving
-- that is what their quantum invariance asserts -- and running core 0
last is a rotation of the round, which for this guest produces the same
global sequence. The check compiles out under an SMP kernel, where the
round has no order.

Against the commit before FreeRTOS, built with three PEs: output and
`--dump` byte-identical for `barrier3.bin` and `guest.bin` at quanta 1, 4,
64 and 4096 on both backends, and the run reports
`os 3 cores as tasks, 302 rounds` at quantum 1 -- so the identical output
came through the tasks, not around them. On the F746, `barrier3` under the
Thumb-2 JIT retires 4,422 / 4,155 / 24 on its three cores, the host's
figures exactly.

## On a board

**Clocks before the scheduler.** The Cortex-M ports program SysTick from
`SystemCoreClock` when the scheduler starts and own its priority from
then on, and SysTick is shared with the HAL's millisecond. So
`board_early_init()` (clocks, caches, the console) runs from `main()`
before `emu_os_start`, and `SysTick_Handler` advances both ticks at
1 kHz. SVC and PendSV are FreeRTOS's own handlers -- V11 checks the vector
table points straight at them -- and the ARMv8-M port's `SysTick_Handler`
is renamed `xPortSysTickHandler` in that one file so the N6 handler can
call it like the others.

**The first task's stack is where the main stack was.** The linker
scripts reserve it at the top of RAM beside a 2 KiB interrupt stack, and
`emu_os.c` takes it from `__task_stack_start`/`__task_stack_end`. As a
`.bss` array it moved everything after it -- on the F746 the JIT's code
buffer slid 9 KiB further out of the DTCM -- and that confounded the first
measurement below. Peaks measured on the F746: 1,312 of 4,096 bytes for
CoreMark under the JIT, 784 interpreted; 884 and 692 of 4,096 for G4MH's
second and third cores. Core stacks exist only in a build that can run
more than one core (`EMU_OS_MAX_CORES`), so a single-core guest pays for
none. The F746 guest has 2,936 bytes less RAM than before.

**Two probes attached** need `-DEMU_PROBE=<VID:PID:serial>` on the flash
target, and the consoles opened by `/dev/serial/by-id/...`: the
`ttyACM` numbers are plug order and swapped mid-session here.

## What it costs

| where | measurement | result |
|---|---|---|
| host | callgrind, crypto guest | 313,564,788 → 314,620,658 host instructions, **+0.34%**, almost all start-up |
| host | `hello`, 30 interleaved runs | 1.97 → 4.51 ms: a fixed ~2.5 ms to create three threads |
| F746 | CoreMark x120, scheduler bypassed vs running, same binary | 152.00 vs 152.79 cycles/instruction: the scheduler is **0.5%** |
| F746 | CoreMark x120, against the commit before | interpreter 107.4 → 111.4, JIT 32 KB 140.0 → 152.8, 96 KB 31.1 → 32.5 |

The last row is mostly **layout, not the kernel**: the scheduler-bypass
build -- FreeRTOS linked in, never started -- is as slow as the one that
runs it. Adding the kernel moved the emulator's hot functions 456 bytes
in flash (`rv_run` 0x0800a590 → 0x0800a758), and this part has been
measured at up to 10% from layout alone. Two FreeRTOS builds that differ
only in where a stack lives give 108.6 and 111.4 interpreted. Making the
hot path insensitive to where the linker puts it -- ITCM, or alignment --
is open in [TODO.md](TODO.md); it predates this change.

## The host

The POSIX port runs each task as a pthread and switches by condition
variable; three things about it shaped `emu_os.c`:

- **SIGALRM is the tick**, sent to whichever task's thread is running and
  installed without `SA_RESTART`, so a `read()` or `write()` it lands in
  fails with `EINTR`. The first task adds `SA_RESTART` to the port's own
  disposition. The tick is 100 Hz on the host, since nothing waits on it.
- **The scheduler owns the main thread**, so the runner's exit status
  leaves through `exit()` from the first task.
- **stdio is safe because the scheduler is cooperative**: a task is never
  switched out in the middle of a `printf`.

## Toward an SMP host

What a FreeRTOS SMP kernel (`configNUMBER_OF_CORES > 1`) would run in
parallel is each round's quanta, and these are the shared things a
quantum touches, none of which is locked today because nothing runs
concurrently:

- **G4MH's shared devices** -- INTC2, the IPIR, the barrier registers,
  OSTM -- and the **reservation table**, which a store on one core clears
  for the others. These are the "explicitly shared bus regions" the
  multicore design kept separate for exactly this; a lock per device,
  taken by its MMIO handler, is the shape.
- **The JIT framework** (`src/emu/emu_jit.c`) keeps the code cache and
  block table in globals; per-core instances, or a lock around
  translation, before two cores may translate at once.
- **The console**, which several cores may write to in one round.
- **Memory ordering.** Guest stores land in host memory with the host's
  ordering. A round barrier is a full barrier, but within a quantum a weak
  guest (RH850) on a stronger host is still emulated *more* strongly than
  the silicon -- the limitation [host/g4mh/multicore.md](host/g4mh/multicore.md)
  already records, now with a way out of it.

The order assertion has to go under SMP, and with it the reproducibility
it protects: that is the trade, and the quantum is still the knob.
