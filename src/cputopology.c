/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * See cputopology.h. Sources: Documentation/admin-guide/cputopology.rst and
 * Documentation/ABI/stable/sysfs-devices-system-cpu.
 */

#include "cputopology.h"

#include <stdlib.h>
#include <string.h>

/* Entries of a kernel CPU list are tiny; a file larger than this is not one. */
#define CPU_LIST_MAX_BYTES 4096

gboolean
gsm_cpu_list_parse (const char *text,
                    gboolean   *members,
                    guint       n_members)
{
  const char *p = text;
  gboolean any = FALSE;

  for (guint i = 0; i < n_members; i++)
    members[i] = FALSE;

  if (text == NULL)
    return FALSE;

  while (*p != '\0' && *p != '\n')
    {
      char *end = NULL;
      guint64 first = g_ascii_strtoull (p, &end, 10);
      guint64 last;

      if (end == p)
        goto malformed;
      last = first;
      p = end;
      if (*p == '-')
        {
          p++;
          last = g_ascii_strtoull (p, &end, 10);
          if (end == p || last < first)
            goto malformed;
          p = end;
        }
      if (*p != ',' && *p != '\0' && *p != '\n')
        goto malformed;

      for (guint64 cpu = first; cpu <= last && cpu < n_members; cpu++)
        members[cpu] = TRUE;
      any = TRUE;

      if (*p == ',')
        {
          p++;
          if (*p == '\0' || *p == '\n')
            goto malformed;
        }
    }

  if (!any)
    return FALSE;
  return TRUE;

malformed:
  for (guint i = 0; i < n_members; i++)
    members[i] = FALSE;
  return FALSE;
}

/* A list file as membership flags; FALSE when it is missing or unparseable
 * (the files vanish with the hardware: ENOENT is a normal outcome). */
static gboolean
read_cpu_list_file (const char *path,
                    gboolean   *members,
                    guint       n_members)
{
  char *text = NULL;
  gsize length = 0;
  gboolean ok;

  for (guint i = 0; i < n_members; i++)
    members[i] = FALSE;

  if (!g_file_get_contents (path, &text, &length, NULL))
    return FALSE;
  if (length > CPU_LIST_MAX_BYTES)
    {
      g_free (text);
      return FALSE;
    }
  ok = gsm_cpu_list_parse (text, members, n_members);
  g_free (text);
  return ok;
}

static gboolean
read_siblings (const char *sysfs_root,
               guint       cpu,
               gboolean   *siblings,
               guint       n_cpus)
{
  char *path = g_strdup_printf ("%s/devices/system/cpu/cpu%u/topology/thread_siblings_list",
                                sysfs_root, cpu);
  gboolean ok = read_cpu_list_file (path, siblings, n_cpus);

  g_free (path);
  return ok;
}

static guint
first_member (const gboolean *members,
              guint           n_members)
{
  for (guint i = 0; i < n_members; i++)
    if (members[i])
      return i;
  return n_members;
}

void
gsm_cpu_topology_read (const char     *sysfs_root,
                       guint           n_cpus,
                       GsmCpuTopology *out)
{
  gboolean *performance = g_new0 (gboolean, n_cpus + 1);
  gboolean *efficiency = g_new0 (gboolean, n_cpus + 1);
  gboolean *siblings = g_new0 (gboolean, n_cpus + 1);
  char *core_path = g_strdup_printf ("%s/devices/cpu_core/cpus", sysfs_root);
  char *atom_path = g_strdup_printf ("%s/devices/cpu_atom/cpus", sysfs_root);

  out->n_cpus = n_cpus;
  out->cpus = g_new0 (GsmCpuInfo, n_cpus + 1);

  /* Hybrid only when both kinds are listed: one file alone says nothing
   * about the other CPUs. */
  gboolean hybrid = read_cpu_list_file (core_path, performance, n_cpus)
                    && read_cpu_list_file (atom_path, efficiency, n_cpus);

  for (guint cpu = 0; hybrid && cpu < n_cpus; cpu++)
    {
      GsmCpuInfo *info = &out->cpus[cpu];
      const gboolean *kind_members;

      if (performance[cpu] == efficiency[cpu]) /* in neither, or in both */
        continue;
      kind_members = performance[cpu] ? performance : efficiency;

      if (!read_siblings (sysfs_root, cpu, siblings, n_cpus) || !siblings[cpu])
        continue;

      /* Every thread of a core is the same kind; anything else is a tree
       * we do not understand. */
      gboolean consistent = TRUE;
      guint count = 0;
      guint index = 0;
      for (guint s = 0; s < n_cpus; s++)
        {
          if (!siblings[s])
            continue;
          if (!kind_members[s])
            consistent = FALSE;
          if (s < cpu)
            index++;
          count++;
        }
      if (!consistent)
        continue;

      info->kind = performance[cpu] ? GSM_CPU_KIND_PERFORMANCE : GSM_CPU_KIND_EFFICIENCY;
      info->first_cpu = first_member (siblings, n_cpus);
      info->thread_index = index;
      info->thread_count = count;
    }

  /* Number the cores of each kind by their first thread: a core's number is
   * one more than the count of same-kind cores whose first thread is lower. */
  for (guint cpu = 0; cpu < n_cpus; cpu++)
    {
      GsmCpuInfo *info = &out->cpus[cpu];
      guint lower_cores = 0;

      if (info->kind == GSM_CPU_KIND_UNKNOWN)
        continue;
      for (guint other = 0; other < info->first_cpu; other++)
        if (out->cpus[other].kind == info->kind && out->cpus[other].first_cpu == other)
          lower_cores++;
      info->core_number = lower_cores + 1;
    }

  g_free (performance);
  g_free (efficiency);
  g_free (siblings);
  g_free (core_path);
  g_free (atom_path);
}

void
gsm_cpu_topology_clear (GsmCpuTopology *topology)
{
  g_clear_pointer (&topology->cpus, g_free);
  topology->n_cpus = 0;
}

char *
gsm_cpu_topology_label (const GsmCpuTopology *topology,
                        guint                 cpu)
{
  const GsmCpuInfo *info;
  char kind_letter;

  if (cpu >= topology->n_cpus || topology->cpus[cpu].kind == GSM_CPU_KIND_UNKNOWN)
    return g_strdup_printf ("CPU %u", cpu);

  info = &topology->cpus[cpu];
  kind_letter = info->kind == GSM_CPU_KIND_PERFORMANCE ? 'P' : 'E';
  if (info->thread_count < 2 || info->thread_index >= 26)
    return g_strdup_printf ("%c%u", kind_letter, info->core_number);
  return g_strdup_printf ("%c%u%c", kind_letter, info->core_number, 'a' + (char) info->thread_index);
}
