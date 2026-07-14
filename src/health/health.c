/*
 * The Health tab.
 *
 * The Disks tab answers "is my filesystem full?". This one answers "is my
 * hardware wearing out, and what is wearing it out?" — which is a different
 * question about different objects (physical devices, not mountpoints), and
 * so it is a different tab.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <glib/gi18n.h>

#include "gsm-battery.h"
#include "gsm-blame.h"
#include "gsm-drive-spec.h"
#include "gsm-health-db.h"
#include "gsm-history-chart.h"
#include "gsm-procio.h"
#include "gsm-udisks.h"
#include "health.h"
#include "settings-keys.h"

#define SAMPLER_UNIT "gnome-system-monitor-health-sampler.service"

/* Refreshing SMART is a D-Bus round trip per drive; wear moves on the scale
 * of days, so there is nothing to gain from doing it often. */
#define REFRESH_INTERVAL_SECS 30

typedef struct {
  GsmHealthView *view;
  GsmDrive      *drive;

  AdwPreferencesGroup *group;
  AdwBanner           *banner;

  GtkLabel  *headline;
  GtkLabel  *basis;
  GtkLabel  *endurance;

  GtkWidget *wear_heading;
  GtkWidget *wear_chart;
  GtkWidget *blame_heading;
  GtkWidget *blame_chart;
  GtkLabel  *blame_note;

  GtkWidget *unlock_button;

  /* Trailing 30-day write rate, in bytes/day; -1 when there is not yet enough
   * history to say. Computed once per refresh — the headline and the chart's
   * projection both need it, and it cannot change between them. */
  double write_rate;
} DriveCard;

struct _GsmHealthView {
  AdwBin parent_instance;

  AdwPreferencesPage *page;
  AdwPreferencesGroup *battery_group;
  AdwPreferencesGroup *tracking_group;
  AdwSwitchRow *tracking_row;
  AdwActionRow *history_row;
  AdwBanner *io_accounting_banner;

  GsmHealthDb *db;
  GDBusConnection *bus;

  GHashTable *cards;          /* drive id -> DriveCard* */
  GPtrArray *battery_rows;    /* rows we added, so we can take them back out */
  GsmHistoryRange range;

  GSettings *settings;
  GCancellable *cancellable;
  guint timeout;
  gboolean refreshing;
  gboolean tracking_enabled;
  gboolean io_delegated;

  /* FALSE until the initial GSettings binding has settled, so the settings
   * bind cannot masquerade as a user flipping the switch. */
  gboolean bound;
};

G_DEFINE_FINAL_TYPE (GsmHealthView, gsm_health_view, ADW_TYPE_BIN)


static void refresh (GsmHealthView *self);


static void
drive_card_free (DriveCard *card)
{
  if (card == NULL)
    return;

  g_clear_object (&card->drive);
  g_free (card);
}


/* ------------------------------------------------------------------ */
/* Formatting the headline                                             */
/* ------------------------------------------------------------------ */

/*
 * The one number a user actually wants. Everything else on the card exists to
 * justify this line, and the line below it always states what it was computed
 * from — a projection with no stated basis is just an oracle.
 */
static void
update_headline (DriveCard *card)
{
  GsmDrive *drive = card->drive;
  GsmSmartSource source = gsm_drive_get_smart_source (drive);
  double rate = card->write_rate;
  double years;
  g_autofree char *basis = NULL;

  if (source == GSM_SMART_SOURCE_LOCKED) {
    gtk_label_set_text (card->headline, _("Health data is locked"));
    gtk_label_set_text (card->basis,
                        _("This drive’s controller will not report health "
                          "without administrator access."));
    gtk_widget_set_visible (card->unlock_button, TRUE);
    return;
  }

  if (source == GSM_SMART_SOURCE_UNSUPPORTED || source == GSM_SMART_SOURCE_NONE) {
    gtk_label_set_text (card->headline, _("No health data"));
    gtk_label_set_text (card->basis,
                        _("This drive does not report SMART data."));
    gtk_widget_set_visible (card->unlock_button, FALSE);
    return;
  }

  gtk_widget_set_visible (card->unlock_button, FALSE);

  /*
   * Some enclosures answer "am I dying?" but not "how worn am I?" — a plain
   * SCSI bridge gives a pass/fail verdict and nothing else. That verdict is
   * still the most valuable single fact about a drive, so show it, and say
   * plainly why there is no lifespan below it rather than leaving a blank.
   */
  if (gsm_drive_get_wear_unavailable (drive)) {
    gtk_label_set_text (card->headline,
                        gsm_drive_get_health (drive) == GSM_DRIVE_HEALTH_FAILING
                        ? _("Reports that it is failing")
                        : _("Reports healthy"));
    gtk_label_set_text (card->basis,
                        _("This enclosure does not report wear or bytes "
                          "written, so no lifespan can be estimated."));
    return;
  }

  years = gsm_drive_project_years_left (drive, rate);

  if (years < 0) {
    /*
     * We know how worn the drive is, but not yet how fast it is wearing.
     * Saying "unknown" and explaining why beats inventing a rate from the
     * drive's lifetime average, which would be dominated by whatever the
     * machine was doing months ago.
     */
    int percent = gsm_drive_get_percent_used (drive);

    if (percent >= 0) {
      g_autofree char *text = g_strdup_printf (_("%d%% worn"), percent);

      gtk_label_set_text (card->headline, text);
    } else {
      gtk_label_set_text (card->headline, _("Healthy"));
    }

    gtk_label_set_text (card->basis,
                        card->view->tracking_enabled
                        ? _("Collecting history — a lifespan estimate needs "
                            "a few hours of data")
                        : _("Turn on wear tracking below to project a lifespan"));
    return;
  }

  {
    g_autofree char *text = NULL;

    if (years >= 100)
      text = g_strdup (_("Outlives the machine"));
    else if (years >= 2)
      text = g_strdup_printf (_("~%.0f years of life left"), years);
    else if (years >= 1)
      text = g_strdup_printf (_("~%.1f years of life left"), years);
    else
      text = g_strdup_printf (_("~%.0f months of life left"), years * 12);

    gtk_label_set_text (card->headline, text);
  }

  {
    g_autofree char *per_day = g_format_size ((guint64) rate);

    basis = g_strdup_printf (_("at your recent write rate of %s/day"), per_day);
    gtk_label_set_text (card->basis, basis);
  }
}


