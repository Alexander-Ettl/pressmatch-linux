/*
 * Focal-systems FT9201 (2808:93a9) native open-source libfprint driver
 *
 * USB bring-up & 96x96 frame capture protocol adapted from OMGrant/ft9201-libfprint
 * (LGPLv2.1+, Copyright (C) 2026 Grant Garrison) and banianitc/ft9201-fingerprint-driver.
 * Matching engine replaced with the 100% open-source v4.1 pressmatch core
 * (RootSIFT + ZNCC-verified Pose Graph + Elastic ZNCC) operating at full 96x96 resolution
 * with full multi-finger identify support — zero proprietary Windows DLLs or PE loaders.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define FP_COMPONENT "ft9201"
#include "drivers_api.h"
#include "../elanpress-match.h"
#include "../elanpress-sift.h"
#include "ft9201_fw.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FT9201_EP_OUT           0x02
#define FT9201_EP_IN            0x83

/* Control request codes */
#define REQ_WAKEUP              0x22
#define REQ_HW_REG              0x03
#define REQ_BULK_ARM            0x34
#define REQ_BULK_SIZE           0x35
#define REQ_SUI_CMD             0x64
#define REQ_SUI_READ            0x60
#define REQ_STATUS              0x30
#define REQ_MCU_DL_MODE         0x57
#define REQ_SPI_WRITE           0x66
#define REQ_SPI_READ            0x65
#define REQ_AFE_READ            0x3a
#define REQ_AFE_WRITE           0x3b
#define REQ_SENSOR_RESET        0x40

/* AFE register indices */
#define REG_DIM_W               0x14
#define REG_DIM_H               0x15
#define REG_MCU_STATUS          0x20
#define REG_FINGER              0x1d
#define REG_VARIANT_CFG1        0x22
#define REG_VARIANT_CFG2        0x23
#define REG_POWER_1F            0x1f
#define REG_POWER_1E            0x1e

#define CTRL_TIMEOUT            5000
#define BULK_TIMEOUT            5000

#define DEFAULT_WIDTH           96
#define DEFAULT_HEIGHT          96
#define FW_CHUNK                64

/* Shared v4.1 pressmatch configuration for 96x96 square sensor */
#define FT9201_PRINT_VERSION    4
#define FT9201_ENROLL_STAGES    10
#define FT9201_ENROLL_MIN_STRIP 8
#define FT9201_ENROLL_MAX_STRIP 14
#define FT9201_ENROLL_MAX_ATT   24
#define FT9201_ENROLL_MAX_KP    200
#define FT9201_PROBE_MAX_KP     300
#define FT9201_EDGE_RATIO       25.0f
#define FT9201_STITCH_MIN_PAIRS 8
#define FT9201_MIN_GAIN         0.003

/* v4.1 multi-tier match thresholds (calibrated via 405,600-trial 96x96 benchmark) */
#define FT9201_T0_PAIRS         14
#define FT9201_T0_ZNCC          0.25
#define FT9201_T0_OV            400

#define FT9201_T1_PAIRS         10
#define FT9201_T1_ZNCC          0.45
#define FT9201_T1_OV            400

#define FT9201_T2_PAIRS         8
#define FT9201_T2_ZNCC          0.60
#define FT9201_T2_OV            1200

#define FT9201_T3_PAIRS         6
#define FT9201_T3_ZNCC          0.72
#define FT9201_T3_OV            2000

struct _FpiDeviceFt9201
{
  FpDevice            parent;

  FpiSsm             *loop_ssm;
  guint8              sensor_width;
  guint8              sensor_height;
  guint8             *image_buf;
  guint8             *background_buf;
  gboolean            has_background;
  gboolean            scan_ok;

  /* Enrollment state */
  GPtrArray          *enroll_images;
  GPtrArray          *enroll_feats;
  ElanpressPoseGraph *enroll_pg;
  ElanpressEnrollMap  enroll_map;
  int                 enroll_stage;
  guint               enroll_attempts;
  guint               enroll_consec_fail;

  /* USB init scratch */
  guint8              sensor_variant;
  guint8              reg_buf[4];
  guint8              fw_chunk_buf[FW_CHUNK];
  guint               fw_offset;
  guint               fw_retries;
};

G_DECLARE_FINAL_TYPE (FpiDeviceFt9201, fpi_device_ft9201,
                      FPI, DEVICE_FT9201, FpDevice)
G_DEFINE_TYPE (FpiDeviceFt9201, fpi_device_ft9201, FP_TYPE_DEVICE)

/* ================================================================== */
/* Image normalization & v4.1 print serialization                      */
/* ================================================================== */

static int
ft9201_cmp_u8 (const void *a, const void *b)
{
  return (int) (*(const guint8 *) a) - (int) (*(const guint8 *) b);
}

/* 2-step percentile tone curve matching pressmatch's 1..255 covered range */
static guint8 *
ft9201_normalize_frame (const guint8 *raw, const guint8 *bg,
                        gboolean has_bg, gsize size)
{
  g_autofree guint8 *fpn = g_malloc (size);
  g_autofree guint8 *sorted = g_malloc (size);
  guint8 *out = g_malloc (size);
  int l0, l1, l2, l3, d01, d12, d23;

  for (gsize i = 0; i < size; i++)
    {
      gint v = has_bg ? ((gint) raw[i] - (gint) bg[i] + 128) : (gint) raw[i];
      fpn[i] = (guint8) CLAMP (v, 0, 255);
    }

  memcpy (sorted, fpn, size);
  qsort (sorted, size, 1, ft9201_cmp_u8);

  l0 = sorted[size / 100];
  l1 = sorted[(size * 3) / 10];
  l2 = sorted[(size * 65) / 100];
  l3 = sorted[(size * 99) / 100];
  d01 = MAX (l1 - l0, 1);
  d12 = MAX (l2 - l1, 1);
  d23 = MAX (l3 - l2, 1);

  for (gsize i = 0; i < size; i++)
    {
      int px = fpn[i];
      int v;
      if (px <= l0)
        v = 1;
      else if (px < l1)
        v = 1 + ((px - l0) * 98) / d01;
      else if (px < l2)
        v = 99 + ((px - l1) * 56) / d12;
      else if (px >= l3)
        v = 255;
      else
        v = 155 + MIN (((px - l2) * 100) / d23, 100);
      out[i] = (guint8) v;
    }

  return out;
}

