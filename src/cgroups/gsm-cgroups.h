/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <sys/types.h>

#include <glib.h>
#include <gmodule.h>

G_BEGIN_DECLS

G_MODULE_EXPORT
gboolean    gsm_cgroups_is_enabled       (void);
G_MODULE_EXPORT
char       *gsm_cgroups_get_name         (pid_t       pid);
/* The display name for a process whose cgroup v2 path is already known
 * (the line "0::PATH" of /proc/PID/cgroup, proc(5), without the prefix):
 * what gsm_cgroups_get_name returns for that file. "" for the root group. */
G_MODULE_EXPORT
char       *gsm_cgroups_name_from_path   (const char *path,
                                          gsize       path_len);

G_END_DECLS
