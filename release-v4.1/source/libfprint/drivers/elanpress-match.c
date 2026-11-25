/*
 * Image processing and keypoint matching for ELAN press-type sensors
 * Copyright (C) 2026 Filip Spanne
 *
 * The imaged area of these sensors is far too small for reliable minutiae
 * matching, so prints store the enrolled images themselves and a press is
 * matched against them by pairing up local keypoint features (see
 * elanpress-sift.c), like the Windows driver does.
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

#include "elanpress-match.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int
elanpress_cmp_short (const void *a, const void *b)
{
  return (int) (*(unsigned short *) a - *(unsigned short *) b);
}

/* the wire format is column-major with frame-height-tall columns; rotate
 * to row-major (same layout as the elan driver uses for these sensors) */
void
elanpress_rotate_frame (const guint8 *raw, unsigned short *out, int w, int h)
{
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      out[y * w + x] = ((const guint16 *) raw)[y + x * h];
}

/* Elantech's recommended 2-step non-linear normalization of the 14-bit
 * background-subtracted frame onto 8 bits (see the elan driver) */
static void
elanpress_normalize (const unsigned short *in, guint8 *out, unsigned int size)
{
  g_autofree unsigned short *sorted = g_malloc (size * sizeof (short));
  unsigned short lvl0, lvl1, lvl2, lvl3;
  unsigned short px;

  memcpy (sorted, in, size * sizeof (short));
  qsort (sorted, size, sizeof (short), elanpress_cmp_short);

  lvl0 = sorted[0];
  lvl1 = sorted[size * 3 / 10];
  lvl2 = sorted[size * 65 / 100];
  lvl3 = sorted[size - 1];

  for (unsigned int i = 0; i < size; i++)
    {
      px = in[i];
      if (px < lvl1)
        px = (px - lvl0) * 99 / MAX (lvl1 - lvl0, 1);
      else if (px < lvl2)
        px = 99 + ((px - lvl1) * 56 / MAX (lvl2 - lvl1, 1));
      else
        px = 155 + MIN ((px - lvl2) * 100 / MAX (lvl3 - lvl2, 1), 100);
      out[i] = (guint8) px;
    }
}

/* average the stable frames of a touch (newest-first list), subtract the
 * background and normalize; returns NULL if the finger wasn't properly on
 * the sensor */
guint8 *
elanpress_process_frames (GSList *frames, int num_frames,
                          const unsigned short *background,
                          unsigned int size)
{
  g_autofree unsigned int *sum = g_malloc0 (size * sizeof (unsigned int));
  g_autofree unsigned short *avg = g_malloc (size * sizeof (unsigned short));
  guint8 *out;
  int skip_oldest = 0, skip_newest = 0, used = 0, i;
  unsigned long total = 0;
  GSList *l;

  if (num_frames >= 6)
    {
      skip_oldest = ELANPRESS_SKIP_OLDEST;
      skip_newest = ELANPRESS_SKIP_NEWEST;
    }
  else if (num_frames >= 4)
    {
      skip_oldest = 1;
      skip_newest = 1;
    }

  for (l = frames, i = 0; l; l = l->next, i++)
    {
      const unsigned short *frame = l->data;

      if (i < skip_newest || i >= num_frames - skip_oldest)
        continue;
      for (unsigned int j = 0; j < size; j++)
        sum[j] += frame[j];
      used++;
    }

  if (used == 0)
    return NULL;

  for (unsigned int j = 0; j < size; j++)
    {
      unsigned int px = sum[j] / used;
      unsigned short bg = background ? background[j] : 0;

      avg[j] = px > bg ? px - bg : 0;
      total += avg[j];
    }

  if (total == 0)
    {
      g_debug ("touch darker than background, ignoring");
      return NULL;
    }

  out = g_malloc (size);
  elanpress_normalize (avg, out, size);
  return out;
}

/* Variant: normalize using percentiles of covered pixels only (diff >=
 * ELANPRESS_TOUCH_PIXEL_DELTA) with 1%/99% clipping so partial touches do not
 * let uncovered background pixels hijack lvl0/lvl1 and crush ridge contrast. */
guint8 *
elanpress_process_frame_covered (const unsigned short *raw_frame,
                                 const unsigned short *background,
                                 unsigned int          size)
{
  g_autofree unsigned short *avg = NULL;
  g_autofree unsigned short *covered = NULL;
  guint8 *out;
  unsigned int n_cov = 0;
  unsigned long total = 0;
  unsigned short lvl0, lvl1, lvl2, lvl3;

  if (!raw_frame || size == 0)
    return NULL;

  avg = g_malloc (size * sizeof (unsigned short));
  covered = g_malloc (size * sizeof (unsigned short));

  for (unsigned int j = 0; j < size; j++)
    {
      unsigned short bg = background ? background[j] : 0;
      unsigned short d = raw_frame[j] > bg ? raw_frame[j] - bg : 0;

      avg[j] = d;
      total += d;
      if (d >= ELANPRESS_TOUCH_PIXEL_DELTA)
        covered[n_cov++] = d;
    }

  if (total == 0)
    return NULL;

  out = g_malloc (size);
  if (n_cov < (unsigned int) (size * ELANPRESS_TOUCH_MIN_COVERAGE) || n_cov < 16)
    {
      elanpress_normalize (avg, out, size);
      return out;
    }

  qsort (covered, n_cov, sizeof (unsigned short), elanpress_cmp_short);
  lvl0 = covered[n_cov / 100];
  lvl1 = covered[n_cov * 3 / 10];
  lvl2 = covered[n_cov * 65 / 100];
  lvl3 = covered[n_cov * 99 / 100];

  for (unsigned int i = 0; i < size; i++)
    {
      unsigned short px = avg[i];
      unsigned int v;

      if (px <= lvl0)
        v = 0;
      else if (px < lvl1)
        v = (unsigned int) (px - lvl0) * 99 / MAX (lvl1 - lvl0, 1);
      else if (px < lvl2)
        v = 99 + ((unsigned int) (px - lvl1) * 56 / MAX (lvl2 - lvl1, 1));
      else if (px >= lvl3)
        v = 255;
      else
        v = 155 + MIN ((unsigned int) (px - lvl2) * 100 / MAX (lvl3 - lvl2, 1), 100);
      out[i] = (guint8) v;
    }

  return out;
}

/* standard deviation of the pixel values of a normalized touch image, as a
 * cheap proxy for how much ridge contrast it actually holds. Flat captures
 * correlate poorly against everything, genuine or not, so it is better to
 * reject them at capture time than to store one or match against it. */
gdouble
elanpress_image_quality (const guint8 *img, unsigned int size)
{
  gdouble sum = 0, sum_sq = 0, mean;

  if (!img || size == 0)
    return 0.0;

  for (unsigned int i = 0; i < size; i++)
    sum += img[i];
  mean = sum / size;

  for (unsigned int i = 0; i < size; i++)
    {
      gdouble d = img[i] - mean;

      sum_sq += d * d;
    }

  return sqrt (sum_sq / size);
}

/* counts how many pixels sit clearly above the background frame, to infer a
 * touch without asking cmd_pre_scan (see the capture state machine) */
gboolean
elanpress_frame_has_touch (const unsigned short *frame,
                           const unsigned short *background,
                           unsigned int size,
                           ElanpressTouchStats *stats)
{
  gint64 sum = 0;
  unsigned int covered = 0;

  if (stats)
    {
      stats->mean_delta = 0;
      stats->coverage = 0;
    }

  /* without a reference frame presence cannot be told apart from an evenly
   * lit sensor, so report no touch rather than guess */
  if (!frame || !background || size == 0)
    return FALSE;

  for (unsigned int i = 0; i < size; i++)
    {
      int d = (int) frame[i] - (int) background[i];

      if (d <= 0)
        continue;

      sum += d;
      if (d >= ELANPRESS_TOUCH_PIXEL_DELTA)
        covered++;
    }

  if (stats)
    {
      stats->mean_delta = (gdouble) sum / size;
      stats->coverage = (gdouble) covered / size;
    }

  return covered >= size * ELANPRESS_TOUCH_MIN_COVERAGE;
}

ElanpressFeatures *
elanpress_image_features (const guint8 *img, int w, int h)
{
  return elanpress_sift_features (img, w, h, ELANPRESS_MAX_KEYPOINTS);
}

static inline gint
elanpress_desc_norm2 (const guint8 *a)
{
  gint sum = 0;

  for (int k = 0; k < ELANPRESS_SIFT_DESC_LEN; k++)
    sum += (gint) a[k] * (gint) a[k];
  return sum;
}

static inline gint
elanpress_desc_dot (const guint8 * restrict a, const guint8 * restrict b)
{
  gint sum = 0;

  for (int k = 0; k < ELANPRESS_SIFT_DESC_LEN; k++)
    sum += (gint) a[k] * (gint) b[k];
  return sum;
}

static inline gint
elanpress_desc_dist2 (const guint8 * restrict a, const guint8 * restrict b)
{
  gint sum = 0;

  for (int k = 0; k < ELANPRESS_SIFT_DESC_LEN; k++)
    {
      gint d = (gint) a[k] - (gint) b[k];
      sum += d * d;
    }
  return sum;
}

typedef struct
{
  gfloat px, py;        /* keypoint on the probe */
  gfloat ex, ey;        /* its partner on the enrolled image */
  gfloat rot;           /* rotation the pair's orientations imply */
  guint  ei;            /* partner index, to count distinct partners */
} ElanpressPair;

typedef struct
{
  gfloat c, s, tx, ty;
} ElanpressPlacement;

/* the placement a single pair implies: its rotation, and the translation
 * that then lands the probe keypoint exactly on its partner */
static void
elanpress_pair_placement (const ElanpressPair *p, ElanpressPlacement *m)
{
  m->c = cosf (p->rot);
  m->s = sinf (p->rot);
  m->tx = p->ex - (m->c * p->px - m->s * p->py);
  m->ty = p->ey - (m->s * p->px + m->c * p->py);
}

static gboolean
elanpress_pair_fits (const ElanpressPair *p, const ElanpressPlacement *m, gfloat tol2)
{
  gfloat dx = m->c * p->px - m->s * p->py + m->tx - p->ex;
  gfloat dy = m->s * p->px + m->c * p->py + m->ty - p->ey;

  return dx * dx + dy * dy < tol2;
}

/* least-squares rotation and translation taking the fitting pairs' probe
 * keypoints onto their partners */
static gboolean
elanpress_refit (const ElanpressPair *pairs, const gboolean *fit, guint n,
                 ElanpressPlacement *m)
{
  gdouble mpx = 0, mpy = 0, mex = 0, mey = 0, sxx = 0, sxy = 0, a;
  guint cnt = 0;

  for (guint i = 0; i < n; i++)
    if (fit[i])
      {
        mpx += pairs[i].px;
        mpy += pairs[i].py;
        mex += pairs[i].ex;
        mey += pairs[i].ey;
        cnt++;
      }
  if (cnt < 2)
    return FALSE;
  mpx /= cnt;
  mpy /= cnt;
  mex /= cnt;
  mey /= cnt;

  for (guint i = 0; i < n; i++)
    if (fit[i])
      {
        gdouble px = pairs[i].px - mpx, py = pairs[i].py - mpy;
        gdouble ex = pairs[i].ex - mex, ey = pairs[i].ey - mey;

        sxx += px * ex + py * ey;
        sxy += px * ey - py * ex;
      }

  a = atan2 (sxy, sxx);
  m->c = cos (a);
  m->s = sin (a);
  m->tx = mex - (m->c * mpx - m->s * mpy);
  m->ty = mey - (m->s * mpx + m->c * mpy);
  return TRUE;
}

