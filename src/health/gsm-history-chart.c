/*
 * Historical charts for the Health tab.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <math.h>

#include <glib/gi18n.h>
#include <pango/pangocairo.h>

#include "gsm-history-chart.h"

/* Layout. Thin marks and a recessive grid: the data should be the only thing
 * with any visual weight on the surface. */
#define LINE_WIDTH        2.0
#define GRID_WIDTH        1.0
#define GRID_ALPHA        0.10
#define AXIS_LABEL_ALPHA  0.55
#define FONT_SIZE         11.0

#define PAD_LEFT          52.0
#define PAD_RIGHT         12.0
#define PAD_TOP           10.0
#define PAD_BOTTOM        22.0

#define Y_GRID_LINES      4

/* Bar mode */
#define BAR_HEIGHT        18.0
#define BAR_GAP            8.0   /* >= 2px of surface between fills */
#define BAR_RADIUS         4.0
#define BAR_LABEL_MIN_W  140.0
#define BAR_LABEL_MAX_SHARE 0.5  /* of the chart's width */
#define BAR_INDENT_W      18.0
#define BAR_VALUE_W       92.0
#define BAR_MIN_FILL       3.0
#define CAPTION_HEIGHT    16.0
#define CAPTION_GAP        4.0

/* Line mode */
#define DOT_RADIUS         2.5
#define MAX_DOTTED_POINTS 60

struct _GsmHistoryChart {
  GtkWidget parent_instance;

  GsmChartMode mode;
  GsmChartUnit unit;

  GArray *points;          /* GsmChartPoint  */
  GPtrArray *bars;         /* GsmChartBar*   */

  double value_min;
  double value_max;

  int64_t projection_ts;
  double projection_value;
  char *projection_label;
  int64_t max_gap_seconds;
};

G_DEFINE_FINAL_TYPE (GsmHistoryChart, gsm_history_chart, GTK_TYPE_WIDGET)


void
gsm_chart_bar_free (GsmChartBar *bar)
{
  if (bar == NULL)
    return;

  g_free (bar->label);
  g_free (bar);
}


/*
 * Theme-awareness without any colour plumbing: the widget's CSS `color` is
 * the theme's foreground ink, so its lightness tells us which theme we are
 * in. That lets us pick the accent and status colours libadwaita defines for
 * that mode instead of flipping one hardcoded set, which is what the design
 * guidance asks for — dark is chosen, not inverted.
 */
static gboolean
is_dark (const GdkRGBA *ink)
{
  double luminance = (0.2126 * ink->red) +
                     (0.7152 * ink->green) +
                     (0.0722 * ink->blue);

  return luminance > 0.5;
}


static GdkRGBA
accent_color (const GdkRGBA *ink)
{
  /* libadwaita @accent_color, which is tuned for legibility against the
   * window background in each mode. Both validated >= 3:1 on their surface. */
  GdkRGBA light = { 0.110, 0.443, 0.847, 1.0 };   /* #1c71d8 */
  GdkRGBA dark  = { 0.471, 0.682, 0.929, 1.0 };   /* #78aeed */

  return is_dark (ink) ? dark : light;
}


static void
set_source (cairo_t *cr, const GdkRGBA *color, double alpha)
{
  cairo_set_source_rgba (cr, color->red, color->green, color->blue,
                         color->alpha * alpha);
}


static char *
format_value (GsmChartUnit unit,
              double       value)
{
  switch (unit) {
    case GSM_CHART_UNIT_PERCENT:
      return g_strdup_printf ("%.0f%%", value);
    case GSM_CHART_UNIT_CELSIUS:
      return g_strdup_printf ("%.0f°", value);
    case GSM_CHART_UNIT_BYTES:
    default:
      return g_format_size ((guint64) MAX (value, 0));
  }
}


/* ------------------------------------------------------------------ */
/* Line mode                                                           */
/* ------------------------------------------------------------------ */

