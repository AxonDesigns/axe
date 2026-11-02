// Own image codecs (no dependency): QOI encode/decode, used for golden images, and a PNG
// writer with stored (uncompressed) deflate blocks, used for viewable diffs and docs
// screenshots. Images are RGBA8, tightly packed rows.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ui/result.h"

typedef struct ui_rgba_image {
    uint32_t width, height;
    uint8_t *pixels; // width * height * 4 bytes
} ui_rgba_image;

// Encodes to QOI (https://qoiformat.org, 4 channels, sRGB). *out is malloc'd.
ui_result ui_qoi_encode(const ui_rgba_image *img, uint8_t **out, size_t *out_size);
// Decodes a QOI file; img->pixels is malloc'd. UI_ERR_MALFORMED for invalid data.
ui_result ui_qoi_decode(const uint8_t *data, size_t size, ui_rgba_image *img);

// Writes an RGBA PNG (8-bit, no compression).
ui_result ui_png_write(const ui_rgba_image *img, const char *path);

// Whole-file helpers.
ui_result ui_file_read(const char *path, uint8_t **out, size_t *out_size);
ui_result ui_file_write(const char *path, const void *data, size_t size);
