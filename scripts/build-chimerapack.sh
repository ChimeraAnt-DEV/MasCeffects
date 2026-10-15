#!/usr/bin/env bash
#
# Builds the MasCeffects native mod and packages it as a .chimerapack for
# GlowberryClient.
#
# Usage:
#   scripts/build-chimerapack.sh [--clean]
#
# Environment:
#   ANDROID_NDK_HOME   Android NDK root (NDK r28 recommended)
#   PRELOADER_ROOT     preloader-android checkout
#                      (default: app/src/main/cpp/preloader)
#
# Output:
#   dist/arm64-v8a/masceffects/             the unpacked mod directory
#   dist/arm64-v8a/masceffects.chimerapack  the importable package
#
# The .chimerapack extension is a ZIP archive. The launcher's importer accepts
# .zip and .chimerapack identically, and the archive root must hold
# manifest.json plus exactly one .so - the layout produced below.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

ABI="arm64-v8a"
BUILD_TYPE="Release"
ANDROID_PLATFORM="android-28"

CLEAN=0
for arg in "$@"; do
  case "$arg" in
    --clean) CLEAN=1 ;;
    *) echo "unknown argument: $arg" >&2; exit 2 ;;
  esac
done

if [[ -z "${PRELOADER_ROOT:-}" ]]; then
  PRELOADER_ROOT="${PROJECT_DIR}/app/src/main/cpp/preloader"
fi

if [[ ! -f "${PRELOADER_ROOT}/include/pl/Mod.hpp" ]]; then
  echo "preloader-android headers not found under ${PRELOADER_ROOT}" >&2
  echo "set PRELOADER_ROOT to a checkout of ChimeraAnt-DEV/preloader-android" >&2
  exit 1
fi

# Resolve the NDK from the locations the client's own script accepts.
if [[ -z "${ANDROID_NDK_HOME:-}" ]]; then
  for candidate in "${ANDROID_NDK_ROOT:-}" "${ANDROID_HOME:-}/ndk-bundle"; do
    if [[ -n "${candidate}" && -f "${candidate}/build/cmake/android.toolchain.cmake" ]]; then
      ANDROID_NDK_HOME="${candidate}"
      break
    fi
  done
fi
if [[ -z "${ANDROID_NDK_HOME:-}" ]]; then
  for sdk in "${ANDROID_HOME:-}" "${ANDROID_SDK_ROOT:-}"; do
    if [[ -n "${sdk}" && -d "${sdk}/ndk" ]]; then
      ANDROID_NDK_HOME="$(ls -1d "${sdk}"/ndk/* 2>/dev/null | sort -V | tail -1)"
      break
    fi
  done
fi
if [[ -z "${ANDROID_NDK_HOME:-}" || ! -f "${ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake" ]]; then
  echo "Android NDK not found; set ANDROID_NDK_HOME" >&2
  exit 1
fi

HOST_BUILD_DIR="${PROJECT_DIR}/build/host-${BUILD_TYPE}"
ANDROID_BUILD_DIR="${PROJECT_DIR}/build/android-${ABI}-${BUILD_TYPE}"
DIST_DIR="${PROJECT_DIR}/dist/${ABI}"
PACKAGE_DIR="${DIST_DIR}/masceffects"

if [[ "${CLEAN}" -eq 1 ]]; then
  rm -rf "${HOST_BUILD_DIR}" "${ANDROID_BUILD_DIR}" "${DIST_DIR}"
fi

# 1. Build and run the host tests first: a broken effect rule must not reach a
#    package.
cmake -S "${PROJECT_DIR}" -B "${HOST_BUILD_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DPRELOADER_ANDROID_ROOT="${PRELOADER_ROOT}"
cmake --build "${HOST_BUILD_DIR}"
"${HOST_BUILD_DIR}/masc_effect_tests"

# 2. Generate the packaged default config and schema.
GEN="${HOST_BUILD_DIR}/masceffects_config_gen"
rm -rf "${PACKAGE_DIR}"
mkdir -p "${PACKAGE_DIR}/config"
"${GEN}" "${PACKAGE_DIR}"

# 3. Build the mod library for the device ABI.
cmake -S "${PROJECT_DIR}" -B "${ANDROID_BUILD_DIR}" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="${ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI="${ABI}" \
  -DANDROID_PLATFORM="${ANDROID_PLATFORM}" \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DPRELOADER_ANDROID_ROOT="${PRELOADER_ROOT}"
cmake --build "${ANDROID_BUILD_DIR}" --target masceffects

LIBRARY="${ANDROID_BUILD_DIR}/out/${ABI}/libmasceffects.so"
if [[ ! -f "${LIBRARY}" ]]; then
  echo "built library not found: ${LIBRARY}" >&2
  exit 1
fi

cp "${PROJECT_DIR}/manifest.json" "${PACKAGE_DIR}/manifest.json"
cp "${LIBRARY}" "${PACKAGE_DIR}/libmasceffects.so"

# 4. Package. The archive root holds manifest.json, the .so and config/, which
#    is exactly what the importer's validation expects.
ARCHIVE="${DIST_DIR}/masceffects.chimerapack"
rm -f "${ARCHIVE}"
if command -v zip >/dev/null 2>&1; then
  ( cd "${PACKAGE_DIR}" && zip -q -r "${ARCHIVE}" . )
else
  # A ZIP with no extra attributes, so the archive is identical wherever it is
  # built. Stored, not deflated, for the .so: it is already compressed.
  python3 - "${PACKAGE_DIR}" "${ARCHIVE}" <<'PY'
import os
import sys
import zipfile

package_dir, archive = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as zf:
    for root, _dirs, files in os.walk(package_dir):
        for name in sorted(files):
            full = os.path.join(root, name)
            rel = os.path.relpath(full, package_dir).replace(os.sep, "/")
            zf.write(full, rel)
PY
fi

echo "Built MasCeffects:"
echo "  ${PACKAGE_DIR}"
echo "  ${ARCHIVE}"