/* fitting pairs, counted once per distinct keypoint on either side: a
 * keypoint with two dominant directions yields two keypoints at one spot,
 * and two probe keypoints can share a partner */
static guint
elanpress_count_distinct (const ElanpressPair *pairs, const gboolean *fit, guint n)
{
  guint probe = 0, enrolled = 0;

  for (guint i = 0; i < n; i++)
    {
      gboolean seen_p = FALSE, seen_e = FALSE;

      if (!fit[i])
        continue;
      for (guint j = 0; j < i; j++)
        {
          if (!fit[j])
            continue;
          if (pairs[j].px == pairs[i].px && pairs[j].py == pairs[i].py)
            seen_p = TRUE;
          if (pairs[j].ex == pairs[i].ex && pairs[j].ey == pairs[i].ey)
            seen_e = TRUE;
        }
      probe += !seen_p;
      enrolled += !seen_e;
    }
  return MIN (probe, enrolled);
}

static guint
elanpress_score_pairs_pose (const ElanpressPair *pairs, guint n,
                            guint *out_best_loose, ElanpressPose *out_pose)
{
  const gfloat loose2 = ELANPRESS_MATCH_LOOSE_PX * ELANPRESS_MATCH_LOOSE_PX;
  const gfloat tol2 = ELANPRESS_MATCH_TOL_PX * ELANPRESS_MATCH_TOL_PX;
  g_autofree guint *support = NULL;
  g_autofree gboolean *fit = NULL, *next = NULL;
  guint best = 0, best_loose = 0;

  if (out_best_loose)
    *out_best_loose = 0;
  if (out_pose)
    {
      out_pose->c = 1.0f;
      out_pose->s = 0.0f;
      out_pose->tx = 0.0f;
      out_pose->ty = 0.0f;
    }
  if (n < 3)
    return 0;

  support = g_new0 (guint, n);
  fit = g_new (gboolean, n);
  next = g_new (gboolean, n);

  /* support for the placement each pair proposes */
  for (guint k = 0; k < n; k++)
    {
      ElanpressPlacement m;

      elanpress_pair_placement (&pairs[k], &m);
      for (guint i = 0; i < n; i++)
        support[k] += elanpress_pair_fits (&pairs[i], &m, loose2);
      best_loose = MAX (best_loose, support[k]);
    }
  if (out_best_loose)
    *out_best_loose = best_loose;

  /* refine the best supported proposals; a genuine placement is proposed by
   * every one of its pairs, so it is among the first few */
  for (int round = 0; round < ELANPRESS_MATCH_REFINED; round++)
    {
      ElanpressPlacement m;
      guint top = 0, cnt;

      for (guint k = 1; k < n; k++)
        if (support[k] > support[top])
          top = k;
      if (support[top] < 3)
        break;
      support[top] = 0;

      elanpress_pair_placement (&pairs[top], &m);
      for (guint i = 0; i < n; i++)
        fit[i] = elanpress_pair_fits (&pairs[i], &m, loose2);

      for (int step = 0; step < 3; step++)
        {
          gboolean same = TRUE;
          guint was = 0, now = 0;

          if (!elanpress_refit (pairs, fit, n, &m))
            break;
          for (guint i = 0; i < n; i++)
            {
              next[i] = elanpress_pair_fits (&pairs[i], &m, tol2);
              same &= next[i] == fit[i];
              was += fit[i];
              now += next[i];
            }
          memcpy (fit, next, n * sizeof (gboolean));
          if (same || now < was)
            break;
        }

      cnt = elanpress_count_distinct (pairs, fit, n);
      if (cnt > best)
        {
          best = cnt;
          if (out_pose)
            {
              out_pose->c = m.c;
              out_pose->s = m.s;
              out_pose->tx = m.tx;
              out_pose->ty = m.ty;
            }
        }
    }

  return best;
}

static guint
elanpress_score_pairs (const ElanpressPair *pairs, guint n, guint *out_best_loose)
{
  return elanpress_score_pairs_pose (pairs, n, out_best_loose, NULL);
}

guint
elanpress_match_features_pose (const ElanpressFeatures *probe,
                               const ElanpressFeatures *enrolled,
                               ElanpressPose           *best_pose,
                               gboolean                 scale_gate)
{
  const gdouble ratio2 = ELANPRESS_MATCH_RATIO * ELANPRESS_MATCH_RATIO;
  g_autofree ElanpressPair *pairs = NULL;
  guint n = 0;

  if (best_pose)
    {
      best_pose->c = 1.0f;
      best_pose->s = 0.0f;
      best_pose->tx = 0.0f;
      best_pose->ty = 0.0f;
    }

  if (!probe || !enrolled || probe->n < 2 || enrolled->n < 2)
    return 0;

  pairs = g_new (ElanpressPair, probe->n);
  for (guint i = 0; i < probe->n; i++)
    {
      const ElanpressKeypoint *kp = &probe->kp[i];
      gint d1 = G_MAXINT, d2 = G_MAXINT;
      guint b1 = 0;

      for (guint j = 0; j < enrolled->n; j++)
        {
          if (scale_gate &&
              (kp->scale < 0.75f * enrolled->kp[j].scale ||
               kp->scale > 1.333333f * enrolled->kp[j].scale))
            continue;

          gint d = elanpress_desc_dist2 (kp->desc, enrolled->kp[j].desc);

          if (d < d1)
            {
              d2 = d1;
              d1 = d;
              b1 = j;
            }
          else if (d < d2)
            {
              d2 = d;
            }
        }
      if (d2 == G_MAXINT || d1 >= ratio2 * d2)
        continue;

      pairs[n].px = kp->x;
      pairs[n].py = kp->y;
      pairs[n].ex = enrolled->kp[b1].x;
      pairs[n].ey = enrolled->kp[b1].y;
      pairs[n].rot = enrolled->kp[b1].angle - kp->angle;
      pairs[n].ei = b1;
      n++;
    }

  return elanpress_score_pairs_pose (pairs, n, NULL, best_pose);
}

guint
elanpress_match_features (const ElanpressFeatures *probe,
                          const ElanpressFeatures *enrolled)
{
  return elanpress_match_features_pose (probe, enrolled, NULL, FALSE);
}

/* Score the same pair of feature sets in memory under the baseline one-way
 * Lowe ratio matcher, plus two reciprocal-matching variants:
 *   - recip_nn: forward ratio 0.8 + reverse nearest-neighbor cross-check
 *   - recip_ratio: forward ratio 0.8 + reverse ratio 0.8 (mutual clear NN) */
guint
elanpress_match_features_diag (const ElanpressFeatures *probe,
                               const ElanpressFeatures *enrolled,
                               ElanpressMatchDiag      *diag)
{
  const gdouble ratio2 = ELANPRESS_MATCH_RATIO * ELANPRESS_MATCH_RATIO;
  ElanpressMatchDiag res = { 0 };
  g_autofree gint *fwd_d1 = NULL, *fwd_d2 = NULL;
  g_autofree guint *fwd_b1 = NULL, *fwd_b2 = NULL;
  g_autofree gint *rev_d1 = NULL, *rev_d2 = NULL;
  g_autofree guint *rev_b1 = NULL;
  g_autofree ElanpressPair *pairs_base = NULL;
  g_autofree ElanpressPair *pairs_rnn = NULL;
  g_autofree ElanpressPair *pairs_rratio = NULL;
  guint n_base = 0, n_rnn = 0, n_rratio = 0;

  if (!probe || !enrolled || probe->n < 2 || enrolled->n < 2)
    {
      if (diag)
        *diag = res;
      return 0;
    }

  fwd_d1 = g_new (gint, probe->n);
  fwd_d2 = g_new (gint, probe->n);
  fwd_b1 = g_new0 (guint, probe->n);
  fwd_b2 = g_new0 (guint, probe->n);
  rev_d1 = g_new (gint, enrolled->n);
  rev_d2 = g_new (gint, enrolled->n);
  rev_b1 = g_new0 (guint, enrolled->n);

  for (guint i = 0; i < probe->n; i++)
    fwd_d1[i] = fwd_d2[i] = G_MAXINT;
  for (guint j = 0; j < enrolled->n; j++)
    rev_d1[j] = rev_d2[j] = G_MAXINT;

  for (guint i = 0; i < probe->n; i++)
    {
      const guint8 *pdesc = probe->kp[i].desc;

      for (guint j = 0; j < enrolled->n; j++)
        {
          gint d = elanpress_desc_dist2 (pdesc, enrolled->kp[j].desc);

          if (d < fwd_d1[i])
            {
              fwd_d2[i] = fwd_d1[i];
              fwd_b2[i] = fwd_b1[i];
              fwd_d1[i] = d;
              fwd_b1[i] = j;
            }
          else if (d < fwd_d2[i])
            {
              fwd_d2[i] = d;
              fwd_b2[i] = j;
            }

          if (d < rev_d1[j])
            {
              rev_d2[j] = rev_d1[j];
              rev_d1[j] = d;
              rev_b1[j] = i;
            }
          else if (d < rev_d2[j])
            {
              rev_d2[j] = d;
            }
        }
    }

  pairs_base = g_new (ElanpressPair, probe->n);
  pairs_rnn = g_new (ElanpressPair, probe->n);
  pairs_rratio = g_new (ElanpressPair, probe->n);

  for (guint i = 0; i < probe->n; i++)
    {
      const ElanpressKeypoint *kp = &probe->kp[i];
      guint b1 = fwd_b1[i];
      guint b2 = fwd_b2[i];
      ElanpressPair pair;

      if (fwd_d2[i] == G_MAXINT || fwd_d1[i] >= ratio2 * fwd_d2[i])
        {
          if (fwd_d2[i] != G_MAXINT &&
              enrolled->kp[b1].x == enrolled->kp[b2].x &&
              enrolled->kp[b1].y == enrolled->kp[b2].y)
            res.n_rej_same_loc++;
          continue;
        }

      pair.px = kp->x;
      pair.py = kp->y;
      pair.ex = enrolled->kp[b1].x;
      pair.ey = enrolled->kp[b1].y;
      pair.rot = enrolled->kp[b1].angle - kp->angle;
      pair.ei = b1;

      pairs_base[n_base++] = pair;
      if (rev_b1[b1] == i)
        {
          pairs_rnn[n_rnn++] = pair;
          if (rev_d2[b1] != G_MAXINT && rev_d1[b1] < ratio2 * rev_d2[b1])
            pairs_rratio[n_rratio++] = pair;
        }
    }

  res.n_pairs_base = n_base;
  res.n_pairs_recip_nn = n_rnn;
  res.n_pairs_recip_ratio = n_rratio;
  res.score_base = elanpress_score_pairs (pairs_base, n_base, &res.loose_base);
  res.score_recip_nn = elanpress_score_pairs (pairs_rnn, n_rnn, NULL);
  res.score_recip_ratio = elanpress_score_pairs (pairs_rratio, n_rratio, NULL);

  if (diag)
    *diag = res;
  return res.score_base;
}

