# Porting to another target

Because drivers live in the guest, the port surface is small:

1. **Clock setup** — one function, ideally using the vendor HAL.
2. **Console transport** — two callbacks (`tx`, `rx`) for the virtual UART.
3. **Linker script** — place `.guest_ram` between `.bss` and the stack; it is
   sized by the linker so the guest automatically gets all unused SRAM.
4. **Region table** — the peripheral windows and their permissions.
5. **Cache ops** — `NULL` if the part has no cache.

The guest is told how much RAM it has at reset (`sp`, `a0` = hartid, `a1` = RAM
size), so guest images do not hardcode a size.

For a different ARM core, retarget with
`-DRV32_ARM_CPU=cortex-m7 -DRV32_ARM_FPU=fpv5-d16`, or
`-DRV32_ARM_CPU=cortex-m0plus -DRV32_ARM_FPU=` for ARMv6-M.

---
