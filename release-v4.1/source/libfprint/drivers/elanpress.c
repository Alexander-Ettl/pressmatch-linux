/*
 * Driver for ELAN press-type (touch) fingerprint sensors
 * Copyright (C) 2026 Filip Spanne
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#define FP_COMPONENT "elanpress"

#include "drivers_api.h"
#include "elanpress.h"
#include "elanpress-match.h"

#include <math.h>

static const guint8 cmd_read_status[ELANPRESS_CMD_LEN] = {0x40, 0x13};
static const guint8 cmd_get_image[ELANPRESS_CMD_LEN] = {0x00, 0x09};
static const guint8 cmd_stop[ELANPRESS_CMD_LEN] = {0x00, 0x0b};
static const guint8 cmd_get_sensor_dim[ELANPRESS_CMD_LEN] = {0x00, 0x0c};
/* ELAN EMSE1000 (04f3:0c6e) One-Touch Power-On / SSO pre-captured image commands */
static const guint8 cmd_check_precap[ELANPRESS_CMD_LEN] = {0x9a, 0x10};
static const guint8 cmd_get_precap_image[ELANPRESS_CMD_LEN] = {0x9a, 0x18};
static const guint8 cmd_delete_precap[ELANPRESS_CMD_LEN] = {0x9a, 0x14};
static const guint8 cmd_swsso_off[9] = {0x46, 0x07, 0x80, 0x01, 0x04, 0x40, 0x00, 0x00, 0x00};

struct _FpiDeviceElanPress
{
  FpDevice        parent;

  unsigned short  frame_width;
  unsigned short  frame_height;

  /* raw background frame (no finger), rotated to row-major */
  unsigned short *background;

  /* pre-captured power-on / wake SSO frame pulled from sensor SRAM (if present) */
  unsigned short *precap_frame;
  gboolean        check_precap;
  gboolean        from_precap;

  /* most recent frame polled from the sensor, not yet classified as a
   * touch or a clear reading */
  unsigned short *last_frame;

  /* rotated raw frames of the touch being captured */
  GSList         *frames;
  int             num_frames;

  /* set once the single-shot settle delay has been granted for the touch */
  gboolean        settled;

  /* hold capture off until the sensor has read clear for a while, so that
   * one finger left resting cannot stand in for several separate presses.
   * It carries over from one action to the next: a touch is imaged as soon
   * as it settles, well before the finger lifts, so without it a verify
   * retried after a no-match would image the same resting finger again
   * rather than wait for the next press. */
  gboolean        wait_for_finger_off;
  guint           finger_off_frames;

  /* consecutive clear polls while waiting for a press, used to decide when
   * the background frame is worth retaking, and the request to do so */
  guint           idle_frames;
  gboolean        bg_refresh;

  /* running totals for the current verify/identify: one accumulated score
   * per gallery print (a single entry when verifying), summed over the
   * presses sampled so far and averaged when the decision is reported */
  int             match_sample;
  int             match_retries;
  gdouble        *score_sums;
  guint           num_score_sums;

  /* processed images collected during adaptive stitched enrollment */
  GPtrArray          *enroll_images;
  GPtrArray          *enroll_s3_feats;
  int                 enroll_stage;
  guint               enroll_attempts;
  guint               enroll_consec_fail;
  guint               enroll_consec_sat;
  ElanpressPoseGraph *enroll_pg;
  ElanpressEnrollMap  enroll_map;
};

G_DEFINE_TYPE (FpiDeviceElanPress, fpi_device_elanpress, FP_TYPE_DEVICE);

/* === image processing === */

static unsigned int
elanpress_frame_size (FpiDeviceElanPress *self)
{
  return (unsigned int) self->frame_width * self->frame_height;
}

static gdouble
elanpress_frame_mean (const unsigned short *frame, unsigned int size)
{
  gint64 sum = 0;

  for (unsigned int i = 0; i < size; i++)
    sum += frame[i];

  return (gdouble) sum / size;
}

/* === print (de)serialization === */

static void
elanpress_print_set_data_v3 (FpiDeviceElanPress       *self,
                             FpPrint                  *print,
                             GPtrArray                *cov_images,
                             const ElanpressPoseGraph *pg)
{
  GVariantBuilder images_builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("aay"));
  g_autoptr(GBytes) pg_bytes = elanpress_pose_graph_serialize (pg);
  gsize pg_len = 0;
  gconstpointer pg_ptr = pg_bytes ? g_bytes_get_data (pg_bytes, &pg_len) : NULL;
  GVariant *pg_v;
  GVariant *fpi_data;

  gsize total_img_bytes = 0;

  for (guint i = 0; i < cov_images->len; i++)
    {
      gsize packed_len = 0;
      g_autofree guint8 *packed =
        elanpress_pack_image_4bit (g_ptr_array_index (cov_images, i),
                                   elanpress_frame_size (self),
                                   &packed_len);
      total_img_bytes += packed_len;
      g_variant_builder_add_value (
        &images_builder,
        g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                   packed, packed_len, sizeof (guint8)));
    }

  pg_v = g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, pg_ptr, pg_len, sizeof (guint8));
  fpi_data = g_variant_new ("(qqqaay@ay)",
                            (guint16) ELANPRESS_PRINT_VERSION,
                            (guint16) self->frame_width,
                            (guint16) self->frame_height,
                            &images_builder,
                            pg_v);

  fp_dbg ("serialized compact v4 print: images=%u B pg=%u B total=%u B",
          (guint) total_img_bytes,
          (guint) pg_len,
          (guint) (total_img_bytes + pg_len));

  fpi_print_set_type (print, FPI_PRINT_RAW);
  g_object_set (print, "fpi-data", fpi_data, NULL);
}

static gboolean
elanpress_print_get_data_v3 (FpiDeviceElanPress  *self,
                             FpPrint             *print,
                             GPtrArray          **cov_images_out,
                             ElanpressPoseGraph **pg_out)
{
  g_autoptr(GVariant) fpi_data = NULL;
  g_autoptr(GVariant) images_v = NULL;
  g_autoptr(GVariant) pg_v = NULL;
  GPtrArray *images;
  ElanpressPoseGraph *pg;
  guint16 version = 0, width = 0, height = 0;
  gsize n_images, pg_len = 0;
  gconstpointer pg_ptr;

  g_object_get (print, "fpi-data", &fpi_data, NULL);
  if (!fpi_data || !g_variant_check_format_string (fpi_data, "(qqq@aay@ay)", FALSE))
    return FALSE;

  g_variant_get (fpi_data, "(qqq@aay@ay)", &version, &width, &height, &images_v, &pg_v);
  if (version != ELANPRESS_PRINT_VERSION ||
      width != self->frame_width || height != self->frame_height)
    return FALSE;

  n_images = g_variant_n_children (images_v);
  if (n_images == 0)
    return FALSE;

  pg_ptr = g_variant_get_fixed_array (pg_v, &pg_len, sizeof (guint8));
  pg = elanpress_pose_graph_deserialize (pg_ptr, pg_len);
  if (!pg || pg->n_strips != n_images)
    {
      elanpress_pose_graph_free (pg);
      return FALSE;
    }

  images = g_ptr_array_new_full (n_images, g_free);
  for (gsize i = 0; i < n_images; i++)
    {
      g_autoptr(GVariant) img_v = g_variant_get_child_value (images_v, i);
      gsize len = 0;
      gconstpointer data = g_variant_get_fixed_array (img_v, &len, sizeof (guint8));
      guint8 *unpacked = elanpress_unpack_image_4bit (data, len, elanpress_frame_size (self));

      if (!unpacked)
        {
          g_ptr_array_unref (images);
          elanpress_pose_graph_free (pg);
          return FALSE;
        }
      g_ptr_array_add (images, unpacked);
    }

  *cov_images_out = images;
  *pg_out = pg;
  return TRUE;
}

