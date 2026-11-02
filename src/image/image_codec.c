#include "image/image_codec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- Files -------------------------------------------------------------------------------

ui_result ui_file_read(const char *path, uint8_t **out, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return UI_ERR_IO;
    uint8_t *data = NULL;
    size_t size = 0, cap = 0;
    for (;;) {
        if (size == cap) {
            cap = cap ? cap * 2 : 1 << 16;
            uint8_t *grown = realloc(data, cap);
            if (!grown) {
                free(data);
                fclose(f);
                return UI_ERR_OUT_OF_MEMORY;
            }
            data = grown;
        }
        size_t n = fread(data + size, 1, cap - size, f);
        size += n;
        if (n == 0) break;
    }
    bool err = ferror(f);
    fclose(f);
    if (err) {
        free(data);
        return UI_ERR_IO;
    }
    *out = data;
    *out_size = size;
    return UI_OK;
}

ui_result ui_file_write(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) return UI_ERR_IO;
    bool ok = fwrite(data, 1, size, f) == size;
    ok = (fclose(f) == 0) && ok;
    return ok ? UI_OK : UI_ERR_IO;
}

// ---- QOI ---------------------------------------------------------------------------------

#define QOI_OP_INDEX 0x00
#define QOI_OP_DIFF 0x40
#define QOI_OP_LUMA 0x80
#define QOI_OP_RUN 0xC0
#define QOI_OP_RGB 0xFE
#define QOI_OP_RGBA 0xFF
#define QOI_MASK 0xC0
#define QOI_HEADER 14
#define QOI_PADDING 8
#define QOI_MAX_PIXELS 400000000u

static uint32_t qoi_hash(const uint8_t *px) {
    return (px[0] * 3u + px[1] * 5u + px[2] * 7u + px[3] * 11u) % 64u;
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get_be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

ui_result ui_qoi_encode(const ui_rgba_image *img, uint8_t **out, size_t *out_size) {
    size_t pixels = (size_t)img->width * img->height;
    if (!img->width || !img->height || pixels > QOI_MAX_PIXELS) return UI_ERR_INVALID_ARGUMENT;
    uint8_t *buf = malloc(QOI_HEADER + pixels * 5 + QOI_PADDING);
    if (!buf) return UI_ERR_OUT_OF_MEMORY;
    memcpy(buf, "qoif", 4);
    put_be32(buf + 4, img->width);
    put_be32(buf + 8, img->height);
    buf[12] = 4; // RGBA
    buf[13] = 0; // sRGB with linear alpha
    size_t o = QOI_HEADER;
    uint8_t index[64][4] = {{0}};
    uint8_t prev[4] = {0, 0, 0, 255};
    uint32_t run = 0;
    for (size_t i = 0; i < pixels; i++) {
        const uint8_t *px = img->pixels + i * 4;
        if (!memcmp(px, prev, 4)) {
            run++;
            if (run == 62 || i + 1 == pixels) {
                buf[o++] = (uint8_t)(QOI_OP_RUN | (run - 1));
                run = 0;
            }
            continue;
        }
        if (run) {
            buf[o++] = (uint8_t)(QOI_OP_RUN | (run - 1));
            run = 0;
        }
        uint32_t h = qoi_hash(px);
        if (!memcmp(index[h], px, 4)) {
            buf[o++] = (uint8_t)(QOI_OP_INDEX | h);
        } else {
            memcpy(index[h], px, 4);
            if (px[3] == prev[3]) {
                int vr = (int8_t)(px[0] - prev[0]), vg = (int8_t)(px[1] - prev[1]),
                    vb = (int8_t)(px[2] - prev[2]);
                int vg_r = vr - vg, vg_b = vb - vg;
                if (vr > -3 && vr < 2 && vg > -3 && vg < 2 && vb > -3 && vb < 2) {
                    buf[o++] = (uint8_t)(QOI_OP_DIFF | (vr + 2) << 4 | (vg + 2) << 2 | (vb + 2));
                } else if (vg_r > -9 && vg_r < 8 && vg > -33 && vg < 32 && vg_b > -9 && vg_b < 8) {
                    buf[o++] = (uint8_t)(QOI_OP_LUMA | (vg + 32));
                    buf[o++] = (uint8_t)((vg_r + 8) << 4 | (vg_b + 8));
                } else {
                    buf[o++] = QOI_OP_RGB;
                    memcpy(buf + o, px, 3);
                    o += 3;
                }
            } else {
                buf[o++] = QOI_OP_RGBA;
                memcpy(buf + o, px, 4);
                o += 4;
            }
        }
        memcpy(prev, px, 4);
    }
    memset(buf + o, 0, 7);
    buf[o + 7] = 1;
    o += QOI_PADDING;
    *out = buf;
    *out_size = o;
    return UI_OK;
}

ui_result ui_qoi_decode(const uint8_t *data, size_t size, ui_rgba_image *img) {
    if (size < QOI_HEADER + QOI_PADDING || memcmp(data, "qoif", 4)) return UI_ERR_MALFORMED;
    uint32_t w = get_be32(data + 4), h = get_be32(data + 8);
    size_t pixels = (size_t)w * h;
    if (!w || !h || pixels > QOI_MAX_PIXELS || data[12] < 3 || data[12] > 4 || data[13] > 1)
        return UI_ERR_MALFORMED;
    uint8_t *out = malloc(pixels * 4);
    if (!out) return UI_ERR_OUT_OF_MEMORY;
    uint8_t index[64][4] = {{0}};
    uint8_t px[4] = {0, 0, 0, 255};
    size_t p = QOI_HEADER, end = size - QOI_PADDING;
    uint32_t run = 0;
    for (size_t i = 0; i < pixels; i++) {
        if (run) {
            run--;
        } else {
            if (p >= end) {
                free(out);
                return UI_ERR_MALFORMED;
            }
            uint8_t b = data[p++];
            if (b == QOI_OP_RGB || b == QOI_OP_RGBA) {
                size_t n = b == QOI_OP_RGB ? 3 : 4;
                if (p + n > end) {
                    free(out);
                    return UI_ERR_MALFORMED;
                }
                memcpy(px, data + p, n);
                p += n;
            } else if ((b & QOI_MASK) == QOI_OP_INDEX) {
                memcpy(px, index[b], 4);
            } else if ((b & QOI_MASK) == QOI_OP_DIFF) {
                px[0] = (uint8_t)(px[0] + ((b >> 4) & 3) - 2);
                px[1] = (uint8_t)(px[1] + ((b >> 2) & 3) - 2);
                px[2] = (uint8_t)(px[2] + (b & 3) - 2);
            } else if ((b & QOI_MASK) == QOI_OP_LUMA) {
                if (p >= end) {
                    free(out);
                    return UI_ERR_MALFORMED;
                }
                uint8_t b2 = data[p++];
                int vg = (b & 0x3F) - 32;
                px[0] = (uint8_t)(px[0] + vg - 8 + ((b2 >> 4) & 0x0F));
                px[1] = (uint8_t)(px[1] + vg);
                px[2] = (uint8_t)(px[2] + vg - 8 + (b2 & 0x0F));
            } else {
                run = b & 0x3F;
            }
            memcpy(index[qoi_hash(px)], px, 4);
        }
        memcpy(out + i * 4, px, 4);
    }
    img->width = w;
    img->height = h;
    img->pixels = out;
    return UI_OK;
}

// ---- PNG ---------------------------------------------------------------------------------

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n) {
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static bool write_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
    uint8_t head[8];
    put_be32(head, len);
    memcpy(head + 4, type, 4);
    uint32_t crc = crc32_update(0, head + 4, 4);
    crc = crc32_update(crc, data, len);
    uint8_t tail[4];
    put_be32(tail, crc);
    return fwrite(head, 1, 8, f) == 8 && (!len || fwrite(data, 1, len, f) == len) &&
           fwrite(tail, 1, 4, f) == 4;
}

