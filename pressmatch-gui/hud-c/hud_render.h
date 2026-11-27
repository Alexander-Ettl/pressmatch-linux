#ifndef PRESSMATCH_HUD_RENDER_H
#define PRESSMATCH_HUD_RENDER_H

#include <stdbool.h>
#include <stdint.h>
#include <cairo/cairo.h>

#define HUD_L_MAX       360.0
#define HUD_BLOOM_PAD   28
#define HUD_LOGICAL_SZ  388  /* (int)HUD_L_MAX + HUD_BLOOM_PAD */

typedef enum {
    HUD_STATE_HIDDEN = 0,
    HUD_STATE_ARMED,
    HUD_STATE_SCANNING,
    HUD_STATE_PARTIAL,
    HUD_STATE_RETRY,
    HUD_STATE_DECLINED,
    HUD_STATE_MATCH,
} HudState;

typedef enum {
    HUD_HINT_NONE = 0,
    HUD_HINT_TOO_LIGHT,
    HUD_HINT_TOO_HARD,
    HUD_HINT_OFF_CENTER,
    HUD_HINT_MOVING,
    HUD_HINT_DRY,
    HUD_HINT_WRONG_AREA,
} HudHintKind;

typedef struct {
    HudState    state;
    HudHintKind hint_kind;
    char        label[128];
    double      L;              /* Current fillet extent in logical px [0..HUD_L_MAX] */
    double      content_alpha;  /* [0..1] interior fade */
    double      phase;          /* Continuous animation phase (seconds) */
    double      state_dt;       /* Seconds elapsed since entering current state */
    double      glint_dt;       /* Seconds since glint trigger (<0 or >glint_dur = off) */
    double      glint_dur;      /* Glint duration (0.92s) */
} HudRenderState;

typedef struct HudRenderer HudRenderer;

HudRenderer *hud_renderer_create(void);
void         hud_renderer_destroy(HudRenderer *r);

bool         hud_is_german(void);
const char  *hud_tr(const char *key);
HudHintKind  hud_classify_hint(const char *hint, HudState fallback_state);
const char  *hud_hint_tr(HudHintKind kind);

/* Render one frame onto a Cairo ARGB32 surface of physical size (phys_w x phys_h)
 * representing the fixed HUD_LOGICAL_SZ x HUD_LOGICAL_SZ logical corner. */
void hud_render_frame(HudRenderer *r, cairo_t *cr, int phys_w, int phys_h,
                      const HudRenderState *st);

/* Render 6 reference PNGs into out_dir (at 1.5x scale, 582x582) for visual parity checks. */
int hud_render_all_previews(const char *out_dir);

#endif