/* Are readings i and i + 1 close enough in time to draw a line between? */
static gboolean
points_joined (GsmHistoryChart *self,
               guint            i)
{
  const GsmChartPoint *a = &g_array_index (self->points, GsmChartPoint, i);
  const GsmChartPoint *b = &g_array_index (self->points, GsmChartPoint, i + 1);

  return self->max_gap_seconds <= 0 || b->ts - a->ts <= self->max_gap_seconds;
}


static void
draw_line_chart (GsmHistoryChart *self,
                 cairo_t         *cr,
                 const GdkRGBA   *ink,
                 int              width,
                 int              height)
{
  GdkRGBA accent = accent_color (ink);
  double plot_w = width - PAD_LEFT - PAD_RIGHT;
  double plot_h = height - PAD_TOP - PAD_BOTTOM;
  double span, range;
  int64_t t0, t1;
  gboolean has_projection;

  if (self->points == NULL || self->points->len == 0 || plot_w <= 0 || plot_h <= 0)
    return;

  t0 = g_array_index (self->points, GsmChartPoint, 0).ts;
  t1 = g_array_index (self->points, GsmChartPoint, self->points->len - 1).ts;

  has_projection = self->projection_ts > t1;

  /*
   * The projection shares the time axis with the history, but only up to the
   * history's own length again (at least a day). A drive 15 years from its
   * rating would otherwise squash a month of readings into the left-hand
   * pixels, where two or three points read as a near-vertical wall. The
   * dashed line is the projection's straight path cut off at the plot's edge;
   * its label says where it ends.
   */
  if (has_projection) {
    int64_t reach = MAX (t1 - t0, 86400);

    t1 = MIN (self->projection_ts, t1 + reach);
  }

  span = (double) (t1 - t0);
  if (span <= 0)
    span = 1;

  range = self->value_max - self->value_min;
  if (range <= 0)
    range = 1;

#define X_FOR(ts)    (PAD_LEFT + (((double) ((ts) - t0)) / span) * plot_w)
#define Y_FOR(value) (PAD_TOP + plot_h - \
                      (((value) - self->value_min) / range) * plot_h)

  /* Grid + y labels. Recessive: thin, low alpha, behind the data. */
  cairo_set_line_width (cr, GRID_WIDTH);
  cairo_set_font_size (cr, FONT_SIZE);

  for (int i = 0; i <= Y_GRID_LINES; i++) {
    double value = self->value_min + (range * i / Y_GRID_LINES);
    double y = Y_FOR (value);
    g_autofree char *label = format_value (self->unit, value);
    cairo_text_extents_t extents;

    set_source (cr, ink, GRID_ALPHA);
    cairo_move_to (cr, PAD_LEFT, y);
    cairo_line_to (cr, width - PAD_RIGHT, y);
    cairo_stroke (cr);

    set_source (cr, ink, AXIS_LABEL_ALPHA);
    cairo_text_extents (cr, label, &extents);
    cairo_move_to (cr, PAD_LEFT - 8 - extents.width, y + (extents.height / 2));
    cairo_show_text (cr, label);
  }

  /* History: one solid series, so no legend — the card's heading names it. */
  set_source (cr, &accent, 1.0);
  cairo_set_line_width (cr, LINE_WIDTH);
  cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);
  cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);

  for (guint i = 0; i < self->points->len; i++) {
    GsmChartPoint *p = &g_array_index (self->points, GsmChartPoint, i);
    double x = X_FOR (p->ts);
    double y = Y_FOR (p->value);

    if (i > 0 && points_joined (self, i - 1))
      cairo_line_to (cr, x, y);
    else
      cairo_move_to (cr, x, y);
  }
  cairo_stroke (cr);

  /* Dots for readings: always when there are few, and for any reading with
   * no neighbour to be joined to, so a lone point does not vanish. */
  {
    gboolean all = self->points->len <= MAX_DOTTED_POINTS;

    for (guint i = 0; i < self->points->len; i++) {
      GsmChartPoint *p = &g_array_index (self->points, GsmChartPoint, i);
      gboolean joined = (i > 0 && points_joined (self, i - 1)) ||
                        (i + 1 < self->points->len && points_joined (self, i));

      if (!all && joined)
        continue;

      cairo_new_sub_path (cr);
      cairo_arc (cr, X_FOR (p->ts), Y_FOR (p->value), DOT_RADIUS, 0, 2 * G_PI);
      cairo_fill (cr);
    }
  }

  if (has_projection) {
    GsmChartPoint *last =
      &g_array_index (self->points, GsmChartPoint, self->points->len - 1);
    double dashes[] = { 4.0, 4.0 };
    double fraction = (double) (t1 - last->ts) /
                      (double) (self->projection_ts - last->ts);
    double end_value = last->value +
                       (self->projection_value - last->value) * fraction;

    /*
     * Dashed, and starting from a visible gap at the last real reading: this
     * is an extrapolation, and it must be impossible to mistake for data the
     * drive actually reported.
     */
    cairo_save (cr);
    set_source (cr, &accent, 0.45);
    cairo_set_dash (cr, dashes, 2, 0);
    cairo_move_to (cr, X_FOR (last->ts), Y_FOR (last->value));
    cairo_line_to (cr, X_FOR (t1), Y_FOR (end_value));
    cairo_stroke (cr);
    cairo_restore (cr);

    if (self->projection_label != NULL) {
      cairo_text_extents_t extents;

      set_source (cr, ink, AXIS_LABEL_ALPHA);
      cairo_text_extents (cr, self->projection_label, &extents);
      cairo_move_to (cr, MAX (PAD_LEFT, width - PAD_RIGHT - extents.width),
                     PAD_TOP + extents.height + 2);
      cairo_show_text (cr, self->projection_label);
    }
  }

