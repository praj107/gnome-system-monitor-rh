/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Write attribution over fake cgroup trees, built at run time in a private
 * directory and read through the tracker's root-path seam. After every step
 * the rows of one device must sum to the root cgroup's own delta (the whole
 * device), through births, deaths, recreations, and a controller being
 * switched on mid-run. A fake tree is the only honest fixture: the counters
 * io.stat shows (Documentation/admin-guide/cgroup-v2.rst, "IO") are
 * cumulative sums that the test advances by hand, the way the kernel does.
 */

#define _XOPEN_SOURCE 700

#include <fcntl.h>
#include <ftw.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "gsm-blame.h"
#include "gsm-health-db.h"

#define DEV_MAJOR 259
#define DEV_MINOR 0

typedef struct
{
  char            *root;
  char            *stash;      /* outside the root: keeps a removed inode alive */
  GsmBlameTracker *tracker;
  uint64_t last_root;
  uint64_t root_now;
} Fx;


static int
remove_entry (const char        *path,
              const struct stat *st,
              int                flag,
              struct FTW        *ftw)
{
  (void) st;
  (void) flag;
  (void) ftw;

  return g_remove (path);
}


static void
rm_tree (const char *path)
{
  nftw (path, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
}


static void
fx_open (Fx *f)
{
  g_autofree char *base = g_dir_make_tmp ("gsm-blame-XXXXXX", NULL);

  g_assert_nonnull (base);
  f->root = g_build_filename (base, "cgroup", NULL);
  f->stash = g_build_filename (base, "stash", NULL);
  g_assert_cmpint (g_mkdir (f->root, 0700), ==, 0);
  g_assert_cmpint (g_mkdir (f->stash, 0700), ==, 0);
  f->tracker = gsm_blame_tracker_new (f->root);
}


static void
fx_close (Fx *f)
{
  g_autofree char *base = g_path_get_dirname (f->root);

  gsm_blame_tracker_free (f->tracker);
  rm_tree (base);
  g_free (f->root);
  g_free (f->stash);
}


/* Set the cumulative write counter of a cgroup on 259:0, creating it. */
static void
put (Fx         *f,
     const char *relative,
     uint64_t    wbytes)
{
  g_autofree char *dir = g_build_filename (f->root, relative, NULL);
  g_autofree char *file = g_build_filename (dir, "io.stat", NULL);
  g_autofree char *text =
    g_strdup_printf ("%d:%d rbytes=7 wbytes=%" G_GUINT64_FORMAT " rios=1 wios=1 dbytes=0 dios=0\n",
                     DEV_MAJOR, DEV_MINOR, wbytes);

  g_assert_cmpint (g_mkdir_with_parents (dir, 0700), ==, 0);
  g_assert_true (g_file_set_contents (file, text, -1, NULL));
  if (relative[0] == '\0')
    f->root_now = wbytes;
}


static void
drop (Fx         *f,
      const char *relative)
{
  g_autofree char *dir = g_build_filename (f->root, relative, NULL);

  rm_tree (dir);
}


/* Move a cgroup out of the tree but keep its directory, so the next one
 * created under the same name cannot be handed the same inode number. */
static void
stash (Fx         *f,
       const char *relative)
{
  g_autofree char *dir = g_build_filename (f->root, relative, NULL);
  g_autofree char *to = g_build_filename (f->stash, "old", NULL);

  g_assert_cmpint (g_rename (dir, to), ==, 0);
}


static uint64_t
sum_rows (GPtrArray *rows)
{
  uint64_t sum = 0;

  for (guint i = 0; i < rows->len; i++)
    sum += ((GsmBlameDelta *) g_ptr_array_index (rows, i))->wbytes;

  return sum;
}


static uint64_t
row_bytes (GPtrArray  *rows,
           const char *name)
{
  uint64_t sum = 0;

  for (guint i = 0; i < rows->len; i++)
    {
      GsmBlameDelta *row = g_ptr_array_index (rows, i);

      if (g_strcmp0 (row->name, name) == 0)
        sum += row->wbytes;
    }

  return sum;
}


/* Read once and check the invariant: rows sum to the root's delta. */
static GPtrArray *
step (Fx *f)
{
  GPtrArray *rows = gsm_blame_tracker_update (f->tracker);

  g_assert_cmpuint (sum_rows (rows), ==, f->root_now - f->last_root);
  f->last_root = f->root_now;

  return rows;
}


static void
baseline (Fx *f)
{
  g_autoptr (GPtrArray) rows = gsm_blame_tracker_update (f->tracker);

  g_assert_cmpuint (rows->len, ==, 0);
  f->last_root = f->root_now;
}


/* ------------------------------------------------------------------ */

static void
test_the_first_read_is_a_baseline_and_the_second_sums_to_the_root (void)
{
  Fx f = {
    0
  };

  g_autoptr (GPtrArray) rows = NULL;

  fx_open (&f);
  put (&f, "", 1000);
  put (&f, "system.slice", 600);
  put (&f, "system.slice/nginx.service", 400);
  put (&f, "user.slice", 300);
  baseline (&f);

  put (&f, "", 1500);
  put (&f, "system.slice", 800);
  put (&f, "system.slice/nginx.service", 550);
  put (&f, "user.slice", 450);
  rows = step (&f);

  g_assert_cmpuint (row_bytes (rows, "nginx.service"), ==, 150);
  g_assert_cmpuint (row_bytes (rows, "system.slice (own and exited units)"), ==, 50);
  g_assert_cmpuint (row_bytes (rows, "user.slice (own and exited units)"), ==, 150);
  g_assert_cmpuint (row_bytes (rows, "kernel and unattributed"), ==, 150);
  fx_close (&f);
}


static void
test_a_child_born_and_gone_between_two_reads_lands_in_its_parents_residue (void)
{
  Fx f = {
    0
  };

  g_autoptr (GPtrArray) rows = NULL;

  fx_open (&f);
  put (&f, "", 100);
  put (&f, "system.slice", 100);
  baseline (&f);

  /* A job wrote 50 bytes and was removed before the next read. */
  put (&f, "", 150);
  put (&f, "system.slice", 150);
  rows = step (&f);

  g_assert_cmpuint (row_bytes (rows, "system.slice (own and exited units)"), ==, 50);
  fx_close (&f);
}


static void
test_a_child_seen_once_then_gone_is_counted_once (void)
{
  Fx f = {
    0
  };

  g_autoptr (GPtrArray) born = NULL;
  g_autoptr (GPtrArray) gone = NULL;
  g_autoptr (GPtrArray) after = NULL;

  fx_open (&f);
  put (&f, "", 100);
  put (&f, "system.slice", 100);
  baseline (&f);

  /* Born with 80 already written: its first counter is its own. */
  put (&f, "", 180);
  put (&f, "system.slice", 180);
  put (&f, "system.slice/job.service", 80);
  born = step (&f);
  g_assert_cmpuint (row_bytes (born, "job.service"), ==, 80);
  g_assert_cmpuint (row_bytes (born, "system.slice (own and exited units)"), ==, 0);

  /* Its last 20 bytes arrive after the read; then it exits. They are the
   * parent's, and the 80 are not counted a second time. */
  drop (&f, "system.slice/job.service");
  put (&f, "", 200);
  put (&f, "system.slice", 200);
  gone = step (&f);
  g_assert_cmpuint (row_bytes (gone, "job.service"), ==, 0);
  g_assert_cmpuint (row_bytes (gone, "system.slice (own and exited units)"), ==, 20);

  after = step (&f);
  g_assert_cmpuint (after->len, ==, 0);
  fx_close (&f);
}


static void
test_a_unit_recreated_under_the_same_name_starts_from_zero (void)
{
  Fx f = {
    0
  };

  g_autoptr (GPtrArray) rows = NULL;

  fx_open (&f);
  put (&f, "", 1000);
  put (&f, "system.slice", 1000);
  put (&f, "system.slice/job.service", 900);
  baseline (&f);

  /* The old instance wrote 10 more and went; the new one (a new inode, a
   * counter that started at zero) is already at 30, which is below the old
   * instance's counter and must not be mistaken for a reset of it. */
  stash (&f, "system.slice/job.service");
  put (&f, "", 1040);
  put (&f, "system.slice", 1040);
  put (&f, "system.slice/job.service", 30);
  rows = step (&f);

  g_assert_cmpuint (row_bytes (rows, "job.service"), ==, 30);
  g_assert_cmpuint (row_bytes (rows, "system.slice (own and exited units)"), ==, 10);
  fx_close (&f);
}


static void
test_a_unit_recreated_with_a_larger_counter_is_not_diffed_against_the_old_one (void)
{
  Fx f = {
    0
  };

  g_autoptr (GPtrArray) rows = NULL;

  fx_open (&f);
  put (&f, "", 1000);
  put (&f, "system.slice", 1000);
  put (&f, "system.slice/job.service", 100);
  baseline (&f);

  stash (&f, "system.slice/job.service");
  put (&f, "", 1500);
  put (&f, "system.slice", 1500);
  put (&f, "system.slice/job.service", 400);
  rows = step (&f);

  g_assert_cmpuint (row_bytes (rows, "job.service"), ==, 400);
  g_assert_cmpuint (row_bytes (rows, "system.slice (own and exited units)"), ==, 100);
  fx_close (&f);
}


static void
test_io_enabled_on_a_subtree_mid_run_is_split_without_a_double_count (void)
{
  Fx f = {
    0
  };

  g_autoptr (GPtrArray) rows = NULL;
  g_autoptr (GPtrArray) next = NULL;

  fx_open (&f);
  put (&f, "", 1000);
  put (&f, "user.slice", 300);
  baseline (&f);

  /* The controller is switched on for user-1000.slice and one scope in it:
   * both appear with counters that started at zero, while user.slice's
   * counter, which includes them, moved by 80. */
  put (&f, "", 1080);
  put (&f, "user.slice", 380);
  put (&f, "user.slice/user-1000.slice", 70);
  put (&f, "user.slice/user-1000.slice/app-foo-123.scope", 25);
  rows = step (&f);

  g_assert_cmpuint (row_bytes (rows, "foo"), ==, 25);
  g_assert_cmpuint (row_bytes (rows, "user-1000.slice (own and exited units)"), ==, 45);
  g_assert_cmpuint (row_bytes (rows, "user.slice (own and exited units)"), ==, 10);

  put (&f, "", 1100);
  put (&f, "user.slice", 400);
  put (&f, "user.slice/user-1000.slice", 90);
  put (&f, "user.slice/user-1000.slice/app-foo-123.scope", 45);
  next = step (&f);
  g_assert_cmpuint (row_bytes (next, "foo"), ==, 20);
  g_assert_cmpuint (row_bytes (next, "user.slice (own and exited units)"), ==, 0);
  fx_close (&f);
}


static void
test_a_slice_that_loses_its_last_child_is_not_counted_again (void)
{
  Fx f = {
    0
  };

  g_autoptr (GPtrArray) rows = NULL;

  fx_open (&f);
  put (&f, "", 500);
  put (&f, "system.slice", 200);
  put (&f, "system.slice/job.service", 150);
  baseline (&f);

  /* The slice becomes a leaf of the tree. Its whole counter (205) must not
   * arrive as a new key; only the 5 bytes it wrote since are new. */
  drop (&f, "system.slice/job.service");
  put (&f, "", 505);
  put (&f, "system.slice", 205);
  rows = step (&f);

  g_assert_cmpuint (row_bytes (rows, "system.slice (own and exited units)"), ==, 5);
  fx_close (&f);
}


static void
test_a_child_ahead_of_its_parent_is_clamped_and_the_shortfall_carried (void)
{
  Fx f = {
    0
  };

  g_autoptr (GPtrArray) first = NULL;
  g_autoptr (GPtrArray) second = NULL;
  uint64_t total;

  fx_open (&f);
  put (&f, "", 1000);
  put (&f, "system.slice", 500);
  put (&f, "system.slice/job.service", 300);
  baseline (&f);

  /* The child's counter was read after the parent's was last folded: it
   * moved 100, the parent only 90. The parent's residue cannot go negative. */
  put (&f, "", 1090);
  put (&f, "system.slice", 590);
  put (&f, "system.slice/job.service", 400);
  first = gsm_blame_tracker_update (f.tracker);
  g_assert_cmpuint (row_bytes (first, "job.service"), ==, 100);
  g_assert_cmpuint (row_bytes (first, "system.slice (own and exited units)"), ==, 0);
  total = sum_rows (first);

  /* The parent catches up: 30 more bytes, of which 10 repay the shortfall. */
  put (&f, "", 1120);
  put (&f, "system.slice", 620);
  second = gsm_blame_tracker_update (f.tracker);
  g_assert_cmpuint (row_bytes (second, "system.slice (own and exited units)"), ==, 20);
  total += sum_rows (second);

  /* Over the two intervals nothing was counted twice or dropped. */
  g_assert_cmpuint (total, ==, 120);
  fx_close (&f);
}


static void
test_a_subcgroup_of_a_unit_is_named_by_the_unit (void)
{
  Fx f = {
    0
  };

  g_autoptr (GPtrArray) rows = NULL;

  fx_open (&f);
  put (&f, "", 100);
  put (&f, "user.slice", 100);
  put (&f, "user.slice/app-chromium-4126.scope", 100);
  put (&f, "user.slice/app-chromium-4126.scope/tab1", 60);
  baseline (&f);

  put (&f, "", 200);
  put (&f, "user.slice", 200);
  put (&f, "user.slice/app-chromium-4126.scope", 200);
  put (&f, "user.slice/app-chromium-4126.scope/tab1", 150);
  rows = step (&f);

  g_assert_cmpuint (row_bytes (rows, "chromium"), ==, 90);
  g_assert_cmpuint (row_bytes (rows, "chromium (own and exited units)"), ==, 10);
  fx_close (&f);
}


static void
test_a_cgroup_that_vanishes_mid_read_is_not_an_error (void)
{
  Fx f = {
    0
  };

  g_autoptr (GPtrArray) rows = NULL;
  g_autofree char *file = NULL;

  fx_open (&f);
  put (&f, "", 100);
  put (&f, "system.slice", 100);
  put (&f, "system.slice/job.service", 50);
  baseline (&f);

  /* A directory with no io.stat (the file went first): its subtree is
   * skipped, and its bytes stay with the ancestor. */
  file = g_build_filename (f.root, "system.slice/job.service/io.stat", NULL);
  g_assert_cmpint (g_remove (file), ==, 0);
  put (&f, "", 160);
  put (&f, "system.slice", 160);
  rows = step (&f);

  g_assert_cmpuint (row_bytes (rows, "system.slice (own and exited units)"), ==, 60);
  fx_close (&f);
}


static void
test_an_unreadable_io_stat_skips_the_update_and_credits_nothing_twice (void)
{
  Fx f = {
    0
  };

  g_autoptr (GPtrArray) first = NULL;
  g_autoptr (GPtrArray) second = NULL;
  g_autoptr (GPtrArray) rows = NULL;
  g_autofree char *file = NULL;
  int fd;

  fx_open (&f);
  put (&f, "", 100);
  put (&f, "system.slice", 100);
  put (&f, "system.slice/job.service", 50);
  baseline (&f);

  put (&f, "", 200);
  put (&f, "system.slice", 200);
  put (&f, "system.slice/job.service", 150);
  file = g_build_filename (f.root, "system.slice/job.service/io.stat", NULL);
  g_assert_cmpint (g_chmod (file, 0), ==, 0);

  /* A file the owner cannot read is not a cgroup that exited (root can read
   * it regardless, and then there is nothing to test). */
  fd = g_open (file, O_RDONLY, 0);
  if (fd >= 0)
    {
      close (fd);
      g_test_skip ("io.stat is readable despite mode 000");
      fx_close (&f);
      return;
    }

  first = gsm_blame_tracker_update (f.tracker);
  g_assert_cmpuint (first->len, ==, 0);

  put (&f, "", 230);
  put (&f, "system.slice", 230);
  second = gsm_blame_tracker_update (f.tracker);
  g_assert_cmpuint (second->len, ==, 0);

  /* Readable again: one read covers both intervals, once. */
  g_assert_cmpint (g_chmod (file, 0600), ==, 0);
  rows = step (&f);
  g_assert_cmpuint (row_bytes (rows, "job.service"), ==, 100);
  g_assert_cmpuint (row_bytes (rows, "system.slice (own and exited units)"), ==, 30);
  fx_close (&f);
}


typedef struct
{
  const char *drive_for_259;      /* NULL: the map does not know 259:0 yet */
  GHashTable *bytes;              /* drive id -> bytes recorded */
} Map;

static const char *
map_lookup (guint    major,
            guint    minor,
            gpointer data)
{
  Map *map = data;

  if (major == 259 && minor == 0)
    return map->drive_for_259;
  if (major == 8 && minor == 0)
    return "SDA";

  return NULL;
}

static void
map_sink (const char          *drive_id,
          const GsmBlameDelta *delta,
          gpointer             data)
{
  Map *map = data;
  uint64_t before = GPOINTER_TO_SIZE (g_hash_table_lookup (map->bytes, drive_id));

  g_hash_table_insert (map->bytes, g_strdup (drive_id),
                       GSIZE_TO_POINTER (before + delta->wbytes));
}

static GsmBlameDelta *
make_delta (guint       major,
            const char *name,
            uint64_t    wbytes)
{
  GsmBlameDelta *delta = g_new0 (GsmBlameDelta, 1);

  delta->cgroup = g_strdup ("x");
  delta->name = g_strdup (name);
  delta->major = major;
  delta->minor = 0;
  delta->wbytes = wbytes;
  delta->basis = GSM_BLAME_BASIS_KERNEL;

  return delta;
}

static uint64_t
map_bytes (Map        *map,
           const char *drive_id)
{
  return GPOINTER_TO_SIZE (g_hash_table_lookup (map->bytes, drive_id));
}


static void
test_bytes_for_a_device_the_map_does_not_know_yet_land_on_its_drive_once (void)
{
  Map map = {
    NULL, g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL)
  };
  GsmBlameHold *hold = gsm_blame_hold_new ();

  g_autoptr (GPtrArray) deltas = g_ptr_array_new_with_free_func ((GDestroyNotify) gsm_blame_delta_free);

  g_ptr_array_add (deltas, make_delta (259, "foo", 100));
  g_ptr_array_add (deltas, make_delta (8, "bar", 50));
  g_ptr_array_add (deltas, make_delta (259, "foo", 20));
  gsm_blame_hold_resolve (hold, deltas, map_lookup, map_sink, &map, FALSE);

  /* The known drive got its bytes; the unknown one's are waiting, summed. */
  g_assert_cmpuint (map_bytes (&map, "SDA"), ==, 50);
  g_assert_cmpuint (gsm_blame_hold_count (hold), ==, 1);

  /* Still unknown after another interval: more waits, nothing is lost. */
  g_ptr_array_set_size (deltas, 0);
  g_ptr_array_add (deltas, make_delta (259, "foo", 5));
  gsm_blame_hold_resolve (hold, deltas, map_lookup, map_sink, &map, FALSE);
  g_assert_cmpuint (gsm_blame_hold_count (hold), ==, 1);

  /* The map learns the drive: everything lands on it, once. */
  map.drive_for_259 = "NVME";
  gsm_blame_hold_resolve (hold, NULL, map_lookup, map_sink, &map, TRUE);
  g_assert_cmpuint (map_bytes (&map, "NVME"), ==, 125);
  g_assert_cmpuint (gsm_blame_hold_count (hold), ==, 0);
  gsm_blame_hold_resolve (hold, NULL, map_lookup, map_sink, &map, TRUE);
  g_assert_cmpuint (map_bytes (&map, "NVME"), ==, 125);
  g_assert_cmpuint (map_bytes (&map, "SDA"), ==, 50);

  gsm_blame_hold_free (hold);
  g_hash_table_unref (map.bytes);
}


