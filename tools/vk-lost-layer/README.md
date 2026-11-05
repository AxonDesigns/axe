# vk-lost-layer

A Vulkan layer that fakes a lost device, so we can see what the stack does when the driver itself
says `VK_ERROR_DEVICE_LOST`. `ui_render_test_lose_device` (tests/test_device_loss.c) can't show
that: it marks the device invalid, but no Vulkan call ever fails. The layer needs no real GPU hang,
so it's safe on a desktop's GPU, and it works on any driver.

After `VK_LOST_AFTER` presents (default 120), the first call of the kind `VK_LOST_SITE` names
returns the error, and then every call that can return it does.

| `VK_LOST_SITE` | Call | What it stands for |
| --- | --- | --- |
| `present` | `vkQueuePresentKHR` | A loss seen when presenting |
| `submit` | `vkQueueSubmit` | A loss seen when submitting |
| `poll` | `vkGetSemaphoreCounterValue` | A loss seen polling for finished work, which wgpu does inside its submit |
| `acquire` | `vkAcquireNextImageKHR` | A loss seen acquiring the next image |

`run.sh` builds it and runs all four, on the GPU and on lavapipe, in a headless sway. Name one to
run only that one:

```sh
tools/vk-lost-layer/run.sh            # hardware and software
tools/vk-lost-layer/run.sh software   # what CI's device-loss job runs
```

Each line says whether the app survived, with one report and a clean exit. CI needs `sway`, which
its job installs. Rerun it locally on every wgpu upgrade, since CI only has lavapipe, and use the
layer for M5h's injected losses.

Three of the four survive only because of
[`third_party/patches/wgpu-native-device-lost.patch`](../../third_party/patches/wgpu-native-device-lost.patch).
Without it they abort inside wgpu-native ([VERSIONS.md](../../third_party/VERSIONS.md), "Patches").

There are no Vulkan headers among the project's dependencies, so the layer declares the few types
it needs, which are stable ABI. `run.sh` compiles it, so CMake doesn't build it.
