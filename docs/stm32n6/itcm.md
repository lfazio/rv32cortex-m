# ITCM on the STM32N6: a bus error on fetch, and what it is not

The Cortex-M55 on the Nucleo-N657X0-Q has 64 KiB of instruction TCM that
nothing in this firmware uses. It is the obvious place for the JIT's code
buffer -- the only memory the core fetches from without crossing a bus,
and the buffer is fetched on every block entry.

The mechanism to put it there is built, is generic, and **is off by
default**, because on this board *executing* from ITCM raises a bus
error. This note records what was established so the next attempt starts
from the fault rather than from the beginning.

## The mechanism

Three pieces, none of which know what a TCM is except the last:

| where | what |
|---|---|
| `EMU_JIT_CODE_SECTION` | the section `g_static_code` is placed in; empty means an ordinary `.bss` array |
| `EMU_HOT_TEXT_SECTION` | the section `EMU_HOT_TEXT` functions are placed in -- `emu_jit_run` and the two IR memory helpers |
| `stm32n657xx_lrun.ld` | declares `ITCM` at `0x10000000`, `.itcm` loaded from ROM, `.jitcode` NOLOAD |

Both defines are set on `emucore`, **not** on the platform executable.
`emu_ir_jit.c` is compiled into `emucore`, so a define on
`emu-stm32n6` reaches nothing -- and the build still links, still runs,
and reports figures identical to the digit. That is the same shape as
`EMU_JIT_CODE_BYTES` reaching only the translator that no longer
existed. **Check `nm`, not the build log.**

Turning it on is:

```sh
cmake -B build/n6 -DEMU_PLATFORM=stm32n6 \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake \
      -DEMU_JIT_CODE_SECTION=.jitcode -DEMU_HOT_TEXT_SECTION=.itcm
arm-none-eabi-nm build/n6/src/platform/stm32n6/emu-stm32n6.elf | grep g_static_code
```

which puts the 32 KiB buffer and 1256 bytes of hot path in ITCM, 34,024
bytes of the 64 KiB.

## The failure

CoreMark, 25 iterations, loaded over `ST-LINK_gdbserver` on **AP 1**:

| build | result |
|---|---|
| neither section in ITCM | completes, `crcfinal 0xa69a`, `Total ticks 2334992`, retired 6,270,489 |
| JIT buffer in ITCM only | banner, then nothing |
| hot path in ITCM only | banner, then nothing |

Both halves fail, so it is not *which* code goes there. Breaking on the
fault handlers catches it:

```
Breakpoint 1, HardFault_Handler
CFSR = 0x00000100     IBUSERR -- instruction bus error
HFSR = 0x40000000     FORCED
SFSR = 0x00000000     not a security fault
stacked PC = 0x10000002
```

## What was ruled out

- **The region is enabled and the right size.** `ITCMCR = 0x00000039`:
  EN set, SZ 7, which decodes to 64 KiB. `board_itcm_bytes()` reports
  it on the console at boot for exactly this reason.
- **The memory holds data.** Writing `0xA5A5F00D` to `0x10000000` and
  `0x12345678` to `0x10001000` over the debugger reads both back.
- **The copy lands.** The first word at `0x10000000` reads `0xe92d2300`,
  which is what `objdump -s -j .itcm` says it should be.
- **It is not security attribution.** `SFSR` is zero, the SAU regions
  are cleared by ST's `SystemInit`, and RM0486 table 2 attributes
  `0x10000000` as Secure -- which is the state this firmware runs in.
- **It is not the clock.** The fault is present with ITCM read back
  before `HAL_RCC_ClockConfig`, at `main` entry, and after it.
- **It is not FLEXRAM being unpowered.** `RCC_MEMENSR_FLEXRAMENS` and
  clearing `RAMCFG_FLEXRAM->CR.SRAMSD` are now done in `SystemInit` and
  changed the fault not at all. They are kept on the documented ground
  that FLEXMEM backs the TCMs, not because they fixed anything.
- **It is not helper-call range.** `t2_call` is `MOVW`/`MOVT` into r12
  then `BLX r12`, which is absolute; the 604 MB from ITCM to AXISRAM2
  would break a `BL` and does not apply here.

`gdb`'s `x/4xw 0x10000000` reporting "Cannot access memory at address
0x10000004" is **a debug-access artifact, not the bug** -- single-word
reads at the same addresses succeed. It cost a detour; do not read it as
evidence about the core.

## What has not been tried

- `RAMCFG_CR_ITCMCFG` non-zero. RM0486 table 35 says the first 64 KiB
  is "I-TCM fix" and needs no allocation, which is why it was not
  touched -- but "fix" has already turned out to mean *not
  configurable* rather than *always working*.
- ST's own N6 examples do not put code in ITCM, so there is no vendor
  sequence to copy. That is itself worth knowing: this port's rule is to
  read ST's sources rather than infer memory behaviour, and here the
  sources are silent.
- The non-secure alias at `0x00000000`, which this firmware cannot fetch
  from while running Secure without an SAU region.

## The two-tier buffer

Splitting the code cache -- hot blocks in ITCM, the rest in SRAM, with
eviction demoting ITCM blocks into the SRAM tier rather than discarding
them -- is a good design and is **not** built, because it would be a
policy layer resting on a fetch that does not work. It needs the bus
error resolved first; nothing about the split would be measurable until
then.