void
elanpress_frame_diag (const unsigned short *raw_frame,
                      const unsigned short *background,
                      const guint8         *norm_img,
                      unsigned int          size,
                      ElanpressFrameDiag   *out)
{
  if (!out)
    return;
  memset (out, 0, sizeof (*out));
  if (size == 0)
    return;

  if (raw_frame && background)
    {
      ElanpressTouchStats ts = { 0, 0 };
      g_autofree unsigned short *diff = g_new (unsigned short, size);
      guint64 bg_sum = 0;
      guint pos_cnt = 0;

      elanpress_frame_has_touch (raw_frame, background, size, &ts);
      out->coverage = ts.coverage * 100.0;
      out->mean_delta = ts.mean_delta;

      for (unsigned int i = 0; i < size; i++)
        {
          bg_sum += background[i];
          if (raw_frame[i] > background[i])
            {
              diff[i] = raw_frame[i] - background[i];
              pos_cnt++;
            }
          else
            {
              diff[i] = 0;
            }
        }
      qsort (diff, size, sizeof (unsigned short), elanpress_cmp_short);
      out->bg_mean = (guint16) (bg_sum / size);
      out->pos_pct = 100.0 * (gdouble) pos_cnt / (gdouble) size;
      out->p0 = diff[0];
      out->p30 = diff[size * 3 / 10];
      out->p65 = diff[size * 65 / 100];
      out->p99 = diff[size * 99 / 100];
      out->p100 = diff[size - 1];
    }

  if (norm_img)
    {
      guint64 sum = 0;
      guint zeros = 0;

      for (unsigned int i = 0; i < size; i++)
        {
          sum += norm_img[i];
          if (norm_img[i] == 0)
            zeros++;
        }
      out->norm_mean = (gdouble) sum / (gdouble) size;
      out->norm_stddev = elanpress_image_quality (norm_img, size);
      out->zero_pct = 100.0 * (gdouble) zeros / (gdouble) size;
    }
}

static ElanpressPose
elanpress_pose_invert (ElanpressPose p)
{
  ElanpressPose inv;
  inv.c = p.c;
  inv.s = -p.s;
  inv.tx = -(p.c * p.tx + p.s * p.ty);
  inv.ty = p.s * p.tx - p.c * p.ty;
  return inv;
}

static ElanpressPose
elanpress_pose_compose (ElanpressPose a, ElanpressPose b)
{
  ElanpressPose out;
  gfloat c = a.c * b.c - a.s * b.s;
  gfloat s = a.s * b.c + a.c * b.s;
  gfloat len = hypotf (c, s);
  if (len > 1e-6f)
    {
      c /= len;
      s /= len;
    }
  out.c = c;
  out.s = s;
  out.tx = a.c * b.tx - a.s * b.ty + a.tx;
  out.ty = a.s * b.tx + a.c * b.ty + a.ty;
  return out;
}

static gfloat
elanpress_angle_diff (gfloat a, gfloat b)
{
  gfloat d = fmodf (a - b + (gfloat) G_PI, (gfloat) (2.0 * G_PI));
  if (d < 0.0f)
    d += (gfloat) (2.0 * G_PI);
  return fabsf (d - (gfloat) G_PI);
}

ElanpressMosaic *
elanpress_mosaic_build (ElanpressFeatures * const *feats,
                        guint                      n_images,
                        int                        frame_w,
                        int                        frame_h,
                        guint                      min_stitch_pairs)
{
  ElanpressMosaic *mosaic = g_new0 (ElanpressMosaic, 1);
  g_autofree guint *scores = NULL;
  g_autofree ElanpressPose *poses = NULL;
  g_autofree gboolean *visited = NULL;
  g_autofree ElanpressPose *global_pose = NULL;
  GPtrArray *comps = g_ptr_array_new ();

  if (!feats || n_images == 0)
    {
      g_ptr_array_free (comps, TRUE);
      return mosaic;
    }

  scores = g_new0 (guint, n_images * n_images);
  poses = g_new0 (ElanpressPose, n_images * n_images);
  visited = g_new0 (gboolean, n_images);
  global_pose = g_new0 (ElanpressPose, n_images);

  for (guint i = 0; i < n_images; i++)
    for (guint j = 0; j < n_images; j++)
      {
        if (i == j || !feats[i] || !feats[j])
          continue;
        scores[i * n_images + j] =
          elanpress_match_features_pose (feats[i], feats[j],
                                         &poses[i * n_images + j], FALSE);
      }

  for (;;)
    {
      guint root = n_images;
      guint best_deg_score = 0;
      g_autofree guint *members = g_new0 (guint, n_images);
      guint n_members = 0;

      for (guint i = 0; i < n_images; i++)
        {
          if (visited[i] || !feats[i])
            continue;
          guint deg = 1;
          for (guint j = 0; j < n_images; j++)
            {
              if (i == j || visited[j])
                continue;
              guint w = MAX (scores[i * n_images + j], scores[j * n_images + i]);
              if (w >= min_stitch_pairs)
                deg += w;
            }
          if (deg > best_deg_score)
            {
              best_deg_score = deg;
              root = i;
            }
        }
      if (root == n_images)
        break;

      visited[root] = TRUE;
      global_pose[root] = (ElanpressPose) { 1.0f, 0.0f, 0.0f, 0.0f };
      members[n_members++] = root;

      /* Grow component via Maximum Spanning Tree edges >= min_stitch_pairs */
      for (;;)
        {
          guint best_u = n_images, best_v = n_images, best_w = 0;
          gboolean use_inv = FALSE;

          for (guint u = 0; u < n_images; u++)
            {
              if (visited[u] || !feats[u])
                continue;
              for (guint mi = 0; mi < n_members; mi++)
                {
                  guint v = members[mi];
                  guint w_uv = scores[u * n_images + v];
                  guint w_vu = scores[v * n_images + u];
                  if (w_uv >= min_stitch_pairs && w_uv > best_w)
                    {
                      best_w = w_uv;
                      best_u = u;
                      best_v = v;
                      use_inv = FALSE;
                    }
                  if (w_vu >= min_stitch_pairs && w_vu > best_w)
                    {
                      best_w = w_vu;
                      best_u = u;
                      best_v = v;
                      use_inv = TRUE;
                    }
                }
            }
          if (best_u == n_images)
            break;

          ElanpressPose rel = use_inv
                                ? elanpress_pose_invert (poses[best_v * n_images + best_u])
                                : poses[best_u * n_images + best_v];
          global_pose[best_u] = elanpress_pose_compose (global_pose[best_v], rel);
          visited[best_u] = TRUE;
          members[n_members++] = best_u;
        }

      /* Merge keypoints of all member strips into one global coordinate frame,
       * deduplicating spatially coincident same-orientation keypoints */
      GArray *kps = g_array_new (FALSE, FALSE, sizeof (ElanpressKeypoint));
      gfloat min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f;

      for (guint mi = 0; mi < n_members; mi++)
        {
          guint u = members[mi];
          ElanpressPose gp = global_pose[u];
          gfloat rot = atan2f (gp.s, gp.c);
          const gfloat cx[4] = { 0.0f, (gfloat) frame_w, 0.0f, (gfloat) frame_w };
          const gfloat cy[4] = { 0.0f, 0.0f, (gfloat) frame_h, (gfloat) frame_h };

          for (int k = 0; k < 4; k++)
            {
              gfloat gx = gp.c * cx[k] - gp.s * cy[k] + gp.tx;
              gfloat gy = gp.s * cx[k] + gp.c * cy[k] + gp.ty;
              min_x = MIN (min_x, gx);
              max_x = MAX (max_x, gx);
              min_y = MIN (min_y, gy);
              max_y = MAX (max_y, gy);
            }

          for (guint i = 0; i < feats[u]->n; i++)
            {
              ElanpressKeypoint kp = feats[u]->kp[i];
              gfloat gx = gp.c * kp.x - gp.s * kp.y + gp.tx;
              gfloat gy = gp.s * kp.x + gp.c * kp.y + gp.ty;
              gfloat ga = kp.angle + rot;
              gboolean dup = FALSE;

              while (ga < 0.0f)
                ga += (gfloat) (2.0 * G_PI);
              while (ga >= (gfloat) (2.0 * G_PI))
                ga -= (gfloat) (2.0 * G_PI);

              for (guint e = 0; e < kps->len; e++)
                {
                  ElanpressKeypoint *ek = &g_array_index (kps, ElanpressKeypoint, e);
                  gfloat dx = gx - ek->x, dy = gy - ek->y;
                  if (dx * dx + dy * dy < 6.25f &&
                      elanpress_angle_diff (ga, ek->angle) < 0.45f)
                    {
                      if (kp.response > ek->response)
                        {
                          kp.x = gx;
                          kp.y = gy;
                          kp.angle = ga;
                          *ek = kp;
                        }
                      dup = TRUE;
                      break;
                    }
                }
              if (!dup)
                {
                  kp.x = gx;
                  kp.y = gy;
                  kp.angle = ga;
                  g_array_append_val (kps, kp);
                }
            }
        }

      ElanpressFeatures *comp = g_new0 (ElanpressFeatures, 1);
      comp->n = kps->len;
      comp->kp = (ElanpressKeypoint *) g_array_free (kps, FALSE);
      g_ptr_array_add (comps, comp);
      mosaic->total_keypoints += comp->n;

      if (n_members > mosaic->largest_nodes)
        {
          mosaic->largest_nodes = n_members;
          mosaic->span_w = max_x - min_x;
          mosaic->span_h = max_y - min_y;
        }
    }

  mosaic->n_components = comps->len;
  mosaic->total_nodes = n_images;
  mosaic->components = (ElanpressFeatures **) g_ptr_array_free (comps, FALSE);
  return mosaic;
}

/* Match probe against a stitched mosaic component using spatial non-maximum
 * exclusion (3.5 px) when computing Lowe's 2nd-nearest neighbor d2, so
 * overlapping observations of the same physical point never cancel each other. */
static guint
elanpress_match_mosaic_component (const ElanpressFeatures *probe,
                                  const ElanpressFeatures *comp)
{
  const gdouble ratio2 = ELANPRESS_MATCH_RATIO * ELANPRESS_MATCH_RATIO;
  g_autofree ElanpressPair *pairs = NULL;
  g_autofree gint *dists = NULL;
  guint n = 0;

  if (!probe || !comp || probe->n < 2 || comp->n < 2)
    return 0;

  pairs = g_new (ElanpressPair, probe->n);
  dists = g_new (gint, comp->n);

  for (guint i = 0; i < probe->n; i++)
    {
      const ElanpressKeypoint *kp = &probe->kp[i];
      gint d1 = G_MAXINT, d2 = G_MAXINT;
      guint b1 = 0;

      for (guint j = 0; j < comp->n; j++)
        {
          gint d = elanpress_desc_dist2 (kp->desc, comp->kp[j].desc);
          dists[j] = d;
          if (d < d1)
            {
              d1 = d;
              b1 = j;
            }
        }

      gfloat b1x = comp->kp[b1].x, b1y = comp->kp[b1].y;
      for (guint j = 0; j < comp->n; j++)
        {
          if (j == b1)
            continue;
          gfloat dx = comp->kp[j].x - b1x, dy = comp->kp[j].y - b1y;
          if (dx * dx + dy * dy < 12.25f)
            continue;
          if (dists[j] < d2)
            d2 = dists[j];
        }

      if (d2 == G_MAXINT || d1 >= ratio2 * d2)
        continue;

      pairs[n].px = kp->x;
      pairs[n].py = kp->y;
      pairs[n].ex = b1x;
      pairs[n].ey = b1y;
      pairs[n].rot = comp->kp[b1].angle - kp->angle;
      pairs[n].ei = b1;
      n++;
    }

  return elanpress_score_pairs (pairs, n, NULL);
}

