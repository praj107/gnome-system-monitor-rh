/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Developer builds only (see dev-ipc.h): dump the process model's raw rows.
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#ifdef HAVE_DEV_IPC
/* Writes one TSV line per process of the Processes tab's base store to
 * `path`, preceded by a comment line and a header line; returns the number of
 * rows, or -1 with *error set. */
int gsm_dev_ipc_write_rows (GtkTreeModel *store,
                            const char   *path,
                            GError      **error);

/* The model column that holds the pid (the enum lives in C++ headers). */
int gsm_dev_ipc_pid_column (void);

/* The tree store under the view's sort and filter models, or NULL. */
GtkTreeModel *gsm_dev_ipc_base_store (GtkTreeView *view);
#endif

G_END_DECLS