#undef X_FOR
#undef Y_FOR
}


/* ------------------------------------------------------------------ */
/* Bar mode                                                            */
/* ------------------------------------------------------------------ */

static void
rounded_bar (cairo_t *cr,
             double   x,
             double   y,
             double   w,
             double   h,
             double   r)
{
  /* Square at the baseline, rounded at the data end — the end that encodes
   * the value is the one that gets the radius. */
  if (w < r)
    r = w;

  cairo_new_sub_path (cr);
  cairo_move_to (cr, x, y);
  cairo_line_to (cr, x + w - r, y);
  cairo_arc (cr, x + w - r, y + r, r, -G_PI_2, 0);
  cairo_line_to (cr, x + w, y + h - r);
  cairo_arc (cr, x + w - r, y + h - r, r, 0, G_PI_2);
  cairo_line_to (cr, x, y + h);
  cairo_close_path (cr);
}


/*
 * Estimated bars are hatched, not recoloured. "Estimated" is a property of
 * the measurement, not a different category of writer — encoding it as a hue
 * would both burn a categorical colour and leave colourblind users with no
 * way to tell the difference. Texture plus the "est." label carries it.
 */
static void
hatch_bar (cairo_t *cr,
           double   x,
           double   y,
           double   w,
           double   h)
{
  cairo_save (cr);
  cairo_clip (cr);

  cairo_set_line_width (cr, 1.0);

  for (double i = -h; i < w + h; i += 5.0) {
    cairo_move_to (cr, x + i, y + h);
    cairo_line_to (cr, x + i + h, y);
  }
  cairo_stroke (cr);

  cairo_restore (cr);
}


/* A one-line Pango layout in the chart's text size, cut off with an
 * ellipsis at `max_width_px` rather than running into its neighbour. */
static PangoLayout *
bar_text_layout (GsmHistoryChart *self,
                 const char      *text,
                 double           max_width_px)
{
  PangoLayout *layout = gtk_widget_create_pango_layout (GTK_WIDGET (self), text);
  PangoFontDescription *font = pango_font_description_new ();

  pango_font_description_set_absolute_size (font, FONT_SIZE * PANGO_SCALE);
  pango_layout_set_font_description (layout, font);
  pango_font_description_free (font);

  pango_layout_set_ellipsize (layout, PANGO_ELLIPSIZE_END);
  pango_layout_set_width (layout, (int) (MAX (max_width_px, 1) * PANGO_SCALE));

  return layout;
}


