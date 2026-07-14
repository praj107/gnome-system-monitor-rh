/*
 * Historical charts for the Health tab.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <math.h>

#include <glib/gi18n.h>

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
#define BAR_LABEL_W      140.0
#define BAR_VALUE_W       92.0
#define BAR_MIN_FILL       3.0

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

  /* The projection has to fit on the same axis as the history, or the dashed
   * line would run off the plot and silently misrepresent when the drive
   * actually reaches end of life. */
  if (has_projection)
    t1 = self->projection_ts;

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

    if (i == 0)
      cairo_move_to (cr, x, y);
    else
      cairo_line_to (cr, x, y);
  }
  cairo_stroke (cr);

  if (has_projection) {
    GsmChartPoint *last =
      &g_array_index (self->points, GsmChartPoint, self->points->len - 1);
    double dashes[] = { 4.0, 4.0 };

    /*
     * Dashed, and starting from a visible gap at the last real reading: this
     * is an extrapolation, and it must be impossible to mistake for data the
     * drive actually reported.
     */
    cairo_save (cr);
    set_source (cr, &accent, 0.45);
    cairo_set_dash (cr, dashes, 2, 0);
    cairo_move_to (cr, X_FOR (last->ts), Y_FOR (last->value));
    cairo_line_to (cr, X_FOR (self->projection_ts), Y_FOR (self->projection_value));
    cairo_stroke (cr);
    cairo_restore (cr);
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


static void
draw_bar_chart (GsmHistoryChart *self,
                cairo_t         *cr,
                const GdkRGBA   *ink,
                int              width,
                int              height)
{
  GdkRGBA accent = accent_color (ink);
  double max_value = 0;
  double track_x = BAR_LABEL_W;
  double track_w = width - BAR_LABEL_W - BAR_VALUE_W;
  double y = 0;

  (void) height;

  if (self->bars == NULL || self->bars->len == 0 || track_w <= 0)
    return;

  for (guint i = 0; i < self->bars->len; i++) {
    GsmChartBar *bar = g_ptr_array_index (self->bars, i);

    max_value = MAX (max_value, bar->value);
  }

  if (max_value <= 0)
    return;

  cairo_set_font_size (cr, FONT_SIZE);

  for (guint i = 0; i < self->bars->len; i++) {
    GsmChartBar *bar = g_ptr_array_index (self->bars, i);
    double w = (bar->value / max_value) * track_w;
    g_autofree char *value_label = format_value (GSM_CHART_UNIT_BYTES, bar->value);
    cairo_text_extents_t extents;

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
    cairo_text_extents (cr, bar->label, &extents);
    cairo_move_to (cr, 0, y + (BAR_HEIGHT / 2) + (extents.height / 2));
    cairo_show_text (cr, bar->label);

    /* Track */
    set_source (cr, ink, GRID_ALPHA);
    rounded_bar (cr, track_x, y, track_w, BAR_HEIGHT, BAR_RADIUS);
    cairo_fill (cr);

    /* Fill */
    set_source (cr, &accent, bar->estimated ? 0.35 : 1.0);
    rounded_bar (cr, track_x, y, w, BAR_HEIGHT, BAR_RADIUS);
    cairo_fill (cr);

    if (bar->estimated) {
      set_source (cr, &accent, 0.9);
      rounded_bar (cr, track_x, y, w, BAR_HEIGHT, BAR_RADIUS);
      hatch_bar (cr, track_x, y, w, BAR_HEIGHT);
    }

    /* Value, direct-labelled at the data end — every bar gets one because
     * there are few of them and the exact figure is the point. */
    set_source (cr, ink, 0.85);
    cairo_text_extents (cr, value_label, &extents);
    cairo_move_to (cr, width - extents.width,
                   y + (BAR_HEIGHT / 2) + (extents.height / 2));
    cairo_show_text (cr, value_label);

    y += BAR_HEIGHT + BAR_GAP;
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
    guint rows = self->bars != NULL ? self->bars->len : 0;
    int needed = (int) (rows * (BAR_HEIGHT + BAR_GAP));

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
gsm_history_chart_set_bars (GsmHistoryChart *self,
                            GPtrArray       *bars)
{
  g_return_if_fail (GSM_IS_HISTORY_CHART (self));

  g_clear_pointer (&self->bars, g_ptr_array_unref);
  self->bars = bars != NULL ? g_ptr_array_ref (bars) : NULL;

  gtk_widget_queue_resize (GTK_WIDGET (self));
}
