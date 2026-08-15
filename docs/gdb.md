# Debugging with gdb

There are **two** gdb connections in this project and they debug different
programs. Getting them confused is the first mistake to avoid.

| you want to debug | connect to | what a breakpoint means |
|---|---|---|
| the **guest** | `emu-host --gdb`, port 1234 | a guest pc |
| the **emulator** | OpenOCD / `probe-rs gdb`, port 3333 | an ARM pc in the firmware |

```sh
# the guest
./build/host/emu-host --gdb --load 0x80000000 build/host/guest/isatest.bin
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
    unsigned reg_bytes;         /* 4 here                                */
    uint32_t (*reg_get)(const emu_cpu_t *, unsigned gdb_regno);
    void     (*reg_set)(emu_cpu_t *, unsigned gdb_regno, uint32_t);
    uint32_t (*pc_get)(const emu_cpu_t *);
    void     (*pc_set)(emu_cpu_t *, uint32_t);
    int         stop_signal;    /* 5 = SIGTRAP                           */
    const char *arch;
    const char *target_xml;     /* qXfer:features:read                   */
    const char *memory_map;     /* qXfer:memory-map:read                 */
} emu_gdb_target_t;
```

RV32's is in `src/frontend/rv32/rv_gdb.c`. **G4MH has none yet**, so
`--gdb` is RV32-only; adding one is a target descriptor and four
accessors.

### The register order is not a choice

gdb's `g` packet is a fixed per-architecture concatenation and gdb does
not ask. For rv32 it is `x0..x31` then `pc`, 32-bit, **little-endian by
byte**.

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

Checking a pc against a short list costs a compare per instruction on a
path that is already checked, and is correct in both backends.

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
