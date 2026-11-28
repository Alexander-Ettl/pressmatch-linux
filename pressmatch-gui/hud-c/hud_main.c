/*
 * pressmatch-hud — Native C11 Wayland OLED G3 Corner Fingerprint HUD
 *
 * Architecture:
 *  - Permanently mapped zwlr_layer_surface_v1 anchored flush BOTTOM|RIGHT at a
 *    fixed 388x388 logical viewport (empty wl_region input mask -> click-through).
 *  - Idle: attaches a 1x1 transparent buffer (wp_single_pixel_buffer_manager_v1
 *    or 1x1 SHM fallback) and munmaps active double-buffered SHM pools once
 *    retired by wl_buffer.release. Zero 500ms timer, zero idle CPU wakeups.
 *  - Active: double-buffered premultiplied ARGB32 wl_shm pools scaled via
 *    wp_fractional_scale_v1 + wp_viewporter, paced by wl_surface.frame VSync.
 *  - Lockscreen handoff: classifies service == "kde-fingerprint" explicitly,
 *    commits staged frames immediately behind KWin's lock layer without waiting
 *    for a frame callback, and holds "match" until ActiveChanged(false).
 *  - Single poll() loop over Wayland fd + HUD Unix DGRAM socket + sd-bus + signalfd.
 *  - Audio: non-blocking async paplay/pw-play child reaped via SIGCHLD.
 */

#include "hud_render.h"

#include "fractional-scale-v1-client-protocol.h"
#include "single-pixel-buffer-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#define EX_CONFIG_NO_LAYER_SHELL 78

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ── SHM Buffer Management ───────────────────────────────────────────────── */

typedef struct HudApp HudApp;

typedef struct {
    HudApp           *owner;
    struct wl_buffer *wl_buf;
    void             *data;
    size_t            size;
    int               w;
    int               h;
    bool              busy;
    bool              retire_on_release;
} ShmBuf;

static int create_shm_fd(size_t size) {
    int fd = memfd_create("pressmatch-hud-shm", MFD_CLOEXEC);
    if (fd < 0) {
        char tmpl[] = "/dev/shm/pressmatch-hud-XXXXXX";
        fd = mkostemp(tmpl, O_CLOEXEC);
        if (fd >= 0) unlink(tmpl);
    }
    if (fd < 0) return -1;
    while (ftruncate(fd, (off_t)size) < 0) {
        if (errno != EINTR) {
            close(fd);
            return -1;
        }
    }
    return fd;
}

static void shm_buf_destroy(ShmBuf *b) {
    if (b->wl_buf) {
        wl_buffer_destroy(b->wl_buf);
        b->wl_buf = NULL;
    }
    if (b->data && b->data != MAP_FAILED) {
        munmap(b->data, b->size);
        b->data = NULL;
    }
    b->size = 0;
    b->w = b->h = 0;
    b->busy = false;
    b->retire_on_release = false;
}

struct HudApp {
    /* Wayland core + extensions */
    struct wl_display                       *display;
    struct wl_registry                      *registry;
    struct wl_compositor                    *compositor;
    struct wl_shm                           *shm;
    struct zwlr_layer_shell_v1              *layer_shell;
    struct wp_viewporter                    *viewporter;
    struct wp_fractional_scale_manager_v1   *frac_mgr;
    struct wp_single_pixel_buffer_manager_v1 *spb_mgr;

    struct wl_surface                       *surface;
    struct zwlr_layer_surface_v1            *layer_surface;
    struct wp_viewport                      *viewport;
    struct wp_fractional_scale_v1           *frac_scale;
    struct wl_callback                      *frame_cb;

    bool     configured;
    bool     closed;
    bool     wl_error;
    int      output_count;
    uint32_t output_names[16];
    uint32_t scale_120;      /* 120 = 1.0x, 180 = 1.5x, 240 = 2.0x */
    int      phys_sz;        /* ceil(HUD_LOGICAL_SZ * scale_120 / 120) */

    /* Idle 1x1 transparent buffer (independent storage) + 2 active SHM buffers */
    struct wl_buffer *idle_spb;
    ShmBuf            idle_shm;
    ShmBuf            active[2];
    bool              attached_idle;
    bool              need_repaint;

    /* Renderer */
    HudRenderer *renderer;

    /* State machine */
    HudState    state;
    HudHintKind hint_kind;
    char        label[128];

    double L;
    double drive_from;
    double drive_to;
    double drive_t0;
    double drive_dur;
    bool   driving;
    bool   hiding;
    double content_alpha;

    double phase;
    double last_tick_t;
    double state_t0;
    double glint_t0;

    bool   screen_locked;
    bool   shadow_locked;
    bool   awaiting_unlock_dwell;
    bool   animating;

    /* Deadlines (monotonic seconds; <= 0 means inactive) */
    double dismiss_at;
    double revert_at;
    double watchdog_at;

    /* Sockets & D-Bus & Audio */
    int     sock_fd;
    char    sock_path[108];
    int     sig_fd;
    sd_bus *bus;
    pid_t   chime_pid;
    char    chime_wav[PATH_MAX];
    char    chime_prog[64];

    /* Demo mode */
    bool   demo_mode;
    int    demo_step;
    double demo_next_at;
    bool   running;
};

static void request_repaint(HudApp *app);
static void maybe_commit_frame(HudApp *app, bool immediate);
static void destroy_layer_surface(HudApp *app);
static bool create_layer_surface(HudApp *app);

static void on_buffer_release(void *data, struct wl_buffer *wl_buf) {
    ShmBuf *b = data;
    HudApp *app = b->owner;
    b->busy = false;
    if (b->retire_on_release) {
        shm_buf_destroy(b);
    }
    if (app && app->need_repaint && !app->frame_cb) {
        maybe_commit_frame(app, !app->animating);
    }
}