static void
update_endurance (DriveCard *card)
{
  GsmDrive *drive = card->drive;
  GString *text = g_string_new (NULL);
  int percent = gsm_drive_get_percent_used (drive);
  uint64_t written = gsm_drive_get_bytes_written (drive);
  uint64_t rated = gsm_drive_get_rated_tbw (drive);
  uint64_t hours = gsm_drive_get_power_on_hours (drive);

  if (percent >= 0)
    g_string_append_printf (text, _("%d%% worn"), percent);

  if (written > 0) {
    g_autofree char *size = g_format_size (written);

    if (text->len > 0)
      g_string_append (text, " · ");
    g_string_append_printf (text, _("%s written"), size);
  }

  if (hours > 0) {
    if (text->len > 0)
      g_string_append (text, " · ");
    g_string_append_printf (text, _("%" G_GUINT64_FORMAT " h powered on"), hours);
  }

  if (rated > 0) {
    g_autofree char *size = g_format_size (rated);
    const char *provenance;

    switch (gsm_drive_get_endurance_source (drive)) {
      case GSM_ENDURANCE_USER:
        provenance = _("you set this");
        break;
      case GSM_ENDURANCE_DERIVED:
        provenance = _("implied by the drive’s own wear");
        break;
      case GSM_ENDURANCE_SPEC_DB:
        provenance = _("from the datasheet");
        break;
      case GSM_ENDURANCE_UNKNOWN:
      default:
        provenance = NULL;
        break;
    }

    g_string_append_c (text, '\n');
    g_string_append_printf (text, _("Rated endurance %s"), size);

    /* Never present a number without saying where it came from: a figure
     * derived from this drive and a figure copied off a datasheet deserve
     * very different amounts of trust. */
    if (provenance != NULL)
      g_string_append_printf (text, " (%s)", provenance);
  }

  /*
   * When the drive's own wear estimate and the datasheet disagree, say so.
   * Silently picking one would make the number look more certain than it is —
   * and on a WD SN740 the two differ by more than threefold.
   */
  if (gsm_drive_endurance_is_conflicted (drive)) {
    g_string_append_c (text, '\n');
    g_string_append (text,
                     _("The drive reports far less wear than its rating "
                       "predicts. Projections follow the drive."));
  }

  gtk_label_set_text (card->endurance, text->str);
  g_string_free (text, TRUE);
}


/*
 * The warning banner. Only fires on things that genuinely mean something —
 * the drive's own failure verdict, spare capacity under the manufacturer's
 * threshold, media errors, reallocated sectors. Every message says what to do
 * about it, because "SMART attribute 5 is nonzero" helps nobody.
 */
static void
update_banner (DriveCard *card)
{
  GsmDrive *drive = card->drive;
  GsmDriveHealth health = gsm_drive_get_health (drive);
  uint64_t reallocated = gsm_drive_get_reallocated (drive);
  uint64_t media_errors = gsm_drive_get_media_errors (drive);
  g_autofree char *message = NULL;

  if (health == GSM_DRIVE_HEALTH_FAILING) {
    message = g_strdup_printf (
      _("%s reports that it is failing. Back up anything on it now."),
      gsm_drive_get_model (drive));
  } else if (media_errors > 0) {
    message = g_strdup_printf (
      ngettext ("%s has lost %" G_GUINT64_FORMAT " block of data it could not recover. Back it up.",
                "%s has lost %" G_GUINT64_FORMAT " blocks of data it could not recover. Back it up.",
                (gulong) media_errors),
      gsm_drive_get_model (drive), media_errors);
  } else if (reallocated > 0) {
    message = g_strdup_printf (
      ngettext ("%s has remapped %" G_GUINT64_FORMAT " bad sector. Keep an eye on it.",
                "%s has remapped %" G_GUINT64_FORMAT " bad sectors. Keep an eye on it.",
                (gulong) reallocated),
      gsm_drive_get_model (drive), reallocated);
  } else if (health == GSM_DRIVE_HEALTH_ATTENTION) {
    message = g_strdup_printf (
      _("%s is near the end of its rated life."), gsm_drive_get_model (drive));
  }

  if (message == NULL) {
    adw_banner_set_revealed (card->banner, FALSE);
    return;
  }

  adw_banner_set_title (card->banner, message);
  adw_banner_set_revealed (card->banner, TRUE);
}


/* ------------------------------------------------------------------ */
/* Charts                                                              */
/* ------------------------------------------------------------------ */

