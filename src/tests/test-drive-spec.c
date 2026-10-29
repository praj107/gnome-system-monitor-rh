/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The endurance table and the wear range. Every figure pinned here is the
 * one the maker's own document prints (the row's URL names it); the model
 * strings are the ones the drives report, and the two sizes of "1 TB" are
 * the byte counts the drives of this workstation report.
 */

#include <glib.h>
#include <string.h>

#include "gsm-drive-spec.h"

#define SIZE_1TB_DECIMAL 1000204886016ULL   /* e.g. a SATA "1 TB" drive */
#define SIZE_1TB_BINARY  1024209543168ULL   /* the NVMe WD PC SN740     */

#define TB(n) ((uint64_t) (n) * GSM_TB)


static void
check_rating (const char *model,
              uint64_t    size_bytes,
              uint32_t    expected_tbw)
{
  const GsmDriveSpecRow *row = gsm_drive_spec_lookup (model, size_bytes);

  g_assert_nonnull (row);
  g_assert_cmpuint (row->rated_tbw, ==, expected_tbw);
  g_assert_cmpuint (gsm_drive_spec_rated_tbw (model, size_bytes), ==, TB (expected_tbw));
}


static void
test_every_row_has_a_source_and_finds_itself (void)
{
  size_t rows = gsm_drive_spec_row_count ();

  g_assert_cmpuint (rows, >, 60);

  for (size_t i = 0; i < rows; i++) {
    const GsmDriveSpecRow *row = gsm_drive_spec_row (i);
    uint64_t nominal_bytes = (uint64_t) row->capacity_gb * 1000 * 1000 * 1000;

    g_assert_true (g_str_has_prefix (row->source_url, "https://"));
    g_assert_cmpuint (strlen (row->source_label), >, 5);
    g_assert_cmpuint (row->rated_tbw, >, 0);

    /* Looked up by its own key at its own nominal size, a row is found and
     * is the only one found: no two keys catch each other. */
    g_assert_true (gsm_drive_spec_lookup (row->model_prefix, nominal_bytes) == row);
    for (size_t j = 0; j < rows; j++) {
      if (j != i)
        g_assert_cmpstr (gsm_drive_spec_row (j)->model_prefix, !=, row->model_prefix);
    }
  }

  g_assert_null (gsm_drive_spec_row (rows));
}


static void
test_samsung_ratings_are_not_linear_in_capacity (void)
{
  check_rating ("Samsung SSD 980 PRO 250GB", 250059350016ULL, 150);
  check_rating ("Samsung SSD 980 PRO 500GB", 500107862016ULL, 300);
  check_rating ("Samsung SSD 980 PRO 1TB", SIZE_1TB_DECIMAL, 600);
  check_rating ("Samsung SSD 980 PRO 2TB", 2000398934016ULL, 1200);
  /* The plain 980 is rated at twice what a per-TB table of 300 gave. */
  check_rating ("Samsung SSD 980 500GB", 500107862016ULL, 300);
  check_rating ("Samsung SSD 980 1TB", SIZE_1TB_DECIMAL, 600);
  check_rating ("Samsung SSD 990 PRO 1TB", SIZE_1TB_DECIMAL, 600);
  check_rating ("Samsung SSD 990 PRO 2TB", 2000398934016ULL, 1200);
  check_rating ("Samsung SSD 970 EVO Plus 500GB", 500107862016ULL, 300);
  check_rating ("Samsung SSD 970 EVO 1TB", SIZE_1TB_DECIMAL, 600);
  check_rating ("Samsung SSD 970 PRO 512GB", 512110190592ULL, 600);
  check_rating ("Samsung SSD 970 PRO 1TB", SIZE_1TB_DECIMAL, 1200);
  check_rating ("Samsung SSD 870 EVO 4TB", 4000787030016ULL, 2400);
  check_rating ("Samsung SSD 860 EVO 500GB", 500107862016ULL, 300);
  /* The 850 EVO steps 75, 75, 150, 150, 300, 300 across its capacities. */
  check_rating ("Samsung SSD 850 EVO 120GB", 120034123776ULL, 75);
  check_rating ("Samsung SSD 850 EVO 250GB", 250059350016ULL, 75);
  check_rating ("Samsung SSD 850 EVO 500GB", 500107862016ULL, 150);
  check_rating ("Samsung SSD 850 EVO 2TB", 2000398934016ULL, 300);
}


