#!/bin/bash
# Fakes a lost device at each kind of Vulkan call, on the GPU and on lavapipe, in a headless sway
# (README.md here). Runs bench_gpu_admin, which only quits on a frame, so it notices if frames stop.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
BENCH=${BENCH:-$ROOT/build/release/bench/bench_gpu_admin}
WORK=$(mktemp -d -t vk-lost.XXXXXX)
trap 'kill "$SWAY_PID" 2>/dev/null; rm -rf "$WORK"' EXIT

cc -std=gnu2x -shared -fPIC -fvisibility=hidden -O1 -o "$WORK/libVkLayer_axe_device_lost.so" \
    "$HERE/vk_lost_layer.c" || exit 1
cp "$HERE/VkLayer_axe_device_lost.json" "$WORK/"

cat >"$WORK/cfg" <<CFG
output HEADLESS-1 mode 1024x768@60Hz scale 1
exec sh -c 'echo \$WAYLAND_DISPLAY > $WORK/display'
CFG
env -u WAYLAND_DISPLAY -u DISPLAY WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 \
    WLR_RENDERER=vulkan sway -c "$WORK/cfg" >"$WORK/sway.log" 2>&1 &
SWAY_PID=$!
for _ in $(seq 100); do [ -s "$WORK/display" ] && break; sleep 0.1; done
export WAYLAND_DISPLAY=$(cat "$WORK/display")

export VK_LAYER_PATH=$WORK VK_INSTANCE_LAYERS=VK_LAYER_AXE_device_lost VK_LOST_AFTER=120
failed=0
for gpu in hardware software; do
    for site in present submit poll acquire; do
        UI_GPU=$gpu VK_LOST_SITE=$site timeout 25 "$BENCH" --seconds 3 --rows=100 >"$WORK/out" 2>&1
        code=$?
        reports=$(grep -c 'GPU device lost' "$WORK/out")
        if [ "$code" = 0 ] && [ "$reports" = 1 ]; then
            echo "$gpu $site: survived"
        else
            echo "$gpu $site: exit $code, $reports report(s), $(grep -m1 'Error in\|panicked at' "$WORK/out")"
            failed=1
        fi
    done
done
exit $failed