static gint
elanpress_match_v3_print (FpiDeviceElanPress      *self,
                          const guint8            *cov_probe,
                          const ElanpressFeatures *s3_feat,
                          FpPrint                 *print,
                          gboolean                *matched_out,
                          gdouble                 *zncc_out,
                          guint                   *ov_out,
                          gdouble                 *t_deser_ms,
                          gdouble                 *t_pg_ms,
                          gdouble                 *t_zncc_ms)
{
  g_autoptr(GPtrArray) cov_images = NULL;
  ElanpressPoseGraph *pg = NULL;
  guint comp = 0, ov = 0, pairs = 0;
  ElanpressPose pose = { 1.0f, 0.0f, 0.0f, 0.0f };
  gdouble zncc = 0.0;
  gint64 t0, t1, t2, t3;

  t0 = g_get_monotonic_time ();
  if (!elanpress_print_get_data_v3 (self, print, &cov_images, &pg))
    return -1;
  t1 = g_get_monotonic_time ();

  pairs = elanpress_pose_graph_match_pose (s3_feat, pg, TRUE, &comp, &pose);
  t2 = g_get_monotonic_time ();

  if (pairs > 0)
    {
      gdouble zncc_rigid = 0.0;
      zncc = elanpress_pose_graph_zncc_elastic (cov_probe, cov_images, 1, 0,
                                                pg, comp, pose,
                                                self->frame_width,
                                                self->frame_height,
                                                ELANPRESS_CANDIDATE_ELASTIC_RADIUS,
                                                &zncc_rigid, &ov);
    }
  t3 = g_get_monotonic_time ();

  elanpress_pose_graph_free (pg);

  if (t_deser_ms)
    *t_deser_ms = (t1 - t0) / 1000.0;
  if (t_pg_ms)
    *t_pg_ms = (t2 - t1) / 1000.0;
  if (t_zncc_ms)
    *t_zncc_ms = (t3 - t2) / 1000.0;
  if (zncc_out)
    *zncc_out = zncc;
  if (ov_out)
    *ov_out = ov;
  if (matched_out)
    *matched_out =
      (pairs >= ELANPRESS_CANDIDATE_SUPER_MIN_PAIRS &&
       zncc >= ELANPRESS_CANDIDATE_SUPER_MIN_ZNCC &&
       ov >= ELANPRESS_CANDIDATE_SUPER_MIN_OVERLAP) ||
      (pairs >= ELANPRESS_CANDIDATE_MIN_PAIRS &&
       zncc >= ELANPRESS_CANDIDATE_MIN_ZNCC &&
       ov >= ELANPRESS_CANDIDATE_MIN_OVERLAP) ||
      (pairs >= ELANPRESS_CANDIDATE_ALT_MIN_PAIRS &&
       zncc >= ELANPRESS_CANDIDATE_ALT_MIN_ZNCC &&
       ov >= ELANPRESS_CANDIDATE_ALT_MIN_OVERLAP) ||
      (pairs >= ELANPRESS_CANDIDATE_HI_MIN_PAIRS &&
       zncc >= ELANPRESS_CANDIDATE_HI_MIN_ZNCC &&
       ov >= ELANPRESS_CANDIDATE_HI_MIN_OVERLAP);
  return (gint) pairs;
}

/* === USB helpers === */

static void
elanpress_reset_capture (FpiDeviceElanPress *self)
{
  g_slist_free_full (g_steal_pointer (&self->frames), g_free);
  self->num_frames = 0;
  self->settled = FALSE;
  self->idle_frames = 0;
  self->bg_refresh = FALSE;
  self->from_precap = FALSE;
  g_clear_pointer (&self->last_frame, g_free);
  g_clear_pointer (&self->precap_frame, g_free);
}

static void
elanpress_send_cmd_len (FpiSsm *ssm, FpDevice *dev, const guint8 *cmd, gsize len,
                        FpiUsbTransferCallback cb)
{
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);

  transfer->ssm = ssm;
  transfer->short_is_error = TRUE;
  fpi_usb_transfer_fill_bulk_full (transfer, ELANPRESS_EP_CMD_OUT,
                                   (guint8 *) cmd, len, NULL);
  fpi_usb_transfer_submit (transfer, ELANPRESS_CMD_TIMEOUT,
                           fpi_device_get_cancellable (dev),
                           cb, NULL);
}

static void
elanpress_send_cmd (FpiSsm *ssm, FpDevice *dev, const guint8 *cmd)
{
  elanpress_send_cmd_len (ssm, dev, cmd, ELANPRESS_CMD_LEN, fpi_ssm_usb_transfer_cb);
}

static void
elanpress_read (FpiSsm *ssm, FpDevice *dev, guint8 ep, gsize len,
                guint timeout, FpiUsbTransferCallback callback)
{
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);

  transfer->ssm = ssm;
  transfer->short_is_error = TRUE;
  fpi_usb_transfer_fill_bulk (transfer, ep, len);
  fpi_usb_transfer_submit (transfer, timeout,
                           fpi_device_get_cancellable (dev),
                           callback, NULL);
}

/* === touch capture state machine === */

/* Finger presence is inferred from the image rather than polled with
 * cmd_pre_scan: that command's status byte reports correctly once per
 * power-up and then answers "finger present" forever, which left capture
 * spinning in its wait-for-lift loop until the operation timed out, with
 * only a full power cycle to clear it. Comparing each frame against a
 * background taken with the sensor clear tells presence apart without it. */

enum capture_states {
  CAPTURE_PRECAP_CHECK_SEND,
  CAPTURE_PRECAP_CHECK_READ,
  CAPTURE_PRECAP_IMAGE_SEND,
  CAPTURE_PRECAP_IMAGE_READ,
  CAPTURE_PRECAP_SWSSO_OFF_SEND,
  CAPTURE_PRECAP_DELETE_SEND,
  CAPTURE_BG_REQUEST,
  CAPTURE_BG_STATUS_READ,
  CAPTURE_BG_IMAGE_REQUEST,
  CAPTURE_BG_READ,
  CAPTURE_PRECAP_EVAL,
  CAPTURE_POLL_REQUEST,
  CAPTURE_POLL_STATUS_READ,
  CAPTURE_POLL_IMAGE_REQUEST,
  CAPTURE_POLL_READ,
  CAPTURE_NUM_STATES,
};

static void
elanpress_precap_soft_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                          gpointer user_data, GError *error)
{
  FpiDeviceElanPress *self = FPI_DEVICE_ELANPRESS (dev);

  if (error)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          fpi_ssm_mark_failed (transfer->ssm, error);
          return;
        }
      fp_dbg ("SSO precap step %d non-fatal error (%s), falling back to live capture",
              fpi_ssm_get_cur_state (transfer->ssm), error->message);
      g_error_free (error);
      g_clear_pointer (&self->precap_frame, g_free);
      fpi_ssm_jump_to_state (transfer->ssm, CAPTURE_BG_REQUEST);
      return;
    }

  fpi_ssm_next_state (transfer->ssm);
}

