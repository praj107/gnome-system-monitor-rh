/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The Processes tab's figures over a captured /proc tree (the collector's
 * root-path seam) and over hand-made records for the edge cases.
 *
 *   test-procsource FIXTURE_PROC
 */

#include <string.h>

#include <string>
#include <utility>

#include <glib.h>
#include <glibtop/procstate.h>

#include "../procsource.h"

static const char *fixture_proc;

static const guint64 kB = 1024;
static const guint64 second_ns = 1000000000ULL;

static gsm_process
make_record (void)
{
  gsm_process record = {};

  record.struct_size = sizeof (record);
  record.pid = 4242;
  record.ppid = 1;
  record.start_ticks = 5000;
  record.state = 'S';
  record.num_threads = 3;
  record.nice = -5;
  record.known = GSM_PROCESS_HAS_RSS_ANON | GSM_PROCESS_HAS_RSS_FILE
                 | GSM_PROCESS_HAS_RSS_SHMEM | GSM_PROCESS_HAS_VM_SIZE
                 | GSM_PROCESS_HAS_UID | GSM_PROCESS_HAS_RUN_NS | GSM_PROCESS_HAS_IO;
  record.rss_anon_bytes = 100 * kB;
  record.rss_file_bytes = 40 * kB;
  record.rss_shmem_bytes = 8 * kB;
  record.vm_size_bytes = 9000 * kB;
  record.uid_real = 1000;
  record.utime_ticks = 70;
  record.stime_ticks = 30;
  return record;
}

static GsmProcWindow
make_window (const gsm_process *previous)
{
  GsmProcWindow window = {};

  window.previous = previous;
  window.previous_at_ns = 10 * second_ns;
  window.current_at_ns = 12 * second_ns;
  window.cpu_count = 4;
  window.solaris_mode = FALSE;
  window.boot_time_s = 1700000000;
  window.clock_ticks_per_s = 100;
  return window;
}

static GsmProcFigures
figures_of (const GsmProcWindow &window,
            const gsm_process   &record)
{
  GsmProcFigures figures;

  gsm_proc_figures_from_record (&window, &record, &figures);
  return figures;
}

static void
test_memory_columns_follow_the_definitions (void)
{
  gsm_process record = make_record ();
  GsmProcFigures figures = figures_of (make_window (NULL), record);

  g_assert_true (figures.mem_known);
  g_assert_cmpuint (figures.mem_bytes, ==, 100 * kB);
  g_assert_true (figures.memshared_known);
  g_assert_cmpuint (figures.memshared_bytes, ==, 48 * kB);
  g_assert_true (figures.memres_known);
  g_assert_cmpuint (figures.memres_bytes, ==, 148 * kB);
  g_assert_true (figures.vmsize_known);
  g_assert_cmpuint (figures.vmsize_bytes, ==, 9000 * kB);
}

static void
test_memory_without_the_status_bits_is_unknown_not_zero (void)
{
  gsm_process record = make_record ();

  record.known = GSM_PROCESS_HAS_UID;
  GsmProcFigures figures = figures_of (make_window (NULL), record);

  g_assert_false (figures.mem_known);
  g_assert_false (figures.memshared_known);
  g_assert_false (figures.memres_known);
  g_assert_false (figures.vmsize_known);

  /* a sum needs all its parts: RssShmem missing leaves Resident and Shared
   * unknown, while Private (RssAnon alone) is still known */
  record.known = GSM_PROCESS_HAS_RSS_ANON | GSM_PROCESS_HAS_RSS_FILE;
  figures = figures_of (make_window (NULL), record);
  g_assert_true (figures.mem_known);
  g_assert_false (figures.memshared_known);
  g_assert_false (figures.memres_known);
}

static void
test_cpu_time_and_start_time (void)
{
  gsm_process record = make_record ();
  GsmProcFigures figures = figures_of (make_window (NULL), record);

  g_assert_cmpuint (figures.cpu_time_ticks, ==, 100);
  g_assert_true (figures.start_time_known);
  g_assert_cmpuint (figures.start_time_s, ==, 1700000000 + 50);

  GsmProcWindow window = make_window (NULL);

  window.boot_time_s = -1;
  figures = figures_of (window, record);
  g_assert_false (figures.start_time_known);
}

