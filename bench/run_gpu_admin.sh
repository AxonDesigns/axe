#!/bin/bash
# Runs bench_gpu_admin (docs/decisions.md, "GPU spike") in a headless sway at each refresh rate,
# on the GPU and on lavapipe, and prints the table. Rerun it on every wgpu upgrade.
#
#   bench/run_gpu_admin.sh [--repeats N] [--seconds S] [--out DIR] [CONFIG...]
#
# CONFIG picks from gpu-60, gpu-144 and software-60 (default: all three). Needs sway; it takes
# about 30 minutes, and should have the machine to itself: anything else running is measured too.
#
# Baseline and load runs alternate, so slow drift in the machine lands on both.
#
# wlroots' headless output paces itself with a timer in whole milliseconds, so `144 Hz` is a 6 ms
# timer that measures about 160 Hz: stricter than 144. The report prints what was measured.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BENCH=${BENCH:-$ROOT/build/release/bench/bench_gpu_admin}
REPEATS=3 SECONDS_PER_RUN=60 OUT= ROWS=100
CONFIGS=()
while [ $# -gt 0 ]; do
    case $1 in
    --repeats) REPEATS=$2; shift ;;
    --seconds) SECONDS_PER_RUN=$2; shift ;;
    --out) OUT=$2; shift ;;
    *) CONFIGS+=("$1") ;;
    esac
    shift
done
[ ${#CONFIGS[@]} -gt 0 ] || CONFIGS=(gpu-60 gpu-144 software-60)
[ -n "$OUT" ] || OUT=$(mktemp -d -t gpu-admin.XXXXXX)
mkdir -p "$OUT"
JSON=$OUT/runs.jsonl
# Driver caches stay on, as on a user's machine, in a directory of their own.
export __GL_SHADER_DISK_CACHE_PATH=$OUT/cache MESA_SHADER_CACHE_DIR=$OUT/cache
mkdir -p "$OUT/cache"

SWAY_PID=
stop_sway() {
    [ -n "$SWAY_PID" ] && kill "$SWAY_PID" 2>/dev/null && wait "$SWAY_PID" 2>/dev/null
    SWAY_PID=
}
trap stop_sway EXIT

start_sway() { # WIDTHxHEIGHT HZ
    local dir=$OUT/sway-$2
    mkdir -p "$dir"
    rm -f "$dir/display"
    cat >"$dir/cfg" <<CFG
output HEADLESS-1 mode $1@$2Hz scale 1
default_border none
exec sh -c 'echo \$WAYLAND_DISPLAY > $dir/display'
CFG
    env -u WAYLAND_DISPLAY -u DISPLAY WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 \
        WLR_RENDERER=vulkan sway -c "$dir/cfg" >"$dir/log" 2>&1 &
    SWAY_PID=$!
    for _ in $(seq 100); do [ -s "$dir/display" ] && break; sleep 0.1; done
    [ -s "$dir/display" ] || { echo "sway didn't start: see $dir/log" >&2; exit 1; }
    export WAYLAND_DISPLAY=$(cat "$dir/display")
}

run() { # LABEL MODE ARGS...
    local label=$1 mode=$2
    shift 2
    "$BENCH" --mode "$mode" --seconds "$SECONDS_PER_RUN" --rows=$ROWS --label "$label" --json "$JSON" "$@" \
        2>&1 | grep -v '^\[info\]'
}

for config in "${CONFIGS[@]}"; do
    case $config in
    gpu-60) size=1920x1080 hz=60 gpu=hardware ;;
    gpu-144) size=1920x1080 hz=144 gpu=hardware ;;
    software-60) size=1024x768 hz=60 gpu=software ;;
    *) echo "unknown config $config" >&2; exit 2 ;;
    esac
    start_sway "$size" "$hz"
    export UI_GPU=$gpu
    for _ in $(seq "$REPEATS"); do
        run "$config" baseline
        run "$config" admin
    done
    # What a pipeline costs on the render thread: must fail, or this bench sees nothing.
    run "$config" render
    # The largest shaders anyone should expect: about 2,000 lines, seconds each on NVIDIA.
    run "$config" admin --large-steps 150 --label "$config-stress"
    # The same sources again and again: what caches would hide if compiles weren't all new.
    run "$config" admin --same
    # No GPU calls at all: what's left is CPU contention, not locks.
    run "$config" burn
    stop_sway
done
python3 "$ROOT/bench/gpu_admin_table.py" "$JSON"
echo "raw results: $JSON"
