/*
 * PressMatch Local Biometric Study — Lossless Raw Frame Capture Tool
 *
 * Captures exact raw capacitive frames directly from BOTH:
 *   1. ELAN 04f3:0c6e (150x52, 14-bit raw + background + covered-pixel 8-bit norm)
 *   2. FocalTech FT9201 2808:93a9 (96x96, 8-bit raw + FPN bg + 96x96 & 64x80 8-bit norm)
 *
 * Strictly local storage (zero network/cloud). Every frame is saved alongside
 * capacitance diagnostics (bg, mean_delta, coverage, norm_stddev) so the exact
 * same physical touches can be replayed across ALL algorithms (NBIS Bozorth3,
 * Upstream v0, PressMatch v1/v2/v4.0/v4.1) and 64x80 vs 96x96 crops.
 *
 * Usage:
 *   ./raw_capture <elan|ft9201> <out_dir> <prefix> <count> [prompt_label]
 */
#define _GNU_SOURCE
#include <glib.h>
#include <libusb-1.0/libusb.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "elanpress-match.h"
#include "ft9201_fw.h"

/* ---- ELAN 04f3:0c6e constants ---- */
#define ELAN_VID        0x04f3
#define ELAN_PID        0x0c6e
#define ELAN_EP_CMD_OUT 0x01
#define ELAN_EP_CMD_IN  0x83
#define ELAN_EP_IMG_IN  0x82

/* ---- FocalTech FT9201 2808:93a9 constants ---- */
#define FT_VID          0x2808
#define FT_PID          0x93a9
#define FT_EP_OUT       0x02
#define FT_EP_IN        0x83
#define USB_TIMEOUT_MS  5000

static int
cmp_u8 (const void *a, const void *b)
{
  return (int) (*(const uint8_t *) a) - (int) (*(const uint8_t *) b);
}

static void
normalize_percentile_u8 (const uint8_t *in, uint8_t *out, int n)
{
  uint8_t *sorted = malloc ((size_t) n);
  memcpy (sorted, in, (size_t) n);
  qsort (sorted, (size_t) n, 1, cmp_u8);
  int l0 = sorted[n / 100];
  int l1 = sorted[(n * 3) / 10];
  int l2 = sorted[(n * 65) / 100];
  int l3 = sorted[(n * 99) / 100];
  int d01 = l1 > l0 ? l1 - l0 : 1;
  int d12 = l2 > l1 ? l2 - l1 : 1;
  int d23 = l3 > l2 ? l3 - l2 : 1;
  for (int i = 0; i < n; i++)
    {
      int px = in[i], v;
      if (px <= l0) v = 1;
      else if (px < l1) v = 1 + (px - l0) * 98 / d01;
      else if (px < l2) v = 99 + (px - l1) * 56 / d12;
      else if (px >= l3) v = 255;
      else v = 155 + ((px - l2) * 100 / d23 > 100 ? 100 : (px - l2) * 100 / d23);
      out[i] = (uint8_t) v;
    }
  free (sorted);
}

static void
write_bin (const char *path, const void *buf, size_t len)
{
  FILE *f = fopen (path, "wb");
  if (!f) { perror (path); return; }
  fwrite (buf, 1, len, f);
  fclose (f);
}

/* ========================================================================= */
/* ELAN 04f3:0c6e (150x52, 14-bit)                                           */
/* ========================================================================= */

