#!/bin/sh
# TCC Test Suite Runner — run inside VibeOS shell.
# Compiles each test with TCC and runs it.
echo "=== TCC Test Suite ==="
echo ""
echo "--- test_all (precompiled) ---"
/bin/test_all
echo ""
echo "--- Compiling tests with TCC... ---"
for t in basic float structs malloc string fileio; do
    echo "  tcc /tests/test_$t.c -o /tmp/t_$t"
    tcc /tests/test_$t.c -o /tmp/t_$t
    if [ $? -ne 0 ]; then
        echo "FAIL: compile error in test_$t"
        continue
    fi
    echo "  running /tmp/t_$t"
    /tmp/t_$t
    if [ $? -ne 0 ]; then
        echo "FAIL: test_$t returned error"
    fi
    echo ""
done
echo "=== Done ==="
