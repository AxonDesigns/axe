#include "unicode/script.h"

#include <stdbool.h>
#include <string.h>

#include "unicode/ucd.h"

#define MAX_CANDIDATES 32 // larger than any Script_Extensions set in UCD 18.0
#define BRACKET_DEPTH 64

// The scripts the current run can still be: `any` until a real script is seen.
typedef struct candidates {
    bool any;
    uint32_t count;
    uint8_t s[MAX_CANDIDATES];
} candidates;

static bool is_any(const uint8_t *set, uint32_t count) {
    return count == 1 && (set[0] == UI_SCRIPT_ZYYY || set[0] == UI_SCRIPT_ZINH);
}

// Intersects c with set; returns false (leaving c unchanged) if the result would be empty.
static bool intersect(candidates *c, const uint8_t *set, uint32_t count) {
    if (c->any) {
        c->any = false;
        c->count = count < MAX_CANDIDATES ? count : MAX_CANDIDATES;
        memcpy(c->s, set, c->count);
        return true;
    }
    uint8_t keep[MAX_CANDIDATES];
    uint32_t n = 0;
    for (uint32_t i = 0; i < c->count; i++) {
        for (uint32_t j = 0; j < count; j++) {
            if (c->s[i] == set[j]) {
                keep[n++] = c->s[i];
                break;
            }
        }
    }
    if (!n) return false;
    memcpy(c->s, keep, n);
    c->count = n;
    return true;
}

typedef struct open_bracket {
    uint32_t close_cp;
    uint32_t run;      // run id the bracket was opened in
    uint8_t script;    // resolved script, or UI_SCRIPT_ZYYY while its run is unresolved
} open_bracket;

void ui_script_runs(const uint32_t *cps, size_t n, uint8_t *out) {
    candidates cur = {.any = true};
    size_t run_start = 0;
    uint32_t run_id = 0;
    uint8_t prev_script = UI_SCRIPT_ZYYY; // resolved script of the previous run
    open_bracket stack[BRACKET_DEPTH];
    uint32_t depth = 0;

    for (size_t i = 0; i <= n; i++) {
        uint32_t count = 0;
        const uint8_t *set = NULL;
        uint8_t single;
        bool closes = false;
        if (i < n) {
            const ui_ucd_record *r = ui_ucd(cps[i]);
            set = ui_ucd_script_extensions(r, &count);
            uint32_t pair;
            bool open;
            if (is_any(set, count) && ui_ucd_paired_bracket(cps[i], &pair, &open)) {
                if (open) {
                    if (depth == BRACKET_DEPTH) { // drop the oldest, as deep nesting is rare
                        memmove(stack, stack + 1, sizeof stack - sizeof *stack);
                        depth--;
                    }
                    stack[depth++] = (open_bracket){pair, run_id,
                                                    cur.any ? UI_SCRIPT_ZYYY : cur.s[0]};
                } else {
                    for (uint32_t j = depth; j-- > 0;) {
                        if (stack[j].close_cp == cps[i]) {
                            depth = j;
                            if (stack[j].script != UI_SCRIPT_ZYYY) {
                                single = stack[j].script;
                                set = &single;
                                count = 1;
                                closes = true;
                            }
                            break;
                        }
                    }
                }
            }
        }
        bool joins = i < n && (is_any(set, count) || intersect(&cur, set, count));
        if (joins) {
            if (!cur.any && !closes) {
                // Brackets opened in this run before it resolved now know their script.
                for (uint32_t j = depth; j-- > 0 && stack[j].run == run_id;) {
                    if (stack[j].script == UI_SCRIPT_ZYYY) stack[j].script = cur.s[0];
                }
            }
            continue;
        }
        // End the run [run_start, i): prefer the previous run's script among candidates.
        uint8_t script = UI_SCRIPT_ZYYY;
        if (!cur.any) {
            script = cur.s[0];
            for (uint32_t j = 0; j < cur.count; j++) {
                if (cur.s[j] == prev_script) script = prev_script;
            }
        }
        memset(out + run_start, script, i - run_start);
        prev_script = script;
        run_start = i;
        run_id++;
        cur = (candidates){.any = true};
        if (i < n) intersect(&cur, set, count);
    }
}