/* Two records one 2 s window apart whose utime + stime grew by cpu_ticks. */
static void
make_pair (guint64      cpu_ticks,
           gsm_process *previous,
           gsm_process *record)
{
  *previous = make_record ();
  *record = make_record ();
  record->utime_ticks = previous->utime_ticks + cpu_ticks * 7 / 10;
  record->stime_ticks = previous->stime_ticks + (cpu_ticks - cpu_ticks * 7 / 10);
}

static void
test_cpu_share_is_cpu_ticks_over_wall_time (void)
{
  gsm_process previous, record;

  make_pair (100, &previous, &record);   /* 1 s of CPU in a 2 s window at 100 ticks/s */

  GsmProcWindow window = make_window (&previous);
  GsmProcFigures figures = figures_of (window, record);

  g_assert_true (figures.cpu_known);
  g_assert_cmpfloat_with_epsilon (figures.cpu_percent, 50.0, 1e-9);

  window.solaris_mode = TRUE;   /* of the whole 4-CPU machine */
  figures = figures_of (window, record);
  g_assert_true (figures.cpu_known);
  g_assert_cmpfloat_with_epsilon (figures.cpu_percent, 12.5, 1e-9);

  window.solaris_mode = FALSE;
  window.clock_ticks_per_s = 250;   /* the tick rate converts, it is not assumed */
  figures = figures_of (window, record);
  g_assert_cmpfloat_with_epsilon (figures.cpu_percent, 20.0, 1e-9);
}

static void
test_a_multithreaded_process_shows_its_whole_group_not_the_leaders_run_time (void)
{
  gsm_process previous, record;

  /* 3 CPUs' worth of utime + stime in the 2 s window, while schedstat (the
   * leader thread alone) says almost nothing: a JVM or a browser */
  make_pair (600, &previous, &record);
  previous.run_ns = 8 * 1000 * 1000;
  record.run_ns = 8 * 1000 * 1000 + 100 * 1000;

  GsmProcWindow window = make_window (&previous);
  GsmProcFigures figures = figures_of (window, record);

  g_assert_true (figures.cpu_known);
  g_assert_cmpfloat_with_epsilon (figures.cpu_percent, 300.0, 1e-9);

  window.solaris_mode = TRUE;
  figures = figures_of (window, record);
  g_assert_cmpfloat_with_epsilon (figures.cpu_percent, 300.0 / 4, 1e-9);

  /* and the other way round: schedstat claiming a lot is not believed */
  make_pair (0, &previous, &record);
  record.run_ns = 100 * second_ns;
  window = make_window (&previous);
  figures = figures_of (window, record);
  g_assert_true (figures.cpu_known);
  g_assert_cmpfloat (figures.cpu_percent, ==, 0.0);
}

static void
test_cpu_share_is_known_without_the_scheduler_statistics (void)
{
  gsm_process previous, record;

  make_pair (100, &previous, &record);
  previous.known &= ~GSM_PROCESS_HAS_RUN_NS;
  record.known &= ~GSM_PROCESS_HAS_RUN_NS;

  GsmProcFigures figures = figures_of (make_window (&previous), record);

  g_assert_true (figures.cpu_known);
  g_assert_cmpfloat_with_epsilon (figures.cpu_percent, 50.0, 1e-9);
}

static void
test_cpu_share_is_capped_at_the_scale (void)
{
  gsm_process previous, record;

  make_pair (100 * 100, &previous, &record);   /* 100 CPU s in a 2 s window */
  GsmProcWindow window = make_window (&previous);
  GsmProcFigures figures = figures_of (window, record);

  g_assert_true (figures.cpu_known);
  g_assert_cmpfloat (figures.cpu_percent, ==, 400.0);

  window.solaris_mode = TRUE;
  figures = figures_of (window, record);
  g_assert_cmpfloat (figures.cpu_percent, ==, 100.0);
}