static void
ft9201_print_set_data (FpiDeviceFt9201          *self,
                       FpPrint                  *print,
                       GPtrArray                *images,
                       const ElanpressPoseGraph *pg)
{
  GVariantBuilder images_builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("aay"));
  g_autoptr(GBytes) pg_bytes = elanpress_pose_graph_serialize (pg);
  gsize pg_len = 0;
  gconstpointer pg_ptr = pg_bytes ? g_bytes_get_data (pg_bytes, &pg_len) : NULL;
  unsigned int frame_sz = (unsigned int) self->sensor_width * self->sensor_height;
  GVariant *pg_v;
  GVariant *fpi_data;

  for (guint i = 0; i < images->len; i++)
    {
      gsize packed_len = 0;
      g_autofree guint8 *packed =
        elanpress_pack_image_4bit (g_ptr_array_index (images, i),
                                   frame_sz, &packed_len);
      g_variant_builder_add_value (
        &images_builder,
        g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                   packed, packed_len, sizeof (guint8)));
    }

  pg_v = g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, pg_ptr, pg_len, sizeof (guint8));
  fpi_data = g_variant_new ("(qqqaay@ay)",
                            (guint16) FT9201_PRINT_VERSION,
                            (guint16) self->sensor_width,
                            (guint16) self->sensor_height,
                            &images_builder,
                            pg_v);

  fpi_print_set_type (print, FPI_PRINT_RAW);
  g_object_set (print, "fpi-data", fpi_data, NULL);
}

static gboolean
ft9201_print_get_data (FpiDeviceFt9201     *self,
                       FpPrint             *print,
                       GPtrArray          **images_out,
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
  unsigned int frame_sz = (unsigned int) self->sensor_width * self->sensor_height;

  g_object_get (print, "fpi-data", &fpi_data, NULL);
  if (!fpi_data || !g_variant_check_format_string (fpi_data, "(qqq@aay@ay)", FALSE))
    return FALSE;

  g_variant_get (fpi_data, "(qqq@aay@ay)", &version, &width, &height, &images_v, &pg_v);
  if (version != FT9201_PRINT_VERSION ||
      width != self->sensor_width || height != self->sensor_height)
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
      guint8 *unpacked = elanpress_unpack_image_4bit (data, len, frame_sz);

      if (!unpacked)
        {
          g_ptr_array_unref (images);
          elanpress_pose_graph_free (pg);
          return FALSE;
        }
      g_ptr_array_add (images, unpacked);
    }

  *images_out = images;
  *pg_out = pg;
  return TRUE;
}

static gint
ft9201_match_print (FpiDeviceFt9201         *self,
                    const guint8            *probe_img,
                    const ElanpressFeatures *probe_feat,
                    FpPrint                 *print,
                    gboolean                *matched_out,
                    gdouble                 *zncc_out,
                    guint                   *ov_out)
{
  g_autoptr(GPtrArray) images = NULL;
  ElanpressPoseGraph *pg = NULL;
  guint comp = 0, ov = 0, pairs = 0;
  ElanpressPose pose = { 1.0f, 0.0f, 0.0f, 0.0f };
  gdouble zncc_rigid = 0.0, zncc_elastic = 0.0;

  if (!ft9201_print_get_data (self, print, &images, &pg))
    return -1;

  pairs = elanpress_pose_graph_match_pose (probe_feat, pg, TRUE, &comp, &pose);
  if (pairs > 0)
    zncc_elastic = elanpress_pose_graph_zncc_elastic (probe_img, images, 1, 0,
                                                      pg, comp, pose,
                                                      self->sensor_width,
                                                      self->sensor_height,
                                                      1, &zncc_rigid, &ov);

  elanpress_pose_graph_free (pg);

  if (zncc_out)
    *zncc_out = zncc_elastic;
  if (ov_out)
    *ov_out = ov;
  if (matched_out)
    *matched_out =
      (pairs >= FT9201_T0_PAIRS && zncc_elastic >= FT9201_T0_ZNCC && ov >= FT9201_T0_OV) ||
      (pairs >= FT9201_T1_PAIRS && zncc_elastic >= FT9201_T1_ZNCC && ov >= FT9201_T1_OV) ||
      (pairs >= FT9201_T2_PAIRS && zncc_elastic >= FT9201_T2_ZNCC && ov >= FT9201_T2_OV) ||
      (pairs >= FT9201_T3_PAIRS && zncc_elastic >= FT9201_T3_ZNCC && ov >= FT9201_T3_OV);

  return (gint) pairs;
}

/* ================================================================== */
/* USB helpers                                                         */
/* ================================================================== */

static void
ctrl_out_cb (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err)
{
  if (err)
    fpi_ssm_mark_failed (t->ssm, err);
  else
    fpi_ssm_next_state (t->ssm);
}

static void
ctrl_out (FpiSsm *ssm, FpDevice *dev, guint8 req, guint16 val, guint16 idx)
{
  FpiUsbTransfer *t = fpi_usb_transfer_new (dev);
  t->ssm = ssm;
  fpi_usb_transfer_fill_control (t,
                                 G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
                                 G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                 G_USB_DEVICE_RECIPIENT_DEVICE,
                                 req, val, idx, 0);
  fpi_usb_transfer_submit (t, CTRL_TIMEOUT,
                           fpi_device_get_cancellable (dev), ctrl_out_cb, NULL);
}

