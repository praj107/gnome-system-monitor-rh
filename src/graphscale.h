/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The vertical scale of the network and disk history graphs, as pure 64-bit
 * functions: the history is the rate (bytes per second) of each past
 * refresh, and the scale is a round number at or above its peak, so the
 * axis labels read "100 MiB/s" rather than "97.3 MiB/s". Rates are 64-bit
 * throughout: an NVMe drive reads 7 GB/s, which does not fit in 32 bits.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* A round number at or above peak_per_s, with some headroom. With in_bits
 * (a network graph shown in bits per second) the scale is a multiple of
 * n_ticks round steps in bits; otherwise it is one significant digit of a
 * binary multiple (1 KiB, 2 KiB, 5 KiB, 10 KiB, … 20 MiB …), never below
 * 1 KiB. n_ticks is only used with in_bits and must then be above 0. */
guint64 gsm_graph_scale_round (guint64  peak_per_s,
                               gboolean in_bits,
                               guint    n_ticks);

/* The scale to use after a new refresh: the round scale for the largest
 * value in history[0..n_history), or current_max itself when that is not
 * above it and not below 80 % of it (so the graph does not rescale for
 * every small change). A new scale above current_max is taken at once. */
guint64 gsm_graph_scale_next (const guint64 *history,
                              guint          n_history,
                              guint64        current_max,
                              gboolean       in_bits,
                              guint          n_ticks);

G_END_DECLS
