// Default line breaking, UAX #14 revision 57 (Unicode 18.0), rules LB1-LB31 without
// tailoring.
//
// The text is first grouped into units: LB9 makes "X (CM | ZWJ)*" behave as X, so each unit
// is a base character plus the combining marks it absorbs, and no break falls inside one.
// Rules are then evaluated in order between adjacent units; the first that matches decides.
// Rules that look past their neighbours (SP*, LB15a/b, LB19a, LB20a, LB25, LB28a) index
// the unit array directly.
#include "unicode/segment.h"

#include <stdlib.h>

#include "runtime/common.h"
#include "unicode/ucd.h"

typedef struct ui_lb_unit {
    uint32_t start;    // index of the unit's first code point
    uint32_t base;     // its base code point (U+0041 for a CM resolved by LB10)
    uint8_t cls;       // resolved class (LB1, LB10)
    uint8_t gc;        // general category of the base
    uint8_t flags;     // UCD flags of the base (East Asian, Extended_Pictographic)
    bool ends_zwj;     // last code point is ZWJ (LB8a)
} ui_lb_unit;

void ui_line_breaker_free(ui_line_breaker *lb) {
    free(lb->units);
    lb->units = NULL;
    lb->capacity = 0;
}

// LB1 for classes that need no context.
static uint8_t resolve_class(const ui_ucd_record *r) {
    switch (r->lb) {
    case UI_LB_AI:
    case UI_LB_SG:
    case UI_LB_XX: return UI_LB_AL;
    case UI_LB_SA: return r->gc == UI_GC_MN || r->gc == UI_GC_MC ? UI_LB_CM : UI_LB_AL;
    case UI_LB_CJ: return UI_LB_NS;
    default: return r->lb;
    }
}

// Classes that cannot be the X of LB9.
static bool lb9_excluded(uint8_t c) {
    return c == UI_LB_BK || c == UI_LB_CR || c == UI_LB_LF || c == UI_LB_NL || c == UI_LB_SP ||
           c == UI_LB_ZW;
}

#define IN2(c, a, b) ((c) == (a) || (c) == (b))
#define IN3(c, a, b, d) ((c) == (a) || (c) == (b) || (c) == (d))
#define NONE_CLASS 0xFF // sot / eot in lookups

static bool east_asian(const ui_lb_unit *u) { return u->flags & UI_UCD_EAST_ASIAN; }

static bool is_hard(uint8_t c) { return c == UI_LB_BK || c == UI_LB_CR || c == UI_LB_LF || c == UI_LB_NL; }

// (AK | dotted circle | AS), for LB28a.
static bool aksara_base(const ui_lb_unit *u) {
    return u->cls == UI_LB_AK || u->cls == UI_LB_AS || u->base == 0x25CC;
}
static bool aksara_or_circle(const ui_lb_unit *u) {
    return u->cls == UI_LB_AK || u->base == 0x25CC;
}