/* Draw `text` with its left edge at x, vertically centred in a row. */
static void
show_bar_text (GsmHistoryChart *self,
               cairo_t         *cr,
               const char      *text,
               double           x,
               double           y,
               double           row_height,
               double           max_width_px)
{
  g_autoptr (PangoLayout) layout = bar_text_layout (self, text, max_width_px);
  int text_height;

  pango_layout_get_pixel_size (layout, NULL, &text_height);
  cairo_move_to (cr, x, y + (row_height - text_height) / 2);
  pango_cairo_show_layout (cr, layout);
}


static double
bar_row_height (const GsmChartBar *bar)
{
  return bar->caption ? CAPTION_HEIGHT : BAR_HEIGHT;
}


static double
bar_row_gap (const GsmChartBar *bar)
{
  return bar->caption ? CAPTION_GAP : BAR_GAP;
}


/* The label column fits the longest label (indent included), within limits:
 * "user.slice (own and exited units)" is longer than a fixed 140 px. */
static double
bar_label_column_width (GsmHistoryChart *self,
                        int              width)
{
  double widest = BAR_LABEL_MIN_W;
  double limit = MAX (BAR_LABEL_MIN_W, width * BAR_LABEL_MAX_SHARE);

  for (guint i = 0; i < self->bars->len; i++) {
    GsmChartBar *bar = g_ptr_array_index (self->bars, i);
    g_autoptr (PangoLayout) layout = NULL;
    int text_width;

    if (bar->caption)
      continue;

    layout = gtk_widget_create_pango_layout (GTK_WIDGET (self), bar->label);
    {
      PangoFontDescription *font = pango_font_description_new ();

      pango_font_description_set_absolute_size (font, FONT_SIZE * PANGO_SCALE);
      pango_layout_set_font_description (layout, font);
      pango_font_description_free (font);
    }
    pango_layout_get_pixel_size (layout, &text_width, NULL);
    widest = MAX (widest, bar->indent * BAR_INDENT_W + text_width + 12);
  }

  return MIN (widest, limit);
}


static void
draw_bar_chart (GsmHistoryChart *self,
                cairo_t         *cr,
                const GdkRGBA   *ink,
                int              width,
                int              height)
{
  GdkRGBA accent = accent_color (ink);
  double max_value = 0;
  double track_x;
  double track_w;
  double y = 0;

  (void) height;

  if (self->bars == NULL || self->bars->len == 0)
    return;

  track_x = bar_label_column_width (self, width);
  track_w = width - track_x - BAR_VALUE_W;
  if (track_w <= 0)
    return;

  for (guint i = 0; i < self->bars->len; i++) {
    GsmChartBar *bar = g_ptr_array_index (self->bars, i);

    if (!bar->caption)
      max_value = MAX (max_value, bar->value);
  }

  for (guint i = 0; i < self->bars->len; i++) {
    GsmChartBar *bar = g_ptr_array_index (self->bars, i);
    double indent_x = bar->indent * BAR_INDENT_W;
    double row_height = bar_row_height (bar);
    double w = max_value > 0 ? (bar->value / max_value) * track_w : 0;
    g_autofree char *value_label = NULL;
    cairo_text_extents_t extents;

    if (bar->caption) {
      /* An explanation, not a measurement: dimmer than the names, across
       * the whole row. */
      set_source (cr, ink, AXIS_LABEL_ALPHA + 0.1);
      show_bar_text (self, cr, bar->label, indent_x, y, row_height,
                     width - indent_x);
      y += row_height + bar_row_gap (bar);
      continue;
    }

    value_label = format_value (GSM_CHART_UNIT_BYTES, bar->value);

    /*
     * Write volumes span orders of magnitude — a browser can out-write a log
     * daemon a thousand to one — so a strictly proportional bar renders every
     * writer but the largest as an empty track, which reads as "wrote nothing"
     * rather than "wrote a little". Give any nonzero value a visible stub. The
     * number is direct-labelled beside it, so the stub cannot mislead about
     * magnitude the way a rescaled or log axis would.
     */
    if (bar->value > 0)
      w = MAX (w, BAR_MIN_FILL);

    /* Writer name, in text ink — never in the series colour. The bar beside
     * it carries the identity. */
    set_source (cr, ink, 0.85);
    show_bar_text (self, cr, bar->label, indent_x, y, row_height,
                   track_x - indent_x - 8);

    /* Track */
    set_source (cr, ink, GRID_ALPHA);
    rounded_bar (cr, track_x, y, track_w, row_height, BAR_RADIUS);
    cairo_fill (cr);

    /* Fill */
    set_source (cr, &accent, bar->estimated ? 0.35 : 1.0);
    rounded_bar (cr, track_x, y, w, row_height, BAR_RADIUS);
    cairo_fill (cr);

    if (bar->estimated) {
      set_source (cr, &accent, 0.9);
      rounded_bar (cr, track_x, y, w, row_height, BAR_RADIUS);
      hatch_bar (cr, track_x, y, w, row_height);
    }

    /* Value, direct-labelled at the data end — every bar gets one because
     * there are few of them and the exact figure is the point. */
    set_source (cr, ink, 0.85);
    cairo_set_font_size (cr, FONT_SIZE);
    cairo_text_extents (cr, value_label, &extents);
    cairo_move_to (cr, width - extents.width,
                   y + (row_height / 2) + (extents.height / 2));
    cairo_show_text (cr, value_label);

    y += row_height + bar_row_gap (bar);
  }
}


