/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * See meminfo.h. Source: Documentation/filesystems/proc.rst (meminfo).
 */

#include "meminfo.h"

#include <stdlib.h>
#include <string.h>

typedef struct
{
  const char *key;
  gsize       offset;
} FieldSpec;

#define FIELD(name, member) { name, G_STRUCT_OFFSET (GsmMemInfo, member) }

/* Data, not an if chain: one row per meminfo key the partition uses. */
static const FieldSpec FIELDS[] = {
  FIELD ("MemTotal",        mem_total_bytes),
  FIELD ("MemFree",         mem_free_bytes),
  FIELD ("MemAvailable",    mem_available_bytes),
  FIELD ("Buffers",         buffers_bytes),
  FIELD ("Cached",          cached_bytes),
  FIELD ("Shmem",           shmem_bytes),
  FIELD ("AnonPages",       anon_pages_bytes),
  FIELD ("SReclaimable",    s_reclaimable_bytes),
  FIELD ("SUnreclaim",      s_unreclaim_bytes),
  FIELD ("KReclaimable",    k_reclaimable_bytes),
  FIELD ("KernelStack",     kernel_stack_bytes),
  FIELD ("PageTables",      page_tables_bytes),
  FIELD ("SecPageTables",   sec_page_tables_bytes),
  FIELD ("Percpu",          percpu_bytes),
  FIELD ("VmallocUsed",     vmalloc_used_bytes),
  FIELD ("Dirty",           dirty_bytes),
  FIELD ("Writeback",       writeback_bytes),
};

static guint64 *
field_slot (GsmMemInfo *info,
            gsize       offset)
{
  return (guint64 *) (((char *) info) + offset);
}

static void
parse_line (const char *line,
            gsize       line_len,
            GsmMemInfo *out)
{
  const char *colon = memchr (line, ':', line_len);
  if (colon == NULL)
    return;

  gsize key_len = colon - line;
  for (gsize i = 0; i < G_N_ELEMENTS (FIELDS); i++)
    {
      if (strlen (FIELDS[i].key) != key_len || strncmp (FIELDS[i].key, line, key_len) != 0)
        continue;

      /* The value is a decimal number of kB; anything else is "missing". */
      char *end = NULL;
      guint64 kib = g_ascii_strtoull (colon + 1, &end, 10);
      if (end == colon + 1 || kib > G_MAXUINT64 / 1024 - 1)
        return;
      *field_slot (out, FIELDS[i].offset) = kib * 1024;
      return;
    }
}

void
gsm_meminfo_parse (const char *text,
                   GsmMemInfo *out)
{
  for (gsize i = 0; i < G_N_ELEMENTS (FIELDS); i++)
    *field_slot (out, FIELDS[i].offset) = GSM_MEMINFO_MISSING;

  if (text == NULL)
    return;

  const char *line = text;
  while (*line != '\0')
    {
      const char *newline = strchr (line, '\n');
      gsize line_len = newline ? (gsize) (newline - line) : strlen (line);
      parse_line (line, line_len, out);
      if (newline == NULL)
        break;
      line = newline + 1;
    }
}

gboolean
gsm_meminfo_read (GsmMemInfo *out)
{
  char *text = NULL;
  gboolean ok = g_file_get_contents ("/proc/meminfo", &text, NULL, NULL);

  gsm_meminfo_parse (ok ? text : NULL, out);
  g_free (text);
  return ok;
}

static gboolean
present (guint64 value)
{
  return value != GSM_MEMINFO_MISSING;
}

/* Fields newer kernels add or older ones drop count as 0 in the sum; the
 * required ones decide validity. */
static guint64
or_zero (guint64 value)
{
  return present (value) ? value : 0;
}

void
gsm_meminfo_partition (const GsmMemInfo *info,
                       GsmMemPartition  *out)
{
  memset (out, 0, sizeof *out);

  gboolean complete = present (info->mem_total_bytes)
                      && present (info->mem_free_bytes)
                      && present (info->buffers_bytes)
                      && present (info->cached_bytes)
                      && present (info->shmem_bytes)
                      && present (info->anon_pages_bytes)
                      && present (info->s_reclaimable_bytes)
                      && present (info->s_unreclaim_bytes)
                      && present (info->kernel_stack_bytes)
                      && present (info->page_tables_bytes)
                      && info->cached_bytes >= info->shmem_bytes;
  if (!complete)
    return;

  out->total_bytes = info->mem_total_bytes;
  out->application_private_bytes = info->anon_pages_bytes;
  out->ram_backed_files_bytes = info->shmem_bytes;
  out->file_cache_bytes = info->cached_bytes - info->shmem_bytes + info->buffers_bytes;
  out->kernel_reclaimable_bytes = MAX (info->s_reclaimable_bytes, or_zero (info->k_reclaimable_bytes));
  out->kernel_fixed_bytes = info->s_unreclaim_bytes
                            + info->kernel_stack_bytes
                            + info->page_tables_bytes
                            + or_zero (info->sec_page_tables_bytes)
                            + or_zero (info->percpu_bytes)
                            + or_zero (info->vmalloc_used_bytes);
  out->free_bytes = info->mem_free_bytes;

  guint64 accounted = out->application_private_bytes
                      + out->ram_backed_files_bytes
                      + out->file_cache_bytes
                      + out->kernel_reclaimable_bytes
                      + out->kernel_fixed_bytes
                      + out->free_bytes;

  if (accounted > out->total_bytes)
    out->overlap_bytes = accounted - out->total_bytes;
  else
    out->unaccounted_bytes = out->total_bytes - accounted;

  out->valid = TRUE;
}
