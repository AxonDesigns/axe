// Result codes returned by every fallible public function.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ui_result {
    UI_OK = 0,
    UI_ERR_INVALID_ARGUMENT, // a parameter is out of range or inconsistent
    UI_ERR_OUT_OF_MEMORY,
    UI_ERR_UNSUPPORTED,      // the platform or configuration cannot perform the operation
    UI_ERR_PLATFORM,         // the windowing system failed or disconnected
    UI_ERR_GPU,              // no adapter, device lost or a GPU operation failed
    UI_ERR_STALE_HANDLE,     // the handle's generation no longer matches a live object
    UI_ERR_MALFORMED,        // a wire buffer failed validation
    UI_ERR_FULL,             // a bounded queue or buffer has no room; retry later
    UI_ERR_IO,
    UI_ERR_END,              // a reader has no more items (not an error)
    UI_ERR_WRONG_STATE,      // the call is not valid in the object's current state
} ui_result;

// Thread: any. Returns a static string; never NULL.
const char *ui_result_str(ui_result result);

#ifdef __cplusplus
}
#endif
