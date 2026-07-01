#!/bin/bash
#
# Apply bento-zephyr-fw patches to the Zephyr source tree.
#
# Usage: apply-patches.sh [zephyr-root]
#   zephyr-root: Path to Zephyr source. Defaults to the in-repo
#                zephyrproject/zephyr checkout, falling back to the
#                ZEPHYR_BASE environment variable.
#
# Exit codes:
#   0 - All patches applied (or already applied)
#   1 - Patch application failed
#
# Patches are idempotent: an already-applied patch is detected via a
# reverse-apply check and skipped, so this is safe to run on every build.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PATCH_DIR="${SCRIPT_DIR}/zephyr"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

if [ -n "$1" ]; then
    ZEPHYR_ROOT="$1"
elif [ -d "${REPO_ROOT}/zephyrproject/zephyr" ]; then
    ZEPHYR_ROOT="${REPO_ROOT}/zephyrproject/zephyr"
elif [ -n "${ZEPHYR_BASE}" ]; then
    ZEPHYR_ROOT="${ZEPHYR_BASE}"
else
    echo "ERROR: No Zephyr root found. Pass one as an argument or set ZEPHYR_BASE."
    exit 1
fi

if [ ! -d "${ZEPHYR_ROOT}" ]; then
    echo "ERROR: Zephyr root not found at ${ZEPHYR_ROOT}"
    exit 1
fi

if [ ! -d "${PATCH_DIR}" ]; then
    echo "ERROR: Patch directory not found at ${PATCH_DIR}"
    exit 1
fi

echo "Applying patches to ${ZEPHYR_ROOT}"

APPLIED=0
SKIPPED=0
FAILED=0

for patch in "${PATCH_DIR}"/0*.patch; do
    [ -f "$patch" ] || continue
    name="$(basename "$patch")"

    # Check if already applied (reverse-apply test)
    if git -C "${ZEPHYR_ROOT}" apply --reverse --check "$patch" > /dev/null 2>&1; then
        echo "SKIP: ${name} (already applied)"
        SKIPPED=$((SKIPPED + 1))
        continue
    fi

    # Check if patch applies cleanly
    if git -C "${ZEPHYR_ROOT}" apply --check "$patch" > /dev/null 2>&1; then
        git -C "${ZEPHYR_ROOT}" apply "$patch"
        echo "OK:   ${name}"
        APPLIED=$((APPLIED + 1))
    else
        echo "FAIL: ${name}"
        git -C "${ZEPHYR_ROOT}" apply --check "$patch" 2>&1 || true
        FAILED=$((FAILED + 1))
    fi
done

echo ""
echo "Patches: ${APPLIED} applied, ${SKIPPED} skipped, ${FAILED} failed"

if [ ${FAILED} -gt 0 ]; then
    exit 1
fi

exit 0