static void
test_cpu_share_is_unknown_without_a_baseline (void)
{
  gsm_process previous, record;

  make_pair (5000, &previous, &record);

  /* first sample */
  g_assert_false (figures_of (make_window (NULL), record).cpu_known);

  /* a reused pid: another start time, and a larger counter than before */
  previous.start_ticks = 4000;
  g_assert_false (figures_of (make_window (&previous), record).cpu_known);

  /* a counter that went backwards */
  make_pair (100, &previous, &record);
  std::swap (previous.utime_ticks, record.utime_ticks);
  std::swap (previous.stime_ticks, record.stime_ticks);
  g_assert_false (figures_of (make_window (&previous), record).cpu_known);

  /* no time passed */
  make_pair (100, &previous, &record);
  GsmProcWindow window = make_window (&previous);
  window.current_at_ns = window.previous_at_ns;
  g_assert_false (figures_of (window, record).cpu_known);

  /* no tick rate to convert with */
  window = make_window (&previous);
  window.clock_ticks_per_s = 0;
  g_assert_false (figures_of (window, record).cpu_known);
}

static void
test_disk_write_excludes_cancelled_writes (void)
{
  gsm_process record = make_record ();

  record.io_read_bytes = 4096;
  record.io_write_bytes = 10000;
  record.io_cancelled_write_bytes = 2500;
  GsmProcFigures figures = figures_of (make_window (NULL), record);

  g_assert_true (figures.io_readable);
  g_assert_cmpuint (figures.disk_read_bytes, ==, 4096);
  g_assert_cmpuint (figures.disk_write_bytes, ==, 7500);
  g_assert_false (figures.io_rate_valid);
}

static void
test_cancelled_writes_beyond_the_written_total_leave_zero (void)
{
  gsm_process record = make_record ();

  record.io_write_bytes = 100;
  record.io_cancelled_write_bytes = 4096;
  GsmProcFigures figures = figures_of (make_window (NULL), record);

  g_assert_true (figures.io_readable);
  g_assert_cmpuint (figures.disk_write_bytes, ==, 0);
}

static void
test_disk_rates_come_from_two_snapshots (void)
{
  gsm_process previous = make_record ();
  gsm_process record = make_record ();

  previous.io_read_bytes = 1000;
  previous.io_write_bytes = 5000;
  previous.io_cancelled_write_bytes = 1000;
  record.io_read_bytes = 5000;           /* +4000 B in 2 s */
  record.io_write_bytes = 9000;
  record.io_cancelled_write_bytes = 1000; /* (9000-1000) - (5000-1000) = +4000 B */
  GsmProcFigures figures = figures_of (make_window (&previous), record);

  g_assert_true (figures.io_rate_valid);
  g_assert_cmpuint (figures.disk_read_bytes_per_s, ==, 2000);
  g_assert_cmpuint (figures.disk_write_bytes_per_s, ==, 2000);
}

static void
test_disk_without_the_io_file_is_unreadable_not_zero (void)
{
  gsm_process previous = make_record ();
  gsm_process record = make_record ();

  record.known &= ~GSM_PROCESS_HAS_IO;
  GsmProcFigures figures = figures_of (make_window (&previous), record);

  g_assert_false (figures.io_readable);
  g_assert_false (figures.io_rate_valid);

  /* readable now, but not in the previous snapshot: totals, no rate */
  record = make_record ();
  previous.known &= ~GSM_PROCESS_HAS_IO;
  figures = figures_of (make_window (&previous), record);
  g_assert_true (figures.io_readable);
  g_assert_false (figures.io_rate_valid);
}

static void
test_disk_rate_is_unknown_for_a_reused_pid_or_a_backwards_counter (void)
{
  gsm_process previous = make_record ();
  gsm_process record = make_record ();

  previous.io_read_bytes = 9000;
  record.io_read_bytes = 100;
  g_assert_false (figures_of (make_window (&previous), record).io_rate_valid);

  /* writes cancelled after the last snapshot can move the net total back */
  previous = make_record ();
  record = make_record ();
  previous.io_write_bytes = 8000;
  record.io_write_bytes = 8000;
  record.io_cancelled_write_bytes = 3000;
  GsmProcFigures figures = figures_of (make_window (&previous), record);

  g_assert_false (figures.io_rate_valid);
  g_assert_true (figures.io_readable);

  previous = make_record ();
  previous.start_ticks = 1;
  record = make_record ();
  g_assert_false (figures_of (make_window (&previous), record).io_rate_valid);
}

