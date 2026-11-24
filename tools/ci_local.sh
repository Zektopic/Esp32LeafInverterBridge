#!/usr/bin/env bash
# Runs every job of .github/workflows/ci.yml locally.
# Heavy steps run in containers limited to 2 CPUs (override with CI_CPUS).
#
#   tools/ci_local.sh            all jobs
#   tools/ci_local.sh host idf   selected jobs (host, idf, ui, arduino)
#
# "ui" needs the QEMU build of the "idf" job; it boots a fresh device in the
# IDF container and runs the Playwright browser test against it.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CPUS="${CI_CPUS:-2}"
IDF_IMAGE="${IDF_IMAGE:-espressif/idf:v6.1.1}"
PLAYWRIGHT_IMAGE="${PLAYWRIGHT_IMAGE:-mcr.microsoft.com/playwright:v1.65.1-noble}"
JOBS=("$@")
[ ${#JOBS[@]} -eq 0 ] && JOBS=(host idf ui arduino)
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

run_host() {
    echo "== host: leaf_can + bridge_settings + firmware-idf helpers (ASan/UBSan)"
    for src in lib/leaf_can lib/bridge_settings firmware-idf/test/host; do
        local dir="$OUT/$(basename "$(dirname "$src")")-$(basename "$src")"
        cmake -S "$ROOT/$src" -B "$dir" -DCMAKE_BUILD_TYPE=Debug \
            -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all" \
            -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined" >/dev/null
        cmake --build "$dir" -j "$CPUS" >/dev/null
        ctest --test-dir "$dir" --output-on-failure
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

run_ui() {
    echo "== ui: browser test of the web UI against a fresh QEMU device"
    # global: the EXIT trap runs after this function returned (set -u)
    UI_CONTAINER="leafbridge-ui-$$"
    local name="$UI_CONTAINER"
    [ -f "$ROOT/firmware-idf/build-qemu/flash_args" ] || { echo "run the idf job first"; return 1; }
    docker run -d --rm --name "$name" --cpus 1 --user "$(id -u):$(id -g)" -e HOME=/tmp \
        -p 127.0.0.1:18080:18080 -v "$ROOT":/project -w /project/firmware-idf "$IDF_IMAGE" \
        python test/qemu_e2e.py --build build-qemu --serve --listen 0.0.0.0 >/dev/null
    trap 'docker rm -f "$UI_CONTAINER" >/dev/null 2>&1 || true; rm -rf "$OUT"' EXIT
    local i
    for i in $(seq 1 90); do
        docker logs "$name" 2>&1 | grep -q SERVING && break
        sleep 2
    done
    docker logs "$name" 2>&1 | grep -q SERVING || { docker logs "$name"; return 1; }
    docker run --rm --cpus 1 --network host --user "$(id -u):$(id -g)" -e HOME=/tmp -e npm_config_cache=/tmp/npm \
        -v "$ROOT":/work -w /work/firmware-idf/test/ui "$PLAYWRIGHT_IMAGE" bash -c '
        npm ci --silent --no-audit --no-fund &&
        node ui_test.mjs --url http://127.0.0.1:18080 --out /work/firmware-idf/build-qemu/ui-shots \
            --firmware /work/firmware-idf/build-qemu/leaf_inverter_bridge.bin'
    docker rm -f "$name" >/dev/null 2>&1 || true
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