static const struct wl_buffer_listener g_buf_listener = {
    .release = on_buffer_release,
};

static bool shm_buf_alloc(HudApp *owner, struct wl_shm *shm, ShmBuf *b, int w, int h) {
    shm_buf_destroy(b);
    int stride = w * 4;
    size_t size = (size_t)stride * (size_t)h;
    int fd = create_shm_fd(size);
    if (fd < 0) return false;

    void *mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        close(fd);
        return false;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int)size);
    close(fd);
    if (!pool) {
        munmap(mem, size);
        return false;
    }
    struct wl_buffer *buf = wl_shm_pool_create_buffer(
        pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    if (!buf) {
        munmap(mem, size);
        return false;
    }
    b->owner = owner;
    wl_buffer_add_listener(buf, &g_buf_listener, b);
    b->wl_buf = buf;
    b->data = mem;
    b->size = size;
    b->w = w;
    b->h = h;
    b->busy = false;
    b->retire_on_release = false;
    return true;
}

static void retire_active_buffers(HudApp *app) {
    for (int i = 0; i < 2; i++) {
        if (!app->active[i].wl_buf) continue;
        if (app->active[i].busy)
            app->active[i].retire_on_release = true;
        else
            shm_buf_destroy(&app->active[i]);
    }
}

static void attach_idle_transparent(HudApp *app) {
    if (!app->configured || !app->surface) return;
    struct wl_buffer *buf = NULL;
    int dmg_sz = 1;

    if (app->viewport) {
        buf = app->idle_spb;
        if (!buf) {
            if (!app->idle_shm.wl_buf) {
                if (!shm_buf_alloc(app, app->shm, &app->idle_shm, 1, 1)) return;
                memset(app->idle_shm.data, 0, 4);
            }
            buf = app->idle_shm.wl_buf;
        }
        wp_viewport_set_source(app->viewport,
            wl_fixed_from_int(0), wl_fixed_from_int(0),
            wl_fixed_from_int(1), wl_fixed_from_int(1));
        wp_viewport_set_destination(app->viewport, HUD_LOGICAL_SZ, HUD_LOGICAL_SZ);
    } else {
        /* Compositor lacks wp_viewporter: match full configured logical size */
        dmg_sz = HUD_LOGICAL_SZ;
        if (!app->idle_shm.wl_buf || app->idle_shm.w != HUD_LOGICAL_SZ) {
            if (!shm_buf_alloc(app, app->shm, &app->idle_shm, HUD_LOGICAL_SZ, HUD_LOGICAL_SZ))
                return;
            memset(app->idle_shm.data, 0, app->idle_shm.size);
        }
        buf = app->idle_shm.wl_buf;
    }

    wl_surface_set_buffer_scale(app->surface, 1);
    wl_surface_attach(app->surface, buf, 0, 0);
    wl_surface_damage_buffer(app->surface, 0, 0, dmg_sz, dmg_sz);
    wl_surface_commit(app->surface);
    app->attached_idle = true;
    retire_active_buffers(app);
}

/* ── Wayland Callbacks ───────────────────────────────────────────────────── */

static void on_frame_done(void *data, struct wl_callback *cb, uint32_t time_ms) {
    HudApp *app = data;
    wl_callback_destroy(cb);
    app->frame_cb = NULL;
    if (app->animating || app->need_repaint) {
        maybe_commit_frame(app, false);
    }
}

static const struct wl_callback_listener g_frame_listener = {
    .done = on_frame_done,
};

static void on_layer_configure(void *data, struct zwlr_layer_surface_v1 *ls,
                               uint32_t serial, uint32_t w, uint32_t h) {
    HudApp *app = data;
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    app->configured = true;
    if (app->L < 0.8 && !app->animating) {
        attach_idle_transparent(app);
    } else {
        maybe_commit_frame(app, true);
    }
}

static void on_layer_closed(void *data, struct zwlr_layer_surface_v1 *ls) {
    HudApp *app = data;
    (void)ls;
    destroy_layer_surface(app);
    if (app->output_count > 0) {
        create_layer_surface(app);
    }
}

static const struct zwlr_layer_surface_v1_listener g_layer_listener = {
    .configure = on_layer_configure,
    .closed    = on_layer_closed,
};

static void on_preferred_scale(void *data, struct wp_fractional_scale_v1 *fs,
                               uint32_t scale) {
    HudApp *app = data;
    if (scale < 60) scale = 120;
    if (scale == app->scale_120) return;
    app->scale_120 = scale;
    app->phys_sz = (int)((HUD_LOGICAL_SZ * scale + 60) / 120);
    retire_active_buffers(app);
    if (app->configured && app->L >= 0.8) {
        maybe_commit_frame(app, true);
    }
}

static const struct wp_fractional_scale_v1_listener g_frac_listener = {
    .preferred_scale = on_preferred_scale,
};

static void on_registry_global(void *data, struct wl_registry *reg,
                               uint32_t name, const char *iface, uint32_t ver) {
    HudApp *app = data;
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        app->compositor = wl_registry_bind(reg, name, &wl_compositor_interface, ver >= 4 ? 4 : ver);
    } else if (strcmp(iface, wl_shm_interface.name) == 0) {
        app->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
        app->layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, ver >= 4 ? 4 : ver);
    } else if (strcmp(iface, wp_viewporter_interface.name) == 0) {
        app->viewporter = wl_registry_bind(reg, name, &wp_viewporter_interface, 1);
    } else if (strcmp(iface, wp_fractional_scale_manager_v1_interface.name) == 0) {
        app->frac_mgr = wl_registry_bind(reg, name, &wp_fractional_scale_manager_v1_interface, 1);
    } else if (strcmp(iface, wp_single_pixel_buffer_manager_v1_interface.name) == 0) {
        app->spb_mgr = wl_registry_bind(reg, name, &wp_single_pixel_buffer_manager_v1_interface, 1);
    } else if (strcmp(iface, wl_output_interface.name) == 0) {
        if (app->output_count < 16)
            app->output_names[app->output_count++] = name;
        if (app->layer_shell && !app->layer_surface)
            create_layer_surface(app);
    }
}