static void
ctrl_in (FpiSsm *ssm, FpDevice *dev,
         guint8 req, guint16 val, guint16 idx, guint16 len,
         FpiUsbTransferCallback cb)
{
  FpiUsbTransfer *t = fpi_usb_transfer_new (dev);
  t->ssm = ssm;
  fpi_usb_transfer_fill_control (t,
                                 G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
                                 G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                 G_USB_DEVICE_RECIPIENT_DEVICE,
                                 req, val, idx, len);
  fpi_usb_transfer_submit (t, CTRL_TIMEOUT,
                           fpi_device_get_cancellable (dev), cb, NULL);
}

/* ================================================================== */
/* Init state machine                                                  */
/* ================================================================== */

enum {
  INIT_WAKE1,
  INIT_WAKE2,
  INIT_REG90_SETUP,
  INIT_REG90_READ,
  INIT_F906,
  INIT_BULK_ARM,
  INIT_BULK_SIZE4,
  INIT_BULK_PAYLOAD,
  INIT_HW_A4,
  INIT_SENSOR_STATUS,

  INIT_MCU_DL,
  INIT_SPI_W_C8,
  INIT_SPI_R_C8,
  INIT_SPI_W_F1,
  INIT_SPI_R_F4,
  INIT_SPI_W_F4,
  INIT_SPI_R_F3,
  INIT_SPI_FIN,

  INIT_FW_WAKE1,
  INIT_FW_WAKE2,
  INIT_FW_MCU_DL,
  INIT_FW_RESET_EP,
  INIT_FW_DL_MODE,
  INIT_FW_BRIDGE_VER,
  INIT_FW_CHUNK_SIZE,
  INIT_FW_UPLOAD,

  INIT_CFG_R01,
  INIT_CFG_R41,
  INIT_CFG_R30,
  INIT_CFG_R30_RD,
  INIT_CFG_R22,
  INIT_CFG_R23,
  INIT_CFG_R54,
  INIT_CFG_R1F,
  INIT_CFG_R1E,

  INIT_SENSOR_RESET,
  INIT_RESET_DELAY,
  INIT_POST_WAKE1,
  INIT_POST_WAKE2,
  INIT_MCU_WAIT,
  INIT_MCU_POLL,

  INIT_READ_W,
  INIT_READ_H,

  INIT_POWER_CFG1,
  INIT_POWER_CFG2,
  INIT_POWER_1F,
  INIT_POWER_1E,

  INIT_NUM_STATES,
};

static void
ignore_read_cb (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err)
{
  if (err)
    g_error_free (err);
  fpi_ssm_next_state (t->ssm);
}

static void
bulk_payload_cb (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err)
{
  if (err)
    fpi_ssm_mark_failed (t->ssm, err);
  else
    fpi_ssm_next_state (t->ssm);
}

static void
spi_r_f4_cb (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err)
{
  FpiDeviceFt9201 *self = FPI_DEVICE_FT9201 (dev);
  if (err)
    {
      fpi_ssm_mark_failed (t->ssm, err);
      return;
    }
  self->reg_buf[0] = t->buffer[0];
  fpi_ssm_next_state (t->ssm);
}

static void
spi_r_f3_cb (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err)
{
  FpiDeviceFt9201 *self = FPI_DEVICE_FT9201 (dev);
  if (err)
    {
      fpi_ssm_mark_failed (t->ssm, err);
      return;
    }
  self->sensor_variant = (t->buffer[0] >> 1) & 0x0f;
  fp_dbg ("ft9201: SPI type_raw=0x%02x variant=%u", t->buffer[0], self->sensor_variant);
  fpi_ssm_next_state (t->ssm);
}

static void
fw_chunk_cb (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err)
{
  FpiDeviceFt9201 *self = FPI_DEVICE_FT9201 (dev);
  if (err)
    {
      fpi_ssm_mark_failed (t->ssm, err);
      return;
    }
  self->fw_offset += FW_CHUNK;
  fpi_ssm_jump_to_state (t->ssm, INIT_FW_UPLOAD);
}

static void
mcu_poll_cb (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err)
{
  FpiDeviceFt9201 *self = FPI_DEVICE_FT9201 (dev);
  if (err)
    {
      fpi_ssm_mark_failed (t->ssm, err);
      return;
    }
  if (t->buffer[0] == 0xa5 && t->buffer[1] == 0x5a)
    {
      fp_dbg ("ft9201: MCU ready after %u polls", self->fw_retries + 1);
      fpi_ssm_next_state (t->ssm);
      return;
    }
  if (++self->fw_retries > 200)
    {
      fpi_ssm_mark_failed (t->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                     "ft9201: MCU did not boot"));
      return;
    }
  fpi_ssm_jump_to_state (t->ssm, INIT_MCU_WAIT);
}

static void
read_w_cb (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err)
{
  FpiDeviceFt9201 *self = FPI_DEVICE_FT9201 (dev);
  if (err)
    {
      fpi_ssm_mark_failed (t->ssm, err);
      return;
    }
  self->sensor_width = t->buffer[0] ? t->buffer[0] : DEFAULT_WIDTH;
  fpi_ssm_next_state (t->ssm);
}

static void
read_h_cb (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err)
{
  FpiDeviceFt9201 *self = FPI_DEVICE_FT9201 (dev);
  if (err)
    {
      fpi_ssm_mark_failed (t->ssm, err);
      return;
    }
  self->sensor_height = t->buffer[0] ? t->buffer[0] : DEFAULT_HEIGHT;
  fp_dbg ("ft9201: native sensor dimensions %ux%u", self->sensor_width, self->sensor_height);
  fpi_ssm_next_state (t->ssm);
}

