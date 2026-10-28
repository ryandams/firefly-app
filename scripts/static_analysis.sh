#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
"$root/scripts/check_thread_registry.sh"
rg -n 'zbus_chan_pub\(' "$root/code" | while IFS= read -r line; do
    case "$line" in *'return zbus_chan_pub'*) ;; *) echo "unchecked zbus publish: $line" >&2; exit 1 ;; esac
done
if command -v clang-tidy >/dev/null && [ -f "${1:-}/compile_commands.json" ]; then
    clang-tidy -p "$1" "$root"/code/*.c "$root"/code/helpers/*.cpp "$root"/code/service/*.{c,cpp}
fi
