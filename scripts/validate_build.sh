#!/usr/bin/env bash
set -euo pipefail
workspace="${1:?workspace path required}"
target="$workspace/builds/build/firefly-app/zephyr"
native="$workspace/builds/build_sim/firefly-app/zephyr"
for alias in led0 led1 led2 led3 sw0 sw1 sw2 sw3; do
    rg -q "^[[:space:]]*$alias = &" "$target/zephyr.dts" || { echo "target missing alias $alias" >&2; exit 1; }
    rg -q "^[[:space:]]*$alias = &" "$native/zephyr.dts" || { echo "native missing alias $alias" >&2; exit 1; }
done
rg -q '^CONFIG_SEGGER_SYSTEMVIEW=y$' "$target/.config"
! rg -q '^CONFIG_SEGGER_SYSTEMVIEW=y$' "$native/.config"
rg -q '^CONFIG_GPIO_EMUL=y$' "$native/.config"
! rg -q '^CONFIG_GPIO_EMUL=y$' "$target/.config"
echo "generated target/native configuration validated"