static void
test_bytes_for_a_device_that_is_not_a_tracked_drive_are_dropped_after_a_refresh (void)
{
  Map map = {
    NULL, g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL)
  };
  GsmBlameHold *hold = gsm_blame_hold_new ();

  g_autoptr (GPtrArray) deltas = g_ptr_array_new_with_free_func ((GDestroyNotify) gsm_blame_delta_free);

  g_ptr_array_add (deltas, make_delta (259, "foo", 100));
  gsm_blame_hold_resolve (hold, deltas, map_lookup, map_sink, &map, FALSE);
  g_assert_cmpuint (gsm_blame_hold_count (hold), ==, 1);

  /* A refresh that still does not know it: not a tracked drive. */
  gsm_blame_hold_resolve (hold, NULL, map_lookup, map_sink, &map, TRUE);
  g_assert_cmpuint (gsm_blame_hold_count (hold), ==, 0);
  g_assert_cmpuint (g_hash_table_size (map.bytes), ==, 0);

  gsm_blame_hold_free (hold);
  g_hash_table_unref (map.bytes);
}


static void
test_names (void)
{
  g_autofree char *a = gsm_blame_node_name ("system.slice/system-systemd\\x2dcoredump.slice");
  g_autofree char *b = gsm_blame_node_name ("");
  g_autofree char *c = gsm_blame_node_name ("user.slice/user-1000.slice/app-org.gnome.Terminal-77.scope");
  g_autofree char *d = gsm_blame_node_name ("system.slice/ssh.service/sub");

  g_assert_cmpstr (a, ==, "system-systemd-coredump.slice");
  g_assert_cmpstr (b, ==, "kernel and unattributed");
  g_assert_cmpstr (c, ==, "org.gnome.Terminal");
  g_assert_cmpstr (d, ==, "ssh.service");
}


