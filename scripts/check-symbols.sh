#!/usr/bin/env bash
#
# Verifies that every pl:: symbol the mod imports is actually exported by the
# preloader.
#
# Why this exists: the preloader is built with -fvisibility=hidden, so only
# symbols carrying PL_EXPORT land in its dynamic symbol table. A public header
# can declare something the built library does not export (GlesFunctions::usable
# and resolveVtableSlotReferencingString are both examples), and importing it
# makes the mod fail to load with "missing symbol" rather than fail to build.
# Catching that here is the difference between a broken release and a clean one.
#
# Usage:
#   PRELOADER_SO=/path/to/libpreloader.so scripts/check-symbols.sh [mod.so]
#
# Exit code 0 means every import is satisfied.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

MOD_SO="${1:-${PROJECT_DIR}/dist/arm64-v8a/masceffects/libmasceffects.so}"
PRELOADER_SO="${PRELOADER_SO:-}"

if [[ -z "${PRELOADER_SO}" ]]; then
  echo "PRELOADER_SO must point at a built libpreloader.so" >&2
  echo "  e.g. cmake -S \$PRELOADER_ROOT -B build/preloader ... && cmake --build build/preloader" >&2
  exit 2
fi
if [[ ! -f "${PRELOADER_SO}" ]]; then
  echo "not found: ${PRELOADER_SO}" >&2
  exit 2
fi
if [[ ! -f "${MOD_SO}" ]]; then
  echo "not found: ${MOD_SO} (build the mod first)" >&2
  exit 2
fi

# Prefer the NDK's readelf, which understands the Android ELF layout.
READELF=""
for candidate in \
    "${ANDROID_NDK_HOME:-}/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf" \
    "$(command -v llvm-readelf 2>/dev/null || true)" \
    "$(command -v readelf 2>/dev/null || true)"; do
  if [[ -n "${candidate}" && -x "${candidate}" ]]; then
    READELF="${candidate}"
    break
  fi
done
if [[ -z "${READELF}" ]]; then
  echo "no readelf found; set ANDROID_NDK_HOME" >&2
  exit 2
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

# Functions the preloader exports.
"${READELF}" --dyn-syms "${PRELOADER_SO}" \
  | awk '$7!="UND" && $4=="FUNC" {print $8}' | sort -u > "${TMP_DIR}/have.txt"

# pl:: symbols the mod imports (mangled names all start with _ZN2pl / _ZNK2pl).
"${READELF}" --dyn-syms "${MOD_SO}" \
  | awk '$7=="UND" {print $8}' | grep -E '^_ZNK?2pl' | sort -u > "${TMP_DIR}/want.txt" || true

WANT_COUNT="$(wc -l < "${TMP_DIR}/want.txt" | tr -d ' ')"
MISSING="$(comm -23 "${TMP_DIR}/want.txt" "${TMP_DIR}/have.txt")"

echo "mod imports      : ${WANT_COUNT} pl:: symbols"
echo "preloader exports: $(wc -l < "${TMP_DIR}/have.txt" | tr -d ' ') functions"

if [[ -n "${MISSING}" ]]; then
  echo
  echo "MISSING SYMBOLS (the mod will fail to load with 'missing symbol'):"
  while IFS= read -r sym; do
    [[ -z "${sym}" ]] && continue
    # Demangle when a demangler is available, so the report names the API.
    pretty="${sym}"
    for demangler in llvm-cxxfilt c++filt; do
      if command -v "${demangler}" >/dev/null 2>&1; then
        pretty="$(${demangler} "${sym}" 2>/dev/null || echo "${sym}")"
        break
      fi
    done
    echo "  ${pretty}"
    echo "    ${sym}"
  done <<< "${MISSING}"
  echo
  echo "Replace the call with an exported equivalent, or drop it."
  exit 1
fi

echo
echo "OK: every imported pl:: symbol is exported by the preloader."