static void
update_charts (DriveCard *card)
{
  GsmHealthView *self = card->view;
  const char *id = gsm_drive_get_id (card->drive);
  g_autoptr (GArray) history = NULL;
  g_autoptr (GArray) wear = g_array_new (FALSE, FALSE, sizeof (GsmChartPoint));
  g_autoptr (GPtrArray) blame = NULL;
  g_autoptr (GPtrArray) bars = NULL;
  gboolean any_estimated = FALSE;
  double years;

  history = gsm_health_db_get_history (self->db, id, self->range);

  for (guint i = 0; i < history->len; i++) {
    GsmHistoryPoint *p = &g_array_index (history, GsmHistoryPoint, i);
    GsmChartPoint point;

    if (p->percent_used < 0)
      continue;

    point.ts = p->ts;
    point.value = p->percent_used;
    g_array_append_val (wear, point);
  }

  gsm_history_chart_set_unit (GSM_HISTORY_CHART (card->wear_chart),
                              GSM_CHART_UNIT_PERCENT);
  gsm_history_chart_set_points (GSM_HISTORY_CHART (card->wear_chart), wear);

  /*
   * Scale the wear axis to a bit above where the drive actually is rather
   * than always to 100%. A drive at 12% on a 0–100 axis is a flat line
   * pinned to the floor, which tells the user nothing about the trend that
   * is the entire reason for the chart.
   */
  {
    double top = 10;

    for (guint i = 0; i < wear->len; i++)
      top = MAX (top, g_array_index (wear, GsmChartPoint, i).value);

    gsm_history_chart_set_value_range (GSM_HISTORY_CHART (card->wear_chart),
                                       0, MIN (100, top * 1.4));
  }

  /* Project forward to end of life, if we can say anything about the rate. */
  years = gsm_drive_project_years_left (card->drive, card->write_rate);

  if (years > 0 && years < 100 && wear->len > 0) {
    int64_t now = g_get_real_time () / G_USEC_PER_SEC;
    int64_t end = now + (int64_t) (years * 365.25 * 86400);

    gsm_history_chart_set_projection (GSM_HISTORY_CHART (card->wear_chart),
                                      end, 100);
  } else {
    gsm_history_chart_set_projection (GSM_HISTORY_CHART (card->wear_chart), 0, 0);
  }

  /* Blame */
  blame = gsm_health_db_get_blame (self->db, id, self->range);
  bars = g_ptr_array_new_with_free_func ((GDestroyNotify) gsm_chart_bar_free);

  for (guint i = 0; i < blame->len && i < 10; i++) {
    GsmBlameRow *row = g_ptr_array_index (blame, i);
    GsmChartBar *bar = g_new0 (GsmChartBar, 1);

    bar->label = g_strdup (row->name);
    bar->value = (double) row->wbytes;
    bar->estimated = !row->exact;

    if (bar->estimated)
      any_estimated = TRUE;

    g_ptr_array_add (bars, bar);
  }

  gsm_history_chart_set_bars (GSM_HISTORY_CHART (card->blame_chart), bars);

  /*
   * An empty chart is worse than no chart: it reserves a large blank rectangle
   * that reads as "broken" rather than "nothing recorded yet". Hide the plot
   * and let the note underneath explain why there is nothing to show.
   *
   * Two points, not one: a single sample draws an empty grid with no line on
   * it, which looks like a rendering failure rather than "we have only just
   * started watching this drive".
   */
  gtk_widget_set_visible (card->wear_heading, wear->len >= 2);
  gtk_widget_set_visible (card->wear_chart, wear->len >= 2);
  gtk_widget_set_visible (card->blame_heading, bars->len > 0);
  gtk_widget_set_visible (card->blame_chart, bars->len > 0);

  if (blame->len == 0) {
    gtk_label_set_text (card->blame_note,
                        self->tracking_enabled
                        ? _("No writes recorded yet.")
                        : _("Turn on wear tracking to find out what writes to "
                            "this drive."));
  } else if (any_estimated) {
    gtk_label_set_text (card->blame_note,
                        _("Hatched bars are estimated: the system reports how "
                          "much an app wrote, but not to which drive."));
  } else {
    gtk_label_set_text (card->blame_note, _("Measured per drive."));
  }
}


/* ------------------------------------------------------------------ */
/* The privileged unlock                                               */
/* ------------------------------------------------------------------ */

/*
 * Parse the helper's key=value output. Deliberately a dumb format: the helper
 * runs as root, and the less it has to serialise the smaller the surface for
 * getting that wrong.
 */
