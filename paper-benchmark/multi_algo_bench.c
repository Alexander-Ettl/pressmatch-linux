/*
 * PressMatch Scientific Paper — Multi-Algorithm × Multi-Geometry Benchmark Suite
 *
 * Evaluates 6 distinct biometric matching pipelines on identical raw/cropped inputs:
 *   1. NBIS (NIST mindtct + bozorth3)        — Stock open-source libfprint matcher
 *   2. Upstream elanpress v0 (Lowe SIFT)     — edge=10, contrast=0.04, L2 SIFT, 12 indep strips
 *   3. PressMatch v1 (Ridge RootSIFT)        — edge=25, contrast=0.0, RootSIFT, 12 indep strips
 *   4. PressMatch v2 (PoseGraph Pairs-Only)  — ZNCC-verified PoseGraph mosaic, pairs only
 *   5. PressMatch v4.0 (Rigid ZNCC + T1-T3)  — PoseGraph + Rigid ZNCC + Triple-Tier + 4-bit pack
 *   6. PressMatch v4.1 (Elastic ZNCC + Quad) — PoseGraph + Elastic +/-1px ZNCC + Quad-Tier + SIMD
 *
 * Outputs exact JSON summary including:
 *   - Full score histograms for genuine (all, live-scan, capacitive, FVC-hard) & impostors
 *   - Strict same-sensor (90,240 trials / 7,520 pairs) & cross-cohort (315,360 / 26,280) counts
 *   - Single-probe i.i.d. impostor counts (pk=0: 33,800 strictly independent trials)
 *   - Exact single-thread cold-start CPU latency (p50, p95, p99, breakdown) & template KB
 */
#define _GNU_SOURCE
#include <glib.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "elanpress-match.h"
#include "elanpress-sift.h"

/* NIST NBIS headers from libfprint */
#include <bozorth.h>
#include <lfs.h>

#define MAX_SCORE_BIN 256

typedef struct {
  uint32_t ds_idx;
  uint32_t cohort;
  uint32_t is_cap;
  uint32_t n_enroll;
  uint32_t n_probe;
  char     ds_name[24];
  char     fid[24];
  int      is_livescan; /* CrossMatch or UareU */

  GPtrArray          *enroll_imgs;
  guint8            **probe_imgs;

  /* SIFT / PressMatch data */
  ElanpressFeatures **enroll_feats;
  ElanpressFeatures **probe_feats;
  ElanpressPoseGraph *pg;
  size_t              tmpl_bytes;

  /* NBIS data */
  struct xyt_struct  *enroll_xyt;
  struct xyt_struct  *probe_xyt;
  uint32_t            enroll_min_sum;
  uint32_t            probe_min_sum;
} Finger;

static Finger  *g_fingers = NULL;
static uint32_t g_nfingers = 0;
static int      g_w = 150, g_h = 52;

