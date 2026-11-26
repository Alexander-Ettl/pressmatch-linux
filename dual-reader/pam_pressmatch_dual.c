/*
 * pam_pressmatch_dual — Lid-Aware Simultaneous Multi-Reader PAM Module for fprintd
 *
 * Arms ALL connected fprintd fingerprint readers that have enrolled prints for the user
 * in parallel over a single GDBus main loop. Whichever reader produces the first
 * `verify-match` immediately cancels the others and unlocks.
 *
 * Ergonomic lid awareness:
 *   Reads /proc/acpi/button/lid/LID0/state (or any lid subdir state file).
 *   When the laptop lid is CLOSED, internal built-in sensors (e.g. ElanTech power button)
 *   are automatically skipped so external USB hub readers (e.g. FocalTech FT9201) answer
 *   instantly without wasting USB polls on an unreachable sensor.
 *
 * Build PAM module:
 *   gcc -O2 -shared -fPIC -Wall -Wextra pam_pressmatch_dual.c \
 *       $(pkg-config --cflags --libs gio-2.0) /usr/lib/x86_64-linux-gnu/libpam.so.0 \
 *       -o pam_pressmatch_dual.so
 *
 * Build standalone CLI tester:
 *   gcc -O2 -Wall -Wextra -DDUAL_CLI_TEST pam_pressmatch_dual.c \
 *       $(pkg-config --cflags --libs gio-2.0) -o dual_unlock_test
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib-unix.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <syslog.h>
#include <unistd.h>

/* Standard Linux-PAM ABI declarations (matches libpam.so.0 on x86_64 Ubuntu/Kubuntu) */
#ifndef DUAL_CLI_TEST
typedef struct pam_handle pam_handle_t;
#define PAM_SUCCESS           0
#define PAM_AUTH_ERR          7
#define PAM_AUTHINFO_UNAVAIL  9
#define PAM_IGNORE            25
#define PAM_SERVICE           1
#define PAM_CONV              5
#define PAM_TEXT_INFO         4
#define PAM_ERROR_MSG         3

struct pam_message {
  int msg_style;
  const char *msg;
};
struct pam_response {
  char *resp;
  int resp_retcode;
};
struct pam_conv {
  int (*conv)(int num_msg, const struct pam_message **msg,
              struct pam_response **resp, void *appdata_ptr);
  void *appdata_ptr;
};

extern int pam_get_user (pam_handle_t *pamh, const char **user, const char *prompt);
extern int pam_get_item (const pam_handle_t *pamh, int item_type, const void **item);

static void
send_pam_msg (pam_handle_t *pamh, int style, const char *text)
{
  const void *item = NULL;
  if (!pamh || !text)
    return;
  if (pam_get_item (pamh, PAM_CONV, &item) != PAM_SUCCESS || !item)
    return;
  const struct pam_conv *conv = (const struct pam_conv *) item;
  if (!conv->conv)
    return;
  struct pam_message msg = { .msg_style = style, .msg = text };
  const struct pam_message *msgs[1] = { &msg };
  struct pam_response *resp = NULL;
  conv->conv (1, msgs, &resp, conv->appdata_ptr);
  if (resp)
    {
      free (resp[0].resp);
      free (resp);
    }
}
#endif

static gboolean
is_laptop_lid_closed (void)
{
  DIR *d = opendir ("/proc/acpi/button/lid");
  if (!d)
    return FALSE;
  struct dirent *ent;
  gboolean closed = FALSE;
  while ((ent = readdir (d)) != NULL)
    {
      if (ent->d_name[0] == '.')
        continue;
      g_autofree char *p = g_strdup_printf ("/proc/acpi/button/lid/%s/state", ent->d_name);
      g_autofree char *contents = NULL;
      if (g_file_get_contents (p, &contents, NULL, NULL) && contents)
        {
          if (strstr (contents, "closed") != NULL)
            {
              closed = TRUE;
              break;
            }
        }
    }
  closedir (d);
  return closed;
}

static gboolean
is_internal_builtin_reader (const char *dev_name)
{
  if (!dev_name)
    return FALSE;
  return (strstr (dev_name, "ElanTech") != NULL || strstr (dev_name, "ELAN") != NULL);
}

typedef struct _DualCtx DualCtx;

typedef struct {
  DualCtx    *ctx;
  char       *path;
  char       *name;
  GDBusProxy *proxy;
  gulong      sig_id;
  gulong      prop_sig_id;
  gboolean    claimed;
  gboolean    verifying;
  gboolean    disabled;
} ActiveDev;