static int
elan_drain_and_stop (libusb_device_handle *h)
{
  static const uint8_t cmd_stop[2] = { 0x00, 0x0b };
  int xfer = 0;
  uint8_t trash[4096];

  libusb_clear_halt (h, ELAN_EP_CMD_OUT);
  libusb_clear_halt (h, ELAN_EP_CMD_IN);
  libusb_clear_halt (h, ELAN_EP_IMG_IN);

  libusb_bulk_transfer (h, ELAN_EP_CMD_OUT, (uint8_t *) cmd_stop, 2, &xfer, 300);
  /* Drain any stale bytes left over from fprintd or an interrupted capture */
  for (int k = 0; k < 4; k++)
    {
      if (libusb_bulk_transfer (h, ELAN_EP_CMD_IN, trash, 64, &xfer, 35) != 0 || xfer == 0)
        break;
    }
  for (int k = 0; k < 6; k++)
    {
      if (libusb_bulk_transfer (h, ELAN_EP_IMG_IN, trash, sizeof (trash), &xfer, 45) != 0 || xfer == 0)
        break;
    }
  usleep (60000);
  return 0;
}

static int
elan_read_frame (libusb_device_handle *h, int w, int hgt, uint16_t *rot_out)
{
  static const uint8_t cmd_status[2] = { 0x40, 0x13 };
  static const uint8_t cmd_img[2]    = { 0x00, 0x09 };
  int xfer = 0;
  uint8_t st = 0;
  int fbytes = w * hgt * 2;
  uint8_t *raw = malloc ((size_t) fbytes);

  int rc = libusb_bulk_transfer (h, ELAN_EP_CMD_OUT, (uint8_t *) cmd_status, 2, &xfer, USB_TIMEOUT_MS);
  if (rc == 0)
    rc = libusb_bulk_transfer (h, ELAN_EP_CMD_IN, &st, 1, &xfer, USB_TIMEOUT_MS);
  if (rc == 0)
    rc = libusb_bulk_transfer (h, ELAN_EP_CMD_OUT, (uint8_t *) cmd_img, 2, &xfer, USB_TIMEOUT_MS);
  if (rc == 0)
    rc = libusb_bulk_transfer (h, ELAN_EP_IMG_IN, raw, fbytes, &xfer, 2500);
  if (rc == 0 && xfer == fbytes)
    elanpress_rotate_frame (raw, rot_out, (unsigned short) w, (unsigned short) hgt);
  else
    rc = (rc != 0) ? rc : -99;
  free (raw);
  return rc;
}