static void on_registry_remove(void *data, struct wl_registry *reg, uint32_t name) {
    HudApp *app = data;
    (void)reg;
    for (int i = 0; i < app->output_count; i++) {
        if (app->output_names[i] == name) {
            app->output_names[i] = app->output_names[--app->output_count];
            break;
        }
    }
}

static const struct wl_registry_listener g_registry_listener = {
    .global        = on_registry_global,
    .global_remove = on_registry_remove,
};

/* ── Audio Chime (non-blocking async paplay/pw-play) ─────────────────────── */

static void resolve_chime_assets(HudApp *app) {
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *dir = dirname(exe); /* .../pressmatch-gui/hud-c */
        snprintf(app->chime_wav, sizeof(app->chime_wav), "%s/../sounds/fingerprint-success.wav", dir);
        if (access(app->chime_wav, R_OK) != 0) {
            snprintf(app->chime_wav, sizeof(app->chime_wav), "%s/sounds/fingerprint-success.wav", dir);
        }
    }
    if (access("/usr/bin/paplay", X_OK) == 0)
        snprintf(app->chime_prog, sizeof(app->chime_prog), "paplay");
    else if (access("/usr/bin/pw-play", X_OK) == 0)
        snprintf(app->chime_prog, sizeof(app->chime_prog), "pw-play");
}

static void play_chime_async(HudApp *app) {
    if (app->chime_pid > 0) return; /* skip overlap */
    if (!app->chime_prog[0] || access(app->chime_wav, R_OK) != 0) return;

    pid_t pid = fork();
    if (pid == 0) {
        /* Restore default signal mask in child and silence stdio */
        sigset_t empty;
        sigemptyset(&empty);
        sigprocmask(SIG_SETMASK, &empty, NULL);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        if (strcmp(app->chime_prog, "paplay") == 0) {
            execlp("paplay", "paplay", "--client-name=PressMatch",
                   "--property=media.role=event", app->chime_wav, (char *)NULL);
        } else {
            execlp("pw-play", "pw-play", "--media-role=Notification",
                   app->chime_wav, (char *)NULL);
        }
        _exit(127);
    } else if (pid > 0) {
        app->chime_pid = pid;
    }
}

/* ── Animation & State Machine ───────────────────────────────────────────── */

static void start_drive(HudApp *app, double target, double dur) {
    app->drive_from = app->L;
    app->drive_to   = target;
    app->drive_t0   = now_sec();
    app->drive_dur  = dur;
    app->driving    = true;
    app->animating  = true;
    app->last_tick_t = app->drive_t0;
    request_repaint(app);
}

static void step_animation(HudApp *app, double now) {
    if (app->last_tick_t <= 0.0) app->last_tick_t = now;
    double step_dt = now - app->last_tick_t;
    if (step_dt < 0.0) step_dt = 0.0;
    if (step_dt > 0.1) step_dt = 0.016;
    app->last_tick_t = now;
    app->phase += step_dt;

    if (app->driving) {
        double t = (now - app->drive_t0) / app->drive_dur;
        if (t > 1.0) t = 1.0;
        if (t < 0.0) t = 0.0;
        bool extending = app->drive_to > app->drive_from;
        double e;
        if (extending) {
            if (t >= 1.0) {
                e = 1.0;
            } else {
                const double w = 11.2, z = 0.64;
                double wd = w * sqrt(1.0 - z * z);
                e = 1.0 - exp(-z * w * t) *
                    (cos(wd * t) + (z / sqrt(1.0 - z * z)) * sin(wd * t));
            }
            app->content_alpha = fmax(0.0, fmin(1.0, (t - 0.35) / 0.45));
        } else {
            e = t * t * t;
            app->content_alpha = fmax(0.0, fmin(1.0, 1.0 - t * 2.2));
        }
        app->L = app->drive_from + (app->drive_to - app->drive_from) * e;
        if (t >= 1.0) {
            app->driving = false;
            app->L = app->drive_to;
            if (extending) {
                app->content_alpha = 1.0;
                app->glint_t0 = now;
            } else if (app->hiding && app->L <= 0.5) {
                app->state = HUD_STATE_HIDDEN;
                app->hiding = false;
                app->L = 0.0;
                app->content_alpha = 0.0;
                app->animating = false;
            }
        }
    } else if (!app->hiding && app->L >= HUD_L_MAX * 0.95) {
        app->content_alpha = fmin(1.0, app->content_alpha + step_dt * 5.0);
    }
}

