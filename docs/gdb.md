# Debugging with gdb

There are **two** gdb connections in this project and they debug different
programs. Getting them confused is the first mistake to avoid.

| you want to debug | connect to | what a breakpoint means |
|---|---|---|
| the **guest** | `emu-host --gdb`, port 1234 | a guest pc |
| the **emulator** | OpenOCD / `probe-rs gdb`, port 3333 | an ARM pc in the firmware |

```sh
# the guest
./build/host/emu-host --gdb build/host/guest/isatest.bin
gdb-multiarch build/host/guest/isatest.elf -ex 'target remote :1234'

# the emulator, on hardware
cmake --build build/f746 --target gdbserver
gdb-multiarch build/f746/src/platform/stm32f746/emu-stm32f746.elf \
  -ex 'target extended-remote :3333'
```

The host `gdb` on Debian is x86-only. Use `gdb-multiarch`, or
`probe-rs gdb`.

---

## The guest stub

`src/emu/emu_gdb.c` is the RSP protocol with **no ISA in it**: packets,
checksums, breakpoints and run control. A frontend supplies one
[`emu_gdb_target_t`](../include/emu/emu_gdb.h) saying how many registers
gdb expects, in what order, and which is the pc. That is the same split
the rest of the tree uses, and for the same reason — `src/emu/` must not
learn what an architecture calls things.

```c
typedef struct emu_gdb_target {
    unsigned nregs;             /* the length of the `g` packet          */
    unsigned reg_bytes;         /* 4, unless reg_width says otherwise    */
    const uint8_t *reg_width;   /* bytes per register, or NULL           */
    unsigned pc_regno;          /* gdb's number for pc -- not the last   */
    uint64_t (*reg_get)(const emu_cpu_t *, unsigned gdb_regno);
    void     (*reg_set)(emu_cpu_t *, unsigned gdb_regno, uint64_t);
    uint32_t (*pc_get)(const emu_cpu_t *);
    void     (*pc_set)(emu_cpu_t *, uint32_t);
    int         stop_signal;    /* 5 = SIGTRAP                           */
    const char *arch;
    const char *target_xml;     /* qXfer:features:read                   */
} emu_gdb_target_t;
```

**Registers have their own widths.** A `g` packet is every register at
its own width, back to back, so ARMv7-M's VFP feature -- d0..d15 at 64
bits after a 32-bit core file -- needs a width per register and offsets
that are the sum of the widths before, not `regno * reg_bytes`.
`tests/unit/test_gdb.c` drives the stub with a 64-bit register between
two 32-bit ones; the mutant that computes offsets the old way, and the
one that ignores the table, each fail it.

**The memory map is the platform's, built from the bus.** It used to be
a string in two targets: RV32's named flash at 0x8000_0000, where guest
code has not lived since images moved to execute in place at
`EMU_GUEST_ROM_BASE`, and G4MH's at 0x0000_0000, below anything the
flash ops accept -- and ARMv7-M and PowerPC had none, so `load` wrote
them with `X`. The stub now lists the bus's regions in address order --
RAM as `ram`, ROM as `rom`, devices and the passthrough window as `ram`
-- and the flash ops' window (`base`, `size()`: the board's arena) as
`flash` with a block size, leaving out any region inside it. Served when
the platform offers flash ops, which is the board: there are none on
the host, so `test_gdb.c` is the map's only test.

Every frontend has one:

| frontend | file | registers | pc is | gdb feature |
|---|---|---|---|---|
| RV32 | `src/frontend/rv32/rv_gdb.c` | 33: x0..x31, pc | 32 | `org.gnu.gdb.riscv.cpu` |
| G4MH | `src/frontend/g4mh/g4mh_gdb.c` | 66: r0..r31, sr0..sr31, pc, fp | 64 | gdb's built-in v850 numbering |
| ARMv7-M | `src/frontend/armv7m/armv7m_gdb.c` | 34: r0..r12, sp, lr, pc, xpsr, d0..d15, fpscr | 15 | `org.gnu.gdb.arm.m-profile`, `org.gnu.gdb.arm.vfp` |
| PowerPC | `src/frontend/ppc/ppc_gdb.c` | 39: r0..r31, pc, msr, cr, lr, ctr, xer, spefscr | 32 | `org.gnu.gdb.power.core`, and spefscr in a feature of its own |

ARMv7-M's d<n> is s<2n+1>:s<2n>, and gdb builds s0..s31 from them --
which is why a gdb session cannot see a swapped pairing and the unit
test holds it to the s registers instead. PowerPC's SPEFSCR is *not* in
gdb's SPE feature: that feature is the SPE's upper halves and its
64-bit `acc`, which this model does not have (EFPU2 is scalar), and
registers that read zero and drop every write would be shown as values.
Not described: MSP/PSP and the masks on ARMv7-M, and the PowerPC SPRs --
the state dump has those.

### Checked by a real gdb

`scripts/gdb-check.sh [BUILD_DIR]`, and the `gdb-check` ctest wherever
`gdb-multiarch` is installed, attach gdb to each frontend in the runner
-- twice when the runner has a JIT, once interpreted and once with
`--jit` -- and compare what it sees with things the stub did not
produce:

| check | against | catches |
|---|---|---|
| pc at `break *main` | the ELF's symbol | the register byte order |
| the register number in the stop reply | gdb's remote number for `pc` | expediting the wrong register |
| a register written, read back after a cache flush | the value written | P/p and g plumbing |
| `stepi` | the pc before it | run control |
| `stepi` lands on the next instruction | gdb's disassembler (`$_` after `x/2i`) | a step that is really a block |
| a breakpoint two instructions ahead | its own address | a `continue` that runs through it |
| a word at the pc | the file (`trust-readonly-sections`), or the image | memory reads |