guint
elanpress_mosaic_match (const ElanpressFeatures *probe,
                        const ElanpressMosaic   *mosaic,
                        guint                   *best_comp_out)
{
  guint best = 0, best_c = 0;

  if (!probe || !mosaic)
    return 0;

  for (guint c = 0; c < mosaic->n_components; c++)
    {
      guint s = elanpress_match_mosaic_component (probe, mosaic->components[c]);
      if (s > best)
        {
          best = s;
          best_c = c;
        }
    }
  if (best_comp_out)
    *best_comp_out = best_c;
  return best;
}

void
elanpress_mosaic_free (ElanpressMosaic *mosaic)
{
  if (!mosaic)
    return;
  for (guint c = 0; c < mosaic->n_components; c++)
    elanpress_features_free (mosaic->components[c]);
  g_free (mosaic->components);
  g_free (mosaic);
}

static ElanpressFeatures *
elanpress_features_dup (const ElanpressFeatures *src)
{
  ElanpressFeatures *dst = g_new0 (ElanpressFeatures, 1);
  if (!src || src->n == 0)
    return dst;
  dst->n = src->n;
  dst->kp = g_memdup2 (src->kp, sizeof (ElanpressKeypoint) * src->n);
  return dst;
}

static guint
elanpress_count_distinct_spatial (const ElanpressPair *pairs, const gboolean *fit,
                                  guint n, gfloat min_sep2)
{
  guint probe = 0, enrolled = 0;

  for (guint i = 0; i < n; i++)
    {
      gboolean seen_p = FALSE, seen_e = FALSE;

      if (!fit[i])
        continue;
      for (guint j = 0; j < i; j++)
        {
          if (!fit[j])
            continue;
          if (pairs[j].px == pairs[i].px && pairs[j].py == pairs[i].py)
            seen_p = TRUE;
          gfloat dx = pairs[j].ex - pairs[i].ex;
          gfloat dy = pairs[j].ey - pairs[i].ey;
          if (dx * dx + dy * dy < min_sep2)
            seen_e = TRUE;
        }
      probe += !seen_p;
      enrolled += !seen_e;
    }
  return MIN (probe, enrolled);
}

static guint
elanpress_score_pairs_spatial_pose (const ElanpressPair *pairs, guint n,
                                    ElanpressPose *out_pose)
{
  const gfloat loose2 = ELANPRESS_MATCH_LOOSE_PX * ELANPRESS_MATCH_LOOSE_PX;
  const gfloat tol2 = ELANPRESS_MATCH_TOL_PX * ELANPRESS_MATCH_TOL_PX;
  g_autofree guint *support = NULL;
  g_autofree gboolean *fit = NULL, *next = NULL;
  guint best = 0;

  if (out_pose)
    *out_pose = (ElanpressPose) { 1.0f, 0.0f, 0.0f, 0.0f };
  if (n < 3)
    return 0;

  support = g_new0 (guint, n);
  fit = g_new (gboolean, n);
  next = g_new (gboolean, n);

  for (guint k = 0; k < n; k++)
    {
      ElanpressPlacement m;
      elanpress_pair_placement (&pairs[k], &m);
      for (guint i = 0; i < n; i++)
        support[k] += elanpress_pair_fits (&pairs[i], &m, loose2);
    }

  for (int round = 0; round < 12; round++)
    {
      ElanpressPlacement m;
      guint top = 0, cnt;

      for (guint k = 1; k < n; k++)
        if (support[k] > support[top])
          top = k;
      if (support[top] < 3)
        break;
      support[top] = 0;

      elanpress_pair_placement (&pairs[top], &m);
      for (guint i = 0; i < n; i++)
        fit[i] = elanpress_pair_fits (&pairs[i], &m, loose2);

      for (int step = 0; step < 3; step++)
        {
          gboolean same = TRUE;
          guint was = 0, now = 0;

          if (!elanpress_refit (pairs, fit, n, &m))
            break;
          for (guint i = 0; i < n; i++)
            {
              next[i] = elanpress_pair_fits (&pairs[i], &m, tol2);
              same &= next[i] == fit[i];
              was += fit[i];
              now += next[i];
            }
          memcpy (fit, next, n * sizeof (gboolean));
          if (same || now < was)
            break;
        }

      cnt = elanpress_count_distinct_spatial (pairs, fit, n, 9.0f);
      if (cnt > best)
        {
          best = cnt;
          if (out_pose)
            {
              out_pose->c = m.c;
              out_pose->s = m.s;
              out_pose->tx = m.tx;
              out_pose->ty = m.ty;
            }
        }
    }

  return best;
}

static gdouble
elanpress_pair_image_zncc (const guint8  *img_a,
                           const guint8  *img_b,
                           ElanpressPose  pose_a_to_b,
                           int            w,
                           int            h,
                           guint         *overlap_px_out)
{
  gdouble sum_a = 0, sum_b = 0, sum_aa = 0, sum_bb = 0, sum_ab = 0;
  guint cnt = 0;

  if (overlap_px_out)
    *overlap_px_out = 0;
  if (!img_a || !img_b || w <= 2 || h <= 2)
    return 0.0;

  for (int py = 0; py < h; py++)
    {
      for (int px = 0; px < w; px++)
        {
          guint8 av = img_a[py * w + px];
          if (av == 0)
            continue;
          gfloat bx = pose_a_to_b.c * px - pose_a_to_b.s * py + pose_a_to_b.tx;
          gfloat by = pose_a_to_b.s * px + pose_a_to_b.c * py + pose_a_to_b.ty;
          int ix = (int) floorf (bx);
          int iy = (int) floorf (by);
          if (ix < 0 || ix >= w - 1 || iy < 0 || iy >= h - 1)
            continue;

          guint8 b00 = img_b[iy * w + ix];
          guint8 b10 = img_b[iy * w + ix + 1];
          guint8 b01 = img_b[(iy + 1) * w + ix];
          guint8 b11 = img_b[(iy + 1) * w + ix + 1];
          if ((b00 | b10 | b01 | b11) == 0)
            continue;

          gfloat fx = bx - ix, fy = by - iy;
          gdouble bv = (1.0 - fy) * ((1.0 - fx) * b00 + fx * b10) +
                       fy * ((1.0 - fx) * b01 + fx * b11);
          gdouble da = (gdouble) av;

          sum_a += da;
          sum_b += bv;
          sum_aa += da * da;
          sum_bb += bv * bv;
          sum_ab += da * bv;
          cnt++;
        }
    }

  if (overlap_px_out)
    *overlap_px_out = cnt;
  if (cnt < 400)
    return 0.0;

  gdouble inv = 1.0 / (gdouble) cnt;
  gdouble va = sum_aa - sum_a * sum_a * inv;
  gdouble vb = sum_bb - sum_b * sum_b * inv;
  if (va <= 1.0 || vb <= 1.0)
    return 0.0;

  return (sum_ab - sum_a * sum_b * inv) / sqrt (va * vb);
}

/* Build a pose graph over the enrollment strips without pooling their
 * descriptors into one diluted Lowe-ratio pool. When cov_images is provided,
 * candidate stitch edges are cross-checked with aligned ridge ZNCC so genuine
 * slightly squished overlaps (5..7 SIFT pairs + ZNCC >= 0.55 over >= 800 px)
 * connect cleanly while false alignments are rejected. */
ElanpressPoseGraph *
elanpress_pose_graph_build_verified (ElanpressFeatures * const *feats,
                                     GPtrArray                 *cov_images,
                                     guint                      n_images,
                                     int                        frame_w,
                                     int                        frame_h,
                                     guint                      min_stitch_pairs)
{
  ElanpressPoseGraph *graph = g_new0 (ElanpressPoseGraph, 1);
  g_autofree guint *scores = NULL;
  g_autofree ElanpressPose *poses = NULL;
  g_autofree gboolean *visited = NULL;
  const guint alt_pairs = min_stitch_pairs > 5 ? 5 : min_stitch_pairs;

  if (!feats || n_images == 0)
    return graph;

  graph->n_strips = n_images;
  graph->strips = g_new0 (ElanpressFeatures *, n_images);
  graph->global_pose = g_new0 (ElanpressPose, n_images);
  graph->comp_id = g_new0 (guint, n_images);
  for (guint i = 0; i < n_images; i++)
    graph->strips[i] = elanpress_features_dup (feats[i]);

  scores = g_new0 (guint, n_images * n_images);
  poses = g_new0 (ElanpressPose, n_images * n_images);
  visited = g_new0 (gboolean, n_images);

  for (guint i = 0; i < n_images; i++)
    for (guint j = 0; j < n_images; j++)
      {
        if (i == j || !feats[i] || !feats[j])
          continue;
        guint sc = elanpress_match_features_pose (feats[i], feats[j],
                                                  &poses[i * n_images + j], FALSE);
        if (cov_images && i < cov_images->len && j < cov_images->len)
          {
            if (sc >= alt_pairs)
              {
                guint ov = 0;
                gdouble zncc = elanpress_pair_image_zncc (g_ptr_array_index (cov_images, i),
                                                          g_ptr_array_index (cov_images, j),
                                                          poses[i * n_images + j],
                                                          frame_w, frame_h, &ov);
                gboolean ok = (sc >= min_stitch_pairs && zncc >= 0.40 && ov >= 400) ||
                              (sc >= alt_pairs && zncc >= 0.55 && ov >= 800);
                if (!ok)
                  sc = 0;
                else if (sc < min_stitch_pairs)
                  /* Promote ZNCC-confirmed overlap so MST accepts it below strong SIFT edges */
                  sc = min_stitch_pairs;
              }
            else
              {
                sc = 0;
              }
          }
        scores[i * n_images + j] = sc;
      }

  guint cid = 0;
  for (;;)
    {
      guint root = n_images;
      guint best_deg = 0;
      g_autofree guint *members = g_new0 (guint, n_images);
      guint n_members = 0;

      for (guint i = 0; i < n_images; i++)
        {
          if (visited[i] || !feats[i])
            continue;
          guint deg = 1;
          for (guint j = 0; j < n_images; j++)
            {
              if (i == j || visited[j])
                continue;
              guint w = MAX (scores[i * n_images + j], scores[j * n_images + i]);
              if (w >= min_stitch_pairs)
                deg += w;
            }
          if (deg > best_deg)
            {
              best_deg = deg;
              root = i;
            }
        }
      if (root == n_images)
        break;

      visited[root] = TRUE;
      graph->comp_id[root] = cid;
      graph->global_pose[root] = (ElanpressPose) { 1.0f, 0.0f, 0.0f, 0.0f };
      members[n_members++] = root;

      for (;;)
        {
          guint best_u = n_images, best_v = n_images, best_w = 0;
          gboolean use_inv = FALSE;

          for (guint u = 0; u < n_images; u++)
            {
              if (visited[u] || !feats[u])
                continue;
              for (guint mi = 0; mi < n_members; mi++)
                {
                  guint v = members[mi];
                  guint w_uv = scores[u * n_images + v];
                  guint w_vu = scores[v * n_images + u];
                  if (w_uv >= min_stitch_pairs && w_uv > best_w)
                    {
                      best_w = w_uv;
                      best_u = u;
                      best_v = v;
                      use_inv = FALSE;
                    }
                  if (w_vu >= min_stitch_pairs && w_vu > best_w)
                    {
                      best_w = w_vu;
                      best_u = u;
                      best_v = v;
                      use_inv = TRUE;
                    }
                }
            }
          if (best_u == n_images)
            break;

          ElanpressPose rel = use_inv
                                ? elanpress_pose_invert (poses[best_v * n_images + best_u])
                                : poses[best_u * n_images + best_v];
          graph->global_pose[best_u] = elanpress_pose_compose (graph->global_pose[best_v], rel);
          graph->comp_id[best_u] = cid;
          visited[best_u] = TRUE;
          members[n_members++] = best_u;
        }

      if (n_members > graph->largest_nodes)
        {
          gfloat min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f;
          const gfloat cx[4] = { 0.0f, (gfloat) frame_w, 0.0f, (gfloat) frame_w };
          const gfloat cy[4] = { 0.0f, 0.0f, (gfloat) frame_h, (gfloat) frame_h };

          for (guint mi = 0; mi < n_members; mi++)
            {
              ElanpressPose gp = graph->global_pose[members[mi]];
              for (int k = 0; k < 4; k++)
                {
                  gfloat gx = gp.c * cx[k] - gp.s * cy[k] + gp.tx;
                  gfloat gy = gp.s * cx[k] + gp.c * cy[k] + gp.ty;
                  min_x = MIN (min_x, gx);
                  max_x = MAX (max_x, gx);
                  min_y = MIN (min_y, gy);
                  max_y = MAX (max_y, gy);
                }
            }
          graph->largest_nodes = n_members;
          graph->span_w = max_x - min_x;
          graph->span_h = max_y - min_y;
        }
      cid++;
    }

  graph->n_components = cid;
  return graph;
}

