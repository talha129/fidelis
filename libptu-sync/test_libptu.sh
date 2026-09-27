#!/bin/bash
# Quick smoke test for libptu.so — run on Linux after 'make'
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LIB="$SCRIPT_DIR/libptu.so"

if [ ! -f "$LIB" ]; then
    echo "ERROR: $LIB not found. Run 'make' first."
    exit 1
fi

echo "=== Test 1: log mode — /bin/ls ==="
OUT=$(mktemp -d)
LIBPTU_MODE=log LIBPTU_OUTPUT="$OUT" LD_PRELOAD="$LIB" /bin/ls /tmp > /dev/null
ENTRIES=$(wc -l < "$OUT/cde.manifest")
echo "  manifest entries: $ENTRIES"
echo "  first 5 entries:"
head -5 "$OUT/cde.manifest" | sed 's/^/    /'
[ "$ENTRIES" -gt 0 ] || { echo "FAIL: empty manifest"; exit 1; }
rm -rf "$OUT"

echo ""
echo "=== Test 2: log mode — python3 matmul ==="
SCRIPT=$(mktemp /tmp/matmul_XXXXXX.py)
cat > "$SCRIPT" << 'EOF'
import numpy as np
A = np.random.rand(200, 200)
B = np.random.rand(200, 200)
C = A @ B
print(f"checksum={float(C[0,0]):.6f}")
EOF
OUT=$(mktemp -d)
LIBPTU_MODE=log LIBPTU_OUTPUT="$OUT" LD_PRELOAD="$LIB" python3 "$SCRIPT"
ENTRIES=$(wc -l < "$OUT/cde.manifest")
echo "  manifest entries: $ENTRIES"
[ "$ENTRIES" -gt 50 ] || { echo "FAIL: too few entries for python+numpy"; exit 1; }
rm -f "$SCRIPT"

echo ""
echo "=== Test 3: overhead measurement ==="
echo "  baseline (no libptu):"
time (for i in $(seq 10); do python3 "$SCRIPT" > /dev/null 2>&1; done) 2>&1 | grep real || true

echo "  with libptu log:"
time (for i in $(seq 10); do \
    LIBPTU_MODE=log LIBPTU_OUTPUT="$OUT" LD_PRELOAD="$LIB" \
    python3 "$SCRIPT" > /dev/null 2>&1; \
    done) 2>&1 | grep real || true

rm -rf "$OUT"
echo ""
echo "=== All tests passed ==="
