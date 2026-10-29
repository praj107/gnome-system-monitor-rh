/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * See procstat.h. Source: proc_stat(5).
 */

#include "procstat.h"

#include <string.h>

/* user nice system idle iowait irq softirq steal: the first eight counters
 * after the name; guest and guest_nice follow and are left out. */
#define N_SUMMED_FIELDS 8
#define MIN_FIELDS 4
#define FIELD_IDLE 3
#define FIELD_IOWAIT 4

static gboolean
parse_counter (const char *token,
               guint64    *out)
{
  char *end = NULL;

  if (token[0] < '0' || token[0] > '9')
    return FALSE;
  *out = g_ascii_strtoull (token, &end, 10);
  return *end == '\0';
}

/* "cpu" -> aggregate; "cpu12" -> 12. FALSE for any other name. */
static gboolean
parse_cpu_name (const char *name,
                gboolean   *is_aggregate,
                guint64    *number)
{
  if (!g_str_has_prefix (name, "cpu"))
    return FALSE;
  name += 3;
  *is_aggregate = name[0] == '\0';
  if (*is_aggregate)
    return TRUE;
  return parse_counter (name, number);
}

/* Fill one line's ticks; FALSE when the counters are not numbers or the
 * sum would overflow. */
static gboolean
parse_ticks (char       **fields,
             guint        n_fields,
             GsmCpuTicks *out)
{
  guint64 value[N_SUMMED_FIELDS] = { 0 };
  guint64 total = 0;
  guint64 idle;

  if (n_fields < MIN_FIELDS + 1)
    return FALSE;
  for (guint i = 0; i < N_SUMMED_FIELDS && i + 1 < n_fields; i++)
    {
      if (!parse_counter (fields[i + 1], &value[i]))
        return FALSE;
      if (value[i] > G_MAXUINT64 - total)
        return FALSE;
      total += value[i];
    }

  idle = value[FIELD_IDLE] + value[FIELD_IOWAIT];
  out->present = TRUE;
  out->idle_ticks = idle;
  out->busy_ticks = total - idle;
  return TRUE;
}

static char **
split_fields (const char *line)
{
  char **raw = g_strsplit_set (line, " \t", -1);
  guint kept = 0;

  for (guint i = 0; raw[i] != NULL; i++)
    {
      if (raw[i][0] == '\0')
        {
          g_free (raw[i]);
          continue;
        }
      raw[kept++] = raw[i];
    }
  raw[kept] = NULL;
  return raw;
}

guint
gsm_procstat_parse (const char  *text,
                    GsmCpuTicks *aggregate,
                    GsmCpuTicks *cpus,
                    guint        n_cpus)
{
  guint n_malformed = 0;
  char **lines;

  memset (aggregate, 0, sizeof *aggregate);
  if (n_cpus > 0)
    memset (cpus, 0, n_cpus * sizeof *cpus);
  if (text == NULL)
    return 0;

  lines = g_strsplit (text, "\n", -1);
  for (guint i = 0; lines[i] != NULL; i++)
    {
      char **fields;
      gboolean is_aggregate;
      guint64 number = 0;
      GsmCpuTicks ticks = { 0 };

      /* Only the "cpu…" lines matter; the rest of the file (intr, ctxt…)
       * is not parsed. */
      if (!g_str_has_prefix (lines[i], "cpu"))
        continue;
      fields = split_fields (lines[i]);
      if (fields[0] == NULL || !parse_cpu_name (fields[0], &is_aggregate, &number)
          || !parse_ticks (fields, g_strv_length (fields), &ticks))
        {
          n_malformed++;
          g_strfreev (fields);
          continue;
        }
      if (is_aggregate)
        *aggregate = ticks;
      else if (number < n_cpus)
        cpus[number] = ticks;
      g_strfreev (fields);
    }
  g_strfreev (lines);
  return n_malformed;
}

gboolean
gsm_procstat_read (const char  *path,
                   GsmCpuTicks *aggregate,
                   GsmCpuTicks *cpus,
                   guint        n_cpus)
{
  char *text = NULL;

  /* The file can vanish with the kernel config; ENOENT is a normal outcome. */
  if (!g_file_get_contents (path, &text, NULL, NULL))
    {
      gsm_procstat_parse (NULL, aggregate, cpus, n_cpus);
      return FALSE;
    }
  gsm_procstat_parse (text, aggregate, cpus, n_cpus);
  g_free (text);
  return TRUE;
}

gboolean
gsm_cpu_ticks_busy_percent (const GsmCpuTicks *before,
                            const GsmCpuTicks *after,
                            double            *percent)
{
  guint64 busy_delta, idle_delta;

  if (!before->present || !after->present)
    return FALSE;
  if (after->busy_ticks < before->busy_ticks || after->idle_ticks < before->idle_ticks)
    return FALSE;

  busy_delta = after->busy_ticks - before->busy_ticks;
  idle_delta = after->idle_ticks - before->idle_ticks;
  if (busy_delta > G_MAXUINT64 - idle_delta || busy_delta + idle_delta == 0)
    return FALSE;

  *percent = 100.0 * (double) busy_delta / (double) (busy_delta + idle_delta);
  return TRUE;
}

guint
gsm_cpu_slots_from_list (const char *text)
{
  guint64 highest = 0;
  gboolean any = FALSE;
  const char *p = text;

  if (text == NULL)
    return 0;
  while (*p != '\0' && *p != '\n')
    {
      char *end = NULL;
      guint64 number;

      if (*p < '0' || *p > '9')
        return 0;
      number = g_ascii_strtoull (p, &end, 10);
      if (number > G_MAXUINT - 1)
        return 0;
      highest = MAX (highest, number);
      any = TRUE;
      p = end;
      /* a range "a-b" continues with b; a list continues after a comma */
      if (*p == '-')
        {
          p++;
          if (*p < '0' || *p > '9')
            return 0;
          number = g_ascii_strtoull (p, &end, 10);
          if (number > G_MAXUINT - 1)
            return 0;
          highest = MAX (highest, number);
          p = end;
        }
      if (*p == ',')
        p++;
      else if (*p != '\0' && *p != '\n')
        return 0;
    }
  return any ? (guint) highest + 1 : 0;
}

guint
gsm_cpu_slot_count (const char *sysfs_root,
                    guint       online_cpus)
{
  char *path = g_build_filename (sysfs_root, "devices", "system", "cpu", "present", NULL);
  char *text = NULL;
  guint slots = 0;

  /* The file can be missing on unusual kernels; fall back to what we know. */
  if (g_file_get_contents (path, &text, NULL, NULL))
    slots = gsm_cpu_slots_from_list (text);
  g_free (text);
  g_free (path);
  return slots > 0 ? slots : MAX (online_cpus, 1);
}
