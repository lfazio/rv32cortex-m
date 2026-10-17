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
| host, a build that can run more than one core, with `EMU_OS_SMP` -- the default for a multicore G4MH | `src/platform/host/freertos_smp` -- written here; tasks are pthreads, one FreeRTOS core per emulated core | **yes**, unless `--serial` |
| host, otherwise | `portable/ThirdParty/GCC/Posix` -- tasks are pthreads | no: the POSIX port runs one task at a time |
| `stm32f746` | `GCC/ARM_CM7/r0p1` | no: one core |
| `stm32f446` | `GCC/ARM_CM4F` | no: one core |
| `stm32n6` | `GCC/ARM_CM55_NTZ/non_secure`, secure-only | no: one core |

In a build with the network (`EMU_NET`) the IP stack has a task too,
`net`, which runs only while the run loop waits for it -- see
[network.md](network.md#on-a-task-of-its-own). It uses notification
slot 1, so a hand-off can never be counted as a core finishing a round.

A multicore guest on a multicore host runs its cores in parallel: see
[SMP on the host](#smp-on-the-host). Everywhere else the cores take turns,
reproducibly. Kernel: FreeRTOS-Kernel `V11.1.0`, MIT, pinned by
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

On every single-core FreeRTOS port the scheduler is **cooperative**
(`configUSE_PREEMPTION 0`, no time slicing) and every task has the same
priority -- that is the boards, and a host build that runs one core or
was configured `-DEMU_OS_SMP=OFF`. An SMP host has no order at all and
`--serial` puts it back; see [SMP on the host](#smp-on-the-host). A task gives up the CPU
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
figures exactly -- for the binary of that time: it was rebuilt on
2026-10-14 from a source the house style had broken, and the start-up
code it carries now changes those counts.

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

**picolibc's thread pointer is set before anything else.** `errno` is
thread-local in picolibc, reached through `__tls`, and these boards start
from ST's Reset_Handler rather than picolibc's crt0 -- so `__tls` was 0
and every `errno` store went to address 8, which is ITCM on the N6 and
the F746. On the N6 it zeroed the `sub sp` at the head of `emu_jit_run`,
and the firmware HardFaulted in its first JIT slice: the reason FreeRTOS
had never run there. The link scripts give `.tdata`/`.tbss` the layout
picolibc's own script does, and `board_tls_init()`, first in
`board_early_init()`, copies the block, points `__tls` at it and checks
that `&errno` falls inside. One block for every task: the boards'
scheduler is cooperative and nothing keeps `errno` across a switch.

**The N6's `flash` target starts the core now, through gdb.** It used
to hand a signed image to `STM32_Programmer_CLI -g`, which reported
success and left the core in the boot ROM: the CLI works on access port
0, which reaches the bus but not the Cortex-M55, and the signed image is
the flash-boot layout, written 0x180400 below the link address.
`scripts/n6-load.sh` starts `ST-LINK_gdbserver -m 1`, resets the core to
the ROM through AIRCR, `load`s the ELF, takes sp and pc from
`g_pfnVectors` and detaches. Through it: RV32 `isatest` passes as on the
F746, and three-PE G4MH `smp.bin` prints the model's line in 3,776
rounds of 4,096, the host's cooperative count -- twice in a row, the
second over the first. `signed-image` still builds the flash-boot image.

**Do not poke the N6 with `STM32_Programmer_CLI` while debugging.** A
software reset and halt attempts through it left access port 1
answering "Target unknown error 32" to every tool, through an NRST
reset, until the board was power-cycled.

**A CC-RH guest is copied to 0x80000000 before it runs.** Every G4MH
guest is linked there, which is guest RAM, so running it in place at
`EMU_GUEST_ROM_BASE` leaves every absolute address wrong -- each string
reads as nothing. `cmake/guest-image.cmake` says so for G4MH images and
the board passes `--load`, as the host's ctest does.

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
was the open question; [jit/tuning.md](jit/tuning.md#layout-on-the-f746--what-the-140-to-153-swing-was)
has what it turned out to be.

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

The SMP port keeps SIGALRM as the tick, sent to core 0's thread, adds
SIGUSR2 for one core asking another to yield, and installs both with
`SA_RESTART` itself. stdio stays safe there for a narrower reason: the
cores' tasks are never preempted -- only idle tasks are, and they print
nothing -- and the guest's console is behind a lock.

## SMP on the host

A G4MH build with more than one PE (`-DG4MH_PE_COUNT=3`) on a host gets
**one FreeRTOS core per emulated core**, and a round's quanta run on
that many host threads at once. `-DEMU_OS_SMP=OFF` builds the
single-core port instead, and `--serial` on the command line runs every
round inline on one task in core order -- the single-threaded
round-robin, reproducible to the instruction -- in the same binary.

### The port

**FreeRTOS has no SMP POSIX port**, in V11.1.0 or in any release up to
V11.3.1: its SMP ports are the RP2040 and XCORE.AI. So
`src/platform/host/freertos_smp` is written here, on the POSIX port's
model -- one pthread per task, a task that is not running is a thread
waiting on its own event -- with several running at once:

- **A core's ID** is a thread-local, set by whoever resumes the thread
  onto a core.
- **A core's interrupts** are two signals, masked by a thread-local flag:
  a handler that finds it set leaves its work pending, and enabling
  services what is pending before it returns -- the kernel's demand that
  a pending yield be serviced *immediately*, which
  `prvCheckForRunStateChange` asserts. That includes a yield whose signal
  is still in flight, because the requester sets the core's pending flag
  first, under the kernel's locks.
- **`portYIELD_CORE`** is a flag and a `pthread_kill` to the thread on
  that core; the handler acts only if the flag is set, so a signal that
  arrives late does nothing.
- **The kernel's two locks** are recursive spinlocks owned by core.
- **Waits spin before they sleep**, for 200 us: a thread waiting to be
  handed a core, and an idle core watching for work -- which takes a
  yield request by itself while it watches, with no signal. Past that
  they sleep, so a host core with nothing to do costs nothing.

One ordering problem decides the design. A core that has just switched
tasks can be asked to yield before the port has recorded which thread now
runs there; if the request lands on the old thread and the flag is then
cleared by the switch, the new task never yields and the kernel asserts.
So "which thread is on this core" and "no yield pending" are written in
`portTASK_SWITCH_HOOK`, which `vTaskSwitchContext` calls holding both
kernel locks -- the locks every yield request is made under.

**Preemption is on under SMP**, and that is not a change of policy: with
it off the kernel never asks an idle core to pick up a task that has just
been released (`taskYIELD_ANY_CORE_IF_USING_PREEMPTION` compiles to
nothing), so idle cores would have to spin looking. The cores' tasks share
one priority above idle and time slicing stays off, so they never preempt
each other; only idle tasks are preempted.

### What the emulator had to stop sharing

The multicore design kept every core's state per core with sharing only
through named devices, precisely so this would be "a locking exercise
rather than a redesign" ([host/g4mh/multicore.md](host/g4mh/multicore.md)).
It was, plus four things that design note could not have seen:

| what | was | now |
|---|---|---|
| the JIT: code buffer, block table, hash, translator scratch | one set of globals | `EMU_PER_CORE` -- `_Thread_local` under `EMU_SMP`, and a cache per core is G4MH's own model, since SYNCI and its I-cache are per PE. A core's thread maps its buffer on its first slice; `emu_jit_flush_all` and a once-per-slice epoch for what changes every core's memory |
| G4MH's shared devices: INTC1/2, OSTM, INTIF, BOOTCTRL, the barrier, IPIR, TPTM, LTSC, KCRC | unlocked | a locking twin of each device's ops, chosen at registration; the controller's public functions lock too. One recursive `emu_lock`, empty unless `EMU_SMP` |
| the reservation table | unlocked | locked |
| CAXI, SET1/CLR1/NOT1, LDL.W/STC.W | atomic because one core ran at a time | **host atomics on RAM**. A lock cannot do it: another core's plain store takes none. STC.W is a compare-and-swap against the word LDL.W read, with the reservation still checked |
| the JIT's memory bit operations | a load, a computation and a store | a helper call into the interpreter's atomic (`g4mh_bitop`), so the block stays whole. Declined until the helper existed, which ended a block at every one |
| the boot protocol's stack pointer | the top of RAM for *every* PE | PE *k* starts *k* strides below PE0, which keeps the top |
| the console | unlocked | locked |

The stack pointer was a bug before any of this: three PEs shared one
stack from the moment they started, so a quantum boundary in the middle
of a call could corrupt another PE's frame. `barrier3` survived it
because its `main` barely touches the stack.

Three came from failures rather than reading. Making the framework's
state thread-local put 4.9 MB of TLS on every thread -- the block table
alone is 4.5 MB -- and glibc takes TLS out of the thread's stack, so
`pthread_create` refused the first 32 KB idle task with EINVAL. The two
big tables are allocated per core now, TLS is 69 KB, and the port gives
every thread at least 2 MB of uncommitted stack. A core whose thread had
not run the backend's init had no code buffer, which the framework treats
as "run the interpreter" -- silently, for the whole run. And the first
LL/SC took the reservation word with a second read after the load that
gave the program its value, so another core's increment could land
between them and be overwritten: 59,996 of 60,000 interpreted, 58,615
translated.

### How it is checked

**`tests/guest/g4mh/smp.c`**, built with CC-RH: three PEs contend -- a
CAXI spinlock around a deliberately non-atomic increment, LL/SC
increments of one word, NOT1 toggles of each PE's own bit in one shared
byte -- and then each runs its own xorshift loop. Every number it prints
is the same at any interleaving, and `smp_want.py` computes them in
Python; ctest holds the default, `--serial`, `--jit` and `--jit --serial`
runs to that one line.

**Each atomic was broken on purpose, and the guest noticed** -- runs
three to eight times each, with the fixes in place every run correct:

| mutant | what the guest printed |
|---|---|
| CAXI as a load and a store | nothing: two PEs inside the lock leave the lock word held by nobody, and every run spins until its timeout -- ctest's 120 s is the verdict |
| NOT1 as a load and a store | 8 of 8 wrong, both backends: `bits=4,0`, `5,5`, `0,6` ... -- and 2 of 3 under `--jit` once the JIT reached it through its helper |
| the JIT's native memory bit operation allowed under SMP | 3 of 3 wrong, in both bytes: `bits=4,6`, `3,4`, `3,2` (4 of 4 in the second byte alone, before the register form was translated) |
| the JIT declining them under SMP again | nothing in the guest -- slower is not wrong -- so `test_bit_manipulation_backends` counts fallbacks exactly and fails |
| STC.W as a plain store | 3 of 3 wrong: `llsc=59768`, `59960`, `59947` |

The third survived the first version of the guest. It toggled bits with
NOT1's register form only, and the JIT then lowered only the immediate
form -- so the mutant was in code the guest never reached. The second
byte is toggled with the immediate form, and the toggle count went from
20,001 to 200,001 when the second row caught two runs in three rather
than all of them. The JIT lowers both forms now, through one function,
and the mutant corrupts both bytes.

What the helper bought on this guest is coverage rather than time:
1.28M fewer instructions handed back to the interpreter (5,440,774 to
4,159,113) and the same wall time within noise, 0.257 s against 0.271 s
over seven interleaved runs. The guest's bit loops are as contended as
its lock, and its time goes there.

**ThreadSanitizer** builds and runs under the port (`-fsanitize=thread`).
With `tests/tsan.supp` -- the kernel's own deliberate unlocked read, the
guest's memory, and BOOTCTRL asking a running core whether it is held --
`barrier3` reports no races, interpreted and translated, at quanta 1, 4
and 64. Its first reports were the port's own bookkeeping, an exported
global the JIT inventory missed (`emu_jit_layout`), and the barrier
device.

### What it buys

A variant of `smp.c` with almost no contention and four times the
independent work, three PEs on a 20-CPU host, five interleaved runs,
medians; every run printed the same line:

| backend | 4,096 | 8,192 | 16,384 | 32,768 (default here) | 65,536 |
|---|---|---|---|---|---|
| interpreter | 2.57x | 2.39x | 2.59x | 2.79x | 2.76x |
| JIT | 1.85x | 2.24x | 2.38x | 2.67x | 2.66x |

The first run of this table, when 4,096 was the default, gave 2.71x and
2.06x there and 2.05x and 1.33x at 1,024; the host was busier for this
one (a load average of 3), so read across a row rather than between the
two. Why a parallel run's default is 32,768 now, and what that costs,
is below.

**A round is a barrier across host threads, and what it cost decided
everything.** The first version of the port took the speedup *below one*
for the JIT at 4,096, then the default: about 66 us of overhead a round
against about 14 us of translated work per core. `strace` counted where
it went -- some 260 system calls a round -- and three changes took it to
the three that the console's poll makes in either mode:

| port | JIT, q 4,096 | interpreter, q 4,096 |
|---|---|---|
| blocking waits; interrupts as the signal mask | 0.88x | 1.62x |
| waits spin 200 us before sleeping | 1.02x | 1.71x |
| an idle core takes a yield without a signal | 1.09x | 1.96x |
| interrupts masked by a flag, not `pthread_sigmask` | **2.06x** | **2.71x** |

The last was 87 `pthread_sigmask` calls a round -- the kernel masks
around every critical section -- several of them made holding the
kernel's locks, so the other cores spun on those locks meanwhile: 71
`sched_yield`s a round. The contention guest itself is slower in parallel
at small quanta, and should be: a PE that fails a CAXI yields its quantum,
and under real contention that happens far more often than when the PEs
take turns.

**What the SMP build costs when it is not parallel**, the same guest
with `--serial` in both, nine interleaved runs: **+3.6%** interpreted,
**+5.4%** translated, against a `-DEMU_OS_SMP=OFF` build -- the JIT's
thread-local state and the locks that are real now. Not the idle cores'
spinning: a build that does not spin measures the same.

**Which is why parallel is not the default for PowerPC's two cores.**
A default host build has PowerPC's second core in it and is mostly run
for RV32, and callgrind puts what the SMP port costs RV32 under the JIT
at 1.7% on Dhrystone and 2.1% on crypto. `EMU_OS_SMP` defaults ON only
where G4MH was configured with several PEs; PowerPC's cores run as tasks
on the single-core port unless it is asked for
([frontend/ppc.md](frontend/ppc.md#two-cores)).

**A core that cannot run is not woken.** A round releases only the
tasks whose core would do something -- not halted, not held at reset,
not waiting with nothing able to wake it (`emu_core_runnable`) -- and
runs core 0 inline when there are none. Waking core 1's task to be told
it was held cost a PowerPC guest that never starts it half a percent of
everything, in task switches. A core released during a round runs in
the next, and the order assertion covers the tasks that were released.

And **`--serial` is the fast way to be reproducible.** It runs a round
inline on one task; the cooperative single-core port sends a three-PE
round through three task switches, and the same guest takes 1.05 s that
way against 0.60 s inline, translated -- about 57 us a round of
switching.

### The quantum

**32,768 by default when an SMP host runs a multicore guest's cores in
parallel, and 4,096 everywhere else** (`EMU_PARALLEL_BUDGET` and
`EMU_DEFAULT_BUDGET`, picked once the core count is known; `--quantum`
overrides both). A parallel round is a barrier across host threads and
the table above stops growing at 32,768: the JIT goes 1.85x, 2.24x,
2.38x, 2.67x, then 2.66x at 65,536.

What a longer quantum costs is not throughput. **The guest's clock moves
once per quantum** -- `advance_guest_time` sets it between slices -- so
the quantum is also the clock's resolution and the bound on how late a
timer interrupt can be taken; a guest that polls the time in a delay
loop spins for at least one quantum per wait, and two reads of `time`
inside one quantum are equal.

**A single default of 32,768 was tried first, and the architecture suite
refused it.** `Zicntr-csrrc-00` and `Zicntr-csrrs-00` read `time` about
11,000 instructions apart and want it to have moved: they pass at 8,192
and fail from 11,000 up. Three workloads measured beforehand had said
nothing -- three interleaved runs of the kernel at each quantum, five of
the guest, ranges and medians:

| workload | 4,096 | 16,384 | 32,768 |
|---|---|---|---|
| Linux kernel init, to the root-mount panic (wall) | 4.25-4.42 s | 4.14-4.34 s | 4.07-4.15 s |
| `smp.c`, contended, 3 PEs in parallel, JIT | 0.257 s | 0.251 s | 0.246 s |
| `smp.c`, contended, 3 PEs in parallel, interpreted | 0.331 s | 0.318 s | 0.297 s |

The kernel is the prebuilt one in `build/linux`, configured for a root
filesystem the disk image does not hold, so it panics at mount -- after
calibration, driver probing and every delay loop of kernel init, about
2.9 guest-seconds, which is the part a coarse clock would slow. And the
single-core RV32 guests -- CoreMark and `bench` both ways, Dhrystone and
`irqtest` -- ran 0-9% faster at 32,768 than at 4,096 with the same
retired counts and exit codes; `irqtest` interpreted was +0.9%.

None of that could see a frozen clock, because none of it times anything
short; a conformance test that does saw it at once. So a single-core run,
which gains a few percent at most, keeps the fine clock, and only the
case that gains 1.4x pays for the coarse one.

**The boards keep 4,096.** Theirs is `EMU_RUN_SLICE`, a separate knob:
one core, so there is no parallelism to buy, and on a board the slice is
also how often the console and the network are serviced, which nothing
here measured. The `g4mh-smp` ctest cases pin `--quantum 4096` for the
other reason: a smaller quantum interleaves the cores more finely, which
is what a contention test wants.

### What it does not do

- **Order.** A round has none under SMP, and the order assertion
  compiles out; the quantum is still the only synchronisation point the
  emulator imposes. Per-core retired counts vary from run to run; a
  program's answer should not.
- **The memory model.** Guest stores land in host memory with the host's
  ordering. x86 is TSO, which is *stronger* than the RH850, so a guest
  missing a barrier still works here -- the limitation
  [host/g4mh/multicore.md](host/g4mh/multicore.md) records, now
  real rather than hypothetical.
- **LL/SC's ABA case.** A word changed and changed back between LDL.W and
  STC.W is not seen, when another core's store did not land in the
  window where it breaks the reservation directly. That is the usual
  price of emulating LL/SC with compare-and-swap.
- **Boards.** Every board here is single-core, and the port is the
  host's. `EMU_SMP` is never set there, so the locks and `EMU_PER_CORE`
  compile to nothing and a board binary is unchanged.