static void
apply_helper_output (DriveCard  *card,
                     const char *output)
{
  g_auto (GStrv) lines = g_strsplit (output, "\n", -1);
  GsmDriveInfo info = { 0 };

  info.temperature = -1;
  info.percent_used = -1;
  info.spare_percent = -1;
  info.spare_threshold = -1;
  info.present = TRUE;
  info.smart_source = GSM_SMART_SOURCE_HELPER;

  for (guint i = 0; lines[i] != NULL; i++) {
    char *eq = strchr (lines[i], '=');
    const char *key, *value;

    if (eq == NULL)
      continue;

    *eq = '\0';
    key = lines[i];
    value = eq + 1;

    if (g_strcmp0 (key, "model") == 0)
      info.model = g_strdup (value);
    else if (g_strcmp0 (key, "serial") == 0)
      info.serial = g_strdup (value);
    else if (g_strcmp0 (key, "firmware") == 0)
      info.firmware = g_strdup (value);
    else if (g_strcmp0 (key, "percent_used") == 0)
      info.percent_used = atoi (value);
    else if (g_strcmp0 (key, "temperature_c") == 0)
      info.temperature = atoi (value);
    else if (g_strcmp0 (key, "available_spare") == 0)
      info.spare_percent = atoi (value);
    else if (g_strcmp0 (key, "available_spare_threshold") == 0)
      info.spare_threshold = atoi (value);
    else if (g_strcmp0 (key, "data_units_written_bytes") == 0)
      info.bytes_written = g_ascii_strtoull (value, NULL, 10);
    else if (g_strcmp0 (key, "data_units_read_bytes") == 0)
      info.bytes_read = g_ascii_strtoull (value, NULL, 10);
    else if (g_strcmp0 (key, "power_on_hours") == 0)
      info.power_on_hours = g_ascii_strtoull (value, NULL, 10);
    else if (g_strcmp0 (key, "power_cycles") == 0)
      info.power_cycles = g_ascii_strtoull (value, NULL, 10);
    else if (g_strcmp0 (key, "unsafe_shutdowns") == 0)
      info.unsafe_shutdowns = g_ascii_strtoull (value, NULL, 10);
    else if (g_strcmp0 (key, "media_errors") == 0)
      info.media_errors = g_ascii_strtoull (value, NULL, 10);
    else if (g_strcmp0 (key, "smart_failing") == 0)
      info.smart_failing = atoi (value) != 0;
    else if (g_strcmp0 (key, "wear_unavailable") == 0)
      info.wear_unavailable = atoi (value) != 0;
    else if (g_strcmp0 (key, "attr.5.raw") == 0)
      info.reallocated = g_ascii_strtoull (value, NULL, 10);
  }

  gsm_drive_apply_info (card->drive, &info);
  gsm_drive_resolve_endurance (
    card->drive,
    gsm_health_db_get_user_tbw (card->view->db, gsm_drive_get_id (card->drive)));

  gsm_health_db_record_drive (card->view->db, card->drive);
  gsm_health_db_commit (card->view->db, NULL);

  gsm_drive_info_clear (&info);

  update_headline (card);
  update_endurance (card);
  update_banner (card);
  update_charts (card);
}


static void
unlock_finished (GObject      *source,
                 GAsyncResult *result,
                 gpointer      user_data)
{
  DriveCard *card = user_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *stdout_text = NULL;
  g_autofree char *stderr_text = NULL;

  if (!g_subprocess_communicate_utf8_finish (G_SUBPROCESS (source), result,
                                             &stdout_text, &stderr_text,
                                             &error)) {
    g_warning ("health: SMART helper failed: %s", error->message);
    return;
  }

  gtk_widget_set_sensitive (card->unlock_button, TRUE);

  if (!g_subprocess_get_successful (G_SUBPROCESS (source))) {
    /*
     * pkexec exits 126 when the user dismisses the prompt. That is a choice,
     * not a fault, so it must not raise an error dialog at them.
     */
    if (g_subprocess_get_exit_status (G_SUBPROCESS (source)) == 126)
      return;

    adw_banner_set_title (card->banner,
                          stderr_text != NULL && *stderr_text != '\0'
                          ? g_strchomp (stderr_text)
                          : _("Could not read SMART data from this drive."));
    adw_banner_set_revealed (card->banner, TRUE);
    return;
  }

  apply_helper_output (card, stdout_text);
}


/*
 * Where the privileged helper lives.
 *
 * In a release build this is fixed at compile time and nothing can redirect
 * it: it is the binary we are about to ask pkexec to run as root, and letting
 * the environment choose that would be handing an attacker a root exec. The
 * override exists only so an uninstalled dev build can reach its own helper,
 * and is compiled out entirely otherwise — the same lock the dev IPC socket
 * uses.
 */
static char *
smart_helper_path (void)
{
#ifdef HAVE_DEV_IPC
  const char *override = g_getenv ("GSM_SMART_HELPER");

  if (override != NULL && *override != '\0')
    return g_strdup (override);
#endif

  return g_build_filename (GSM_LIBEXEC_DIR, "gsm-smart", NULL);
}


static void
on_unlock_clicked (GtkButton *button,
                   gpointer   user_data)
{
  DriveCard *card = user_data;
  g_autoptr (GSubprocess) proc = NULL;
  g_autoptr (GError) error = NULL;
  const char *device = gsm_drive_get_device (card->drive);
  g_autofree char *helper = smart_helper_path ();

  if (device == NULL)
    return;

  gtk_widget_set_sensitive (GTK_WIDGET (button), FALSE);

  /* One drive, one read, then the helper exits. Nothing privileged stays
   * resident, and the background sampler never goes down this path at all. */
  proc = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                           G_SUBPROCESS_FLAGS_STDERR_PIPE,
                           &error,
                           "pkexec", helper, device, NULL);
  if (proc == NULL) {
    g_warning ("health: cannot launch helper: %s", error->message);
    gtk_widget_set_sensitive (GTK_WIDGET (button), TRUE);
    return;
  }

  g_subprocess_communicate_utf8_async (proc, NULL, NULL, unlock_finished, card);
}


/* ------------------------------------------------------------------ */
/* Building a drive card                                               */
/* ------------------------------------------------------------------ */

static void
on_range_changed (GtkToggleButton *button,
                  gpointer         user_data)
{
  GsmHealthView *self = user_data;
  GsmHistoryRange range;

  if (!gtk_toggle_button_get_active (button))
    return;

  range = (GsmHistoryRange) GPOINTER_TO_INT (
    g_object_get_data (G_OBJECT (button), "range"));

  if (self->range == range)
    return;

  self->range = range;
  refresh (self);
}