static void
test_samsung_look_alikes_are_not_caught (void)
{
  /* 990 PRO 4 TB: the data sheet read lists 1 and 2 TB only. */
  g_assert_null (gsm_drive_spec_lookup ("Samsung SSD 990 PRO 4TB", 4000787030016ULL));
  /* Form factors whose sheets were not read. */
  g_assert_null (gsm_drive_spec_lookup ("Samsung SSD 860 EVO M.2 1TB", SIZE_1TB_DECIMAL));
  g_assert_null (gsm_drive_spec_lookup ("Samsung SSD 850 EVO M.2 250GB", 250059350016ULL));
  /* A longer model name never falls into a shorter row. */
  g_assert_null (gsm_drive_spec_lookup ("Samsung SSD 980 PRO with Heatsink 2TB", 2000398934016ULL));
  g_assert_null (gsm_drive_spec_lookup ("Samsung SSD 9800 1TB", SIZE_1TB_DECIMAL));
}


static void
test_wd_ratings (void)
{
  check_rating ("WD_BLACK SN850X 1000GB", SIZE_1TB_DECIMAL, 600);
  check_rating ("WD_BLACK SN850X 2000GB", 2000398934016ULL, 1200);
  check_rating ("WD_BLACK SN770 250GB", 250059350016ULL, 200);
  check_rating ("WD_BLACK SN770 2TB", 2000398934016ULL, 1200);
  check_rating ("WD Blue SN570 250GB", 250059350016ULL, 150);
  check_rating ("WD Blue SN570 1TB", SIZE_1TB_DECIMAL, 600);
  check_rating ("WD Blue SN570 2TB", 2000398934016ULL, 900);
  check_rating ("WD PC SN740 SDDPNQD-256G-1001", 256060514304ULL, 200);
  check_rating ("WD PC SN740 SDDPNQD-512G-1001", 512110190592ULL, 300);
  check_rating ("WD PC SN740 SDDQNQD-1T00", SIZE_1TB_DECIMAL, 400);
  check_rating ("WD PC SN740 SDDQNQE-2T00", 2048408248320ULL, 500);
  check_rating ("WD PC SN730 SDBQNTY-256G-1001", 256060514304ULL, 200);
  check_rating ("WD PC SN730 SDBPNTY-512G", 512110190592ULL, 300);
  check_rating ("WD PC SN730 SDBPNTY-1T00", SIZE_1TB_DECIMAL, 400);
}


static void
test_the_wd_pc_sn740_1tb_is_rated_400_tbw_by_its_product_brief (void)
{
  /* This workstation's NVMe drive, as udev reports it, at both byte counts
   * a "1 TB" drive is seen with. */
  const char *model = "WD PC SN740 SDDQNQD-1T00-1014";
  const uint64_t sizes[] = { SIZE_1TB_BINARY, SIZE_1TB_DECIMAL };

  for (gsize i = 0; i < G_N_ELEMENTS (sizes); i++) {
    const GsmDriveSpecRow *row = gsm_drive_spec_lookup (model, sizes[i]);

    g_assert_nonnull (row);
    g_assert_cmpuint (row->rated_tbw, ==, 400);
    g_assert_cmpstr (row->source_label, ==, "PC SN740 product brief");
    g_assert_true (g_str_has_prefix (row->source_url, "https://documents.sandisk.com/"));
  }

  /* The same model string on a drive of another size is another row's job. */
  g_assert_null (gsm_drive_spec_lookup (model, 2048408248320ULL));
  /* The old per-TB table gave 800 TB for the 2 TB drive. */
  check_rating ("WD PC SN740 SDDPNQE-2T00", 2048408248320ULL, 500);
}