static void maybe_commit_frame(HudApp *app, bool immediate) {
    if (!app->configured || !app->surface || app->closed) return;

    double now = now_sec();
    if (app->animating) {
        step_animation(app, now);
    }

    if (app->L < 0.8 && !app->animating) {
        app->need_repaint = false;
        if (!app->attached_idle) {
            attach_idle_transparent(app);
        }
        return;
    }

    /* Pick a non-busy active SHM buffer (max 2 buffers) */
    int sz = app->phys_sz > 0 ? app->phys_sz : HUD_LOGICAL_SZ;
    ShmBuf *b = NULL;
    for (int i = 0; i < 2; i++) {
        if (!app->active[i].busy) {
            b = &app->active[i];
            break;
        }
    }
    if (!b) {
        /* Both buffers busy: wait for wl_buffer.release / frame callback */
        app->need_repaint = true;
        return;
    }
    if (!b->wl_buf || b->w != sz || b->h != sz) {
        if (!shm_buf_alloc(app, app->shm, b, sz, sz)) return;
    }

    cairo_surface_t *cs = cairo_image_surface_create_for_data(
        (unsigned char *)b->data, CAIRO_FORMAT_ARGB32, sz, sz, sz * 4);
    cairo_t *cr = cairo_create(cs);

    HudRenderState rs = {
        .state         = app->state,
        .hint_kind     = app->hint_kind,
        .L             = app->L,
        .content_alpha = app->content_alpha,
        .phase         = app->phase,
        .state_dt      = fmax(0.0, now - app->state_t0),
        .glint_dt      = (app->glint_t0 > 0.0) ? (now - app->glint_t0) : -1.0,
        .glint_dur     = 0.92,
    };
    snprintf(rs.label, sizeof(rs.label), "%s", app->label);

    hud_render_frame(app->renderer, cr, sz, sz, &rs);
    cairo_destroy(cr);
    cairo_surface_destroy(cs);

    /* Attach with fixed 388x388 logical viewport destination -> zero resize handshake */
    if (app->viewport) {
        wp_viewport_set_source(app->viewport,
            wl_fixed_from_int(0), wl_fixed_from_int(0),
            wl_fixed_from_int(sz), wl_fixed_from_int(sz));
        wp_viewport_set_destination(app->viewport, HUD_LOGICAL_SZ, HUD_LOGICAL_SZ);
    }
    wl_surface_set_buffer_scale(app->surface, 1);
    wl_surface_attach(app->surface, b->wl_buf, 0, 0);
    wl_surface_damage_buffer(app->surface, 0, 0, sz, sz);
    b->busy = true;
    app->attached_idle = false;
    app->need_repaint = false;

    /* Pace ongoing visible animation with compositor VSync frame callbacks */
    (void)immediate;
    if (app->animating && !app->frame_cb) {
        app->frame_cb = wl_surface_frame(app->surface);
        wl_callback_add_listener(app->frame_cb, &g_frame_listener, app);
    }
    wl_surface_commit(app->surface);
}

static void request_repaint(HudApp *app) {
    app->need_repaint = true;
    if (!app->frame_cb) {
        maybe_commit_frame(app, false);
    }
}

static void stage_behind_lockscreen(HudApp *app) {
    /* Pre-commit L_MAX frame behind KWin's lock layer WITHOUT waiting for a
     * wl_surface.frame callback (compositors may withhold callbacks when obscured)
     * and WITHOUT running the animation loop (0% idle CPU while locked). */
    app->shadow_locked = true;
    app->hiding = false;
    app->driving = false;
    app->animating = false;
    app->L = HUD_L_MAX;
    app->content_alpha = 1.0;
    if (app->frame_cb) {
        wl_callback_destroy(app->frame_cb);
        app->frame_cb = NULL;
    }
    maybe_commit_frame(app, true);
}

static void show_smooth(HudApp *app, bool instant) {
    app->shadow_locked = false;
    app->hiding = false;
    bool already_opening = app->driving && app->drive_to >= HUD_L_MAX * 0.9;
    bool already_open    = app->L >= HUD_L_MAX * 0.95 && !app->driving;
    if (instant) {
        app->driving = false;
        app->L = HUD_L_MAX;
        app->content_alpha = 1.0;
        app->animating = true;
        app->last_tick_t = now_sec();
        maybe_commit_frame(app, true);
        return;
    }
    app->animating = true;
    app->last_tick_t = now_sec();
    if (!already_open && !already_opening) {
        start_drive(app, HUD_L_MAX, 0.72);
    } else {
        request_repaint(app);
    }
}

static void hide_smooth(HudApp *app) {
    app->shadow_locked = false;
    app->awaiting_unlock_dwell = false;
    app->dismiss_at  = 0.0;
    app->revert_at   = 0.0;
    app->watchdog_at = 0.0;
    if (app->L <= 0.5 || app->hiding) return;
    app->hiding = true;
    start_drive(app, 0.0, 0.42);
}

static void revert_to_armed(HudApp *app) {
    if (app->state == HUD_STATE_RETRY || app->state == HUD_STATE_PARTIAL) {
        app->state = HUD_STATE_ARMED;
        app->hint_kind = HUD_HINT_NONE;
        app->state_t0 = now_sec();
        snprintf(app->label, sizeof(app->label), "%s", hud_tr("armed"));
        request_repaint(app);
    }
}

/* ── Minimal Safe JSON Field Extractor (PAM emits flat JSON objects) ─────── */

static bool json_get_string(const char *json, const char *key, char *out, size_t cap) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (*p != '"') return false;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'n') out[n++] = ' ';
            else out[n++] = *p;
            p++;
        } else {
            out[n++] = *p++;
        }
    }
    out[n] = '\0';
    return true;
}

static int json_get_int(const char *json, const char *key, int fallback) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return fallback;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    return (end != p) ? (int)v : fallback;
}