ElanpressPoseGraph *
elanpress_pose_graph_build (ElanpressFeatures * const *feats,
                            guint                      n_images,
                            int                        frame_w,
                            int                        frame_h,
                            guint                      min_stitch_pairs)
{
  return elanpress_pose_graph_build_verified (feats, NULL, n_images,
                                              frame_w, frame_h, min_stitch_pairs);
}

guint
elanpress_pose_graph_match_pose (const ElanpressFeatures  *probe,
                                 const ElanpressPoseGraph *graph,
                                 gboolean                  recip_nn,
                                 guint                    *best_comp_out,
                                 ElanpressPose            *best_pose_out)
{
  const gdouble ratio2 = ELANPRESS_MATCH_RATIO * ELANPRESS_MATCH_RATIO;
  guint best = 0, best_c = 0;
  ElanpressPose best_p = { 1.0f, 0.0f, 0.0f, 0.0f };

  if (best_pose_out)
    *best_pose_out = best_p;
  if (!probe || !graph || probe->n < 2)
    return 0;

  for (guint cid = 0; cid < graph->n_components; cid++)
    {
      GArray *gpairs = g_array_new (FALSE, FALSE, sizeof (ElanpressPair));

      for (guint u = 0; u < graph->n_strips; u++)
        {
          if (graph->comp_id[u] != cid)
            continue;
          const ElanpressFeatures *enrolled = graph->strips[u];
          if (!enrolled || enrolled->n < 2)
            continue;

          ElanpressPose gp = graph->global_pose[u];
          gfloat rot_u = atan2f (gp.s, gp.c);
          g_autofree gint *fwd_d1 = g_new (gint, probe->n);
          g_autofree gint *fwd_d2 = g_new (gint, probe->n);
          g_autofree guint *fwd_b1 = g_new0 (guint, probe->n);
          g_autofree gint *rev_d1 = recip_nn ? g_new (gint, enrolled->n) : NULL;
          g_autofree guint *rev_b1 = recip_nn ? g_new0 (guint, enrolled->n) : NULL;

          g_autofree gint *p_norm2 = g_new (gint, probe->n);
          g_autofree gint *e_norm2 = g_new (gint, enrolled->n);

          for (guint i = 0; i < probe->n; i++)
            {
              fwd_d1[i] = fwd_d2[i] = G_MAXINT;
              p_norm2[i] = elanpress_desc_norm2 (probe->kp[i].desc);
            }
          for (guint j = 0; j < enrolled->n; j++)
            {
              e_norm2[j] = elanpress_desc_norm2 (enrolled->kp[j].desc);
              if (recip_nn)
                rev_d1[j] = G_MAXINT;
            }

          for (guint i = 0; i < probe->n; i++)
            {
              const guint8 *pdesc = probe->kp[i].desc;
              gint pn = p_norm2[i];
              gint d1 = G_MAXINT, d2 = G_MAXINT;
              guint b1 = 0;
              for (guint j = 0; j < enrolled->n; j++)
                {
                  gint d = pn + e_norm2[j] - 2 * elanpress_desc_dot (pdesc, enrolled->kp[j].desc);
                  if (d < d1)
                    {
                      d2 = d1;
                      d1 = d;
                      b1 = j;
                    }
                  else if (d < d2)
                    {
                      d2 = d;
                    }
                  if (recip_nn && d < rev_d1[j])
                    {
                      rev_d1[j] = d;
                      rev_b1[j] = i;
                    }
                }
              fwd_d1[i] = d1;
              fwd_d2[i] = d2;
              fwd_b1[i] = b1;
            }

          for (guint i = 0; i < probe->n; i++)
            {
              guint b1 = fwd_b1[i];
              if (fwd_d2[i] == G_MAXINT || fwd_d1[i] >= ratio2 * fwd_d2[i])
                continue;
              if (recip_nn && rev_b1[b1] != i)
                continue;

              const ElanpressKeypoint *pk = &probe->kp[i];
              const ElanpressKeypoint *ek = &enrolled->kp[b1];
              gfloat gx = gp.c * ek->x - gp.s * ek->y + gp.tx;
              gfloat gy = gp.s * ek->x + gp.c * ek->y + gp.ty;
              gboolean dup = FALSE;

              for (guint k = 0; k < gpairs->len; k++)
                {
                  const ElanpressPair *ep = &g_array_index (gpairs, ElanpressPair, k);
                  if (ep->px == pk->x && ep->py == pk->y)
                    {
                      gfloat dx = ep->ex - gx, dy = ep->ey - gy;
                      if (dx * dx + dy * dy < 12.25f)
                        {
                          dup = TRUE;
                          break;
                        }
                    }
                }
              if (dup)
                continue;

              ElanpressPair pair;
              pair.px = pk->x;
              pair.py = pk->y;
              pair.ex = gx;
              pair.ey = gy;
              pair.rot = (ek->angle + rot_u) - pk->angle;
              pair.ei = b1;
              g_array_append_val (gpairs, pair);
            }
        }

      ElanpressPose comp_p = { 1.0f, 0.0f, 0.0f, 0.0f };
      guint score = elanpress_score_pairs_spatial_pose ((const ElanpressPair *) gpairs->data,
                                                        gpairs->len, &comp_p);
      g_array_free (gpairs, TRUE);
      if (score > best)
        {
          best = score;
          best_c = cid;
          best_p = comp_p;
        }
    }

  if (best_comp_out)
    *best_comp_out = best_c;
  if (best_pose_out)
    *best_pose_out = best_p;
  return best;
}

guint
elanpress_pose_graph_match (const ElanpressFeatures  *probe,
                            const ElanpressPoseGraph *graph,
                            gboolean                  recip_nn,
                            guint                    *best_comp_out)
{
  return elanpress_pose_graph_match_pose (probe, graph, recip_nn, best_comp_out, NULL);
}

/* Second-stage ridge-pixel verification: given the global rigid placement
 * proposed by pose-graph SIFT matching, compute Zero-Mean Normalized
 * Cross-Correlation (ZNCC) over the overlapping covered pixels against each
 * strip in the matched component. Genuine aligned ridges yield strongly
 * positive ZNCC; chance impostor alignments sit near 0. */
gdouble
elanpress_pose_graph_zncc (const guint8             *probe_img,
                           GPtrArray                *enrolled_images,
                           guint                     stride,
                           guint                     offset,
                           const ElanpressPoseGraph *graph,
                           guint                     comp_id,
                           ElanpressPose             global_pose,
                           int                       w,
                           int                       h,
                           guint                    *overlap_px_out)
{
  gdouble best_zncc = -1.0;
  guint best_ov = 0;

  if (overlap_px_out)
    *overlap_px_out = 0;
  if (!probe_img || !enrolled_images || !graph || stride == 0 || w <= 2 || h <= 2)
    return 0.0;

  for (guint u = 0; u < graph->n_strips; u++)
    {
      guint img_idx = u * stride + offset;
      if (graph->comp_id[u] != comp_id || img_idx >= enrolled_images->len)
        continue;

      const guint8 *eimg = g_ptr_array_index (enrolled_images, img_idx);
      ElanpressPose lm = elanpress_pose_compose (elanpress_pose_invert (graph->global_pose[u]),
                                                 global_pose);
      gdouble sum_p = 0, sum_e = 0, sum_pp = 0, sum_ee = 0, sum_pe = 0;
      guint cnt = 0;

      for (int py = 0; py < h; py++)
        {
          for (int px = 0; px < w; px++)
            {
              guint8 pv = probe_img[py * w + px];
              if (pv == 0)
                continue;
              gfloat ex = lm.c * px - lm.s * py + lm.tx;
              gfloat ey = lm.s * px + lm.c * py + lm.ty;
              int ix = (int) floorf (ex);
              int iy = (int) floorf (ey);
              if (ix < 0 || ix >= w - 1 || iy < 0 || iy >= h - 1)
                continue;

              guint8 e00 = eimg[iy * w + ix];
              guint8 e10 = eimg[iy * w + ix + 1];
              guint8 e01 = eimg[(iy + 1) * w + ix];
              guint8 e11 = eimg[(iy + 1) * w + ix + 1];
              if ((e00 | e10 | e01 | e11) == 0)
                continue;

              gfloat fx = ex - ix, fy = ey - iy;
              gdouble ev = (1.0 - fy) * ((1.0 - fx) * e00 + fx * e10) +
                           fy * ((1.0 - fx) * e01 + fx * e11);
              gdouble dp = (gdouble) pv;

              sum_p += dp;
              sum_e += ev;
              sum_pp += dp * dp;
              sum_ee += ev * ev;
              sum_pe += dp * ev;
              cnt++;
            }
        }

      if (cnt < 400)
        continue;

      gdouble inv = 1.0 / (gdouble) cnt;
      gdouble vp = sum_pp - sum_p * sum_p * inv;
      gdouble ve = sum_ee - sum_e * sum_e * inv;
      if (vp > 1.0 && ve > 1.0)
        {
          gdouble zncc = (sum_pe - sum_p * sum_e * inv) / sqrt (vp * ve);
          if (zncc > best_zncc)
            {
              best_zncc = zncc;
              best_ov = cnt;
            }
        }
    }

  if (overlap_px_out)
    *overlap_px_out = best_ov;
  return best_zncc > -1.0 ? best_zncc : 0.0;
}

/* Patch-wise elastic ZNCC (v4.1): finds the best rigid-aligned strip in the
 * matched component, warps it into probe coordinates with border padding, and
 * evaluates tile-wise ZNCC on a 3x2 (strip) or 3x3 (square) grid with small
 * +/-elastic_radius px local shifts per tile to absorb skin stretching.
 * Requires >= 2 valid tiles (>= 200 px each) and >= 600 total valid pixels so
 * single-corner coincidences cannot inflate impostor scores. */