static AdwPreferencesGroup *
build_range_switcher (GsmHealthView *self)
{
  AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkToggleButton *first = NULL;
  const struct {
    const char *label;
    GsmHistoryRange range;
  } ranges[] = {
    { N_("24 hours"), GSM_HISTORY_RANGE_DAY },
    { N_("30 days"),  GSM_HISTORY_RANGE_MONTH },
    { N_("1 year"),   GSM_HISTORY_RANGE_YEAR },
  };

  gtk_widget_add_css_class (box, "linked");
  gtk_widget_set_halign (box, GTK_ALIGN_CENTER);

  for (gsize i = 0; i < G_N_ELEMENTS (ranges); i++) {
    GtkWidget *button = gtk_toggle_button_new_with_label (_(ranges[i].label));

    g_object_set_data (G_OBJECT (button), "range",
                       GINT_TO_POINTER (ranges[i].range));

    if (first == NULL)
      first = GTK_TOGGLE_BUTTON (button);
    else
      gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (button), first);

    if (ranges[i].range == self->range)
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (button), TRUE);

    g_signal_connect (button, "toggled", G_CALLBACK (on_range_changed), self);

    gtk_box_append (GTK_BOX (box), button);
  }

  adw_preferences_group_add (group, box);

  return group;
}


static GtkWidget *
section_heading (const char *text)
{
  GtkWidget *label = gtk_label_new (text);

  gtk_widget_add_css_class (label, "heading");
  gtk_widget_add_css_class (label, "dim-label");
  gtk_widget_set_halign (label, GTK_ALIGN_START);
  gtk_widget_set_margin_top (label, 12);

  return label;
}


static DriveCard *
drive_card_new (GsmHealthView *self,
                GsmDrive      *drive)
{
  DriveCard *card = g_new0 (DriveCard, 1);
  GtkWidget *box;
  GtkWidget *headline;
  GtkWidget *basis;
  GtkWidget *endurance;
  GtkWidget *note;

  card->view = self;
  card->drive = g_object_ref (drive);
  card->write_rate = -1;   /* "not enough history yet", not "wrote nothing" */

  card->group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);

  card->banner = ADW_BANNER (adw_banner_new (""));
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (card->banner));

  headline = gtk_label_new (NULL);
  gtk_widget_add_css_class (headline, "title-1");
  gtk_widget_set_halign (headline, GTK_ALIGN_START);
  gtk_label_set_wrap (GTK_LABEL (headline), TRUE);
  card->headline = GTK_LABEL (headline);
  gtk_box_append (GTK_BOX (box), headline);

  basis = gtk_label_new (NULL);
  gtk_widget_add_css_class (basis, "dim-label");
  gtk_widget_set_halign (basis, GTK_ALIGN_START);
  gtk_label_set_wrap (GTK_LABEL (basis), TRUE);
  card->basis = GTK_LABEL (basis);
  gtk_box_append (GTK_BOX (box), basis);

  card->unlock_button = gtk_button_new_with_label (_("Unlock Full SMART…"));
  gtk_widget_add_css_class (card->unlock_button, "suggested-action");
  gtk_widget_set_halign (card->unlock_button, GTK_ALIGN_START);
  gtk_widget_set_margin_top (card->unlock_button, 6);
  gtk_widget_set_visible (card->unlock_button, FALSE);
  g_signal_connect (card->unlock_button, "clicked",
                    G_CALLBACK (on_unlock_clicked), card);
  gtk_box_append (GTK_BOX (box), card->unlock_button);

  endurance = gtk_label_new (NULL);
  gtk_widget_add_css_class (endurance, "dim-label");
  gtk_widget_add_css_class (endurance, "caption");
  gtk_widget_set_halign (endurance, GTK_ALIGN_START);
  gtk_label_set_wrap (GTK_LABEL (endurance), TRUE);
  gtk_label_set_xalign (GTK_LABEL (endurance), 0);
  gtk_widget_set_margin_top (endurance, 6);
  card->endurance = GTK_LABEL (endurance);
  gtk_box_append (GTK_BOX (box), endurance);

  card->wear_heading = section_heading (_("Wear over time"));
  gtk_box_append (GTK_BOX (box), card->wear_heading);

  card->wear_chart = gsm_history_chart_new ();
  gsm_history_chart_set_mode (GSM_HISTORY_CHART (card->wear_chart),
                              GSM_CHART_LINE);
  gtk_box_append (GTK_BOX (box), card->wear_chart);

  card->blame_heading = section_heading (_("What writes to this drive"));
  gtk_box_append (GTK_BOX (box), card->blame_heading);

  card->blame_chart = gsm_history_chart_new ();
  gsm_history_chart_set_mode (GSM_HISTORY_CHART (card->blame_chart),
                              GSM_CHART_BAR);
  gtk_box_append (GTK_BOX (box), card->blame_chart);

  note = gtk_label_new (NULL);
  gtk_widget_add_css_class (note, "dim-label");
  gtk_widget_add_css_class (note, "caption");
  gtk_widget_set_halign (note, GTK_ALIGN_START);
  gtk_label_set_wrap (GTK_LABEL (note), TRUE);
  gtk_label_set_xalign (GTK_LABEL (note), 0);
  card->blame_note = GTK_LABEL (note);
  gtk_box_append (GTK_BOX (box), note);

  adw_preferences_group_add (card->group, box);

  return card;
}


/*
 * AdwPreferencesPage only appends, but drives are discovered at runtime while
 * Battery and Wear Tracking come from the template — so left alone, the
 * hardware people opened this tab to look at would sit underneath a settings
 * switch. Re-append the trailing groups whenever a new drive card shows up to
 * push them back to the bottom.
 */
