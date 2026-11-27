#include "hud_render.h"

#include <ctype.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define G3_K        0.50
#define G3_STEPS    110

typedef struct {
    double r, g, b;
} RGB;

typedef struct {
    RGB    rim;
    RGB    rim_peak;
    RGB    bloom;
    RGB    bloom_peak;
    RGB    label;
    double label_a;
    RGB    glyph;
} Palette;

struct HudRenderer {
    PangoFontDescription *font_desc;
    int                   font_px;
    cairo_font_options_t *font_opts;
};

static bool g_is_de_checked = false;
static bool g_is_de = false;

bool hud_is_german(void) {
    if (!g_is_de_checked) {
        const char *vars[] = {"LC_ALL", "LC_MESSAGES", "LANG", "LANGUAGE"};
        for (size_t i = 0; i < 4; i++) {
            const char *v = getenv(vars[i]);
            if (v && v[0]) {
                g_is_de = (tolower((unsigned char)v[0]) == 'd' &&
                           tolower((unsigned char)v[1]) == 'e');
                break;
            }
        }
        g_is_de_checked = true;
    }
    return g_is_de;
}

const char *hud_tr(const char *key) {
    static const struct { const char *k; const char *de; const char *en; } map[] = {
        {"armed",      "Zum Entsperren berühren",    "Touch to unlock"},
        {"scanning",   "Wird gelesen…",              "Reading…"},
        {"match",      "Bestätigt",                  "Verified"},
        {"unlocked",   "Entsperrt",                  "Unlocked"},
        {"retry",      "Nochmal versuchen",          "Try again"},
        {"partial",    "Fester drücken",             "Press a little firmer"},
        {"declined",   "Passwort verwenden",         "Use your password"},
        {"too_light",  "Fester drücken",             "Press a little firmer"},
        {"too_hard",   "Sanfter drücken",            "Press more gently"},
        {"off_center", "Finger zentrieren",          "Center your finger"},
        {"moving",     "Ruhig halten",               "Hold still until it reacts"},
        {"dry",        "Finger & Sensor abwischen",  "Wipe finger and sensor"},
        {"wrong_area", "Finger leicht bewegen",      "Move finger slightly"},
    };
    bool de = hud_is_german();
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(map[i].k, key) == 0)
            return de ? map[i].de : map[i].en;
    }
    return key;
}

const char *hud_hint_tr(HudHintKind kind) {
    switch (kind) {
    case HUD_HINT_TOO_LIGHT:  return hud_tr("too_light");
    case HUD_HINT_TOO_HARD:   return hud_tr("too_hard");
    case HUD_HINT_OFF_CENTER: return hud_tr("off_center");
    case HUD_HINT_MOVING:     return hud_tr("moving");
    case HUD_HINT_DRY:        return hud_tr("dry");
    case HUD_HINT_WRONG_AREA: return hud_tr("wrong_area");
    default:                  return "";
    }
}

static bool str_contains_ci(const char *hay, const char *needle) {
    if (!hay || !needle || !needle[0]) return false;
    size_t nlen = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nlen) return true;
    }
    return false;
}

HudHintKind hud_classify_hint(const char *hint, HudState fallback_state) {
    if (hint && hint[0]) {
        if (str_contains_ci(hint, "firmer") || str_contains_ci(hint, "light") ||
            str_contains_ci(hint, "cover")  || str_contains_ci(hint, "fester"))
            return HUD_HINT_TOO_LIGHT;
        if (str_contains_ci(hint, "gently") || str_contains_ci(hint, "hard") ||
            str_contains_ci(hint, "sanfter"))
            return HUD_HINT_TOO_HARD;
        if (str_contains_ci(hint, "center") || str_contains_ci(hint, "zentrier"))
            return HUD_HINT_OFF_CENTER;
        if (str_contains_ci(hint, "still")  || str_contains_ci(hint, "moving") ||
            str_contains_ci(hint, "ruhig"))
            return HUD_HINT_MOVING;
        if (str_contains_ci(hint, "wipe")   || str_contains_ci(hint, "dry") ||
            str_contains_ci(hint, "wisch"))
            return HUD_HINT_DRY;
        if (str_contains_ci(hint, "setup")  || str_contains_ci(hint, "move") ||
            str_contains_ci(hint, "slightly") || str_contains_ci(hint, "roll"))
            return HUD_HINT_WRONG_AREA;
    }
    if (fallback_state == HUD_STATE_PARTIAL)
        return HUD_HINT_TOO_LIGHT;
    return HUD_HINT_NONE;
}