static int
capture_elan (const char *out_dir, const char *prefix, int count, const char *label, int start_idx)
{
  libusb_context *ctx = NULL;
  libusb_device_handle *h = NULL;
  if (libusb_init (&ctx) < 0) return 1;
  h = libusb_open_device_with_vid_pid (ctx, ELAN_VID, ELAN_PID);
  if (!h) { fprintf (stderr, "ERROR: Cannot open ELAN 04f3:0c6e (stop fprintd or run with sudo)\n"); libusb_exit (ctx); return 1; }

  int claimed = 0;
  for (int tries = 0; tries < 8; tries++)
    {
      if (libusb_claim_interface (h, 0) == 0) { claimed = 1; break; }
      usleep (150000);
    }
  if (!claimed) { fprintf (stderr, "ERROR: Cannot claim ELAN interface 0 (stop fprintd first)\n"); libusb_close (h); libusb_exit (ctx); return 1; }

  /* Always reset/drain stale USB state left over from fprintd or a stopped run */
  elan_drain_and_stop (h);

  /* Read sensor dimensions (same odd-parity quirk as elanpress.c) */
  uint8_t cmd_dim[2] = { 0x00, 0x0c };
  uint8_t dim_buf[4] = { 0 };
  int xfer = 0;
  int w = 150, hgt = 52;
  if (libusb_bulk_transfer (h, ELAN_EP_CMD_OUT, cmd_dim, 2, &xfer, USB_TIMEOUT_MS) == 0 &&
      libusb_bulk_transfer (h, ELAN_EP_CMD_IN, dim_buf, 4, &xfer, USB_TIMEOUT_MS) == 0 && xfer >= 4)
    {
      int rh = dim_buf[0];
      int rw = dim_buf[2];
      if ((rw % 2 == 1) && (rh % 2 == 1)) { rw++; rh++; }
      if (rw >= 50 && rw <= 256 && rh >= 20 && rh <= 256) { w = rw; hgt = rh; }
    }
  unsigned int sz = (unsigned int) (w * hgt);
  uint16_t *bg = calloc (sz, sizeof (uint16_t));
  uint16_t *frame = calloc (sz, sizeof (uint16_t));

  /* Capture clean empty background (wait if a finger is still resting on sensor!)
   * On ELAN 04f3:0c6e, empty sensor mean is ~3030 counts; a resting finger pushes
   * mean > 4200 and would poison touch detection if adopted as background. */
  char ref_bg_path[512];
  snprintf (ref_bg_path, sizeof (ref_bg_path), "%s/elan_bg_ref.raw16", out_dir);
  int have_ref_bg = 0;
  {
    FILE *rf = fopen (ref_bg_path, "rb");
    if (rf)
      {
        if (fread (bg, sizeof (uint16_t), sz, rf) == sz)
          {
            int64_t s = 0;
            for (unsigned int k = 0; k < sz; k++) s += bg[k];
            double m = (double) s / sz;
            if (m > 1000.0 && m < 3900.0)
              have_ref_bg = 1;
          }
        fclose (rf);
      }
  }

  if (!have_ref_bg)
    {
      int warned = 0;
      int read_fails = 0;
      while (1)
        {
          int rrc = elan_read_frame (h, w, hgt, bg);
          if (rrc != 0)
            {
              read_fails++;
              if (read_fails >= 10)
                {
                  fprintf (stderr, "ERROR: Failed to read ELAN background frame after %d tries (rc=%d)\n", read_fails, rrc);
                  goto fail;
                }
              if (read_fails == 4)
                {
                  libusb_reset_device (h);
                  usleep (180000);
                }
              elan_drain_and_stop (h);
              usleep (100000);
              continue;
            }
          read_fails = 0;
          int64_t s = 0;
          for (unsigned int k = 0; k < sz; k++) s += bg[k];
          double m = (double) s / sz;
          if (m < 3900.0)
            break;
          if (!warned)
            {
              printf ("  (lift finger off power button to calibrate empty sensor... bg=%.0f)\n", m);
              fflush (stdout);
              warned = 1;
            }
          usleep (40000);
        }
      write_bin (ref_bg_path, bg, sz * sizeof (uint16_t));
    }
  char pbuf[512];
  snprintf (pbuf, sizeof (pbuf), "%s/%s_bg.raw16", out_dir, prefix);
  write_bin (pbuf, bg, sz * sizeof (uint16_t));

  snprintf (pbuf, sizeof (pbuf), "%s/captures.jsonl", out_dir);
  FILE *logf = fopen (pbuf, "a");

  for (int i = start_idx; i < count; i++)
    {
      printf ("  [ELAN %d/%d] %s — touch power button...\n", i + 1, count, label);
      fflush (stdout);

      /* Wait for finger to land */
      ElanpressTouchStats st = { 0, 0 };
      while (1)
        {
          if (elan_read_frame (h, w, hgt, frame) != 0) { usleep (20000); continue; }
          if (elanpress_frame_has_touch (frame, bg, sz, &st))
            break;
          usleep (10000);
        }
      /* Exact 200 ms PressMatch v4.1 settle */
      usleep (200000);
      if (elan_read_frame (h, w, hgt, frame) != 0)
        {
          fprintf (stderr, "  ! read failed, retrying touch\n");
          i--;
          continue;
        }

      guint8 *norm = elanpress_process_frame_covered (frame, bg, sz);
      ElanpressFrameDiag fd = { 0 };
      elanpress_frame_diag (frame, bg, norm, sz, &fd);

      snprintf (pbuf, sizeof (pbuf), "%s/%s_%02d.raw16", out_dir, prefix, i);
      write_bin (pbuf, frame, sz * sizeof (uint16_t));

      if (norm)
        {
          snprintf (pbuf, sizeof (pbuf), "%s/%s_%02d.u8", out_dir, prefix, i);
          write_bin (pbuf, norm, sz);
          g_free (norm);
        }
      else
        {
          /* Still save zeroed fallback u8 so replay counts even ultra-low-quality captures honestly! */
          guint8 *z = calloc (sz, 1);
          snprintf (pbuf, sizeof (pbuf), "%s/%s_%02d.u8", out_dir, prefix, i);
          write_bin (pbuf, z, sz);
          free (z);
        }

      if (logf)
        {
          fprintf (logf,
                   "{\"sensor\":\"elan\",\"w\":%d,\"h\":%d,\"prefix\":\"%s\",\"idx\":%d,"
                   "\"label\":\"%s\",\"bg\":%d,\"md\":%.1f,\"cov\":%.1f,\"sd\":%.2f,\"ts\":%ld}\n",
                   w, hgt, prefix, i, label, fd.bg_mean, fd.mean_delta, fd.coverage, fd.norm_stddev,
                   (long) time (NULL));
          fflush (logf);
        }
      printf ("    OK  cov=%.0f%%  md=%.0f  sd=%.1f  (lift finger)\n", fd.coverage, fd.mean_delta, fd.norm_stddev);
      fflush (stdout);

      /* Wait for finger lift (5 consecutive clear frames) */
      int clear_cnt = 0;
      while (clear_cnt < 5)
        {
          if (elan_read_frame (h, w, hgt, frame) != 0) { usleep (15000); continue; }
          if (elanpress_frame_has_touch (frame, bg, sz, &st)) clear_cnt = 0;
          else clear_cnt++;
          usleep (10000);
        }
    }

  if (logf) fclose (logf);
  {
    uint8_t cmd_stop[2] = { 0x00, 0x0b };
    libusb_bulk_transfer (h, ELAN_EP_CMD_OUT, cmd_stop, 2, &xfer, USB_TIMEOUT_MS);
  }
  free (bg);
  free (frame);
  libusb_release_interface (h, 0);
  libusb_close (h);
  libusb_exit (ctx);
  return 0;

fail:
  free (bg);
  free (frame);
  libusb_release_interface (h, 0);
  libusb_close (h);
  libusb_exit (ctx);
  return 1;
}