static void
test_state_letters_map_like_libgtop (void)
{
  gsm_process record = make_record ();
  const struct { char letter; guint status; } cases[] = {
    { 'R', GLIBTOP_PROCESS_RUNNING },
    { 'S', GLIBTOP_PROCESS_INTERRUPTIBLE },
    { 'D', GLIBTOP_PROCESS_UNINTERRUPTIBLE },
    { 'T', GLIBTOP_PROCESS_STOPPED },
    { 'Z', GLIBTOP_PROCESS_ZOMBIE },
    { 'I', 0 },
  };

  for (const auto &c : cases)
    {
      record.state = c.letter;
      g_assert_cmpuint (figures_of (make_window (NULL), record).status, ==, c.status);
    }
}

static void
test_the_cgroup_path_points_into_the_string_table (void)
{
  gsm_process record = make_record ();
  const char strings[] = "xx/user.slice/a.scope\0yy";

  GsmProcWindow window = make_window (NULL);

  window.strings = strings;
  window.strings_len = sizeof (strings) - 1;

  g_assert_null (figures_of (window, record).cgroup_path);

  record.known |= GSM_PROCESS_HAS_CGROUP;
  record.cgroup_offset = 2;
  record.cgroup_len = 19;
  GsmProcFigures figures = figures_of (window, record);

  g_assert_nonnull (figures.cgroup_path);
  g_assert_cmpuint (figures.cgroup_path_len, ==, 19);
  g_assert_cmpint (strncmp (figures.cgroup_path, "/user.slice/a.scope", 19), ==, 0);

  /* an (offset, length) outside the table is not followed */
  record.cgroup_offset = 20;
  g_assert_null (figures_of (window, record).cgroup_path);
}

/* A record with a layout-3 class, unit, user unit and session, over this
 * string table. */
static const char label_strings[] =
  "user@1000.service" "app-gnome\\x2dtweaks.scope" "session-3.scope" "org.gnome.Tweaks";

static gsm_process
make_classified_record (void)
{
  gsm_process record = make_record ();

  record.known |= GSM_PROCESS_HAS_CLASS | GSM_PROCESS_HAS_UNIT | GSM_PROCESS_HAS_USER_UNIT
                  | GSM_PROCESS_HAS_SESSION | GSM_PROCESS_HAS_OWNER_UID | GSM_PROCESS_HAS_VM_SWAP;
  record.workload_class = GSM_CLASS_APPLICATIONS;
  record.class_confidence = GSM_CONFIDENCE_HIGH;
  record.class_rule = 0;
  record.owner_uid = 1000;
  record.vm_swap_bytes = 12 * kB;
  record.unit_offset = 0;
  record.unit_length = 17;
  record.user_unit_offset = 17;
  record.user_unit_length = 25;
  record.login_session_offset = 42;
  record.login_session_length = 15;
  record.class_key_offset = 57;
  record.class_key_length = 16;
  return record;
}

static GsmProcLabels
labels_of (const gsm_process &record)
{
  GsmProcWindow window = make_window (NULL);
  GsmProcLabels labels;

  window.strings = label_strings;
  window.strings_len = sizeof (label_strings) - 1;
  gsm_proc_labels_from_record (&window, &record, &labels);
  return labels;
}

static void
test_class_name_and_tooltip_come_from_the_record (void)
{
  gsm_process record = make_classified_record ();
  GsmProcLabels labels = labels_of (record);
  const std::string rule = gsm_class_rule_name (record.class_rule);

  g_assert_true (labels.class_known);
  g_assert_cmpuint (labels.workload_class, ==, GSM_CLASS_APPLICATIONS);
  g_assert_cmpstr (labels.class_name.c_str (), ==, "Applications");
  g_assert_cmpstr (labels.class_rule.c_str (), ==, rule.c_str ());
  const std::string wanted = "Rule: " + rule + " \u00b7 Confidence: high \u00b7 Key: org.gnome.Tweaks";

  g_assert_cmpstr (labels.class_tooltip.c_str (), ==, wanted.c_str ());
}

static void
test_a_classifier_that_did_not_run_gives_no_class (void)
{
  gsm_process record = make_classified_record ();

  record.known &= ~GSM_PROCESS_HAS_CLASS;
  g_assert_false (labels_of (record).class_known);
}