ui_result ui_png_write(const ui_rgba_image *img, const char *path) {
    size_t row = (size_t)img->width * 4 + 1; // filter byte + pixels
    size_t raw = row * img->height;
    size_t blocks = raw / 65535 + 1;
    size_t zlen = 2 + raw + blocks * 5 + 4;
    if (!img->width || !img->height || zlen > UINT32_MAX) return UI_ERR_INVALID_ARGUMENT;
    uint8_t *z = malloc(zlen);
    uint8_t *scan = malloc(raw);
    if (!z || !scan) {
        free(z);
        free(scan);
        return UI_ERR_OUT_OF_MEMORY;
    }
    for (uint32_t y = 0; y < img->height; y++) {
        scan[y * row] = 0; // no filter
        memcpy(scan + y * row + 1, img->pixels + (size_t)y * img->width * 4, row - 1);
    }
    // zlib: header, stored deflate blocks, Adler-32.
    size_t o = 0;
    z[o++] = 0x78;
    z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw || i == 0;) {
        size_t n = raw - i < 65535 ? raw - i : 65535;
        z[o++] = i + n == raw ? 1 : 0;
        z[o++] = (uint8_t)n;
        z[o++] = (uint8_t)(n >> 8);
        z[o++] = (uint8_t)~n;
        z[o++] = (uint8_t)(~n >> 8);
        memcpy(z + o, scan + i, n);
        for (size_t k = 0; k < n; k++) {
            a = (a + scan[i + k]) % 65521;
            b = (b + a) % 65521;
        }
        o += n;
        i += n;
        if (!n) break;
    }
    put_be32(z + o, b << 16 | a);
    o += 4;
    free(scan);

    uint8_t ihdr[13];
    put_be32(ihdr, img->width);
    put_be32(ihdr + 4, img->height);
    ihdr[8] = 8;  // bit depth
    ihdr[9] = 6;  // RGBA
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    FILE *f = fopen(path, "wb");
    if (!f) {
        free(z);
        return UI_ERR_IO;
    }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    bool ok = fwrite(sig, 1, 8, f) == 8 && write_chunk(f, "IHDR", ihdr, 13) &&
              write_chunk(f, "IDAT", z, (uint32_t)o) && write_chunk(f, "IEND", NULL, 0);
    ok = (fclose(f) == 0) && ok;
    free(z);
    return ok ? UI_OK : UI_ERR_IO;
}