static void
init_ssm_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFt9201 *self = FPI_DEVICE_FT9201 (dev);
  FpiUsbTransfer *t;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case INIT_WAKE1:
    case INIT_WAKE2:
    case INIT_FW_WAKE1:
    case INIT_FW_WAKE2:
    case INIT_POST_WAKE1:
    case INIT_POST_WAKE2:
      ctrl_out (ssm, dev, REQ_WAKEUP, 0x70, 0x70);
      break;
    case INIT_REG90_SETUP:
      ctrl_out (ssm, dev, REQ_SUI_CMD, 0x9001, 0x0000);
      break;
    case INIT_REG90_READ:
      ctrl_in (ssm, dev, REQ_SUI_READ, 0, 0, 1, ignore_read_cb);
      break;
    case INIT_F906:
      ctrl_out (ssm, dev, REQ_SUI_CMD, 0x0603, 0x00f9);
      break;
    case INIT_BULK_ARM:
    case INIT_FW_DL_MODE:
      ctrl_out (ssm, dev, REQ_BULK_ARM, 0x0002, 0x0000);
      break;
    case INIT_BULK_SIZE4:
      ctrl_out (ssm, dev, REQ_BULK_SIZE, 0x0004, 0x85c0);
      break;
    case INIT_BULK_PAYLOAD:
      {
        static const guint8 payload[] = { 0x11, 0xee, 0x02, 0x00 };
        t = fpi_usb_transfer_new (dev);
        t->ssm = ssm;
        fpi_usb_transfer_fill_bulk_full (t, FT9201_EP_OUT,
                                         (guint8 *) payload, sizeof (payload), NULL);
        fpi_usb_transfer_submit (t, BULK_TIMEOUT,
                                 fpi_device_get_cancellable (dev), bulk_payload_cb, NULL);
      }
      break;
    case INIT_HW_A4:
      ctrl_out (ssm, dev, REQ_HW_REG, 0x0001, 0x00a4);
      break;
    case INIT_SENSOR_STATUS:
      ctrl_in (ssm, dev, REQ_STATUS, 0, 0x85c0, 2, ignore_read_cb);
      break;
    case INIT_MCU_DL:
    case INIT_FW_MCU_DL:
      ctrl_out (ssm, dev, REQ_MCU_DL_MODE, 0, 0);
      break;
    case INIT_SPI_W_C8:
      ctrl_out (ssm, dev, REQ_SPI_WRITE, 0x00df, 0x00c8);
      break;
    case INIT_SPI_R_C8:
      ctrl_in (ssm, dev, REQ_SPI_READ, 0, 0x00c8, 1, ignore_read_cb);
      break;
    case INIT_SPI_W_F1:
      ctrl_out (ssm, dev, REQ_SPI_WRITE, 0x001d, 0x00f1);
      break;
    case INIT_SPI_R_F4:
      ctrl_in (ssm, dev, REQ_SPI_READ, 0, 0x00f4, 1, spi_r_f4_cb);
      break;
    case INIT_SPI_W_F4:
      ctrl_out (ssm, dev, REQ_SPI_WRITE, (guint16) (self->reg_buf[0] | 0x01), 0x00f4);
      break;
    case INIT_SPI_R_F3:
      ctrl_in (ssm, dev, REQ_SPI_READ, 0, 0x00f3, 1, spi_r_f3_cb);
      break;
    case INIT_SPI_FIN:
      ctrl_out (ssm, dev, REQ_SPI_WRITE, 0x0000, 0x00f4);
      break;
    case INIT_FW_RESET_EP:
      ctrl_out (ssm, dev, REQ_BULK_ARM, 0x00ff, 0x0000);
      break;
    case INIT_FW_BRIDGE_VER:
      ctrl_in (ssm, dev, 0x1a, 0, 0, 2, ignore_read_cb);
      break;
    case INIT_FW_CHUNK_SIZE:
      self->fw_offset = 0;
      ctrl_out (ssm, dev, REQ_BULK_SIZE, FW_CHUNK, 0);
      break;
    case INIT_FW_UPLOAD:
      if (self->fw_offset >= ft9201_fw_size)
        {
          fp_dbg ("ft9201: uploaded %u bytes of MCU firmware", self->fw_offset);
          fpi_ssm_next_state (ssm);
          break;
        }
      {
        guint remaining = (guint) (ft9201_fw_size - self->fw_offset);
        guint n = MIN (remaining, (guint) FW_CHUNK);
        memset (self->fw_chunk_buf, 0, FW_CHUNK);
        memcpy (self->fw_chunk_buf, ft9201_fw_data + self->fw_offset, n);
        t = fpi_usb_transfer_new (dev);
        t->ssm = ssm;
        fpi_usb_transfer_fill_bulk_full (t, FT9201_EP_OUT,
                                         self->fw_chunk_buf, FW_CHUNK, NULL);
        fpi_usb_transfer_submit (t, BULK_TIMEOUT,
                                 fpi_device_get_cancellable (dev), fw_chunk_cb, NULL);
      }
      break;
    case INIT_CFG_R01:
      ctrl_out (ssm, dev, REQ_AFE_WRITE, 0x01, 0x01);
      break;
    case INIT_CFG_R41:
      ctrl_out (ssm, dev, REQ_AFE_WRITE, 0x0f, 0x41);
      break;
    case INIT_CFG_R30:
      ctrl_out (ssm, dev, REQ_AFE_WRITE, 0xbb, 0x30);
      break;
    case INIT_CFG_R30_RD:
      ctrl_in (ssm, dev, REQ_AFE_READ, 0, 0x30, 1, ignore_read_cb);
      break;
    case INIT_CFG_R22:
      ctrl_out (ssm, dev, REQ_AFE_WRITE, 0x00, 0x22);
      break;
    case INIT_CFG_R23:
      ctrl_out (ssm, dev, REQ_AFE_WRITE, 0x0e, 0x23);
      break;
    case INIT_CFG_R54:
      ctrl_out (ssm, dev, REQ_AFE_WRITE, 0x01, 0x54);
      break;
    case INIT_CFG_R1F:
      ctrl_out (ssm, dev, REQ_AFE_WRITE, 0x01, 0x1f);
      break;
    case INIT_CFG_R1E:
      ctrl_out (ssm, dev, REQ_AFE_WRITE, 0x01, 0x1e);
      break;
    case INIT_SENSOR_RESET:
      ctrl_out (ssm, dev, REQ_SENSOR_RESET, 0, 0);
      break;
    case INIT_RESET_DELAY:
      fpi_ssm_next_state_delayed (ssm, 160);
      break;
    case INIT_MCU_WAIT:
      fpi_ssm_next_state_delayed (ssm, 2);
      break;
    case INIT_MCU_POLL:
      ctrl_in (ssm, dev, REQ_AFE_READ, 0, REG_MCU_STATUS, 2, mcu_poll_cb);
      break;
    case INIT_READ_W:
      ctrl_in (ssm, dev, REQ_AFE_READ, 0, REG_DIM_W, 4, read_w_cb);
      break;
    case INIT_READ_H:
      ctrl_in (ssm, dev, REQ_AFE_READ, 0, REG_DIM_H, 4, read_h_cb);
      break;
    case INIT_POWER_CFG1:
      if (self->sensor_variant == 2 || self->sensor_variant == 3)
        ctrl_out (ssm, dev, REQ_AFE_WRITE, 0x00, REG_VARIANT_CFG1);
      else
        fpi_ssm_next_state (ssm);
      break;
    case INIT_POWER_CFG2:
      if (self->sensor_variant == 2 || self->sensor_variant == 3)
        ctrl_out (ssm, dev, REQ_AFE_WRITE, 0x0e, REG_VARIANT_CFG2);
      else
        fpi_ssm_next_state (ssm);
      break;
    case INIT_POWER_1F:
      if (self->sensor_variant == 2 || self->sensor_variant == 3)
        ctrl_out (ssm, dev, REQ_AFE_WRITE, 0x01, REG_POWER_1F);
      else
        fpi_ssm_next_state (ssm);
      break;
    case INIT_POWER_1E:
      if (self->sensor_variant == 2 || self->sensor_variant == 3)
        ctrl_out (ssm, dev, REQ_AFE_WRITE, 0x01, REG_POWER_1E);
      else
        fpi_ssm_next_state (ssm);
      break;
    default:
      g_assert_not_reached ();
    }
}

