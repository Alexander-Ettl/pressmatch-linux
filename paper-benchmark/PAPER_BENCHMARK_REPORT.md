# PressMatch: Comprehensive Multi-Algorithm & Multi-Sensor Scientific Benchmark

**Evaluation Corpus:** 11 public 500 DPI real human fingerprint databases (`206` distinct fingers, `1,648` impressions), evaluated across **3 physical sensor geometries** (`64×80` legacy Windows-DLL crop, `150×52` ELAN built-in strip, `96×96` FT9201 native square) and **6 compiled C biometric algorithms** = **`1,216,800` impostor trials per algorithm family** (`7,416` held-out genuine trials).

## 1. Master Comparison Matrix (All 6 Algorithms × All 3 Sensor Geometries)

| Sensor Geometry | Algorithm / Version | TAR @ FAR ≤ 1/100 (1%) | TAR @ FAR ≤ 1/50k (Touch ID) | Live-Scan TAR (116 f., ≤1/50k) | Shipped Empirical FAR | 95% Clopper-Pearson Upper Bound | Cold CPU (p50) | Template |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| FT9201 64×80 (Crop) | 1. NBIS (`mindtct`+`bozorth3`, stock `libfprint`) | 0.0% | **0.0%** | **0.0%** | `0 / 405,600` (0.0000%) | < 1 in 135,393 | `1.2 ms` | `28 KB` |
| FT9201 64×80 (Crop) | 2. Upstream `elanpress` v0 (Lowe SIFT, `edge=10`) | 70.9% | **58.4%** | **85.6%** | `0 / 101,400` (0.0000%) | < 1 in 33,849 | `100.9 ms` | `60 KB` |
| FT9201 64×80 (Crop) | 3. PressMatch v1 (RootSIFT, `edge=25`, indep.) | 68.2% | **61.8%** | **87.9%** | `0 / 101,400` (0.0000%) | < 1 in 33,849 | `11.8 ms` | `290 KB` |
| FT9201 64×80 (Crop) | 4. PressMatch v2 (PoseGraph pairs-only) | 85.8% | **64.6%** | **90.2%** | `4 / 405,600` (1 in 101,400) | < 1 in 44,348 | `20.0 ms` | `317 KB` |
| FT9201 64×80 (Crop) | 5. PressMatch v4.0 (PoseGraph + Rigid ZNCC) | 59.3% | **59.3%** | **82.8%** | `0 / 405,600` (0.0000%) | < 1 in 135,393 | `20.4 ms` | `317 KB` |
| FT9201 64×80 (Crop) | **6. PressMatch v4.1 (Elastic ZNCC + Quad + SIMD)** | 72.9% | **63.6%** | **88.7%** | `0 / 405,600` (0.0000%) | < 1 in 135,393 | `20.6 ms` | `317 KB` |
| ELAN 150×52 (Strip) | 1. NBIS (`mindtct`+`bozorth3`, stock `libfprint`) | 0.0% | **0.0%** | **0.0%** | `0 / 405,600` (0.0000%) | < 1 in 135,393 | `2.0 ms` | `28 KB` |
| ELAN 150×52 (Strip) | 2. Upstream `elanpress` v0 (Lowe SIFT, `edge=10`) | 73.0% | **61.9%** | **88.1%** | `0 / 101,400` (0.0000%) | < 1 in 33,849 | `141.2 ms` | `91 KB` |
| ELAN 150×52 (Strip) | 3. PressMatch v1 (RootSIFT, `edge=25`, indep.) | 70.1% | **61.4%** | **87.1%** | `0 / 101,400` (0.0000%) | < 1 in 33,849 | `18.4 ms` | `433 KB` |
| ELAN 150×52 (Strip) | 4. PressMatch v2 (PoseGraph pairs-only) | 85.8% | **67.5%** | **91.1%** | `0 / 405,600` (0.0000%) | < 1 in 135,393 | `29.1 ms` | `360 KB` |
| ELAN 150×52 (Strip) | 5. PressMatch v4.0 (PoseGraph + Rigid ZNCC) | 56.6% | **56.6%** | **76.9%** | `0 / 405,600` (0.0000%) | < 1 in 135,393 | `29.6 ms` | `360 KB` |
| ELAN 150×52 (Strip) | **6. PressMatch v4.1 (Elastic ZNCC + Quad + SIMD)** | 73.7% | **65.7%** | **89.6%** | `0 / 405,600` (0.0000%) | < 1 in 135,393 | `29.8 ms` | `360 KB` |
| FT9201 96×96 (Native) | 1. NBIS (`mindtct`+`bozorth3`, stock `libfprint`) | 0.2% | **0.2%** | **0.2%** | `0 / 405,600` (0.0000%) | < 1 in 135,393 | `2.5 ms` | `28 KB` |
| FT9201 96×96 (Native) | 2. Upstream `elanpress` v0 (Lowe SIFT, `edge=10`) | 79.6% | **69.5%** | **94.3%** | `0 / 101,400` (0.0000%) | < 1 in 33,849 | `186.9 ms` | `108 KB` |
| FT9201 96×96 (Native) | 3. PressMatch v1 (RootSIFT, `edge=25`, indep.) | 76.9% | **70.5%** | **94.0%** | `0 / 101,400` (0.0000%) | < 1 in 33,849 | `20.5 ms` | `471 KB` |
| FT9201 96×96 (Native) | 4. PressMatch v2 (PoseGraph pairs-only) | 89.5% | **75.1%** | **96.4%** | `1 / 405,600` (1 in 405,600) | < 1 in 85,786 | `34.2 ms` | `368 KB` |
| FT9201 96×96 (Native) | 5. PressMatch v4.0 (PoseGraph + Rigid ZNCC) | 66.6% | **66.6%** | **85.6%** | `0 / 405,600` (0.0000%) | < 1 in 135,393 | `34.8 ms` | `368 KB` |
| FT9201 96×96 (Native) | **6. PressMatch v4.1 (Elastic ZNCC + Quad + SIMD)** | 81.6% | **74.4%** | **95.9%** | `0 / 405,600` (0.0000%) | < 1 in 135,393 | `34.9 ms` | `368 KB` |

