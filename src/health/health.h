/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <adwaita.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GSM_TYPE_HEALTH_VIEW (gsm_health_view_get_type ())
G_DECLARE_FINAL_TYPE (GsmHealthView, gsm_health_view, GSM, HEALTH_VIEW, AdwBin)

G_END_DECLS