static void
test_crucial_ratings_are_absolute_figures (void)
{
  check_rating ("CT500MX500SSD1", 500107862016ULL, 180);
  check_rating ("CT1000MX500SSD1", SIZE_1TB_DECIMAL, 360);
  check_rating ("CT2000MX500SSD1", 2000398934016ULL, 700);
  check_rating ("CT250MX500SSD1", 250059350016ULL, 100);
  check_rating ("CT500P3SSD8", 500107862016ULL, 110);
  check_rating ("CT1000P3SSD8", SIZE_1TB_DECIMAL, 220);
  check_rating ("CT2000P3SSD8", 2000398934016ULL, 440);
  check_rating ("CT4000P3SSD8", 4000787030016ULL, 800);
  check_rating ("CT1000P3PSSD8", SIZE_1TB_DECIMAL, 220);
  check_rating ("CT2000P3PSSD8", 2000398934016ULL, 440);
  check_rating ("CT4000P3PSSD8", 4000787030016ULL, 800);
}


static void
test_the_crucial_x9_has_no_published_rating (void)
{
  /* This workstation's SATA SSD. */
  g_assert_null (gsm_drive_spec_lookup ("CT1000X9SSD9", SIZE_1TB_DECIMAL));
  g_assert_null (gsm_drive_spec_lookup ("CT1000X9SSD9    ", SIZE_1TB_DECIMAL));
  g_assert_cmpuint (gsm_drive_spec_rated_tbw ("CT1000X9SSD9", SIZE_1TB_DECIMAL), ==, 0);
  /* Other X9 capacities and unlisted MX500 SKUs are no different. */
  g_assert_null (gsm_drive_spec_lookup ("CT2000X9SSD9", 2000398934016ULL));
  g_assert_null (gsm_drive_spec_lookup ("CT500MX500SSD4", 500107862016ULL));
}


static void
test_kingston_ratings (void)
{
  check_rating ("KINGSTON SA2000M8250G", 250059350016ULL, 150);
  check_rating ("KINGSTON SA2000M8500G", 500107862016ULL, 350);
  check_rating ("KINGSTON SA2000M81000G", SIZE_1TB_DECIMAL, 600);
  check_rating ("KINGSTON SNV2S250G", 250059350016ULL, 80);
  check_rating ("KINGSTON SNV2S500G", 500107862016ULL, 160);
  check_rating ("KINGSTON SNV2S1000G", SIZE_1TB_DECIMAL, 320);
  check_rating ("KINGSTON SNV2S2000G", 2000398934016ULL, 640);
  check_rating ("KINGSTON SNV2S4000G", 4000787030016ULL, 1280);
}


static void
test_intel_solidigm_and_skhynix_ratings (void)
{
  check_rating ("INTEL SSDPEKNW512G8", 512110190592ULL, 100);
  check_rating ("INTEL SSDPEKNW010T8", SIZE_1TB_DECIMAL, 200);
  check_rating ("INTEL SSDPEKNW020T8", 2048408248320ULL, 400);
  check_rating ("INTEL SSDPEKNU512GZ", 512110190592ULL, 185);
  check_rating ("INTEL SSDPEKNU010TZ", SIZE_1TB_DECIMAL, 370);
  check_rating ("INTEL SSDPEKNU020TZ", 2048408248320ULL, 740);
  check_rating ("SOLIDIGM SSDPFKNU512GZ", 512110190592ULL, 200);
  check_rating ("SOLIDIGM SSDPFKNU010TZ", SIZE_1TB_DECIMAL, 400);
  check_rating ("SOLIDIGM SSDPFKNU020TZ", 2048408248320ULL, 800);
  check_rating ("SHPP41-2000GM", 2000398934016ULL, 1200);
}


