#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
registry="$root/tests/thread/registry.txt"
status=0
while IFS=: read -r file _ declaration; do
    class="$(printf '%s\n' "$declaration" | sed -nE 's/.*class[[:space:]]+([A-Za-z_][A-Za-z0-9_]*).*/\1/p')"
    [ -n "$class" ] || continue
    relative="${file#"$root/"}"
    row="$(awk -v class="$class" '$1 == class {print; exit}' "$registry")"
    if [ -z "$row" ]; then echo "unregistered Thread subclass: $class ($relative)" >&2; status=1; continue; fi
    source_path="$(printf '%s\n' "$row" | awk '{print $2}')"
    test_path="$(printf '%s\n' "$row" | awk '{print $3}')"
    [ "$source_path" = "$relative" ] || { echo "$class registry source mismatch" >&2; status=1; }
    [ -f "$root/$test_path" ] || { echo "$class test missing: $test_path" >&2; status=1; }
done < <(rg -n 'class[[:space:]]+[A-Za-z_][A-Za-z0-9_]*.*public[[:space:]]+firefly::Thread<' "$root/code" -g '*.hpp' -g '*.cpp')
exit "$status"
