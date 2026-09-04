#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Build ST's Template FSBL for the Nucleo-N657X0-Q, as a control.
#
# Why this exists
# ---------------
# The N6 port spent several rounds unable to distinguish "our firmware is
# broken" from "the board or the tooling is". The thing that settled it
# was building ST's own template and loading it exactly the same way: if
# ST's code does not run either, the fault is not ours.
#
# It earns its place beyond that one occasion. It blinks the three LEDs
# and needs no console, so it can be judged without a working UART -- and
# a console is precisely what this port could not get. Keep it working.
#
# **Judge it by a witness, not by the LEDs' registers.** The boot ROM
# configures those same pins itself, so GPIOG MODER reads FFDFFFFF before
# anything of ours is started, and reading it gave two false "it runs"
# conclusions. Either watch the board with your eyes, or stop in main
# under a debugger.
#
#   tools/n6-control/build.sh [output-dir]
#
# Needs STM32CubeN6 checked out (it fetches one if absent) and the vendor
# packs, which the N6 firmware build already downloads into
# build/hw-n6/_deps.

set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
OUT=${1:-$ROOT/tools/n6-control}
DEPS=$ROOT/build/hw-n6/_deps
CUBE=${STM32CUBEN6_DIR:-$OUT/STM32CubeN6}

HAL=$DEPS/stm32n6xx_hal_driver-src
DEV=$DEPS/cmsis_device_n6-src
CORE=$DEPS/cmsis_core-src

if [ ! -d "$HAL" ]; then
    echo "error: vendor packs not found at $DEPS" >&2
    echo "       configure the N6 firmware first:" >&2
    echo "       cmake -B build/hw-n6 -DEMU_PLATFORM=stm32n6 \\" >&2
    echo "             -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake" >&2
    exit 1
fi

#
# The BSP is a *submodule* of STM32CubeN6, so a plain clone leaves
# Drivers/BSP/STM32N6xx_Nucleo empty and the build fails on a missing
# stm32n6xx_nucleo.h. That is what --filter=blob:none and sparse checkout
# looked like too, which cost a while to tell apart: three different ways
# to get an empty directory that all look like "the file is not in the
# repository".
#
if [ ! -f "$CUBE/Drivers/BSP/STM32N6xx_Nucleo/stm32n6xx_nucleo.c" ]; then
    if [ ! -d "$CUBE/.git" ]; then
        echo "fetching STM32CubeN6 into $CUBE"
        git clone --depth 1 -q \
            https://github.com/STMicroelectronics/STM32CubeN6.git "$CUBE"
    fi
    echo "fetching the Nucleo BSP submodule"
    (cd "$CUBE" && git submodule update --init --depth 1 \
        Drivers/BSP/STM32N6xx_Nucleo >/dev/null)
fi

T=$CUBE/Projects/NUCLEO-N657X0-Q/Templates/Template
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cp "$T"/FSBL/Src/*.c "$T"/FSBL/Inc/*.h "$WORK/"
cp "$CUBE"/Drivers/BSP/STM32N6xx_Nucleo/stm32n6xx_nucleo.c \
   "$CUBE"/Drivers/BSP/STM32N6xx_Nucleo/stm32n6xx_nucleo.h \
   "$CUBE"/Drivers/BSP/STM32N6xx_Nucleo/stm32n6xx_nucleo_errno.h "$WORK/"
cp "$DEV/Source/Templates/gcc/startup_stm32n657xx_fsbl.s" "$WORK/"
cp "$T/STM32CubeIDE/FSBL/STM32N657X0HXQ_AXISRAM2_fsbl.ld" "$WORK/"

#
# -mcmse, for the same reason the firmware needs it: the FSBL system file
# reaches SAU and SCB_NS, and stm32n657xx.h defines CPU_IN_SECURE_STATE
# off __ARM_FEATURE_CMSE -- which decides whether USART1 means the secure
# alias or the non-secure one.
#
CF="-mcpu=cortex-m55 -mthumb -mfloat-abi=hard -mcmse
    -O2 -ffunction-sections -fdata-sections
    -DSTM32N657xx -DUSE_HAL_DRIVER
    -I$WORK -I$CORE/Include -I$DEV/Include -I$HAL/Inc"

SRC="$WORK/main.c $WORK/stm32n6xx_hal_msp.c $WORK/stm32n6xx_it.c
     $WORK/system_stm32n6xx_fsbl.c $WORK/stm32n6xx_nucleo.c
     $WORK/startup_stm32n657xx_fsbl.s"
for m in hal hal_cortex hal_dma hal_exti hal_gpio hal_pwr hal_pwr_ex \
         hal_rcc hal_rcc_ex; do
    SRC="$SRC $HAL/Src/stm32n6xx_$m.c"
done

# shellcheck disable=SC2086
arm-none-eabi-gcc $CF $SRC \
    -T "$WORK/STM32N657X0HXQ_AXISRAM2_fsbl.ld" \
    -Wl,--gc-sections --specs=picolibc.specs \
    -o "$OUT/st-template-fsbl.elf"

arm-none-eabi-objcopy -O binary "$OUT/st-template-fsbl.elf" \
    "$OUT/st-template-fsbl.bin"

echo "built $OUT/st-template-fsbl.elf"
arm-none-eabi-nm "$OUT/st-template-fsbl.elf" |
    awk '/ (Reset_Handler|main)$/ { printf "  %-14s 0x%s\n", $3, $1 }'
echo
echo "load it with the \"N6: ST's Template FSBL (control)\" configuration"
echo "in .vscode/launch.json, or by hand:"
echo "  STM32_Programmer_CLI -c port=SWD mode=HOTPLUG ap=1 \\"
echo "      -w $OUT/st-template-fsbl.bin 0x34180400"