static void
test_an_empty_key_reads_none_and_an_agent_shows_its_pid (void)
{
  gsm_process record = make_classified_record ();
  guint rule = 0;

  record.class_key_length = 0;
  g_assert_nonnull (strstr (labels_of (record).class_tooltip.c_str (), "Key: none"));
  g_assert_null (strstr (labels_of (record).class_tooltip.c_str (), "Agent pid"));

  for (guint i = 0; i < gsm_class_rule_count (); i++)
    if (strcmp (gsm_class_rule_name (i), "agent ancestry") == 0)
      rule = i;
  g_assert_cmpstr (gsm_class_rule_name (rule), ==, "agent ancestry");

  record.class_rule = rule;
  record.known |= GSM_PROCESS_HAS_AGENT;
  record.agent_pid = 777;
  g_assert_nonnull (strstr (labels_of (record).class_tooltip.c_str (), "Agent pid: 777"));
}

static void
test_the_shown_unit_is_the_innermost_one (void)
{
  gsm_process record = make_classified_record ();
  GsmProcLabels labels = labels_of (record);

  g_assert_cmpstr (labels.unit.c_str (), ==, "user@1000.service");
  g_assert_cmpstr (labels.user_unit.c_str (), ==, "app-gnome-tweaks.scope");
  g_assert_cmpstr (labels.shown_unit.c_str (), ==, "app-gnome-tweaks.scope");
  g_assert_cmpstr (labels.session.c_str (), ==, "session-3.scope");
  g_assert_cmpstr (labels.unit_tooltip.c_str (), ==,
                   "Unit: app-gnome-tweaks.scope\nOwned by: user@1000.service\nSession: session-3.scope");

  /* a system service has no user unit: the unit that owns the path is shown */
  record.known &= ~(GSM_PROCESS_HAS_USER_UNIT | GSM_PROCESS_HAS_SESSION);
  labels = labels_of (record);
  g_assert_cmpstr (labels.shown_unit.c_str (), ==, "user@1000.service");
  g_assert_cmpstr (labels.unit_tooltip.c_str (), ==, "Unit: user@1000.service");

  /* nothing in the cgroup path names a unit */
  record.known &= ~GSM_PROCESS_HAS_UNIT;
  labels = labels_of (record);
  g_assert_cmpstr (labels.shown_unit.c_str (), ==, "");
  g_assert_cmpstr (labels.unit_tooltip.c_str (), ==, "Unit: none");
}

static void
test_a_slice_outside_the_string_table_is_not_followed (void)
{
  gsm_process record = make_classified_record ();

  record.unit_offset = 50;
  record.unit_length = 100;
  g_assert_cmpstr (labels_of (record).unit.c_str (), ==, "");
}

static void
test_owner_and_swap_are_unknown_without_their_bits (void)
{
  gsm_process record = make_classified_record ();
  GsmProcLabels labels = labels_of (record);

  g_assert_true (labels.owner_known);
  g_assert_cmpuint (labels.owner_uid, ==, 1000);
  g_assert_true (labels.swap_known);
  g_assert_cmpuint (labels.swap_bytes, ==, 12 * kB);

  /* a process with nothing swapped out is 0 with its bit set, not unknown */
  record.vm_swap_bytes = 0;
  labels = labels_of (record);
  g_assert_true (labels.swap_known);
  g_assert_cmpuint (labels.swap_bytes, ==, 0);

  record.known &= ~(GSM_PROCESS_HAS_OWNER_UID | GSM_PROCESS_HAS_VM_SWAP);
  labels = labels_of (record);
  g_assert_false (labels.owner_known);
  g_assert_false (labels.swap_known);
  g_assert_false (figures_of (make_window (NULL), record).swap_known);
}

static void
test_swap_reaches_the_figures (void)
{
  gsm_process record = make_classified_record ();
  GsmProcFigures figures = figures_of (make_window (NULL), record);

  g_assert_true (figures.swap_known);
  g_assert_cmpuint (figures.swap_bytes, ==, 12 * kB);
  g_assert_nonnull (figures.record);
}