static void
test_models_without_a_published_rating_say_not_published (void)
{
  /* The vendor-wide keys the table used to carry. */
  g_assert_null (gsm_drive_spec_lookup ("WDC WDS100T2B0A-00SM50", SIZE_1TB_DECIMAL));
  g_assert_null (gsm_drive_spec_lookup ("WD_BLACK SN850 1TB", SIZE_1TB_DECIMAL));
  g_assert_null (gsm_drive_spec_lookup ("WD_BLACK SN850X 4000GB", 4000787030016ULL));
  g_assert_null (gsm_drive_spec_lookup ("SKHynix_HFS001TEJ9X115N", SIZE_1TB_DECIMAL));
  g_assert_null (gsm_drive_spec_lookup ("SHPP41-500GM", 500107862016ULL));
  g_assert_null (gsm_drive_spec_lookup ("SOLIDIGM SSDPFKKW010X7", SIZE_1TB_DECIMAL));
  g_assert_null (gsm_drive_spec_lookup ("INTEL SSDPEKNW010T9", SIZE_1TB_DECIMAL));
  g_assert_null (gsm_drive_spec_lookup ("INTEL SSDPEKKW256G7", 256060514304ULL));
  /* Nothing, or a size we cannot class. */
  g_assert_null (gsm_drive_spec_lookup (NULL, SIZE_1TB_DECIMAL));
  g_assert_null (gsm_drive_spec_lookup ("", SIZE_1TB_DECIMAL));
  g_assert_null (gsm_drive_spec_lookup ("Samsung SSD 980 PRO 1TB", 0));
  /* The right name on a drive of the wrong size is not a match. */
  g_assert_null (gsm_drive_spec_lookup ("Samsung SSD 980 PRO 1TB", 500107862016ULL));
}


static void
test_model_matching_ignores_case (void)
{
  check_rating ("samsung ssd 980 pro 1tb", SIZE_1TB_DECIMAL, 600);
  check_rating ("kingston snv2s1000g", SIZE_1TB_DECIMAL, 320);
}


static void
test_12_percent_used_at_217_tb_written_implies_1_67_to_1_81_pb (void)
{
  GsmEnduranceRange range;

  /* This workstation's SN740: 217.1 TB written, 12 % used. */
  g_assert_true (gsm_drive_spec_implied_endurance (217131853824000ULL, 12, &range));
  g_assert_cmpuint (range.low_bytes, ==, 1670245029415384ULL);           /* /0.13 */
  g_assert_cmpuint (range.high_bytes, ==, 1809432115200000ULL);           /* /0.12 */
  g_assert_cmpfloat_with_epsilon ((double) range.low_bytes / 1e15, 1.67, 0.005);
  g_assert_cmpfloat_with_epsilon ((double) range.high_bytes / 1e15, 1.81, 0.005);
}


static void
test_the_wear_range_is_unbounded_without_a_whole_percent (void)
{
  GsmEnduranceRange range;

  g_assert_false (gsm_drive_spec_implied_endurance (217131853824000ULL, 0, &range));
  g_assert_cmpuint (range.high_bytes, ==, 0);
  g_assert_false (gsm_drive_spec_implied_endurance (217131853824000ULL, -1, &range));
  g_assert_false (gsm_drive_spec_implied_endurance (0, 12, &range));

  /* At 1 % the range spans a factor of two: wide, and honest about it. */
  g_assert_true (gsm_drive_spec_implied_endurance (1000, 1, &range));
  g_assert_cmpuint (range.high_bytes, ==, 100000);
  g_assert_cmpuint (range.low_bytes, ==, 50000);

  /* No overflow on absurd inputs. */
  g_assert_true (gsm_drive_spec_implied_endurance (G_MAXUINT64 / 2, 255, &range));
  g_assert_cmpuint (range.low_bytes, <=, range.high_bytes);
}


static GsmDrive *
make_drive (const char *model,
            uint64_t    size_bytes,
            uint64_t    written_bytes,
            int         percent_used)
{
  GsmDrive *drive = gsm_drive_new ("test-drive");
  GsmDriveInfo info = { 0 };

  info.model = g_strdup (model);
  info.size = size_bytes;
  info.bytes_written = written_bytes;
  info.percent_used = percent_used;
  info.present = TRUE;
  info.temperature = -1;
  info.spare_percent = -1;
  info.spare_threshold = -1;
  info.smart_source = GSM_SMART_SOURCE_UDISKS;
  gsm_drive_apply_info (drive, &info);
  gsm_drive_info_clear (&info);

  return drive;
}


