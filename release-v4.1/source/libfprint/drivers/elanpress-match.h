/*
 * Image processing and correlation matching for ELAN press-type sensors
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

#pragma once

#include <glib.h>

#include "elanpress-sift.h"

/* frames dropped at the start (finger settling) and end (possible lift)
 * of a touch when enough frames are available */
#define ELANPRESS_SKIP_OLDEST 2
#define ELANPRESS_SKIP_NEWEST 1

/* A press is matched against an enrolled image by pairing up keypoints whose
 * descriptors are each other's clear nearest neighbour, then counting how
 * many of those pairs agree on a single rigid placement of one image on the
 * other. Unrelated fingers still pair up a handful of keypoints by chance,
 * but their placements disagree, so they reach only a few such pairs.
 *
 * Measured on labelled presses from the reference sensor (40 genuine, 30
 * from five other fingers, against 8 to 12 enrolled images): the other
 * fingers reached at most 4 pairs, while 97-99% of genuine presses reached 12
 * or more and half of them over 60. Correlating whole images instead (the
 * previous matcher) left the two overlapping on the same data: at 0.68 it
 * rejected about 1 genuine press in 10 and still accepted the odd impostor. */
#define ELANPRESS_MATCH_MIN_PAIRS 12

/* the nearest descriptor must be closer than this fraction of the distance
 * to the second nearest to count as a pair (Lowe's ratio test) */
#define ELANPRESS_MATCH_RATIO 0.8

/* how far a keypoint may land from its partner under the placement, in
 * pixels: the first value gathers support for a placement proposed by a
 * single pair, the second decides the final count after refitting it */
#define ELANPRESS_MATCH_LOOSE_PX 6.0
#define ELANPRESS_MATCH_TOL_PX 3.0

/* proposed placements refined by least squares, best supported first */
#define ELANPRESS_MATCH_REFINED 8

/* keypoints kept per image, strongest first */
#define ELANPRESS_MAX_KEYPOINTS 300

/* Presence is read out of the image, because the status byte cmd_pre_scan
 * returns answers once per power-up and then wedges at "finger present".
 *
 * A pixel counts as covered when it sits this far above the background, and
 * the frame counts as touched once that many of them are. Counting covered
 * pixels rather than averaging the delta over the whole sensor is what keeps
 * a lightly resting finger from being diluted by the untouched area around
 * it: a finger landing on a third of the pad drives those pixels hard but
 * moves the whole-sensor mean very little, so a mean test only notices once
 * the finger is pressed hard or slid across to cover more of the pad. */
#define ELANPRESS_TOUCH_PIXEL_DELTA 1200
#define ELANPRESS_TOUCH_MIN_COVERAGE 0.12

/* what a frame looked like against the background, for tuning the two
 * constants above from fp_dbg output */
typedef struct
{
  gdouble mean_delta;
  gdouble coverage;
} ElanpressTouchStats;

/* a processed touch whose contrast (pixel std-dev) falls below this carries
 * too little ridge detail to enroll or to match against: a light or partial
 * press correlates poorly against everything, genuine or not. A well-imaged
 * press lands around 30-50. Rejecting these is what makes a bad press fail
 * rather than score like a stranger's finger. */
#define ELANPRESS_MIN_QUALITY_STDDEV 10.0

typedef struct
{
  guint n_pairs_base;
  guint n_rej_same_loc;
  guint loose_base;
  guint score_base;
  guint n_pairs_recip_nn;
  guint score_recip_nn;
  guint n_pairs_recip_ratio;
  guint score_recip_ratio;
} ElanpressMatchDiag;

typedef struct
{
  gdouble coverage;
  gdouble mean_delta;
  guint16 bg_mean;
  guint16 p0;
  guint16 p30;
  guint16 p65;
  guint16 p99;
  guint16 p100;
  gdouble pos_pct;
  gdouble norm_mean;
  gdouble norm_stddev;
  gdouble zero_pct;
} ElanpressFrameDiag;

typedef struct
{
  gfloat c, s, tx, ty;
} ElanpressPose;

typedef struct
{
  ElanpressFeatures **components;
  guint               n_components;
  guint               largest_nodes;
  guint               total_nodes;
  guint               total_keypoints;
  gfloat              span_w;
  gfloat              span_h;
} ElanpressMosaic;

void     elanpress_rotate_frame (const guint8 *raw, unsigned short *out,
                                 int w, int h);
guint8 * elanpress_process_frames (GSList *frames, int num_frames,
                                   const unsigned short *background,
                                   unsigned int size);
guint8 * elanpress_process_frame_covered (const unsigned short *raw_frame,
                                          const unsigned short *background,
                                          unsigned int          size);
ElanpressFeatures * elanpress_image_features (const guint8 *img, int w, int h);
guint    elanpress_match_features (const ElanpressFeatures *probe,
                                   const ElanpressFeatures *enrolled);
guint    elanpress_match_features_pose (const ElanpressFeatures *probe,
                                        const ElanpressFeatures *enrolled,
                                        ElanpressPose           *best_pose,
                                        gboolean                 scale_gate);
guint    elanpress_match_features_diag (const ElanpressFeatures *probe,
                                        const ElanpressFeatures *enrolled,
                                        ElanpressMatchDiag      *diag);
typedef struct
{
  ElanpressFeatures **strips;
  ElanpressPose      *global_pose;
  guint              *comp_id;
  guint               n_strips;
  guint               n_components;
  guint               largest_nodes;
  gfloat              span_w;
  gfloat              span_h;
} ElanpressPoseGraph;

