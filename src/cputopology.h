/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * What each logical CPU is, read from the kernel's topology files under a
 * sysfs root: a performance-core thread, its SMT sibling, an efficiency
 * core, or unknown. Follows Documentation/ABI/stable/sysfs-devices-system-cpu
 * (topology/thread_siblings_list) and Documentation/admin-guide/cputopology.rst;
 * the hybrid split is /sys/devices/cpu_core/cpus and /sys/devices/cpu_atom/cpus
 * (the kernel's per-PMU cpumask files). Pure functions, no GTK: the sysfs
 * root is a parameter so tests read fixture trees.
 *
 * A CPU whose kind cannot be established from those files is "unknown" and
 * labelled with its number; it is never guessed from frequencies.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum
{
  GSM_CPU_KIND_UNKNOWN,
  GSM_CPU_KIND_PERFORMANCE, /* member of /sys/devices/cpu_core/cpus */
  GSM_CPU_KIND_EFFICIENCY   /* member of /sys/devices/cpu_atom/cpus */
} GsmCpuKind;

typedef struct
{
  GsmCpuKind kind;
  guint      core_number;  /* 1-based among cores of this kind, numbered by
                            * first thread; 0 when kind is unknown */
  guint      thread_index; /* 0-based position among the core's threads */
  guint      thread_count; /* threads on the core (1 without SMT) */
  guint      first_cpu;    /* lowest CPU number sharing this core */
} GsmCpuInfo;

typedef struct
{
  guint       n_cpus;
  GsmCpuInfo *cpus; /* n_cpus entries, indexed by CPU number */
} GsmCpuTopology;

/* Parse a kernel CPU list ("0-11", "0-3,8,10-11", trailing newline allowed)
 * into members[0..n_members): members[i] is TRUE when CPU i is listed. CPUs
 * at or beyond n_members are ignored. Returns FALSE (members all FALSE) when
 * the text is empty or malformed. */
gboolean gsm_cpu_list_parse (const char *text,
                             gboolean   *members,
                             guint       n_members);

/* Read the topology of CPUs 0..n_cpus-1 under sysfs_root (normally "/sys").
 * Always fills a table; every CPU is GSM_CPU_KIND_UNKNOWN unless both hybrid
 * files are readable and the CPU's thread_siblings_list agrees with them. */
void gsm_cpu_topology_read (const char     *sysfs_root,
                            guint           n_cpus,
                            GsmCpuTopology *out);

void gsm_cpu_topology_clear (GsmCpuTopology *topology);

/* "P1a", "P1b", "E1", or "CPU 7" when unknown. Free with g_free. */
char *gsm_cpu_topology_label (const GsmCpuTopology *topology,
                              guint                 cpu);

G_END_DECLS