static void handle_pam_event(HudApp *app, const char *json) {
    char state[32] = {0}, service[64] = {0}, hint[128] = {0};
    if (!json_get_string(json, "state", state, sizeof(state))) return;
    json_get_string(json, "service", service, sizeof(service));
    json_get_string(json, "hint", hint, sizeof(hint));

    /* Explicit match on kde-fingerprint so sudo-pressmatch-fingerprint takes desktop path */
    bool is_lockscreen = (strcmp(service, "kde-fingerprint") == 0);
    double now = now_sec();

    app->awaiting_unlock_dwell = false;
    app->dismiss_at = 0.0;
    app->revert_at  = 0.0;

    if (is_lockscreen && strcmp(state, "match") != 0) {
        if (strcmp(state, "armed") == 0) {
            app->state = HUD_STATE_ARMED;
            app->hint_kind = HUD_HINT_NONE;
            app->state_t0 = now;
            app->watchdog_at = 0.0;
            snprintf(app->label, sizeof(app->label), "%s", hud_tr("armed"));
            stage_behind_lockscreen(app);
        } else if (strcmp(state, "scanning") == 0) {
            app->state = HUD_STATE_SCANNING;
            app->hint_kind = HUD_HINT_NONE;
            app->state_t0 = now;
            snprintf(app->label, sizeof(app->label), "%s", hud_tr("scanning"));
            stage_behind_lockscreen(app);
        } else if (strcmp(state, "partial") == 0) {
            app->state = HUD_STATE_PARTIAL;
            app->hint_kind = hud_classify_hint(hint, HUD_STATE_PARTIAL);
            app->state_t0 = now;
            const char *hl = hud_hint_tr(app->hint_kind);
            snprintf(app->label, sizeof(app->label), "%s",
                     hl[0] ? hl : (hint[0] ? hint : hud_tr("partial")));
            stage_behind_lockscreen(app);
        } else if (strcmp(state, "retry") == 0) {
            app->state = HUD_STATE_RETRY;
            app->hint_kind = hud_classify_hint(hint, HUD_STATE_RETRY);
            app->state_t0 = now;
            const char *hl = hud_hint_tr(app->hint_kind);
            snprintf(app->label, sizeof(app->label), "%s",
                     hl[0] ? hl : (hint[0] ? hint : hud_tr("retry")));
            stage_behind_lockscreen(app);
        } else if (strcmp(state, "declined") == 0) {
            app->state = HUD_STATE_DECLINED;
            app->hint_kind = HUD_HINT_NONE;
            app->state_t0 = now;
            snprintf(app->label, sizeof(app->label), "%s", hud_tr("declined"));
            stage_behind_lockscreen(app);
        } else if (strcmp(state, "ended") == 0) {
            if (!app->screen_locked && app->shadow_locked) {
                hide_smooth(app);
            }
        }
        return;
    }

    if (strcmp(state, "armed") == 0) {
        app->state = HUD_STATE_ARMED;
        app->hint_kind = HUD_HINT_NONE;
        app->state_t0 = now;
        snprintf(app->label, sizeof(app->label), "%s", hud_tr("armed"));
        app->watchdog_at = now + 600.0;
        show_smooth(app, false);
    } else if (strcmp(state, "scanning") == 0) {
        app->state = HUD_STATE_SCANNING;
        app->hint_kind = HUD_HINT_NONE;
        app->state_t0 = now;
        snprintf(app->label, sizeof(app->label), "%s", hud_tr("scanning"));
        app->watchdog_at = now + 10.0;
        show_smooth(app, false);
    } else if (strcmp(state, "match") == 0) {
        app->state = HUD_STATE_MATCH;
        app->hint_kind = HUD_HINT_NONE;
        app->state_t0 = now;
        app->glint_t0 = now;
        app->watchdog_at = 0.0;
        snprintf(app->label, sizeof(app->label), "%s",
                 hud_tr(is_lockscreen ? "unlocked" : "match"));
        show_smooth(app, is_lockscreen);
        play_chime_async(app);
        if (is_lockscreen && app->screen_locked) {
            app->awaiting_unlock_dwell = true;
            app->dismiss_at = now + 4.0; /* safety cap */
        } else {
            app->dismiss_at = now + (is_lockscreen ? 0.95 : 1.10);
        }
    } else if (strcmp(state, "partial") == 0) {
        app->state = HUD_STATE_PARTIAL;
        app->hint_kind = hud_classify_hint(hint, HUD_STATE_PARTIAL);
        app->state_t0 = now;
        const char *hl = hud_hint_tr(app->hint_kind);
        snprintf(app->label, sizeof(app->label), "%s",
                 hl[0] ? hl : (hint[0] ? hint : hud_tr("partial")));
        show_smooth(app, false);
        app->revert_at = now + 3.2;
    } else if (strcmp(state, "retry") == 0) {
        int tries = json_get_int(json, "tries", 1);
        int max_tries = json_get_int(json, "max_tries", 5);
        app->state = HUD_STATE_RETRY;
        app->hint_kind = hud_classify_hint(hint, HUD_STATE_RETRY);
        app->state_t0 = now;
        const char *hl = hud_hint_tr(app->hint_kind);
        if (hl[0])
            snprintf(app->label, sizeof(app->label), "%s", hl);
        else if (hint[0])
            snprintf(app->label, sizeof(app->label), "%s", hint);
        else
            snprintf(app->label, sizeof(app->label), "%s (%d/%d)", hud_tr("retry"), tries, max_tries);
        show_smooth(app, false);
        if (tries < max_tries)
            app->revert_at = now + (hint[0] ? 3.2 : 1.5);
        else
            app->dismiss_at = now + 1.1;
    } else if (strcmp(state, "declined") == 0) {
        app->state = HUD_STATE_DECLINED;
        app->hint_kind = HUD_HINT_NONE;
        app->state_t0 = now;
        snprintf(app->label, sizeof(app->label), "%s", hud_tr("declined"));
        show_smooth(app, false);
        app->dismiss_at = now + 2.6;
    } else if (strcmp(state, "ended") == 0) {
        if (app->state != HUD_STATE_MATCH &&
            app->state != HUD_STATE_RETRY &&
            app->state != HUD_STATE_DECLINED) {
            hide_smooth(app);
        }
    }
}

