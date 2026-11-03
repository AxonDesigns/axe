// Bytecode bundles (docs/script.md, "Bundles"): engine bytecode behind a 16-byte header.
//
//   0  u32  magic "AXBC"
//   4  u16  container version (1)
//   6  u16  flags (0)
//   8  u32  engine id: FNV-1a of the engine's name, a space and its version
//  12  u32  bytecode size
//  16  ...  bytecode
//
// Anything else is ES module source. Bytecode is trusted input: the header only catches a
// bundle built for another engine version, not a malicious one.
#include <stdlib.h>
#include <string.h>

#include "script/host/host.h"
#include "wire/wire.h"

uint32_t ui_script_engine_id(void) {
    uint32_t h = 2166136261u;
    const char *parts[] = {ui_js_engine_name(), " ", ui_js_engine_version()};
    for (size_t i = 0; i < UI_ARRAY_LEN(parts); i++)
        for (const char *p = parts[i]; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    return h;
}

ui_result ui_script_bundle_wrap(const uint8_t *bytecode, size_t size, uint8_t **out, size_t *out_size) {
    if (size > UINT32_MAX - UI_SCRIPT_BUNDLE_HEADER) return UI_ERR_INVALID_ARGUMENT;
    uint8_t *b = malloc(UI_SCRIPT_BUNDLE_HEADER + size);
    if (!b) return UI_ERR_OUT_OF_MEMORY;
    uint8_t *p = ui_wire_put_u32(b, UI_SCRIPT_BUNDLE_MAGIC);
    p = ui_wire_put_u16(p, UI_SCRIPT_BUNDLE_VERSION);
    p = ui_wire_put_u16(p, 0);
    p = ui_wire_put_u32(p, ui_script_engine_id());
    p = ui_wire_put_u32(p, (uint32_t)size);
    memcpy(p, bytecode, size);
    *out = b;
    *out_size = UI_SCRIPT_BUNDLE_HEADER + size;
    return UI_OK;
}

ui_result ui_script_bundle_open(const uint8_t *bytes, size_t size, const uint8_t **code, size_t *code_size,
                                bool *is_bytecode, const char **message) {
    *message = NULL;
    if (size < 4 || ui_wire_get_u32(bytes) != UI_SCRIPT_BUNDLE_MAGIC) {
        *code = bytes;
        *code_size = size;
        *is_bytecode = false;
        return UI_OK;
    }
    if (size < UI_SCRIPT_BUNDLE_HEADER || ui_wire_get_u16(bytes + 4) != UI_SCRIPT_BUNDLE_VERSION ||
        ui_wire_get_u32(bytes + 12) != size - UI_SCRIPT_BUNDLE_HEADER) {
        *message = "not a bytecode bundle this host reads (bad header or size)";
        return UI_ERR_MALFORMED;
    }
    if (ui_wire_get_u32(bytes + 8) != ui_script_engine_id()) {
        *message = "bytecode built for another engine or engine version: build the bundle again";
        return UI_ERR_UNSUPPORTED;
    }
    *code = bytes + UI_SCRIPT_BUNDLE_HEADER;
    *code_size = size - UI_SCRIPT_BUNDLE_HEADER;
    *is_bytecode = true;
    return UI_OK;
}
