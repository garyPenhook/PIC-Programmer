#!/bin/sh
# PostToolUse hook: after a C edit, report clang-format drift and any build break to Claude.
f=$(jq -r '(.tool_input.file_path // .tool_response.filePath) // empty')
case "$f" in
*.c | *.h) ;;
*) exit 0 ;;
esac
cd "$(dirname "$0")/.." || exit 0
case "$f" in tests/vendor/*|*/tests/vendor/*) exit 0 ;; esac
out=""
fmt=$(clang-format --dry-run "$f" 2>&1 | grep -c 'warning:')
[ "$fmt" -gt 0 ] && out="clang-format: $fmt style deviation(s) in $f (run clang-format -i on it).\n"
case "$f" in
*/tests/*) ;;
*)
    if [ -f build/Debug/Makefile ]; then
        b=$(cmake --build build/Debug 2>&1 | grep -E 'error|Error|warning:' | head -15)
        [ -n "$b" ] && out="${out}Build problems:\n$b\n"
    fi
    ;;
esac
[ -n "$out" ] && jq -n --arg c "$(printf "$out")" '{hookSpecificOutput:{hookEventName:"PostToolUse",additionalContext:$c}}'
exit 0
