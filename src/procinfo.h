/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <string>

#include <gdkmm/texture.h>
#include <glib.h>
#include <gtk/gtk.h>
#include <sys/types.h>

#include "util.h"

struct MutableProcInfo
  : private procman::NonCopyable
{
  MutableProcInfo ()
    : vmsize (0UL),
    memres (0UL),
    memshared (0UL),
    memwritable (0UL),
    mem (0UL),
    start_time (0UL),
    vmsize_known (true),
    memres_known (true),
    memshared_known (true),
    mem_known (true),
    start_time_known (true),
    cpu_time (0ULL),
    disk_read_bytes_total (0ULL),
    disk_write_bytes_total (0ULL),
    disk_read_bytes_current (0ULL),
    disk_write_bytes_current (0ULL),
    status (0U),
    pcpu (0),
    cpu_known (false),
    cpu_baseline_valid (false),
    nice (0),
    num_threads (0U),
    io_readable (false),
    io_rate_valid (false),
    io_sample_us (0),
    gpu_percent (0.0),
    gpu_mem (0UL),
    gpu_percent_known (false),
    gpu_mem_known (false),
    class_known (false),
    workload_class (0),
    swap_known (false),
    swap (0UL)
  {
  }

  std::string user;

  std::string wchan;

  // all these members are filled with libgtop which uses
  // guint64 (to have fixed size data) but we don't need more
  // than an unsigned long (even for 32bit apps on a 64bit
  // kernel) as these data are amounts, not offsets.
  gulong vmsize;
  gulong memres;
  gulong memshared;
  gulong memwritable;
  gulong mem;

  gulong start_time;
  // The libgtop path always has these figures; the collector path leaves one
  // unknown (shown as "—", left out of the totals) when the kernel did not
  // provide the counters it is made of.
  bool vmsize_known;
  bool memres_known;
  bool memshared_known;
  bool mem_known;
  bool start_time_known;
  guint64 cpu_time;
  guint64 disk_read_bytes_total;
  guint64 disk_write_bytes_total;
  guint64 disk_read_bytes_current;
  guint64 disk_write_bytes_current;
  guint status;
  gdouble pcpu;
  // pcpu is a rate between two readings of the CPU counter: unknown on the
  // first sample, after a failed read, a backwards counter or a reused pid.
  bool cpu_known;
  // the stored cpu_time / start_time come from a good reading of this process
  bool cpu_baseline_valid;
  gint nice;
  guint num_threads;
  // /proc/<pid>/io is readable only for the user's own processes; an
  // unreadable value is unknown and shown as such, never as 0.
  bool io_readable;
  // a rate needs two readable samples: the first one has none
  bool io_rate_valid;
  gint64 io_sample_us; // g_get_monotonic_time () of the last readable sample
  gdouble gpu_percent;
  gulong gpu_mem;
  // unknown unless the GPU query reported the process
  bool gpu_percent_known;
  bool gpu_mem_known;
  std::string cgroup_name;

  // The workload class the collector's rules give the process (a
  // GSM_CLASS_* value). Unknown on the libgtop path, which has no classifier.
  bool class_known;
  guint workload_class;
  std::string class_name;
  std::string class_rule;
  // VmSwap; unknown unless the collector read it.
  bool swap_known;
  gulong swap;

  // The unit that owns the cgroup path (systemd's cg_path_get_unit): the
  // definition both sources share, and what the row comparison reads.
  std::string unit;
  // The innermost unit, shown in the Unit column: the unit inside the user
  // manager or login session when there is one. On the libgtop path it is
  // `unit`, as libsystemd's per-process call gives only that.
  std::string unit_label;
  std::string user_unit;
  // Hover text of the Unit and Class cells; empty on the libgtop path.
  std::string unit_tooltip;
  std::string class_tooltip;
  std::string session;
  std::string seat;

  std::string owner;
  std::string process_type;
};


class ProcInfo
  : public MutableProcInfo
{
public:
  ProcInfo& operator= (const ProcInfo&) = delete;
  ProcInfo(const ProcInfo&) = delete;
  ProcInfo(pid_t pid);

  // adds one more ref to icon
  void        set_icon (Glib::RefPtr<Gdk::Texture> icon);
  void        set_user (guint uid);
  std::string lookup_user (guint uid);

  GtkTreeIter node;
  Glib::RefPtr<Gdk::Texture> icon;
  std::string tooltip;
  std::string name;
  std::string arguments;

  std::string security_context;

  const pid_t pid;
  pid_t ppid;
  guint uid;
};

G_BEGIN_DECLS

void        gsm_proc_info_load_cgroups   (ProcInfo     *self);
void        gsm_proc_info_load_selinux   (ProcInfo     *self);
void        gsm_proc_info_load_systemd   (ProcInfo     *self);

G_END_DECLS