gdouble
elanpress_pose_graph_zncc_elastic (const guint8             *probe_img,
                                   GPtrArray                *enrolled_images,
                                   guint                     stride,
                                   guint                     offset,
                                   const ElanpressPoseGraph *graph,
                                   guint                     comp_id,
                                   ElanpressPose             global_pose,
                                   int                       w,
                                   int                       h,
                                   int                       elastic_radius,
                                   gdouble                  *rigid_zncc_out,
                                   guint                    *overlap_px_out)
{
  gdouble best_rigid = -1.0;
  guint best_ov = 0;
  gint best_idx = -1;
  ElanpressPose best_lm = { 1.0f, 0.0f, 0.0f, 0.0f };

  if (rigid_zncc_out)
    *rigid_zncc_out = 0.0;
  if (overlap_px_out)
    *overlap_px_out = 0;
  if (!probe_img || !enrolled_images || !graph || stride == 0 || w <= 2 || h <= 2)
    return 0.0;

  for (guint u = 0; u < graph->n_strips; u++)
    {
      guint img_idx = u * stride + offset;
      if (graph->comp_id[u] != comp_id || img_idx >= enrolled_images->len)
        continue;

      const guint8 *eimg = g_ptr_array_index (enrolled_images, img_idx);
      ElanpressPose lm = elanpress_pose_compose (elanpress_pose_invert (graph->global_pose[u]),
                                                 global_pose);
      gdouble sum_p = 0, sum_e = 0, sum_pp = 0, sum_ee = 0, sum_pe = 0;
      guint cnt = 0;

      for (int py = 0; py < h; py++)
        {
          for (int px = 0; px < w; px++)
            {
              guint8 pv = probe_img[py * w + px];
              if (pv == 0)
                continue;
              gfloat ex = lm.c * px - lm.s * py + lm.tx;
              gfloat ey = lm.s * px + lm.c * py + lm.ty;
              int ix = (int) floorf (ex);
              int iy = (int) floorf (ey);
              if (ix < 0 || ix >= w - 1 || iy < 0 || iy >= h - 1)
                continue;
              guint8 e00 = eimg[iy * w + ix];
              guint8 e10 = eimg[iy * w + ix + 1];
              guint8 e01 = eimg[(iy + 1) * w + ix];
              guint8 e11 = eimg[(iy + 1) * w + ix + 1];
              if ((e00 | e10 | e01 | e11) == 0)
                continue;
              gfloat fx = ex - ix, fy = ey - iy;
              gdouble ev = (1.0 - fy) * ((1.0 - fx) * e00 + fx * e10) +
                           fy * ((1.0 - fx) * e01 + fx * e11);
              gdouble dp = (gdouble) pv;

              sum_p += dp;
              sum_e += ev;
              sum_pp += dp * dp;
              sum_ee += ev * ev;
              sum_pe += dp * ev;
              cnt++;
            }
        }

      if (cnt < 400)
        continue;

      gdouble inv = 1.0 / (gdouble) cnt;
      gdouble vp = sum_pp - sum_p * sum_p * inv;
      gdouble ve = sum_ee - sum_e * sum_e * inv;
      if (vp > 1.0 && ve > 1.0)
        {
          gdouble zncc = (sum_pe - sum_p * sum_e * inv) / sqrt (vp * ve);
          if (zncc > best_rigid)
            {
              best_rigid = zncc;
              best_ov = cnt;
              best_idx = (gint) img_idx;
              best_lm = lm;
            }
        }
    }

  if (best_idx < 0)
    return 0.0;

  if (rigid_zncc_out)
    *rigid_zncc_out = best_rigid;
  if (overlap_px_out)
    *overlap_px_out = best_ov;

  int rad = CLAMP (elastic_radius, 0, 2);
  if (rad == 0 || best_ov < 600)
    return best_rigid;

  int pw = w + 2 * rad;
  int ph = h + 2 * rad;
  g_autofree gfloat *warped = g_new0 (gfloat, (gsize) pw * ph);
  g_autofree guint8 *valid = g_new0 (guint8, (gsize) pw * ph);
  const guint8 *eimg = g_ptr_array_index (enrolled_images, (guint) best_idx);

  for (int wy = -rad; wy < h + rad; wy++)
    {
      int gy = wy + rad;
      for (int wx = -rad; wx < w + rad; wx++)
        {
          int gx = wx + rad;
          int idx = gy * pw + gx;
          gfloat ex = best_lm.c * wx - best_lm.s * wy + best_lm.tx;
          gfloat ey = best_lm.s * wx + best_lm.c * wy + best_lm.ty;
          int ix = (int) floorf (ex);
          int iy = (int) floorf (ey);
          if (ix < 0 || ix >= w - 1 || iy < 0 || iy >= h - 1)
            continue;
          guint8 e00 = eimg[iy * w + ix];
          guint8 e10 = eimg[iy * w + ix + 1];
          guint8 e01 = eimg[(iy + 1) * w + ix];
          guint8 e11 = eimg[(iy + 1) * w + ix + 1];
          if ((e00 | e10 | e01 | e11) == 0)
            continue;
          gfloat fx = ex - ix, fy = ey - iy;
          warped[idx] = (1.0f - fy) * ((1.0f - fx) * e00 + fx * e10) +
                        fy * ((1.0f - fx) * e01 + fx * e11);
          valid[idx] = 1;
        }
    }

  int ncols = 3;
  int nrows = (h >= 72) ? 3 : 2;
  gdouble wsum = 0.0, zsum = 0.0;
  int ntiles = 0;

  for (int ty = 0; ty < nrows; ty++)
    {
      int y0 = (ty * h) / nrows;
      int y1 = ((ty + 1) * h) / nrows;
      for (int tx = 0; tx < ncols; tx++)
        {
          int x0 = (tx * w) / ncols;
          int x1 = ((tx + 1) * w) / ncols;
          gdouble best_tz = -2.0;
          guint best_tc = 0;

          for (int dy = -rad; dy <= rad; dy++)
            {
              for (int dx = -rad; dx <= rad; dx++)
                {
                  gdouble sp = 0, se = 0, spp = 0, see = 0, spe = 0;
                  guint cnt = 0;

                  for (int py = y0; py < y1; py++)
                    {
                      const guint8 *prow = probe_img + py * w;
                      int wrow = (py + rad + dy) * pw + rad + dx;
                      for (int px = x0; px < x1; px++)
                        {
                          guint8 pv = prow[px];
                          if (pv == 0)
                            continue;
                          int wi = wrow + px;
                          if (!valid[wi])
                            continue;
                          gdouble dp = (gdouble) pv;
                          gdouble ev = (gdouble) warped[wi];
                          sp += dp;
                          se += ev;
                          spp += dp * dp;
                          see += ev * ev;
                          spe += dp * ev;
                          cnt++;
                        }
                    }

                  if (cnt < 200)
                    continue;

                  gdouble inv = 1.0 / (gdouble) cnt;
                  gdouble vp = spp - sp * sp * inv;
                  gdouble ve = see - se * se * inv;
                  if (vp > 1.0 && ve > 1.0)
                    {
                      gdouble z = (spe - sp * se * inv) / sqrt (vp * ve);
                      if (z > best_tz)
                        {
                          best_tz = z;
                          best_tc = cnt;
                        }
                    }
                }
            }

          if (best_tz > -1.5)
            {
              zsum += best_tz * best_tc;
              wsum += best_tc;
              ntiles++;
            }
        }
    }

  if (ntiles >= 2 && wsum >= 600.0)
    return MAX (best_rigid, zsum / wsum);
  return best_rigid;
}

/* Compact portable little-endian serialization for FpPrint fpi-data (v4):
 * stores (x, y, angle) as u16 fixed-point (6 B vs 20 B of floats) + lossless
 * 128-byte RootSIFT descriptors, capped to top 160 keypoints per strip. */
#define ELANPRESS_PG_MAGIC 0x45504734u
#define ELANPRESS_PG_MAX_KP_STORED 200u

static void
pg_append_u16 (GByteArray *ba, guint16 v)
{
  guint16 le = GUINT16_TO_LE (v);
  g_byte_array_append (ba, (const guint8 *) &le, sizeof (le));
}

static void
pg_append_u32 (GByteArray *ba, guint32 v)
{
  guint32 le = GUINT32_TO_LE (v);
  g_byte_array_append (ba, (const guint8 *) &le, sizeof (le));
}

static void
pg_append_f32 (GByteArray *ba, gfloat v)
{
  guint32 bits;
  memcpy (&bits, &v, sizeof (bits));
  pg_append_u32 (ba, bits);
}

static gboolean
pg_read_u16 (const guint8 *data, gsize len, gsize *pos, guint16 *out)
{
  guint16 le;
  if (*pos + sizeof (le) > len)
    return FALSE;
  memcpy (&le, data + *pos, sizeof (le));
  *pos += sizeof (le);
  *out = GUINT16_FROM_LE (le);
  return TRUE;
}

static gboolean
pg_read_u32 (const guint8 *data, gsize len, gsize *pos, guint32 *out)
{
  guint32 le;
  if (*pos + sizeof (le) > len)
    return FALSE;
  memcpy (&le, data + *pos, sizeof (le));
  *pos += sizeof (le);
  *out = GUINT32_FROM_LE (le);
  return TRUE;
}

static gboolean
pg_read_f32 (const guint8 *data, gsize len, gsize *pos, gfloat *out)
{
  guint32 bits = 0;
  if (!pg_read_u32 (data, len, pos, &bits))
    return FALSE;
  memcpy (out, &bits, sizeof (*out));
  return isfinite (*out);
}

/* Pack 8-bit covnorm image (0=uncovered, 1..255=covered) into 4-bit nibbles
 * (0=uncovered, 1..15=covered), halving image storage with <0.002 ZNCC delta. */
guint8 *
elanpress_pack_image_4bit (const guint8 *img, unsigned int size, gsize *out_len)
{
  gsize n_bytes = (size + 1) / 2;
  guint8 *out;

  if (out_len)
    *out_len = 0;
  if (!img || size == 0)
    return NULL;

  out = g_malloc0 (n_bytes);
  for (unsigned int i = 0; i < size; i++)
    {
      guint8 v = img[i];
      guint8 q = 0;
      if (v > 0)
        q = (guint8) CLAMP (1 + ((unsigned int) (v - 1) * 14 + 127) / 254, 1, 15);
      if ((i & 1u) == 0)
        out[i >> 1] = q;
      else
        out[i >> 1] |= (guint8) (q << 4);
    }
  if (out_len)
    *out_len = n_bytes;
  return out;
}

guint8 *
elanpress_unpack_image_4bit (const guint8 *packed, gsize packed_len, unsigned int size)
{
  gsize need = (size + 1) / 2;
  guint8 *out;

  if (!packed || size == 0 || packed_len != need)
    return NULL;

  out = g_malloc (size);
  for (unsigned int i = 0; i < size; i++)
    {
      guint8 b = packed[i >> 1];
      guint8 q = (i & 1u) == 0 ? (b & 0x0Fu) : ((b >> 4) & 0x0Fu);
      out[i] = q == 0 ? 0 : (guint8) (1 + ((unsigned int) (q - 1) * 254) / 14);
    }
  return out;
}