HudRenderer *hud_renderer_create(void) {
    HudRenderer *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->font_desc = pango_font_description_from_string("Sans Semi-Bold 13px");
    r->font_px = 13;
    r->font_opts = cairo_font_options_create();
    cairo_font_options_set_antialias(r->font_opts, CAIRO_ANTIALIAS_GRAY);
    cairo_font_options_set_hint_style(r->font_opts, CAIRO_HINT_STYLE_SLIGHT);
    return r;
}

void hud_renderer_destroy(HudRenderer *r) {
    if (!r) return;
    if (r->font_desc) pango_font_description_free(r->font_desc);
    if (r->font_opts) cairo_font_options_destroy(r->font_opts);
    free(r);
}

/* ── Exact CAD G3 corner Bézier (degree 7, K = 0.50) ─────────────────────── */

static const double BINOM7[8] = {1, 7, 21, 35, 35, 21, 7, 1};

static void g3_control_points(double w, double h, double L, double pts[8][2]) {
    double step = (L - G3_K * L) / 3.0;
    for (int i = 0; i < 4; i++) {
        pts[i][0] = w - (L - i * step);
        pts[i][1] = h;
    }
    for (int i = 0; i < 4; i++) {
        int j = 3 - i;
        pts[4 + i][0] = w;
        pts[4 + i][1] = h - (L - j * step);
    }
}

static void g3_eval(const double pts[8][2], double t, double *ox, double *oy) {
    double mt = 1.0 - t;
    double x = 0.0, y = 0.0;
    double mt_pow[8], t_pow[8];
    mt_pow[0] = 1.0;
    t_pow[0] = 1.0;
    for (int i = 1; i < 8; i++) {
        mt_pow[i] = mt_pow[i - 1] * mt;
        t_pow[i]  = t_pow[i - 1] * t;
    }
    for (int i = 0; i < 8; i++) {
        double b = BINOM7[i] * mt_pow[7 - i] * t_pow[i];
        x += b * pts[i][0];
        y += b * pts[i][1];
    }
    *ox = x;
    *oy = y;
}

/* ── State Palettes (exact match to pressmatch_hud.py & PressMatchPill.qml) ─ */