A register written and read back **cannot** see the byte order -- a
swapped encoding is swapped both ways and round-trips perfectly -- which
is why the pc is compared with the symbol. Each check was confirmed by
reverting what it guards: little-endian registers make PowerPC's pc at
`main` read `e4050080` for `800005e4`; "the pc is the last register"
makes the other three report 16, 37 and 65 for 15, 32 and 64.

### The register order is not a choice

gdb's `g` packet is a fixed per-architecture concatenation and gdb does
not ask. For rv32 it is `x0..x31` then `pc`, 32-bit, **little-endian by
byte** -- because rv32 is little-endian, not because the packet is. Each
register travels in the *target's* byte order, which the stub takes from
the bus (`emu_bus_t::big_endian`, set once by a big-endian frontend). It
encoded every register little-endian until PowerPC had a target, and a
round trip through gdb could not have shown it.

**The stop reply names the pc by number, and the number is not "the
last register".** The stub used `nregs - 1`, which is the pc for RV32
and `fp` for G4MH -- so every G4MH stop told gdb "fp = pc". It never
showed: gdb 17, still lacking the pc, fetched the whole `g` packet and
overwrote the bogus `fp`. So the expedite was worthless on every stop,
and comparing a register before and after a cache flush -- the obvious
test -- passes against the bug. `pc_regno` says which register it is.

Getting it wrong yields an `info registers` that is plausible and
entirely wrong — every value present, every value in the wrong place. So
check it against the board's own state dump or `emu-host --dump`, not
against itself.

`target_xml` is optional and worth supplying. Without it gdb cannot know
what it is talking to, and every session has to begin with
`set architecture riscv:rv32` or an ELF — the difference between the stub
being usable and being a thing you have to remember how to use.

### Three things in RSP that fail by hanging rather than erroring

These cost real time, because nothing reports an error:

- **The checksum is a modulo-256 sum of the packet body**, and gdb
  silently *retries* a bad one. A stub that computes it wrong looks like
  a slow link, not a broken one.
- **`c` and `s` must not be answered until the guest actually stops.**
  Replying early makes gdb believe it halted where it already was, so
  `continue` appears to do nothing.
- **Ctrl-C arrives as a bare `0x03` outside any packet.** A stub that
  only looks for `$` makes ^C do nothing at all, and there is then no way
  to interrupt a running guest.

### Breakpoints are a pc list, not a patched instruction

`EMU_GDB_MAX_BREAK` of them, and gdb is told "Too many" past that rather
than silently losing one.

Patching a trap instruction into the code is what real silicon needs and
is wrong here twice over:

- the read-only half of a guest image is served from the board's **flash**
  (see [memory.md](memory.md)), so there is nothing to patch;
- a patched instruction is invisible to the **JIT** until the block is
  retranslated, so the breakpoint would fire only on interpreted paths.

### Under the JIT

The stub runs whatever backend the session chose, so a JIT miscompile
can be chased with breakpoints rather than disappearing under them.
Two mechanisms make that exact:

- **`step` is one instruction under every backend.** The contract in
  `emu_cpu.h` always said so, and every frontend implemented it as
  `backend->run(cpu, 1)` -- one instruction from an interpreter, a whole
  block (or chain of blocks) from the JIT. A JIT backend now supplies
  `emu_backend_t::step`, which is `emu_jit_step`: one instruction
  through the interpreter, then the frontend's `after_interp`, as the
  dispatcher does for an instruction it declines.
- **While a breakpoint is set the stub steps where one could be, and
  runs one block elsewhere** -- with the JIT's chained exits undone
  (`emu_jit_unchain`), because a chained exit runs into the next block
  without returning and "one block" would otherwise be any number. A
  breakpoint within 4 KB ahead of the pc means step; the longest block
  any translator builds is 64 instructions.

Both were missing until gdb-check ran with `--jit`. At the commit before,
RV32 and PowerPC under the JIT failed its two new checks exactly: `stepi`
ran the guest to its exit loop, and the breakpoint ahead was never hit.
ARMv7-M passed, because its step had always called the interpreter.

## Debugging the emulator on hardware

`--target gdbserver` starts OpenOCD on :3333. Useful when the *firmware*
is misbehaving rather than the guest:

- `emu-host --dump` prints the full guest register file on exit.
- `-DEMU_ENABLE_TRACE=ON` builds a per-instruction trace hook —
  `--trace-skip N --trace-count M`. Read the **pc deltas**: an
  instruction that changes the pc without retiring is a trap, and that is
  how a missing G4MH instruction was found after three sessions of
  looking at the arithmetic it appeared to break. The disassembler is a
  separate implementation and is allowed to be behind the interpreter, so
  trust the addresses, not the mnemonics.
- A `HardFault` on the ARM side usually means the passthrough window let
  a guest access reach an address the ARM bus rejects. An unimplemented
  address in that window makes the AHB signal an error, which is a fault
  in the **emulator**, not one delivered to the guest — the firmware dies
  rather than the test failing.

**With `EMU_NET=ON` the serial console is gone**: `emu_net_init()` is a
one-way handover and after it the wire carries only IP. Debug output is
telnet, and a firmware that has given its console away cannot report its
own failure — which is why `fatal_halt` must not mask interrupts.
