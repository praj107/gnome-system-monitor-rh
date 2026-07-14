/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/*
 * Developer control socket.
 *
 * Lets a test harness drive the running UI — switch tabs, scroll, click, and
 * capture the window — without synthesising input events, so it neither steals
 * the developer's focus nor breaks when the window is behind another one.
 *
 * This is a hole punched straight through the UI, so it is locked twice:
 *
 *   1. compile time — only built with -Ddev_ipc=true, which is NOT implied by
 *      -Ddevelopment=true and must never be set for a release build;
 *   2. run time — even then it stays dormant unless GSM_DEV_IPC=1 is set.
 *
 * When either lock is closed this is an empty inline no-op, so the socket code
 * is not merely unreachable in a shipped binary, it is not present in it.
 */
#ifdef HAVE_DEV_IPC
void gsm_dev_ipc_init (GtkWindow *window);
#else
static inline void
gsm_dev_ipc_init (GtkWindow *window)
{
  (void) window;
}
#endif

G_END_DECLS
