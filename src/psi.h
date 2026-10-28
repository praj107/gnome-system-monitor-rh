/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Pressure Stall Information: parse a /proc/pressure/{cpu,memory,io} file
 * and turn two samples into "the share of the window spent stalled".
 * Follows Documentation/accounting/psi.rst. Pure functions, no GTK: the
 * path (or the text) is a parameter so tests use fixtures.
 *
 * `some` is the time at least one task was stalled; `full` the time all
 * non-idle tasks were stalled. `total` is cumulative microseconds. A line
 * that is not in the file (the `full` line of /proc/pressure/cpu on kernels
 * before 5.13) is unknown, never 0.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct
{
  gboolean present;  /* FALSE when the line was missing or unparseable */
  double   avg10;    /* the kernel's 10 s average, percent */
  guint64  total_us; /* cumulative stall time, microseconds */
} GsmPsiLine;

typedef struct
{
  GsmPsiLine some;
  GsmPsiLine full;
} GsmPsi;

/* Fill out from the file's text. Returns TRUE when the `some` line is
 * present (every pressure file has one); `full` may be absent. */
gboolean gsm_psi_parse (const char *text,
                        GsmPsi     *out);

/* Read and parse one pressure file. FALSE when unreadable (the file does
 * not exist without CONFIG_PSI, or with psi=0 reads fail with EOPNOTSUPP). */
gboolean gsm_psi_read (const char *path,
                       GsmPsi     *out);

/* Share of the window stalled, in percent: (after - before) / elapsed * 100.
 * FALSE (and *percent_out untouched) when either line is absent, the window
 * is not positive, or the counter went backwards. Capped at 100. */
gboolean gsm_psi_window_share (const GsmPsiLine *before,
                               const GsmPsiLine *after,
                               gint64            elapsed_us,
                               double           *percent_out);

G_END_DECLS
