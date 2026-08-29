#!/bin/sh
# Host-side tests for the parts of the bootloader that are pure logic:
# EP0 control-IN packetisation, descriptor consistency, and DFU addressing.
#
# These compile the shipping src/usb_dfu.c natively with the USB register and
# packet-memory windows redirected to arrays, so they exercise the real code.
# They cannot cover anything that needs actual USB silicon - see the hardware
# checklist in the pull request / commit message for that.
set -eu

cd "$(dirname "$0")/.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

${CC:-cc} -std=gnu11 -Wall -Wextra -O1 -g \
    -I include -Wno-int-to-pointer-cast \
    -DUSB_BASE='((uintptr_t)usb_mock)' \
    -DUSB_PMAADDR='((uintptr_t)pma_mock)' \
    -DRCC_BASE='((uintptr_t)rcc_mock)' \
    -DFLASH_BASE='((uintptr_t)flash_mock)' \
    tools/test_ep0.c -o "$out/test_ep0"

if "$out/test_ep0" > "$out/log" 2>&1; then
    cat "$out/log"
else
    cat "$out/log"
    exit 1
fi

# The README documents a full-region dfu-util read, whose length has to track
# whatever region the descriptor advertises. Checked here because that constant
# has already gone stale twice when the region moved.
want=$(sed -n 's/^DOCS-DFUSE-ADDRESS: //p' "$out/log")
if [ -n "$want" ]; then
    if grep -q -- "$want" README.md; then
        echo "README --dfuse-address matches the advertised region ($want)"
    else
        echo "README --dfuse-address is stale: should be $want" >&2
        exit 1
    fi
fi
