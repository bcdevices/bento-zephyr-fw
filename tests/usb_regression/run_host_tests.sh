#!/bin/bash
#
# Build and run the USB regression tests as a native host binary.
#
# native_sim requires Linux, so on macOS this is the only way to run them
# locally. Under Linux/CI, prefer the real thing:
#     west twister -p native_sim -T tests/usb_regression
#
set -e
cd "$(dirname "$0")"

python3 - <<'PY'
import re, glob

files = sorted(glob.glob("src/test_*.c"))
tests = []
for f in files:
    src = open(f).read()
    tests += re.findall(r'^ZTEST\((\w+),\s*(\w+)\)', src, re.M)

with open("src/host_main.c", "w") as out:
    out.write('#include "../host_shim.h"\n')
    for f in files:
        out.write('#include "%s"\n' % f.split("/")[-1])
    out.write("int main(void)\n{\n")
    out.write('\tprintf("USB regression tests\\n");\n')
    for suite, name in tests:
        out.write("\trun_%s_%s();\n" % (suite, name))
    out.write('\tprintf("\\n%d run, %d failed\\n", host_tests_run, '
              'host_tests_failed);\n')
    out.write("\treturn host_tests_failed ? 1 : 0;\n}\n")

print("generated runner for %d tests" % len(tests))
PY

cc -DHOST_TEST -I. -Wno-unused-function -o /tmp/usb_regression_tests \
   src/host_main.c -Wall
/tmp/usb_regression_tests