static void
test_a_drive_keeps_the_makers_rating_and_its_own_range_side_by_side (void)
{
  g_autoptr (GsmDrive) drive = make_drive ("WD PC SN740 SDDQNQD-1T00-1014",
                                           SIZE_1TB_BINARY, 217131853824000ULL, 12);
  uint64_t low = 0, high = 0;

  gsm_drive_resolve_endurance (drive, 0);

  /* The drive's own wear is what the projection follows ... */
  g_assert_cmpint (gsm_drive_get_endurance_source (drive), ==, GSM_ENDURANCE_DERIVED);
  g_assert_cmpuint (gsm_drive_get_rated_tbw (drive), ==, 1809432115200000ULL);

  /* ... and the maker's rating and the range are both available. */
  g_assert_cmpuint (gsm_drive_get_maker_rated_tbw (drive), ==, TB (400));
  g_assert_cmpstr (gsm_drive_get_maker_source_label (drive), ==, "PC SN740 product brief");
  g_assert_nonnull (gsm_drive_get_maker_source_url (drive));
  g_assert_true (gsm_drive_get_implied_endurance (drive, &low, &high));
  g_assert_cmpuint (low, <, high);
  g_assert_true (gsm_drive_endurance_is_conflicted (drive));
}


static void
test_a_lightly_worn_drive_follows_the_makers_rating (void)
{
  g_autoptr (GsmDrive) drive = make_drive ("Samsung SSD 980 PRO 1TB",
                                           SIZE_1TB_DECIMAL, 5000000000000ULL, 1);

  gsm_drive_resolve_endurance (drive, 0);

  g_assert_cmpint (gsm_drive_get_endurance_source (drive), ==, GSM_ENDURANCE_SPEC_DB);
  g_assert_cmpuint (gsm_drive_get_rated_tbw (drive), ==, TB (600));
  g_assert_cmpuint (gsm_drive_get_maker_rated_tbw (drive), ==, TB (600));
  g_assert_false (gsm_drive_get_implied_endurance (drive, NULL, NULL));
  g_assert_false (gsm_drive_endurance_is_conflicted (drive));
}


static void
test_a_drive_without_a_published_rating_is_unknown_until_it_has_worn (void)
{
  g_autoptr (GsmDrive) drive = make_drive ("CT1000X9SSD9", SIZE_1TB_DECIMAL,
                                           2000000000000ULL, 1);

  gsm_drive_resolve_endurance (drive, 0);
  g_assert_cmpint (gsm_drive_get_endurance_source (drive), ==, GSM_ENDURANCE_UNKNOWN);
  g_assert_cmpuint (gsm_drive_get_rated_tbw (drive), ==, 0);
  g_assert_cmpuint (gsm_drive_get_maker_rated_tbw (drive), ==, 0);
  g_assert_null (gsm_drive_get_maker_source_label (drive));
  g_assert_null (gsm_drive_get_maker_source_url (drive));
  g_assert_false (gsm_drive_endurance_is_conflicted (drive));

  /* Once worn it derives, with nothing from the maker to contradict. */
  {
    g_autoptr (GsmDrive) worn = make_drive ("CT1000X9SSD9", SIZE_1TB_DECIMAL,
                                            50000000000000ULL, 10);

    gsm_drive_resolve_endurance (worn, 0);
    g_assert_cmpint (gsm_drive_get_endurance_source (worn), ==, GSM_ENDURANCE_DERIVED);
    g_assert_cmpuint (gsm_drive_get_rated_tbw (worn), ==, 500000000000000ULL);
    g_assert_false (gsm_drive_endurance_is_conflicted (worn));
  }
}