GBytes *
elanpress_pose_graph_serialize (const ElanpressPoseGraph *graph)
{
  GByteArray *ba;
  const gfloat two_pi = (gfloat) (2.0 * G_PI);

  if (!graph)
    return NULL;

  ba = g_byte_array_new ();
  pg_append_u32 (ba, ELANPRESS_PG_MAGIC);
  pg_append_u32 (ba, graph->n_strips);
  pg_append_u32 (ba, graph->n_components);
  pg_append_u32 (ba, graph->largest_nodes);
  pg_append_f32 (ba, graph->span_w);
  pg_append_f32 (ba, graph->span_h);

  for (guint u = 0; u < graph->n_strips; u++)
    {
      const ElanpressFeatures *f = graph->strips[u];
      guint32 n_kp = f ? MIN (f->n, ELANPRESS_PG_MAX_KP_STORED) : 0;

      pg_append_u32 (ba, graph->comp_id[u]);
      pg_append_f32 (ba, graph->global_pose[u].c);
      pg_append_f32 (ba, graph->global_pose[u].s);
      pg_append_f32 (ba, graph->global_pose[u].tx);
      pg_append_f32 (ba, graph->global_pose[u].ty);
      pg_append_u32 (ba, n_kp);

      for (guint i = 0; i < n_kp; i++)
        {
          const ElanpressKeypoint *kp = &f->kp[i];
          gfloat ang = fmodf (kp->angle, two_pi);
          if (ang < 0.0f)
            ang += two_pi;
          guint16 qx = (guint16) CLAMP (lroundf (kp->x * 100.0f), 0, 65535);
          guint16 qy = (guint16) CLAMP (lroundf (kp->y * 100.0f), 0, 65535);
          guint16 qa = (guint16) CLAMP (lroundf ((ang / two_pi) * 65535.0f), 0, 65535);

          pg_append_u16 (ba, qx);
          pg_append_u16 (ba, qy);
          pg_append_u16 (ba, qa);
          g_byte_array_append (ba, kp->desc, ELANPRESS_SIFT_DESC_LEN);
        }
    }

  return g_byte_array_free_to_bytes (ba);
}

ElanpressPoseGraph *
elanpress_pose_graph_deserialize (const guint8 *data, gsize len)
{
  gsize pos = 0;
  guint32 magic = 0, n_strips = 0, n_comps = 0, largest = 0;
  gfloat span_w = 0.0f, span_h = 0.0f;
  const gfloat two_pi = (gfloat) (2.0 * G_PI);
  ElanpressPoseGraph *graph;

  if (!data || len < 24)
    return NULL;

  if (!pg_read_u32 (data, len, &pos, &magic) || magic != ELANPRESS_PG_MAGIC ||
      !pg_read_u32 (data, len, &pos, &n_strips) || n_strips == 0 || n_strips > 64 ||
      !pg_read_u32 (data, len, &pos, &n_comps) || n_comps == 0 || n_comps > n_strips ||
      !pg_read_u32 (data, len, &pos, &largest) || largest > n_strips ||
      !pg_read_f32 (data, len, &pos, &span_w) ||
      !pg_read_f32 (data, len, &pos, &span_h))
    return NULL;

  graph = g_new0 (ElanpressPoseGraph, 1);
  graph->n_strips = n_strips;
  graph->n_components = n_comps;
  graph->largest_nodes = largest;
  graph->span_w = span_w;
  graph->span_h = span_h;
  graph->strips = g_new0 (ElanpressFeatures *, n_strips);
  graph->global_pose = g_new0 (ElanpressPose, n_strips);
  graph->comp_id = g_new0 (guint, n_strips);

  for (guint u = 0; u < n_strips; u++)
    {
      guint32 cid = 0, n_kp = 0;
      ElanpressFeatures *f;

      if (!pg_read_u32 (data, len, &pos, &cid) || cid >= n_comps ||
          !pg_read_f32 (data, len, &pos, &graph->global_pose[u].c) ||
          !pg_read_f32 (data, len, &pos, &graph->global_pose[u].s) ||
          !pg_read_f32 (data, len, &pos, &graph->global_pose[u].tx) ||
          !pg_read_f32 (data, len, &pos, &graph->global_pose[u].ty) ||
          !pg_read_u32 (data, len, &pos, &n_kp) || n_kp > 1024)
        {
          elanpress_pose_graph_free (graph);
          return NULL;
        }
      graph->comp_id[u] = cid;

      f = g_new0 (ElanpressFeatures, 1);
      f->n = n_kp;
      if (n_kp > 0)
        f->kp = g_new0 (ElanpressKeypoint, n_kp);
      graph->strips[u] = f;

      for (guint i = 0; i < n_kp; i++)
        {
          ElanpressKeypoint *kp = &f->kp[i];
          guint16 qx = 0, qy = 0, qa = 0;

          if (!pg_read_u16 (data, len, &pos, &qx) ||
              !pg_read_u16 (data, len, &pos, &qy) ||
              !pg_read_u16 (data, len, &pos, &qa) ||
              pos + ELANPRESS_SIFT_DESC_LEN > len)
            {
              elanpress_pose_graph_free (graph);
              return NULL;
            }
          kp->x = qx / 100.0f;
          kp->y = qy / 100.0f;
          kp->angle = (qa / 65535.0f) * two_pi;
          memcpy (kp->desc, data + pos, ELANPRESS_SIFT_DESC_LEN);
          pos += ELANPRESS_SIFT_DESC_LEN;
        }
    }

  if (pos != len)
    {
      elanpress_pose_graph_free (graph);
      return NULL;
    }

  return graph;
}

static void
elanpress_pose_graph_recompute_span (ElanpressPoseGraph *graph,
                                     int                 frame_w,
                                     int                 frame_h)
{
  const gfloat cx[4] = { 0.0f, (gfloat) frame_w, 0.0f, (gfloat) frame_w };
  const gfloat cy[4] = { 0.0f, 0.0f, (gfloat) frame_h, (gfloat) frame_h };
  gfloat min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f;
  guint cnt = 0;

  if (!graph || graph->n_strips == 0)
    return;

  for (guint u = 0; u < graph->n_strips; u++)
    {
      if (graph->comp_id[u] != 0)
        continue;
      ElanpressPose gp = graph->global_pose[u];
      for (int k = 0; k < 4; k++)
        {
          gfloat gx = gp.c * cx[k] - gp.s * cy[k] + gp.tx;
          gfloat gy = gp.s * cx[k] + gp.c * cy[k] + gp.ty;
          min_x = MIN (min_x, gx);
          max_x = MAX (max_x, gx);
          min_y = MIN (min_y, gy);
          max_y = MAX (max_y, gy);
        }
      cnt++;
    }
  if (cnt > 0)
    {
      graph->largest_nodes = cnt;
      graph->span_w = max_x - min_x;
      graph->span_h = max_y - min_y;
    }
}

ElanpressPoseGraph *
elanpress_pose_graph_new_single (const ElanpressFeatures *feat,
                                 int                      frame_w,
                                 int                      frame_h)
{
  ElanpressPoseGraph *graph = g_new0 (ElanpressPoseGraph, 1);

  graph->n_strips = 1;
  graph->n_components = 1;
  graph->largest_nodes = 1;
  graph->span_w = (gfloat) frame_w;
  graph->span_h = (gfloat) frame_h;
  graph->strips = g_new0 (ElanpressFeatures *, 1);
  graph->global_pose = g_new0 (ElanpressPose, 1);
  graph->comp_id = g_new0 (guint, 1);
  graph->strips[0] = elanpress_features_dup (feat);
  graph->global_pose[0] = (ElanpressPose) { 1.0f, 0.0f, 0.0f, 0.0f };
  graph->comp_id[0] = 0;
  return graph;
}

void
elanpress_pose_graph_append (ElanpressPoseGraph      *graph,
                             const ElanpressFeatures *feat,
                             guint                    comp_id,
                             ElanpressPose            global_pose,
                             int                      frame_w,
                             int                      frame_h)
{
  guint idx;

  if (!graph || !feat)
    return;

  idx = graph->n_strips;
  graph->n_strips = idx + 1;
  graph->strips = g_renew (ElanpressFeatures *, graph->strips, graph->n_strips);
  graph->global_pose = g_renew (ElanpressPose, graph->global_pose, graph->n_strips);
  graph->comp_id = g_renew (guint, graph->comp_id, graph->n_strips);
  graph->strips[idx] = elanpress_features_dup (feat);
  graph->global_pose[idx] = global_pose;
  graph->comp_id[idx] = comp_id;
  if (comp_id + 1 > graph->n_components)
    graph->n_components = comp_id + 1;
  elanpress_pose_graph_recompute_span (graph, frame_w, frame_h);
}

static gdouble
elanpress_depth_weight (guint8 d)
{
  if (d == 0)
    return 0.0;
  if (d == 1)
    return 0.55;
  if (d == 2)
    return 0.88;
  return 1.0;
}

static gboolean
elanpress_strip_covers_world_pt (const guint8 *cov_img,
                                 ElanpressPose inv_pose,
                                 gfloat        wx,
                                 gfloat        wy,
                                 int           w,
                                 int           h)
{
  gfloat lx = inv_pose.c * wx - inv_pose.s * wy + inv_pose.tx;
  gfloat ly = inv_pose.s * wx + inv_pose.c * wy + inv_pose.ty;
  int cx = (int) lroundf (lx);
  int cy = (int) lroundf (ly);
  int hits = 0;

  if (cx < -3 || cx >= w + 3 || cy < -3 || cy >= h + 3)
    return FALSE;

  for (int dy = -3; dy <= 3; dy += 3)
    {
      int py = cy + dy;
      if (py < 0 || py >= h)
        continue;
      for (int dx = -3; dx <= 3; dx += 3)
        {
          int px = cx + dx;
          if (px < 0 || px >= w)
            continue;
          if (cov_img[py * w + px] > 0)
            hits++;
        }
    }
  return hits >= 3;
}

