#include "text/fontconfig/fc.h"

#include <fontconfig/fontconfig.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

struct ui_fc {
    FcConfig *config;
};

ui_result ui_fc_create(bool system, const char *const *extra_dirs, uint32_t dir_count,
                       ui_fc **out) {
    ui_fc *fc = calloc(1, sizeof *fc);
    if (!fc) return UI_ERR_OUT_OF_MEMORY;
    // FcInitLoadConfigAndFonts reads the system configuration; FcConfigCreate starts empty.
    fc->config = system ? FcInitLoadConfigAndFonts() : FcConfigCreate();
    if (!fc->config) {
        free(fc);
        return UI_ERR_PLATFORM;
    }
    // An empty configuration has no cache directory; give it the user's standard one so
    // Fontconfig can cache scans instead of complaining.
    if (!system) {
        static const char cache[] =
            "<?xml version=\"1.0\"?><fontconfig><cachedir prefix=\"xdg\">fontconfig</cachedir>"
            "</fontconfig>";
        FcConfigParseAndLoadFromMemory(fc->config, (const FcChar8 *)cache, FcFalse);
    }
    for (uint32_t i = 0; i < dir_count; i++) {
        if (!FcConfigAppFontAddDir(fc->config, (const FcChar8 *)extra_dirs[i])) {
            FcConfigDestroy(fc->config);
            free(fc);
            return UI_ERR_IO;
        }
    }
    *out = fc;
    return UI_OK;
}

void ui_fc_destroy(ui_fc *fc) {
    if (!fc) return;
    FcConfigDestroy(fc->config);
    free(fc);
}

static bool is_generic(const char *family) {
    return !strcasecmp(family, "sans-serif") || !strcasecmp(family, "serif") ||
           !strcasecmp(family, "monospace") || !strcasecmp(family, "system-ui");
}

static FcPattern *base_pattern(uint16_t weight, bool italic) {
    FcPattern *p = FcPatternCreate();
    if (!p) return NULL;
    FcPatternAddDouble(p, FC_WEIGHT, FcWeightFromOpenTypeDouble(weight));
    FcPatternAddInteger(p, FC_SLANT, italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
    FcPatternAddBool(p, FC_SCALABLE, FcTrue);
    return p;
}

static bool extract(FcPattern *font, ui_fc_font *out) {
    FcChar8 *file;
    int index = 0;
    if (FcPatternGetString(font, FC_FILE, 0, &file) != FcResultMatch) return false;
    FcPatternGetInteger(font, FC_INDEX, 0, &index);
    size_t len = strlen((const char *)file);
    if (len >= sizeof out->path) return false;
    memcpy(out->path, file, len + 1);
    // Variable fonts report named instances as index | instance << 16; we set axes
    // ourselves, so only the face index matters.
    out->index = (uint32_t)index & 0xFFFFu;
    return true;
}

bool ui_fc_match(ui_fc *fc, const char *family, uint16_t weight, bool italic, ui_fc_font *out) {
    FcPattern *p = base_pattern(weight, italic);
    if (!p) return false;
    const char *query = !strcasecmp(family, "system-ui") ? "sans-serif" : family;
    FcPatternAddString(p, FC_FAMILY, (const FcChar8 *)query);
    FcConfigSubstitute(fc->config, p, FcMatchPattern);
    FcDefaultSubstitute(p);
    FcResult result;
    FcPattern *font = FcFontMatch(fc->config, p, &result);
    FcPatternDestroy(p);
    if (!font) return false;
    bool ok = true;
    if (!is_generic(family)) {
        // FcFontMatch always returns something; accept it only if the family matches.
        ok = false;
        FcChar8 *name;
        for (int i = 0; FcPatternGetString(font, FC_FAMILY, i, &name) == FcResultMatch; i++) {
            if (!strcasecmp((const char *)name, family)) {
                ok = true;
                break;
            }
        }
    }
    ok = ok && extract(font, out);
    FcPatternDestroy(font);
    return ok;
}

bool ui_fc_fallback(ui_fc *fc, const uint32_t *cps, uint32_t count, uint16_t weight,
                    bool italic, ui_fc_font *out) {
    FcPattern *p = base_pattern(weight, italic);
    FcCharSet *cs = FcCharSetCreate();
    if (!p || !cs) {
        if (p) FcPatternDestroy(p);
        if (cs) FcCharSetDestroy(cs);
        return false;
    }
    for (uint32_t i = 0; i < count; i++) FcCharSetAddChar(cs, cps[i]);
    FcPatternAddString(p, FC_FAMILY, (const FcChar8 *)"sans-serif");
    FcPatternAddCharSet(p, FC_CHARSET, cs);
    FcConfigSubstitute(fc->config, p, FcMatchPattern);
    FcDefaultSubstitute(p);
    FcResult result;
    FcFontSet *set = FcFontSort(fc->config, p, FcFalse, NULL, &result);
    bool ok = false;
    for (int i = 0; set && i < set->nfont && !ok; i++) {
        FcCharSet *have;
        if (FcPatternGetCharSet(set->fonts[i], FC_CHARSET, 0, &have) != FcResultMatch) continue;
        if (FcCharSetIsSubset(cs, have)) ok = extract(set->fonts[i], out);
    }
    if (set) FcFontSetDestroy(set);
    FcCharSetDestroy(cs);
    FcPatternDestroy(p);
    return ok;
}
