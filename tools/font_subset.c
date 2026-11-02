// Subsets a font to the code points that appear in a UTF-8 text file, keeping variations.
// Builds tests/fonts/NotoSansSC-Subset.ttf from the full upstream font:
//   cmake --build --preset debug --target font_subset
//   build/debug/tools/font_subset NotoSansSC[wght].ttf tests/fonts/cjk_chars.txt \
//       tests/fonts/NotoSansSC-Subset.ttf
// Developer tool only: it is excluded from the default build.
#include <hb-subset.h>
#include <hb.h>
#include <stdio.h>
#include <stdlib.h>

#include "unicode/utf8.h"

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s FONT CHARS.txt OUT\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[2], "rb");
    if (!f) {
        perror(argv[2]);
        return 1;
    }
    static uint8_t text[1 << 20];
    size_t n = fread(text, 1, sizeof text, f);
    fclose(f);
    if (!ui_utf8_valid(text, n)) {
        fprintf(stderr, "%s is not valid UTF-8\n", argv[2]);
        return 1;
    }
    hb_blob_t *blob = hb_blob_create_from_file_or_fail(argv[1]);
    if (!blob) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    hb_face_t *face = hb_face_create(blob, 0);
    hb_subset_input_t *input = hb_subset_input_create_or_fail();
    // Keep every name record: by default only IDs 0-6 survive, which drops the typographic
    // family (ID 16) of a variable font and renames it after its default instance.
    hb_set_t *names = hb_subset_input_set(input, HB_SUBSET_SETS_NAME_ID);
    hb_set_clear(names);
    hb_set_invert(names);
    hb_set_t *unicodes = hb_subset_input_unicode_set(input);
    unsigned count = 0;
    for (size_t i = 0; i < n;) {
        uint32_t cp = ui_utf8_next(text, &i);
        if (cp >= 0x20) {
            hb_set_add(unicodes, cp);
            count++;
        }
    }
    hb_face_t *sub = hb_subset_or_fail(face, input);
    if (!sub) {
        fprintf(stderr, "subsetting failed\n");
        return 1;
    }
    hb_blob_t *out = hb_face_reference_blob(sub);
    unsigned len;
    const char *data = hb_blob_get_data(out, &len);
    FILE *o = fopen(argv[3], "wb");
    if (!o || fwrite(data, 1, len, o) != len || fclose(o)) {
        perror(argv[3]);
        return 1;
    }
    fprintf(stderr, "%u code points (%u distinct) -> %u bytes\n", count,
            hb_set_get_population(unicodes), len);
    hb_blob_destroy(out);
    hb_face_destroy(sub);
    hb_subset_input_destroy(input);
    hb_face_destroy(face);
    hb_blob_destroy(blob);
    return 0;
}