static void
test_whose_processes_selects_like_libgtop (void)
{
  gsm_process running = make_record ();
  gsm_process sleeping = make_record ();
  gsm_process stranger = make_record ();
  gsm_process no_uid = make_record ();

  running.state = 'R';
  stranger.uid_real = 0;
  no_uid.known &= ~GSM_PROCESS_HAS_UID;

  g_assert_true (gsm_proc_source_selects (GSM_PROC_WHOSE_ALL, 1000, &stranger));
  g_assert_true (gsm_proc_source_selects (GSM_PROC_WHOSE_ALL, 1000, &no_uid));

  /* GLIBTOP_EXCLUDE_IDLE: only the running ones */
  g_assert_true (gsm_proc_source_selects (GSM_PROC_WHOSE_ACTIVE, 1000, &running));
  g_assert_false (gsm_proc_source_selects (GSM_PROC_WHOSE_ACTIVE, 1000, &sleeping));

  /* GLIBTOP_KERN_PROC_UID: the user's own; a process whose owner is unknown is not shown */
  g_assert_true (gsm_proc_source_selects (GSM_PROC_WHOSE_USER, 1000, &sleeping));
  g_assert_false (gsm_proc_source_selects (GSM_PROC_WHOSE_USER, 1000, &stranger));
  g_assert_false (gsm_proc_source_selects (GSM_PROC_WHOSE_USER, 1000, &no_uid));
}

static const GsmProcFigures *
find_pid (const GsmProcFigures *figures,
          guint                 count,
          pid_t                 pid)
{
  for (guint i = 0; i < count; i++)
    if (figures[i].pid == pid)
      return &figures[i];
  return NULL;
}

static void
test_a_captured_proc_tree_gives_the_documented_figures (void)
{
  GsmProcSource *source = gsm_proc_source_new (fixture_proc);

  g_assert_nonnull (source);

  GsmProcFigures *figures = NULL;

  g_assert_cmpuint (gsm_proc_source_figures (source, GSM_PROC_WHOSE_ALL, 1000, 4, FALSE, &figures), ==, 0);
  g_assert_null (figures);

  g_assert_true (gsm_proc_source_sample (source));
  guint count = gsm_proc_source_figures (source, GSM_PROC_WHOSE_ALL, 1000, 4, FALSE, &figures);

  g_assert_cmpuint (count, ==, 5);
  for (guint i = 1; i < count; i++)
    g_assert_cmpint (figures[i - 1].pid, <, figures[i].pid);

  const GsmProcFigures *sleep = find_pid (figures, count, 344260);

  g_assert_nonnull (sleep);
  g_assert_cmpint (sleep->ppid, ==, 344259);
  g_assert_cmpuint (sleep->status, ==, GLIBTOP_PROCESS_INTERRUPTIBLE);
  g_assert_cmpuint (sleep->num_threads, ==, 1);
  g_assert_true (sleep->mem_known);
  g_assert_cmpuint (sleep->mem_bytes, ==, 112 * kB);
  g_assert_cmpuint (sleep->memshared_bytes, ==, 2044 * kB);
  g_assert_cmpuint (sleep->memres_bytes, ==, (112 + 2044) * kB);
  g_assert_cmpuint (sleep->vmsize_bytes, ==, 8344 * kB);
  g_assert_true (sleep->io_readable);
  g_assert_nonnull (sleep->cgroup_path);
  g_autofree char *cgroup_path = g_strndup (sleep->cgroup_path, sleep->cgroup_path_len);

  g_assert_true (g_str_has_suffix (cgroup_path, "CodeOSS-4247.scope"));
  g_assert_false (sleep->cpu_known);        /* the first sample has no baseline */
  g_assert_false (sleep->io_rate_valid);
  g_assert_false (sleep->start_time_known); /* the fixture has no /proc/stat */
  g_free (figures);

  /* a second sample of an unchanged tree: a known 0, not an unknown */
  g_assert_true (gsm_proc_source_sample (source));
  count = gsm_proc_source_figures (source, GSM_PROC_WHOSE_USER, 1000, 4, FALSE, &figures);
  sleep = find_pid (figures, count, 344260);
  g_assert_nonnull (sleep);
  g_assert_true (sleep->cpu_known);
  g_assert_cmpfloat (sleep->cpu_percent, ==, 0.0);
  g_assert_true (sleep->io_rate_valid);
  g_assert_cmpuint (sleep->disk_read_bytes_per_s, ==, 0);
  g_assert_null (find_pid (figures, count, 1));   /* pid 1 is root's */
  g_free (figures);

  /* a reset forgets the baseline */
  gsm_proc_source_reset (source);
  g_assert_true (gsm_proc_source_sample (source));
  count = gsm_proc_source_figures (source, GSM_PROC_WHOSE_ALL, 1000, 4, FALSE, &figures);
  g_assert_false (find_pid (figures, count, 344260)->cpu_known);
  g_free (figures);

  gsm_proc_source_free (source);
}

