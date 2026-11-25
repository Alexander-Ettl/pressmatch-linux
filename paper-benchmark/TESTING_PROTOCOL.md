# PressMatch Research Paper — Local Multi-Profile 10-Minute Testing Protocol

**Privacy & Local-Only Guarantee:** All captures and replays run **100% locally** under `paper-benchmark/profiles/<name>/` (`chmod 700`). No cloud service, Cloudflare worker, or network upload is ever used. Raw frames can be securely deleted after generating aggregate CSV/JSON tables for the paper.

---

## 1. Why Capture Once & Replay Offline Across All Algorithms?

1. **Why we cannot reuse `/var/lib/fprint/<user>/` across other algorithms:**
   - `fprintd` only stores pre-extracted `v4.1` templates (4-bit packed strips + RootSIFT descriptors) with zero raw 14-bit ELAN / 8-bit FT9201 probe frames and zero held-out posture/grime/impostor captures.
2. **Why Capture-Once-Replay-All is the gold standard in biometric papers:**
   - Capturing raw lossless frames **once** per participant guarantees **100% paired evaluation** — every algorithm (`NBIS Bozorth3`, `Upstream v0`, `PressMatch v1`, `v2`, `v4.0`, `v4.1`, plus `FT9201 64×80 crop` vs `96×96 native`) is scored on the **exact same physical touches**, eliminating skin moisture/temperature drift between algorithms and cutting participant testing time by **6×**.

---

## 2. The Strict ≤ 10-Minute Protocol Per Participant

Each participant (e.g., `p01`, `p02`, `p03`, `p04`) runs **one 8–9 minute clean session** (hard-capped at `10:00` by `capture_profile.py`), plus an **optional ~3-minute contamination (`--dirty`) session**.

### Preparation (Before Starting Timer)
1. Wipe both sensors dry with a clean microfiber cloth.
2. Participant chooses **any 2 comfortable fingers** (`Finger 1` and `Finger 2` — e.g., right index + left index, or whichever feels most ergonomic). They must use the **same two fingers** on both ELAN and FT9201.
3. Hands should be at normal clean, dry room temperature (washed & dried ≥ 5 minutes prior).

### Clean Session (~8–9 Minutes Total, 48 Touches/Sensor)
Run locally:
```bash
sudo python3 paper-benchmark/capture_profile.py --profile p01
```
Repeat for each participant (`--profile p02`, `--profile p03`, `--profile p04`, etc.):

| Block | Touches | Sensor | Purpose |
|---|---:|---|---|
| `f1_enr_flat / shift / tilt` | `4 + 4 + 4 = 12` | ELAN `150×52` | Guided enrollment mosaic (Finger 1) |
| `f1_prb_flat / shift / tilt` | `4 + 4 + 4 = 12` | ELAN `150×52` | Held-out clean probes (posture torture) |
| `f2_enr_flat / shift / tilt` | `4 + 4 + 4 = 12` | ELAN `150×52` | Guided enrollment mosaic (Finger 2) |
| `f2_prb_flat / shift / tilt` | `4 + 4 + 4 = 12` | ELAN `150×52` | Held-out clean probes + cross-finger impostors |
| *(Same 4 blocks repeated)* | `48` | FT9201 `96×96` *(+ auto `64×80` crop)* | Direct paired sensor geometry comparison |

### Optional Contamination / Grime Session (~3 Minutes, 16 Touches/Sensor)
Tests which algorithm **maintains 0% false accepts** while tolerating real-world skin contamination:
```bash
sudo python3 paper-benchmark/capture_profile.py --profile p01 --dirty
```
- **Oil / Lotion block (`4 + 4` touches):** Rub a tiny, repeatable amount of standard hand lotion or forehead sebum onto the fingertip (simulates warm/oily capacitive valley bridging).
- **Grime / Dust block (`4 + 4` touches):** Lightly tap dry flour, chalk dust, or desk grime onto the fingertip (simulates particulate ridge occlusion).
- Wipe both sensors clean immediately afterward!

---

## 3. Automatic Multi-Algorithm Replay (Zero Extra Touches)

Once 1 or more profiles are captured, replay **every profile** across **all 6 algorithms × 3 geometries** in seconds:
```bash
python3 paper-benchmark/replay_profiles.py
```

### Statistical Power With 4 Profiles (`p01`–`p04`)
- **8 distinct real physical fingers** tested on both ELAN (`150×52`) and FT9201 (`96×96` + `64×80`).
- **Genuine clean probes:** `96` per sensor (`192` total) across flat, shifted, and `±15°` tilted touches.
- **Genuine contaminated probes:** `64` per sensor (`128` total) across oily and grimy fingers.
- **Real physical cross-person & cross-finger impostors (automatic):** `8 × 7 × 12 = 672` clean + `448` dirty = **`1,120` real capacitive impostor trials per sensor** (`2,240` total) — including **biologically related family members** (a harsher stress test than random strangers!).
- Combined with our **1,216,800-trial 11-database public corpus**, this gives both **large-scale `1 in N` statistical proof** AND **real multi-subject physical capacitive hardware validation**!
