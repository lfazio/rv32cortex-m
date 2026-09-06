# ITCM on the STM32N6: the TCM has ECC, and ECC must be written first

The Cortex-M55 on the Nucleo-N657X0-Q has 64 KiB of instruction TCM that
nothing else in this firmware uses. The JIT's code buffer and the
per-dispatch path both live there now -- it is the only memory this core
fetches from without crossing a bus, and it is fetched on every block
entry.

**It costs a full-region scrub at boot, and without that it does not
work at all.** The TCM is ECC memory: a location that has never been
written has no valid check bits, so *reading* it is an error rather than
a read of undefined data. On an instruction fetch that arrives as
IBUSERR. What follows is how that presented, because it does not look
like uninitialised memory.

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

## The fix

`board_itcm_init` fills all 64 KiB with `uint32_t` writes before copying
anything into it. Two details are load-bearing:

- **The whole region, not the bytes in use.** The processor fetches
  ahead, so it reads past the end of a short function into locations no
  copy touched.
- **Word writes.** A sub-word write to ECC memory is a read-modify-write,
  which reads the very check bits it was meant to establish -- so
  `memcpy`'s byte tail is exactly the wrong shape.

This is ST's own sequence. `SystemInit` in STM32CubeN6's
`Projects/STM32N6570-DK/Applications/VENC/VENC_RTSP_Server` FSBL fills
the whole DTCM with `0xa5a5a5a5` before doing anything else, skipping
only the region below MSP because that is the live stack. Nothing here
executes from ITCM at that point, so this fill has no such exception.

**ST declares ITCM and puts nothing in it.** Their application scripts
map `ITCM (rx) : ORIGIN = 0x10000000, LENGTH = 128K` -- the FLEXMEM
extension, which needs `RAMCFG_CR_ITCMCFG` -- and no `>ITCM` placement
appears anywhere in the tree. What they *do* use is DTCM, for the stack.
So there was no vendor example to copy for the instruction side, only
the ECC discipline from the data side, and that turned out to be the
whole of it.

## What it is worth

CoreMark, 25 iterations, the same binary reflashed:

| | ticks |
|---|---|
| buffer and hot path in SRAM | 2,261,843 / 2,261,576 |
| both in ITCM | 2,231,496 / 2,230,417 |

**1.36%**, with `crcfinal 0xa69a` and 6,270,489 retired in every run.
The counter repeats to about 0.05% between runs of one binary, which is
what makes a difference this small readable; CLAUDE.md records layout
alone moving this board by up to 10% between *different* binaries, so do
not quote a smaller gap than this without reflashing the same image.

## How it presented

CoreMark, 25 iterations, loaded over `ST-LINK_gdbserver` on **AP 1**:

| build | result |
|---|---|
| neither section in ITCM | completes, `crcfinal 0xa69a`, `Total ticks 2334992`, retired 6,270,489 |
| JIT buffer in ITCM only | banner, then nothing |
| hot path in ITCM only | banner, then nothing |

Both halves failed, so it was not *which* code went there. Breaking on
the fault handlers caught it:

```
Breakpoint 1, HardFault_Handler
CFSR = 0x00000100     IBUSERR -- instruction bus error
HFSR = 0x40000000     FORCED
SFSR = 0x00000000     not a security fault
stacked PC = 0x10000002
```

## What was ruled out on the way

All of this was true and none of it was the cause, which is why the list
is worth keeping -- every item reads like a candidate:

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

The lesson, which is the one this port already had and did not apply
widely enough: **read ST's sources rather than infer memory behaviour**.
The answer was in a `SystemInit` for a video-encoder demo, in a loop over
a memory this port was not using, and every architectural reading of the
fault pointed elsewhere.

## Not used, and why

- `RAMCFG_CR_ITCMCFG`, which would extend ITCM to 128 or 256 KiB out of
  FLEXRAM. 64 KiB already holds a 32 KiB buffer and the hot path with
  30 KiB to spare, and the extension takes the memory from the AXI side
  where the guest lives.
- The non-secure alias at `0x00000000`. This firmware runs Secure and
  would need an SAU region to fetch from it.

## The two-tier buffer

Splitting the code cache -- hot blocks in ITCM, the rest in SRAM, with
eviction demoting an ITCM block into the SRAM tier rather than
discarding it -- is now buildable, since the fetch works. Whether it
pays is a separate question: at 32 KiB the buffer already holds
CoreMark's whole working set, so the tier boundary would never be
crossed by the one workload that has been measured here. It wants a
guest whose translated set exceeds ITCM before the policy can be judged.