static Palette select_palette(HudState st, HudHintKind hk) {
    static const Palette PAL_ARMED = {
        {220/255.0, 230/255.0, 247/255.0}, {1.0, 1.0, 1.0},
        {143/255.0, 186/255.0, 255/255.0}, {216/255.0, 233/255.0, 255/255.0},
        {235/255.0, 235/255.0, 245/255.0}, 185/255.0,
        {1.0, 1.0, 1.0}
    };
    static const Palette PAL_SCANNING = {
        {90/255.0,  200/255.0, 250/255.0}, {216/255.0, 244/255.0, 255/255.0},
        {10/255.0,  132/255.0, 255/255.0}, {100/255.0, 210/255.0, 255/255.0},
        {124/255.0, 200/255.0, 255/255.0}, 1.0,
        {10/255.0,  132/255.0, 255/255.0}
    };
    static const Palette PAL_WRONG_AREA = {
        {100/255.0, 210/255.0, 255/255.0}, {230/255.0, 249/255.0, 255/255.0},
        {10/255.0,  132/255.0, 255/255.0}, {100/255.0, 210/255.0, 255/255.0},
        {100/255.0, 210/255.0, 255/255.0}, 1.0,
        {100/255.0, 210/255.0, 255/255.0}
    };
    static const Palette PAL_MATCH = {
        {76/255.0,  217/255.0, 100/255.0}, {217/255.0, 255/255.0, 226/255.0},
        {48/255.0,  209/255.0, 88/255.0},  {125/255.0, 255/255.0, 155/255.0},
        {48/255.0,  209/255.0, 88/255.0},  1.0,
        {48/255.0,  209/255.0, 88/255.0}
    };
    static const Palette PAL_AMBER = {
        {255/255.0, 179/255.0, 64/255.0},  {255/255.0, 232/255.0, 191/255.0},
        {255/255.0, 159/255.0, 10/255.0},  {255/255.0, 208/255.0, 128/255.0},
        {255/255.0, 179/255.0, 64/255.0},  1.0,
        {255/255.0, 159/255.0, 10/255.0}
    };
    static const Palette PAL_CORAL = {
        {255/255.0, 105/255.0, 97/255.0},  {255/255.0, 212/255.0, 209/255.0},
        {255/255.0, 69/255.0,  58/255.0},  {255/255.0, 140/255.0, 133/255.0},
        {255/255.0, 105/255.0, 97/255.0},  1.0,
        {255/255.0, 94/255.0,  77/255.0}
    };

    if (st == HUD_STATE_MATCH) return PAL_MATCH;
    if (st == HUD_STATE_SCANNING) return PAL_SCANNING;
    if ((st == HUD_STATE_PARTIAL || st == HUD_STATE_RETRY) &&
        (hk == HUD_HINT_TOO_LIGHT || hk == HUD_HINT_TOO_HARD || hk == HUD_HINT_DRY))
        return PAL_AMBER;
    if (st == HUD_STATE_RETRY && hk == HUD_HINT_WRONG_AREA) return PAL_WRONG_AREA;
    if (st == HUD_STATE_RETRY && (hk == HUD_HINT_OFF_CENTER || hk == HUD_HINT_MOVING)) {
        Palette p = PAL_ARMED;
        p.label_a = 0.92;
        return p;
    }
    if (st == HUD_STATE_RETRY || st == HUD_STATE_DECLINED) return PAL_CORAL;
    return PAL_ARMED;
}