static void
test_a_user_override_wins_but_both_truths_stay_available (void)
{
  g_autoptr (GsmDrive) drive = make_drive ("WD PC SN740 SDDQNQD-1T00-1014",
                                           SIZE_1TB_BINARY, 217131853824000ULL, 12);

  gsm_drive_resolve_endurance (drive, TB (1000));

  g_assert_cmpint (gsm_drive_get_endurance_source (drive), ==, GSM_ENDURANCE_USER);
  g_assert_cmpuint (gsm_drive_get_rated_tbw (drive), ==, TB (1000));
  g_assert_cmpuint (gsm_drive_get_maker_rated_tbw (drive), ==, TB (400));
  g_assert_true (gsm_drive_get_implied_endurance (drive, NULL, NULL));
}


static void
test_a_rating_inside_the_implied_range_is_not_a_conflict (void)
{
  /* A 980 PRO 1 TB (rated 600 TBW) at 6 % used with 36 TB written implies
   * 514 to 600 TB: the drive and the maker agree. */
  g_autoptr (GsmDrive) drive = make_drive ("Samsung SSD 980 PRO 1TB",
                                           SIZE_1TB_DECIMAL, 36000000000000ULL, 6);

  gsm_drive_resolve_endurance (drive, 0);
  g_assert_true (gsm_drive_get_implied_endurance (drive, NULL, NULL));
  g_assert_false (gsm_drive_endurance_is_conflicted (drive));
}


int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/drive-spec/every row has a source and finds itself",
                   test_every_row_has_a_source_and_finds_itself);
  g_test_add_func ("/drive-spec/Samsung ratings are not linear in capacity",
                   test_samsung_ratings_are_not_linear_in_capacity);
  g_test_add_func ("/drive-spec/Samsung look-alikes are not caught",
                   test_samsung_look_alikes_are_not_caught);
  g_test_add_func ("/drive-spec/WD ratings", test_wd_ratings);
  g_test_add_func ("/drive-spec/the WD PC SN740 1 TB is rated 400 TBW by its product brief",
                   test_the_wd_pc_sn740_1tb_is_rated_400_tbw_by_its_product_brief);
  g_test_add_func ("/drive-spec/Crucial ratings are absolute figures",
                   test_crucial_ratings_are_absolute_figures);
  g_test_add_func ("/drive-spec/the Crucial X9 has no published rating",
                   test_the_crucial_x9_has_no_published_rating);
  g_test_add_func ("/drive-spec/Kingston ratings", test_kingston_ratings);
  g_test_add_func ("/drive-spec/Intel, Solidigm and SK hynix ratings",
                   test_intel_solidigm_and_skhynix_ratings);
  g_test_add_func ("/drive-spec/models without a published rating say not published",
                   test_models_without_a_published_rating_say_not_published);
  g_test_add_func ("/drive-spec/model matching ignores case", test_model_matching_ignores_case);
  g_test_add_func ("/drive-spec/12 percent used at 217.1 TB written implies 1.67 to 1.81 PB",
                   test_12_percent_used_at_217_tb_written_implies_1_67_to_1_81_pb);
  g_test_add_func ("/drive-spec/the wear range is unbounded without a whole percent",
                   test_the_wear_range_is_unbounded_without_a_whole_percent);
  g_test_add_func ("/drive-spec/a drive keeps the maker's rating and its own range side by side",
                   test_a_drive_keeps_the_makers_rating_and_its_own_range_side_by_side);
  g_test_add_func ("/drive-spec/a lightly worn drive follows the maker's rating",
                   test_a_lightly_worn_drive_follows_the_makers_rating);
  g_test_add_func ("/drive-spec/a drive without a published rating is unknown until it has worn",
                   test_a_drive_without_a_published_rating_is_unknown_until_it_has_worn);
  g_test_add_func ("/drive-spec/a user override wins but both truths stay available",
                   test_a_user_override_wins_but_both_truths_stay_available);
  g_test_add_func ("/drive-spec/a rating inside the implied range is not a conflict",
                   test_a_rating_inside_the_implied_range_is_not_a_conflict);

  return g_test_run ();
}