static void
test_a_numbered_template_instance_collapses (void)
{
  g_autofree char *dump = gsm_blame_normalize_name ("systemd-coredump@5213-4047792-0.service");
  g_autofree char *user = gsm_blame_normalize_name ("user@1000.service");
  g_autofree char *getty = gsm_blame_normalize_name ("getty@tty1.service");
  g_autofree char *node = gsm_blame_node_name ("system.slice/system-systemd\\x2dcoredump.slice/"
                                               "systemd-coredump@5213-4047792-0.service");

  g_assert_cmpstr (dump, ==, "systemd-coredump@.service");
  g_assert_cmpstr (user, ==, "user@1000.service");
  g_assert_cmpstr (getty, ==, "getty@tty1.service");
  g_assert_cmpstr (node, ==, "systemd-coredump@.service");
}


/* ------------------------------------------------------------------ */
/* Storage                                                             */
/* ------------------------------------------------------------------ */

static char *
temp_db_dir (void)
{
  char *dir = g_dir_make_tmp ("gsm-blame-db-XXXXXX", NULL);

  g_assert_nonnull (dir);

  return dir;
}


static void
test_a_version_1_database_keeps_its_bytes_and_gains_a_basis (void)
{
  g_autofree char *dir = temp_db_dir ();
  g_autofree char *path = g_build_filename (dir, "health.db", NULL);

  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) detail = NULL;
  g_autoptr (GPtrArray) rows = NULL;
  sqlite3 *raw = NULL;
  sqlite3_stmt *stmt = NULL;
  GsmHealthDb *db;
  int64_t today = (int64_t) (g_get_real_time () / G_USEC_PER_SEC / 86400);
  g_autofree char *seed = NULL;

  g_assert_cmpint (sqlite3_open (path, &raw), ==, SQLITE_OK);
  seed = g_strdup_printf (
    "CREATE TABLE blame_daily (drive_id TEXT NOT NULL, day INTEGER NOT NULL,"
    " name TEXT NOT NULL, wbytes INTEGER NOT NULL DEFAULT 0,"
    " exact INTEGER NOT NULL DEFAULT 0, PRIMARY KEY (drive_id, day, name)) WITHOUT ROWID;"
    "CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT);"
    "INSERT INTO meta VALUES ('schema_version', '1');"
    "INSERT INTO blame_daily VALUES ('D1', %lld, 'nginx.service', 5000, 1);"
    "INSERT INTO blame_daily VALUES ('D1', %lld, 'firefox', 700, 0);",
    (long long) today, (long long) today);
  g_assert_cmpint (sqlite3_exec (raw, seed, NULL, NULL, NULL), ==, SQLITE_OK);
  sqlite3_close (raw);

  db = gsm_health_db_new (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (db);

  detail = gsm_health_db_get_blame_detail (db, "D1", GSM_HISTORY_RANGE_DAY);
  g_assert_cmpuint (detail->len, ==, 2);
  {
    GsmBlameDetail *big = g_ptr_array_index (detail, 0);
    GsmBlameDetail *small = g_ptr_array_index (detail, 1);

    g_assert_cmpstr (big->name, ==, "nginx.service");
    g_assert_cmpstr (big->basis, ==, "kernel");
    g_assert_cmpstr (big->via, ==, "");
    g_assert_cmpuint (big->wbytes, ==, 5000);
    g_assert_cmpstr (small->name, ==, "firefox");
    g_assert_cmpstr (small->basis, ==, "procfs");
    g_assert_cmpuint (small->wbytes, ==, 700);
  }

  rows = gsm_health_db_get_blame (db, "D1", GSM_HISTORY_RANGE_DAY);
  g_assert_cmpuint (rows->len, ==, 2);
  g_assert_true (((GsmBlameRow *) g_ptr_array_index (rows, 0))->exact);
  g_assert_false (((GsmBlameRow *) g_ptr_array_index (rows, 1))->exact);
  g_object_unref (db);

  /* Opening it again is a no-op, and the version is recorded. */
  db = gsm_health_db_new (path, &error);
  g_assert_no_error (error);
  g_object_unref (db);

  g_assert_cmpint (sqlite3_open (path, &raw), ==, SQLITE_OK);
  g_assert_cmpint (sqlite3_prepare_v2 (raw, "SELECT value FROM meta WHERE key = 'schema_version'",
                                       -1, &stmt, NULL), ==, SQLITE_OK);
  g_assert_cmpint (sqlite3_step (stmt), ==, SQLITE_ROW);
  g_assert_cmpstr ((const char *) sqlite3_column_text (stmt, 0), ==, "2");
  sqlite3_finalize (stmt);
  sqlite3_close (raw);

  rm_tree (dir);
}


