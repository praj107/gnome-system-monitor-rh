/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <stdint.h>

#include <glib.h>

G_BEGIN_DECLS

/*
 * One process's cumulative block-layer I/O, from /proc/<pid>/io.
 *
 * This exists because cgroup io.stat cannot see desktop apps: systemd
 * delegates cpu/memory/pids into user.slice but not io, so the whole user
 * session shows up as one opaque lump. /proc/<pid>/io is readable without
 * privilege for our own processes, which is precisely the set of apps a
 * desktop user cares about.
 *
 * The tradeoff is that it reports no device. `major`/`minor` are our best
 * guess at which drive the writes landed on, and `exact` is always FALSE —
 * the UI must present these rows as estimates, never as measurements.
 */
typedef struct {
  int      pid;
  char    *name;
  uint64_t wbytes;      /* write_bytes: actually submitted to the block layer */
  uint64_t rbytes;
  guint    major;       /* 0 if we could not work out a drive */
  guint    minor;
  gboolean exact;       /* always FALSE here; kept so blame rows are uniform */
} GsmProcIoEntry;

void       gsm_proc_io_entry_free (GsmProcIoEntry *entry);

/*
 * Snapshot every process whose /proc/<pid>/io we are allowed to read.
 * Unprivileged: in practice this is every process the user owns.
 */
GPtrArray *gsm_proc_io_collect    (void);

/*
 * Map a filesystem's dev_t to the whole disk that backs it, so a write to
 * /dev/nvme0n1p2 is charged to nvme0n1 — cgroup io.stat keys on the whole
 * disk, and the two have to agree for the blame chart to add up.
 *
 * Returns FALSE for anonymous devices (btrfs, overlayfs, tmpfs), which have
 * no block device to point at.
 */
gboolean   gsm_disk_for_devt      (guint  dev_major,
                                   guint  dev_minor,
                                   guint *out_major,
                                   guint *out_minor);

G_END_DECLS