static double clamp01(double v) {
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

static cairo_pattern_t *make_edge_fade_grad(double x1, double y1, double x2, double y2,
                                            RGB edge, RGB peak, double max_a) {
    cairo_pattern_t *pat = cairo_pattern_create_linear(x1, y1, x2, y2);
    double a_mid = clamp01(max_a * 0.36);
    double a_pk  = clamp01(max_a);
    cairo_pattern_add_color_stop_rgba(pat, 0.00, edge.r, edge.g, edge.b, 0.0);
    cairo_pattern_add_color_stop_rgba(pat, 0.16, edge.r, edge.g, edge.b, a_mid);
    cairo_pattern_add_color_stop_rgba(pat, 0.50, peak.r, peak.g, peak.b, a_pk);
    cairo_pattern_add_color_stop_rgba(pat, 0.84, edge.r, edge.g, edge.b, a_mid);
    cairo_pattern_add_color_stop_rgba(pat, 1.00, edge.r, edge.g, edge.b, 0.0);
    return pat;
}

static void trace_curve(cairo_t *cr, const double curve[G3_STEPS + 1][2]) {
    cairo_new_path(cr);
    cairo_move_to(cr, curve[0][0], curve[0][1]);
    for (int i = 1; i <= G3_STEPS; i++)
        cairo_line_to(cr, curve[i][0], curve[i][1]);
}

static void trace_shape(cairo_t *cr, const double curve[G3_STEPS + 1][2],
                        double w, double h, double L) {
    trace_curve(cr, curve);
    cairo_line_to(cr, w + 4.0, h - L);
    cairo_line_to(cr, w + 4.0, h + 4.0);
    cairo_line_to(cr, w - L,   h + 4.0);
    cairo_close_path(cr);
}

/* ── Touch ID Vertical Whorl Glyph & Checkmark ───────────────────────────── */

static void draw_fingerprint_glyph(cairo_t *cr, double cx, double cy,
                                   RGB col, double alpha, double scale,
                                   double fill_frac) {
    static const double arcs[7][5] = {
        { 5.0,  7.2, -10.0, 215.0, 1.8},
        {10.0, 12.6,  -5.0, 198.0, 1.2},
        {10.0, 12.6, 232.0, 108.0, 1.2},
        {15.0, 17.6,  12.0, 172.0, 0.6},
        {15.0, 17.6, 212.0, 128.0, 0.6},
        {20.0, 22.8,  28.0, 138.0, 0.0},
        {20.0, 22.8, 200.0, 148.0, 0.0},
    };
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_scale(cr, scale, scale);
    cairo_set_source_rgba(cr, col.r, col.g, col.b, clamp01(alpha));
    cairo_set_line_width(cr, 2.0);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

    if (fill_frac > 0.05) {
        double f = fill_frac * 3.0;
        if (f > 1.0) f = 1.0;
        cairo_new_path(cr);
        cairo_move_to(cr, 0.0, 2.0);
        cairo_line_to(cr, 0.0, 2.0 + 9.6 * f);
        cairo_stroke(cr);
    }

    for (int i = 0; i < 7; i++) {
        double rx = arcs[i][0], ry = arcs[i][1];
        double start_deg = arcs[i][2], span_deg = arcs[i][3], y_off = arcs[i][4];
        double span = span_deg;
        if (fill_frac < 1.0) {
            double local = clamp01((fill_frac * 7.0 - i * 0.55) / 1.4);
            if (local <= 0.01) continue;
            span = span_deg * local;
        }
        /* Match QPainter/Canvas counterclockwise mathematical angle convention */
        double a0 = -start_deg * (M_PI / 180.0);
        double a1 = -(start_deg + span) * (M_PI / 180.0);

        cairo_save(cr);
        cairo_translate(cr, 0.0, y_off);
        cairo_scale(cr, 1.0, ry / rx);
        cairo_new_path(cr);
        cairo_arc_negative(cr, 0.0, 0.0, rx, a0, a1);
        cairo_restore(cr); /* restore before stroke so line_width stays uniform 2.0 */
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

static void draw_checkmark(cairo_t *cr, double cx, double cy, double scale,
                           double prog, RGB col, double alpha) {
    cairo_save(cr);
    cairo_set_source_rgba(cr, col.r, col.g, col.b, clamp01(alpha));
    cairo_set_line_width(cr, 3.2 * scale);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    double p1x = cx - 11.0 * scale, p1y = cy + 1.0 * scale;
    double p2x = cx -  3.0 * scale, p2y = cy + 9.0 * scale;
    double p3x = cx + 13.0 * scale, p3y = cy - 9.0 * scale;

    cairo_new_path(cr);
    cairo_move_to(cr, p1x, p1y);
    if (prog <= 0.42) {
        double t = prog / 0.42;
        cairo_line_to(cr, p1x + (p2x - p1x) * t, p1y + (p2y - p1y) * t);
    } else {
        double t = (prog - 0.42) / 0.58;
        cairo_line_to(cr, p2x, p2y);
        cairo_line_to(cr, p2x + (p3x - p2x) * t, p2y + (p3y - p2y) * t);
    }
    cairo_stroke(cr);
    cairo_restore(cr);
}

/* ── Curved Inner-Fillet Typography via PangoCairo ───────────────────────── */

static void sample_polyline(const double pts[G3_STEPS + 1][2],
                            const double cum[G3_STEPS + 1],
                            double dist, double *ox, double *oy, double *oang) {
    double total = cum[G3_STEPS];
    if (dist < 0.0) dist = 0.0;
    if (dist > total) dist = total;
    int lo = 0, hi = G3_STEPS;
    while (lo < hi - 1) {
        int mid = (lo + hi) >> 1;
        if (cum[mid] <= dist) lo = mid;
        else hi = mid;
    }
    double seg = cum[hi] - cum[lo];
    double f = (seg > 1e-6) ? (dist - cum[lo]) / seg : 0.0;
    *ox = pts[lo][0] + (pts[hi][0] - pts[lo][0]) * f;
    *oy = pts[lo][1] + (pts[hi][1] - pts[lo][1]) * f;
    *oang = atan2(pts[hi][1] - pts[lo][1], pts[hi][0] - pts[lo][0]);
}

static void draw_curved_label(HudRenderer *r, cairo_t *cr,
                              const double curve[G3_STEPS + 1][2],
                              double w, double h, double rad,
                              const char *raw_text, RGB col, double alpha) {
    if (!raw_text || !raw_text[0]) return;

    char *owned_valid = NULL;
    const char *text = raw_text;
    if (!g_utf8_validate(raw_text, -1, NULL)) {
        owned_valid = g_utf8_make_valid(raw_text, -1);
        text = owned_valid;
    }

    double off = rad * 0.30;
    if (off < 18.0) off = 18.0;

    double inner[G3_STEPS + 1][2];
    double cum[G3_STEPS + 1];
    cum[0] = 0.0;
    for (int i = 0; i <= G3_STEPS; i++) {
        int ia = i > 0 ? i - 1 : 0;
        int ib = i < G3_STEPS ? i + 1 : G3_STEPS;
        double tx = curve[ib][0] - curve[ia][0];
        double ty = curve[ib][1] - curve[ia][1];
        double norm = hypot(tx, ty);
        if (norm < 1e-6) norm = 1.0;
        double nx = -ty / norm;
        double ny =  tx / norm;
        if ((w - curve[i][0]) * nx + (h - curve[i][1]) * ny < 0.0) {
            nx = -nx;
            ny = -ny;
        }
        inner[i][0] = curve[i][0] + nx * off;
        inner[i][1] = curve[i][1] + ny * off;
        if (i > 0) {
            cum[i] = cum[i - 1] + hypot(inner[i][0] - inner[i - 1][0],
                                        inner[i][1] - inner[i - 1][1]);
        }
    }
    double total_len = cum[G3_STEPS];
    if (total_len < 40.0) {
        g_free(owned_valid);
        return;
    }

    int px = (int)llround(fmax(12.0, fmin(14.5, rad * 0.21)));
    if (px != r->font_px) {
        pango_font_description_set_absolute_size(r->font_desc, px * PANGO_SCALE);
        r->font_px = px;
    }

    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_cairo_context_set_font_options(pango_layout_get_context(layout), r->font_opts);
    pango_layout_set_font_description(layout, r->font_desc);

    /* Measure UTF-8 codepoints individually to place cleanly along the curve */
    const char *starts[128];
    int         lens[128];
    double      advs[128];
    int         count = 0;
    double      text_w = 0.0;

    for (const char *p = text; *p && count < 128; ) {
        const char *next = g_utf8_next_char(p);
        int clen = (int)(next - p);
        pango_layout_set_text(layout, p, clen);
        int iw = 0, ih = 0;
        pango_layout_get_size(layout, &iw, &ih);
        double adv = (double)iw / PANGO_SCALE;
        starts[count] = p;
        lens[count]   = clen;
        advs[count]   = adv;
        text_w += adv;
        count++;
        p = next;
    }

    double baseline = (double)pango_layout_get_baseline(layout) / PANGO_SCALE;
    double s = (total_len - text_w) * 0.5;
    cairo_set_source_rgba(cr, col.r, col.g, col.b, clamp01(alpha));

    for (int i = 0; i < count; i++) {
        double x, y, ang;
        sample_polyline(inner, cum, s + advs[i] * 0.5, &x, &y, &ang);
        pango_layout_set_text(layout, starts[i], lens[i]);
        cairo_save(cr);
        cairo_translate(cr, x, y);
        cairo_rotate(cr, ang);
        cairo_move_to(cr, -advs[i] * 0.5, -baseline);
        pango_cairo_show_layout(cr, layout);
        cairo_restore(cr);
        s += advs[i];
    }

    g_object_unref(layout);
    g_free(owned_valid);
}

/* ── Full Frame Renderer ─────────────────────────────────────────────────── */

void hud_render_frame(HudRenderer *r, cairo_t *cr, int phys_w, int phys_h,
                      const HudRenderState *st) {
    /* Clear backing buffer to 100% transparent */
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_restore(cr);

    double L = st->L;
    if (L < 0.8) return;

    double w = (double)HUD_LOGICAL_SZ;
    double h = (double)HUD_LOGICAL_SZ;

    cairo_save(cr);
    cairo_scale(cr, (double)phys_w / w, (double)phys_h / h);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);

    double pts[8][2];
    double curve[G3_STEPS + 1][2];
    g3_control_points(w, h, L, pts);
    for (int i = 0; i <= G3_STEPS; i++)
        g3_eval(pts, (double)i / G3_STEPS, &curve[i][0], &curve[i][1]);

    Palette pal = select_palette(st->state, st->hint_kind);
    double ph = st->phase;
    double dt = st->state_dt;

    /* 1. Outer-only OLED ambient bloom drawn BEHIND pure #000000 cutout */
    double breath = 0.5 + 0.5 * sin(ph * 2.1);
    double bloom_mult = 0.72;
    if (st->state == HUD_STATE_ARMED)
        bloom_mult = 0.42 + 0.24 * breath;
    else if (st->state == HUD_STATE_MATCH)
        bloom_mult = 0.95;
    else if (st->state == HUD_STATE_SCANNING || st->state == HUD_STATE_PARTIAL ||
             st->hint_kind != HUD_HINT_NONE)
        bloom_mult = 0.65 + 0.20 * sin(ph * 3.5);
    else if (st->state == HUD_STATE_DECLINED)
        bloom_mult = 0.34;

    static const double bloom_passes[5][2] = {
        {26.0, 10.0 / 255.0},
        {18.0, 18.0 / 255.0},
        {12.0, 30.0 / 255.0},
        { 7.0, 48.0 / 255.0},
        { 3.8, 76.0 / 255.0},
    };
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    for (int i = 0; i < 5; i++) {
        cairo_pattern_t *pat = make_edge_fade_grad(
            w - L, h, w, h - L, pal.bloom, pal.bloom_peak,
            bloom_passes[i][1] * bloom_mult);
        trace_curve(cr, curve);
        cairo_set_source(cr, pat);
        cairo_set_line_width(cr, bloom_passes[i][0]);
        cairo_stroke(cr);
        cairo_pattern_destroy(pat);
    }

    /* 2. Pure #000000 OLED hardware cutout */
    trace_shape(cr, curve, w, h, L);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 1.0);
    cairo_fill(cr);

    /* 3. Centered 1.35-logical-px specular keyline (fades to 0% at bezel ends) */
    double rim_a = (st->state == HUD_STATE_MATCH || st->state == HUD_STATE_SCANNING)
                   ? (230.0 / 255.0) : (205.0 / 255.0);
    cairo_pattern_t *rim_pat = make_edge_fade_grad(
        w - L, h, w, h - L, pal.rim, pal.rim_peak, rim_a);
    trace_curve(cr, curve);
    cairo_set_source(cr, rim_pat);
    cairo_set_line_width(cr, 1.35);
    cairo_stroke(cr);
    cairo_pattern_destroy(rim_pat);

    /* 4. Traveling soft specular glint on extend / match */
    if (st->glint_dt >= 0.0 && st->glint_dt <= st->glint_dur && st->glint_dur > 0.0) {
        double gt = st->glint_dt / st->glint_dur;
        double ge = gt < 0.5 ? (2.0 * gt * gt) : (1.0 - pow(-2.0 * gt + 2.0, 2.0) / 2.0);
        double center = 0.12 + ge * 0.76;
        double env = sin(gt * M_PI);
        cairo_pattern_t *gp = cairo_pattern_create_linear(w - L, h, w, h - L);
        cairo_pattern_add_color_stop_rgba(gp, fmax(0.0, center - 0.16), 1.0, 1.0, 1.0, 0.0);
        cairo_pattern_add_color_stop_rgba(gp, center,                  1.0, 1.0, 1.0, clamp01(0.84 * env));
        cairo_pattern_add_color_stop_rgba(gp, fmin(1.0, center + 0.16), 1.0, 1.0, 1.0, 0.0);
        trace_curve(cr, curve);
        cairo_set_source(cr, gp);
        cairo_set_line_width(cr, 2.2);
        cairo_stroke(cr);
        cairo_pattern_destroy(gp);
    }

    /* 5. Clipped interior: Touch ID glyph, guidance animations, curved label */
    double ca = clamp01(st->content_alpha);
    if (ca > 0.01) {
        cairo_save(cr);
        trace_shape(cr, curve, w, h, L);
        cairo_clip(cr);

        double mx, my;
        g3_eval(pts, 0.5, &mx, &my);
        double D = hypot(w - mx, h - my);
        double rad = D / (1.0 + sqrt(2.0));
        double cx = w - rad * 0.86;
        double cy = h - rad * 0.86;
        double sc = (rad * 0.43) / 22.8;
        const RGB WHITE = {1.0, 1.0, 1.0};

        if (st->state == HUD_STATE_MATCH) {
            double ridge_a = clamp01(1.0 - fmax(0.0, dt - 0.12) / 0.22);
            if (ridge_a > 0.01)
                draw_fingerprint_glyph(cr, cx, cy, pal.glyph, ridge_a * ca, sc, 1.0);
            double cprog = clamp01((dt - 0.16) / 0.32);
            if (cprog > 0.0)
                draw_checkmark(cr, cx, cy, sc * 1.12, cprog, pal.glyph, ca);
        } else if (st->hint_kind == HUD_HINT_TOO_LIGHT || st->hint_kind == HUD_HINT_TOO_HARD) {
            bool inward = (st->hint_kind == HUD_HINT_TOO_LIGHT);
            double period = inward ? 1.2 : 1.3;
            for (int i = 0; i < 3; i++) {
                double t = fmod((ph / period) + (double)i / 3.0, 1.0);
                double rsc = inward ? (1.42 - 0.60 * t) : (0.84 + 0.58 * t);
                double ra  = inward ? (sin(t * M_PI) * 0.82) : ((1.0 - t) * 0.85);
                cairo_new_path(cr);
                cairo_arc(cr, cx, cy, 25.0 * sc * rsc, 0.0, 2.0 * M_PI);
                cairo_set_source_rgba(cr, pal.glyph.r, pal.glyph.g, pal.glyph.b, clamp01(ra * ca));
                cairo_set_line_width(cr, 1.5 * sc);
                cairo_stroke(cr);
            }
            double pulse = 0.5 - 0.5 * cos(ph * (2.0 * M_PI / period));
            double gsc = sc * (inward ? (1.0 - 0.12 * pulse) : (0.90 + 0.14 * pulse));
            draw_fingerprint_glyph(cr, cx, cy, pal.glyph, 0.95 * ca, gsc, 1.0);
        } else if (st->hint_kind == HUD_HINT_WRONG_AREA) {
            double dashes[2] = {3.5 * sc, 4.5 * sc};
            cairo_save(cr);
            cairo_set_dash(cr, dashes, 2, 0.0);
            cairo_new_path(cr);
            cairo_arc(cr, cx, cy, 27.0 * sc, 0.0, 2.0 * M_PI);
            cairo_set_source_rgba(cr, 100/255.0, 210/255.0, 1.0, 0.28 * ca);
            cairo_set_line_width(cr, 1.4 * sc);
            cairo_stroke(cr);
            cairo_restore(cr);
            double ang = ph * 2.8;
            double ox = sin(ang) * 5.2 * sc;
            double oy = sin(ang * 2.0) * 3.2 * sc;
            draw_fingerprint_glyph(cr, cx + ox, cy + oy, pal.glyph, 0.95 * ca, sc, 1.0);
        } else if (st->hint_kind == HUD_HINT_OFF_CENTER) {
            cairo_new_path(cr);
            cairo_arc(cr, cx, cy, 26.0 * sc, 0.0, 2.0 * M_PI);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.25 * ca);
            cairo_set_line_width(cr, 1.3 * sc);
            cairo_stroke(cr);
            double t = fmod(ph / 1.3, 1.0);
            double ease = 1.0 - pow(1.0 - t, 3.0);
            double gx = (1.0 - ease) * (-12.0 * sc);
            draw_fingerprint_glyph(cr, cx, cy, WHITE, 0.37 * ca, sc, 1.0);
            draw_fingerprint_glyph(cr, cx + gx, cy, WHITE, 0.85 * sin(t * M_PI) * ca, sc, 1.0);
        } else if (st->state == HUD_STATE_SCANNING) {
            draw_fingerprint_glyph(cr, cx, cy, WHITE, 0.28 * ca, sc, 1.0);
            double fill_prog = clamp01(fmod(dt, 0.95) / 0.62);
            draw_fingerprint_glyph(cr, cx, cy, pal.glyph, ca, sc, fill_prog);
        } else if (st->state == HUD_STATE_RETRY) {
            double shake_x = (dt < 0.48) ? (sin(dt * 42.0) * 6.5 * exp(-dt * 7.5)) : 0.0;
            draw_fingerprint_glyph(cr, cx + shake_x, cy, pal.glyph, 0.95 * ca, sc, 1.0);
        } else if (st->state == HUD_STATE_DECLINED) {
            draw_fingerprint_glyph(cr, cx, cy, pal.glyph, 0.47 * ca, sc, 1.0);
        } else {
            double b = 0.74 + 0.26 * (0.5 + 0.5 * sin(ph * 2.1));
            draw_fingerprint_glyph(cr, cx, cy, WHITE, 0.94 * b * ca, sc, 1.0);
        }

        draw_curved_label(r, cr, curve, w, h, rad, st->label, pal.label, pal.label_a * ca);
        cairo_restore(cr);
    }

    cairo_restore(cr);
}