ElanpressMosaic *elanpress_mosaic_build (ElanpressFeatures * const *feats,
                                         guint                      n_images,
                                         int                        frame_w,
                                         int                        frame_h,
                                         guint                      min_stitch_pairs);
guint    elanpress_mosaic_match (const ElanpressFeatures *probe,
                                 const ElanpressMosaic   *mosaic,
                                 guint                   *best_comp_out);
void     elanpress_mosaic_free (ElanpressMosaic *mosaic);
ElanpressPoseGraph *elanpress_pose_graph_build (ElanpressFeatures * const *feats,
                                                guint                      n_images,
                                                int                        frame_w,
                                                int                        frame_h,
                                                guint                      min_stitch_pairs);
ElanpressPoseGraph *elanpress_pose_graph_build_verified (ElanpressFeatures * const *feats,
                                                         GPtrArray                 *cov_images,
                                                         guint                      n_images,
                                                         int                        frame_w,
                                                         int                        frame_h,
                                                         guint                      min_stitch_pairs);
guint    elanpress_pose_graph_match (const ElanpressFeatures  *probe,
                                     const ElanpressPoseGraph *graph,
                                     gboolean                  recip_nn,
                                     guint                    *best_comp_out);
guint    elanpress_pose_graph_match_pose (const ElanpressFeatures  *probe,
                                          const ElanpressPoseGraph *graph,
                                          gboolean                  recip_nn,
                                          guint                    *best_comp_out,
                                          ElanpressPose            *best_pose_out);
gdouble  elanpress_pose_graph_zncc (const guint8             *probe_img,
                                    GPtrArray                *enrolled_images,
                                    guint                     stride,
                                    guint                     offset,
                                    const ElanpressPoseGraph *graph,
                                    guint                     comp_id,
                                    ElanpressPose             global_pose,
                                    int                       w,
                                    int                       h,
                                    guint                    *overlap_px_out);
gdouble  elanpress_pose_graph_zncc_elastic (const guint8             *probe_img,
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
                                            guint                    *overlap_px_out);
/* Coarse 2D fingertip coverage heatmap (10x10 px per cell, zero ridge detail) */
#define ELANPRESS_MAP_COLS 24
#define ELANPRESS_MAP_ROWS 16
#define ELANPRESS_MAP_CELL_PX 10.0f
#define ELANPRESS_MAP_TARGET_RX 10.0f
#define ELANPRESS_MAP_TARGET_RY 6.2f

typedef struct
{
  guint8   grid[ELANPRESS_MAP_ROWS * ELANPRESS_MAP_COLS];
  guint8   last_mask[ELANPRESS_MAP_ROWS * ELANPRESS_MAP_COLS];
  guint    target_cells;
  guint    covered_cells;
  guint    deep_cells;
  guint    core_cells;
  guint    stitched_strips;
  guint    total_strips;
  gdouble  weighted_score;
  guint    progress_pct;
  gboolean complete;
  gboolean last_in_main;
  gfloat   hint_dx;
  gfloat   hint_dy;
} ElanpressEnrollMap;

GBytes             *elanpress_pose_graph_serialize (const ElanpressPoseGraph *graph);
ElanpressPoseGraph *elanpress_pose_graph_deserialize (const guint8 *data,
                                                      gsize         len);
guint8             *elanpress_pack_image_4bit (const guint8 *img, unsigned int size, gsize *out_len);
guint8             *elanpress_unpack_image_4bit (const guint8 *packed, gsize packed_len, unsigned int size);
ElanpressPoseGraph *elanpress_pose_graph_new_single (const ElanpressFeatures *feat,
                                                     int                      frame_w,
                                                     int                      frame_h);
void     elanpress_pose_graph_append (ElanpressPoseGraph      *graph,
                                      const ElanpressFeatures *feat,
                                      guint                    comp_id,
                                      ElanpressPose            global_pose,
                                      int                      frame_w,
                                      int                      frame_h);
void     elanpress_enroll_map_compute (GPtrArray                *cov_images,
                                       const ElanpressPoseGraph *graph,
                                       guint                     last_strip_idx,
                                       int                       frame_w,
                                       int                       frame_h,
                                       guint                     min_strips,
                                       ElanpressEnrollMap       *out);
gdouble  elanpress_enroll_map_preview_gain (const ElanpressEnrollMap *cur,
                                            const guint8             *cov_img,
                                            ElanpressPose             global_pose,
                                            int                       frame_w,
                                            int                       frame_h);
void     elanpress_pose_graph_free (ElanpressPoseGraph *graph);
void     elanpress_enroll_telemetry_emit (const char               *sensor_id,
                                          int                       frame_w,
                                          int                       frame_h,
                                          const char               *status,
                                          int                       stage,
                                          int                       total_stages,
                                          guint                     attempts,
                                          gdouble                   quality_sd,
                                          gdouble                   mean_delta,
                                          const ElanpressPoseGraph *graph,
                                          const ElanpressEnrollMap *map);
void     elanpress_frame_diag (const unsigned short *raw_frame,
                               const unsigned short *background,
                               const guint8         *norm_img,
                               unsigned int          size,
                               ElanpressFrameDiag   *out);
gboolean elanpress_frame_has_touch (const unsigned short *frame,
                                    const unsigned short *background,
                                    unsigned int size,
                                    ElanpressTouchStats *stats);
gdouble  elanpress_image_quality (const guint8 *img, unsigned int size);
