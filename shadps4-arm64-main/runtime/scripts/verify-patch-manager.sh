#!/usr/bin/env bash
set -euo pipefail

# Dedicated patch-manager verification entrypoint.
#
# The managed-patch boundary test (NativePatchDomainE2ETest) drives the REAL native C ABI
# through the JNI symbols, so the host library MUST be built first and the JNI E2E MUST run.
# This script builds the host .so, then runs the Android unit tests with
# -Dpatch.domain.require=true so the E2E FAILS (instead of silently skipping) whenever the
# library is missing.
#
# Usage: runtime/scripts/verify-patch-manager.sh

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$project_root"

if ! command -v cmake >/dev/null 2>&1; then
  echo "Missing cmake. Run: sudo runtime/scripts/install-debian-runtime-deps.sh"
  exit 1
fi

build_dir="Build/x64-tests"
cmake -S . -B "$build_dir" >/dev/null
cmake --build "$build_dir" --target shadps4_patch_domain_shared -j"$(nproc)"

host_lib="$build_dir/patch_domain/libbachata_patch_domain.so"
if [[ ! -f "$host_lib" ]]; then
  echo "Host patch-domain library not produced: $host_lib"
  exit 1
fi
host_lib=$(realpath "$host_lib")

if ! command -v java >/dev/null 2>&1; then
  echo "Missing java. Run: sudo runtime/scripts/install-debian-runtime-deps.sh"
  exit 1
fi

android_dir="android/BachataS4"
if [[ -n "${ANDROID_HOME:-}" && -d "$ANDROID_HOME" ]]; then
  :
else
  echo "ANDROID_HOME is not set to an existing directory"
  exit 1
fi

(cd "$android_dir" && ./gradlew :core:data:testDebugUnitTest \
  -Dpatch.domain.so="$host_lib" \
  -Dpatch.domain.require=true)

echo "patch-manager verification passed: host .so built and JNI E2E executed"