/* ── Offscreen PNG Preview Suite (at 1.5x scale = 582x582) ───────────────── */

int hud_render_all_previews(const char *out_dir) {
    mkdir(out_dir, 0755);
    HudRenderer *r = hud_renderer_create();
    if (!r) return 1;

    const int sz = 582; /* 388 * 1.5 */
    struct {
        const char *name;
        double bg_r, bg_g, bg_b;
        HudState state;
        HudHintKind hint;
        const char *label;
        double glint_dt;
    } cases[] = {
        {"armed_oled",      0.0, 0.0, 0.0,          HUD_STATE_ARMED,    HUD_HINT_NONE,       hud_tr("armed"),      -1.0},
        {"armed_light",     232/255.0, 238/255.0, 248/255.0, HUD_STATE_ARMED, HUD_HINT_NONE, hud_tr("armed"),      -1.0},
        {"scanning_oled",   0.0, 0.0, 0.0,          HUD_STATE_SCANNING, HUD_HINT_NONE,       hud_tr("scanning"),   -1.0},
        {"light_oled",      0.0, 0.0, 0.0,          HUD_STATE_PARTIAL,  HUD_HINT_TOO_LIGHT,  hud_tr("too_light"),  -1.0},
        {"wrong_area_oled", 0.0, 0.0, 0.0,          HUD_STATE_RETRY,    HUD_HINT_WRONG_AREA, hud_tr("wrong_area"), -1.0},
        {"match_oled",      0.0, 0.0, 0.0,          HUD_STATE_MATCH,    HUD_HINT_NONE,       hud_tr("match"),       0.38},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        cairo_surface_t *fg = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, sz, sz);
        cairo_t *fcr = cairo_create(fg);
        HudRenderState st = {
            .state         = cases[i].state,
            .hint_kind     = cases[i].hint,
            .L             = HUD_L_MAX,
            .content_alpha = 1.0,
            .phase         = 0.8,
            .state_dt      = 0.55,
            .glint_dt      = cases[i].glint_dt,
            .glint_dur     = 0.92,
        };
        snprintf(st.label, sizeof(st.label), "%s", cases[i].label);
        hud_render_frame(r, fcr, sz, sz, &st);
        cairo_destroy(fcr);

        cairo_surface_t *comp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, sz, sz);
        cairo_t *ccr = cairo_create(comp);
        cairo_set_source_rgb(ccr, cases[i].bg_r, cases[i].bg_g, cases[i].bg_b);
        cairo_paint(ccr);
        cairo_set_source_surface(ccr, fg, 0.0, 0.0);
        cairo_paint(ccr);
        cairo_destroy(ccr);

        char path[512];
        snprintf(path, sizeof(path), "%s/hud_%s.png", out_dir, cases[i].name);
        cairo_surface_write_to_png(comp, path);
        cairo_surface_destroy(comp);
        cairo_surface_destroy(fg);
    }

    hud_renderer_destroy(r);
    printf("Rendered 6 native C11 OLED G3 HUD previews (582x582 @ 1.5x) to %s\n", out_dir);
    return 0;
}
