#!/bin/sh
# Report how much of the 12KB bootloader region each build uses.
#
# The linker already fails hard if an image overflows the region, so this is
# about visibility: the bootloader shares 0x08000000-0x08002FFF with nothing,
# and silently creeping toward the ceiling is how you find out the hard way.
set -eu

LIMIT=12288   # must match FLASH LENGTH in bootloader.ld
ENVS="${*:-nova_bootloader nova_bootloader_120}"

SIZE=$(command -v arm-none-eabi-size 2>/dev/null || true)
if [ -z "$SIZE" ]; then
    SIZE=$(find "${HOME}/.platformio/packages/toolchain-gccarmnoneeabi/bin" \
                -name 'arm-none-eabi-size' 2>/dev/null | head -1)
fi
if [ -z "$SIZE" ]; then
    echo "arm-none-eabi-size not found" >&2
    exit 1
fi

status=0
for env in $ENVS; do
    elf=".pio/build/${env}/firmware.elf"
    if [ ! -f "$elf" ]; then
        echo "$env: no build at $elf - run 'pio run -e $env' first" >&2
        status=1
        continue
    fi

    # size output: text data bss dec hex filename
    set -- $("$SIZE" "$elf" | tail -n1)
    used=$(( $1 + $2 ))
    pct=$(( used * 100 / LIMIT ))

    printf '%-22s %6d / %d bytes  (%d%% of the bootloader region)\n' \
           "$env" "$used" "$LIMIT" "$pct"

    if [ "$used" -gt "$LIMIT" ]; then
        echo "  ERROR: overflows the 12KB bootloader region" >&2
        status=1
    fi
done

exit $status
