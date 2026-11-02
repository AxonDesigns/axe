#include "platform/platform.h"

ui_result ui_platform_wayland_create(const ui_logger *log, ui_platform_listener listener,
                                     ui_platform **out) {
    (void)log;
    (void)listener;
    (void)out;
    return UI_ERR_UNSUPPORTED;
}
