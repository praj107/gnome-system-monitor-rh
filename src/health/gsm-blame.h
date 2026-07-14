/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <stdint.h>

#include <glib.h>

G_BEGIN_DECLS

/*
 * One cgroup's I/O against one block device, as of right now.
 *
 * `cgroup` is the full path and is unique per *instance* — every launch of an
 * app gets a fresh scope (app-org.chromium.Chromium-4126.scope), and the
 * counters inside it start at zero. `name` is that collapsed back down to a
 * stable identity ("org.chromium.Chromium") so writes can be accumulated
 * across launches.
 */
typedef struct {
  char    *cgroup;
  char    *name;
  guint    major;
  guint    minor;
  uint64_t wbytes;
  uint64_t rbytes;
} GsmBlameEntry;

void       gsm_blame_entry_free (GsmBlameEntry *entry);

/*
 * Snapshot every leaf cgroup's per-device I/O counters. Unprivileged.
 *
 * Counters are cumulative *for the lifetime of that cgroup*, not of the
 * machine: they vanish when the app exits. Turning these snapshots into
 * durable per-app totals is the sampler's job (see gsm-sampler.c) — that is
 * why long-horizon blame needs background sampling to be meaningful.
 */
GPtrArray *gsm_blame_collect     (void);

/* Collapse an instance cgroup name to a stable identity. Exposed for tests. */
char      *gsm_blame_normalize_name (const char *cgroup_basename);

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
gboolean   gsm_blame_user_io_is_delegated (GPtrArray *cgroups);

G_END_DECLS
