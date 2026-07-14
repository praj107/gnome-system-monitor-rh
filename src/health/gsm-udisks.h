/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <gio/gio.h>

#include "gsm-drive.h"

G_BEGIN_DECLS

/*
 * Enumerate every drive udisks2 knows about and read whatever health data it
 * will hand over without a privilege prompt. Blocking — call it from a worker
 * thread or from the sampler process, never from the UI thread.
 *
 * Returns a GPtrArray of GsmDriveInfo*, owned by the caller. Drives that
 * udisks can see but not read SMART from come back with a smart_source of
 * GSM_SMART_SOURCE_LOCKED, which is the UI's cue to offer the unlock button.
 */
GPtrArray *gsm_udisks_collect (GDBusConnection  *bus,
                               GError          **error);

void       gsm_drive_info_free (GsmDriveInfo *info);

G_END_DECLS
