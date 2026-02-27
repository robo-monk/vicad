#!/bin/sh
# check-opcode-sync.sh — verify OpCode enum in ipc_protocol.h is in sync with
# worker/ipc_protocol.ts (matching numeric values) and docs/ipc-protocol.md.
#
# Exits 0 if all entries match; exits 1 with a summary of failures.

PASS=0
FAIL=0

# Extract "Name = value" lines from the C++ OpCode enum.
# Reads between "enum class OpCode" and the closing "};"
CPP_OPCODES=$(awk '/enum class OpCode/,/^\};/' src/ipc_protocol.h \
    | grep -E '^\s+[A-Za-z]+ = [0-9]+,' \
    | sed 's/[[:space:]]//g; s/,//')

if [ -z "$CPP_OPCODES" ]; then
    echo "ERROR: could not parse OpCode enum from src/ipc_protocol.h" >&2
    exit 1
fi

while IFS='=' read -r name value; do
    # Derive UPPER_SNAKE key from CamelCase name.
    # e.g. CrossFilletCorners → CROSS_FILLET_CORNERS
    ts_key=$(printf '%s' "$name" \
        | sed 's/\([A-Z][a-z]\)/_ \1/g; s/^ //; s/ //g' \
        | tr '[:lower:]' '[:upper:]' \
        | sed 's/\([A-Z]\)\([A-Z][a-z]\)/\1_\2/g')
    # The sed above doesn't quite handle all transitions; use a simpler approach:
    ts_key=$(printf '%s' "$name" | sed 's/\([a-z0-9]\)\([A-Z]\)/\1_\2/g' | tr '[:lower:]' '[:upper:]')

    # ── TypeScript check ────────────────────────────────────────────────────
    # Match "  KEY: VALUE," or "  KEY: VALUE," (trailing comma optional)
    ts_line=$(grep -E "^\s+${ts_key}\s*:\s*${value}" worker/ipc_protocol.ts)
    if [ -n "$ts_line" ]; then
        echo "PASS: OP.${ts_key} = ${value}"
        PASS=$((PASS + 1))
    else
        echo "FAIL: OP.${ts_key} = ${value} — not found in worker/ipc_protocol.ts"
        FAIL=$((FAIL + 1))
    fi

    # ── Docs check ─────────────────────────────────────────────────────────
    # Match markdown table row "| VALUE | NAME |"
    doc_line=$(grep -E "^\|\s*${value}\s*\|\s*${name}\s*\|" docs/ipc-protocol.md)
    if [ -n "$doc_line" ]; then
        echo "PASS: docs op ${value} ${name}"
        PASS=$((PASS + 1))
    else
        echo "FAIL: op ${value} ${name} — not found in docs/ipc-protocol.md"
        FAIL=$((FAIL + 1))
    fi
done <<EOF
$CPP_OPCODES
EOF

echo ""
echo "Op code sync: $PASS passed, $FAIL failed."
[ "$FAIL" -eq 0 ]