/* ── sd-bus ScreenSaver Tracking ─────────────────────────────────────────── */

static void apply_screensaver_active(HudApp *app, bool active) {
    app->screen_locked = active;
    if (!active) {
        if (app->shadow_locked) {
            hide_smooth(app);
        } else if (app->awaiting_unlock_dwell && app->state == HUD_STATE_MATCH) {
            app->awaiting_unlock_dwell = false;
            app->dismiss_at = now_sec() + 0.95;
        }
    }
}

static int on_screensaver_signal(sd_bus_message *m, void *userdata, sd_bus_error *err) {
    HudApp *app = userdata;
    int active = 0;
    if (sd_bus_message_read(m, "b", &active) >= 0) {
        apply_screensaver_active(app, active != 0);
    }
    return 0;
}

static int on_get_active_reply(sd_bus_message *m, void *userdata, sd_bus_error *err) {
    HudApp *app = userdata;
    if (sd_bus_message_is_method_error(m, NULL)) return 0;
    int active = 0;
    if (sd_bus_message_read(m, "b", &active) >= 0) {
        apply_screensaver_active(app, active != 0);
    }
    return 0;
}

static void query_screensaver_async(HudApp *app) {
    if (!app->bus) return;
    sd_bus_call_method_async(
        app->bus, NULL,
        "org.freedesktop.ScreenSaver", "/ScreenSaver",
        "org.freedesktop.ScreenSaver", "GetActive",
        on_get_active_reply, app, "");
}

static int on_name_owner_changed(sd_bus_message *m, void *userdata, sd_bus_error *err) {
    HudApp *app = userdata;
    const char *name = NULL, *old_o = NULL, *new_o = NULL;
    if (sd_bus_message_read(m, "sss", &name, &old_o, &new_o) >= 0 &&
        name && strcmp(name, "org.freedesktop.ScreenSaver") == 0 && new_o && new_o[0]) {
        query_screensaver_async(app);
    }
    return 0;
}

static void init_sdbus(HudApp *app) {
    if (sd_bus_open_user(&app->bus) < 0) {
        app->bus = NULL;
        return;
    }
    sd_bus_match_signal(app->bus, NULL,
        "org.freedesktop.ScreenSaver", "/ScreenSaver",
        "org.freedesktop.ScreenSaver", "ActiveChanged",
        on_screensaver_signal, app);
    sd_bus_add_match(app->bus, NULL,
        "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
        "interface='org.freedesktop.DBus',member='NameOwnerChanged',"
        "arg0='org.freedesktop.ScreenSaver'",
        on_name_owner_changed, app);
    query_screensaver_async(app);
}

/* ── Unix DGRAM Socket ───────────────────────────────────────────────────── */

static bool bind_hud_socket(HudApp *app) {
    const char *run_dir = getenv("XDG_RUNTIME_DIR");
    char fallback[64];
    if (!run_dir || !run_dir[0]) {
        snprintf(fallback, sizeof(fallback), "/run/user/%u", (unsigned)getuid());
        run_dir = fallback;
    }
    snprintf(app->sock_path, sizeof(app->sock_path), "%s/pressmatch-hud.sock", run_dir);
    unlink(app->sock_path);

    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", app->sock_path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return false;
    }
    chmod(app->sock_path, 0622); /* allow root PAM helpers (polkit/sudo) to sendto */
    app->sock_fd = fd;
    return true;
}

/* ── Demo Mode Sequencer ─────────────────────────────────────────────────── */

static void step_demo(HudApp *app, double now) {
    static const struct { double dt; const char *json; } steps[] = {
        {0.10, "{\"state\":\"armed\",\"service\":\"polkit-1\",\"readers\":[\"ELAN\",\"FT9201\"]}"},
        {1.40, "{\"state\":\"partial\",\"service\":\"polkit-1\",\"hint\":\"Press firmer, cover the sensor\"}"},
        {1.70, "{\"state\":\"retry\",\"service\":\"polkit-1\",\"tries\":1,\"max_tries\":5,\"hint\":\"Place it like during setup\"}"},
        {1.80, "{\"state\":\"scanning\",\"service\":\"polkit-1\",\"device\":\"ELAN\"}"},
        {0.90, "{\"state\":\"match\",\"service\":\"polkit-1\",\"device\":\"ELAN\",\"ms\":235}"},
        {1.80, NULL},
    };
    const int n = (int)(sizeof(steps) / sizeof(steps[0]));
    if (app->demo_step >= n) return;
    if (now < app->demo_next_at) return;

    const char *ev = steps[app->demo_step].json;
    app->demo_step++;
    if (!ev) {
        app->running = false;
        return;
    }
    handle_pam_event(app, ev);
    if (app->demo_step < n) {
        app->demo_next_at = now + steps[app->demo_step].dt;
    }
}

/* ── Wayland Initialization ──────────────────────────────────────────────── */

static void destroy_layer_surface(HudApp *app) {
    if (app->frame_cb) {
        wl_callback_destroy(app->frame_cb);
        app->frame_cb = NULL;
    }
    retire_active_buffers(app);
    if (app->layer_surface) {
        zwlr_layer_surface_v1_destroy(app->layer_surface);
        app->layer_surface = NULL;
    }
    if (app->frac_scale) {
        wp_fractional_scale_v1_destroy(app->frac_scale);
        app->frac_scale = NULL;
    }
    if (app->viewport) {
        wp_viewport_destroy(app->viewport);
        app->viewport = NULL;
    }
    if (app->surface) {
        wl_surface_destroy(app->surface);
        app->surface = NULL;
    }
    app->configured = false;
    app->attached_idle = false;
}