static void
test_a_flush_keeps_twenty_rows_per_kind_and_folds_the_rest (void)
{
  g_autofree char *dir = temp_db_dir ();
  g_autofree char *path = g_build_filename (dir, "health.db", NULL);

  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) detail = NULL;
  g_autoptr (GPtrArray) rows = NULL;
  GsmHealthDb *db = gsm_health_db_new (path, &error);
  uint64_t total = 0;
  uint64_t other = 0;
  guint kept = 0;

  g_assert_no_error (error);

  for (int i = 0; i < 25; i++)
    {
      g_autofree char *name = g_strdup_printf ("writer-%02d", i);

      gsm_health_db_record_blame_row (db, "D1", name, "", GSM_HEALTH_BASIS_KERNEL,
                                      1000 + (uint64_t) i);
      total += 1000 + (uint64_t) i;
    }
  /* An on-behalf row is its own kind and re-divides bytes already charged. */
  gsm_health_db_record_blame_row (db, "D1", "crasher", "systemd-coredump@.service",
                                  GSM_HEALTH_BASIS_COREDUMP_PROPORTIONAL, 777);
  g_assert_true (gsm_health_db_flush (db, &error));

  detail = gsm_health_db_get_blame_detail (db, "D1", GSM_HISTORY_RANGE_DAY);
  for (guint i = 0; i < detail->len; i++)
    {
      GsmBlameDetail *row = g_ptr_array_index (detail, i);

      if (row->via[0] != '\0')
        continue;
      if (g_strcmp0 (row->name, "other") == 0)
        other = row->wbytes;
      else
        kept++;
      if (row->via[0] == '\0')
        total -= row->wbytes;
    }
  g_assert_cmpuint (kept, ==, 20);
  g_assert_cmpuint (other, >, 0);
  g_assert_cmpuint (total, ==, 0);          /* nothing dropped */
  g_assert_cmpuint (detail->len, ==, 22);   /* 20 + other + the on-behalf row */

  rows = gsm_health_db_get_blame (db, "D1", GSM_HISTORY_RANGE_DAY);
  for (guint i = 0; i < rows->len; i++)
    g_assert_cmpstr (((GsmBlameRow *) g_ptr_array_index (rows, i))->name, !=, "crasher");

  g_object_unref (db);
  rm_tree (dir);
}


