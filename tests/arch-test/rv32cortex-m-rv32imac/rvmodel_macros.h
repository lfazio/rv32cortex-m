# rv32cortex-m RVMODEL macros for the official RISC-V architecture tests.
# SPDX-License-Identifier: Apache-2.0
#
# The suite is self-checking: each test reports through RVMODEL_IO_WRITE_STR
# and terminates through RVMODEL_HALT_PASS / RVMODEL_HALT_FAIL. Those are
# mapped onto the emulator's own platform:
#
#   console      the virtual NS16550 at 0x10000000
#   termination  ecall with a7=93 (newlib exit), which the host runner turns
#                into its process exit status
#   timer        the virtual CLINT at 0x02000000
#
# Because these are the same devices the STM32 firmware provides, a test
# image built against this file also runs on the target unchanged.

#ifndef _RVMODEL_MACROS_H
#define _RVMODEL_MACROS_H

#define RVMODEL_DATA_SECTION

# Machine mode only: no S or U, so the Sm profile applies.
#define STANDARD_SM_SUPPORTED

##### STARTUP #####

# Reset leaves mcountinhibit clear and every CSR at its architectural reset
# value, so no boot fixup is needed.
//#define RVMODEL_BOOT

# Nothing is mapped at 0x70000000, so an access there raises a load or
# store access fault. This enables the suite's access-fault tests.
#define RVMODEL_ACCESS_FAULT_ADDRESS 0x70000000

##### TERMINATION #####

# a7=93 is the newlib exit syscall. The host runner intercepts it and exits
# with a0 as its status, which is what run_tests.py checks.
#define RVMODEL_HALT_PASS \
  li a7, 93              ;\
  li a0, 0               ;\
  ecall                  ;\
self_loop_pass:          ;\
  j self_loop_pass       ;\

#define RVMODEL_HALT_FAIL \
  li a7, 93              ;\
  li a0, 1               ;\
  ecall                  ;\
self_loop_fail:          ;\
  j self_loop_fail       ;\

##### IO #####

#define RVMODEL_IO_INIT(_R1, _R2, _R3)

# Byte-at-a-time to the virtual UART's transmit holding register. The
# emulated transmitter is never busy, so there is no need to poll LSR.
#define RVMODEL_IO_WRITE_STR(_R1, _R2, _R3, _STR_PTR) \
1:                           ;                        \
  lbu  _R1, 0(_STR_PTR)      ; /* load byte        */  \
  beqz _R1, 3f               ; /* stop at the NUL  */  \
2:                           ;                        \
  li   _R2, 0x10000000       ; /* UART0 THR        */  \
  sw   _R1, 0(_R2)           ;                         \
  addi _STR_PTR, _STR_PTR, 1 ;                         \
  j 1b                       ;                         \
3:

##### Interrupt latency #####

# The interpreter samples for a pending interrupt once per instruction, so
# an armed timer is taken on the next instruction boundary.
#define RVMODEL_INTERRUPT_LATENCY 10

##### Machine timer #####

# The virtual CLINT uses the standard SiFive register layout.
#define RVMODEL_MTIME_ADDRESS    0x0200BFF8
#define RVMODEL_MTIMECMP_ADDRESS 0x02004000
#define RVMODEL_TIMER_INT_SOON_DELAY 100

##### Machine interrupts #####

# Software interrupts go through the CLINT's msip register. There is no
# external interrupt controller, so MEXT is left unimplemented.
#define RVMODEL_SET_MSW_INT(_R1, _R2) \
  li _R2, 0x02000000                 ;\
  li _R1, 1                          ;\
  sw _R1, 0(_R2)                     ;

#define RVMODEL_CLR_MSW_INT(_R1, _R2) \
  li _R2, 0x02000000                 ;\
  sw x0, 0(_R2)                      ;

#define RVMODEL_SET_MEXT_INT(_R1, _R2)
#define RVMODEL_CLR_MEXT_INT(_R1, _R2)

##### Supervisor interrupts #####

# No S-mode on this implementation.
#define RVMODEL_SET_SEXT_INT(_R1, _R2)
#define RVMODEL_CLR_SEXT_INT(_R1, _R2)
#define RVMODEL_SET_SSW_INT(_R1, _R2)
#define RVMODEL_CLR_SSW_INT(_R1, _R2)

#endif // _RVMODEL_MACROS_H