static void
push_trailing_groups_down (GsmHealthView *self)
{
  g_object_ref (self->battery_group);
  g_object_ref (self->tracking_group);

  adw_preferences_page_remove (self->page, self->battery_group);
  adw_preferences_page_remove (self->page, self->tracking_group);

  adw_preferences_page_add (self->page, self->battery_group);
  adw_preferences_page_add (self->page, self->tracking_group);

  g_object_unref (self->battery_group);
  g_object_unref (self->tracking_group);
}


static void
update_card_title (DriveCard *card)
{
  GsmDrive *drive = card->drive;
  GString *subtitle = g_string_new (NULL);
  const char *device = gsm_drive_get_device (drive);
  const char *connection = gsm_drive_get_connection (drive);
  uint64_t size = gsm_drive_get_size (drive);

  if (device != NULL)
    g_string_append (subtitle, device);

  if (size > 0) {
    g_autofree char *formatted = g_format_size (size);

    if (subtitle->len > 0)
      g_string_append (subtitle, " · ");
    g_string_append (subtitle, formatted);
  }

  if (connection != NULL && *connection != '\0') {
    if (subtitle->len > 0)
      g_string_append (subtitle, " · ");
    g_string_append (subtitle, connection);
  }

  if (!gsm_drive_get_present (drive)) {
    int64_t last_seen = gsm_drive_get_last_seen (drive);

    if (last_seen > 0) {
      g_autoptr (GDateTime) when = g_date_time_new_from_unix_local (last_seen);
      g_autofree char *formatted = g_date_time_format (when, "%x");

      g_string_append_printf (subtitle, _(" · last seen %s"), formatted);
    }
  }

  adw_preferences_group_set_title (card->group,
                                   gsm_drive_get_model (drive) ?: _("Drive"));
  adw_preferences_group_set_description (card->group, subtitle->str);

  g_string_free (subtitle, TRUE);
}


/* ------------------------------------------------------------------ */
/* Battery                                                             */
/* ------------------------------------------------------------------ */

static void
refresh_batteries (GsmHealthView *self)
{
  g_autoptr (GPtrArray) batteries = gsm_battery_collect ();

  /*
   * Rebuilding is fine: there are at most two of these and they change on the
   * scale of weeks. We remove the rows we remember adding rather than walking
   * the group's children — an AdwPreferencesGroup's first child is its own
   * internal box, not our rows, so removing by traversal removes nothing and
   * spins.
   */
  for (guint i = 0; i < self->battery_rows->len; i++) {
    adw_preferences_group_remove (self->battery_group,
                                  g_ptr_array_index (self->battery_rows, i));
  }
  g_ptr_array_set_size (self->battery_rows, 0);

  gtk_widget_set_visible (GTK_WIDGET (self->battery_group), batteries->len > 0);

  for (guint i = 0; i < batteries->len; i++) {
    GsmBatteryInfo *info = g_ptr_array_index (batteries, i);
    GtkWidget *row = adw_action_row_new ();
    GString *subtitle = g_string_new (NULL);
    GtkWidget *meter;

    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row),
                                   info->model && *info->model
                                   ? info->model : info->id);

    if (info->health_percent >= 0 && info->design_wh > 0) {
      g_string_append_printf (subtitle,
                              _("%d%% of original capacity"),
                              info->health_percent);
      g_string_append_printf (subtitle, " (%.1f / %.1f Wh)",
                              info->full_wh, info->design_wh);
    }

    if (info->cycle_count >= 0) {
      if (subtitle->len > 0)
        g_string_append (subtitle, " · ");
      g_string_append_printf (subtitle,
                              ngettext ("%d charge cycle",
                                        "%d charge cycles",
                                        info->cycle_count),
                              info->cycle_count);
    }

    adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle->str);
    g_string_free (subtitle, TRUE);

    if (info->health_percent >= 0) {
      meter = gtk_level_bar_new_for_interval (0, 100);
      gtk_level_bar_set_value (GTK_LEVEL_BAR (meter), info->health_percent);
      gtk_widget_set_valign (meter, GTK_ALIGN_CENTER);
      gtk_widget_set_size_request (meter, 120, -1);
      adw_action_row_add_suffix (ADW_ACTION_ROW (row), meter);
    }

    adw_preferences_group_add (self->battery_group, row);
    g_ptr_array_add (self->battery_rows, row);
  }
}


/* ------------------------------------------------------------------ */
/* Refresh                                                             */
/* ------------------------------------------------------------------ */

/*
 * Reading SMART means a GetManagedObjects round trip plus a SmartGetAttributes
 * call per drive, all synchronous. Doing that on the main loop would freeze the
 * entire window — including the Processes tab — for as long as udisksd takes to
 * answer, which on a drive that has spun down or a wedged USB bridge can be
 * seconds. So it runs on a worker thread and the widgets are only touched once
 * it comes back.
 */
static void
collect_in_thread (GTask        *task,
                   gpointer      source,
                   gpointer      task_data,
                   GCancellable *cancellable)
{
  GDBusConnection *bus = task_data;
  GError *error = NULL;
  GPtrArray *infos;

  (void) source;
  (void) cancellable;

  infos = gsm_udisks_collect (bus, &error);

  if (infos == NULL)
    g_task_return_error (task, error);
  else
    g_task_return_pointer (task, infos, (GDestroyNotify) g_ptr_array_unref);
}


/*
 * Apply a completed snapshot to the cards. Main thread only — it touches
 * widgets.
 */
