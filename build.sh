#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="${SCRIPT_DIR}/out"

if [ "$1" = "clean" ]; then
    echo "Cleaning build outputs..."
    rm -rf "${OUT_DIR}" "${SCRIPT_DIR}/vendor.oplus.hardware.urcc-service"
    echo "Clean complete."
    exit 0
fi

# Clean legacy output in root folder if present
rm -f "${SCRIPT_DIR}/vendor.oplus.hardware.urcc-service"

mkdir -p "${OUT_DIR}"

NDK_PATH="${ANDROID_NDK_ROOT:-/home/rapli/android-ndk-r26b}"
TOOLCHAIN="${NDK_PATH}/toolchains/llvm/prebuilt/linux-x86_64"
CC="${TOOLCHAIN}/bin/aarch64-linux-android34-clang"
STRIP="${TOOLCHAIN}/bin/llvm-strip"
READELF="${TOOLCHAIN}/bin/llvm-readelf"

SOURCE_FILE="${SCRIPT_DIR}/urcc_service.cpp"
RC_FILE="${SCRIPT_DIR}/vendor.oplus.hardware.urcc-service.rc"
OUTPUT_BINARY="${OUT_DIR}/vendor.oplus.hardware.urcc-service"
OUTPUT_RC="${OUT_DIR}/vendor.oplus.hardware.urcc-service.rc"
LIBS_DIR="${SCRIPT_DIR}/device_libs"

echo "=== Building vendor.oplus.hardware.urcc-service for Android 14+ (API 34) ==="

if [ ! -f "${CC}" ]; then
    echo "Error: Clang compiler not found at ${CC}"
    exit 1
fi

if [ ! -d "${LIBS_DIR}" ]; then
    echo "Error: device_libs directory not found at ${LIBS_DIR}"
    exit 1
fi

echo "[1/4] Compiling C source for aarch64 Android 14+ (API 34)..."
"${CC}" -O2 -Wall -Wextra \
    -Wl,-z,max-page-size=16384 \
    "${SOURCE_FILE}" \
    -L"${LIBS_DIR}" \
    -llog -lbinder_ndk \
    -o "${OUTPUT_BINARY}"

echo "[2/4] Stripping binary symbols..."
"${STRIP}" --strip-unneeded "${OUTPUT_BINARY}"
chmod 755 "${OUTPUT_BINARY}"

echo "[3/4] Copying init configuration (.rc) and VINTF manifest..."
cp -f "${RC_FILE}" "${OUTPUT_RC}"
chmod 644 "${OUTPUT_RC}"
cp -f "${SCRIPT_DIR}/manifest_oplus_urcc.xml" "${OUT_DIR}/manifest_oplus_urcc.xml"
chmod 644 "${OUT_DIR}/manifest_oplus_urcc.xml"

echo "[4/4] Verifying binary dependencies..."
"${READELF}" -d "${OUTPUT_BINARY}" | grep -E "NEEDED|SONAME|FLAGS"

tar -czf "${OUT_DIR}/urcc_package.tar.gz" -C "${OUT_DIR}" \
    vendor.oplus.hardware.urcc-service \
    vendor.oplus.hardware.urcc-service.rc \
    manifest_oplus_urcc.xml

python3 -c '
import zipfile, os
os.chdir("'"${OUT_DIR}"'")
with zipfile.ZipFile("urcc_package.zip", "w", zipfile.ZIP_DEFLATED) as z:
    for f in ["vendor.oplus.hardware.urcc-service", "vendor.oplus.hardware.urcc-service.rc", "manifest_oplus_urcc.xml"]:
        z.write(f)
'

echo ""
echo "=== Build Complete: Output files located in ${OUT_DIR} ==="
ls -lh "${OUT_DIR}"
file "${OUTPUT_BINARY}"
