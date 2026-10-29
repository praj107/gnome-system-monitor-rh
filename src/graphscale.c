/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * See graphscale.h. The rounding follows Paul Heckbert's "nice numbers for
 * graph labels" (Graphics Gems, 1990), as the graphs have always done.
 */

#include "graphscale.h"

#include <math.h>

/* Largest peak the arithmetic below handles without overflow (the bits
 * scale multiplies by 8 and the byte scale by 1.1). 2^60 B/s is far above
 * any link or bus. */
#define MAX_PEAK_PER_S (G_GUINT64_CONSTANT (1) << 60)

#define MIN_SCALE_BYTES 1024
#define MIN_SCALE_BITS 10000

/* A "nice" number near x: the ceiling of its leading digit to 1, 2, 5 or
 * 10 times a power of ten. */
static double
nice_ceiling (double x)
{
  double exponent = floor (log10 (x));
  double fraction = x / pow (10.0, exponent);
  double nice;

  if (fraction <= 1.0)
    nice = 1.0;
  else if (fraction <= 2.0)
    nice = 2.0;
  else if (fraction <= 5.0)
    nice = 5.0;
  else
    nice = 10.0;
  return nice * pow (10.0, exponent);
}

guint64
gsm_graph_scale_round (guint64  peak_per_s,
                       gboolean in_bits,
                       guint    n_ticks)
{
  guint64 scale = MIN (peak_per_s, MAX_PEAK_PER_S);

  if (in_bits)
    {
      /* Gets messy at low values because of the division by 8. */
      guint64 bit_scale = MAX (scale * 8, (guint64) MIN_SCALE_BITS);
      double tick_bits;

      if (n_ticks == 0)
        return scale;
      tick_bits = nice_ceiling ((double) MAX (bit_scale / n_ticks, (guint64) 1));
      return (guint64) (n_ticks * tick_bits) / 8;
    }

  /* Round up to get some extra space, then to one significant digit of a
   * binary multiple: scale = coefficient * 2^(10 * base), the coefficient
   * below 1024 and having one significant decimal digit
   * (100.5 KiB -> 101 KiB -> 200 KiB). */
  scale = MAX ((guint64) (1.1 * (double) scale), (guint64) MIN_SCALE_BYTES);

  guint64 base = (guint64) floor (log2 ((double) scale)) / 10;
  guint64 unit = G_GUINT64_CONSTANT (1) << (base * 10);
  guint64 coefficient = (scale + unit - 1) / unit;
  guint64 digit_unit = (guint64) pow (10.0, floor (log10 ((double) coefficient)));

  coefficient = (coefficient + digit_unit - 1) / digit_unit * digit_unit;
  return coefficient * unit;
}

guint64
gsm_graph_scale_next (const guint64 *history,
                      guint          n_history,
                      guint64        current_max,
                      gboolean       in_bits,
                      guint          n_ticks)
{
  guint64 peak = 0;
  guint64 new_max;

  for (guint i = 0; i < n_history; i++)
    peak = MAX (peak, history[i]);

  new_max = gsm_graph_scale_round (peak, in_bits, n_ticks);
  if (0.8 * (double) current_max < (double) new_max && new_max <= current_max)
    return current_max;
  return new_max;
}