static void
elanpress_precap_check_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                           gpointer user_data, GError *error)
{
  if (error)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          fpi_ssm_mark_failed (transfer->ssm, error);
          return;
        }
      fp_dbg ("SSO precap check unsupported/error (%s), falling back to live capture",
              error->message);
      g_error_free (error);
      fpi_ssm_jump_to_state (transfer->ssm, CAPTURE_BG_REQUEST);
      return;
    }

  gboolean has_precap = FALSE;
  if (transfer->actual_length >= 1 && transfer->buffer[0] == 0x00)
    has_precap = TRUE;
  else if (transfer->actual_length >= 3 &&
           transfer->buffer[0] == 0x9a &&
           transfer->buffer[1] == 0x10 &&
           transfer->buffer[2] == 0x00)
    has_precap = TRUE;

  if (has_precap)
    {
      fp_dbg ("SSO pre-captured power-on fingerprint image detected (len=%zd b0=0x%02x)",
              transfer->actual_length,
              transfer->actual_length > 0 ? transfer->buffer[0] : 0xff);
      fpi_ssm_next_state (transfer->ssm);
    }
  else
    {
      fp_dbg ("SSO no pre-captured image (len=%zd b0=0x%02x)",
              transfer->actual_length,
              transfer->actual_length > 0 ? transfer->buffer[0] : 0xff);
      fpi_ssm_jump_to_state (transfer->ssm, CAPTURE_BG_REQUEST);
    }
}

static void
elanpress_frame_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                    gpointer user_data, GError *error)
{
  FpiDeviceElanPress *self = FPI_DEVICE_ELANPRESS (dev);
  unsigned short *frame;
  int cur_state = fpi_ssm_get_cur_state (transfer->ssm);

  if (error)
    {
      if (cur_state == CAPTURE_PRECAP_IMAGE_READ &&
          !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          fp_dbg ("SSO precap image read failed (%s), falling back to live capture",
                  error->message);
          g_error_free (error);
          g_clear_pointer (&self->precap_frame, g_free);
          fpi_ssm_jump_to_state (transfer->ssm, CAPTURE_BG_REQUEST);
          return;
        }
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  frame = g_malloc (elanpress_frame_size (self) * sizeof (short));
  elanpress_rotate_frame (transfer->buffer, frame,
                          self->frame_width, self->frame_height);

  if (cur_state == CAPTURE_PRECAP_IMAGE_READ)
    {
      g_free (self->precap_frame);
      self->precap_frame = frame;
      fp_dbg ("SSO read pre-captured power-on frame (%ux%u, mean=%.0f)",
              self->frame_width, self->frame_height,
              elanpress_frame_mean (frame, elanpress_frame_size (self)));
    }
  else if (cur_state == CAPTURE_BG_READ)
    {
      unsigned int size = elanpress_frame_size (self);

      self->bg_refresh = FALSE;

      if (!self->background)
        {
          self->background = frame;
          fp_dbg ("captured background frame");
        }
      else if (elanpress_frame_mean (frame, size) <=
               elanpress_frame_mean (self->background, size))
        {
          g_free (self->background);
          self->background = frame;
          fp_dbg ("background refreshed");
        }
      else
        {
          /* a finger only ever adds brightness, so a brighter candidate
           * means something was resting on the sensor. Adopting it would
           * write that finger into the reference and hide it for good. */
          fp_dbg ("background candidate brighter than current, keeping it");
          g_free (frame);
        }
    }
  else
    {
      g_free (self->last_frame);
      self->last_frame = frame;
    }

  fpi_ssm_next_state (transfer->ssm);
}

/* The status byte arms a fresh exposure on some revisions of this sensor. */
static void
elanpress_capture_status_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                             gpointer user_data, GError *error)
{
  if (error)
    fpi_ssm_mark_failed (transfer->ssm, error);
  else
    fpi_ssm_next_state (transfer->ssm);
}