/* ========================================================================= */
/* FocalTech FT9201 2808:93a9 (96x96 native + 64x80 center crop)             */
/* ========================================================================= */

static int
ft_ctrl_out (libusb_device_handle *h, uint8_t req, uint16_t val, uint16_t idx)
{
  return libusb_control_transfer (h, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
                                  req, val, idx, NULL, 0, USB_TIMEOUT_MS);
}

static int
ft_ctrl_in (libusb_device_handle *h, uint8_t req, uint16_t val, uint16_t idx, uint8_t *buf, uint16_t len)
{
  return libusb_control_transfer (h, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
                                  req, val, idx, buf, len, USB_TIMEOUT_MS);
}

static int
ft_capture_frame (libusb_device_handle *h, int w, int hgt, uint8_t *raw_out, uint8_t q[2])
{
  int total = w * hgt + 2;
  uint8_t *buf = calloc ((size_t) total, 1);
  int actual = 0;
  ft_ctrl_out (h, 0x34, 0xff, 0);
  usleep (20000);
  ft_ctrl_out (h, 0x34, 0x03, 0);
  ft_ctrl_out (h, 0x35, (uint16_t) total, 0x3400);
  int rc = libusb_bulk_transfer (h, FT_EP_IN, buf, total, &actual, USB_TIMEOUT_MS);
  if (rc == 0 && actual >= total)
    {
      q[0] = buf[0];
      q[1] = buf[1];
      memcpy (raw_out, buf + 2, (size_t) (w * hgt));
    }
  free (buf);
  return (rc == 0 && actual >= total) ? 0 : -1;
}