static void
apply_drives (GsmHealthView *self,
              GPtrArray     *infos)
{
  g_autoptr (GHashTable) seen =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  GHashTableIter iter;
  const char *id;
  DriveCard *card;

  for (guint i = 0; i < infos->len; i++) {
    GsmDriveInfo *info = g_ptr_array_index (infos, i);

    if (info->serial == NULL)
      continue;

    g_hash_table_add (seen, g_strdup (info->serial));

    card = g_hash_table_lookup (self->cards, info->serial);

    if (card == NULL) {
      g_autoptr (GsmDrive) drive = gsm_drive_new (info->serial);

      card = drive_card_new (self, drive);
      g_hash_table_insert (self->cards, g_strdup (info->serial), card);
      adw_preferences_page_add (self->page, card->group);
      push_trailing_groups_down (self);
    }

    /*
     * A drive that has already been unlocked keeps its richer data: the
     * merge rules in GsmDrive refuse to let this thinner unprivileged
     * snapshot overwrite what the helper read.
     */
    gsm_drive_apply_info (card->drive, info);
    gsm_drive_resolve_endurance (
      card->drive, gsm_health_db_get_user_tbw (self->db, info->serial));

    /* Both the headline and the chart's projection are built from this, so
     * measure it once rather than running the same query twice. */
    card->write_rate = gsm_health_db_get_write_rate (self->db, info->serial, 30);

    update_card_title (card);
    update_headline (card);
    update_endurance (card);
    update_banner (card);
    update_charts (card);
  }

  /* Mark drives we know about but can no longer see as absent, rather than
   * deleting their card — an unplugged SSD still has a history worth reading. */
  g_hash_table_iter_init (&iter, self->cards);
  while (g_hash_table_iter_next (&iter, (gpointer *) &id, (gpointer *) &card)) {
    if (g_hash_table_contains (seen, id))
      continue;

    gsm_drive_set_present (card->drive, FALSE);
    update_card_title (card);
  }

  refresh_batteries (self);

  /* History size / retention, so the record we keep is never a mystery. */
  {
    uint64_t bytes = gsm_health_db_get_size_bytes (self->db);
    int64_t oldest = gsm_health_db_get_oldest_sample (self->db);
    g_autofree char *size = g_format_size (bytes);
    g_autofree char *subtitle = NULL;

    if (oldest > 0) {
      int64_t now = g_get_real_time () / G_USEC_PER_SEC;
      int days = (int) ((now - oldest) / 86400);

      subtitle = g_strdup_printf (
        ngettext ("%d day of history · %s",
                  "%d days of history · %s", days),
        days, size);
    } else {
      subtitle = g_strdup_printf (_("No history yet · %s"), size);
    }

    adw_action_row_set_subtitle (self->history_row, subtitle);
  }
}


static void
refresh_finished (GObject      *source,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  GsmHealthView *self = GSM_HEALTH_VIEW (source);
  g_autoptr (GPtrArray) infos = NULL;
  g_autoptr (GError) error = NULL;

  (void) user_data;

  self->refreshing = FALSE;

  infos = g_task_propagate_pointer (G_TASK (result), &error);

  if (infos == NULL) {
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning ("health: %s", error->message);
    return;
  }

  /* The GTask kept us alive, but the view may have been disposed while the
   * worker was in flight — in which case the widgets and the database are
   * already gone and there is nothing to update. */
  if (self->db == NULL)
    return;

  apply_drives (self, infos);
}


static void
refresh (GsmHealthView *self)
{
  g_autoptr (GTask) task = NULL;

  if (self->bus == NULL || self->db == NULL)
    return;

  /* udisks can be slower than our refresh interval. Overlapping collections
   * would just queue up behind each other and fight over the same cards. */
  if (self->refreshing)
    return;

  self->refreshing = TRUE;

  task = g_task_new (self, self->cancellable, refresh_finished, NULL);
  g_task_set_source_tag (task, refresh);
  g_task_set_task_data (task, g_object_ref (self->bus), g_object_unref);
  g_task_run_in_thread (task, collect_in_thread);
}


static gboolean
refresh_timeout (gpointer data)
{
  refresh (GSM_HEALTH_VIEW (data));

  return G_SOURCE_CONTINUE;
}


/* ------------------------------------------------------------------ */
/* Tracking toggle                                                     */
/* ------------------------------------------------------------------ */

static void
run_systemctl (const char *const *args)
{
  g_autoptr (GSubprocess) proc = NULL;
  g_autoptr (GError) error = NULL;

  proc = g_subprocess_newv (args,
                            G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                            G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                            &error);
  if (proc == NULL)
    g_warning ("health: systemctl: %s", error->message);
}


static void
on_tracking_toggled (GsmHealthView *self)
{
  gboolean enabled = adw_switch_row_get_active (self->tracking_row);

  if (enabled == self->tracking_enabled)
    return;

  self->tracking_enabled = enabled;

  /*
   * g_settings_bind pushes the stored value into the switch during init, which
   * fires this handler before the user has touched anything. Acting on that
   * would mean merely opening the tab runs `systemctl --user enable --now` and
   * restarts a service the user may have deliberately stopped by hand. Only a
   * real interaction should start or stop anything.
   */
  if (!self->bound)
    return;

  /*
   * The sampler is a plain user service: no privilege, and entirely the
   * user's to switch off. Enabling it is also the moment they consent to a
   * local record of which apps write how much, which is why it is off until
   * they ask for it.
   */
  if (enabled) {
    const char *const enable[] = {
      "systemctl", "--user", "enable", "--now", SAMPLER_UNIT, NULL
    };

    run_systemctl (enable);
  } else {
    const char *const disable[] = {
      "systemctl", "--user", "disable", "--now", SAMPLER_UNIT, NULL
    };

    run_systemctl (disable);
  }

  refresh (self);
}


