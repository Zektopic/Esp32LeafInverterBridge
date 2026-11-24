#!/usr/bin/env bash
# Runs every job of .github/workflows/ci.yml locally.
# Heavy steps run in containers limited to 2 CPUs (override with CI_CPUS).
#
#   tools/ci_local.sh            all jobs
#   tools/ci_local.sh host idf   selected jobs (host, idf, arduino)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CPUS="${CI_CPUS:-2}"
IDF_IMAGE="${IDF_IMAGE:-espressif/idf:v6.1.1}"
JOBS=("$@")
[ ${#JOBS[@]} -eq 0 ] && JOBS=(host idf arduino)
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

run_host() {
    echo "== host: leaf_can + bridge_settings (ASan/UBSan)"
    for lib in leaf_can bridge_settings; do
        cmake -S "$ROOT/lib/$lib" -B "$OUT/$lib" -DCMAKE_BUILD_TYPE=Debug \
            -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all" \
            -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined" >/dev/null
        cmake --build "$OUT/$lib" -j "$CPUS" >/dev/null
        ctest --test-dir "$OUT/$lib" --output-on-failure
    done
}

run_idf() {
    echo "== idf: hardware build, QEMU builds, QEMU end-to-end"
    docker run --rm --cpus "$CPUS" --user "$(id -u):$(id -g)" -e HOME=/tmp \
        -v "$ROOT":/project -w /project/firmware-idf "$IDF_IMAGE" bash -c '
        set -e
        idf.py build
        idf.py -B build-qemu -D SDKCONFIG=build-qemu/sdkconfig \
            -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.qemu" build
        printf "CONFIG_BRIDGE_SELFTEST_REQUIRE_CAN=y\n" > build-qemu-rb.overlay
        idf.py -B build-qemu-rb -D SDKCONFIG=build-qemu-rb/sdkconfig \
            -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.qemu;build-qemu-rb.overlay" build
        rm -f build-qemu-rb.overlay
        python test/qemu_e2e.py --build build-qemu --rollback-image build-qemu-rb/leaf_inverter_bridge.bin'
}

run_arduino() {
    echo "== arduino: esp32dev, esp32dev-arduino2, esp32dev + SERIAL_DEBUG"
    local pio=(taskset -c "0-$((CPUS - 1))" nice -n 10 pio)
    (cd "$ROOT/firmware" &&
        "${pio[@]}" run -e esp32dev &&
        "${pio[@]}" run -e esp32dev-arduino2 &&
        PLATFORMIO_BUILD_FLAGS="-DSERIAL_DEBUG" PLATFORMIO_BUILD_DIR="$OUT/pio-debug" "${pio[@]}" run -e esp32dev)
}

for job in "${JOBS[@]}"; do
    "run_$job"
done
echo "== all requested jobs passed: ${JOBS[*]}"