static void
test_a_missing_proc_root_does_not_sample (void)
{
  GsmProcSource *source = gsm_proc_source_new ("/nonexistent/proc-root");

  /* the collector opens, a sample over nothing readable fails or is empty:
   * either way no figure is invented */
  if (source == NULL)
    return;

  GsmProcFigures *figures = NULL;
  guint count = 0;

  if (gsm_proc_source_sample (source))
    count = gsm_proc_source_figures (source, GSM_PROC_WHOSE_ALL, 1000, 4, FALSE, &figures);
  g_assert_cmpuint (count, ==, 0);
  g_free (figures);
  gsm_proc_source_free (source);
}

int
main (int    argc,
      char **argv)
{
  g_test_init (&argc, &argv, NULL);

  if (argc < 2)
    g_error ("usage: test-procsource FIXTURE_PROC");
  fixture_proc = argv[1];

  g_test_add_func ("/procsource/memory/follows-the-definitions", test_memory_columns_follow_the_definitions);
  g_test_add_func ("/procsource/memory/unknown-bits", test_memory_without_the_status_bits_is_unknown_not_zero);
  g_test_add_func ("/procsource/time", test_cpu_time_and_start_time);
  g_test_add_func ("/procsource/cpu/share", test_cpu_share_is_cpu_ticks_over_wall_time);
  g_test_add_func ("/procsource/cpu/whole-thread-group", test_a_multithreaded_process_shows_its_whole_group_not_the_leaders_run_time);
  g_test_add_func ("/procsource/cpu/without-schedstat", test_cpu_share_is_known_without_the_scheduler_statistics);
  g_test_add_func ("/procsource/cpu/cap", test_cpu_share_is_capped_at_the_scale);
  g_test_add_func ("/procsource/cpu/unknown", test_cpu_share_is_unknown_without_a_baseline);
  g_test_add_func ("/procsource/io/cancelled", test_disk_write_excludes_cancelled_writes);
  g_test_add_func ("/procsource/io/cancelled-beyond-written", test_cancelled_writes_beyond_the_written_total_leave_zero);
  g_test_add_func ("/procsource/io/rates", test_disk_rates_come_from_two_snapshots);
  g_test_add_func ("/procsource/io/unreadable", test_disk_without_the_io_file_is_unreadable_not_zero);
  g_test_add_func ("/procsource/io/rate-unknown", test_disk_rate_is_unknown_for_a_reused_pid_or_a_backwards_counter);
  g_test_add_func ("/procsource/state", test_state_letters_map_like_libgtop);
  g_test_add_func ("/procsource/cgroup", test_the_cgroup_path_points_into_the_string_table);
  g_test_add_func ("/procsource/labels/class", test_class_name_and_tooltip_come_from_the_record);
  g_test_add_func ("/procsource/labels/no-class", test_a_classifier_that_did_not_run_gives_no_class);
  g_test_add_func ("/procsource/labels/key-and-agent", test_an_empty_key_reads_none_and_an_agent_shows_its_pid);
  g_test_add_func ("/procsource/labels/innermost-unit", test_the_shown_unit_is_the_innermost_one);
  g_test_add_func ("/procsource/labels/outside-table", test_a_slice_outside_the_string_table_is_not_followed);
  g_test_add_func ("/procsource/labels/owner-swap", test_owner_and_swap_are_unknown_without_their_bits);
  g_test_add_func ("/procsource/swap", test_swap_reaches_the_figures);
  g_test_add_func ("/procsource/whose", test_whose_processes_selects_like_libgtop);
  g_test_add_func ("/procsource/fixture", test_a_captured_proc_tree_gives_the_documented_figures);
  g_test_add_func ("/procsource/missing-root", test_a_missing_proc_root_does_not_sample);

  return g_test_run ();
}