static void
init_ssm_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceFt9201 *self = FPI_DEVICE_FT9201 (dev);

  if (!error)
    {
      gsize img_bytes = (gsize) self->sensor_width * self->sensor_height;
      self->image_buf = g_malloc0 (img_bytes + 2);
      self->background_buf = g_malloc0 (img_bytes);
      self->has_background = FALSE;
    }
  fpi_device_open_complete (dev, error);
}

/* ================================================================== */
/* Scan state machine                                                  */
/* ================================================================== */

enum {
  SCAN_BG_WAIT_CLEAR_DELAY,
  SCAN_BG_WAIT_CLEAR,
  SCAN_BG_RESET_EP,
  SCAN_BG_CAP_DELAY,
  SCAN_BG_ARM,
  SCAN_BG_CFG_BULK,
  SCAN_BG_READ,
  SCAN_WAIT_DELAY,
  SCAN_WAIT_FINGER,
  SCAN_SETTLE_DELAY,
  SCAN_START_FF,
  SCAN_CAP_DELAY,
  SCAN_START_03,
  SCAN_CONFIG_BULK,
  SCAN_READ_IMAGE,
  SCAN_WAIT_LIFT_DELAY,
  SCAN_WAIT_LIFT,
  SCAN_NUM_STATES,
};

static void start_scan (FpDevice *dev);

static void
bg_wait_clear_cb (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err)
{
  if (err)
    {
      fpi_ssm_mark_failed (t->ssm, err);
      return;
    }
  if (t->buffer[0] != 0xa0)
    fpi_ssm_next_state (t->ssm);
  else
    fpi_ssm_jump_to_state (t->ssm, SCAN_BG_WAIT_CLEAR_DELAY);
}

static void
wait_finger_cb (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err)
{
  if (err)
    {
      fpi_ssm_mark_failed (t->ssm, err);
      return;
    }
  if (t->buffer[0] == 0xa0)
    {
      fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NEEDED | FP_FINGER_STATUS_PRESENT);
      fpi_ssm_next_state (t->ssm);
    }
  else
    {
      fpi_ssm_jump_to_state (t->ssm, SCAN_WAIT_DELAY);
    }
}

static void
wait_lift_cb (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err)
{
  if (err)
    {
      fpi_ssm_mark_failed (t->ssm, err);
      return;
    }
  if (t->buffer[0] != 0xa0)
    {
      fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NONE);
      fpi_ssm_mark_completed (t->ssm);
    }
  else
    {
      fpi_ssm_jump_to_state (t->ssm, SCAN_WAIT_LIFT_DELAY);
    }
}

static void
scan_bg_image_cb (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err)
{
  FpiDeviceFt9201 *self = FPI_DEVICE_FT9201 (dev);
  gsize img_bytes = (gsize) self->sensor_width * self->sensor_height;

  if (!err && t->actual_length >= (gssize) (img_bytes + 2))
    {
      gdouble bg_sd = elanpress_image_quality (t->buffer + 2, (unsigned int) img_bytes);
      if (bg_sd < 25.0)
        {
          memcpy (self->background_buf, t->buffer + 2, img_bytes);
          self->has_background = TRUE;
          fp_dbg ("ft9201: captured clean FPN background (sd=%.1f)", bg_sd);
        }
      else
        {
          fp_dbg ("ft9201: rejecting contaminated FPN background (sd=%.1f >= 25.0)", bg_sd);
          self->has_background = FALSE;
        }
    }
  else
    {
      self->has_background = FALSE;
      if (err)
        g_error_free (err);
    }
  fpi_ssm_next_state (t->ssm);
}