static bool create_layer_surface(HudApp *app) {
    if (!app->compositor || !app->layer_shell || app->layer_surface)
        return app->layer_surface != NULL;

    app->surface = wl_compositor_create_surface(app->compositor);
    struct wl_region *empty = wl_compositor_create_region(app->compositor);
    wl_surface_set_input_region(app->surface, empty);
    wl_region_destroy(empty);

    if (app->viewporter) {
        app->viewport = wp_viewporter_get_viewport(app->viewporter, app->surface);
        wp_viewport_set_destination(app->viewport, HUD_LOGICAL_SZ, HUD_LOGICAL_SZ);
    }
    if (app->frac_mgr) {
        app->frac_scale = wp_fractional_scale_manager_v1_get_fractional_scale(
            app->frac_mgr, app->surface);
        wp_fractional_scale_v1_add_listener(app->frac_scale, &g_frac_listener, app);
    }

    app->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        app->layer_shell, app->surface, NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "pressmatch-hud");
    zwlr_layer_surface_v1_set_size(app->layer_surface, HUD_LOGICAL_SZ, HUD_LOGICAL_SZ);
    zwlr_layer_surface_v1_set_anchor(app->layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(app->layer_surface, -1);
    zwlr_layer_surface_v1_set_margin(app->layer_surface, 0, 0, 0, 0);
    zwlr_layer_surface_v1_set_keyboard_interactivity(app->layer_surface,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    zwlr_layer_surface_v1_add_listener(app->layer_surface, &g_layer_listener, app);

    wl_surface_commit(app->surface);
    return true;
}

static bool init_wayland(HudApp *app) {
    app->display = wl_display_connect(NULL);
    if (!app->display) {
        fprintf(stderr, "pressmatch-hud: cannot connect to Wayland display\n");
        return false;
    }
    app->registry = wl_display_get_registry(app->display);
    wl_registry_add_listener(app->registry, &g_registry_listener, app);
    wl_display_roundtrip(app->display);

    if (!app->compositor || !app->shm) {
        fprintf(stderr, "pressmatch-hud: missing wl_compositor or wl_shm\n");
        return false;
    }
    if (!app->layer_shell) {
        fprintf(stderr, "pressmatch-hud: compositor does not advertise zwlr_layer_shell_v1\n");
        return false;
    }

    app->scale_120 = 120;
    app->phys_sz   = HUD_LOGICAL_SZ;

    if (app->spb_mgr) {
        app->idle_spb = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(
            app->spb_mgr, 0, 0, 0, 0);
    }

    create_layer_surface(app);
    wl_display_roundtrip(app->display);
    return true;
}

static void cleanup_app(HudApp *app) {
    if (app->frame_cb) wl_callback_destroy(app->frame_cb);
    shm_buf_destroy(&app->active[0]);
    shm_buf_destroy(&app->active[1]);
    shm_buf_destroy(&app->idle_shm);
    if (app->idle_spb)      wl_buffer_destroy(app->idle_spb);
    if (app->layer_surface) zwlr_layer_surface_v1_destroy(app->layer_surface);
    if (app->frac_scale)    wp_fractional_scale_v1_destroy(app->frac_scale);
    if (app->viewport)      wp_viewport_destroy(app->viewport);
    if (app->surface)       wl_surface_destroy(app->surface);
    if (app->spb_mgr)       wp_single_pixel_buffer_manager_v1_destroy(app->spb_mgr);
    if (app->frac_mgr)      wp_fractional_scale_manager_v1_destroy(app->frac_mgr);
    if (app->viewporter)    wp_viewporter_destroy(app->viewporter);
    if (app->layer_shell)   zwlr_layer_shell_v1_destroy(app->layer_shell);
    if (app->shm)           wl_shm_destroy(app->shm);
    if (app->compositor)    wl_compositor_destroy(app->compositor);
    if (app->registry)      wl_registry_destroy(app->registry);
    if (app->display)       wl_display_disconnect(app->display);

    if (app->bus) sd_bus_unref(app->bus);
    if (app->sock_fd >= 0) {
        close(app->sock_fd);
        if (app->sock_path[0]) unlink(app->sock_path);
    }
    if (app->sig_fd >= 0) close(app->sig_fd);
    hud_renderer_destroy(app->renderer);
}

static int compute_poll_timeout_ms(const HudApp *app, double now) {
    double soonest = -1.0;
    const double deadlines[4] = {
        app->dismiss_at,
        app->revert_at,
        app->watchdog_at,
        app->demo_mode ? app->demo_next_at : 0.0,
    };
    for (int i = 0; i < 4; i++) {
        if (deadlines[i] > 0.0) {
            double rem = deadlines[i] - now;
            if (rem < 0.0) rem = 0.0;
            if (soonest < 0.0 || rem < soonest) soonest = rem;
        }
    }
    if (app->bus) {
        uint64_t usec = 0;
        if (sd_bus_get_timeout(app->bus, &usec) >= 0 && usec != UINT64_MAX) {
            double rem = (double)usec * 1e-6 - now;
            if (rem < 0.0) rem = 0.0;
            if (soonest < 0.0 || rem < soonest) soonest = rem;
        }
    }
    if (soonest < 0.0) return -1; /* infinite sleep when idle -> 0% CPU, 0 wakeups */
    int ms = (int)ceil(soonest * 1000.0);
    return ms < 0 ? 0 : ms;
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "--render-all") == 0) {
        return hud_render_all_previews(argv[2]);
    }

    HudApp app = {0};
    app.sock_fd = -1;
    app.sig_fd  = -1;
    app.running = true;
    app.glint_t0 = -10.0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--demo") == 0)
            app.demo_mode = true;
    }

    app.renderer = hud_renderer_create();
    if (!app.renderer) return 1;
    resolve_chime_assets(&app);

    /* Block SIGTERM/SIGINT/SIGCHLD and handle them via signalfd (zero 500ms timer) */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    app.sig_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);

    if (!init_wayland(&app)) {
        bool no_ls = (app.display && !app.layer_shell);
        cleanup_app(&app);
        return no_ls ? EX_CONFIG_NO_LAYER_SHELL : 1;
    }

    if (!app.demo_mode) {
        if (!bind_hud_socket(&app)) {
            fprintf(stderr, "pressmatch-hud: cannot bind %s: %s\n",
                    app.sock_path, strerror(errno));
            cleanup_app(&app);
            return 1;
        }
        init_sdbus(&app);
    } else {
        app.demo_next_at = now_sec() + 0.10;
    }

    int wl_fd = wl_display_get_fd(app.display);

    while (app.running && !app.closed) {
        /* 1. Dispatch any pending Wayland & sd-bus work before polling */
        while (wl_display_prepare_read(app.display) != 0) {
            wl_display_dispatch_pending(app.display);
        }
        if (app.bus) {
            while (sd_bus_process(app.bus, NULL) > 0) {}
        }

        /* 2. Check timers & demo sequencer */
        double now = now_sec();
        if (app.demo_mode) step_demo(&app, now);
        if (app.dismiss_at > 0.0 && now >= app.dismiss_at) {
            app.dismiss_at = 0.0;
            hide_smooth(&app);
        }
        if (app.revert_at > 0.0 && now >= app.revert_at) {
            app.revert_at = 0.0;
            revert_to_armed(&app);
        }
        if (app.watchdog_at > 0.0 && now >= app.watchdog_at) {
            app.watchdog_at = 0.0;
            hide_smooth(&app);
        }
        if (!app.running || app.closed) {
            wl_display_cancel_read(app.display);
            break;
        }

        /* 3. Flush Wayland writes (monitor POLLOUT if EAGAIN) */
        bool need_wl_out = false;
        if (wl_display_flush(app.display) < 0) {
            if (errno == EAGAIN) {
                need_wl_out = true;
            } else {
                app.wl_error = true;
                wl_display_cancel_read(app.display);
                break;
            }
        }

        /* 4. Single poll() over Wayland + HUD socket + signalfd + sd-bus */
        struct pollfd fds[4];
        int nfds = 0;
        int idx_wl = nfds++;
        fds[idx_wl].fd     = wl_fd;
        fds[idx_wl].events = POLLIN | (need_wl_out ? POLLOUT : 0);

        int idx_sig = -1;
        if (app.sig_fd >= 0) {
            idx_sig = nfds++;
            fds[idx_sig].fd     = app.sig_fd;
            fds[idx_sig].events = POLLIN;
        }

        int idx_sock = -1;
        if (app.sock_fd >= 0) {
            idx_sock = nfds++;
            fds[idx_sock].fd     = app.sock_fd;
            fds[idx_sock].events = POLLIN;
        }

        int idx_bus = -1;
        if (app.bus) {
            int bfd = sd_bus_get_fd(app.bus);
            if (bfd >= 0) {
                idx_bus = nfds++;
                fds[idx_bus].fd     = bfd;
                fds[idx_bus].events = (short)sd_bus_get_events(app.bus);
            }
        }

        int timeout_ms = compute_poll_timeout_ms(&app, now_sec());
        int pr = poll(fds, (nfds_t)nfds, timeout_ms);
        if (pr < 0) {
            wl_display_cancel_read(app.display);
            if (errno == EINTR) continue;
            break;
        }

        /* 5. Complete or cancel Wayland read */
        if (fds[idx_wl].revents & (POLLERR | POLLHUP)) {
            app.wl_error = true;
            wl_display_cancel_read(app.display);
            break;
        }
        if (fds[idx_wl].revents & POLLIN) {
            if (wl_display_read_events(app.display) < 0) {
                app.wl_error = true;
                break;
            }
            wl_display_dispatch_pending(app.display);
        } else {
            wl_display_cancel_read(app.display);
        }
        if (fds[idx_wl].revents & POLLOUT) {
            wl_display_flush(app.display);
        }

        /* 6. Signals (SIGTERM/SIGINT/SIGCHLD) */
        if (idx_sig >= 0 && (fds[idx_sig].revents & POLLIN)) {
            struct signalfd_siginfo si;
            while (read(app.sig_fd, &si, sizeof(si)) == (ssize_t)sizeof(si)) {
                if (si.ssi_signo == SIGTERM || si.ssi_signo == SIGINT) {
                    app.running = false;
                } else if (si.ssi_signo == SIGCHLD) {
                    int status;
                    while (waitpid(-1, &status, WNOHANG) > 0) {}
                    app.chime_pid = 0;
                }
            }
        }

        /* 7. HUD Unix DGRAM socket */
        if (idx_sock >= 0 && (fds[idx_sock].revents & POLLIN)) {
            char buf[4096];
            for (;;) {
                ssize_t n = recv(app.sock_fd, buf, sizeof(buf) - 1, 0);
                if (n <= 0) break;
                buf[n] = '\0';
                handle_pam_event(&app, buf);
            }
        }

        /* 8. sd-bus */
        if (idx_bus >= 0 && fds[idx_bus].revents) {
            while (sd_bus_process(app.bus, NULL) > 0) {}
        }
    }

    bool had_err = app.wl_error;
    cleanup_app(&app);
    return had_err ? 1 : 0;
}