static void
capture_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceElanPress *self = FPI_DEVICE_ELANPRESS (dev);
  gsize frame_bytes = elanpress_frame_size (self) * 2;
  ElanpressTouchStats stats = { 0, 0 };
  gboolean has_touch;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case CAPTURE_PRECAP_CHECK_SEND:
      if (!self->check_precap)
        {
          fpi_ssm_jump_to_state (ssm, CAPTURE_BG_REQUEST);
          break;
        }
      self->check_precap = FALSE;
      elanpress_send_cmd_len (ssm, dev, cmd_check_precap, ELANPRESS_CMD_LEN,
                              elanpress_precap_soft_cb);
      break;

    case CAPTURE_PRECAP_CHECK_READ:
      {
        FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);
        transfer->ssm = ssm;
        transfer->short_is_error = FALSE;
        fpi_usb_transfer_fill_bulk (transfer, ELANPRESS_EP_CMD_IN, 4);
        fpi_usb_transfer_submit (transfer, 500,
                                 fpi_device_get_cancellable (dev),
                                 elanpress_precap_check_cb, NULL);
      }
      break;

    case CAPTURE_PRECAP_IMAGE_SEND:
      elanpress_send_cmd_len (ssm, dev, cmd_get_precap_image, ELANPRESS_CMD_LEN,
                              elanpress_precap_soft_cb);
      break;

    case CAPTURE_PRECAP_IMAGE_READ:
      elanpress_read (ssm, dev, ELANPRESS_EP_IMG_IN, frame_bytes,
                      ELANPRESS_FRAME_TIMEOUT, elanpress_frame_cb);
      break;

    case CAPTURE_PRECAP_SWSSO_OFF_SEND:
      elanpress_send_cmd_len (ssm, dev, cmd_swsso_off, sizeof (cmd_swsso_off),
                              elanpress_precap_soft_cb);
      break;

    case CAPTURE_PRECAP_DELETE_SEND:
      elanpress_send_cmd_len (ssm, dev, cmd_delete_precap, ELANPRESS_CMD_LEN,
                              elanpress_precap_soft_cb);
      break;

    case CAPTURE_BG_REQUEST:
      if (self->background && !self->bg_refresh)
        {
          fpi_ssm_jump_to_state (ssm, CAPTURE_PRECAP_EVAL);
          break;
        }
      elanpress_send_cmd (ssm, dev, cmd_read_status);
      break;

    case CAPTURE_BG_STATUS_READ:
    case CAPTURE_POLL_STATUS_READ:
      elanpress_read (ssm, dev, ELANPRESS_EP_CMD_IN, 1,
                      ELANPRESS_CMD_TIMEOUT, elanpress_capture_status_cb);
      break;

    case CAPTURE_BG_IMAGE_REQUEST:
    case CAPTURE_POLL_IMAGE_REQUEST:
      elanpress_send_cmd (ssm, dev, cmd_get_image);
      break;

    case CAPTURE_BG_READ:
      elanpress_read (ssm, dev, ELANPRESS_EP_IMG_IN, frame_bytes,
                      ELANPRESS_FRAME_TIMEOUT, elanpress_frame_cb);
      break;

    case CAPTURE_PRECAP_EVAL:
      if (self->precap_frame && self->background)
        {
          has_touch = elanpress_frame_has_touch (self->precap_frame,
                                                 self->background,
                                                 elanpress_frame_size (self),
                                                 &stats);
          fp_dbg ("SSO precap vs live bg: %s (coverage %.1f%%, mean delta %.0f)",
                  has_touch ? "touch" : "clear",
                  stats.coverage * 100, stats.mean_delta);
          if (has_touch)
            {
              self->frames = g_slist_prepend (self->frames,
                                              g_steal_pointer (&self->precap_frame));
              self->num_frames = 1;
              self->from_precap = TRUE;
              fpi_ssm_mark_completed (ssm);
              break;
            }
          g_clear_pointer (&self->precap_frame, g_free);
        }
      fpi_ssm_next_state (ssm);
      break;

    case CAPTURE_POLL_REQUEST:
      if (!self->wait_for_finger_off)
        fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NEEDED);
      elanpress_send_cmd (ssm, dev, cmd_read_status);
      break;

    case CAPTURE_POLL_READ:
      elanpress_read (ssm, dev, ELANPRESS_EP_IMG_IN, frame_bytes,
                      ELANPRESS_FRAME_TIMEOUT, elanpress_frame_cb);
      break;

    default:
      /* after a frame: classify it and decide what the touch needs next */
      has_touch = elanpress_frame_has_touch (self->last_frame,
                                             self->background,
                                             elanpress_frame_size (self),
                                             &stats);

      fp_dbg ("poll: %s (coverage %.1f%%, mean delta %.0f)",
              has_touch ? "touch" : "clear",
              stats.coverage * 100, stats.mean_delta);

      if (self->wait_for_finger_off)
        {
          /* Wait briefly until the sensor reads clear so a held finger never
           * triggers two actions back-to-back. */
          g_clear_pointer (&self->last_frame, g_free);
          if (has_touch)
            {
              self->finger_off_frames = 0;
            }
          else
            {
              if (++self->finger_off_frames >= 5)
                {
                  self->wait_for_finger_off = FALSE;
                  self->finger_off_frames = 0;
                  fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NEEDED);
                }
            }
          fpi_ssm_jump_to_state_delayed (ssm, CAPTURE_POLL_REQUEST,
                                         ELANPRESS_IDLE_POLL_INTERVAL_MS);
          break;
        }

      if (has_touch)
        {
          fpi_device_report_finger_status (dev,
                                           FP_FINGER_STATUS_NEEDED |
                                           FP_FINGER_STATUS_PRESENT);

          if (!self->settled)
            {
              /* Single-shot 200 ms settle before capturing the crisp touch frame */
              self->settled = TRUE;
              g_clear_pointer (&self->last_frame, g_free);
              fpi_ssm_jump_to_state_delayed (ssm, CAPTURE_POLL_REQUEST,
                                             ELANPRESS_SETTLE_MS);
              break;
            }

          if (self->num_frames < 1)
            {
              self->frames = g_slist_prepend (self->frames,
                                              g_steal_pointer (&self->last_frame));
              self->num_frames++;
            }
          else
            g_clear_pointer (&self->last_frame, g_free);
          self->idle_frames = 0;

          fpi_ssm_mark_completed (ssm);
          break;
        }

      g_clear_pointer (&self->last_frame, g_free);

      if (self->num_frames > 0)
        {
          /* finger lifted before ELANPRESS_MAX_FRAMES: use what there is */
          fp_dbg ("touch complete, %d frames", self->num_frames);
          fpi_ssm_mark_completed (ssm);
        }
      else
        {
          /* still waiting for a press, or it bounced during the settle
           * delay before any frame was kept */
          if (self->settled)
            {
              self->wait_for_finger_off = TRUE;
              self->finger_off_frames = 0;
            }
          self->settled = FALSE;

          if (++self->idle_frames >= ELANPRESS_BG_REFRESH_FRAMES)
            {
              fp_dbg ("sensor clear for %u polls, retaking the background",
                      self->idle_frames);
              self->idle_frames = 0;
              self->bg_refresh = TRUE;
              fpi_ssm_jump_to_state_delayed (ssm, CAPTURE_BG_REQUEST,
                                             ELANPRESS_IDLE_POLL_INTERVAL_MS);
              break;
            }

          fpi_ssm_jump_to_state_delayed (ssm, CAPTURE_POLL_REQUEST,
                                         ELANPRESS_IDLE_POLL_INTERVAL_MS);
        }
      break;
    }
}

static void
elanpress_capture_touch (FpiDeviceElanPress *self, FpiSsmCompletedCallback done)
{
  FpiSsm *ssm;

  elanpress_reset_capture (self);
  self->wait_for_finger_off = TRUE;
  self->finger_off_frames = 0;
  /* one extra state acts as the post-frame decision point */
  ssm = fpi_ssm_new (FP_DEVICE (self), capture_run_state,
                     CAPTURE_NUM_STATES + 1);
  fpi_ssm_start (ssm, done);
}

/* === stop helper: turn the sensor off after an action === */

static void
elanpress_send_stop (FpDevice *dev)
{
  g_autoptr(FpiUsbTransfer) transfer = fpi_usb_transfer_new (dev);
  g_autoptr(GError) error = NULL;

  fpi_usb_transfer_fill_bulk_full (transfer, ELANPRESS_EP_CMD_OUT,
                                   (guint8 *) cmd_stop, ELANPRESS_CMD_LEN,
                                   NULL);
  if (!fpi_usb_transfer_submit_sync (transfer, ELANPRESS_CMD_TIMEOUT, &error))
    fp_warn ("failed to send stop command: %s", error->message);
}

/* === enroll === */

static void
elanpress_enroll_fail_clean (FpDevice *dev, const char *reason, const char *msg)
{
  FpiDeviceElanPress *self = FPI_DEVICE_ELANPRESS (dev);

  elanpress_enroll_telemetry_emit ("elan", self->frame_width, self->frame_height,
                                   "error", self->enroll_stage, ELANPRESS_ENROLL_STAGES,
                                   self->enroll_attempts, 0.0, 0.0,
                                   self->enroll_pg, &self->enroll_map);
  elanpress_send_stop (dev);
  fp_dbg ("enroll failed (%s): %s", reason, msg);
  g_clear_pointer (&self->enroll_images, g_ptr_array_unref);
  g_clear_pointer (&self->enroll_s3_feats, g_ptr_array_unref);
  g_clear_pointer (&self->enroll_pg, elanpress_pose_graph_free);
  fpi_device_enroll_complete (dev, NULL,
                              fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL, "%s", msg));
}

static void
elanpress_enroll_finalize (FpDevice *dev)
{
  FpiDeviceElanPress *self = FPI_DEVICE_ELANPRESS (dev);
  FpPrint *print = NULL;

  elanpress_enroll_telemetry_emit ("elan", self->frame_width, self->frame_height,
                                   "complete", ELANPRESS_ENROLL_STAGES, ELANPRESS_ENROLL_STAGES,
                                   self->enroll_attempts, 0.0, 0.0,
                                   self->enroll_pg, &self->enroll_map);
  elanpress_send_stop (dev);
  fp_dbg ("enroll complete: stitched=%u/%u comps=%u span=%.0fx%.0f attempts=%u pct=%u",
          self->enroll_pg->largest_nodes, self->enroll_pg->n_strips,
          self->enroll_pg->n_components, self->enroll_pg->span_w, self->enroll_pg->span_h,
          self->enroll_attempts, self->enroll_map.progress_pct);

  fpi_device_get_enroll_data (dev, &print);
  elanpress_print_set_data_v3 (self, print, self->enroll_images, self->enroll_pg);
  g_clear_pointer (&self->enroll_images, g_ptr_array_unref);
  g_clear_pointer (&self->enroll_s3_feats, g_ptr_array_unref);
  g_clear_pointer (&self->enroll_pg, elanpress_pose_graph_free);

  fpi_device_enroll_complete (dev, g_object_ref (print), NULL);
}

