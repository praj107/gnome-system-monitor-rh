/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <stdint.h>

#include <gtk/gtk.h>

G_BEGIN_DECLS

/*
 * A chart for data that already happened.
 *
 * GsmGraph, which the Resources tab uses, is a rolling realtime scope: it
 * pulls from a callback, scrolls, and has no notion of an absolute time axis.
 * Wear history is the opposite — a fixed window over months, read once from
 * the database — so this is a separate widget rather than a mode bolted onto
 * that one. It deliberately borrows GsmGraph's visual language (recessive
 * grid, thin marks, CSS-driven colour) so the two read as one family.
 */

typedef enum {
  GSM_CHART_LINE,   /* value over time, with an optional projection */
  GSM_CHART_BAR,    /* ranked horizontal bars — the blame breakdown */
} GsmChartMode;

typedef enum {
  GSM_CHART_UNIT_PERCENT,
  GSM_CHART_UNIT_BYTES,
  GSM_CHART_UNIT_CELSIUS,
} GsmChartUnit;

typedef struct {
  int64_t ts;      /* unix seconds */
  double  value;
} GsmChartPoint;

typedef struct {
  char    *label;
  double   value;
  gboolean estimated;  /* drawn hatched and labelled, never just recoloured */
  int      indent;     /* 0 for a top-level row; 1 for one that belongs to the
                        * row above it (shifts the label, not the bar) */
  gboolean caption;    /* a line of explanation: no bar, no value, dimmed */
} GsmChartBar;

void gsm_chart_bar_free (GsmChartBar *bar);

#define GSM_TYPE_HISTORY_CHART (gsm_history_chart_get_type ())
G_DECLARE_FINAL_TYPE (GsmHistoryChart, gsm_history_chart, GSM, HISTORY_CHART, GtkWidget)

GtkWidget *gsm_history_chart_new            (void);

void       gsm_history_chart_set_mode       (GsmHistoryChart *self,
                                             GsmChartMode     mode);
void       gsm_history_chart_set_unit       (GsmHistoryChart *self,
                                             GsmChartUnit     unit);

/* Line mode. Points must be sorted oldest first. */
void       gsm_history_chart_set_points     (GsmHistoryChart *self,
                                             GArray          *points);
void       gsm_history_chart_set_value_range (GsmHistoryChart *self,
                                              double           min,
                                              double           max);

/*
 * Draw a dashed line from the last real datapoint to where the trend is
 * heading. Dashed, and never joined seamlessly to the solid history, because
 * it is a prediction and must not be mistaken for a measurement. Pass a ts of
 * 0 to clear.
 */
void       gsm_history_chart_set_projection (GsmHistoryChart *self,
                                             int64_t          ts,
                                             double           value);

/*
 * Words drawn on the plot beside a projection, so the dashed line says what
 * it is wherever the chart is seen. NULL for none.
 */
void       gsm_history_chart_set_projection_label (GsmHistoryChart *self,
                                                   const char      *label);

/*
 * Line mode. Two points further apart than this are not joined by a line:
 * the chart has no reading in between, and a straight stroke across the gap
 * would claim one. Points are drawn as dots so a lone reading still shows.
 * 0 (the default) joins every pair.
 */
void       gsm_history_chart_set_max_gap    (GsmHistoryChart *self,
                                             int64_t          seconds);

/* Bar mode. Takes a GPtrArray of GsmChartBar*, biggest first. */
void       gsm_history_chart_set_bars       (GsmHistoryChart *self,
                                             GPtrArray       *bars);

G_END_DECLS
