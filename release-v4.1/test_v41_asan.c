/*
 * ASan + UBSan unit, elastic-ZNCC, 96x96/150x52 multi-geometry, and fuzz test suite
 * for v4.1 pressmatch core (elanpress-match.c + elanpress-sift.c).
 */
#include <assert.h>
#include <glib.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "elanpress-match.h"
#include "elanpress-sift.h"

static guint8 *
make_synth_ridges (int w, int h, float shift_x, float shift_y, float angle_rad,
                   float elastic_amp, float freq_scale, float phase)
{
  guint8 *img = g_malloc ((gsize) w * h);
  float c = cosf (angle_rad), s = sinf (angle_rad);
  float cx = w * 0.5f, cy = h * 0.5f;

  for (int y = 0; y < h; y++)
    {
      for (int x = 0; x < w; x++)
        {
          float dx = x - cx, dy = y - cy;
          float rx = c * dx + s * dy + cx + shift_x;
          float ry = -s * dx + c * dy + cy + shift_y;
          /* Non-linear elastic skin stretch */
          rx += elastic_amp * sinf (ry * 0.09f);
          ry += elastic_amp * cosf (rx * 0.07f);
          /* Curved ridge field with local bifurcation-like phase modulation */
          float r = sqrtf ((rx - 35.0f) * (rx - 35.0f) + (ry - 25.0f) * (ry - 25.0f));
          float wv = sinf (freq_scale * 0.72f * r + phase + 1.8f * sinf (rx * 0.06f) * cosf (ry * 0.08f));
          /* Add realistic micro-pores / ridge-endings for rich SIFT keypoints */
          float pores = 0.25f * sinf (rx * 0.41f + ry * 0.29f) * cosf (rx * 0.23f - ry * 0.37f);
          float v = 128.0f + 95.0f * wv + 28.0f * pores;
          int iv = (int) lroundf (v);
          img[y * w + x] = (guint8) CLAMP (iv, 1, 255);
        }
    }
  return img;
}