void
elanpress_enroll_map_compute (GPtrArray                *cov_images,
                              const ElanpressPoseGraph *graph,
                              guint                     last_strip_idx,
                              int                       frame_w,
                              int                       frame_h,
                              guint                     min_strips,
                              ElanpressEnrollMap       *out)
{
  g_autofree ElanpressPose *inv_poses = NULL;
  ElanpressPose inv_anchor = { 1.0f, 0.0f, 0.0f, 0.0f };
  gfloat ox = frame_w * 0.5f;
  gfloat oy = frame_h * 0.5f;
  guint main_comp = 0;
  guint main_count = 0;
  guint anchor_idx = 0;
  gdouble sum_w = 0.0;
  gdouble miss_w_sum = 0.0, miss_x = 0.0, miss_y = 0.0;

  memset (out, 0, sizeof (*out));
  if (!cov_images || !graph || graph->n_strips == 0 || frame_w <= 0 || frame_h <= 0)
    return;

  /* Pick the largest connected component (ties keep component containing strip 0) */
  for (guint cid = 0; cid < graph->n_components; cid++)
    {
      guint cnt = 0;
      for (guint u = 0; u < graph->n_strips; u++)
        if (graph->comp_id[u] == cid)
          cnt++;
      if (cnt > main_count)
        {
          main_count = cnt;
          main_comp = cid;
        }
    }

  /* Pin canvas origin to the earliest strip in main_comp so the map stays rock-steady */
  for (guint u = 0; u < graph->n_strips; u++)
    {
      if (graph->comp_id[u] == main_comp)
        {
          anchor_idx = u;
          break;
        }
    }

  inv_anchor = elanpress_pose_invert (graph->global_pose[anchor_idx]);
  inv_poses = g_new0 (ElanpressPose, graph->n_strips);
  for (guint u = 0; u < graph->n_strips; u++)
    {
      if (graph->comp_id[u] == main_comp)
        {
          ElanpressPose aligned = elanpress_pose_compose (inv_anchor, graph->global_pose[u]);
          inv_poses[u] = elanpress_pose_invert (aligned);
        }
    }

  out->stitched_strips = main_count;
  out->total_strips = graph->n_strips;
  out->last_in_main = (last_strip_idx < graph->n_strips &&
                       graph->comp_id[last_strip_idx] == main_comp);

  for (int r = 0; r < ELANPRESS_MAP_ROWS; r++)
    {
      gfloat wy = oy + (((gfloat) r + 0.5f) - ELANPRESS_MAP_ROWS * 0.5f) * ELANPRESS_MAP_CELL_PX;
      gfloat ny = (((gfloat) r + 0.5f) - ELANPRESS_MAP_ROWS * 0.5f) / ELANPRESS_MAP_TARGET_RY;

      for (int c = 0; c < ELANPRESS_MAP_COLS; c++)
        {
          int idx = r * ELANPRESS_MAP_COLS + c;
          gfloat wx = ox + (((gfloat) c + 0.5f) - ELANPRESS_MAP_COLS * 0.5f) * ELANPRESS_MAP_CELL_PX;
          gfloat nx = (((gfloat) c + 0.5f) - ELANPRESS_MAP_COLS * 0.5f) / ELANPRESS_MAP_TARGET_RX;
          guint depth = 0;

          for (guint u = 0; u < graph->n_strips && u < cov_images->len; u++)
            {
              if (graph->comp_id[u] != main_comp)
                continue;
              const guint8 *img = g_ptr_array_index (cov_images, u);
              if (img && elanpress_strip_covers_world_pt (img, inv_poses[u], wx, wy, frame_w, frame_h))
                {
                  if (depth < 9)
                    depth++;
                  if (u == last_strip_idx)
                    out->last_mask[idx] = 1;
                }
            }

          out->grid[idx] = (guint8) depth;

          if (nx * nx + ny * ny <= 1.0f)
            {
              gdouble cw = elanpress_depth_weight ((guint8) depth);
              out->target_cells++;
              if (depth >= 1)
                out->covered_cells++;
              if (depth >= 2)
                out->deep_cells++;
              if (depth >= 3)
                out->core_cells++;
              sum_w += cw;

              gdouble deficit = 1.0 - cw;
              if (deficit > 0.01)
                {
                  miss_w_sum += deficit;
                  miss_x += deficit * nx;
                  miss_y += deficit * ny;
                }
            }
        }
    }

  if (out->target_cells > 0)
    {
      gdouble deep = (gdouble) out->deep_cells / out->target_cells;
      gdouble wscore = sum_w / out->target_cells;
      guint edge_strips = graph->n_strips > main_count ? graph->n_strips - main_count : 0;
      gdouble w_term = MIN (1.0, wscore / 0.70);
      gdouble s_term = MIN (1.0, (gdouble) main_count / (gdouble) MAX (min_strips, 1u));
      gdouble t_term = MIN (1.0, (gdouble) graph->n_strips / 12.0);
      gdouble d_term = MIN (1.0, deep / 0.42);
      gdouble edge_bonus = MIN (0.12, edge_strips * 0.03);
      gdouble pct = 100.0 * MIN (1.0, 0.50 * w_term + 0.25 * s_term + 0.15 * d_term + 0.10 * t_term + edge_bonus);

      out->weighted_score = wscore;
      out->complete =
        (main_count >= min_strips && wscore >= 0.70 && deep >= 0.42) ||
        (graph->n_strips >= 10 && main_count >= 6 && wscore >= 0.60) ||
        (graph->n_strips >= 16);

      if (out->complete)
        out->progress_pct = 100;
      else
        out->progress_pct = (guint) MIN (99.0, MAX (0.0, round (pct)));
    }

  if (miss_w_sum > 1e-3)
    {
      out->hint_dx = (gfloat) (miss_x / miss_w_sum);
      out->hint_dy = (gfloat) (miss_y / miss_w_sum);
    }
}

gdouble
elanpress_enroll_map_preview_gain (const ElanpressEnrollMap *cur,
                                   const guint8             *cov_img,
                                   ElanpressPose             global_pose,
                                   int                       frame_w,
                                   int                       frame_h)
{
  ElanpressPose inv_p = elanpress_pose_invert (global_pose);
  gfloat ox = frame_w * 0.5f;
  gfloat oy = frame_h * 0.5f;
  gdouble delta = 0.0;

  if (!cur || !cov_img || cur->target_cells == 0)
    return 1.0;

  for (int r = 0; r < ELANPRESS_MAP_ROWS; r++)
    {
      gfloat wy = oy + (((gfloat) r + 0.5f) - ELANPRESS_MAP_ROWS * 0.5f) * ELANPRESS_MAP_CELL_PX;
      gfloat ny = (((gfloat) r + 0.5f) - ELANPRESS_MAP_ROWS * 0.5f) / ELANPRESS_MAP_TARGET_RY;
      for (int c = 0; c < ELANPRESS_MAP_COLS; c++)
        {
          gfloat nx = (((gfloat) c + 0.5f) - ELANPRESS_MAP_COLS * 0.5f) / ELANPRESS_MAP_TARGET_RX;
          if (nx * nx + ny * ny > 1.0f)
            continue;
          int idx = r * ELANPRESS_MAP_COLS + c;
          guint8 d = cur->grid[idx];
          if (d >= 3)
            continue;
          gfloat wx = ox + (((gfloat) c + 0.5f) - ELANPRESS_MAP_COLS * 0.5f) * ELANPRESS_MAP_CELL_PX;
          if (elanpress_strip_covers_world_pt (cov_img, inv_p, wx, wy, frame_w, frame_h))
            delta += elanpress_depth_weight ((guint8) (d + 1)) - elanpress_depth_weight (d);
        }
    }

  return delta / (gdouble) cur->target_cells;
}

void
elanpress_pose_graph_free (ElanpressPoseGraph *graph)
{
  if (!graph)
    return;
  for (guint i = 0; i < graph->n_strips; i++)
    elanpress_features_free (graph->strips[i]);
  g_free (graph->strips);
  g_free (graph->global_pose);
  g_free (graph->comp_id);
  g_free (graph);
}

/* Privacy-safe spatial enrollment telemetry sideband for graphical setup wizards.
 * Emits ONLY coarse 24x16 cell counts (10x10 px blocks), strip pose coordinates,
 * and coaching hints — strictly ZERO raw ridge pixels, keypoints, or descriptors. */
void
elanpress_enroll_telemetry_emit (const char               *sensor_id,
                                 int                       frame_w,
                                 int                       frame_h,
                                 const char               *status,
                                 int                       stage,
                                 int                       total_stages,
                                 guint                     attempts,
                                 gdouble                   quality_sd,
                                 gdouble                   mean_delta,
                                 const ElanpressPoseGraph *graph,
                                 const ElanpressEnrollMap *map)
{
  static guint seq = 0;
  const char *env_path = g_getenv ("PRESSMATCH_ENROLL_MAP_PATH");
  const char *path = (env_path && env_path[0] != '\0')
                       ? env_path
                       : "/run/pressmatch/enroll-map.json";
  g_autofree gchar *dir = g_path_get_dirname (path);

  if (!g_file_test (dir, G_FILE_TEST_IS_DIR))
    return;

  seq++;
  gint64 ts_ms = g_get_real_time () / 1000;
  g_autoptr(GString) js = g_string_sized_new (2048);
  const char *sid = sensor_id ? sensor_id : "unknown";
  const char *st = status ? status : "unknown";

  guint n_strips = graph ? graph->n_strips : 0;
  guint stitched = map ? map->stitched_strips : (graph ? graph->largest_nodes : 0);
  guint comps = graph ? graph->n_components : 0;
  gdouble span_w = graph ? (gdouble) graph->span_w : 0.0;
  gdouble span_h = graph ? (gdouble) graph->span_h : 0.0;
  guint pct = map ? map->progress_pct : 0;
  gboolean complete = map ? map->complete : FALSE;
  guint cov_cells = map ? map->covered_cells : 0;
  guint deep_cells = map ? map->deep_cells : 0;
  guint tgt_cells = map ? map->target_cells : 0;
  gdouble hdx = map ? (gdouble) map->hint_dx : 0.0;
  gdouble hdy = map ? (gdouble) map->hint_dy : 0.0;
  gboolean last_main = map ? map->last_in_main : TRUE;

  g_string_append_printf (
    js,
    "{\"seq\":%u,\"ts_ms\":%" G_GINT64_FORMAT ",\"sensor\":\"%s\","
    "\"width\":%d,\"height\":%d,\"status\":\"%s\","
    "\"stage\":%d,\"total_stages\":%d,\"attempts\":%u,"
    "\"progress_pct\":%u,\"complete\":%s,"
    "\"stitched\":%u,\"strips\":%u,\"components\":%u,"
    "\"span_w\":%.1f,\"span_h\":%.1f,"
    "\"covered_cells\":%u,\"deep_cells\":%u,\"target_cells\":%u,"
    "\"hint_dx\":%.3f,\"hint_dy\":%.3f,\"last_in_main\":%s,"
    "\"quality_sd\":%.1f,\"mean_delta\":%.1f,"
    "\"cols\":%d,\"rows\":%d,\"grid\":[",
    seq, ts_ms, sid, frame_w, frame_h, st,
    stage, total_stages, attempts,
    pct, complete ? "true" : "false",
    stitched, n_strips, comps,
    span_w, span_h,
    cov_cells, deep_cells, tgt_cells,
    hdx, hdy, last_main ? "true" : "false",
    quality_sd, mean_delta,
    ELANPRESS_MAP_COLS, ELANPRESS_MAP_ROWS);

  for (int i = 0; i < ELANPRESS_MAP_ROWS * ELANPRESS_MAP_COLS; i++)
    {
      if (i > 0)
        g_string_append_c (js, ',');
      g_string_append_printf (js, "%u", map ? (guint) map->grid[i] : 0u);
    }

  g_string_append (js, "],\"last_mask\":[");
  for (int i = 0; i < ELANPRESS_MAP_ROWS * ELANPRESS_MAP_COLS; i++)
    {
      if (i > 0)
        g_string_append_c (js, ',');
      g_string_append_printf (js, "%u", map ? (guint) map->last_mask[i] : 0u);
    }

  g_string_append (js, "],\"poses\":[");
  if (graph && graph->global_pose && graph->comp_id)
    {
      for (guint i = 0; i < graph->n_strips; i++)
        {
          ElanpressPose p = graph->global_pose[i];
          gdouble cx = (gdouble) frame_w * 0.5;
          gdouble cy = (gdouble) frame_h * 0.5;
          gdouble wx = p.c * cx - p.s * cy + p.tx - cx;
          gdouble wy = p.s * cx + p.c * cy + p.ty - cy;
          gdouble deg = atan2 ((gdouble) p.s, (gdouble) p.c) * (180.0 / G_PI);
          if (i > 0)
            g_string_append_c (js, ',');
          g_string_append_printf (js, "{\"x\":%.1f,\"y\":%.1f,\"deg\":%.1f,\"comp\":%u}",
                                  wx, wy, deg, graph->comp_id[i]);
        }
    }
  g_string_append (js, "]}\n");

  if (g_file_set_contents (path, js->str, (gssize) js->len, NULL))
    (void) chmod (path, 0644);
}
