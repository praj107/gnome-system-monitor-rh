/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * /proc/meminfo parsing and a partition of MemTotal into rows that say
 * what each byte is: Application private (AnonPages), RAM-backed files
 * (Shmem: tmpfs and shared memory, not reclaimable without swap), File
 * cache (Cached - Shmem + Buffers), Kernel reclaimable, Kernel fixed, Free,
 * and an explicit Unaccounted residue. Pure functions, no GTK: the text goes
 * in, bytes come out. Follows Documentation/filesystems/proc.rst (meminfo).
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* A field that is not in the text. Never 0: a missing field is unknown. */
#define GSM_MEMINFO_MISSING G_MAXUINT64

/* All values in bytes (the file's kB times 1024), or GSM_MEMINFO_MISSING. */
typedef struct
{
  guint64 mem_total_bytes;
  guint64 mem_free_bytes;
  guint64 mem_available_bytes;
  guint64 buffers_bytes;
  guint64 cached_bytes;
  guint64 shmem_bytes;
  guint64 anon_pages_bytes;
  guint64 s_reclaimable_bytes;
  guint64 s_unreclaim_bytes;
  guint64 k_reclaimable_bytes;
  guint64 kernel_stack_bytes;
  guint64 page_tables_bytes;
  guint64 sec_page_tables_bytes;
  guint64 percpu_bytes;
  guint64 vmalloc_used_bytes;
  guint64 dirty_bytes;
  guint64 writeback_bytes;
} GsmMemInfo;

/* The rows of the partition. valid is FALSE when a field the rows need is
 * missing; the other members are then 0 and must not be shown. */
typedef struct
{
  gboolean valid;
  guint64 total_bytes;
  guint64 application_private_bytes; /* AnonPages */
  guint64 ram_backed_files_bytes;    /* Shmem: not reclaimable without swap */
  guint64 file_cache_bytes;          /* Cached - Shmem + Buffers */
  guint64 kernel_reclaimable_bytes;  /* max (SReclaimable, KReclaimable) */
  guint64 kernel_fixed_bytes;        /* SUnreclaim + KernelStack + PageTables
                                      * + SecPageTables + Percpu + VmallocUsed
                                      * (approximate: VmallocUsed may overlap) */
  guint64 free_bytes;                /* MemFree */
  guint64 unaccounted_bytes;         /* total - sum of the rows, never negative */
  guint64 overlap_bytes;             /* sum of the rows - total when positive, else 0 */
} GsmMemPartition;

void     gsm_meminfo_parse (const char *text,
                            GsmMemInfo *out);

gboolean gsm_meminfo_read (GsmMemInfo *out);

void     gsm_meminfo_partition (const GsmMemInfo  *info,
                                GsmMemPartition   *out);

G_END_DECLS