struct _DualCtx {
  GMainContext    *mctx;
  GMainLoop       *loop;
  GDBusConnection *bus;
  const char      *username;
  const char      *service;
  uid_t            target_uid;
  gboolean         has_uid;
  gboolean         lid_closed;
  gboolean         builtin_only;
  gboolean         quiet;
  GPtrArray       *devs;       /* ActiveDev* */
  int              max_tries;
  int              tries_used;
  int              result;     /* 0=match, 1=auth_err, 2=unavail/timeout, 3=rearm, 4=cancel */
  char            *winner_name;
  char            *winner_path;
  gint64           t_start_us;
  gdouble          unlock_ms;
  GSource         *timeout_src;
#ifndef DUAL_CLI_TEST
  pam_handle_t    *pamh;
#endif
};

static const char *
short_reader_label (const char *dev_name)
{
  if (!dev_name)
    return "Reader";
  if (strstr (dev_name, "ElanTech") || strstr (dev_name, "ELAN"))
    return "ELAN";
  if (strstr (dev_name, "FT9201") || strstr (dev_name, "Focal"))
    return "FT9201";
  return dev_name;
}

static void
emit_hud_json (const DualCtx *ctx, const char *json)
{
  if (!ctx || !ctx->has_uid || ctx->quiet || !json)
    return;

  int fd = socket (AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return;

  struct sockaddr_un addr;
  memset (&addr, 0, sizeof (addr));
  addr.sun_family = AF_UNIX;
  snprintf (addr.sun_path, sizeof (addr.sun_path),
            "/run/user/%u/pressmatch-hud.sock", (unsigned int) ctx->target_uid);
  (void) sendto (fd, json, strlen (json), MSG_DONTWAIT | MSG_NOSIGNAL,
                 (const struct sockaddr *) &addr, sizeof (addr));
  close (fd);
}

static void
json_append_escaped (GString *out, const char *s)
{
  g_string_append_c (out, '"');
  if (s)
    {
      for (const unsigned char *p = (const unsigned char *) s; *p; p++)
        {
          if (*p == '"' || *p == '\\')
            {
              g_string_append_c (out, '\\');
              g_string_append_c (out, (gchar) *p);
            }
          else if (*p < 0x20)
            {
              g_string_append_printf (out, "\\u%04x", *p);
            }
          else
            {
              g_string_append_c (out, (gchar) *p);
            }
        }
    }
  g_string_append_c (out, '"');
}

static void
emit_hud_armed (const DualCtx *ctx)
{
  if (!ctx || !ctx->has_uid || ctx->quiet)
    return;
  GString *payload = g_string_new ("{\"state\":\"armed\",\"readers\":[");
  gboolean first = TRUE;
  for (guint i = 0; i < ctx->devs->len; i++)
    {
      ActiveDev *ad = g_ptr_array_index (ctx->devs, i);
      if (ad->verifying && !ad->disabled)
        {
          if (!first)
            g_string_append_c (payload, ',');
          json_append_escaped (payload, short_reader_label (ad->name));
          first = FALSE;
        }
    }
  g_string_append (payload, "],\"service\":");
  json_append_escaped (payload, ctx->service ? ctx->service : "pam");
  g_string_append_printf (payload, ",\"max_tries\":%d}", ctx->max_tries);
  emit_hud_json (ctx, payload->str);
  g_string_free (payload, TRUE);
}

static void
on_dev_props_changed (GDBusProxy *proxy,
                      GVariant   *changed_properties,
                      GStrv       invalidated_properties,
                      gpointer    user_data)
{
  (void) proxy;
  (void) invalidated_properties;
  ActiveDev *ad = (ActiveDev *) user_data;
  DualCtx *ctx = ad ? ad->ctx : NULL;
  if (!ctx || !changed_properties || ctx->result == 0)
    return;

  gboolean present = FALSE;
  if (g_variant_lookup (changed_properties, "finger-present", "b", &present) && present)
    {
      const char *label = short_reader_label (ad->name);
      GString *payload = g_string_new ("{\"state\":\"scanning\",\"device\":");
      json_append_escaped (payload, label);
      g_string_append (payload, ",\"service\":");
      json_append_escaped (payload, ctx->service ? ctx->service : "pam");
      g_string_append_c (payload, '}');
      emit_hud_json (ctx, payload->str);
      g_string_free (payload, TRUE);
#ifndef DUAL_CLI_TEST
      if (!ctx->quiet)
        {
          g_autofree char *msg = g_strdup_printf ("Scanning fingerprint on %s…", label);
          send_pam_msg (ctx->pamh, PAM_TEXT_INFO, msg);
        }
#endif
    }
}

static void
pm_log (const char *fmt, ...)
{
  va_list ap;
  va_start (ap, fmt);
  g_autofree char *msg = g_strdup_vprintf (fmt, ap);
  va_end (ap);
#ifdef DUAL_CLI_TEST
  fprintf (stderr, "[dual] %s\n", msg);
  fflush (stderr);
#else
  syslog (LOG_AUTHPRIV | LOG_INFO, "pam_pressmatch_dual: %s", msg);
#endif
}

static gboolean
restart_device_timer (gpointer data)
{
  ActiveDev *ad = (ActiveDev *) data;
  if (!ad || ad->disabled || !ad->claimed || ad->ctx->result == 0)
    return G_SOURCE_REMOVE;

  g_autoptr(GError) err = NULL;
  g_autoptr(GVariant) r = g_dbus_proxy_call_sync (
    ad->proxy, "VerifyStart", g_variant_new ("(s)", "any"),
    G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &err);
  if (r)
    {
      ad->verifying = TRUE;
      pm_log ("re-armed '%s' (%s)", ad->name, ad->path);
    }
  else
    {
      ad->disabled = TRUE;
      pm_log ("re-arm VerifyStart failed on '%s' (%s): %s",
              ad->name, ad->path, err ? err->message : "unknown");
    }
  return G_SOURCE_REMOVE;
}

static void
on_verify_status (GDBusProxy  *proxy,
                  const gchar *sender_name,
                  const gchar *signal_name,
                  GVariant    *parameters,
                  gpointer     user_data)
{
  (void) proxy;
  (void) sender_name;
  ActiveDev *ad = (ActiveDev *) user_data;
  DualCtx *ctx = ad->ctx;

  if (g_strcmp0 (signal_name, "VerifyStatus") != 0)
    return;

  const gchar *status = NULL;
  gboolean done = FALSE;
  g_variant_get (parameters, "(&sb)", &status, &done);

  pm_log ("signal VerifyStatus dev='%s' path=%s status=%s done=%d tries=%d/%d",
          ad->name, ad->path, status ? status : "?", done ? 1 : 0,
          ctx->tries_used, ctx->max_tries);

  if (done)
    ad->verifying = FALSE;

  if (g_strcmp0 (status, "verify-match") == 0)
    {
      ctx->result = 0;
      ctx->winner_name = g_strdup (ad->name);
      ctx->winner_path = g_strdup (ad->path);
      ctx->unlock_ms = (g_get_monotonic_time () - ctx->t_start_us) / 1000.0;
      {
        GString *payload = g_string_new ("{\"state\":\"match\",\"device\":");
        json_append_escaped (payload, short_reader_label (ad->name));
        g_string_append_printf (payload, ",\"ms\":%.0f,\"service\":", ctx->unlock_ms);
        json_append_escaped (payload, ctx->service ? ctx->service : "pam");
        g_string_append_c (payload, '}');
        emit_hud_json (ctx, payload->str);
        g_string_free (payload, TRUE);
      }
      if (g_main_loop_is_running (ctx->loop))
        g_main_loop_quit (ctx->loop);
      return;
    }

  if (g_strcmp0 (status, "verify-no-match") == 0 ||
      g_strcmp0 (status, "verify-retry-scan") == 0 ||
      g_strcmp0 (status, "verify-swipe-too-short") == 0 ||
      g_strcmp0 (status, "verify-finger-not-centered") == 0 ||
      g_strcmp0 (status, "verify-remove-and-retry") == 0)
    {
      ctx->tries_used++;
      {
        GString *payload = g_string_new ("{\"state\":\"retry\",\"device\":");
        json_append_escaped (payload, short_reader_label (ad->name));
        g_string_append_printf (payload, ",\"tries\":%d,\"max_tries\":%d,\"status\":",
                                ctx->tries_used, ctx->max_tries);
        json_append_escaped (payload, status ? status : "verify-no-match");
        g_string_append (payload, ",\"service\":");
        json_append_escaped (payload, ctx->service ? ctx->service : "pam");
        g_string_append_c (payload, '}');
        emit_hud_json (ctx, payload->str);
        g_string_free (payload, TRUE);
      }
#ifdef DUAL_CLI_TEST
      printf ("  [%s] %s (attempt %d/%d)\n", ad->name, status, ctx->tries_used, ctx->max_tries);
      fflush (stdout);
#else
      if (!ctx->quiet && ctx->tries_used < ctx->max_tries)
        send_pam_msg (ctx->pamh, PAM_TEXT_INFO, "Failed to match fingerprint — try again on either reader");
#endif
      if (ctx->tries_used >= ctx->max_tries)
        {
          ctx->result = 1;
          if (g_main_loop_is_running (ctx->loop))
            g_main_loop_quit (ctx->loop);
          return;
        }
      /* Stop & re-arm this reader on our private GMainContext while the other reader stays armed */
      g_autoptr(GError) err = NULL;
      g_autoptr(GVariant) st = g_dbus_proxy_call_sync (
        ad->proxy, "VerifyStop", NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err);
      if (!st)
        pm_log ("VerifyStop after %s on '%s' returned: %s",
                status, ad->name, err ? err->message : "err");
      /* Give the sensor 150 ms to finish stop/idle before re-arming */
      GSource *tsrc = g_timeout_source_new (150);
      g_source_set_callback (tsrc, restart_device_timer, ad, NULL);
      g_source_attach (tsrc, ctx->mctx);
      g_source_unref (tsrc);
      return;
    }

  /* Unknown/disconnected error on one reader (e.g. USB suspend/resume or unplug):
   * disable this reader; keep going if another reader is still alive. If all readers
   * dropped, return result=3 so lockscreen mode (timeout <= 0) can re-arm cleanly
   * after USB wakes back up. */
  if (done)
    {
      ad->disabled = TRUE;
      pm_log ("disabled '%s' (%s) after terminal status=%s",
              ad->name, ad->path, status ? status : "?");
      gboolean any_alive = FALSE;
      for (guint i = 0; i < ctx->devs->len; i++)
        {
          ActiveDev *other = g_ptr_array_index (ctx->devs, i);
          if (!other->disabled && other->claimed)
            any_alive = TRUE;
        }
      if (!any_alive && g_main_loop_is_running (ctx->loop))
        {
          ctx->result = 3;
          g_main_loop_quit (ctx->loop);
        }
    }
}

static ActiveDev *
try_claim_device (DualCtx *ctx, const char *obj_path, gboolean *had_enrolled_io)
{
  /* Avoid duplicate arming of the same object path */
  for (guint i = 0; i < ctx->devs->len; i++)
    {
      ActiveDev *existing = g_ptr_array_index (ctx->devs, i);
      if (g_strcmp0 (existing->path, obj_path) == 0 && !existing->disabled)
        return NULL;
    }

  g_autoptr(GError) derr = NULL;
  GDBusProxy *dev = g_dbus_proxy_new_sync (
    ctx->bus, G_DBUS_PROXY_FLAGS_NONE, NULL,
    "net.reactivated.Fprint", obj_path,
    "net.reactivated.Fprint.Device", NULL, &derr);
  if (!dev)
    {
      pm_log ("proxy failed for %s: %s", obj_path, derr ? derr->message : "unknown");
      return NULL;
    }

  g_autoptr(GVariant) name_v = g_dbus_proxy_get_cached_property (dev, "name");
  const char *dname = (name_v && g_variant_is_of_type (name_v, G_VARIANT_TYPE_STRING))
                        ? g_variant_get_string (name_v, NULL)
                        : obj_path;

  if (ctx->lid_closed && is_internal_builtin_reader (dname))
    {
      pm_log ("skipping built-in '%s' (%s) because lid is closed", dname, obj_path);
      g_object_unref (dev);
      return NULL;
    }

  if (ctx->builtin_only && !is_internal_builtin_reader (dname))
    {
      pm_log ("skipping external '%s' (%s) in builtin-only mode", dname, obj_path);
      g_object_unref (dev);
      return NULL;
    }

  /* Check if user has any enrolled fingers on this reader */
  g_autoptr(GVariant) f_res = g_dbus_proxy_call_sync (
    dev, "ListEnrolledFingers", g_variant_new ("(s)", ctx->username),
    G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &derr);
  if (!f_res)
    {
      pm_log ("ListEnrolledFingers failed on '%s' (%s): %s",
              dname, obj_path, derr ? derr->message : "unknown");
      g_object_unref (dev);
      return NULL;
    }
  g_autoptr(GVariant) arr = g_variant_get_child_value (f_res, 0);
  gsize n_fingers = g_variant_n_children (arr);
  if (n_fingers == 0)
    {
      pm_log ("skipping '%s' (%s): 0 enrolled fingers for %s", dname, obj_path, ctx->username);
      g_object_unref (dev);
      return NULL;
    }
  if (had_enrolled_io)
    *had_enrolled_io = TRUE;

  /* Claim device */
  g_autoptr(GVariant) c_res = g_dbus_proxy_call_sync (
    dev, "Claim", g_variant_new ("(s)", ctx->username),
    G_DBUS_CALL_FLAGS_NONE, 4000, NULL, &derr);
  if (!c_res)
    {
      pm_log ("Claim failed on '%s' (%s): %s",
              dname, obj_path, derr ? derr->message : "unknown");
      g_object_unref (dev);
      return NULL;
    }

  ActiveDev *ad = g_new0 (ActiveDev, 1);
  ad->ctx = ctx;
  ad->path = g_strdup (obj_path);
  ad->name = g_strdup (dname);
  ad->proxy = dev;
  ad->claimed = TRUE;
  ad->sig_id = g_signal_connect (dev, "g-signal", G_CALLBACK (on_verify_status), ad);
  ad->prop_sig_id = g_signal_connect (dev, "g-properties-changed", G_CALLBACK (on_dev_props_changed), ad);
  g_ptr_array_add (ctx->devs, ad);
  pm_log ("claimed '%s' (%s, %zu enrolled)", ad->name, ad->path, n_fingers);
  return ad;
}

static void
on_manager_signal (GDBusProxy  *proxy,
                   const gchar *sender_name,
                   const gchar *signal_name,
                   GVariant    *parameters,
                   gpointer     user_data)
{
  (void) proxy;
  (void) sender_name;
  DualCtx *ctx = (DualCtx *) user_data;
  if (!ctx || ctx->result == 0)
    return;

  if (g_strcmp0 (signal_name, "DeviceAdded") == 0)
    {
      const gchar *obj_path = NULL;
      g_variant_get (parameters, "(&o)", &obj_path);
      if (!obj_path)
        return;
      pm_log ("hotplug DeviceAdded: %s", obj_path);
      ActiveDev *ad = try_claim_device (ctx, obj_path, NULL);
      if (ad)
        {
          g_autoptr(GError) serr = NULL;
          g_autoptr(GVariant) s_res = g_dbus_proxy_call_sync (
            ad->proxy, "VerifyStart", g_variant_new ("(s)", "any"),
            G_DBUS_CALL_FLAGS_NONE, 4000, NULL, &serr);
          if (s_res)
            {
              ad->verifying = TRUE;
              pm_log ("hotplug armed '%s' (%s)", ad->name, ad->path);
              emit_hud_armed (ctx);
            }
          else
            {
              ad->disabled = TRUE;
              pm_log ("hotplug VerifyStart failed on '%s' (%s): %s",
                      ad->name, ad->path, serr ? serr->message : "unknown");
            }
        }
    }
}

static gboolean
on_timeout (gpointer user_data)
{
  DualCtx *ctx = (DualCtx *) user_data;
  ctx->timeout_src = NULL;
  ctx->result = 2;
  if (g_main_loop_is_running (ctx->loop))
    g_main_loop_quit (ctx->loop);
  return G_SOURCE_REMOVE;
}

static gboolean
on_unix_cancel (gpointer user_data)
{
  DualCtx *ctx = (DualCtx *) user_data;
  ctx->result = 4; /* cancelled by caller (e.g. SIGTERM) */
  if (g_main_loop_is_running (ctx->loop))
    g_main_loop_quit (ctx->loop);
  return G_SOURCE_CONTINUE;
}

static int
run_dual_verify_once (const char *username, const char *service, int max_tries, int timeout_sec,
                      gboolean respect_lid, gboolean builtin_only, gboolean quiet,
                      gboolean send_initial_prompt, gboolean allow_rearm_on_empty,
#ifndef DUAL_CLI_TEST
                      pam_handle_t *pamh,
#endif
                      char **winner_out, double *ms_out)
{
  /* CRITICAL for KDE Plasma 6 kscreenlocker_greet (Qt6 + QEventDispatcherGlib):
   * Qt's main GUI thread already permanently owns GLib's global default GMainContext (NULL).
   * We MUST create a private per-thread GMainContext and push it as thread-default so
   * GDBus signals, timers, and g_main_loop_run() never deadlock on the global default context! */
  GMainContext *mctx = g_main_context_new ();
  g_main_context_push_thread_default (mctx);

  g_autoptr(GError) err = NULL;
  g_autoptr(GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &err);
  if (!bus)
    {
      pm_log ("system bus failed: %s", err ? err->message : "unknown");
      g_main_context_pop_thread_default (mctx);
      g_main_context_unref (mctx);
      return 2;
    }

  g_autoptr(GDBusProxy) mgr = g_dbus_proxy_new_sync (
    bus, G_DBUS_PROXY_FLAGS_NONE, NULL,
    "net.reactivated.Fprint", "/net/reactivated/Fprint/Manager",
    "net.reactivated.Fprint.Manager", NULL, &err);
  if (!mgr)
    {
      pm_log ("manager proxy failed: %s", err ? err->message : "unknown");
      g_main_context_pop_thread_default (mctx);
      g_main_context_unref (mctx);
      return 2;
    }

  g_autoptr(GVariant) res = g_dbus_proxy_call_sync (
    mgr, "GetDevices", NULL, G_DBUS_CALL_FLAGS_NONE, 4000, NULL, &err);
  if (!res)
    {
      pm_log ("GetDevices failed: %s", err ? err->message : "unknown");
      g_main_context_pop_thread_default (mctx);
      g_main_context_unref (mctx);
      return 2;
    }

  gboolean lid_closed = respect_lid && is_laptop_lid_closed ();
  gboolean had_enrolled = FALSE;
  DualCtx ctx = { 0 };
  ctx.mctx = mctx;
  ctx.loop = g_main_loop_new (mctx, FALSE);
  ctx.bus = bus;
  ctx.username = username;
  ctx.service = service ? service : "pam";
  {
    struct passwd *pw = getpwnam (username);
    if (pw)
      {
        ctx.target_uid = pw->pw_uid;
        ctx.has_uid = TRUE;
      }
  }
  ctx.lid_closed = lid_closed;
  ctx.builtin_only = builtin_only;
  ctx.quiet = quiet;
  ctx.devs = g_ptr_array_new ();
  ctx.max_tries = max_tries > 0 ? max_tries : 3;
  ctx.result = 2;
  ctx.t_start_us = g_get_monotonic_time ();
#ifndef DUAL_CLI_TEST
  ctx.pamh = pamh;
#endif

  g_autoptr(GVariantIter) iter = NULL;
  const gchar *obj_path = NULL;
  g_variant_get (res, "(ao)", &iter);
  gsize n_devs = g_variant_iter_n_children (iter);
  pm_log ("GetDevices returned %zu device(s) (lid_closed=%d, respect_lid=%d)",
          n_devs, lid_closed ? 1 : 0, respect_lid ? 1 : 0);

  while (g_variant_iter_loop (iter, "&o", &obj_path))
    try_claim_device (&ctx, obj_path, &had_enrolled);

  if (ctx.devs->len == 0)
    {
      pm_log ("0 devices claimed (had_enrolled=%d, allow_rearm=%d)",
              had_enrolled ? 1 : 0, allow_rearm_on_empty ? 1 : 0);
      g_ptr_array_free (ctx.devs, TRUE);
      g_main_loop_unref (ctx.loop);
      g_main_context_pop_thread_default (mctx);
      g_main_context_unref (mctx);
      return (had_enrolled || allow_rearm_on_empty) ? 3 : 2;
    }

  /* Arm ALL claimed readers in parallel! */
  guint armed = 0;
  for (guint i = 0; i < ctx.devs->len; i++)
    {
      ActiveDev *ad = g_ptr_array_index (ctx.devs, i);
      g_autoptr(GError) serr = NULL;
      g_autoptr(GVariant) s_res = g_dbus_proxy_call_sync (
        ad->proxy, "VerifyStart", g_variant_new ("(s)", "any"),
        G_DBUS_CALL_FLAGS_NONE, 4000, NULL, &serr);
      if (s_res)
        {
          ad->verifying = TRUE;
          armed++;
          pm_log ("armed '%s' (%s)", ad->name, ad->path);
        }
      else
        {
          ad->disabled = TRUE;
          pm_log ("VerifyStart failed on '%s' (%s): %s",
                  ad->name, ad->path, serr ? serr->message : "unknown");
        }
    }

  /* Listen for hotplug / USB re-enumeration while locked (e.g. if ELAN finishes
   * re-enumerating 1 second after locking, arm it automatically!). */
  gulong mgr_sig_id = g_signal_connect (mgr, "g-signal", G_CALLBACK (on_manager_signal), &ctx);

  if (armed > 0)
    {
      emit_hud_armed (&ctx);
      if (send_initial_prompt && !quiet)
        {
          g_autofree char *prompt = NULL;
          if (armed > 1)
            prompt = g_strdup ("Place your finger on either reader (ELAN or FT9201)");
          else
            {
              ActiveDev *only = g_ptr_array_index (ctx.devs, 0);
              for (guint i = 0; i < ctx.devs->len; i++)
                {
                  ActiveDev *cand = g_ptr_array_index (ctx.devs, i);
                  if (cand->verifying)
                    {
                      only = cand;
                      break;
                    }
                }
              prompt = lid_closed
                         ? g_strdup_printf ("Place your finger on %s (lid closed)", only->name)
                         : g_strdup_printf ("Place your finger on %s", only->name);
            }
#ifdef DUAL_CLI_TEST
          printf ("Armed %u reader(s) simultaneously (lid=%s, timeout=%ds): %s\n",
                  armed, lid_closed ? "CLOSED" : "open", timeout_sec, prompt);
          for (guint i = 0; i < ctx.devs->len; i++)
            {
              ActiveDev *ad = g_ptr_array_index (ctx.devs, i);
              if (ad->verifying)
                printf ("  • %s (%s)\n", ad->name, ad->path);
            }
          fflush (stdout);
#else
          send_pam_msg (pamh, PAM_TEXT_INFO, prompt);
#endif
        }
      /* Only arm timeout when timeout_sec > 0. Attach ALL sources to our private mctx! */
      GSource *sig_term = NULL;
      GSource *sig_int = NULL;
#ifdef DUAL_CLI_TEST
      const gboolean cancellable_worker = TRUE;
#else
      const gboolean cancellable_worker = g_strcmp0 (service, "sudo-pressmatch-fingerprint") == 0;
#endif
      if (cancellable_worker)
        {
          sig_term = g_unix_signal_source_new (SIGTERM);
          g_source_set_callback (sig_term, on_unix_cancel, &ctx, NULL);
          g_source_attach (sig_term, mctx);
          sig_int = g_unix_signal_source_new (SIGINT);
          g_source_set_callback (sig_int, on_unix_cancel, &ctx, NULL);
          g_source_attach (sig_int, mctx);
        }
      if (timeout_sec > 0)
        {
          ctx.timeout_src = g_timeout_source_new_seconds ((guint) timeout_sec);
          g_source_set_callback (ctx.timeout_src, on_timeout, &ctx, NULL);
          g_source_attach (ctx.timeout_src, mctx);
        }
      g_main_loop_run (ctx.loop);
      if (ctx.timeout_src)
        {
          g_source_destroy (ctx.timeout_src);
          g_source_unref (ctx.timeout_src);
          ctx.timeout_src = NULL;
        }
      if (cancellable_worker)
        {
          g_source_destroy (sig_term);
          g_source_unref (sig_term);
          g_source_destroy (sig_int);
          g_source_unref (sig_int);
        }
    }
  else
    {
      ctx.result = 3;
    }

  if (ctx.result != 0)
    {
      GString *payload = g_string_new ("{\"state\":\"ended\",\"rc\":");
      g_string_append_printf (payload, "%d,\"service\":", ctx.result);
      json_append_escaped (payload, ctx.service ? ctx.service : "pam");
      g_string_append_c (payload, '}');
      emit_hud_json (&ctx, payload->str);
      g_string_free (payload, TRUE);
    }

  if (mgr_sig_id)
    g_signal_handler_disconnect (mgr, mgr_sig_id);

  /* Cleanly stop and release ALL devices */
  for (guint i = 0; i < ctx.devs->len; i++)
    {
      ActiveDev *ad = g_ptr_array_index (ctx.devs, i);
      if (ad->sig_id)
        g_signal_handler_disconnect (ad->proxy, ad->sig_id);
      if (ad->prop_sig_id)
        g_signal_handler_disconnect (ad->proxy, ad->prop_sig_id);
      if (ad->verifying)
        {
          g_autoptr(GError) e = NULL;
          g_autoptr(GVariant) r = g_dbus_proxy_call_sync (
            ad->proxy, "VerifyStop", NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &e);
          if (!r)
            pm_log ("cleanup VerifyStop err on '%s': %s", ad->name, e ? e->message : "err");
        }
      if (ad->claimed)
        {
          g_autoptr(GError) e = NULL;
          g_autoptr(GVariant) r = g_dbus_proxy_call_sync (
            ad->proxy, "Release", NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &e);
          if (!r)
            pm_log ("cleanup Release err on '%s': %s", ad->name, e ? e->message : "err");
        }
      g_object_unref (ad->proxy);
      g_free (ad->path);
      g_free (ad->name);
      g_free (ad);
    }
  g_ptr_array_free (ctx.devs, TRUE);
  g_main_loop_unref (ctx.loop);
  g_main_context_pop_thread_default (mctx);
  g_main_context_unref (mctx);

  if (winner_out)
    *winner_out = ctx.winner_name;
  else
    g_free (ctx.winner_name);
  g_free (ctx.winner_path);
  if (ms_out)
    *ms_out = ctx.unlock_ms;

  return ctx.result;
}

static int
run_dual_verify (const char *username, const char *service, int max_tries, int timeout_sec,
                 gboolean respect_lid, gboolean builtin_only, gboolean quiet,
                 gboolean persistent_rearm,
#ifndef DUAL_CLI_TEST
                 pam_handle_t *pamh,
#endif
                 char **winner_out, double *ms_out)
{
  gboolean first = TRUE;
  for (int rearm = 0; ; rearm++)
    {
      int rc = run_dual_verify_once (username, service, max_tries, timeout_sec, respect_lid,
                                     builtin_only, quiet, first,
                                     persistent_rearm && rearm > 0,
#ifndef DUAL_CLI_TEST
                                     pamh,
#endif
                                     winner_out, ms_out);
      first = FALSE;
      if (rc == 4)
        return 2; /* cleanly cancelled by SIGTERM/SIGINT */
      /* In dedicated parallel lockscreen mode (persistent_rearm && timeout_sec <= 0),
       * if devices disconnected during suspend/resume or USB reset (rc == 3), wait briefly
       * and re-arm seamlessly. For sequential services (e.g. polkit-1 with timeout=-1),
       * do NOT loop forever if no reader is usable — fall through to password immediately! */
      if (rc == 3 && persistent_rearm && timeout_sec <= 0)
        {
          g_usleep (2000000); /* 2 s for USB re-enumeration after sleep/resume */
          continue;
        }
      return (rc == 3) ? 2 : rc;
    }
}

#ifdef DUAL_CLI_TEST
int
main (int argc, char **argv)
{
  const char *user = argc > 1 ? argv[1] : g_get_user_name ();
  int timeout = argc > 2 ? atoi (argv[2]) : 20;
  char *winner = NULL;
  double ms = 0.0;
  int rc = run_dual_verify (user, "cli-test", 3, timeout, TRUE, FALSE, FALSE, FALSE, &winner, &ms);
  if (rc == 0)
    printf ("UNLOCKED by [%s] in %.0f ms!\n", winner ? winner : "?", ms);
  else if (rc == 1)
    printf ("DENIED (max tries reached)\n");
  else
    printf ("TIMEOUT / UNAVAILABLE\n");
  g_free (winner);
  return rc;
}
#else
int
pam_sm_authenticate (pam_handle_t *pamh, int flags, int argc, const char **argv)
{
  (void) flags;
  const char *user = NULL;
  const void *svc_item = NULL;
  const char *service = NULL;
  int max_tries = 3;
  int timeout_sec = 15;
  gboolean timeout_explicit = FALSE;
  gboolean max_tries_explicit = FALSE;
  gboolean respect_lid = TRUE;
  gboolean builtin_only = FALSE;
  gboolean quiet = FALSE;

  if (pam_get_user (pamh, &user, NULL) != PAM_SUCCESS || !user || !*user)
    return PAM_AUTHINFO_UNAVAIL;

  if (pam_get_item (pamh, PAM_SERVICE, &svc_item) == PAM_SUCCESS && svc_item)
    service = (const char *) svc_item;

  /* Dedicated parallel lockscreen workers (kde-fingerprint, gdm-fingerprint) run
   * side-by-side with the password box, so they must NOT time out after 15 seconds
   * while the user is away from the desk, and should re-arm after suspend/USB reset! */
  gboolean persistent_rearm = FALSE;
  if (service && (strstr (service, "fingerprint") != NULL))
    {
      timeout_sec = -1;
      max_tries = 5;
      persistent_rearm = g_strcmp0 (service, "sudo-pressmatch-fingerprint") != 0;
    }

  for (int i = 0; i < argc; i++)
    {
      if (strncmp (argv[i], "max-tries=", 10) == 0)
        {
          max_tries = atoi (argv[i] + 10);
          max_tries_explicit = TRUE;
        }
      else if (strncmp (argv[i], "timeout=", 8) == 0)
        {
          timeout_sec = atoi (argv[i] + 8);
          timeout_explicit = TRUE;
        }
      else if (strcmp (argv[i], "ignore-lid") == 0)
        respect_lid = FALSE;
      else if (strcmp (argv[i], "builtin-only") == 0)
        builtin_only = TRUE;
      else if (strcmp (argv[i], "quiet") == 0)
        quiet = TRUE;
    }
  (void) timeout_explicit;
  (void) max_tries_explicit;

  syslog (LOG_AUTHPRIV | LOG_INFO, "pam_pressmatch_dual: starting user=%s service=%s max_tries=%d timeout=%d builtin_only=%d",
          user, service ? service : "?", max_tries, timeout_sec, builtin_only ? 1 : 0);

  char *winner = NULL;
  double ms = 0.0;
  int rc = run_dual_verify (user, service, max_tries, timeout_sec, respect_lid, builtin_only, quiet, persistent_rearm, pamh, &winner, &ms);
  if (rc == 0)
    {
      syslog (LOG_AUTHPRIV | LOG_INFO, "pam_pressmatch_dual: matched user=%s service=%s on '%s' (%.0f ms)",
              user, service ? service : "?", winner ? winner : "?", ms);
      g_free (winner);
      return PAM_SUCCESS;
    }
  syslog (LOG_AUTHPRIV | LOG_INFO, "pam_pressmatch_dual: ended user=%s service=%s rc=%d",
          user, service ? service : "?", rc);
  g_free (winner);
  return (rc == 1) ? PAM_AUTH_ERR : PAM_AUTHINFO_UNAVAIL;
}

int
pam_sm_setcred (pam_handle_t *pamh, int flags, int argc, const char **argv)
{
  (void) pamh; (void) flags; (void) argc; (void) argv;
  return PAM_SUCCESS;
}

int
pam_sm_chauthtok (pam_handle_t *pamh, int flags, int argc, const char **argv)
{
  (void) pamh; (void) flags; (void) argc; (void) argv;
  return PAM_IGNORE;
}
#endif
