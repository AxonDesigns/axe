// axe-jsc: compiles an ES module bundle (esbuild's output) to a bytecode bundle (.axbc) at
// build time (docs/script.md, "Bundles"). Run by the build (axe_ts_app), not by apps.
//
//   axe-jsc input.js output.axbc [--name NAME]
//
// NAME is the module's name in stack traces (default: the input's file name).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "script/engine.h"
#include "script/host/host.h"

static char *slurp(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *data = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long n = ftell(f);
        if (n >= 0 && fseek(f, 0, SEEK_SET) == 0 && (data = malloc((size_t)n + 1))) {
            if (fread(data, 1, (size_t)n, f) != (size_t)n) {
                free(data);
                data = NULL;
            }
            *size = (size_t)n;
        }
    }
    fclose(f);
    return data;
}

int main(int argc, char **argv) {
    const char *in = NULL, *out = NULL, *name = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!in) in = argv[i];
        else if (!out) out = argv[i];
        else in = NULL, i = argc;
    }
    if (!in || !out) {
        fprintf(stderr, "usage: %s input.js output.axbc [--name NAME]\n", argv[0]);
        return 2;
    }
    if (!name) {
        const char *slash = strrchr(in, '/');
        name = slash ? slash + 1 : in;
    }
    size_t size;
    char *src = slurp(in, &size);
    if (!src) {
        fprintf(stderr, "axe-jsc: can't read %s\n", in);
        return 1;
    }
    ui_js *js;
    if (ui_js_create(&(ui_js_desc){0}, &js) != UI_OK) return 1;
    uint8_t *code = NULL, *bundle = NULL;
    size_t code_size, bundle_size;
    int status = 0;
    if (!ui_js_compile_module(js, name, src, size, &code, &code_size)) {
        ui_js_exception e;
        if (ui_js_take_exception(js, &e, NULL)) {
            fprintf(stderr, "%s: %s%s%s\n%s\n", in, e.name, e.name[0] ? ": " : "", e.message, e.stack);
            ui_js_exception_free(&e);
        }
        status = 1;
    } else if (ui_script_bundle_wrap(code, code_size, &bundle, &bundle_size) != UI_OK) {
        status = 1;
    } else {
        FILE *f = fopen(out, "wb");
        if (!f || fwrite(bundle, 1, bundle_size, f) != bundle_size) {
            fprintf(stderr, "axe-jsc: can't write %s\n", out);
            status = 1;
        }
        if (f && fclose(f) != 0) status = 1;
    }
    free(code);
    free(bundle);
    free(src);
    ui_js_destroy(js);
    return status;
}