static void
scan_image_cb (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err)
{
  FpiDeviceFt9201 *self = FPI_DEVICE_FT9201 (dev);
  gsize img_bytes = (gsize) self->sensor_width * self->sensor_height;

  if (err)
    {
      fpi_ssm_mark_failed (t->ssm, err);
      return;
    }

  if (t->actual_length >= (gssize) (img_bytes + 2))
    {
      self->image_buf[img_bytes] = t->buffer[0];
      self->image_buf[img_bytes + 1] = t->buffer[1];
      memcpy (self->image_buf, t->buffer + 2, img_bytes);
      self->scan_ok = !(t->buffer[0] == 0 && t->buffer[1] == 0);
    }
  else
    {
      self->scan_ok = FALSE;
    }

  fpi_ssm_next_state (t->ssm);
}

static void
scan_ssm_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFt9201 *self = FPI_DEVICE_FT9201 (dev);
  FpiUsbTransfer *t;
  gsize total = (gsize) self->sensor_width * self->sensor_height + 2;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case SCAN_BG_WAIT_CLEAR_DELAY:
      if (self->has_background)
        {
          fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NEEDED);
          fpi_ssm_jump_to_state (ssm, SCAN_WAIT_FINGER);
          break;
        }
      fpi_ssm_next_state_delayed (ssm, 20);
      break;
    case SCAN_BG_WAIT_CLEAR:
      ctrl_in (ssm, dev, REQ_AFE_READ, 0, REG_FINGER, 4, bg_wait_clear_cb);
      break;
    case SCAN_BG_RESET_EP:
      ctrl_out (ssm, dev, REQ_BULK_ARM, 0xff, 0);
      break;
    case SCAN_BG_CAP_DELAY:
    case SCAN_CAP_DELAY:
    case SCAN_WAIT_DELAY:
    case SCAN_WAIT_LIFT_DELAY:
      fpi_ssm_next_state_delayed (ssm, 20);
      break;
    case SCAN_BG_ARM:
    case SCAN_START_03:
      ctrl_out (ssm, dev, REQ_BULK_ARM, 0x03, 0);
      break;
    case SCAN_BG_CFG_BULK:
    case SCAN_CONFIG_BULK:
      ctrl_out (ssm, dev, REQ_BULK_SIZE, (guint16) total, 0x3400);
      break;
    case SCAN_BG_READ:
      t = fpi_usb_transfer_new (dev);
      t->ssm = ssm;
      fpi_usb_transfer_fill_bulk (t, FT9201_EP_IN, total);
      fpi_usb_transfer_submit (t, BULK_TIMEOUT, fpi_device_get_cancellable (dev), scan_bg_image_cb, NULL);
      break;
    case SCAN_WAIT_FINGER:
      fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NEEDED);
      ctrl_in (ssm, dev, REQ_AFE_READ, 0, REG_FINGER, 4, wait_finger_cb);
      break;
    case SCAN_SETTLE_DELAY:
      fpi_ssm_next_state_delayed (ssm, 100);
      break;
    case SCAN_START_FF:
      ctrl_out (ssm, dev, REQ_BULK_ARM, 0xff, 0);
      break;
    case SCAN_READ_IMAGE:
      t = fpi_usb_transfer_new (dev);
      t->ssm = ssm;
      fpi_usb_transfer_fill_bulk (t, FT9201_EP_IN, total);
      fpi_usb_transfer_submit (t, BULK_TIMEOUT, fpi_device_get_cancellable (dev), scan_image_cb, NULL);
      break;
    case SCAN_WAIT_LIFT:
      /* Only wait for finger lift during multi-stage enroll; verify/identify returns immediately */
      if (fpi_device_get_current_action (dev) != FPI_DEVICE_ACTION_ENROLL)
        {
          fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NONE);
          fpi_ssm_mark_completed (ssm);
          break;
        }
      ctrl_in (ssm, dev, REQ_AFE_READ, 0, REG_FINGER, 4, wait_lift_cb);
      break;
    default:
      g_assert_not_reached ();
    }
}

/* ================================================================== */
/* Enroll / Verify / Identify completion via v4.1 pressmatch           */
/* ================================================================== */

static void
ft9201_enroll_clear (FpiDeviceFt9201 *self)
{
  g_clear_pointer (&self->enroll_images, g_ptr_array_unref);
  g_clear_pointer (&self->enroll_feats, g_ptr_array_unref);
  g_clear_pointer (&self->enroll_pg, elanpress_pose_graph_free);
}

