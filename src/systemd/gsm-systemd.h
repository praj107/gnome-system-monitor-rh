/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <sys/types.h>

#include <glib.h>
#include <gmodule.h>

G_BEGIN_DECLS

G_MODULE_EXPORT
gboolean  gsm_systemd_is_running       (void);
/* The seat of a login session by its id ("c1"), cached for the life of the
 * program; "" when the session has none or cannot be looked up. The string
 * belongs to the cache and stays valid. */
G_MODULE_EXPORT
const char * gsm_systemd_get_session_seat (const char *session);
G_MODULE_EXPORT
gboolean  gsm_systemd_get_process_info (pid_t     pid,
                                        char    **sd_unit,
                                        char    **sd_session,
                                        char    **sd_seat,
                                        uid_t    *sd_owner);

G_END_DECLS