/* ------------------------------------------------------------------ */

static void
gsm_history_chart_snapshot (GtkWidget   *widget,
                            GtkSnapshot *snapshot)
{
  GsmHistoryChart *self = GSM_HISTORY_CHART (widget);
  int width = gtk_widget_get_width (widget);
  int height = gtk_widget_get_height (widget);
  GdkRGBA ink;
  cairo_t *cr;

  if (width <= 0 || height <= 0)
    return;

  gtk_widget_get_color (widget, &ink);

  cr = gtk_snapshot_append_cairo (snapshot,
                                  &GRAPHENE_RECT_INIT (0, 0, width, height));

  cairo_set_antialias (cr, CAIRO_ANTIALIAS_GOOD);

  if (self->mode == GSM_CHART_BAR)
    draw_bar_chart (self, cr, &ink, width, height);
  else
    draw_line_chart (self, cr, &ink, width, height);

  cairo_destroy (cr);
}


static void
gsm_history_chart_measure (GtkWidget      *widget,
                           GtkOrientation  orientation,
                           int             for_size,
                           int            *minimum,
                           int            *natural,
                           int            *minimum_baseline,
                           int            *natural_baseline)
{
  GsmHistoryChart *self = GSM_HISTORY_CHART (widget);

  (void) for_size;
  *minimum_baseline = -1;
  *natural_baseline = -1;

  if (orientation == GTK_ORIENTATION_HORIZONTAL) {
    *minimum = 240;
    *natural = 480;
    return;
  }

  if (self->mode == GSM_CHART_BAR) {
    /* Grow to fit however many writers there are, rather than scrolling or
     * clipping them. */
    double needed_px = 0;
    int needed;

    for (guint i = 0; self->bars != NULL && i < self->bars->len; i++) {
      const GsmChartBar *bar = g_ptr_array_index (self->bars, i);

      needed_px += bar_row_height (bar) + bar_row_gap (bar);
    }
    needed = (int) needed_px;

    *minimum = MAX (needed, 24);
    *natural = *minimum;
    return;
  }

  *minimum = 120;
  *natural = 160;
}


static void
gsm_history_chart_dispose (GObject *object)
{
  GsmHistoryChart *self = GSM_HISTORY_CHART (object);

  g_clear_pointer (&self->points, g_array_unref);
  g_clear_pointer (&self->bars, g_ptr_array_unref);
  g_clear_pointer (&self->projection_label, g_free);

  G_OBJECT_CLASS (gsm_history_chart_parent_class)->dispose (object);
}