static void
on_clear_history (GsmHealthView *self)
{
  gsm_health_db_clear (self->db);
  refresh (self);
}


static void
io_accounting_finished (GObject      *source,
                        GAsyncResult *result,
                        gpointer      user_data)
{
  GsmHealthView *self = user_data;
  g_autoptr (GError) error = NULL;

  if (!g_subprocess_wait_check_finish (G_SUBPROCESS (source), result, &error)) {
    /* 126 is pkexec's "the user said no", which is an answer, not a failure. */
    if (g_subprocess_get_exit_status (G_SUBPROCESS (source)) != 126)
      g_warning ("health: enabling IO accounting failed: %s", error->message);
    return;
  }

  adw_banner_set_title (
    self->io_accounting_banner,
    _("Per-app I/O accounting is enabled. Log out and back in to start "
      "attributing writes to individual apps."));
  adw_banner_set_button_label (self->io_accounting_banner, NULL);
}


static void
on_enable_io_accounting (GsmHealthView *self)
{
  g_autoptr (GSubprocess) proc = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree char *helper =
    g_build_filename (GSM_LIBEXEC_DIR, "gsm-io-accounting", NULL);

  proc = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_SILENCE,
                           &error,
                           "pkexec", helper, NULL);
  if (proc == NULL) {
    g_warning ("health: cannot launch IO accounting helper: %s", error->message);
    return;
  }

  g_subprocess_wait_check_async (proc, NULL, io_accounting_finished, self);
}


/* ------------------------------------------------------------------ */

static void
gsm_health_view_map (GtkWidget *widget)
{
  GsmHealthView *self = GSM_HEALTH_VIEW (widget);

  GTK_WIDGET_CLASS (gsm_health_view_parent_class)->map (widget);

  refresh (self);
}


static void
gsm_health_view_dispose (GObject *object)
{
  GsmHealthView *self = GSM_HEALTH_VIEW (object);

  g_clear_handle_id (&self->timeout, g_source_remove);

  /* An in-flight udisks collection holds a ref on us, so it cannot dangle —
   * but there is no point letting it finish, and refresh_finished checks for
   * a cleared db before touching anything. */
  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);

  g_clear_pointer (&self->cards, g_hash_table_unref);
  g_clear_pointer (&self->battery_rows, g_ptr_array_unref);
  g_clear_object (&self->db);
  g_clear_object (&self->bus);
  g_clear_object (&self->settings);

  G_OBJECT_CLASS (gsm_health_view_parent_class)->dispose (object);
}


static void
gsm_health_view_class_init (GsmHealthViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = gsm_health_view_dispose;
  widget_class->map = gsm_health_view_map;

  gtk_widget_class_set_template_from_resource (
    widget_class, "/org/gnome/gnome-system-monitor/data/health.ui");

  gtk_widget_class_bind_template_child (widget_class, GsmHealthView, page);
  gtk_widget_class_bind_template_child (widget_class, GsmHealthView, battery_group);
  gtk_widget_class_bind_template_child (widget_class, GsmHealthView, tracking_group);
  gtk_widget_class_bind_template_child (widget_class, GsmHealthView, tracking_row);
  gtk_widget_class_bind_template_child (widget_class, GsmHealthView, history_row);
  gtk_widget_class_bind_template_child (widget_class, GsmHealthView, io_accounting_banner);

  gtk_widget_class_bind_template_callback (widget_class, on_tracking_toggled);
  gtk_widget_class_bind_template_callback (widget_class, on_clear_history);
  gtk_widget_class_bind_template_callback (widget_class, on_enable_io_accounting);
}


static void
gsm_health_view_init (GsmHealthView *self)
{
  g_autoptr (GError) error = NULL;

  gtk_widget_init_template (GTK_WIDGET (self));

  self->cards = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                       (GDestroyNotify) drive_card_free);
  self->battery_rows = g_ptr_array_new ();
  self->cancellable = g_cancellable_new ();
  self->range = GSM_HISTORY_RANGE_MONTH;
  self->settings = g_settings_new (GSM_GSETTINGS_SCHEMA);

  self->db = gsm_health_db_new (NULL, &error);
  if (self->db == NULL)
    g_warning ("health: no history database: %s", error->message);

  self->bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);
  if (self->bus == NULL)
    g_warning ("health: no system bus: %s", error->message);

  g_settings_bind (self->settings, GSM_SETTING_HEALTH_TRACKING,
                   self->tracking_row, "active", G_SETTINGS_BIND_DEFAULT);

  self->tracking_enabled = adw_switch_row_get_active (self->tracking_row);
  self->bound = TRUE;

  adw_preferences_page_add (self->page, build_range_switcher (self));

  /*
   * Say plainly that per-app blame on this system cannot name a drive — and
   * offer the fix rather than just the complaint. Hidden entirely on systems
   * that already delegate io, where there is nothing to fix.
   */
  {
    g_autoptr (GPtrArray) cgroups = gsm_blame_collect ();

    self->io_delegated = gsm_blame_user_io_is_delegated (cgroups);
    adw_banner_set_revealed (self->io_accounting_banner, !self->io_delegated);
  }

  self->timeout = g_timeout_add_seconds (REFRESH_INTERVAL_SECS,
                                         refresh_timeout, self);
}