ui_result ui_line_breaks(ui_line_breaker *lb, const uint32_t *cps, size_t n, uint8_t *out) {
    if (n == 0) {
        out[0] = UI_LINE_BREAK_MANDATORY;
        return UI_OK;
    }
    if (n >= UINT32_MAX) return UI_ERR_INVALID_ARGUMENT;
    if (ui_grow((void **)&lb->units, &lb->capacity, (uint32_t)n, sizeof *lb->units) != UI_OK)
        return UI_ERR_OUT_OF_MEMORY;
    ui_lb_unit *u = lb->units;

    // Build units (LB9, LB10). Positions inside a unit never break.
    uint32_t count = 0;
    for (uint32_t i = 0; i < n; i++) {
        const ui_ucd_record *r = ui_ucd(cps[i]);
        uint8_t cls = resolve_class(r);
        out[i] = UI_LINE_BREAK_NONE;
        bool combining = cls == UI_LB_CM || cls == UI_LB_ZWJ;
        if (combining && count && !lb9_excluded(u[count - 1].cls)) {
            u[count - 1].ends_zwj = cls == UI_LB_ZWJ;
            continue;
        }
        ui_lb_unit *v = &u[count++];
        v->start = i;
        v->ends_zwj = cls == UI_LB_ZWJ;
        if (combining) {
            // LB10: as if U+0041 (AL, Lu, Na, not Extended_Pictographic).
            v->base = 0x41;
            v->cls = UI_LB_AL;
            v->gc = UI_GC_LU;
            v->flags = 0;
        } else {
            v->base = cps[i];
            v->cls = cls;
            v->gc = r->gc;
            v->flags = r->flags;
        }
    }
    out[n] = UI_LINE_BREAK_MANDATORY; // LB3

    uint32_t before_sp = UINT32_MAX; // last non-SP unit up to the current one; MAX = sot
    uint32_t ri_run = 0;             // regional indicators ending at the current unit
    // LB25: the units so far match NU (SY | IS)* (in_num), or that followed by CL or CP
    // (num_close).
    bool in_num = false, num_close = false;

    for (uint32_t k = 0; k + 1 < count; k++) {
        const ui_lb_unit *A = &u[k], *B = &u[k + 1];
        uint8_t a = A->cls, b = B->cls;
        if (a != UI_LB_SP) before_sp = k;
        ri_run = a == UI_LB_RI ? ri_run + 1 : 0;
        bool prev_in_num = in_num;
        num_close = IN2(a, UI_LB_CL, UI_LB_CP) && prev_in_num;
        in_num = a == UI_LB_NU || (IN2(a, UI_LB_SY, UI_LB_IS) && prev_in_num);

        const ui_lb_unit *X = before_sp == UINT32_MAX ? NULL : &u[before_sp]; // before SP*
        uint8_t x = X ? X->cls : NONE_CLASS;
        const ui_lb_unit *P = k ? &u[k - 1] : NULL;            // the unit before A
        const ui_lb_unit *N = k + 2 < count ? &u[k + 2] : NULL; // the unit after B
        const ui_lb_unit *N2 = k + 3 < count ? &u[k + 3] : NULL;
        uint8_t p = P ? P->cls : NONE_CLASS, nx = N ? N->cls : NONE_CLASS;

        uint8_t r;
        if (a == UI_LB_BK) r = UI_LINE_BREAK_MANDATORY;                                // LB4
        else if (a == UI_LB_CR && b == UI_LB_LF) r = UI_LINE_BREAK_NONE;                // LB5
        else if (IN3(a, UI_LB_CR, UI_LB_LF, UI_LB_NL)) r = UI_LINE_BREAK_MANDATORY;     // LB5
        else if (is_hard(b)) r = UI_LINE_BREAK_NONE;                                    // LB6
        else if (IN2(b, UI_LB_SP, UI_LB_ZW)) r = UI_LINE_BREAK_NONE;                    // LB7
        else if (x == UI_LB_ZW) r = UI_LINE_BREAK_ALLOWED;                              // LB8
        else if (A->ends_zwj) r = UI_LINE_BREAK_NONE;                                   // LB8a
        else if (a == UI_LB_WJ || b == UI_LB_WJ) r = UI_LINE_BREAK_NONE;                // LB11
        else if (a == UI_LB_GL) r = UI_LINE_BREAK_NONE;                                 // LB12
        else if (b == UI_LB_GL && !IN3(a, UI_LB_SP, UI_LB_HY, UI_LB_HH))
            r = UI_LINE_BREAK_NONE;                                                     // LB12a
        else if (b == UI_LB_CL || b == UI_LB_CP || b == UI_LB_EX || b == UI_LB_SY)
            r = UI_LINE_BREAK_NONE;                                                     // LB13
        else if (x == UI_LB_OP) r = UI_LINE_BREAK_NONE;                                 // LB14
        else if (x == UI_LB_QU && X->gc == UI_GC_PI &&
                 (before_sp == 0 ||
                  (u[before_sp - 1].cls == UI_LB_BK || u[before_sp - 1].cls == UI_LB_CR ||
                   u[before_sp - 1].cls == UI_LB_LF || u[before_sp - 1].cls == UI_LB_NL ||
                   u[before_sp - 1].cls == UI_LB_OP || u[before_sp - 1].cls == UI_LB_QU ||
                   u[before_sp - 1].cls == UI_LB_GL || u[before_sp - 1].cls == UI_LB_SP ||
                   u[before_sp - 1].cls == UI_LB_ZW)))
            r = UI_LINE_BREAK_NONE;                                                     // LB15a
        else if (b == UI_LB_QU && B->gc == UI_GC_PF &&
                 (nx == NONE_CLASS || nx == UI_LB_SP || nx == UI_LB_GL || nx == UI_LB_WJ ||
                  nx == UI_LB_CL || nx == UI_LB_QU || nx == UI_LB_CP || nx == UI_LB_EX ||
                  nx == UI_LB_IS || nx == UI_LB_SY || is_hard(nx) || nx == UI_LB_ZW))
            r = UI_LINE_BREAK_NONE;                                                     // LB15b
        else if (a == UI_LB_SP && b == UI_LB_IS && nx == UI_LB_NU)
            r = UI_LINE_BREAK_ALLOWED;                                                  // LB15c
        else if (b == UI_LB_IS) r = UI_LINE_BREAK_NONE;                                 // LB15d
        else if (IN2(x, UI_LB_CL, UI_LB_CP) && b == UI_LB_NS) r = UI_LINE_BREAK_NONE;   // LB16
        else if (x == UI_LB_B2 && b == UI_LB_B2) r = UI_LINE_BREAK_NONE;                // LB17
        else if (a == UI_LB_SP) r = UI_LINE_BREAK_ALLOWED;                              // LB18
        else if ((b == UI_LB_QU && B->gc != UI_GC_PI) || (a == UI_LB_QU && A->gc != UI_GC_PF))
            r = UI_LINE_BREAK_NONE;                                                     // LB19
        else if ((b == UI_LB_QU && !east_asian(A)) ||
                 (b == UI_LB_QU && (!N || !east_asian(N))) ||
                 (a == UI_LB_QU && !east_asian(B)) ||
                 (a == UI_LB_QU && (!P || !east_asian(P))))
            r = UI_LINE_BREAK_NONE;                                                     // LB19a
        else if (b == UI_LB_CB || a == UI_LB_CB) r = UI_LINE_BREAK_ALLOWED;             // LB20
        else if (IN2(a, UI_LB_HY, UI_LB_HH) && IN2(b, UI_LB_AL, UI_LB_HL) &&
                 (p == NONE_CLASS || is_hard(p) || p == UI_LB_SP || p == UI_LB_ZW ||
                  p == UI_LB_CB || p == UI_LB_GL))
            r = UI_LINE_BREAK_NONE;                                                     // LB20a
        else if (b == UI_LB_BA || b == UI_LB_HH || b == UI_LB_HY || b == UI_LB_NS ||
                 a == UI_LB_BB)
            r = UI_LINE_BREAK_NONE;                                                     // LB21
        else if (p == UI_LB_HL && IN2(a, UI_LB_HY, UI_LB_HH) && b != UI_LB_HL)
            r = UI_LINE_BREAK_NONE;                                                     // LB21a
        else if (a == UI_LB_SY && b == UI_LB_HL) r = UI_LINE_BREAK_NONE;                // LB21b
        else if (b == UI_LB_IN) r = UI_LINE_BREAK_NONE;                                 // LB22
        else if ((IN2(a, UI_LB_AL, UI_LB_HL) && b == UI_LB_NU) ||
                 (a == UI_LB_NU && IN2(b, UI_LB_AL, UI_LB_HL)))
            r = UI_LINE_BREAK_NONE;                                                     // LB23
        else if ((a == UI_LB_PR && IN3(b, UI_LB_ID, UI_LB_EB, UI_LB_EM)) ||
                 (IN3(a, UI_LB_ID, UI_LB_EB, UI_LB_EM) && b == UI_LB_PO))
            r = UI_LINE_BREAK_NONE;                                                     // LB23a
        else if ((IN2(a, UI_LB_PR, UI_LB_PO) && IN2(b, UI_LB_AL, UI_LB_HL)) ||
                 (IN2(a, UI_LB_AL, UI_LB_HL) && IN2(b, UI_LB_PR, UI_LB_PO)))
            r = UI_LINE_BREAK_NONE;                                                     // LB24
        else if ((num_close && IN2(b, UI_LB_PO, UI_LB_PR)) ||
                 (in_num && IN2(b, UI_LB_PO, UI_LB_PR)) ||
                 (IN2(a, UI_LB_PO, UI_LB_PR) && b == UI_LB_OP &&
                  (nx == UI_LB_NU || (nx == UI_LB_IS && N2 && N2->cls == UI_LB_NU))) ||
                 (IN2(a, UI_LB_PO, UI_LB_PR) && b == UI_LB_NU) ||
                 (a == UI_LB_HY && b == UI_LB_NU) || (a == UI_LB_IS && b == UI_LB_NU) ||
                 (in_num && b == UI_LB_NU))
            r = UI_LINE_BREAK_NONE;                                                     // LB25
        else if ((a == UI_LB_JL && (b == UI_LB_JL || b == UI_LB_JV || b == UI_LB_H2 ||
                                    b == UI_LB_H3)) ||
                 (IN2(a, UI_LB_JV, UI_LB_H2) && IN2(b, UI_LB_JV, UI_LB_JT)) ||
                 (IN2(a, UI_LB_JT, UI_LB_H3) && b == UI_LB_JT))
            r = UI_LINE_BREAK_NONE;                                                     // LB26
        else if (((a == UI_LB_JL || a == UI_LB_JV || a == UI_LB_JT || a == UI_LB_H2 ||
                   a == UI_LB_H3) && b == UI_LB_PO) ||
                 (a == UI_LB_PR && (b == UI_LB_JL || b == UI_LB_JV || b == UI_LB_JT ||
                                    b == UI_LB_H2 || b == UI_LB_H3)))
            r = UI_LINE_BREAK_NONE;                                                     // LB27
        else if (IN2(a, UI_LB_AL, UI_LB_HL) && IN2(b, UI_LB_AL, UI_LB_HL))
            r = UI_LINE_BREAK_NONE;                                                     // LB28
        else if ((a == UI_LB_AP && aksara_base(B)) ||
                 (aksara_base(A) && IN2(b, UI_LB_VF, UI_LB_VI)) ||
                 (a == UI_LB_VI && P && aksara_base(P) && aksara_or_circle(B)) ||
                 (aksara_base(A) && aksara_base(B) && nx == UI_LB_VF))
            r = UI_LINE_BREAK_NONE;                                                     // LB28a
        else if (a == UI_LB_IS && IN2(b, UI_LB_AL, UI_LB_HL)) r = UI_LINE_BREAK_NONE;   // LB29
        else if ((IN3(a, UI_LB_AL, UI_LB_HL, UI_LB_NU) && b == UI_LB_OP && !east_asian(B)) ||
                 (a == UI_LB_CP && !east_asian(A) && IN3(b, UI_LB_AL, UI_LB_HL, UI_LB_NU)))
            r = UI_LINE_BREAK_NONE;                                                     // LB30
        else if (a == UI_LB_RI && b == UI_LB_RI && (ri_run & 1))
            r = UI_LINE_BREAK_NONE;                                                     // LB30a
        else if (b == UI_LB_EM &&
                 (a == UI_LB_EB || ((A->flags & UI_UCD_EXTPICT) && A->gc == UI_GC_CN)))
            r = UI_LINE_BREAK_NONE;                                                     // LB30b
        else r = UI_LINE_BREAK_ALLOWED;                                                 // LB31
        out[B->start] = r;
    }
    return UI_OK;
}