static void
gsm_history_chart_class_init (GsmHistoryChartClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = gsm_history_chart_dispose;

  widget_class->snapshot = gsm_history_chart_snapshot;
  widget_class->measure = gsm_history_chart_measure;

  gtk_widget_class_set_css_name (widget_class, "historychart");
}


static void
gsm_history_chart_init (GsmHistoryChart *self)
{
  self->mode = GSM_CHART_LINE;
  self->unit = GSM_CHART_UNIT_PERCENT;
  self->value_min = 0;
  self->value_max = 100;
}


GtkWidget *
gsm_history_chart_new (void)
{
  return g_object_new (GSM_TYPE_HISTORY_CHART, NULL);
}


void
gsm_history_chart_set_mode (GsmHistoryChart *self,
                            GsmChartMode     mode)
{
  g_return_if_fail (GSM_IS_HISTORY_CHART (self));

  if (self->mode == mode)
    return;

  self->mode = mode;
  gtk_widget_queue_resize (GTK_WIDGET (self));
}


void
gsm_history_chart_set_unit (GsmHistoryChart *self,
                            GsmChartUnit     unit)
{
  g_return_if_fail (GSM_IS_HISTORY_CHART (self));

  self->unit = unit;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}


void
gsm_history_chart_set_points (GsmHistoryChart *self,
                              GArray          *points)
{
  g_return_if_fail (GSM_IS_HISTORY_CHART (self));

  g_clear_pointer (&self->points, g_array_unref);
  self->points = points != NULL ? g_array_ref (points) : NULL;

  gtk_widget_queue_draw (GTK_WIDGET (self));
}


void
gsm_history_chart_set_value_range (GsmHistoryChart *self,
                                   double           min,
                                   double           max)
{
  g_return_if_fail (GSM_IS_HISTORY_CHART (self));

  self->value_min = min;
  self->value_max = max;

  gtk_widget_queue_draw (GTK_WIDGET (self));
}


void
gsm_history_chart_set_projection (GsmHistoryChart *self,
                                  int64_t          ts,
                                  double           value)
{
  g_return_if_fail (GSM_IS_HISTORY_CHART (self));

  self->projection_ts = ts;
  self->projection_value = value;

  gtk_widget_queue_draw (GTK_WIDGET (self));
}


void
gsm_history_chart_set_projection_label (GsmHistoryChart *self,
                                        const char      *label)
{
  g_return_if_fail (GSM_IS_HISTORY_CHART (self));

  g_free (self->projection_label);
  self->projection_label = g_strdup (label);

  gtk_widget_queue_draw (GTK_WIDGET (self));
}


void
gsm_history_chart_set_max_gap (GsmHistoryChart *self,
                               int64_t          seconds)
{
  g_return_if_fail (GSM_IS_HISTORY_CHART (self));

  self->max_gap_seconds = seconds;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}


void
gsm_history_chart_set_bars (GsmHistoryChart *self,
                            GPtrArray       *bars)
{
  g_return_if_fail (GSM_IS_HISTORY_CHART (self));

  g_clear_pointer (&self->bars, g_ptr_array_unref);
  self->bars = bars != NULL ? g_ptr_array_ref (bars) : NULL;

  /* The bars are drawn, so say them in words too: assistive technology
   * reads this. */
  {
    GString *words = g_string_new (NULL);

    for (guint i = 0; self->bars != NULL && i < self->bars->len; i++) {
      const GsmChartBar *bar = g_ptr_array_index (self->bars, i);

      if (bar->caption) {
        g_string_append_printf (words, "%*s%s\n", bar->indent * 2, "", bar->label);
      } else {
        g_autofree char *value = format_value (GSM_CHART_UNIT_BYTES, bar->value);

        g_string_append_printf (words, "%*s%s: %s%s\n", bar->indent * 2, "",
                                bar->label, value,
                                bar->estimated ? _(" (estimated)") : "");
      }
    }

    gtk_accessible_update_property (GTK_ACCESSIBLE (self),
                                    GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, words->str,
                                    -1);
    g_string_free (words, TRUE);
  }

  gtk_widget_queue_resize (GTK_WIDGET (self));
}
