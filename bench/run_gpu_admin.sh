#!/bin/bash
# Runs bench_gpu_admin (docs/decisions.md, "GPU spike") in a headless sway at each refresh rate,
# on the GPU and on lavapipe, and prints the table. Rerun it on every wgpu upgrade.
#
#   bench/run_gpu_admin.sh [--repeats N] [--seconds S] [--out DIR] [--controls "NAME..."] [CONFIG...]
#
# CONFIG picks from gpu-60, gpu-144 and software-60 (default: all three). --controls picks the
# runs after the baseline and admin pairs, from render, stress, same, burn, fallback (default: all
# of these), nice and baseline, and can name one more than once. Runs add to --out's results. Needs sway; it takes
# about 30 minutes, and should have the machine to itself: anything else running is measured too.
#
# Baseline and load runs alternate, so slow drift in the machine lands on both.
#
# It's safe to leave running: results and driver caches go to disk, each run is capped at
# MEM_CAP (default 3G) with no swap, so the kernel kills the run and not the desktop, and it all
# stops if the system falls below MIN_AVAILABLE_MB (default 3000), which also catches what the cap
# doesn't see, such as tmpfs or the compositor. mem.log has each run's memory, once a second.
# An early run filled this machine's RAM and took the desktop with it, so keep these.
#
# A software client needs sway's pixman renderer. With wlroots' Vulkan renderer, sway keeps every
# wl_shm frame it's sent: about 230 MB/s at 1024x768, outside any cap on the bench, which is what
# filled the RAM. A GPU client needs the Vulkan renderer, for its dmabufs.
#
# wlroots' headless output paces itself with a timer in whole milliseconds, so `144 Hz` is a 6 ms
# timer that measures about 160 Hz: stricter than 144. The report prints what was measured.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BENCH=${BENCH:-$ROOT/build/release/bench/bench_gpu_admin}
REPEATS=3 SECONDS_PER_RUN=60 OUT= ROWS=100
MEM_CAP=${MEM_CAP:-3G} MIN_AVAILABLE_MB=${MIN_AVAILABLE_MB:-3000}
CONTROLS="render stress same burn fallback"
CONFIGS=()
while [ $# -gt 0 ]; do
    case $1 in
    --repeats) REPEATS=$2; shift ;;
    --seconds) SECONDS_PER_RUN=$2; shift ;;
    --out) OUT=$2; shift ;;
    --controls) CONTROLS=$2; shift ;;
    *) CONFIGS+=("$1") ;;
    esac
    shift
done
[ ${#CONFIGS[@]} -gt 0 ] || CONFIGS=(gpu-60 gpu-144 software-60)
[ -n "$OUT" ] || OUT=$ROOT/build/gpu-admin/$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT"
JSON=$OUT/runs.jsonl
# Driver caches stay on, as on a user's machine, in a directory of their own.
# Never under /tmp: that's RAM here.
export __GL_SHADER_DISK_CACHE_PATH=$OUT/cache MESA_SHADER_CACHE_DIR=$OUT/cache
mkdir -p "$OUT/cache"

SWAY_PID=
stop_sway() {
    [ -n "$SWAY_PID" ] && kill "$SWAY_PID" 2>/dev/null && wait "$SWAY_PID" 2>/dev/null
    SWAY_PID=
}
trap stop_sway EXIT

start_sway() { # WIDTHxHEIGHT HZ RENDERER
    local dir=$OUT/sway-$2
    mkdir -p "$dir"
    rm -f "$dir/display"
    cat >"$dir/cfg" <<CFG
output HEADLESS-1 mode $1@$2Hz scale 1
default_border none
exec sh -c 'echo \$WAYLAND_DISPLAY > $dir/display'
CFG
    env -u WAYLAND_DISPLAY -u DISPLAY WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 \
        WLR_RENDERER=$3 sway -c "$dir/cfg" >"$dir/log" 2>&1 &
    SWAY_PID=$!
    for _ in $(seq 100); do [ -s "$dir/display" ] && break; sleep 0.1; done
    [ -s "$dir/display" ] || { echo "sway didn't start: see $dir/log" >&2; exit 1; }
    export WAYLAND_DISPLAY=$(cat "$dir/display")
}

available_mb() { awk '/^MemAvailable/ { print int($2 / 1024) }' /proc/meminfo; }

RUNS=0
run() { # LABEL MODE ARGS...
    local label=$1 mode=$2 unit=gpu-admin-$$-$((++RUNS))
    shift 2
    echo "# $label $mode $* (available $(available_mb) MB)" >>"$OUT/mem.log"
    systemd-run --user --scope --quiet --collect --unit="$unit" -p MemoryMax="$MEM_CAP" -p MemorySwapMax=0 \
        "$BENCH" --mode "$mode" --seconds "$SECONDS_PER_RUN" --rows=$ROWS --label "$label" --json "$JSON" "$@" \
        >"$OUT/run.out" 2>&1 &
    local pid=$! begin=$SECONDS peak=0
    while kill -0 "$pid" 2>/dev/null; do
        sleep 1
        local group current available
        group=$(systemctl --user show -p ControlGroup --value "$unit.scope" 2>/dev/null)
        current=$(cat "/sys/fs/cgroup$group/memory.current" 2>/dev/null || echo 0)
        available=$(available_mb)
        [ "$current" -gt "$peak" ] && peak=$current
        echo "$((SECONDS - begin)) $((current / 1048576)) $available" >>"$OUT/mem.log"
        if [ "$available" -lt "$MIN_AVAILABLE_MB" ]; then
            echo "only $available MB available: stopping everything" >&2
            systemctl --user kill -s KILL "$unit.scope" 2>/dev/null
            exit 3
        fi
    done
    wait "$pid"
    local code=$?
    grep -v '^\[info\]' "$OUT/run.out"
    echo "  memory peak $((peak / 1048576)) MB, exit $code"
    [ "$code" = 137 ] && echo "  KILLED at the $MEM_CAP cap"
}

for config in "${CONFIGS[@]}"; do
    case $config in
    gpu-60) size=1920x1080 hz=60 gpu=hardware renderer=vulkan budget=8 ;;
    gpu-144) size=1920x1080 hz=144 gpu=hardware renderer=vulkan budget=3 ;;
    software-60) size=1024x768 hz=60 gpu=software renderer=pixman budget=8 ;;
    *) echo "unknown config $config" >&2; exit 2 ;;
    esac
    start_sway "$size" "$hz" "$renderer"
    export UI_GPU=$gpu
    for _ in $(seq "$REPEATS"); do
        run "$config" baseline
        run "$config" admin
    done
    for control in $CONTROLS; do
        case $control in
        # What a pipeline costs on the render thread: must fail, or this bench sees nothing.
        render) run "$config" render ;;
        baseline) run "$config" baseline ;;
        # The second thread at the lowest priority: scheduling, if it helps.
        nice) run "$config" admin --nice 19 --label "$config-nice" ;;
        # The largest shaders anyone should expect: about 2,000 lines, seconds each on NVIDIA.
        stress) run "$config" admin --large-steps 150 --label "$config-stress" ;;
        # The same sources again and again: what caches would hide if compiles weren't all new.
        same) run "$config" admin --same ;;
        # No GPU calls at all: what's left is CPU contention, not locks.
        burn) run "$config" burn ;;
        # About half a refresh after present for pipelines, as the design's fallback proposes.
        fallback) run "$config" fallback --budget-ms "$budget" ;;
        esac
    done
    stop_sway
done
python3 "$ROOT/bench/gpu_admin_table.py" "$JSON"
echo "raw results: $JSON"
