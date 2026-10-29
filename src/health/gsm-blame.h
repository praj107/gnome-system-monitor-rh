/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <stdint.h>

#include <glib.h>

G_BEGIN_DECLS

/*
 * One cgroup's I/O against one block device, as of right now (cumulative
 * counters, as io.stat reports them).
 *
 * `cgroup` is the path relative to the cgroup root and is unique per
 * *instance* — every launch of an app gets a fresh scope
 * (app-org.chromium.Chromium-4126.scope), and the counters inside it start
 * at zero. `name` is that collapsed back down to a stable identity
 * ("org.chromium.Chromium") so writes can be accumulated across launches.
 */
typedef struct
{
  char    *cgroup;
  char    *name;
  guint major;
  guint minor;
  uint64_t wbytes;
  uint64_t rbytes;
} GsmBlameEntry;

void        gsm_blame_entry_free (GsmBlameEntry *entry);

/*
 * Snapshot every cgroup that carries io.stat, interior nodes included.
 * Unprivileged. The counters are cumulative for the lifetime of the cgroup,
 * and an interior node's counters already include its descendants (see
 * Documentation/admin-guide/cgroup-v2.rst, "IO"); turning snapshots into
 * non-overlapping per-interval bytes is the tracker's job, below.
 */
GPtrArray * gsm_blame_collect (void);

/* Collapse an instance cgroup name to a stable identity. Exposed for tests. */
char *      gsm_blame_normalize_name (const char *cgroup_basename);

/*
 * The name a cgroup is shown under, from its path relative to the cgroup
 * root. A unit (.service, .scope) is named by the normaliser, a slice by its
 * own name with systemd's \xNN escapes decoded (never by its top-level
 * ancestor), a plain sub-cgroup by the unit it lives in. The root is
 * "kernel and unattributed". Exposed for tests.
 */
char *      gsm_blame_node_name (const char *relative_path);

/* How a row of bytes was arrived at. */
#define GSM_BLAME_BASIS_KERNEL  "kernel"    /* the kernel charged this cgroup */
#define GSM_BLAME_BASIS_RESIDUE "residue"   /* a node's total minus its children's */

/*
 * Bytes written to one device by one named writer during one interval.
 * Per device and interval the rows of one update sum exactly to the root
 * cgroup's own delta (except when a counter ran backwards; see
 * gsm_blame_tracker_update).
 */
typedef struct
{
  char       *cgroup;     /* node the row came from, relative to the root */
  char       *name;
  guint major;
  guint minor;
  uint64_t wbytes;
  const char *basis;      /* GSM_BLAME_BASIS_*, static */
} GsmBlameDelta;

void gsm_blame_delta_free (GsmBlameDelta *delta);

typedef struct _GsmBlameTracker GsmBlameTracker;

/*
 * `cgroup_root` is the cgroup2 mount point; NULL means /sys/fs/cgroup. Tests
 * pass a tree they built.
 */
GsmBlameTracker * gsm_blame_tracker_new (const char *cgroup_root);
void              gsm_blame_tracker_free (GsmBlameTracker *tracker);

/*
 * Read the tree and return a GPtrArray of GsmBlameDelta for the interval
 * since the previous call. The first call only sets the baseline and returns
 * no rows: bytes written before tracking began are not "just now".
 *
 * Each node's delta is split into what its children account for (a child
 * alive at both reads contributes its own delta; a child born in the
 * interval, or carrying a new inode under an old name, its whole counter)
 * and a residue: the node's own writes, the final writes of children that
 * exited, and children that lived and died between two reads. A residue that
 * would be negative (a child read a moment later than its parent) is clamped
 * to zero and the shortfall is held back from the node's next residue.
 */
GPtrArray *       gsm_blame_tracker_update (GsmBlameTracker *tracker);

/*
 * Rows for a device the drive map does not know yet. Dropping them would
 * lose bytes (a drive that has just appeared is exactly when it matters), so
 * they wait here, summed per device, name and basis, until a refresh of the
 * map either resolves the device or shows it is not a tracked drive.
 */
typedef struct _GsmBlameHold GsmBlameHold;

/* The drive id for a device, or NULL when the map does not know it. */
typedef const char * (*GsmBlameDriveLookup) (guint    major,
                                             guint    minor,
                                             gpointer user_data);
typedef void (*GsmBlameSink) (const char          *drive_id,
                              const GsmBlameDelta *delta,
                              gpointer             user_data);

GsmBlameHold * gsm_blame_hold_new (void);
void           gsm_blame_hold_free (GsmBlameHold *hold);
guint          gsm_blame_hold_count (GsmBlameHold *hold);

/*
 * Hand `deltas` (may be NULL) and everything held to `sink` for each device
 * `lookup` knows, once; keep the rest. With `discard_unmapped`, call this
 * right after a *successful* refresh of the map: what is still unknown then
 * is not a tracked drive and is dropped instead of kept.
 */
void           gsm_blame_hold_resolve (GsmBlameHold       *hold,
                                       GPtrArray          *deltas,
                                       GsmBlameDriveLookup lookup,
                                       GsmBlameSink        sink,
                                       gpointer            user_data,
                                       gboolean            discard_unmapped);

/* After an update: was io delegated into user.slice's children? */
gboolean gsm_blame_tracker_user_io_is_delegated (GsmBlameTracker *tracker);

/*
 * Does this system account for I/O *inside* the user session?
 *
 * By default systemd delegates cpu, memory and pids into user.slice but not
 * io, so io.stat exists at /user.slice and nowhere beneath it and the whole
 * desktop collapses into one opaque number. When it is delegated, we get real
 * per-app cgroups and blame becomes exact rather than estimated.
 *
 * Both the sampler (to decide whether to read /proc as well) and the UI (to
 * decide whether to offer the fix) need this answer, and they must never
 * disagree — hence one implementation, here, next to the collector it reads.
 */
gboolean gsm_blame_user_io_is_delegated (GPtrArray *cgroups);

G_END_DECLS