static void
elanpress_enroll_touch_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceElanPress *self = FPI_DEVICE_ELANPRESS (dev);
  g_autofree guint8 *cov_image = NULL;
  g_autoptr(ElanpressFeatures) s3_feat = NULL;
  const ElanpressSiftParams p_s3 = {
    .contrast_thr = 0.0f,
    .edge_ratio = ELANPRESS_CANDIDATE_EDGE_RATIO,
    .rootsift = TRUE,
  };

  fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NONE);

  if (error)
    {
      elanpress_send_stop (dev);
      fpi_device_enroll_complete (dev, NULL, error);
      return;
    }

  unsigned int size = elanpress_frame_size (self);
  ElanpressFrameDiag early_fd = { 0 };
  const unsigned short *raw_early = self->num_frames > 0 ? g_slist_last (self->frames)->data : NULL;

  self->enroll_attempts++;
  cov_image = elanpress_process_frame_covered (raw_early, self->background, size);
  elanpress_frame_diag (raw_early, self->background, cov_image, size, &early_fd);
  elanpress_reset_capture (self);

  if (cov_image && early_fd.norm_stddev < ELANPRESS_MIN_QUALITY_STDDEV)
    {
      fp_dbg ("touch too low-contrast to enroll (std-dev %.1f < %.1f)",
              early_fd.norm_stddev, ELANPRESS_MIN_QUALITY_STDDEV);
      g_clear_pointer (&cov_image, g_free);
    }

  /* each stage has to be its own press, not a continuation of this one */
  self->wait_for_finger_off = TRUE;
  self->finger_off_frames = 0;

  if (!cov_image)
    {
      self->enroll_consec_fail++;
      fp_dbg ("enroll reject: attempt=%u sd=%.1f cov=%.1f consec=%u",
              self->enroll_attempts, early_fd.norm_stddev,
              early_fd.coverage, self->enroll_consec_fail);
      if (self->enroll_consec_fail >= ELANPRESS_ENROLL_MAX_CONSEC_FAIL ||
          self->enroll_attempts >= ELANPRESS_ENROLL_MAX_ATTEMPTS)
        {
          elanpress_enroll_fail_clean (dev, "low_quality",
                                       "Could not read clear ridges — please wipe your finger and sensor, then try again.");
          return;
        }
      elanpress_enroll_telemetry_emit ("elan", self->frame_width, self->frame_height,
                                       "low_quality", self->enroll_stage, ELANPRESS_ENROLL_STAGES,
                                       self->enroll_attempts, early_fd.norm_stddev, early_fd.mean_delta,
                                       self->enroll_pg, &self->enroll_map);
      fpi_device_enroll_progress (dev, self->enroll_stage, NULL,
                                  fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL));
      elanpress_capture_touch (self, elanpress_enroll_touch_done);
      return;
    }

  if (early_fd.coverage < ELANPRESS_ENROLL_MIN_COVERAGE)
    {
      self->enroll_consec_fail++;
      fp_dbg ("enroll low coverage: attempt=%u cov=%.1f consec=%u",
              self->enroll_attempts, early_fd.coverage, self->enroll_consec_fail);
      if (self->enroll_consec_fail >= ELANPRESS_ENROLL_MAX_CONSEC_FAIL ||
          self->enroll_attempts >= ELANPRESS_ENROLL_MAX_ATTEMPTS)
        {
          elanpress_enroll_fail_clean (dev, "low_coverage",
                                       "Too many partial touches — please clean your finger and sensor, then try again.");
          return;
        }
      elanpress_enroll_telemetry_emit ("elan", self->frame_width, self->frame_height,
                                       "low_coverage", self->enroll_stage, ELANPRESS_ENROLL_STAGES,
                                       self->enroll_attempts, early_fd.norm_stddev, early_fd.mean_delta,
                                       self->enroll_pg, &self->enroll_map);
      fpi_device_enroll_progress (dev, self->enroll_stage, NULL,
                                  fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER));
      elanpress_capture_touch (self, elanpress_enroll_touch_done);
      return;
    }

  /* Saturation guard: reject rail-saturated/damp/over-pressed enrollment frames
   * (capped at 3 consecutive rejections so naturally warm hands never deadlock). */
  if ((early_fd.mean_delta > ELANPRESS_ENROLL_SAT_HARD_MD ||
       (early_fd.mean_delta > ELANPRESS_ENROLL_SAT_SOFT_MD &&
        early_fd.norm_stddev < ELANPRESS_ENROLL_SAT_MIN_SD)) &&
      self->enroll_consec_sat < ELANPRESS_ENROLL_MAX_CONSEC_SAT &&
      self->enroll_attempts + 2 < ELANPRESS_ENROLL_MAX_ATTEMPTS)
    {
      self->enroll_consec_sat++;
      fp_dbg ("enroll saturated press: attempt=%u md=%.0f sd=%.1f consec=%u",
              self->enroll_attempts, early_fd.mean_delta, early_fd.norm_stddev,
              self->enroll_consec_sat);
      elanpress_enroll_telemetry_emit ("elan", self->frame_width, self->frame_height,
                                       "saturated", self->enroll_stage, ELANPRESS_ENROLL_STAGES,
                                       self->enroll_attempts, early_fd.norm_stddev, early_fd.mean_delta,
                                       self->enroll_pg, &self->enroll_map);
      fpi_device_enroll_progress (dev, self->enroll_stage, NULL,
                                  fpi_device_retry_new (FP_DEVICE_RETRY_REMOVE_FINGER));
      elanpress_capture_touch (self, elanpress_enroll_touch_done);
      return;
    }
  self->enroll_consec_sat = 0;

  s3_feat = elanpress_sift_features_opts (cov_image, self->frame_width, self->frame_height,
                                          ELANPRESS_ENROLL_MAX_STRIP_KP, &p_s3, NULL);

  /* First accepted scan becomes the anchor at the center of the finger map */
  if (self->enroll_images->len == 0)
    {
      g_ptr_array_add (self->enroll_images, g_steal_pointer (&cov_image));
      g_ptr_array_add (self->enroll_s3_feats, g_steal_pointer (&s3_feat));
      self->enroll_pg = elanpress_pose_graph_new_single (g_ptr_array_index (self->enroll_s3_feats, 0),
                                                         self->frame_width, self->frame_height);
      elanpress_enroll_map_compute (self->enroll_images, self->enroll_pg, 0,
                                    self->frame_width, self->frame_height,
                                    ELANPRESS_ENROLL_MIN_STRIPS, &self->enroll_map);
      self->enroll_consec_fail = 0;
      self->enroll_stage = 1;

      fp_dbg ("enroll anchor: cov=%.1f%% md=%.0f sd=%.1f kp=%u",
              early_fd.coverage, early_fd.mean_delta, early_fd.norm_stddev,
              ((ElanpressFeatures *) g_ptr_array_index (self->enroll_s3_feats, 0))->n);
      elanpress_enroll_telemetry_emit ("elan", self->frame_width, self->frame_height,
                                       "accepted", self->enroll_stage, ELANPRESS_ENROLL_STAGES,
                                       self->enroll_attempts, early_fd.norm_stddev, early_fd.mean_delta,
                                       self->enroll_pg, &self->enroll_map);
      fpi_device_enroll_progress (dev, self->enroll_stage, NULL, NULL);
      elanpress_capture_touch (self, elanpress_enroll_touch_done);
      return;
    }

  /* Check overlap against existing map for redundancy detection */
  {
    guint comp = 0, ov = 0;
    ElanpressPose pose = { 1.0f, 0.0f, 0.0f, 0.0f };
    guint pairs = elanpress_pose_graph_match_pose (s3_feat, self->enroll_pg,
                                                   TRUE, &comp, &pose);
    gdouble zncc = 0.0;
    gdouble gain = 1.0;

    if (pairs > 0)
      zncc = elanpress_pose_graph_zncc (cov_image, self->enroll_images, 1, 0,
                                        self->enroll_pg, comp, pose,
                                        self->frame_width, self->frame_height, &ov);

    /* Only skip true saturated duplicates (landed right on top of a deep spot) */
    if (pairs >= ELANPRESS_ENROLL_STITCH_MIN_PAIRS &&
        zncc >= ELANPRESS_CANDIDATE_MIN_ZNCC &&
        ov >= ELANPRESS_CANDIDATE_MIN_OVERLAP &&
        self->enroll_images->len >= 3)
      {
        gain = elanpress_enroll_map_preview_gain (&self->enroll_map, cov_image, pose,
                                                  self->frame_width, self->frame_height);
        if (gain < ELANPRESS_ENROLL_MIN_GAIN)
          {
            fp_dbg ("enroll redundant touch: attempt=%u strips=%u pairs=%u zncc=%.3f gain=%.4f",
                    self->enroll_attempts, self->enroll_images->len, pairs, zncc, gain);
            if (self->enroll_attempts >= ELANPRESS_ENROLL_MAX_ATTEMPTS &&
                self->enroll_images->len >= ELANPRESS_ENROLL_MIN_STRIPS)
              {
                self->enroll_stage = ELANPRESS_ENROLL_STAGES;
                fpi_device_enroll_progress (dev, self->enroll_stage, NULL, NULL);
                elanpress_enroll_finalize (dev);
                return;
              }
            elanpress_enroll_telemetry_emit ("elan", self->frame_width, self->frame_height,
                                             "redundant", self->enroll_stage, ELANPRESS_ENROLL_STAGES,
                                             self->enroll_attempts, early_fd.norm_stddev, early_fd.mean_delta,
                                             self->enroll_pg, &self->enroll_map);
            fpi_device_enroll_progress (dev, self->enroll_stage, NULL,
                                        fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER));
            elanpress_capture_touch (self, elanpress_enroll_touch_done);
            return;
          }
      }

    /* Accept clean strip (stitched OR outer edge that can bridge later) and rebuild pose graph
     * using ZNCC-verified stitching so genuine slightly-shifted overlaps snap into the core. */
    guint new_idx = self->enroll_images->len;
    guint kp_s3_n = s3_feat->n;

    g_ptr_array_add (self->enroll_images, g_steal_pointer (&cov_image));
    g_ptr_array_add (self->enroll_s3_feats, g_steal_pointer (&s3_feat));

    g_clear_pointer (&self->enroll_pg, elanpress_pose_graph_free);
    self->enroll_pg = elanpress_pose_graph_build_verified (
      (ElanpressFeatures * const *) self->enroll_s3_feats->pdata,
      self->enroll_images,
      self->enroll_s3_feats->len,
      self->frame_width, self->frame_height,
      ELANPRESS_ENROLL_STITCH_MIN_PAIRS);

    elanpress_enroll_map_compute (self->enroll_images, self->enroll_pg, new_idx,
                                  self->frame_width, self->frame_height,
                                  ELANPRESS_ENROLL_MIN_STRIPS, &self->enroll_map);
    self->enroll_consec_fail = 0;

    gboolean done = self->enroll_map.complete ||
                    self->enroll_images->len >= ELANPRESS_ENROLL_MAX_STRIPS ||
                    (self->enroll_attempts >= ELANPRESS_ENROLL_MAX_ATTEMPTS &&
                     self->enroll_images->len >= ELANPRESS_ENROLL_MIN_STRIPS);

    int mapped = done
                   ? ELANPRESS_ENROLL_STAGES
                   : CLAMP ((int) ((self->enroll_map.progress_pct * (ELANPRESS_ENROLL_STAGES - 1)) / 100),
                            1, ELANPRESS_ENROLL_STAGES - 1);
    if (mapped > self->enroll_stage)
      self->enroll_stage = mapped;

    fp_dbg ("enroll strip=%u/%u stitched=%u cov=%.1f%% md=%.0f sd=%.1f kp=%u pairs=%u zncc=%.3f pct=%u%%",
            self->enroll_images->len, ELANPRESS_ENROLL_MAX_STRIPS,
            self->enroll_map.stitched_strips, early_fd.coverage, early_fd.mean_delta,
            early_fd.norm_stddev, kp_s3_n, pairs, zncc, self->enroll_map.progress_pct);

    elanpress_enroll_telemetry_emit ("elan", self->frame_width, self->frame_height,
                                     done ? "complete" : "accepted",
                                     self->enroll_stage, ELANPRESS_ENROLL_STAGES,
                                     self->enroll_attempts, early_fd.norm_stddev, early_fd.mean_delta,
                                     self->enroll_pg, &self->enroll_map);
    fpi_device_enroll_progress (dev, self->enroll_stage, NULL, NULL);

    if (!done)
      {
        elanpress_capture_touch (self, elanpress_enroll_touch_done);
        return;
      }

    elanpress_enroll_finalize (dev);
  }
}