## 2. Pessimistic Statistical Audit & Threats to Validity (Peer-Review Readiness)

To ensure our scientific claims withstand hostile peer review:
1. **Exact Apple Touch ID Clarification:** Apple publishes **`1 in 50,000`** (`0.002%`) FAR per enrolled finger for Touch ID (not `1 in 100`). Above we report **both** loose `1 in 100` (`1%`) AND strict Touch ID `1 in 50,000` (`0.002%`) AND zero-error `0 / 405,600` operating points.
2. **Independent Finger-Pair Bound (Pessimistic Accounting):** Because the `405,600` impostor trials come from `33,800` distinct `(enrolled_finger, impostor_finger)` pairs × 12 probe crops each, repeated probes from the same finger-pair are correlated. Counting **strictly 1 i.i.d. probe per finger-pair (`N = 33,800` independent trials)** yields a conservative 95% Clopper-Pearson upper bound of **`< 1 in 11,267`** (`0 / 33,800`), and counting **strict same-sensor trials (`N = 90,240`)** yields **`< 1 in 30,080`** (`0 / 90,240`).
3. **Multi-Finger `identify` Penalty:** Enrolling `M` fingers multiplies per-tap FAR by approximately `M` (`1 - (1 - FAR)^M ≈ M · FAR`). At 2 enrolled fingers, our 95% bound is `< 1 in 67,600` (all-pool) / `< 1 in 15,040` (strict same-sensor).
4. **Optical-to-Capacitive Domain Gap (Why Live Multi-Person Capture Matters):** Cropped public 500 DPI datasets do not capture real fixed-gain capacitive ADC saturation (warm/oily skin) or physical grime/lotion contamination. That is precisely why our local multi-profile hardware capture (`capture_profile.py` + `replay_profiles.py`) tests multiple human participants on real physical `04f3:0c6e` and `2808:93a9` frames under clean AND intentionally dirty/grimy conditions!

## 3. Publication Figures
- [fig1_algorithm_evolution.png](figures/fig1_algorithm_evolution.png) — Step-by-step TAR evolution across all 6 algorithms
- [fig2_security_det_curve.png](figures/fig2_security_det_curve.png) — Security-Usability Log-FAR DET curves
- [fig3_sensor_geometry.png](figures/fig3_sensor_geometry.png) — Sensor geometry ablation (`64×80` vs `150×52` vs `96×96`)
- [fig4_speed_pareto.png](figures/fig4_speed_pareto.png) — Cold-start CPU latency & accuracy Pareto frontier
- [fig5_live_hardware_replay.png](figures/fig5_live_hardware_replay.png) — Anonymized live physical capacitive hardware replay (`4` participants, `8` fingers, `288` genuine + `2,016` cross-impostor trials, `0` false accepts)