static void
scan_ssm_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceFt9201 *self = FPI_DEVICE_FT9201 (dev);
  FpiDeviceAction action = fpi_device_get_current_action (dev);
  gsize img_bytes = (gsize) self->sensor_width * self->sensor_height;
  const ElanpressSiftParams p_v4 = {
    .contrast_thr = 0.0f,
    .edge_ratio = FT9201_EDGE_RATIO,
    .rootsift = TRUE,
  };

  self->loop_ssm = NULL;
  fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NONE);

  if (error)
    {
      if (action == FPI_DEVICE_ACTION_ENROLL)
        {
          ft9201_enroll_clear (self);
          fpi_device_enroll_complete (dev, NULL, error);
        }
      else if (action == FPI_DEVICE_ACTION_IDENTIFY)
        fpi_device_identify_complete (dev, error);
      else
        fpi_device_verify_complete (dev, error);
      return;
    }

  g_autofree guint8 *norm = NULL;
  gdouble quality = 0.0;
  if (self->scan_ok)
    {
      norm = ft9201_normalize_frame (self->image_buf, self->background_buf,
                                     self->has_background, img_bytes);
      quality = elanpress_image_quality (norm, (unsigned int) img_bytes);
    }

  if (!norm || quality < ELANPRESS_MIN_QUALITY_STDDEV)
    {
      if (action == FPI_DEVICE_ACTION_ENROLL)
        {
          self->enroll_attempts++;
          self->enroll_consec_fail++;
          if (self->enroll_consec_fail >= 8 || self->enroll_attempts >= FT9201_ENROLL_MAX_ATT)
            {
              elanpress_enroll_telemetry_emit ("ft9201", self->sensor_width, self->sensor_height,
                                               "error", self->enroll_stage, FT9201_ENROLL_STAGES,
                                               self->enroll_attempts, quality, 0.0,
                                               self->enroll_pg, &self->enroll_map);
              ft9201_enroll_clear (self);
              fpi_device_enroll_complete (
                dev, NULL,
                fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                          "ft9201: low-contrast touches, please clean finger and sensor"));
              return;
            }
          elanpress_enroll_telemetry_emit ("ft9201", self->sensor_width, self->sensor_height,
                                           "low_quality", self->enroll_stage, FT9201_ENROLL_STAGES,
                                           self->enroll_attempts, quality, 0.0,
                                           self->enroll_pg, &self->enroll_map);
          fpi_device_enroll_progress (dev, self->enroll_stage, NULL,
                                      fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL));
          start_scan (dev);
          return;
        }
      start_scan (dev);
      return;
    }

  /* ---- ENROLL ---- */
  if (action == FPI_DEVICE_ACTION_ENROLL)
    {
      g_autoptr(ElanpressFeatures) feat =
        elanpress_sift_features_opts (norm, self->sensor_width, self->sensor_height,
                                      FT9201_ENROLL_MAX_KP, &p_v4, NULL);
      self->enroll_attempts++;

      if (self->enroll_images->len == 0)
        {
          g_ptr_array_add (self->enroll_images, g_steal_pointer (&norm));
          g_ptr_array_add (self->enroll_feats, g_steal_pointer (&feat));
          self->enroll_pg = elanpress_pose_graph_new_single (
            g_ptr_array_index (self->enroll_feats, 0),
            self->sensor_width, self->sensor_height);
          elanpress_enroll_map_compute (self->enroll_images, self->enroll_pg, 0,
                                        self->sensor_width, self->sensor_height,
                                        FT9201_ENROLL_MIN_STRIP, &self->enroll_map);
          self->enroll_consec_fail = 0;
          self->enroll_stage = 1;
          elanpress_enroll_telemetry_emit ("ft9201", self->sensor_width, self->sensor_height,
                                           "accepted", self->enroll_stage, FT9201_ENROLL_STAGES,
                                           self->enroll_attempts, quality, 0.0,
                                           self->enroll_pg, &self->enroll_map);
          fpi_device_enroll_progress (dev, self->enroll_stage, NULL, NULL);
          start_scan (dev);
          return;
        }

      guint comp = 0, ov = 0;
      ElanpressPose pose = { 1.0f, 0.0f, 0.0f, 0.0f };
      guint pairs = elanpress_pose_graph_match_pose (feat, self->enroll_pg, TRUE, &comp, &pose);
      gdouble zncc = 0.0;
      if (pairs > 0)
        zncc = elanpress_pose_graph_zncc (norm, self->enroll_images, 1, 0,
                                          self->enroll_pg, comp, pose,
                                          self->sensor_width, self->sensor_height, &ov);

      if (pairs >= FT9201_STITCH_MIN_PAIRS && zncc >= 0.45 && ov >= 400 &&
          self->enroll_images->len >= 3)
        {
          gdouble gain = elanpress_enroll_map_preview_gain (&self->enroll_map, norm, pose,
                                                            self->sensor_width, self->sensor_height);
          if (gain < FT9201_MIN_GAIN && self->enroll_attempts < FT9201_ENROLL_MAX_ATT)
            {
              elanpress_enroll_telemetry_emit ("ft9201", self->sensor_width, self->sensor_height,
                                               "redundant", self->enroll_stage, FT9201_ENROLL_STAGES,
                                               self->enroll_attempts, quality, 0.0,
                                               self->enroll_pg, &self->enroll_map);
              fpi_device_enroll_progress (dev, self->enroll_stage, NULL,
                                          fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER));
              start_scan (dev);
              return;
            }
        }

      guint new_idx = self->enroll_images->len;
      g_ptr_array_add (self->enroll_images, g_steal_pointer (&norm));
      g_ptr_array_add (self->enroll_feats, g_steal_pointer (&feat));
      g_clear_pointer (&self->enroll_pg, elanpress_pose_graph_free);
      self->enroll_pg = elanpress_pose_graph_build_verified (
        (ElanpressFeatures * const *) self->enroll_feats->pdata,
        self->enroll_images, self->enroll_feats->len,
        self->sensor_width, self->sensor_height, FT9201_STITCH_MIN_PAIRS);
      elanpress_enroll_map_compute (self->enroll_images, self->enroll_pg, new_idx,
                                    self->sensor_width, self->sensor_height,
                                    FT9201_ENROLL_MIN_STRIP, &self->enroll_map);
      self->enroll_consec_fail = 0;

      gboolean done = self->enroll_map.complete ||
                      self->enroll_images->len >= FT9201_ENROLL_MAX_STRIP ||
                      (self->enroll_attempts >= FT9201_ENROLL_MAX_ATT &&
                       self->enroll_images->len >= FT9201_ENROLL_MIN_STRIP);

      int mapped = done
                     ? FT9201_ENROLL_STAGES
                     : CLAMP ((int) ((self->enroll_map.progress_pct * (FT9201_ENROLL_STAGES - 1)) / 100),
                              1, FT9201_ENROLL_STAGES - 1);
      if (mapped > self->enroll_stage)
        self->enroll_stage = mapped;

      elanpress_enroll_telemetry_emit ("ft9201", self->sensor_width, self->sensor_height,
                                       done ? "complete" : "accepted",
                                       self->enroll_stage, FT9201_ENROLL_STAGES,
                                       self->enroll_attempts, quality, 0.0,
                                       self->enroll_pg, &self->enroll_map);
      fpi_device_enroll_progress (dev, self->enroll_stage, NULL, NULL);

      if (!done)
        {
          start_scan (dev);
          return;
        }

      FpPrint *print = NULL;
      fpi_device_get_enroll_data (dev, &print);
      ft9201_print_set_data (self, print, self->enroll_images, self->enroll_pg);
      fp_dbg ("ft9201 enroll complete: stitched=%u/%u span=%.0fx%.0f",
              self->enroll_pg->largest_nodes, self->enroll_pg->n_strips,
              self->enroll_pg->span_w, self->enroll_pg->span_h);
      ft9201_enroll_clear (self);
      fpi_device_enroll_complete (dev, g_object_ref (print), NULL);
      return;
    }

  /* ---- VERIFY & IDENTIFY ---- */
  g_autoptr(ElanpressFeatures) probe_feat =
    elanpress_sift_features_opts (norm, self->sensor_width, self->sensor_height,
                                  FT9201_PROBE_MAX_KP, &p_v4, NULL);

  if (action == FPI_DEVICE_ACTION_IDENTIFY)
    {
      GPtrArray *gallery = NULL;
      FpPrint *winner = NULL;
      gint best_score = 0;

      fpi_device_get_identify_data (dev, &gallery);
      for (guint i = 0; gallery && i < gallery->len; i++)
        {
          FpPrint *print = g_ptr_array_index (gallery, i);
          gboolean matched = FALSE;
          gdouble zncc = 0.0;
          guint ov = 0;
          gint pairs = ft9201_match_print (self, norm, probe_feat, print,
                                           &matched, &zncc, &ov);
          if (pairs >= 0 && matched && pairs > best_score)
            {
              best_score = pairs;
              winner = print;
            }
        }
      fp_dbg ("ft9201 identify: winner=%s best_pairs=%d", winner ? "MATCH" : "none", best_score);
      fpi_device_identify_report (dev, winner, NULL, NULL);
      fpi_device_identify_complete (dev, NULL);
    }
  else
    {
      FpPrint *print = NULL;
      gboolean matched = FALSE;
      gdouble zncc = 0.0;
      guint ov = 0;
      gint pairs;

      fpi_device_get_verify_data (dev, &print);
      pairs = ft9201_match_print (self, norm, probe_feat, print,
                                  &matched, &zncc, &ov);
      if (pairs < 0)
        {
          fpi_device_verify_complete (dev, fpi_device_error_new (FP_DEVICE_ERROR_DATA_INVALID));
          return;
        }
      fp_dbg ("ft9201 verify: match=%d pairs=%d zncc=%.3f ov=%u", matched ? 1 : 0, pairs, zncc, ov);
      fpi_device_verify_report (dev, matched ? FPI_MATCH_SUCCESS : FPI_MATCH_FAIL, NULL, NULL);
      fpi_device_verify_complete (dev, NULL);
    }
}