static void
elanpress_enroll (FpDevice *dev)
{
  FpiDeviceElanPress *self = FPI_DEVICE_ELANPRESS (dev);

  self->check_precap = FALSE;
  self->from_precap = FALSE;
  self->enroll_stage = 0;
  self->enroll_attempts = 0;
  self->enroll_consec_fail = 0;
  self->enroll_consec_sat = 0;
  self->finger_off_frames = 0;
  memset (&self->enroll_map, 0, sizeof (self->enroll_map));
  g_clear_pointer (&self->enroll_images, g_ptr_array_unref);
  g_clear_pointer (&self->enroll_s3_feats, g_ptr_array_unref);
  g_clear_pointer (&self->enroll_pg, elanpress_pose_graph_free);
  self->enroll_images = g_ptr_array_new_with_free_func (g_free);
  self->enroll_s3_feats = g_ptr_array_new_with_free_func ((GDestroyNotify) elanpress_features_free);

  elanpress_enroll_telemetry_emit ("elan", self->frame_width, self->frame_height,
                                   "started", 0, ELANPRESS_ENROLL_STAGES,
                                   0, 0.0, 0.0, NULL, &self->enroll_map);
  elanpress_capture_touch (self, elanpress_enroll_touch_done);
}

/* === verify and identify === */

static void
elanpress_match_cleanup (FpiDeviceElanPress *self)
{
  g_clear_pointer (&self->score_sums, g_free);
  self->num_score_sums = 0;
}

/* one running total per candidate print, allocated on the first press */
static gboolean
elanpress_match_alloc_sums (FpiDeviceElanPress *self, guint n)
{
  if (n == 0)
    return FALSE;

  if (!self->score_sums)
    {
      self->score_sums = g_new0 (gdouble, n);
      self->num_score_sums = n;
    }

  return self->num_score_sums == n;
}