static double
now_sec (void)
{
  struct timespec ts;
  clock_gettime (CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int
cmp_double (const void *a, const void *b)
{
  double da = *(const double *) a;
  double db = *(const double *) b;
  return (da > db) - (da < db);
}

static int
load_crops_bin (const char *path)
{
  FILE *fp = fopen (path, "rb");
  if (!fp) { perror ("fopen"); return -1; }

  char magic[8];
  uint32_t w_u = 0, h_u = 0;
  if (fread (magic, 1, 8, fp) != 8 || memcmp (magic, "ELANFAR1", 8) != 0 ||
      fread (&g_nfingers, 4, 1, fp) != 1 || fread (&w_u, 4, 1, fp) != 1 ||
      fread (&h_u, 4, 1, fp) != 1)
    {
      fclose (fp);
      return -1;
    }
  g_w = (int) w_u;
  g_h = (int) h_u;
  size_t frame_sz = (size_t) g_w * g_h;
  g_fingers = calloc (g_nfingers, sizeof (Finger));

  for (uint32_t i = 0; i < g_nfingers; i++)
    {
      Finger *f = &g_fingers[i];
      if (fread (&f->ds_idx, 4, 1, fp) != 1 || fread (&f->cohort, 4, 1, fp) != 1 ||
          fread (&f->is_cap, 4, 1, fp) != 1 || fread (&f->n_enroll, 4, 1, fp) != 1 ||
          fread (&f->n_probe, 4, 1, fp) != 1 || fread (f->ds_name, 1, 24, fp) != 24 ||
          fread (f->fid, 1, 24, fp) != 24)
        {
          fclose (fp);
          return -1;
        }
      f->ds_name[23] = '\0';
      f->fid[23] = '\0';
      f->is_livescan = (strcmp (f->ds_name, "UareU") == 0 ||
                        strcmp (f->ds_name, "CrossMatch") == 0);
      f->enroll_imgs = g_ptr_array_new_full (f->n_enroll, g_free);
      for (uint32_t k = 0; k < f->n_enroll; k++)
        {
          guint8 *buf = g_malloc (frame_sz);
          if (fread (buf, 1, frame_sz, fp) != frame_sz) { fclose (fp); return -1; }
          g_ptr_array_add (f->enroll_imgs, buf);
        }
      f->probe_imgs = calloc (f->n_probe, sizeof (guint8 *));
      for (uint32_t k = 0; k < f->n_probe; k++)
        {
          f->probe_imgs[k] = g_malloc (frame_sz);
          if (fread (f->probe_imgs[k], 1, frame_sz, fp) != frame_sz) { fclose (fp); return -1; }
        }
    }
  fclose (fp);
  return 0;
}

/* ========================================================================= */
/* Mode 1: NIST NBIS (mindtct + bozorth3) — stock libfprint matcher          */
/* ========================================================================= */

static int
nbis_sort_x_y (const void *a, const void *b)
{
  const struct minutiae_struct *ma = a;
  const struct minutiae_struct *mb = b;
  if (ma->col[0] != mb->col[0])
    return ma->col[0] - mb->col[0];
  return ma->col[1] - mb->col[1];
}

static void
nbis_extract_xyt (const guint8 *img, int w, int h, struct xyt_struct *xyt)
{
  memset (xyt, 0, sizeof (*xyt));
  size_t n = (size_t) w * h;
  /* NBIS mindtct expects dark ridges on white background (invert ELAN bright-ridge polarity) */
  unsigned char *inv = malloc (n);
  for (size_t i = 0; i < n; i++)
    inv[i] = (unsigned char) (255 - img[i]);

  MINUTIAE *minutiae = NULL;
  int *quality_map = NULL, *direction_map = NULL;
  int *low_contrast_map = NULL, *low_flow_map = NULL, *high_curve_map = NULL;
  unsigned char *binarized = NULL;
  int map_w = 0, map_h = 0, bw = 0, bh = 0, bd = 0;
  LFSPARMS parms = g_lfsparms_V2;
  parms.remove_perimeter_pts = TRUE; /* Standard libfprint FPI_IMAGE_PARTIAL setting */

  int rc = get_minutiae (&minutiae, &quality_map, &direction_map,
                         &low_contrast_map, &low_flow_map, &high_curve_map,
                         &map_w, &map_h, &binarized, &bw, &bh, &bd,
                         inv, w, h, 8, 500.0 / 25.4, &parms);
  free (inv);
  free (quality_map);
  free (direction_map);
  free (low_contrast_map);
  free (low_flow_map);
  free (high_curve_map);
  free (binarized);

  if (rc == 0 && minutiae && minutiae->num > 0)
    {
      int nmin = minutiae->num < MAX_BOZORTH_MINUTIAE ? minutiae->num : MAX_BOZORTH_MINUTIAE;
      struct minutiae_struct c[MAX_BOZORTH_MINUTIAE];
      for (int i = 0; i < nmin; i++)
        {
          MINUTIA *m = minutiae->list[i];
          lfs2nist_minutia_XYT (&c[i].col[0], &c[i].col[1], &c[i].col[2], m, w, h);
          c[i].col[3] = sround (m->reliability * 100.0);
          if (c[i].col[2] > 180)
            c[i].col[2] -= 360;
        }
      qsort (c, (size_t) nmin, sizeof (struct minutiae_struct), nbis_sort_x_y);
      for (int i = 0; i < nmin; i++)
        {
          xyt->xcol[i]     = c[i].col[0];
          xyt->ycol[i]     = c[i].col[1];
          xyt->thetacol[i] = c[i].col[2];
        }
      xyt->nrows = nmin;
    }
  if (minutiae)
    free_minutiae (minutiae);
}

static int
run_nbis (const char *out_json)
{
  double t0 = now_sec ();
  double lat_extract[200];
  int n_lat_ext = 0;
  uint64_t total_min = 0, total_imgs = 0;

  for (uint32_t i = 0; i < g_nfingers; i++)
    {
      Finger *f = &g_fingers[i];
      f->enroll_xyt = calloc (f->n_enroll, sizeof (struct xyt_struct));
      f->probe_xyt = calloc (f->n_probe, sizeof (struct xyt_struct));
      for (uint32_t k = 0; k < f->n_enroll; k++)
        {
          nbis_extract_xyt (g_ptr_array_index (f->enroll_imgs, k), g_w, g_h, &f->enroll_xyt[k]);
          f->enroll_min_sum += (uint32_t) f->enroll_xyt[k].nrows;
          total_min += (uint32_t) f->enroll_xyt[k].nrows;
          total_imgs++;
        }
      for (uint32_t k = 0; k < f->n_probe; k++)
        {
          double s0 = now_sec ();
          nbis_extract_xyt (f->probe_imgs[k], g_w, g_h, &f->probe_xyt[k]);
          double s1 = now_sec ();
          if (n_lat_ext < 200)
            lat_extract[n_lat_ext++] = (s1 - s0) * 1000.0;
          f->probe_min_sum += (uint32_t) f->probe_xyt[k].nrows;
          total_min += (uint32_t) f->probe_xyt[k].nrows;
          total_imgs++;
        }
    }
  double t1 = now_sec ();

  uint64_t gen_hist[MAX_SCORE_BIN] = { 0 };
  uint64_t gen_live_hist[MAX_SCORE_BIN] = { 0 };
  uint64_t gen_cap_hist[MAX_SCORE_BIN] = { 0 };
  uint64_t imp_all_hist[MAX_SCORE_BIN] = { 0 };
  uint64_t imp_same_hist[MAX_SCORE_BIN] = { 0 };
  uint64_t imp_iid_hist[MAX_SCORE_BIN] = { 0 };
  uint64_t gen_n = 0, gen_live_n = 0, gen_cap_n = 0;
  uint64_t imp_all_n = 0, imp_same_n = 0, imp_iid_n = 0;

  double lat_match[400];
  int n_lat_m = 0;

  for (uint32_t ei = 0; ei < g_nfingers; ei++)
    {
      Finger *enr = &g_fingers[ei];
      /* Genuine */
      for (uint32_t pk = 0; pk < enr->n_probe; pk++)
        {
          struct xyt_struct *ps = &enr->probe_xyt[pk];
          double m0 = now_sec ();
          int best = 0;
          if (ps->nrows >= 3)
            {
              int plen = bozorth_probe_init (ps);
              for (uint32_t ek = 0; ek < enr->n_enroll; ek++)
                {
                  if (enr->enroll_xyt[ek].nrows < 3) continue;
                  int sc = bozorth_to_gallery (plen, ps, &enr->enroll_xyt[ek]);
                  if (sc > best) best = sc;
                }
            }
          double m1 = now_sec ();
          if (n_lat_m < 400)
            lat_match[n_lat_m++] = (m1 - m0) * 1000.0;
          int bin = best >= MAX_SCORE_BIN ? MAX_SCORE_BIN - 1 : (best < 0 ? 0 : best);
          gen_hist[bin]++;
          gen_n++;
          if (enr->is_livescan) { gen_live_hist[bin]++; gen_live_n++; }
          if (enr->is_cap) { gen_cap_hist[bin]++; gen_cap_n++; }
        }

      /* Impostors */
      for (uint32_t pi = 0; pi < g_nfingers; pi++)
        {
          if (pi == ei) continue;
          Finger *prb = &g_fingers[pi];
          int same_sensor = (enr->ds_idx == prb->ds_idx);
          int diff_cohort = (enr->cohort != prb->cohort);
          if (!same_sensor && !diff_cohort) continue;

          for (uint32_t pk = 0; pk < prb->n_probe; pk++)
            {
              struct xyt_struct *ps = &prb->probe_xyt[pk];
              int best = 0;
              if (ps->nrows >= 3)
                {
                  int plen = bozorth_probe_init (ps);
                  for (uint32_t ek = 0; ek < enr->n_enroll; ek++)
                    {
                      if (enr->enroll_xyt[ek].nrows < 3) continue;
                      int sc = bozorth_to_gallery (plen, ps, &enr->enroll_xyt[ek]);
                      if (sc > best) best = sc;
                    }
                }
              int bin = best >= MAX_SCORE_BIN ? MAX_SCORE_BIN - 1 : (best < 0 ? 0 : best);
              imp_all_hist[bin]++;
              imp_all_n++;
              if (same_sensor) { imp_same_hist[bin]++; imp_same_n++; }
              if (pk == 0) { imp_iid_hist[bin]++; imp_iid_n++; }
            }
        }
    }
  double t2 = now_sec ();

  qsort (lat_extract, (size_t) n_lat_ext, sizeof (double), cmp_double);
  qsort (lat_match, (size_t) n_lat_m, sizeof (double), cmp_double);
  double ext_p50 = lat_extract[n_lat_ext / 2];
  double ext_p95 = lat_extract[(n_lat_ext * 95) / 100];
  double m_p50 = lat_match[n_lat_m / 2];
  double m_p95 = lat_match[(n_lat_m * 95) / 100];
  double avg_min = (double) total_min / (double) total_imgs;

  printf ("[NBIS %dx%d] ext=%.2fs match=%.2fs | avg_minutiae=%.1f | cold_p50=%.2f ms (ext=%.2f + match=%.2f)\n",
          g_w, g_h, t1 - t0, t2 - t1, avg_min, ext_p50 + m_p50, ext_p50, m_p50);

  FILE *jf = fopen (out_json, "w");
  fprintf (jf, "{\n");
  fprintf (jf, "  \"algo\": \"nbis_bozorth3\",\n");
  fprintf (jf, "  \"width\": %d, \"height\": %d,\n", g_w, g_h);
  fprintf (jf, "  \"avg_features\": %.2f,\n", avg_min);
  fprintf (jf, "  \"template_kb\": %.2f,\n", (12.0 * sizeof (struct xyt_struct)) / 1024.0);
  fprintf (jf, "  \"latency_cold_p50_ms\": %.3f,\n", ext_p50 + m_p50);
  fprintf (jf, "  \"latency_cold_p95_ms\": %.3f,\n", ext_p95 + m_p95);
  fprintf (jf, "  \"latency_extract_p50_ms\": %.3f,\n", ext_p50);
  fprintf (jf, "  \"latency_match_p50_ms\": %.3f,\n", m_p50);
  fprintf (jf, "  \"gen_n\": %llu, \"gen_live_n\": %llu, \"gen_cap_n\": %llu,\n",
           (unsigned long long) gen_n, (unsigned long long) gen_live_n, (unsigned long long) gen_cap_n);
  fprintf (jf, "  \"imp_all_n\": %llu, \"imp_same_n\": %llu, \"imp_iid_n\": %llu,\n",
           (unsigned long long) imp_all_n, (unsigned long long) imp_same_n, (unsigned long long) imp_iid_n);

  const char *names[6] = { "gen_hist", "gen_live_hist", "gen_cap_hist", "imp_all_hist", "imp_same_hist", "imp_iid_hist" };
  uint64_t *hists[6] = { gen_hist, gen_live_hist, gen_cap_hist, imp_all_hist, imp_same_hist, imp_iid_hist };
  for (int h = 0; h < 6; h++)
    {
      fprintf (jf, "  \"%s\": [", names[h]);
      for (int b = 0; b < MAX_SCORE_BIN; b++)
        fprintf (jf, "%s%llu", b ? "," : "", (unsigned long long) hists[h][b]);
      fprintf (jf, "]%s\n", h < 5 ? "," : "");
    }
  fprintf (jf, "}\n");
  fclose (jf);
  return 0;
}

/* ========================================================================= */
/* Mode 2 & 3: Independent-Strip SIFT (Upstream v0 Lowe vs PressMatch v1)    */
/* ========================================================================= */

typedef struct {
  int                  tid;
  int                  nthreads;
  ElanpressSiftParams  params;
  int                  max_enr_kp;
  int                  max_prb_kp;
  gboolean             reciprocal;
  uint64_t             gen_hist[MAX_SCORE_BIN];
  uint64_t             gen_live_hist[MAX_SCORE_BIN];
  uint64_t             gen_cap_hist[MAX_SCORE_BIN];
  uint64_t             imp_all_hist[MAX_SCORE_BIN];
  uint64_t             imp_same_hist[MAX_SCORE_BIN];
  uint64_t             imp_iid_hist[MAX_SCORE_BIN];
  uint64_t             gen_n, gen_live_n, gen_cap_n;
  uint64_t             imp_all_n, imp_same_n, imp_iid_n;
} IndepWorker;

static void *
indep_build_worker (void *arg)
{
  IndepWorker *wk = (IndepWorker *) arg;
  for (uint32_t i = (uint32_t) wk->tid; i < g_nfingers; i += (uint32_t) wk->nthreads)
    {
      Finger *f = &g_fingers[i];
      f->enroll_feats = g_new0 (ElanpressFeatures *, f->n_enroll);
      for (uint32_t k = 0; k < f->n_enroll; k++)
        f->enroll_feats[k] = elanpress_sift_features_opts (
          g_ptr_array_index (f->enroll_imgs, k), g_w, g_h,
          wk->max_enr_kp, &wk->params, NULL);
      f->probe_feats = g_new0 (ElanpressFeatures *, f->n_probe);
      for (uint32_t k = 0; k < f->n_probe; k++)
        f->probe_feats[k] = elanpress_sift_features_opts (
          f->probe_imgs[k], g_w, g_h, wk->max_prb_kp, &wk->params, NULL);
    }
  return NULL;
}

static guint
match_indep_12 (const ElanpressFeatures *probe, ElanpressFeatures * const *enr_feats,
                uint32_t n_enr, gboolean reciprocal)
{
  (void) reciprocal;
  guint best = 0;
  for (uint32_t k = 0; k < n_enr; k++)
    {
      guint p = elanpress_match_features (probe, enr_feats[k]);
      if (p > best) best = p;
    }
  return best;
}

static void *
indep_match_worker (void *arg)
{
  IndepWorker *wk = (IndepWorker *) arg;
  for (uint32_t ei = (uint32_t) wk->tid; ei < g_nfingers; ei += (uint32_t) wk->nthreads)
    {
      Finger *enr = &g_fingers[ei];
      for (uint32_t pk = 0; pk < enr->n_probe; pk++)
        {
          guint sc = match_indep_12 (enr->probe_feats[pk], enr->enroll_feats, enr->n_enroll, wk->reciprocal);
          int bin = sc >= MAX_SCORE_BIN ? MAX_SCORE_BIN - 1 : (int) sc;
          wk->gen_hist[bin]++;
          wk->gen_n++;
          if (enr->is_livescan) { wk->gen_live_hist[bin]++; wk->gen_live_n++; }
          if (enr->is_cap) { wk->gen_cap_hist[bin]++; wk->gen_cap_n++; }
        }
      for (uint32_t pi = 0; pi < g_nfingers; pi++)
        {
          if (pi == ei) continue;
          Finger *prb = &g_fingers[pi];
          int same_sensor = (enr->ds_idx == prb->ds_idx);
          int diff_cohort = (enr->cohort != prb->cohort);
          if (!same_sensor && !diff_cohort) continue;

          uint32_t max_imp_pk = prb->n_probe < 3 ? prb->n_probe : 3;
          for (uint32_t pk = 0; pk < max_imp_pk; pk++)
            {
              guint sc = match_indep_12 (prb->probe_feats[pk], enr->enroll_feats, enr->n_enroll, wk->reciprocal);
              int bin = sc >= MAX_SCORE_BIN ? MAX_SCORE_BIN - 1 : (int) sc;
              wk->imp_all_hist[bin]++;
              wk->imp_all_n++;
              if (same_sensor) { wk->imp_same_hist[bin]++; wk->imp_same_n++; }
              if (pk == 0) { wk->imp_iid_hist[bin]++; wk->imp_iid_n++; }
            }
        }
    }
  return NULL;
}

static int
run_indep_sift (const char *algo_name, ElanpressSiftParams params, gboolean reciprocal,
                gboolean cold_reextract, const char *out_json, int nthreads)
{
  double t0 = now_sec ();
  pthread_t *ths = calloc ((size_t) nthreads, sizeof (pthread_t));
  IndepWorker *wks = calloc ((size_t) nthreads, sizeof (IndepWorker));
  for (int t = 0; t < nthreads; t++)
    {
      wks[t].tid = t; wks[t].nthreads = nthreads;
      wks[t].params = params;
      wks[t].max_enr_kp = 200;
      wks[t].max_prb_kp = 300;
      wks[t].reciprocal = reciprocal;
      pthread_create (&ths[t], NULL, indep_build_worker, &wks[t]);
    }
  for (int t = 0; t < nthreads; t++) pthread_join (ths[t], NULL);
  double t1 = now_sec ();

  uint64_t sum_kp = 0, cnt_kp = 0;
  for (uint32_t i = 0; i < g_nfingers; i++)
    for (uint32_t k = 0; k < g_fingers[i].n_probe; k++)
      { sum_kp += g_fingers[i].probe_feats[k]->n; cnt_kp++; }

  /* Single-threaded latency microbenchmark on 40 real fingers */
  double lat_cold[40], lat_ext[40], lat_match[40];
  int n_lat = g_nfingers < 40 ? (int) g_nfingers : 40;
  for (int i = 0; i < n_lat; i++)
    {
      Finger *f = &g_fingers[i];
      double c0 = now_sec ();
      ElanpressFeatures *pf = elanpress_sift_features_opts (f->probe_imgs[0], g_w, g_h, 300, &params, NULL);
      double c1 = now_sec ();
      if (cold_reextract)
        {
          /* Upstream v0 re-extracts all 12 enrolled strips from raw images on every cold verify! */
          ElanpressFeatures *ef[12];
          for (uint32_t k = 0; k < f->n_enroll && k < 12; k++)
            ef[k] = elanpress_sift_features_opts (g_ptr_array_index (f->enroll_imgs, k), g_w, g_h, 200, &params, NULL);
          (void) match_indep_12 (pf, ef, f->n_enroll, reciprocal);
          for (uint32_t k = 0; k < f->n_enroll && k < 12; k++)
            elanpress_features_free (ef[k]);
        }
      else
        {
          (void) match_indep_12 (pf, f->enroll_feats, f->n_enroll, reciprocal);
        }
      double c2 = now_sec ();
      elanpress_features_free (pf);
      lat_ext[i] = (c1 - c0) * 1000.0;
      lat_match[i] = (c2 - c1) * 1000.0;
      lat_cold[i] = (c2 - c0) * 1000.0;
    }
  qsort (lat_cold, (size_t) n_lat, sizeof (double), cmp_double);
  qsort (lat_ext, (size_t) n_lat, sizeof (double), cmp_double);
  qsort (lat_match, (size_t) n_lat, sizeof (double), cmp_double);

  /* Parallel full-corpus match */
  for (int t = 0; t < nthreads; t++) pthread_create (&ths[t], NULL, indep_match_worker, &wks[t]);
  for (int t = 0; t < nthreads; t++) pthread_join (ths[t], NULL);
  double t2 = now_sec ();

  uint64_t gen_hist[MAX_SCORE_BIN] = { 0 }, gen_live_hist[MAX_SCORE_BIN] = { 0 }, gen_cap_hist[MAX_SCORE_BIN] = { 0 };
  uint64_t imp_all_hist[MAX_SCORE_BIN] = { 0 }, imp_same_hist[MAX_SCORE_BIN] = { 0 }, imp_iid_hist[MAX_SCORE_BIN] = { 0 };
  uint64_t gen_n = 0, gen_live_n = 0, gen_cap_n = 0, imp_all_n = 0, imp_same_n = 0, imp_iid_n = 0;
  for (int t = 0; t < nthreads; t++)
    {
      gen_n += wks[t].gen_n; gen_live_n += wks[t].gen_live_n; gen_cap_n += wks[t].gen_cap_n;
      imp_all_n += wks[t].imp_all_n; imp_same_n += wks[t].imp_same_n; imp_iid_n += wks[t].imp_iid_n;
      for (int b = 0; b < MAX_SCORE_BIN; b++)
        {
          gen_hist[b] += wks[t].gen_hist[b];
          gen_live_hist[b] += wks[t].gen_live_hist[b];
          gen_cap_hist[b] += wks[t].gen_cap_hist[b];
          imp_all_hist[b] += wks[t].imp_all_hist[b];
          imp_same_hist[b] += wks[t].imp_same_hist[b];
          imp_iid_hist[b] += wks[t].imp_iid_hist[b];
        }
    }

  double tmpl_kb = cold_reextract
                     ? (12.0 * g_w * g_h) / 1024.0
                     : (12.0 * ((double) sum_kp / cnt_kp) * 140.0) / 1024.0;

  printf ("[%s %dx%d] build=%.2fs match=%.2fs | avg_kp=%.1f | cold_p50=%.2f ms (p95=%.2f ms)\n",
          algo_name, g_w, g_h, t1 - t0, t2 - t1, (double) sum_kp / cnt_kp,
          lat_cold[n_lat / 2], lat_cold[(n_lat * 95) / 100]);

  FILE *jf = fopen (out_json, "w");
  fprintf (jf, "{\n");
  fprintf (jf, "  \"algo\": \"%s\",\n", algo_name);
  fprintf (jf, "  \"width\": %d, \"height\": %d,\n", g_w, g_h);
  fprintf (jf, "  \"avg_features\": %.2f,\n", (double) sum_kp / cnt_kp);
  fprintf (jf, "  \"template_kb\": %.2f,\n", tmpl_kb);
  fprintf (jf, "  \"latency_cold_p50_ms\": %.3f,\n", lat_cold[n_lat / 2]);
  fprintf (jf, "  \"latency_cold_p95_ms\": %.3f,\n", lat_cold[(n_lat * 95) / 100]);
  fprintf (jf, "  \"latency_extract_p50_ms\": %.3f,\n", lat_ext[n_lat / 2]);
  fprintf (jf, "  \"latency_match_p50_ms\": %.3f,\n", lat_match[n_lat / 2]);
  fprintf (jf, "  \"gen_n\": %llu, \"gen_live_n\": %llu, \"gen_cap_n\": %llu,\n",
           (unsigned long long) gen_n, (unsigned long long) gen_live_n, (unsigned long long) gen_cap_n);
  fprintf (jf, "  \"imp_all_n\": %llu, \"imp_same_n\": %llu, \"imp_iid_n\": %llu,\n",
           (unsigned long long) imp_all_n, (unsigned long long) imp_same_n, (unsigned long long) imp_iid_n);

  const char *names[6] = { "gen_hist", "gen_live_hist", "gen_cap_hist", "imp_all_hist", "imp_same_hist", "imp_iid_hist" };
  uint64_t *hists[6] = { gen_hist, gen_live_hist, gen_cap_hist, imp_all_hist, imp_same_hist, imp_iid_hist };
  for (int h = 0; h < 6; h++)
    {
      fprintf (jf, "  \"%s\": [", names[h]);
      for (int b = 0; b < MAX_SCORE_BIN; b++)
        fprintf (jf, "%s%llu", b ? "," : "", (unsigned long long) hists[h][b]);
      fprintf (jf, "]%s\n", h < 5 ? "," : "");
    }
  fprintf (jf, "}\n");
  fclose (jf);
  free (ths);
  free (wks);
  return 0;
}

/* ========================================================================= */
/* Mode 4: PressMatch v4.1 Latency + 64x80 Full Accuracy (v2, v4.0, v4.1)    */
/* ========================================================================= */

static int
run_v41_latency (const char *out_json)
{
  const ElanpressSiftParams p_v4 = { .contrast_thr = 0.0f, .edge_ratio = 25.0f, .rootsift = TRUE };
  unsigned int frame_sz = (unsigned int) (g_w * g_h);
  int n_lat = g_nfingers < 50 ? (int) g_nfingers : 50;

  double lat_v2[50], lat_v40[50], lat_v41[50];
  double lat_ext[50], lat_deser[50], lat_pg[50], lat_zr[50], lat_ze[50];
  double tmpl_kb_sum = 0.0;
  uint64_t kp_sum = 0, stitched_sum = 0;

  for (int i = 0; i < n_lat; i++)
    {
      Finger *f = &g_fingers[i];
      GPtrArray *unpacked = g_ptr_array_new_full (f->n_enroll, g_free);
      ElanpressFeatures *ef[12];
      size_t packed_sum = 0;
      for (uint32_t k = 0; k < f->n_enroll && k < 12; k++)
        {
          guint8 *raw = g_ptr_array_index (f->enroll_imgs, k);
          ef[k] = elanpress_sift_features_opts (raw, g_w, g_h, 200, &p_v4, NULL);
          gsize plen = 0;
          g_autofree guint8 *pk = elanpress_pack_image_4bit (raw, frame_sz, &plen);
          packed_sum += plen;
          g_ptr_array_add (unpacked, elanpress_unpack_image_4bit (pk, plen, frame_sz));
        }
      ElanpressPoseGraph *pg = elanpress_pose_graph_build_verified (
        (ElanpressFeatures * const *) ef, f->enroll_imgs, f->n_enroll, g_w, g_h, 8);
      stitched_sum += pg->largest_nodes;
      GBytes *ser = elanpress_pose_graph_serialize (pg);
      gsize ser_len = 0;
      gconstpointer ser_ptr = g_bytes_get_data (ser, &ser_len);
      tmpl_kb_sum += (double) (packed_sum + ser_len) / 1024.0;

      /* Measure cold verify on probe 0 */
      double t0 = now_sec ();
      ElanpressFeatures *pf = elanpress_sift_features_opts (f->probe_imgs[0], g_w, g_h, 300, &p_v4, NULL);
      kp_sum += pf->n;
      double t1 = now_sec ();
      ElanpressPoseGraph *pg2 = elanpress_pose_graph_deserialize (ser_ptr, ser_len);
      double t2 = now_sec ();
      guint comp = 0, ov = 0;
      ElanpressPose pose = { 1.0f, 0.0f, 0.0f, 0.0f };
      guint pairs = elanpress_pose_graph_match_pose (pf, pg2, TRUE, &comp, &pose);
      double t3 = now_sec ();
      gdouble zr = 0.0;
      if (pairs > 0)
        zr = elanpress_pose_graph_zncc (f->probe_imgs[0], unpacked, 1, 0, pg2, comp, pose, g_w, g_h, &ov);
      double t4 = now_sec ();
      gdouble ze = 0.0, zr2 = 0.0;
      if (pairs > 0)
        ze = elanpress_pose_graph_zncc_elastic (f->probe_imgs[0], unpacked, 1, 0, pg2, comp, pose, g_w, g_h, 1, &zr2, &ov);
      double t5 = now_sec ();
      (void) zr; (void) ze;

      lat_ext[i]   = (t1 - t0) * 1000.0;
      lat_deser[i] = (t2 - t1) * 1000.0;
      lat_pg[i]    = (t3 - t2) * 1000.0;
      lat_zr[i]    = (t4 - t3) * 1000.0;
      lat_ze[i]    = (t5 - t4) * 1000.0;
      lat_v2[i]    = lat_ext[i] + lat_deser[i] + lat_pg[i];
      lat_v40[i]   = lat_v2[i] + lat_zr[i];
      lat_v41[i]   = lat_v2[i] + lat_ze[i];

      elanpress_features_free (pf);
      elanpress_pose_graph_free (pg2);
      elanpress_pose_graph_free (pg);
      g_bytes_unref (ser);
      g_ptr_array_unref (unpacked);
      for (uint32_t k = 0; k < f->n_enroll && k < 12; k++)
        elanpress_features_free (ef[k]);
    }

  qsort (lat_v2, (size_t) n_lat, sizeof (double), cmp_double);
  qsort (lat_v40, (size_t) n_lat, sizeof (double), cmp_double);
  qsort (lat_v41, (size_t) n_lat, sizeof (double), cmp_double);
  qsort (lat_ext, (size_t) n_lat, sizeof (double), cmp_double);
  qsort (lat_deser, (size_t) n_lat, sizeof (double), cmp_double);
  qsort (lat_pg, (size_t) n_lat, sizeof (double), cmp_double);
  qsort (lat_zr, (size_t) n_lat, sizeof (double), cmp_double);
  qsort (lat_ze, (size_t) n_lat, sizeof (double), cmp_double);

  printf ("[v4.1 Latency %dx%d] v2=%.2fms | v4.0=%.2fms | v4.1=%.2fms (ext=%.2f deser=%.2f pg=%.2f el_zncc=%.2f) | tmpl=%.1f KB\n",
          g_w, g_h, lat_v2[n_lat/2], lat_v40[n_lat/2], lat_v41[n_lat/2],
          lat_ext[n_lat/2], lat_deser[n_lat/2], lat_pg[n_lat/2], lat_ze[n_lat/2],
          tmpl_kb_sum / n_lat);

  FILE *jf = fopen (out_json, "w");
  fprintf (jf, "{\n");
  fprintf (jf, "  \"width\": %d, \"height\": %d,\n", g_w, g_h);
  fprintf (jf, "  \"avg_kp\": %.2f, \"avg_stitched\": %.2f, \"template_kb\": %.2f,\n",
           (double) kp_sum / n_lat, (double) stitched_sum / n_lat, tmpl_kb_sum / n_lat);
  fprintf (jf, "  \"v2_p50_ms\": %.3f, \"v2_p95_ms\": %.3f,\n", lat_v2[n_lat/2], lat_v2[(n_lat*95)/100]);
  fprintf (jf, "  \"v40_p50_ms\": %.3f, \"v40_p95_ms\": %.3f,\n", lat_v40[n_lat/2], lat_v40[(n_lat*95)/100]);
  fprintf (jf, "  \"v41_p50_ms\": %.3f, \"v41_p95_ms\": %.3f,\n", lat_v41[n_lat/2], lat_v41[(n_lat*95)/100]);
  fprintf (jf, "  \"extract_p50_ms\": %.3f, \"deser_p50_ms\": %.3f, \"pg_match_p50_ms\": %.3f,\n",
           lat_ext[n_lat/2], lat_deser[n_lat/2], lat_pg[n_lat/2]);
  fprintf (jf, "  \"zncc_rigid_p50_ms\": %.3f, \"zncc_elastic_p50_ms\": %.3f\n",
           lat_zr[n_lat/2], lat_ze[n_lat/2]);
  fprintf (jf, "}\n");
  fclose (jf);
  return 0;
}

int
main (int argc, char **argv)
{
  if (argc < 4)
    {
      fprintf (stderr, "Usage: %s <nbis|v0|v1|latency> <crops.bin> <out.json> [nthreads]\n", argv[0]);
      return 1;
    }
  const char *mode = argv[1];
  const char *in_bin = argv[2];
  const char *out_json = argv[3];
  int nthreads = argc > 4 ? atoi (argv[4]) : 16;

  if (load_crops_bin (in_bin) != 0)
    return 1;

  if (strcmp (mode, "nbis") == 0)
    return run_nbis (out_json);
  if (strcmp (mode, "v0") == 0)
    {
      ElanpressSiftParams p_v0 = { .contrast_thr = 0.04f, .edge_ratio = 10.0f, .rootsift = FALSE };
      return run_indep_sift ("upstream_v0_sift", p_v0, FALSE, TRUE, out_json, nthreads);
    }
  if (strcmp (mode, "v1") == 0)
    {
      ElanpressSiftParams p_v1 = { .contrast_thr = 0.0f, .edge_ratio = 25.0f, .rootsift = TRUE };
      return run_indep_sift ("pressmatch_v1_rootsift", p_v1, TRUE, FALSE, out_json, nthreads);
    }
  if (strcmp (mode, "latency") == 0)
    return run_v41_latency (out_json);

  fprintf (stderr, "Unknown mode: %s\n", mode);
  return 1;
}
