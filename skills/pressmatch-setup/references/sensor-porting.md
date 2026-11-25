# Porting PressMatch v4.1 to Similar Small-Area Capacitive Sensors

Use this guide when `detect_hardware.sh` detects a USB fingerprint sensor whose VID:PID is **not** yet in `elanpress_id_table` (`04f3:0c6e`, `04f3:0c4f`, `04f3:0c63`) or `ft9201` (`2808:93a9`).

## 1. Confirm Match-on-Host vs. Match-on-Chip (MoC)

Before writing driver code, inspect the USB descriptor:

```bash
lsusb -v -d <VID>:<PID>
```

- **Match-on-Host (ELAN press family):** Typically vendor `04f3`, 1 vendor-specific interface (`bInterfaceClass = 255`) with:
  - Bulk OUT endpoint `0x01` (2-byte commands: `00 0c` dimensions, `00 09` capture frame, `00 0b` stop, `40 13` status)
  - Bulk IN command response endpoint `0x83`
  - Bulk IN image stream endpoint `0x82` (raw 16-bit little-endian pixel stream, e.g. `150×52×2 = 15,600 B`, `96×96×2 = 18,432 B`, `80×80×2`, or `64×80×2`)
- **Match-on-Chip (ELAN MoC / Goodix MoC / Synaptics MoC / FPC MoC):** Usually responds to structured TLV/firmware-matching commands rather than streaming raw 16-bit ridge frames. Check if stock `libfprint` (`elanmoc`, `goodixmoc`, `fpcmoc`, `synaptics`, `focaltech_moc`) already claims that VID:PID — if so, do **not** redirect it to `elanpress`; instead keep stock `libfprint` for that device and still install `pam_pressmatch_dual.so` + the Touch ID HUD!

## 2. Non-Destructive Raw Frame Probe (`04f3:XXXX` ELAN Candidates)

Compile the standalone libusb probe tool from this repo:

```bash
cd paper-benchmark
gcc -O2 -Wall -Wextra raw_capture.c $(pkg-config --cflags --libs libusb-1.0) -o raw_capture -lm
```

Verify the sensor responds to `00 0c` (get dimensions) and streams a crisp ridge image when touched. Check:
1. **Sensor dimensions (`W × H`)**: Reported by `00 0c` (1-indexed raw width/height bytes).
2. **Dynamic range & background level**: Confirm idle (no finger) vs. finger-present mean delta exceeds `ELANPRESS_TOUCH_MIN_MEAN_DELTA` and does not clip at the ADC rail.
3. **Orientation / aspect ratio**: `elanpress.c` automatically rotates raw frames to row-major (`frame_width × frame_height`).

## 3. Registering a Compatible ELAN PID

1. Check `release-v4.1/source/libfprint/drivers/elan.c` and `elanmoc/` to make sure no other `libfprint` driver claims the same `VID:PID` (duplicate VID:PIDs cause nondeterministic probe conflicts).
2. Add the entry to `elanpress_id_table[]` in `release-v4.1/source/libfprint/drivers/elanpress.h`:
   ```c
   {.vid = ELANPRESS_VEND_ID, .pid = 0xXXXX, }, /* User's ELAN press sensor */
   ```
3. **One-Touch Power-On SSO (`0x9a 0x10`)**: Only enable pre-capture SRAM polling if `dual-reader/pressmatch-elan-sso --check` receives a valid `40 9a 10 ...` response without USB stall/timeout. Otherwise keep `check_precap = FALSE` for that PID.

## 4. Calibrating Thresholds for New Geometries

The PressMatch v4.1 core (`elanpress-match.c` + `elanpress-sift.c`) is resolution-agnostic and already calibrated across three geometries (`64×80`, `150×52`, `96×96`):
- **Tiny sensors (`< 6,000 px`, e.g., `64×80` or `80×64`):** Use `ELANPRESS_ENROLL_STAGES = 12`, keep quad-tier thresholds (`T0: 14/0.25/400`, `T1: 10/0.45/400`, `T2: 8/0.60/1200`, `T3: 6/0.72/2000`).
- **Square sensors (`~96×96`, `≥ 8,500 px`):** 10 enrollment stages (`FT9201_ENROLL_STAGES = 10`) suffice for `>95%` live-scan TAR at `0` false accepts.
- **Very narrow strips (`< 48 px` short edge):** Keep `ELANPRESS_CANDIDATE_EDGE_RATIO = 25.0f` and increase `ELANPRESS_ENROLL_STAGES` to `14–16` so spatial Pose-Graph stitching bridges across strips reliably.

Always run `release-v4.1/test_v41_asan` and test at least **10 genuine touches (flat + shifted + tilted)** AND **10 wrong-finger impostor touches** before enabling PAM login.