/* decide on the mean score across the presses that were sampled */
static void
elanpress_match_report (FpDevice *dev)
{
  FpiDeviceElanPress *self = FPI_DEVICE_ELANPRESS (dev);
  FpiDeviceAction action = fpi_device_get_current_action (dev);
  FpPrint *matched_print = NULL;
  gdouble best_mean = -1.0;
  guint best_index = 0;
  gboolean matched;

  elanpress_send_stop (dev);

  for (guint i = 0; i < self->num_score_sums; i++)
    {
      gdouble mean = self->score_sums[i] / self->match_sample;

      if (mean > best_mean)
        {
          best_mean = mean;
          best_index = i;
        }
    }

  matched = self->match_sample > 0 && best_mean >= ELANPRESS_CANDIDATE_MIN_PAIRS;

  fp_dbg ("%s on the mean of %d press(es): %.1f pairs (threshold %d)",
          matched ? "MATCH" : "no match", self->match_sample,
          best_mean, ELANPRESS_CANDIDATE_MIN_PAIRS);

  elanpress_match_cleanup (self);

  if (action == FPI_DEVICE_ACTION_IDENTIFY)
    {
      GPtrArray *gallery = NULL;

      fpi_device_get_identify_data (dev, &gallery);
      if (matched && gallery && best_index < gallery->len)
        matched_print = g_ptr_array_index (gallery, best_index);

      fpi_device_identify_report (dev, matched_print, NULL, NULL);
      fpi_device_identify_complete (dev, NULL);
    }
  else
    {
      fpi_device_verify_report (dev,
                                matched ? FPI_MATCH_SUCCESS : FPI_MATCH_FAIL,
                                NULL, NULL);
      fpi_device_verify_complete (dev, NULL);
    }
}

/* give up after too many presses that yielded nothing to score */
static void
elanpress_match_report_retry (FpDevice *dev)
{
  FpiDeviceElanPress *self = FPI_DEVICE_ELANPRESS (dev);
  FpiDeviceAction action = fpi_device_get_current_action (dev);
  GError *retry = fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL);

  elanpress_match_cleanup (self);
  elanpress_send_stop (dev);

  if (action == FPI_DEVICE_ACTION_IDENTIFY)
    {
      fpi_device_identify_report (dev, NULL, NULL, retry);
      fpi_device_identify_complete (dev, NULL);
    }
  else
    {
      fpi_device_verify_report (dev, FPI_MATCH_ERROR, NULL, retry);
      fpi_device_verify_complete (dev, NULL);
    }
}

static void
elanpress_match_touch_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceElanPress *self = FPI_DEVICE_ELANPRESS (dev);
  FpiDeviceAction action = fpi_device_get_current_action (dev);
  g_autofree guint8 *cov_probe = NULL;
  g_autoptr(ElanpressFeatures) s3_feat = NULL;
  const ElanpressSiftParams p_s3 = {
    .contrast_thr = 0.0f,
    .edge_ratio = ELANPRESS_CANDIDATE_EDGE_RATIO,
    .rootsift = TRUE,
  };
  int probe_frames = self->num_frames;
  gboolean was_precap = self->from_precap;
  gdouble quality = 0.0;
  gdouble t_ext_ms = 0.0;
  gint press_best = -1;

  fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NONE);

  if (error)
    {
      elanpress_match_cleanup (self);
      elanpress_send_stop (dev);
      if (action == FPI_DEVICE_ACTION_IDENTIFY)
        fpi_device_identify_complete (dev, error);
      else
        fpi_device_verify_complete (dev, error);
      return;
    }

  unsigned int size = elanpress_frame_size (self);
  ElanpressFrameDiag early_fd = { 0 };
  const unsigned short *raw_early = self->num_frames > 0 ? g_slist_last (self->frames)->data : NULL;

  cov_probe = elanpress_process_frame_covered (raw_early, self->background, size);
  elanpress_frame_diag (raw_early, self->background, cov_probe, size, &early_fd);
  elanpress_reset_capture (self);

  if (cov_probe)
    {
      quality = early_fd.norm_stddev;

      /* too little ridge detail to say anything about whose finger this is */
      if (quality < ELANPRESS_MIN_QUALITY_STDDEV)
        {
          fp_dbg ("press too low-contrast to score (std-dev %.1f < %.1f)",
                  quality, ELANPRESS_MIN_QUALITY_STDDEV);
          g_clear_pointer (&cov_probe, g_free);
        }
    }

  /* whatever happens next needs its own press */
  self->wait_for_finger_off = TRUE;
  self->finger_off_frames = 0;

  if (!cov_probe)
    {
      if (was_precap)
        {
          fp_dbg ("SSO precap frame unusable (sd=%.1f cov=%.1f%%), falling back to live touch",
                  early_fd.norm_stddev, early_fd.coverage);
          elanpress_capture_touch (self, elanpress_match_touch_done);
          return;
        }
      /* a press that produced nothing is not evidence either way, so ask
       * again rather than averaging it in */
      if (self->match_retries < ELANPRESS_MATCH_MAX_RETRIES)
        {
          self->match_retries++;
          fp_dbg ("unusable press, asking again (retry %d of %d)",
                  self->match_retries, ELANPRESS_MATCH_MAX_RETRIES);
          elanpress_capture_touch (self, elanpress_match_touch_done);
          return;
        }

      elanpress_match_report_retry (dev);
      return;
    }

  {
    gint64 t0 = g_get_monotonic_time ();
    s3_feat = elanpress_sift_features_opts (cov_probe, self->frame_width,
                                            self->frame_height,
                                            ELANPRESS_MAX_KEYPOINTS, &p_s3, NULL);
    t_ext_ms = (g_get_monotonic_time () - t0) / 1000.0;
  }

  if (action == FPI_DEVICE_ACTION_IDENTIFY)
    {
      GPtrArray *gallery = NULL;

      fpi_device_get_identify_data (dev, &gallery);

      if (!elanpress_match_alloc_sums (self, gallery ? gallery->len : 0))
        {
          elanpress_match_cleanup (self);
          elanpress_send_stop (dev);
          fpi_device_identify_report (dev, NULL, NULL, NULL);
          fpi_device_identify_complete (dev, NULL);
          return;
        }

      gdouble sum_deser = 0.0, sum_pg = 0.0, sum_zncc = 0.0;
      gint winner_idx = -1;

      for (guint i = 0; i < gallery->len; i++)
        {
          FpPrint *print = g_ptr_array_index (gallery, i);
          gboolean cand_matched = FALSE;
          gdouble zncc = 0.0;
          guint ov = 0;
          gdouble td = 0.0, tp = 0.0, tz = 0.0;
          gint c = elanpress_match_v3_print (self, cov_probe, s3_feat, print,
                                             &cand_matched, &zncc, &ov,
                                             &td, &tp, &tz);
          gint effective = (c >= 0 && cand_matched) ? MAX (c, ELANPRESS_CANDIDATE_MIN_PAIRS) : 0;

          sum_deser += td;
          sum_pg += tp;
          sum_zncc += tz;

          self->score_sums[i] += effective;
          if (effective > press_best)
            {
              press_best = effective;
              winner_idx = effective > 0 ? (gint) i : -1;
            }
        }

      fp_dbg ("identify n=%u winner=%d cov=%.1f%% md=%.0f kp=%u cpu=%.1fms (deser=%.2f ext=%.1f pg=%.1f zncc=%.2f)",
              gallery->len, winner_idx, early_fd.coverage, early_fd.mean_delta,
              s3_feat ? s3_feat->n : 0,
              sum_deser + t_ext_ms + sum_pg + sum_zncc,
              sum_deser, t_ext_ms, sum_pg, sum_zncc);
    }
  else
    {
      FpPrint *print = NULL;
      gboolean cand_matched = FALSE;
      gdouble zncc = 0.0;
      guint ov = 0;
      gdouble t_deser_ms = 0.0, t_pg_ms = 0.0, t_zncc_ms = 0.0;
      gint c;
      gint effective;

      fpi_device_get_verify_data (dev, &print);
      c = elanpress_match_v3_print (self, cov_probe, s3_feat, print,
                                    &cand_matched, &zncc, &ov,
                                    &t_deser_ms, &t_pg_ms, &t_zncc_ms);

      effective = (c >= 0 && cand_matched) ? MAX (c, ELANPRESS_CANDIDATE_MIN_PAIRS) : 0;

      fp_dbg ("verify match=%d pairs=%d zncc=%.3f ov=%u cov=%.1f%% md=%.0f cpu=%.1fms",
              cand_matched ? 1 : 0, c, zncc, ov, early_fd.coverage, early_fd.mean_delta,
              t_deser_ms + t_ext_ms + t_pg_ms + t_zncc_ms);

      /* the stored print itself is unusable; more presses cannot help */
      if (c < 0)
        {
          elanpress_match_cleanup (self);
          elanpress_send_stop (dev);
          fpi_device_verify_complete (dev,
                                      fpi_device_error_new (FP_DEVICE_ERROR_DATA_INVALID));
          return;
        }

      if (!elanpress_match_alloc_sums (self, 1))
        {
          elanpress_match_cleanup (self);
          elanpress_send_stop (dev);
          fpi_device_verify_complete (dev,
                                      fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
          return;
        }

      self->score_sums[0] += effective;
      press_best = effective;
    }

  if (was_precap && press_best < ELANPRESS_CANDIDATE_MIN_PAIRS)
    {
      /* Pre-captured power-on image did not match (e.g. non-enrolled finger or
       * edge click during boot): do NOT waste a verify attempt, fall smoothly
       * into live touch waiting! */
      fp_dbg ("SSO precap frame did not match (best=%d < %d), waiting for live touch",
              press_best, ELANPRESS_CANDIDATE_MIN_PAIRS);
      elanpress_match_cleanup (self);
      self->match_sample = 0;
      elanpress_capture_touch (self, elanpress_match_touch_done);
      return;
    }

  self->match_sample++;

  fp_dbg ("press %d of %d%s: %d pairs from %d frame(s), %u keypoints, std-dev %.1f",
          self->match_sample, ELANPRESS_MATCH_SAMPLES,
          was_precap ? " [SSO-PRECAP]" : "",
          press_best, probe_frames, s3_feat ? s3_feat->n : 0, quality);

  if (self->match_sample < ELANPRESS_MATCH_SAMPLES)
    {
      elanpress_capture_touch (self, elanpress_match_touch_done);
      return;
    }

  elanpress_match_report (dev);
}