static int
capture_ft9201 (const char *out_dir, const char *prefix, int count, const char *label, int start_idx)
{
  libusb_context *ctx = NULL;
  libusb_device_handle *h = NULL;
  uint8_t rb[4] = { 0 };
  int xfer = 0;

  if (libusb_init (&ctx) < 0) return 1;
  h = libusb_open_device_with_vid_pid (ctx, FT_VID, FT_PID);
  if (!h) { fprintf (stderr, "ERROR: Cannot open FT9201 2808:93a9 (is USB dongle plugged in?)\n"); libusb_exit (ctx); return 1; }
  int claimed = 0;
  for (int tries = 0; tries < 8; tries++)
    {
      if (libusb_claim_interface (h, 0) == 0) { claimed = 1; break; }
      usleep (150000);
    }
  if (!claimed) { fprintf (stderr, "ERROR: Cannot claim FT9201 interface 0 (stop fprintd first)\n"); libusb_close (h); libusb_exit (ctx); return 1; }

  /* Full 1:1 MCU bring-up identical to proven release-v4.1 ft9201.c driver */
  ft_ctrl_out (h, 0x22, 0x70, 0x70);
  ft_ctrl_out (h, 0x22, 0x70, 0x70);
  ft_ctrl_out (h, 0x64, 0x9001, 0x0000);
  ft_ctrl_in  (h, 0x60, 0, 0, rb, 1);
  ft_ctrl_out (h, 0x64, 0x0603, 0x00f9);
  ft_ctrl_out (h, 0x34, 0x0002, 0x0000);
  ft_ctrl_out (h, 0x35, 0x0004, 0x85c0);
  uint8_t pld[4] = { 0x11, 0xee, 0x02, 0x00 };
  libusb_bulk_transfer (h, FT_EP_OUT, pld, 4, &xfer, USB_TIMEOUT_MS);
  ft_ctrl_out (h, 0x03, 0x0001, 0x00a4);
  ft_ctrl_in  (h, 0x30, 0, 0x85c0, rb, 2);

  /* SPI sensor variant probe */
  uint8_t reg_f4 = 0, sensor_variant = 0;
  ft_ctrl_out (h, 0x57, 0, 0);
  ft_ctrl_out (h, 0x66, 0x00df, 0x00c8);
  ft_ctrl_in  (h, 0x65, 0, 0x00c8, rb, 1);
  ft_ctrl_out (h, 0x66, 0x001d, 0x00f1);
  if (ft_ctrl_in (h, 0x65, 0, 0x00f4, rb, 1) >= 1)
    reg_f4 = rb[0];
  ft_ctrl_out (h, 0x66, (uint16_t) (reg_f4 | 0x01), 0x00f4);
  if (ft_ctrl_in (h, 0x65, 0, 0x00f3, rb, 1) >= 1)
    sensor_variant = (uint8_t) ((rb[0] >> 1) & 0x0f);
  ft_ctrl_out (h, 0x66, 0x0000, 0x00f4);

  /* Upload 8051 MCU firmware in exact 64-byte zero-padded chunks (matches ft9201.c) */
  ft_ctrl_out (h, 0x22, 0x70, 0x70);
  ft_ctrl_out (h, 0x22, 0x70, 0x70);
  ft_ctrl_out (h, 0x57, 0, 0);
  ft_ctrl_out (h, 0x34, 0x00ff, 0x0000);
  ft_ctrl_out (h, 0x34, 0x0002, 0x0000);
  ft_ctrl_in  (h, 0x1a, 0, 0, rb, 2);
  ft_ctrl_out (h, 0x35, 64, 0x0000);
  size_t fw_len = (size_t) ft9201_fw_size;
  for (size_t off = 0; off < fw_len; off += 64)
    {
      uint8_t chunk_buf[64];
      size_t n = (fw_len - off > 64) ? 64 : (fw_len - off);
      memset (chunk_buf, 0, sizeof (chunk_buf));
      memcpy (chunk_buf, ft9201_fw_data + off, n);
      libusb_bulk_transfer (h, FT_EP_OUT, chunk_buf, 64, &xfer, USB_TIMEOUT_MS);
    }

  /* Post-firmware AFE register config & sensor reset */
  ft_ctrl_out (h, 0x3b, 0x01, 0x01);
  ft_ctrl_out (h, 0x3b, 0x0f, 0x41);
  ft_ctrl_out (h, 0x3b, 0xbb, 0x30);
  ft_ctrl_in  (h, 0x3a, 0, 0x30, rb, 1);
  ft_ctrl_out (h, 0x3b, 0x00, 0x22);
  ft_ctrl_out (h, 0x3b, 0x0e, 0x23);
  ft_ctrl_out (h, 0x3b, 0x01, 0x54);
  ft_ctrl_out (h, 0x3b, 0x01, 0x1f);
  ft_ctrl_out (h, 0x3b, 0x01, 0x1e);
  ft_ctrl_out (h, 0x40, 0, 0);
  usleep (160000);
  ft_ctrl_out (h, 0x22, 0x70, 0x70);
  ft_ctrl_out (h, 0x22, 0x70, 0x70);

  int mcu_ready = 0;
  for (int p = 0; p < 200; p++)
    {
      rb[0] = rb[1] = 0;
      if (ft_ctrl_in (h, 0x3a, 0, 0x20, rb, 2) >= 2 && rb[0] == 0xa5 && rb[1] == 0x5a)
        {
          mcu_ready = 1;
          break;
        }
      usleep (3000);
    }
  if (!mcu_ready)
    {
      fprintf (stderr, "ERROR: FT9201 MCU did not boot (status=0x%02x 0x%02x)\n", rb[0], rb[1]);
      libusb_release_interface (h, 0);
      libusb_close (h);
      libusb_exit (ctx);
      return 1;
    }

  int w = 96, hgt = 96;
  if (ft_ctrl_in (h, 0x3a, 0, 0x14, rb, 4) >= 2 && rb[0] >= 32) w = rb[0];
  if (ft_ctrl_in (h, 0x3a, 0, 0x15, rb, 4) >= 2 && rb[0] >= 32) hgt = rb[0];
  if (sensor_variant == 2 || sensor_variant == 3)
    {
      ft_ctrl_out (h, 0x3b, 0x00, 0x22);
      ft_ctrl_out (h, 0x3b, 0x0e, 0x23);
      ft_ctrl_out (h, 0x3b, 0x01, 0x1f);
      ft_ctrl_out (h, 0x3b, 0x01, 0x1e);
    }
  int sz = w * hgt;

  uint8_t *bg = calloc ((size_t) sz, 1);
  uint8_t *raw = calloc ((size_t) sz, 1);
  uint8_t *fpn = calloc ((size_t) sz, 1);
  uint8_t *norm = calloc ((size_t) sz, 1);
  uint8_t q[2] = { 0, 0 };

  /* Wait clear + capture FPN background */
  for (int k = 0; k < 50; k++)
    {
      ft_ctrl_in (h, 0x3a, 0, 0x1d, rb, 4);
      if (rb[0] != 0xa0) break;
      usleep (20000);
    }
  int has_bg = 0;
  if (ft_capture_frame (h, w, hgt, bg, q) == 0)
    {
      double bg_sd = elanpress_image_quality (bg, (unsigned int) sz);
      if (bg_sd < 25.0) has_bg = 1;
    }

  char pbuf[512];
  snprintf (pbuf, sizeof (pbuf), "%s/%s_bg.raw8", out_dir, prefix);
  write_bin (pbuf, bg, (size_t) sz);

  snprintf (pbuf, sizeof (pbuf), "%s/captures.jsonl", out_dir);
  FILE *logf = fopen (pbuf, "a");

  for (int i = start_idx; i < count; i++)
    {
      printf ("  [FT9201 %d/%d] %s — touch USB reader...\n", i + 1, count, label);
      fflush (stdout);

      while (1)
        {
          ft_ctrl_in (h, 0x3a, 0, 0x1d, rb, 4);
          if (rb[0] == 0xa0) break;
          usleep (20000);
        }
      usleep (100000); /* Exact 100 ms FT9201 settle */
      if (ft_capture_frame (h, w, hgt, raw, q) != 0)
        {
          fprintf (stderr, "  ! read failed, retrying touch\n");
          i--;
          continue;
        }
      for (int k = 0; k < sz; k++)
        {
          int v = has_bg ? ((int) raw[k] - (int) bg[k] + 128) : (int) raw[k];
          fpn[k] = (uint8_t) (v < 0 ? 0 : (v > 255 ? 255 : v));
        }
      normalize_percentile_u8 (fpn, norm, sz);
      double fpn_sd = elanpress_image_quality (fpn, (unsigned int) sz);
      double norm_sd = elanpress_image_quality (norm, (unsigned int) sz);

      snprintf (pbuf, sizeof (pbuf), "%s/%s_%02d.raw8", out_dir, prefix, i);
      write_bin (pbuf, raw, (size_t) sz);
      snprintf (pbuf, sizeof (pbuf), "%s/%s_%02d.u8", out_dir, prefix, i);
      write_bin (pbuf, norm, (size_t) sz);

      /* Also save exact 64x80 center-cropped OMGrant/Windows-DLL equivalent! */
      if (w == 96 && hgt == 96)
        {
          uint8_t c64[64 * 80], c64_norm[64 * 80];
          for (int r = 0; r < 80; r++)
            memcpy (c64 + r * 64, fpn + (r + 8) * 96 + 16, 64);
          normalize_percentile_u8 (c64, c64_norm, 64 * 80);
          snprintf (pbuf, sizeof (pbuf), "%s/%s_%02d.u8_64x80", out_dir, prefix, i);
          write_bin (pbuf, c64_norm, 64 * 80);
        }

      if (logf)
        {
          fprintf (logf,
                   "{\"sensor\":\"ft9201\",\"w\":%d,\"h\":%d,\"prefix\":\"%s\",\"idx\":%d,"
                   "\"label\":\"%s\",\"fpn_sd\":%.2f,\"sd\":%.2f,\"q0\":%u,\"q1\":%u,\"ts\":%ld}\n",
                   w, hgt, prefix, i, label, fpn_sd, norm_sd, q[0], q[1], (long) time (NULL));
          fflush (logf);
        }
      printf ("    OK  fpn_sd=%.1f  norm_sd=%.1f  (lift finger)\n", fpn_sd, norm_sd);
      fflush (stdout);

      /* Wait for lift */
      while (1)
        {
          ft_ctrl_in (h, 0x3a, 0, 0x1d, rb, 4);
          if (rb[0] != 0xa0) break;
          usleep (20000);
        }
      usleep (60000);
    }

  if (logf) fclose (logf);
  free (bg); free (raw); free (fpn); free (norm);
  libusb_release_interface (h, 0);
  libusb_close (h);
  libusb_exit (ctx);
  return 0;
}

int
main (int argc, char **argv)
{
  if (argc < 5)
    {
      fprintf (stderr, "Usage: %s <elan|ft9201> <out_dir> <prefix> <count> [label]\n", argv[0]);
      return 1;
    }
  const char *sensor = argv[1];
  const char *out_dir = argv[2];
  const char *prefix = argv[3];
  int count = atoi (argv[4]);
  const char *label = argc > 5 ? argv[5] : prefix;
  int start_idx = argc > 6 ? atoi (argv[6]) : 0;
  if (start_idx < 0) start_idx = 0;
  if (start_idx >= count) return 0;

  mkdir (out_dir, 0700);
  if (strcmp (sensor, "elan") == 0)
    return capture_elan (out_dir, prefix, count, label, start_idx);
  if (strcmp (sensor, "ft9201") == 0)
    return capture_ft9201 (out_dir, prefix, count, label, start_idx);

  fprintf (stderr, "Unknown sensor: %s\n", sensor);
  return 1;
}
