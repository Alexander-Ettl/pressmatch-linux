/*
 * pressmatch-elan-sso — ELAN EMSE1000 (04f3:0c6e) One-Touch Power-On / Sleep SSO helper
 *
 * Commands verified from official ASUS WbfUsbDriver.dll (3.5.12201.11508):
 *   CheckPreCaptureImage : EP1 OUT {0x9a, 0x10} -> EP3 IN 4B (b0==0x00 has image, 0x40 none)
 *   DeletePreCaptureImage: EP1 OUT {0x9a, 0x14}
 *   SoftwareSSO_On (S3)  : EP1 OUT {0x46, 0x07, 0x80, 0x01, 0x04, 0x00, 0x00, 0x00, 0x00}
 *   SoftwareSSO_Off(S4/5): EP1 OUT {0x46, 0x07, 0x80, 0x01, 0x04, 0x40, 0x00, 0x00, 0x00}
 *
 * Build:
 *   gcc -O2 -Wall -Wextra elan_sso_helper.c $(pkg-config --cflags --libs libusb-1.0) -o pressmatch-elan-sso
 */
#include <libusb-1.0/libusb.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ELAN_VID 0x04f3
#define ELAN_PID 0x0c6e
#define EP_CMD_OUT 0x01
#define EP_CMD_IN  0x83

int
main (int argc, char **argv)
{
  const char *mode = (argc > 1) ? argv[1] : "--check";
  libusb_context *ctx = NULL;
  libusb_device_handle *h = NULL;
  int rc = 0;

  if (libusb_init (&ctx) != 0)
    return 1;

  h = libusb_open_device_with_vid_pid (ctx, ELAN_VID, ELAN_PID);
  if (!h)
    {
      fprintf (stderr, "elan-sso: 04f3:0c6e not found or no permission\n");
      libusb_exit (ctx);
      return 2;
    }

  if (libusb_claim_interface (h, 0) != 0)
    {
      /* Device busy (e.g. fprintd currently verifying) — skip quietly */
      libusb_close (h);
      libusb_exit (ctx);
      return 3;
    }

  int xfer = 0;
  if (strcmp (mode, "--swsso-on") == 0)
    {
      uint8_t cmd[9] = { 0x46, 0x07, 0x80, 0x01, 0x04, 0x00, 0x00, 0x00, 0x00 };
      rc = libusb_bulk_transfer (h, EP_CMD_OUT, cmd, sizeof (cmd), &xfer, 1000);
      printf ("elan-sso: SoftwareSSO_On rc=%d xfer=%d\n", rc, xfer);
    }
  else if (strcmp (mode, "--swsso-off") == 0)
    {
      uint8_t cmd[9] = { 0x46, 0x07, 0x80, 0x01, 0x04, 0x40, 0x00, 0x00, 0x00 };
      rc = libusb_bulk_transfer (h, EP_CMD_OUT, cmd, sizeof (cmd), &xfer, 1000);
      printf ("elan-sso: SoftwareSSO_Off rc=%d xfer=%d\n", rc, xfer);
    }
  else if (strcmp (mode, "--delete") == 0)
    {
      uint8_t cmd[2] = { 0x9a, 0x14 };
      rc = libusb_bulk_transfer (h, EP_CMD_OUT, cmd, sizeof (cmd), &xfer, 1000);
      printf ("elan-sso: DeletePreCaptureImage rc=%d xfer=%d\n", rc, xfer);
    }
  else
    {
      uint8_t cmd[2] = { 0x9a, 0x10 };
      uint8_t resp[4] = { 0xff, 0xff, 0xff, 0xff };
      rc = libusb_bulk_transfer (h, EP_CMD_OUT, cmd, sizeof (cmd), &xfer, 1000);
      if (rc == 0)
        {
          int rlen = 0;
          rc = libusb_bulk_transfer (h, EP_CMD_IN, resp, sizeof (resp), &rlen, 1000);
          printf ("elan-sso: CheckPreCaptureImage rc=%d len=%d bytes=%02x %02x %02x %02x (%s)\n",
                  rc, rlen, resp[0], resp[1], resp[2], resp[3],
                  (rc == 0 && rlen >= 1 && resp[0] == 0x00) ? "HAS_PRECAP_IMAGE" :
                  (rc == 0 && rlen >= 1 && resp[0] == 0x40) ? "NO_PRECAP_IMAGE" : "OTHER");
        }
      else
        {
          fprintf (stderr, "elan-sso: CheckPreCaptureImage write failed rc=%d\n", rc);
        }
    }

  libusb_release_interface (h, 0);
  libusb_close (h);
  libusb_exit (ctx);
  return (rc == 0) ? 0 : 4;
}