static void
elanpress_identify_verify (FpDevice *dev)
{
  FpiDeviceElanPress *self = FPI_DEVICE_ELANPRESS (dev);

  self->match_sample = 0;
  self->match_retries = 0;
  self->finger_off_frames = 0;
  elanpress_match_cleanup (self);

  /* First check sensor SRAM for an ELAN EMSE1000 pre-captured power-on/wake image */
  self->check_precap = TRUE;
  elanpress_capture_touch (self, elanpress_match_touch_done);
}

/* === open and close === */

enum open_states {
  OPEN_GET_DIM_SEND,
  OPEN_GET_DIM_READ,
  OPEN_NUM_STATES,
};

static void
elanpress_dim_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                  gpointer user_data, GError *error)
{
  FpiDeviceElanPress *self = FPI_DEVICE_ELANPRESS (dev);

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  /* raw frames are column-major: byte 0 is the column height, byte 2 the
   * number of columns (see the elan driver) */
  self->frame_height = transfer->buffer[0];
  self->frame_width = transfer->buffer[2];

  /* work around sensors returning the sizes as zero-based index rather
   * than the number of pixels (same quirk as the elan driver) */
  if ((self->frame_width % 2 == 1) && (self->frame_height % 2 == 1))
    {
      self->frame_width++;
      self->frame_height++;
    }
  fp_dbg ("sensor dimensions, WxH: %dx%d", self->frame_width,
          self->frame_height);

  if (self->frame_width < 50 || self->frame_height < 20)
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Unexpected sensor dimensions %dx%d",
                                                     self->frame_width,
                                                     self->frame_height));
      return;
    }

  fpi_ssm_next_state (transfer->ssm);
}

static void
open_run_state (FpiSsm *ssm, FpDevice *dev)
{
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case OPEN_GET_DIM_SEND:
      elanpress_send_cmd (ssm, dev, cmd_get_sensor_dim);
      break;

    case OPEN_GET_DIM_READ:
      elanpress_read (ssm, dev, ELANPRESS_EP_CMD_IN, 4,
                      ELANPRESS_CMD_TIMEOUT, elanpress_dim_cb);
      break;
    }
}

static void
open_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  fpi_device_open_complete (dev, error);
}

static void
elanpress_open (FpDevice *dev)
{
  FpiDeviceElanPress *self = FPI_DEVICE_ELANPRESS (dev);
  GError *error = NULL;

  /* a finger already on the sensor when the device is opened is the press
   * the user meant for it, so it is imaged rather than waited out */
  self->wait_for_finger_off = FALSE;

  if (!g_usb_device_claim_interface (fpi_device_get_usb_device (dev),
                                     0, 0, &error))
    {
      fpi_device_open_complete (dev, error);
      return;
    }

  fpi_ssm_start (fpi_ssm_new (dev, open_run_state, OPEN_NUM_STATES),
                 open_complete);
}

static void
elanpress_close (FpDevice *dev)
{
  FpiDeviceElanPress *self = FPI_DEVICE_ELANPRESS (dev);
  GError *error = NULL;

  elanpress_reset_capture (self);
  elanpress_match_cleanup (self);
  g_clear_pointer (&self->background, g_free);
  g_clear_pointer (&self->enroll_images, g_ptr_array_unref);
  g_clear_pointer (&self->enroll_s3_feats, g_ptr_array_unref);
  g_clear_pointer (&self->enroll_pg, elanpress_pose_graph_free);

  g_usb_device_release_interface (fpi_device_get_usb_device (dev),
                                  0, 0, &error);
  fpi_device_close_complete (dev, error);
}

static void
elanpress_cancel (FpDevice *dev)
{
  /* in-flight transfers are submitted with the action's cancellable, so
   * they fail on their own; nothing device-side to do here */
}

static void
fpi_device_elanpress_init (FpiDeviceElanPress *self)
{
}

static void
fpi_device_elanpress_class_init (FpiDeviceElanPressClass *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  dev_class->id = "elanpress";
  dev_class->full_name = "ElanTech press-type fingerprint sensor";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = elanpress_id_table;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->nr_enroll_stages = ELANPRESS_ENROLL_STAGES;
  /* the sensor spends lock-screen sessions waiting for a finger; it is a
   * tiny capacitive sensor designed to be always-on */
  dev_class->temp_hot_seconds = -1;

  dev_class->open = elanpress_open;
  dev_class->close = elanpress_close;
  dev_class->enroll = elanpress_enroll;
  dev_class->verify = elanpress_identify_verify;
  dev_class->identify = elanpress_identify_verify;
  dev_class->cancel = elanpress_cancel;

  fpi_device_class_auto_initialize_features (dev_class);
}
