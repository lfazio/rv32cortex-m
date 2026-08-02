/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core_cm4.h - the shim, under the name ST's device header asks for.
 *
 * stm32f446xx.h does `#include "core_cm4.h"` unconditionally, so putting
 * this directory ahead of CMSIS on the include path is the whole
 * integration: every vendor header and every HAL source then gets the RV32
 * implementation of CMSIS-Core without a line of them changing.
 *
 * IRQn_Type is already defined by the time this is reached -- the device
 * header declares the enumeration and then includes this -- so the shim's
 * fallback definition is suppressed.
 */
#ifndef CORE_CM4_H_SHIM
#define CORE_CM4_H_SHIM

#define CMSIS_RV32_HAS_IRQN 1
#include "cmsis_rv32.h"

/*
 * SCB exists mostly so startup and HAL code can set VTOR and read CPUID.
 * There is no vector table to relocate here, so the fields are a plain
 * shadow: writes are accepted and have no effect, which is the honest
 * behaviour for a register whose function does not exist.
 */
typedef struct {
    __I  uint32_t CPUID;
    __IO uint32_t ICSR;
    __IO uint32_t VTOR;
    __IO uint32_t AIRCR;
    __IO uint32_t SCR;
    __IO uint32_t CCR;
    __IO uint8_t  SHP[12];
    __IO uint32_t SHCSR;
    __IO uint32_t CFSR;
    __IO uint32_t HFSR;
    __IO uint32_t DFSR;
    __IO uint32_t MMFAR;
    __IO uint32_t BFAR;
} SCB_Type_shim;

extern SCB_Type_shim cmsis_rv32_scb;
#define SCB (&cmsis_rv32_scb)

#define SCB_AIRCR_VECTKEY_Pos    16u
#define SCB_AIRCR_PRIGROUP_Pos    8u
#define SCB_AIRCR_PRIGROUP_Msk  (7u << SCB_AIRCR_PRIGROUP_Pos)
#define SCB_CCR_DIV_0_TRP_Msk   (1u << 4)

/* Cache maintenance: the Cortex-M4 has none, and neither does this. */
#define SCB_EnableICache()      ((void)0)
#define SCB_EnableDCache()      ((void)0)
#define SCB_DisableICache()     ((void)0)
#define SCB_DisableDCache()     ((void)0)

/*
 * SysTick as a register block. HAL only ever reaches it through
 * SysTick_Config, which the shim implements against the ACLINT timer, but
 * driver code occasionally pokes CTRL directly to stop the tick.
 */
typedef struct {
    __IO uint32_t CTRL;
    __IO uint32_t LOAD;
    __IO uint32_t VAL;
    __I  uint32_t CALIB;
} SysTick_Type_shim;

extern SysTick_Type_shim cmsis_rv32_systick;
#define SysTick (&cmsis_rv32_systick)

#define SysTick_CTRL_ENABLE_Msk     (1u << 0)
#define SysTick_CTRL_TICKINT_Msk    (1u << 1)
#define SysTick_CTRL_CLKSOURCE_Msk  (1u << 2)
#define SysTick_CTRL_COUNTFLAG_Msk  (1u << 16)
#define SysTick_LOAD_RELOAD_Msk     0x00FFFFFFu

/*
 * Bits of SCB that HAL names. Sleep and fault-enable control hardware that
 * does not exist here, so the shadow accepts them and nothing happens --
 * which is better than failing to compile, because the calls sit in
 * initialisation paths a driver cannot easily avoid.
 */
#define SCB_SCR_SLEEPDEEP_Msk       (1u << 2)
#define SCB_SCR_SLEEPONEXIT_Msk     (1u << 1)
#define SCB_SCR_SEVONPEND_Msk       (1u << 4)
#define SCB_SHCSR_MEMFAULTENA_Msk   (1u << 16)
#define SCB_SHCSR_BUSFAULTENA_Msk   (1u << 17)
#define SCB_SHCSR_USGFAULTENA_Msk   (1u << 18)

/*
 * The MPU, as a shadow. RISC-V's equivalent is PMP, which the emulator
 * implements -- but the two describe regions differently enough that
 * translating HAL_MPU_ConfigRegion would be a guess about intent rather
 * than a mapping. Accepting the writes keeps vendor initialisation
 * compiling and running; a guest that actually needs protection should
 * program PMP directly.
 */
typedef struct {
    __I  uint32_t TYPE;
    __IO uint32_t CTRL;
    __IO uint32_t RNR;
    __IO uint32_t RBAR;
    __IO uint32_t RASR;
} MPU_Type_shim;

extern MPU_Type_shim cmsis_rv32_mpu;
#define MPU (&cmsis_rv32_mpu)

#define MPU_CTRL_ENABLE_Msk         (1u << 0)
#define MPU_CTRL_PRIVDEFENA_Msk     (1u << 2)
#define MPU_RASR_ENABLE_Msk         (1u << 0)
#define MPU_RASR_ENABLE_Pos          0u
#define MPU_RASR_SIZE_Pos            1u
#define MPU_RASR_SRD_Pos             8u
#define MPU_RASR_B_Pos              16u
#define MPU_RASR_C_Pos              17u
#define MPU_RASR_S_Pos              18u
#define MPU_RASR_TEX_Pos            19u
#define MPU_RASR_AP_Pos             24u
#define MPU_RASR_XN_Pos             28u
#define MPU_RBAR_ADDR_Msk           0xFFFFFFE0u

/*
 * The remaining NVIC calls HAL_Cortex makes. NVIC_GetActive has no meaning
 * with one flat interrupt level and no nesting, and decoding a priority
 * group is the inverse of an encode that does nothing.
 */
__STATIC_INLINE uint32_t NVIC_GetActive(IRQn_Type irqn) { (void)irqn; return 0u; }

__STATIC_INLINE void NVIC_DecodePriority(uint32_t priority, uint32_t group,
                                         uint32_t *pre, uint32_t *sub)
{
    (void)group;
    if (pre != 0) { *pre = priority; }
    if (sub != 0) { *sub = 0u; }
}

#define SysTick_IRQn                (-1)
#define NonMaskableInt_IRQn         (-14)

#endif /* CORE_CM4_H_SHIM */
