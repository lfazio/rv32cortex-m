# rv32cortex-m — working notes

RV32 emulator for ARM Cortex-M hosts. The guest drives the host's real
peripherals through an identity-mapped passthrough window, so **peripheral
drivers live in the guest, not in the emulator**. Keep it that way: adding a
GPIO or UART driver to `src/platform/` is almost always the wrong fix.

## Build and test

```sh
# host: development and both test suites
cmake -B build/host -DRV32_PLATFORM=host -DCMAKE_BUILD_TYPE=Release
cmake --build build/host && ctest --test-dir build/host -L fast

# firmware
cmake -B build/stm32f446 -DRV32_PLATFORM=stm32f446 \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake \
      -DCMAKE_BUILD_TYPE=Release -DRV32_GUEST=isatest
cmake --build build/stm32f446 --target flash
```

Useful options: `-DRV32_JIT=OFF` (interpreter, for isolating JIT bugs),
`-DRV32_JIT_CODE_BYTES=2048` (forces compaction — a good stress test),
`-DRV32_NATIVE_COREMARK=ON` (native ARM baseline instead of the emulator),
`-DRV_GUEST_MARCH=...` (guest ISA; a **cache variable**, so pass it explicitly
when changing it), `-DRV32_GUEST=isatest|hello|bench|stm32drv|coremark`.

## Validation — run before claiming anything works

```sh
./scripts/run-arch-test.sh      # official riscv-arch-test, currently 133/133
./scripts/run-riscv-tests.sh    # Berkeley suite, 75/77 (2 need PMP/Sdtrig)
```

The JIT cannot be exercised by the x86 host suites. Validate it by flashing
`isatest` (104 checks) and reading the UART — **this has caught real JIT bugs**,
including an inlined store that skipped the LR/SC reservation break.

Hardware: Nucleo-F446RE on ST-LINK, console `/dev/ttyACM0` at 115200 8N1.
`probe-rs download --chip STM32F446RETx <elf>` then `probe-rs reset`.

## Things that have bitten, and will again

- **Extensions sharing an opcode slot must be decoded in one place.** Zbb's
  `min`/`max` and Zbc's `clmul` share funct7 0x05; a separate `else if` later in
  the chain is unreachable and every `clmul` raised illegal-instruction.
- **Put the common case first in decode.** Zbb tests placed ahead of
  `SLLI`/`SRLI` made every shift pay an extra compare and cost more than Zbb
  saved.
- **In the JIT, what you decline costs more than what you translate badly.**
  Ending a block for an untranslatable instruction fragments hot code. Route it
  through a helper call instead — `jit_helper_alu` exists for exactly this.
- **JIT fast paths bypass the C helpers and their side effects.** The inlined
  store had to drop the LR/SC reservation by hand.
- **Do not conflate "nothing translatable here" with "cache full".** Sharing a
  recovery path made every interpreted `div` flush the code cache.
- **Measure; do not reason about performance.** Interpreter-in-SRAM was
  *slower*, lazy-IRQ was neutral, and the `clmul` fix was 1.3% when the real
  cost was 4.12-instruction blocks. Layout noise is ±3%, so ignore differences
  below that.
- Guest images link `-nostdlib`; there is no libc.
- CoreMark's `core_main.c` defines `main()`; `-Dmain=...` must be scoped to that
  file or it renames the firmware's entry point.

## Conventions

`src/core/` is portable C11 with no platform dependencies and must stay that
way — it builds for ARMv6-M through ARMv8.1-M and for the host. Shared
semantics (`rv_hart_amo`, `rv_hart_cbo`) live in the core so the interpreter and
JIT cannot drift apart. New ISA work goes in **both** backends plus
`tests/arch-test/` config, or is declared unsupported — see Zacas, which is
implemented but disabled because it does not pass.