int
main (int   argc,
      char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/health-blame/the first read is a baseline and the second sums to the root",
                   test_the_first_read_is_a_baseline_and_the_second_sums_to_the_root);
  g_test_add_func ("/health-blame/a child born and gone between two reads lands in its parent's residue",
                   test_a_child_born_and_gone_between_two_reads_lands_in_its_parents_residue);
  g_test_add_func ("/health-blame/a child seen once then gone is counted once",
                   test_a_child_seen_once_then_gone_is_counted_once);
  g_test_add_func ("/health-blame/a unit recreated under the same name starts from zero",
                   test_a_unit_recreated_under_the_same_name_starts_from_zero);
  g_test_add_func ("/health-blame/a unit recreated with a larger counter is not diffed against the old one",
                   test_a_unit_recreated_with_a_larger_counter_is_not_diffed_against_the_old_one);
  g_test_add_func ("/health-blame/io enabled on a subtree mid-run is split without a double count",
                   test_io_enabled_on_a_subtree_mid_run_is_split_without_a_double_count);
  g_test_add_func ("/health-blame/a slice that loses its last child is not counted again",
                   test_a_slice_that_loses_its_last_child_is_not_counted_again);
  g_test_add_func ("/health-blame/a child ahead of its parent is clamped and the shortfall carried",
                   test_a_child_ahead_of_its_parent_is_clamped_and_the_shortfall_carried);
  g_test_add_func ("/health-blame/a subcgroup of a unit is named by the unit",
                   test_a_subcgroup_of_a_unit_is_named_by_the_unit);
  g_test_add_func ("/health-blame/a cgroup without io.stat is skipped, its bytes stay with the ancestor",
                   test_a_cgroup_that_vanishes_mid_read_is_not_an_error);
  g_test_add_func ("/health-blame/an unreadable io.stat skips the update and credits nothing twice",
                   test_an_unreadable_io_stat_skips_the_update_and_credits_nothing_twice);
  g_test_add_func ("/health-blame/bytes for a device the map does not know yet land on its drive once",
                   test_bytes_for_a_device_the_map_does_not_know_yet_land_on_its_drive_once);
  g_test_add_func ("/health-blame/bytes for a device that is not a tracked drive are dropped after a refresh",
                   test_bytes_for_a_device_that_is_not_a_tracked_drive_are_dropped_after_a_refresh);
  g_test_add_func ("/health-blame/names: slices decoded, root, scopes, subcgroups",
                   test_names);
  g_test_add_func ("/health-blame/a numbered template instance collapses, a named one does not",
                   test_a_numbered_template_instance_collapses);
  g_test_add_func ("/health-blame/a version 1 database keeps its bytes and gains a basis",
                   test_a_version_1_database_keeps_its_bytes_and_gains_a_basis);
  g_test_add_func ("/health-blame/a flush keeps twenty rows per kind and folds the rest",
                   test_a_flush_keeps_twenty_rows_per_kind_and_folds_the_rest);

  return g_test_run ();
}
