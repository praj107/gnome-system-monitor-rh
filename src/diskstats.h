/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Whole-disk I/O totals from /proc/diskstats, one file read per refresh.
 * Follows Documentation/admin-guide/iostats.rst: a line is
 * "major minor name" followed by at least 11 counters (14 fields in all;
 * newer kernels append more), and the sector counters are always in units
 * of 512 bytes whatever the device's own sector size.
 *
 * Only whole disks with real storage are counted. A device is one when it
 * has a directory /sys/block/<name> (partitions are listed under their
 * disk instead, so counting them would double the bytes) and is not a
 * loop, ram or zram device (RAM, not storage) or a device-mapper or md
 * device (stacked on disks that are already counted). Pure functions, no
 * GTK: the paths are parameters so tests use fixture trees.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct
{
  guint64 read_bytes;         /* sectors read x 512, summed over the disks */
  guint64 written_bytes;      /* sectors written x 512 */
  guint   n_disks;            /* devices counted */
  guint   n_malformed_lines;  /* lines skipped: too few fields or not numbers */
  guint64 device_set_hash;    /* identifies which devices were counted, so a
                               * caller can tell a disk appearing or vanishing
                               * from a burst of I/O */
} GsmDiskTotals;

/* Sum the counters in the file's text. sysfs_root is normally "/sys". */
void gsm_diskstats_parse (const char    *text,
                          const char    *sysfs_root,
                          GsmDiskTotals *out);

/* Read diskstats_path (normally "/proc/diskstats") and parse it. FALSE, with
 * *out zeroed, when the file cannot be read: the totals are then unknown. */
gboolean gsm_diskstats_read (const char    *diskstats_path,
                             const char    *sysfs_root,
                             GsmDiskTotals *out);

/* Whether a device name from diskstats is a whole physical disk under
 * sysfs_root (see above). */
gboolean gsm_diskstats_is_physical_disk (const char *name,
                                         const char *sysfs_root);

G_END_DECLS