/* ================================================================== */
/* FpDevice vfuncs                                                     */
/* ================================================================== */

static void
start_scan (FpDevice *dev)
{
  FpiDeviceFt9201 *self = FPI_DEVICE_FT9201 (dev);
  self->loop_ssm = fpi_ssm_new (dev, scan_ssm_run_state, SCAN_NUM_STATES);
  fpi_ssm_start (self->loop_ssm, scan_ssm_complete);
}

static void
dev_open (FpDevice *dev)
{
  FpiDeviceFt9201 *self = FPI_DEVICE_FT9201 (dev);
  GError *error = NULL;
  FpiSsm *ssm;

  if (!g_usb_device_claim_interface (fpi_device_get_usb_device (dev), 0, 0, &error))
    {
      fpi_device_open_complete (dev, error);
      return;
    }

  self->fw_offset = 0;
  self->fw_retries = 0;
  ssm = fpi_ssm_new (dev, init_ssm_run_state, INIT_NUM_STATES);
  fpi_ssm_start (ssm, init_ssm_complete);
}

static void
dev_close (FpDevice *dev)
{
  FpiDeviceFt9201 *self = FPI_DEVICE_FT9201 (dev);
  GError *error = NULL;

  ft9201_enroll_clear (self);
  g_clear_pointer (&self->image_buf, g_free);
  g_clear_pointer (&self->background_buf, g_free);
  g_usb_device_release_interface (fpi_device_get_usb_device (dev), 0, 0, &error);
  fpi_device_close_complete (dev, error);
}

static void
dev_enroll (FpDevice *dev)
{
  FpiDeviceFt9201 *self = FPI_DEVICE_FT9201 (dev);

  ft9201_enroll_clear (self);
  self->enroll_stage = 0;
  self->enroll_attempts = 0;
  self->enroll_consec_fail = 0;
  memset (&self->enroll_map, 0, sizeof (self->enroll_map));
  self->enroll_images = g_ptr_array_new_with_free_func (g_free);
  self->enroll_feats = g_ptr_array_new_with_free_func ((GDestroyNotify) elanpress_features_free);
  self->has_background = FALSE;

  elanpress_enroll_telemetry_emit ("ft9201", self->sensor_width, self->sensor_height,
                                   "started", 0, FT9201_ENROLL_STAGES,
                                   0, 0.0, 0.0, NULL, &self->enroll_map);
  start_scan (dev);
}

static void
dev_identify_verify (FpDevice *dev)
{
  FpiDeviceFt9201 *self = FPI_DEVICE_FT9201 (dev);
  self->has_background = FALSE;
  start_scan (dev);
}

static void
dev_cancel (FpDevice *dev)
{
}

static const FpIdEntry id_table[] = {
  { .vid = 0x2808, .pid = 0x93a9 },
  { .vid = 0,      .pid = 0      },
};

static void
fpi_device_ft9201_init (FpiDeviceFt9201 *self)
{
}

static void
fpi_device_ft9201_class_init (FpiDeviceFt9201Class *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  dev_class->id = "ft9201";
  dev_class->full_name = "Focal-systems FT9201 Fingerprint Reader";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = id_table;
  dev_class->nr_enroll_stages = FT9201_ENROLL_STAGES;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->temp_hot_seconds = -1;

  dev_class->open = dev_open;
  dev_class->close = dev_close;
  dev_class->enroll = dev_enroll;
  dev_class->verify = dev_identify_verify;
  dev_class->identify = dev_identify_verify;
  dev_class->cancel = dev_cancel;

  fpi_device_class_auto_initialize_features (dev_class);
}