static void
test_geometry (const char *name, int w, int h)
{
  const ElanpressSiftParams p = { .contrast_thr = 0.0f, .edge_ratio = 25.0f, .rootsift = TRUE };
  GPtrArray *imgs = g_ptr_array_new_full (4, g_free);
  ElanpressFeatures *feats[4];

  for (int k = 0; k < 4; k++)
    {
      guint8 *im = make_synth_ridges (w, h, (k - 1.5f) * 6.0f, (k - 1.5f) * 3.0f,
                                      (k - 1.5f) * 0.04f, 0.0f, 1.0f, 0.0f);
      feats[k] = elanpress_sift_features_opts (im, w, h, 200, &p, NULL);
      assert (feats[k] && feats[k]->n >= 20);
      g_ptr_array_add (imgs, im);
    }

  ElanpressPoseGraph *pg = elanpress_pose_graph_build_verified (feats, imgs, 4, w, h, 8);
  assert (pg != NULL && pg->largest_nodes >= 2);

  /* Serialize -> deserialize roundtrip + 4-bit pack/unpack roundtrip */
  g_autoptr(GBytes) ser = elanpress_pose_graph_serialize (pg);
  assert (ser != NULL);
  gsize slen = 0;
  const guint8 *sptr = g_bytes_get_data (ser, &slen);
  ElanpressPoseGraph *pg2 = elanpress_pose_graph_deserialize (sptr, slen);
  assert (pg2 != NULL && pg2->n_strips == 4 && pg2->largest_nodes == pg->largest_nodes);

  GPtrArray *imgs4 = g_ptr_array_new_full (4, g_free);
  for (int k = 0; k < 4; k++)
    {
      gsize plen = 0;
      g_autofree guint8 *pk = elanpress_pack_image_4bit (g_ptr_array_index (imgs, k),
                                                         (unsigned int) (w * h), &plen);
      assert (pk && plen == (gsize) ((w * h + 1) / 2));
      guint8 *up = elanpress_unpack_image_4bit (pk, plen, (unsigned int) (w * h));
      assert (up != NULL);
      g_ptr_array_add (imgs4, up);
    }

  /* Genuine probe with 1.2 px elastic distortion */
  g_autofree guint8 *probe_gen = make_synth_ridges (w, h, 2.0f, -1.5f, 0.03f, 1.2f, 1.0f, 0.0f);
  g_autoptr(ElanpressFeatures) fgen = elanpress_sift_features_opts (probe_gen, w, h, 300, &p, NULL);
  guint comp = 0, ov = 0;
  ElanpressPose pose = { 1, 0, 0, 0 };
  gint64 t0 = g_get_monotonic_time ();
  guint pairs = elanpress_pose_graph_match_pose (fgen, pg2, TRUE, &comp, &pose);
  gdouble zr = 0.0;
  gdouble ze = elanpress_pose_graph_zncc_elastic (probe_gen, imgs4, 1, 0, pg2, comp, pose,
                                                  w, h, 1, &zr, &ov);
  gdouble ms = (g_get_monotonic_time () - t0) / 1000.0;

  printf ("  [%s %dx%d] genuine(elastic=1.2px): pairs=%u rigid_zncc=%.3f elastic_zncc=%.3f ov=%u (%.2f ms)\n",
          name, w, h, pairs, zr, ze, ov, ms);
  assert (pairs >= 10);
  assert (ze >= zr);
  assert (ze >= 0.70);

  /* Impostor probe (completely different ridge frequency & phase) */
  g_autofree guint8 *probe_imp = make_synth_ridges (w, h, -15.0f, 12.0f, 0.95f, 0.0f, 1.43f, 2.1f);
  g_autoptr(ElanpressFeatures) fimp = elanpress_sift_features_opts (probe_imp, w, h, 300, &p, NULL);
  guint imp_comp = 0, imp_ov = 0;
  ElanpressPose imp_pose = { 1, 0, 0, 0 };
  guint imp_pairs = elanpress_pose_graph_match_pose (fimp, pg2, TRUE, &imp_comp, &imp_pose);
  gdouble imp_zr = 0.0, imp_ze = 0.0;
  if (imp_pairs > 0)
    imp_ze = elanpress_pose_graph_zncc_elastic (probe_imp, imgs4, 1, 0, pg2, imp_comp, imp_pose,
                                                w, h, 1, &imp_zr, &imp_ov);
  printf ("  [%s %dx%d] impostor: pairs=%u rigid_zncc=%.3f elastic_zncc=%.3f ov=%u\n",
          name, w, h, imp_pairs, imp_zr, imp_ze, imp_ov);
  assert (imp_pairs < 10 && imp_ze < 0.60);

  /* Fuzz corrupted deserialization (truncated + bit-flipped) */
  for (gsize trunc = 0; trunc < MIN (slen, (gsize) 64); trunc++)
    {
      ElanpressPoseGraph *bad = elanpress_pose_graph_deserialize (sptr, trunc);
      assert (bad == NULL);
    }
  g_autofree guint8 *corrupt = g_memdup2 (sptr, slen);
  corrupt[0] ^= 0xFF;
  assert (elanpress_pose_graph_deserialize (corrupt, slen) == NULL);

  elanpress_pose_graph_free (pg);
  elanpress_pose_graph_free (pg2);
  for (int k = 0; k < 4; k++)
    elanpress_features_free (feats[k]);
  g_ptr_array_unref (imgs);
  g_ptr_array_unref (imgs4);
}

int
main (void)
{
  printf ("Running v4.1 pressmatch ASan+UBSan test suite...\n");
  test_geometry ("ELAN-strip", 150, 52);
  test_geometry ("FT9201-square", 96, 96);
  printf ("ALL v4.1 ASAN+UBSAN TESTS PASSED!\n");
  return 0;
}
