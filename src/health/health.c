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

#include <string.h>

#include <glib/gi18n.h>

#include "gsm-battery.h"
#include "gsm-blame.h"
#include "gsm-drive-spec.h"
#include "gsm-health-db.h"
#include "gsm-history-chart.h"
#include "gsm-onbehalf.h"
#include "gsm-procio.h"
#include "gsm-smart-file.h"
#include "gsm-udisks.h"
#include "health.h"
#include "dev-ipc.h"
#include "settings-keys.h"

#define SAMPLER_UNIT "gnome-system-monitor-health-sampler.service"

/* Refreshing SMART is a D-Bus round trip per drive; wear moves on the scale
 * of days, so there is nothing to gain from doing it often. */
#define REFRESH_INTERVAL_SECS 30

/* The timer that runs the system SMART reader, asked about over D-Bus. */
#define SMART_READER_TIMER "gsm-smart-reader.timer"

/* The reader runs every ten minutes (the file goes stale after three runs). */
#define SMART_READER_PERIOD_MINUTES 10

/*
 * The shortest stretch of recorded write counts the database needs before it
 * reports a write rate. Kept equal to the database's own minimum so the
 * "collecting history" line can say how much more is needed.
 */
#define RATE_NEEDS_SECONDS (6 * 3600)

/* The trailing window the write rate is measured over. */
#define RATE_WINDOW_DAYS 30

/* Whether the system SMART reader's service is installed and switched on,
 * as systemd's unit-file state says. Only asked when the reader's snapshot
 * is missing, because only then does it explain anything. */
typedef enum {
  SERVICE_UNKNOWN,         /* systemd could not say */
  SERVICE_NOT_INSTALLED,
  SERVICE_TURNED_OFF,
  SERVICE_TURNED_ON,
} ServiceState;

/* What the reader's snapshot says about one USB disk udisks cannot read. */
typedef struct {
  gboolean            applies;       /* a USB disk with no udisks SMART */
  ServiceState        service;
  GsmSmartFileState   file_state;
  int64_t             generated_at;  /* when the snapshot was written */
  gboolean            listed;        /* the snapshot names this disk */
  GsmSmartDriveStatus status;
  char               *usb_id;        /* "0634:5605" */
  char               *bridge;        /* "asmedia" */
  gboolean            observed;      /* bridge basis: not in smartmontools */
  int64_t             last_ok_at;    /* 0 when never */
  gboolean            have_figures;  /* figures taken from the snapshot */
} UsbReading;

typedef struct {
  GsmHealthView *view;
  GsmDrive      *drive;
  UsbReading     usb;

  AdwPreferencesGroup *group;
  AdwBanner           *banner;

  GtkLabel  *headline;
  GtkLabel  *basis;
  GtkLabel  *source_note;
  GtkLabel  *endurance;

  GtkWidget *wear_heading;
  GtkWidget *wear_chart;
  GtkWidget *blame_heading;
  GtkWidget *blame_chart;
  GtkLabel  *blame_note;

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
usb_reading_clear (UsbReading *usb)
{
  g_clear_pointer (&usb->usb_id, g_free);
  g_clear_pointer (&usb->bridge, g_free);
  memset (usb, 0, sizeof *usb);
}


static void
drive_card_free (DriveCard *card)
{
  if (card == NULL)
    return;

  usb_reading_clear (&card->usb);
  g_clear_object (&card->drive);
  g_free (card);
}


/* ------------------------------------------------------------------ */
/* Saying where the figures came from                                  */
/* ------------------------------------------------------------------ */

/* "5 minutes", "2 hours", "3 days": an age to read, rounded down. */
static char *
age_text (int64_t seconds)
{
  if (seconds < 60)
    return g_strdup (_("less than a minute"));

  if (seconds < 3600) {
    int minutes = (int) (seconds / 60);

    return g_strdup_printf (ngettext ("%d minute", "%d minutes", minutes), minutes);
  }

  if (seconds < 86400) {
    int hours = (int) (seconds / 3600);

    return g_strdup_printf (ngettext ("%d hour", "%d hours", hours), hours);
  }

  {
    int days = (int) (seconds / 86400);

    return g_strdup_printf (ngettext ("%d day", "%d days", days), days);
  }
}


static int64_t
now_seconds (void)
{
  return g_get_real_time () / G_USEC_PER_SEC;
}


/* The bridge as a person writes it. The reader names the protocol in
 * lower case; an unfamiliar word is shown as it came. */
static const char *
bridge_display_name (const char *bridge)
{
  if (g_strcmp0 (bridge, "asmedia") == 0)
    return "ASMedia";
  if (g_strcmp0 (bridge, "jmicron") == 0)
    return "JMicron";
  if (g_strcmp0 (bridge, "realtek") == 0)
    return "Realtek";
  if (g_strcmp0 (bridge, "sat") == 0)
    return "SAT";

  return bridge != NULL ? bridge : "unknown";
}


/*
 * A drive's figures are shown only when this refresh produced them: from
 * udisks, or from a fresh snapshot of the system SMART reader. A USB disk
 * the snapshot could not describe keeps nothing on screen from an earlier
 * refresh that would look current.
 */
static gboolean
card_has_figures (DriveCard *card)
{
  GsmSmartSource source = gsm_drive_get_smart_source (card->drive);

  if (source != GSM_SMART_SOURCE_UDISKS && source != GSM_SMART_SOURCE_HELPER)
    return FALSE;

  return !card->usb.applies || card->usb.have_figures;
}


/*
 * Why a USB disk has no figures, in words, from the service's state, the
 * snapshot's state and the disk's own status: never a zero, never "locked".
 * Sets the headline (a static string) and returns the explanation.
 */
static char *
usb_explanation (const UsbReading *usb,
                 const char      **headline)
{
  g_autofree char *age = NULL;

  *headline = _("No health data");

  switch (usb->file_state) {
    case GSM_SMART_FILE_MISSING:
      switch (usb->service) {
        case SERVICE_NOT_INSTALLED:
          return g_strdup (_("SMART for USB drives comes from the "
                             "gsm-smart-reader service, which is not installed."));
        case SERVICE_TURNED_OFF:
          return g_strdup (_("SMART for USB drives comes from the "
                             "gsm-smart-reader service, which is turned off. "
                             "To turn it on: "
                             "sudo systemctl enable --now " SMART_READER_TIMER));
        case SERVICE_TURNED_ON:
          return g_strdup_printf (_("SMART for USB drives comes from the "
                                    "gsm-smart-reader service. It is turned on "
                                    "but has not run yet; it runs shortly after "
                                    "boot and every %d minutes."),
                                  SMART_READER_PERIOD_MINUTES);
        case SERVICE_UNKNOWN:
        default:
          return g_strdup (_("SMART for USB drives comes from the "
                             "gsm-smart-reader service. It has not written "
                             "anything yet, and whether it is installed could "
                             "not be determined."));
      }
    case GSM_SMART_FILE_UNREADABLE:
      return g_strdup (_("The gsm-smart-reader service has written a "
                         "snapshot, but this program cannot read it."));
    case GSM_SMART_FILE_MALFORMED:
      return g_strdup (_("The gsm-smart-reader snapshot could not be "
                         "understood, so nothing from it is shown."));
    case GSM_SMART_FILE_UNKNOWN_SCHEMA:
      return g_strdup (_("The gsm-smart-reader snapshot is in a newer format "
                         "than this version reads."));
    case GSM_SMART_FILE_STALE:
      age = age_text (now_seconds () - usb->generated_at);
      *headline = _("Reading is out of date");
      return g_strdup_printf (_("The gsm-smart-reader service last ran %s ago, "
                                "more than %d minutes, so its figures are not "
                                "shown. It should run every %d minutes; check "
                                "that " SMART_READER_TIMER " is active."),
                              age, GSM_SMART_FILE_MAX_AGE_SECONDS / 60,
                              SMART_READER_PERIOD_MINUTES);
    case GSM_SMART_FILE_OK:
    default:
      break;
  }

  if (!usb->listed)
    return g_strdup_printf (_("The last gsm-smart-reader run did not list this "
                              "drive. It reads USB drives every %d minutes and "
                              "when one is plugged in."),
                            SMART_READER_PERIOD_MINUTES);

  switch (usb->status) {
    case GSM_SMART_DRIVE_ASLEEP:
      *headline = _("Asleep");
      return g_strdup (_("This drive is asleep. The reader leaves a sleeping "
                         "drive alone rather than wake it, and has no earlier "
                         "reading of it."));
    case GSM_SMART_DRIVE_UNSUPPORTED:
      *headline = _("Enclosure not supported");
      return g_strdup_printf (_("This USB enclosure (USB %s) is not one the "
                                "reader knows how to talk to, so it sends it "
                                "nothing."),
                              usb->usb_id != NULL ? usb->usb_id : _("id unknown"));
    case GSM_SMART_DRIVE_FAILED:
      *headline = _("Could not be read");
      return g_strdup (_("The reader asked this drive for SMART data and got "
                         "no usable answer."));
    case GSM_SMART_DRIVE_GARBLED:
      *headline = _("Answer not believed");
      return g_strdup (_("This drive answered, but the answer failed the "
                         "reader's sanity checks, so it is not shown."));
    case GSM_SMART_DRIVE_UNKNOWN_STATUS:
      return g_strdup (_("The reader reported a state this version does not "
                         "know."));
    case GSM_SMART_DRIVE_OK:
    default:
      return g_strdup (_("The reader read this drive, but it gave none of the "
                         "figures shown here."));
  }
}


/*
 * One or two sentences on where this card's health figures come from, and
 * how much to trust them. Empty (and hidden) when there are none.
 */
static void
update_source_note (DriveCard *card)
{
  const UsbReading *usb = &card->usb;
  GString *text = g_string_new (NULL);

  if (!card_has_figures (card)) {
    /* nothing to attribute */
  } else if (usb->applies) {
    g_autofree char *age = NULL;

    if (usb->status == GSM_SMART_DRIVE_ASLEEP) {
      if (usb->last_ok_at > 0) {
        age = age_text (now_seconds () - usb->last_ok_at);
        g_string_append_printf (text,
                                _("This drive is asleep, so the reader left it "
                                  "alone. These are its last good figures, read "
                                  "%s ago."), age);
      } else {
        g_string_append (text,
                         _("This drive is asleep, so the reader left it alone. "
                           "These are its last good figures."));
      }
    } else {
      age = age_text (now_seconds () - usb->generated_at);
      g_string_append_printf (text,
                              _("Health figures from the gsm-smart-reader "
                                "service, read %s ago."), age);
    }

    if (usb->observed) {
      g_string_append_c (text, '\n');
      g_string_append_printf (text,
                              _("This enclosure (USB %s) is not in "
                                "smartmontools' drive database. Its figures "
                                "come through the %s tunnel and pass sanity "
                                "checks, but are unverified."),
                              usb->usb_id != NULL ? usb->usb_id : _("id unknown"),
                              bridge_display_name (usb->bridge));
    }
  } else {
    g_string_append (text, _("Health figures read through udisks."));
  }

  gtk_label_set_text (card->source_note, text->str);
  gtk_widget_set_visible (GTK_WIDGET (card->source_note), text->len > 0);
  g_string_free (text, TRUE);
}


/* ------------------------------------------------------------------ */
/* Formatting the headline                                             */
/* ------------------------------------------------------------------ */

/*
 * How long the recorded write counts for this drive span, in seconds: what
 * the database measures its write rate over, from the same trailing window.
 */
static int64_t
recorded_write_span_seconds (DriveCard *card)
{
  g_autoptr (GArray) history = NULL;
  int64_t first = 0;
  int64_t last = 0;

  history = gsm_health_db_get_history (card->view->db,
                                       gsm_drive_get_id (card->drive),
                                       GSM_HISTORY_RANGE_MONTH);

  for (guint i = 0; i < history->len; i++) {
    const GsmHistoryPoint *p = &g_array_index (history, GsmHistoryPoint, i);

    if (p->bytes_written == 0)
      continue;
    if (first == 0)
      first = p->ts;
    last = p->ts;
  }

  return last - first;
}


/* The "Collecting history" line: how much is recorded and how much the
 * estimate needs. */
static char *
collecting_history_text (DriveCard *card)
{
  int64_t recorded = recorded_write_span_seconds (card);
  g_autofree char *needed = age_text (RATE_NEEDS_SECONDS);

  if (recorded >= RATE_NEEDS_SECONDS)
    return g_strdup_printf (_("Collecting history: the write counts recorded "
                              "so far do not yet give a usable write rate "
                              "(a lifespan estimate needs at least %s of "
                              "them rising)."), needed);

  if (recorded <= 0)
    return g_strdup_printf (_("Collecting history: nothing recorded for this "
                              "drive yet; a lifespan estimate needs at least "
                              "%s of write counts."), needed);

  {
    g_autofree char *have = age_text (recorded);

    return g_strdup_printf (_("Collecting history: %s of write counts "
                              "recorded; a lifespan estimate needs at least "
                              "%s."), have, needed);
  }
}


static char *format_size_plain (uint64_t bytes);


/* "about 1.7 years", "about 8 months": how long, for a sentence. */
static char *
duration_text (double years)
{
  if (years >= 2)
    return g_strdup_printf (_("about %.0f years"), years);
  if (years >= 1)
    return g_strdup_printf (_("about %.1f years"), years);
  if (years * 12 >= 1.5)
    return g_strdup_printf (_("about %.0f months"), years * 12);

  return g_strdup (_("under 2 months"));
}


/*
 * When the maker's published rating is reached at the same write rate. The
 * headline's lifespan comes from the drive's own wear counter; the two can
 * disagree by years (a rating is a warranty line, the counter a wear
 * estimate), so when a rating exists both are said. NULL when the maker
 * publishes none or the rate is not known.
 */
static char *
maker_rating_line (GsmDrive *drive,
                   double    bytes_per_day)
{
  uint64_t rated = gsm_drive_get_maker_rated_tbw (drive);
  uint64_t written = gsm_drive_get_bytes_written (drive);
  g_autofree char *size = NULL;
  g_autofree char *when = NULL;

  if (rated == 0 || bytes_per_day <= 0)
    return NULL;

  size = format_size_plain (rated);

  if (written >= rated)
    return g_strdup_printf (_("The maker's %s rating has already been passed."),
                            size);

  when = duration_text (((double) (rated - written) / bytes_per_day) / 365.25);

  return g_strdup_printf (_("At this rate the maker's %s rating is reached in %s."),
                          size, when);
}


/*
 * The one number a user actually wants. Everything else on the card exists to
 * justify this line, and the line below it always states what it was computed
 * from — a projection with no stated basis is just an oracle.
 */
static void
update_headline (DriveCard *card)
{
  GsmDrive *drive = card->drive;
  double rate = card->write_rate;
  double years;
  g_autofree char *basis = NULL;

  if (card->usb.applies && !card->usb.have_figures) {
    const char *headline = NULL;

    basis = usb_explanation (&card->usb, &headline);
    gtk_label_set_text (card->headline, headline);
    gtk_label_set_text (card->basis, basis);
    return;
  }

  if (!card_has_figures (card)) {
    GsmSmartSource source = gsm_drive_get_smart_source (drive);

    if (source == GSM_SMART_SOURCE_UNSUPPORTED) {
      gtk_label_set_text (card->headline, _("No health data"));
      gtk_label_set_text (card->basis,
                          _("This drive does not report SMART data."));
    } else {
      gtk_label_set_text (card->headline, _("No health data yet"));
      gtk_label_set_text (card->basis,
                          _("The system's disk service (udisks) has not read "
                            "SMART data from this drive."));
    }
    return;
  }

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

    if (card->view->tracking_enabled && card->view->db != NULL)
      basis = collecting_history_text (card);

    gtk_label_set_text (card->basis,
                        basis != NULL
                        ? basis
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

    g_autofree char *rating = maker_rating_line (drive, rate);

    basis = rating != NULL
            ? g_strdup_printf (_("at your recent write rate of %s/day.\n%s"),
                               per_day, rating)
            : g_strdup_printf (_("at your recent write rate of %s/day"), per_day);
    gtk_label_set_text (card->basis, basis);
  }
}


/* g_format_size() with a whole number left whole: "400 TB", not "400.0 TB",
 * as a maker writes a rating. */
static char *
format_size_plain (uint64_t bytes)
{
  char *text = g_format_size (bytes);
  char *unit = text;

  /* The number ends where the digits and the decimal mark do; GLib puts a
   * no-break space, not a plain one, between it and the unit. */
  while (g_ascii_isdigit (*unit) || *unit == '.' || *unit == ',')
    unit++;

  if (unit - text >= 3 && unit[-1] == '0' && (unit[-2] == '.' || unit[-2] == ','))
    memmove (unit - 2, unit, strlen (unit) + 1);

  return text;
}


/* "1.67-1.81 PB": both ends in the unit of the larger, two decimals each,
 * because the range is the point and one decimal would hide it. */
static char *
format_endurance_range (uint64_t low_bytes,
                        uint64_t high_bytes)
{
  static const char *const units[] = { "B", "kB", "MB", "GB", "TB", "PB", "EB" };
  double scale = 1;
  gsize unit = 0;

  while (unit + 1 < G_N_ELEMENTS (units) && (double) high_bytes / scale >= 1000) {
    scale *= 1000;
    unit++;
  }

  return g_strdup_printf ("%.2f-%.2f %s",
                          (double) low_bytes / scale,
                          (double) high_bytes / scale, units[unit]);
}


/* What share of the maker's rating has been written, in whole percent. */
static int
written_share_percent (uint64_t written_bytes,
                       uint64_t rated_bytes)
{
  return (int) (100.0 * (double) written_bytes / (double) rated_bytes + 0.5);
}


/*
 * Two truths about endurance, kept apart and each with its source: what the
 * maker publishes (an exact model and capacity, with the document it came
 * from, or "not published"), and what the drive's own wear counter says.
 * The counter is whole percent, so what it implies is a range.
 */
static void
update_endurance (DriveCard *card)
{
  GsmDrive *drive = card->drive;
  GString *text = g_string_new (NULL);
  int percent = gsm_drive_get_percent_used (drive);
  uint64_t written = gsm_drive_get_bytes_written (drive);
  uint64_t maker = gsm_drive_get_maker_rated_tbw (drive);
  uint64_t hours = gsm_drive_get_power_on_hours (drive);
  uint64_t low = 0, high = 0;
  gboolean implied;

  if (!card_has_figures (card)) {
    gtk_label_set_text (card->endurance, "");
    gtk_widget_set_visible (GTK_WIDGET (card->endurance), FALSE);
    g_string_free (text, TRUE);
    return;
  }

  implied = gsm_drive_get_implied_endurance (drive, &low, &high);

  if (written > 0) {
    g_autofree char *size = g_format_size (written);

    g_string_append_printf (text, _("%s written"), size);
  }

  /* The drive's own hour count: some controllers do not count wall-clock
   * time (570 h on a drive in daily use), so it is never used for a rate. */
  if (hours > 0) {
    if (text->len > 0)
      g_string_append (text, " · ");
    g_string_append_printf (text, _("%" G_GUINT64_FORMAT " h powered on, as "
                                    "counted by the drive"), hours);
  }

  /* The maker's truth. */
  if (text->len > 0)
    g_string_append_c (text, '\n');

  if (maker > 0) {
    g_autofree char *rated = format_size_plain (maker);
    const char *label = gsm_drive_get_maker_source_label (drive);

    g_string_append_printf (text, _("Rated %s written by the maker (%s)"),
                            rated, label != NULL ? label : _("source unnamed"));

    if (written > 0) {
      g_autofree char *size = format_size_plain (written);

      g_string_append_printf (text, _("; %s written is %d %% of it"), size,
                              written_share_percent (written, maker));
    }
  } else {
    g_string_append (text, _("Endurance not published by the maker"));
  }

  /* The drive's truth. */
  if (percent >= 0) {
    g_string_append_c (text, '\n');
    g_string_append_printf (text, _("The drive's own wear counter: %d %% used"),
                            percent);

    if (implied) {
      g_autofree char *range = format_endurance_range (low, high);

      g_string_append_printf (text, _(" (implies %s)"), range);
    } else {
      g_string_append (text, _(" (too little worn to imply an endurance)"));
    }
  }

  if (gsm_drive_get_endurance_source (drive) == GSM_ENDURANCE_USER) {
    g_autofree char *size = format_size_plain (gsm_drive_get_rated_tbw (drive));

    g_string_append_c (text, '\n');
    g_string_append_printf (text, _("Endurance you set: %s"), size);
  }

  /*
   * When the drive's own wear estimate and the maker's rating disagree, say
   * so. Silently picking one would make the number look more certain than it
   * is — on a WD SN740 the two differ by more than threefold.
   */
  if (gsm_drive_endurance_is_conflicted (drive)) {
    gboolean drive_says_less = implied && maker > 0 && high > maker;

    g_string_append_c (text, '\n');
    g_string_append_printf (text,
                            drive_says_less
                            ? _("The drive reports far less wear than the "
                                "maker's rating predicts. %s")
                            : _("The drive reports far more wear than the "
                                "maker's rating predicts. %s"),
                            gsm_drive_get_endurance_source (drive) == GSM_ENDURANCE_USER
                            ? _("Projections follow the figure you set.")
                            : _("Projections follow the drive."));
  }

  gtk_label_set_text (card->endurance, text->str);
  gtk_widget_set_visible (GTK_WIDGET (card->endurance), text->len > 0);
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

  /* A verdict from an earlier refresh is not a warning about now. */
  if (!card_has_figures (card)) {
    adw_banner_set_revealed (card->banner, FALSE);
    return;
  }

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


/*
 * The sampler holds writes in memory and flushes every few minutes, so the
 * figures below can lag. Say when the record was last written; say nothing
 * when it never was, rather than invent a time.
 */
static char *
recorded_up_to_text (GsmHealthView *self)
{
  int64_t flushed = gsm_health_db_get_last_flush (self->db);
  g_autoptr (GDateTime) when = NULL;
  g_autoptr (GDateTime) now = NULL;
  g_autofree char *stamp = NULL;

  if (flushed <= 0)
    return NULL;

  when = g_date_time_new_from_unix_local (flushed);
  now = g_date_time_new_now_local ();
  if (when == NULL || now == NULL)
    return NULL;

  stamp = g_date_time_get_day_of_year (when) == g_date_time_get_day_of_year (now) &&
          g_date_time_get_year (when) == g_date_time_get_year (now)
          ? g_date_time_format (when, "%R")
          : g_date_time_format (when, "%x %R");

  return g_strdup_printf (_("Recorded up to %s."), stamp);
}


/* ------------------------------------------------------------------ */
/* Charts                                                              */
/* ------------------------------------------------------------------ */

/* The longest silence between two readings that still counts as one
 * continuous line, per range: a sampler that runs writes at least hourly,
 * and a laptop asleep overnight is not a gap worth hiding. */
static int64_t
chart_max_gap_seconds (GsmHistoryRange range)
{
  switch (range) {
    case GSM_HISTORY_RANGE_DAY:
      return 3 * 3600;
    case GSM_HISTORY_RANGE_MONTH:
      return 2 * 86400;
    case GSM_HISTORY_RANGE_YEAR:
    default:
      return 3 * 86400;
  }
}


/* ---- the write lists ---- */

/* One row of "what writes to this drive", before it becomes a bar. */
typedef struct {
  char    *name;
  char    *basis;      /* GSM_HEALTH_BASIS_*; unused for charged rows */
  uint64_t wbytes;
  gboolean inferred;   /* charged row: the device is inferred (/proc) */
} WriteRow;

/* The on-behalf rows of one proxy writer. */
typedef struct {
  char      *via;
  uint64_t   wbytes;
  GPtrArray *rows;     /* WriteRow*, grouped by basis */
  gboolean   placed;   /* already listed under a charged-to row */
} ProxyGroup;

/* How many charged-to rows the list shows before folding the rest. */
#define CHARGED_ROWS_SHOWN 12

static void
write_row_free (WriteRow *row)
{
  g_free (row->name);
  g_free (row->basis);
  g_free (row);
}


static void
proxy_group_free (ProxyGroup *group)
{
  g_free (group->via);
  g_ptr_array_unref (group->rows);
  g_free (group);
}


static int
compare_write_rows_by_bytes (gconstpointer a,
                             gconstpointer b)
{
  const WriteRow *x = *(WriteRow *const *) a;
  const WriteRow *y = *(WriteRow *const *) b;

  return (x->wbytes < y->wbytes) - (x->wbytes > y->wbytes);
}


static int
compare_proxy_groups_by_bytes (gconstpointer a,
                               gconstpointer b)
{
  const ProxyGroup *x = *(ProxyGroup *const *) a;
  const ProxyGroup *y = *(ProxyGroup *const *) b;

  return (x->wbytes < y->wbytes) - (x->wbytes > y->wbytes);
}


/*
 * Is this charged-to row the proxy writer `via` itself? The rows come from
 * cgroup names (the coredump handler is "system-systemd-coredump.slice" or
 * its template unit, journald "systemd-journald.service") and `via` from the
 * proxy's own name, so match on the daemon's name rather than the spelling.
 */
static gboolean
row_is_proxy_writer (const char *row_name,
                     const char *via)
{
  if (g_strcmp0 (row_name, via) == 0)
    return TRUE;
  if (strstr (via, "coredump") != NULL)
    return strstr (row_name, "systemd-coredump") != NULL;
  if (strstr (via, "journald") != NULL)
    return strstr (row_name, "systemd-journald") != NULL;

  return FALSE;
}


static gboolean
is_unknown_originator (const char *name)
{
  return g_str_has_prefix (name, "unknown:");
}


/* Within a proxy group: rows of one basis together (so one caption covers
 * them), the rows that cannot say who last, biggest first within each. */
static int
compare_on_behalf_rows (gconstpointer a,
                        gconstpointer b)
{
  const WriteRow *x = *(WriteRow *const *) a;
  const WriteRow *y = *(WriteRow *const *) b;
  gboolean x_unknown = is_unknown_originator (x->name);
  gboolean y_unknown = is_unknown_originator (y->name);
  int by_basis;

  if (x_unknown != y_unknown)
    return x_unknown ? 1 : -1;

  by_basis = g_strcmp0 (x->basis, y->basis);
  if (by_basis != 0)
    return by_basis;

  return compare_write_rows_by_bytes (a, b);
}


/* The basis of an on-behalf row in words; NULL when it has no label. */
static const char *
basis_phrase (const char *basis)
{
  if (g_strcmp0 (basis, GSM_HEALTH_BASIS_COREDUMP_PROPORTIONAL) == 0)
    return _("estimated from core sizes");
  if (g_strcmp0 (basis, GSM_HEALTH_BASIS_COREDUMP_EXACT) == 0)
    return _("measured per crash");
  if (g_strcmp0 (basis, GSM_HEALTH_BASIS_JOURNALD_LINES) == 0)
    return _("estimated from log lines");

  return NULL;
}


static gboolean
basis_is_estimate (const char *basis)
{
  return g_strcmp0 (basis, GSM_HEALTH_BASIS_COREDUMP_EXACT) != 0;
}


/* "systemd-coredump wrote these bytes while storing the crashes of (measured
 * per crash):", the line above a run of on-behalf rows. */
static char *
on_behalf_caption (const char *via,
                   const char *basis)
{
  const char *phrase = basis_phrase (basis);
  const char *what;

  if (g_strcmp0 (via, gsm_proxy_writer_name (GSM_PROXY_COREDUMP)) == 0)
    what = _("systemd-coredump wrote these bytes while storing the crashes of");
  else if (g_strcmp0 (via, gsm_proxy_writer_name (GSM_PROXY_JOURNALD)) == 0)
    what = _("systemd-journald wrote these bytes while logging for");
  else
    what = _("This writer wrote these bytes on behalf of");

  if (phrase == NULL)
    return g_strdup_printf ("%s:", what);

  return g_strdup_printf ("%s (%s):", what, phrase);
}


static GsmChartBar *
new_chart_bar (const char *label,
               double      value,
               gboolean    estimated,
               int         indent,
               gboolean    caption)
{
  GsmChartBar *bar = g_new0 (GsmChartBar, 1);

  bar->label = g_strdup (label);
  bar->value = value;
  bar->estimated = estimated;
  bar->indent = indent;
  bar->caption = caption;

  return bar;
}


/* What the list adds up to and which kinds of row it holds, for the note. */
typedef struct {
  uint64_t charged_total_bytes;
  gboolean any_inferred;
  gboolean any_on_behalf;
  gboolean any_on_behalf_estimate;
  gboolean any_row;
} WriteListSummary;


/* Under a proxy writer's row: its on-behalf rows, a caption before each run
 * of one basis, and for the rows that could not say who, why. */
static void
place_proxy_group (ProxyGroup       *group,
                   GPtrArray        *bars,
                   WriteListSummary *summary)
{
  const char *last_basis = NULL;

  summary->any_on_behalf = TRUE;
  g_ptr_array_sort (group->rows, compare_on_behalf_rows);

  for (guint r = 0; r < group->rows->len; r++) {
    WriteRow *on_behalf = g_ptr_array_index (group->rows, r);
    gboolean unknown = is_unknown_originator (on_behalf->name);
    gboolean estimate = basis_is_estimate (on_behalf->basis);

    if (!unknown && g_strcmp0 (last_basis, on_behalf->basis) != 0) {
      g_autofree char *caption = on_behalf_caption (group->via, on_behalf->basis);

      g_ptr_array_add (bars, new_chart_bar (caption, 0, FALSE, 1, TRUE));
      last_basis = on_behalf->basis;
    }

    summary->any_on_behalf_estimate |= estimate && !unknown;
    g_ptr_array_add (bars, new_chart_bar (on_behalf->name,
                                          (double) on_behalf->wbytes,
                                          estimate, 1, FALSE));

    if (g_strcmp0 (on_behalf->name, GSM_ON_BEHALF_NO_ACCESS) == 0)
      g_ptr_array_add (bars, new_chart_bar (_("The journal could not be "
                                              "read: join the adm or "
                                              "systemd-journal group to "
                                              "see who these bytes were "
                                              "written for."),
                                            0, FALSE, 1, TRUE));
  }
}


/*
 * Split the stored rows into the two lists of ADR 0012: what the kernel
 * charged to each cgroup (exact rows, residues included, summed per name, the
 * biggest first, the tail folded into one line), and under the proxy writers
 * their on-behalf rows. Charged rows add up to the drive's own counter; the
 * on-behalf rows re-divide a proxy's share of it and are never added in.
 */
static GPtrArray *
build_write_bars (GPtrArray        *detail,
                  WriteListSummary *summary)
{
  g_autoptr (GHashTable) charged_by_name =
    g_hash_table_new (g_str_hash, g_str_equal);
  g_autoptr (GPtrArray) charged =
    g_ptr_array_new_with_free_func ((GDestroyNotify) write_row_free);
  g_autoptr (GPtrArray) groups =
    g_ptr_array_new_with_free_func ((GDestroyNotify) proxy_group_free);
  GPtrArray *bars =
    g_ptr_array_new_with_free_func ((GDestroyNotify) gsm_chart_bar_free);
  uint64_t folded_bytes = 0;
  guint folded_count = 0;
  gboolean folded_inferred = FALSE;
  guint shown = 0;

  memset (summary, 0, sizeof *summary);

  for (guint i = 0; i < detail->len; i++) {
    GsmBlameDetail *d = g_ptr_array_index (detail, i);

    if (d->wbytes == 0)
      continue;

    if (d->via != NULL && d->via[0] != '\0') {
      ProxyGroup *group = NULL;
      WriteRow *row;

      for (guint g = 0; g < groups->len; g++) {
        ProxyGroup *candidate = g_ptr_array_index (groups, g);

        if (g_strcmp0 (candidate->via, d->via) == 0)
          group = candidate;
      }

      if (group == NULL) {
        group = g_new0 (ProxyGroup, 1);
        group->via = g_strdup (d->via);
        group->rows = g_ptr_array_new_with_free_func ((GDestroyNotify) write_row_free);
        g_ptr_array_add (groups, group);
      }

      row = g_new0 (WriteRow, 1);
      row->name = g_strdup (d->name);
      row->basis = g_strdup (d->basis);
      row->wbytes = d->wbytes;
      g_ptr_array_add (group->rows, row);
      group->wbytes += d->wbytes;
      continue;
    }

    {
      WriteRow *row = g_hash_table_lookup (charged_by_name, d->name);

      if (row == NULL) {
        row = g_new0 (WriteRow, 1);
        row->name = g_strdup (d->name);
        g_ptr_array_add (charged, row);
        g_hash_table_insert (charged_by_name, row->name, row);
      }
      row->wbytes += d->wbytes;
      row->inferred |= g_strcmp0 (d->basis, GSM_HEALTH_BASIS_PROCFS) == 0;
      summary->charged_total_bytes += d->wbytes;
    }
  }

  g_ptr_array_sort (charged, compare_write_rows_by_bytes);
  g_ptr_array_sort (groups, compare_proxy_groups_by_bytes);

  for (guint i = 0; i < charged->len; i++) {
    WriteRow *row = g_ptr_array_index (charged, i);
    gboolean is_proxy = FALSE;

    for (guint g = 0; g < groups->len; g++) {
      ProxyGroup *group = g_ptr_array_index (groups, g);

      is_proxy |= !group->placed && row_is_proxy_writer (row->name, group->via);
    }

    /* A proxy's row stays, whatever its size: its on-behalf rows hang from it. */
    if (shown >= CHARGED_ROWS_SHOWN && !is_proxy) {
      folded_bytes += row->wbytes;
      folded_count++;
      folded_inferred |= row->inferred;
      continue;
    }

    shown++;
    summary->any_row = TRUE;
    summary->any_inferred |= row->inferred;
    g_ptr_array_add (bars, new_chart_bar (row->name, (double) row->wbytes,
                                          row->inferred, 0, FALSE));

    for (guint g = 0; g < groups->len; g++) {
      ProxyGroup *group = g_ptr_array_index (groups, g);

      if (group->placed || !row_is_proxy_writer (row->name, group->via))
        continue;

      group->placed = TRUE;
      place_proxy_group (group, bars, summary);

    }
  }

  /* A proxy writer with no row of its own above (it wrote too little to be
   * charged on this drive) still gets its originators listed. */
  for (guint g = 0; g < groups->len; g++) {
    ProxyGroup *group = g_ptr_array_index (groups, g);

    if (group->placed)
      continue;

    {
      g_autofree char *heading = g_strdup_printf (_("Written by %s:"), group->via);

      g_ptr_array_add (bars, new_chart_bar (heading, 0, FALSE, 0, TRUE));
    }
    place_proxy_group (group, bars, summary);
  }

  if (folded_count > 0) {
    g_autofree char *label = g_strdup_printf (ngettext ("%u smaller writer",
                                                        "%u smaller writers",
                                                        folded_count),
                                              folded_count);

    summary->any_row = TRUE;
    summary->any_inferred |= folded_inferred;
    g_ptr_array_add (bars, new_chart_bar (label, (double) folded_bytes,
                                          folded_inferred, 0, FALSE));
  }

  return bars;
}


/* The note under the list: what the plain rows add up to, and how to read the
 * indented and hatched ones. */
static char *
write_list_note (const WriteListSummary *summary)
{
  GString *note = g_string_new (NULL);
  g_autofree char *total = g_format_size (summary->charged_total_bytes);

  if (!summary->any_inferred)
    g_string_append_printf (note, _("Rows that are not indented add up to %s, "
                                    "the bytes the kernel counted as written "
                                    "to this drive over this range."), total);
  else
    g_string_append_printf (note, _("Rows that are not indented add up to %s. "
                                    "Hatched ones are inferred: the system "
                                    "reports how much an app wrote, but not "
                                    "to which drive."), total);

  if (summary->any_on_behalf) {
    g_string_append (note, " ");
    g_string_append (note, summary->any_on_behalf_estimate
                     ? _("Indented rows split a proxy writer's bytes among "
                         "those it wrote for; they are not added in again, "
                         "and the hatched ones are estimates.")
                     : _("Indented rows split a proxy writer's bytes among "
                         "those it wrote for; they are not added in again."));
  }

  return g_string_free (note, FALSE);
}


static void
update_charts (DriveCard *card)
{
  GsmHealthView *self = card->view;
  const char *id = gsm_drive_get_id (card->drive);
  g_autoptr (GArray) history = NULL;
  g_autoptr (GArray) wear = g_array_new (FALSE, FALSE, sizeof (GsmChartPoint));
  g_autoptr (GPtrArray) detail = NULL;
  g_autoptr (GPtrArray) bars = NULL;
  WriteListSummary summary;
  int64_t max_gap = chart_max_gap_seconds (self->range);
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
  gsm_history_chart_set_max_gap (GSM_HISTORY_CHART (card->wear_chart), max_gap);
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

  /*
   * Project forward to end of life, if we can say anything about the rate,
   * and only from a reading that is part of a continuous run: the line starts
   * at the last point, so if that point stands alone (the previous one is
   * months back) the dashed stroke would hang from a single reading and
   * suggest a trend the history does not show.
   */
  years = gsm_drive_project_years_left (card->drive, card->write_rate);

  if (years > 0 && years < 100 && wear->len >= 2 &&
      g_array_index (wear, GsmChartPoint, wear->len - 1).ts -
      g_array_index (wear, GsmChartPoint, wear->len - 2).ts <= max_gap) {
    int64_t now = g_get_real_time () / G_USEC_PER_SEC;
    int64_t end = now + (int64_t) (years * 365.25 * 86400);
    g_autofree char *when = duration_text (years);
    g_autofree char *label = g_strdup_printf (_("Dashed: projected at the recent "
                                                "write rate, 100%% in %s"), when);

    gsm_history_chart_set_projection (GSM_HISTORY_CHART (card->wear_chart),
                                      end, 100);
    gsm_history_chart_set_projection_label (GSM_HISTORY_CHART (card->wear_chart),
                                            label);
  } else {
    gsm_history_chart_set_projection (GSM_HISTORY_CHART (card->wear_chart), 0, 0);
    gsm_history_chart_set_projection_label (GSM_HISTORY_CHART (card->wear_chart),
                                            NULL);
  }

  /* Blame */
  detail = gsm_health_db_get_blame_detail (self->db, id, self->range);
  bars = build_write_bars (detail, &summary);

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

  {
    g_autofree char *base = NULL;
    g_autofree char *fresh = recorded_up_to_text (self);

    if (!summary.any_row)
      base = g_strdup (self->tracking_enabled
                       ? _("No writes recorded yet.")
                       : _("Turn on wear tracking to find out what writes to "
                           "this drive."));
    else
      base = write_list_note (&summary);

    if (fresh != NULL) {
      g_autofree char *note = g_strdup_printf ("%s %s", base, fresh);

      gtk_label_set_text (card->blame_note, note);
    } else {
      gtk_label_set_text (card->blame_note, base);
    }
  }
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
  GtkWidget *source_note;
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
  gtk_label_set_xalign (GTK_LABEL (basis), 0);
  /* So the command that turns the SMART service on can be copied. */
  gtk_label_set_selectable (GTK_LABEL (basis), TRUE);
  card->basis = GTK_LABEL (basis);
  gtk_box_append (GTK_BOX (box), basis);

  source_note = gtk_label_new (NULL);
  gtk_widget_add_css_class (source_note, "dim-label");
  gtk_widget_add_css_class (source_note, "caption");
  gtk_widget_set_halign (source_note, GTK_ALIGN_START);
  gtk_label_set_wrap (GTK_LABEL (source_note), TRUE);
  gtk_label_set_xalign (GTK_LABEL (source_note), 0);
  gtk_widget_set_visible (source_note, FALSE);
  card->source_note = GTK_LABEL (source_note);
  gtk_box_append (GTK_BOX (box), source_note);

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
/* Everything one refresh learned, handed from the worker to the main loop. */
typedef struct {
  GPtrArray    *infos;        /* GsmDriveInfo*, from udisks */
  GsmSmartFile *smart_file;   /* NULL unless a USB disk needed it */
  ServiceState  service;      /* only asked when the snapshot is missing */
} Snapshot;


static void
snapshot_free (Snapshot *snapshot)
{
  if (snapshot == NULL)
    return;

  g_clear_pointer (&snapshot->infos, g_ptr_array_unref);
  g_clear_pointer (&snapshot->smart_file, gsm_smart_file_free);
  g_free (snapshot);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Snapshot, snapshot_free)


static gboolean
info_is_usb (const GsmDriveInfo *info)
{
  return g_strcmp0 (info->connection, "usb") == 0;
}


/* A USB disk udisks gave no SMART for: the only kind the reader's snapshot
 * is for. */
static gboolean
info_needs_smart_file (const GsmDriveInfo *info)
{
  return info_is_usb (info) && info->smart_source != GSM_SMART_SOURCE_UDISKS;
}


/*
 * Where the reader's snapshot lives. A developer build can point it at a
 * scratch file to show each state; a release build cannot be redirected.
 */
static const char *
smart_file_path (void)
{
#ifdef HAVE_DEV_IPC
  const char *override = g_getenv ("GSM_SMART_FILE");

  if (override != NULL && *override != '\0')
    return override;
#endif

  return GSM_SMART_FILE_DEFAULT_PATH;
}


/*
 * Is the reader's timer installed and switched on? systemd's unit-file state
 * answers without privilege. Anything but a clear answer is "unknown": the
 * card then says it could not tell, rather than guessing "not installed".
 */
static ServiceState
query_service_state (GDBusConnection *bus)
{
  g_autoptr (GVariant) reply = NULL;
  g_autoptr (GError) error = NULL;
  const char *state = NULL;

  reply = g_dbus_connection_call_sync (bus,
                                       "org.freedesktop.systemd1",
                                       "/org/freedesktop/systemd1",
                                       "org.freedesktop.systemd1.Manager",
                                       "GetUnitFileState",
                                       g_variant_new ("(s)", SMART_READER_TIMER),
                                       G_VARIANT_TYPE ("(s)"),
                                       G_DBUS_CALL_FLAGS_NONE, 2000,
                                       NULL, &error);
  if (reply == NULL) {
    g_autofree char *remote = g_dbus_error_get_remote_error (error);

    /* systemd answers FileNotFound for a unit file that does not exist. */
    if (g_strcmp0 (remote, "org.freedesktop.DBus.Error.FileNotFound") == 0 ||
        g_strcmp0 (remote, "org.freedesktop.systemd1.NoSuchUnit") == 0)
      return SERVICE_NOT_INSTALLED;

    return SERVICE_UNKNOWN;
  }

  g_variant_get (reply, "(&s)", &state);

  if (g_strcmp0 (state, "enabled") == 0 || g_strcmp0 (state, "enabled-runtime") == 0 ||
      g_strcmp0 (state, "static") == 0 || g_strcmp0 (state, "generated") == 0 ||
      g_strcmp0 (state, "transient") == 0 || g_strcmp0 (state, "alias") == 0)
    return SERVICE_TURNED_ON;

  if (g_strcmp0 (state, "disabled") == 0 || g_strcmp0 (state, "masked") == 0 ||
      g_strcmp0 (state, "masked-runtime") == 0 || g_strcmp0 (state, "linked") == 0 ||
      g_strcmp0 (state, "linked-runtime") == 0)
    return SERVICE_TURNED_OFF;

  return SERVICE_UNKNOWN;
}


/*
 * Reading SMART means a GetManagedObjects round trip plus a SmartGetAttributes
 * call per drive, all synchronous. Doing that on the main loop would freeze the
 * entire window — including the Processes tab — for as long as udisksd takes to
 * answer, which on a drive that has spun down or a wedged USB bridge can be
 * seconds. So it runs on a worker thread and the widgets are only touched once
 * it comes back. The reader's snapshot is one small file read, taken here too.
 */
static void
collect_in_thread (GTask        *task,
                   gpointer      source,
                   gpointer      task_data,
                   GCancellable *cancellable)
{
  GDBusConnection *bus = task_data;
  GError *error = NULL;
  Snapshot *snapshot;
  GPtrArray *infos;
  gboolean wants_file = FALSE;

  (void) source;
  (void) cancellable;

  infos = gsm_udisks_collect (bus, &error);

  if (infos == NULL) {
    g_task_return_error (task, error);
    return;
  }

  snapshot = g_new0 (Snapshot, 1);
  snapshot->infos = infos;

  for (guint i = 0; i < infos->len && !wants_file; i++)
    wants_file = info_needs_smart_file (g_ptr_array_index (infos, i));

  if (wants_file) {
    snapshot->smart_file = gsm_smart_file_load (smart_file_path (),
                                                now_seconds (),
                                                GSM_SMART_FILE_MAX_AGE_SECONDS);

    if (snapshot->smart_file->state == GSM_SMART_FILE_MISSING)
      snapshot->service = query_service_state (bus);
  }

  g_task_return_pointer (task, snapshot, (GDestroyNotify) snapshot_free);
}


/*
 * What the reader's snapshot says about this USB disk, and its figures into
 * `info` when it has fresh ones. An asleep disk's snapshot entry keeps the
 * last good figures; they are taken as that, with their age shown.
 */
static void
usb_reading_update (DriveCard          *card,
                    GsmDriveInfo       *info,
                    const Snapshot     *snapshot)
{
  UsbReading *usb = &card->usb;
  const GsmSmartFile *file = snapshot->smart_file;
  const GsmSmartFileDrive *entry;

  usb_reading_clear (usb);

  if (file == NULL || !info_needs_smart_file (info))
    return;

  usb->applies = TRUE;
  usb->service = snapshot->service;
  usb->file_state = file->state;
  usb->generated_at = file->generated_at;

  entry = gsm_smart_file_find (file, info->serial);
  if (entry == NULL)
    return;

  usb->listed = TRUE;
  usb->status = entry->status;
  usb->usb_id = g_strdup (entry->usb_id);
  usb->bridge = g_strdup (entry->bridge);
  usb->observed = g_strcmp0 (entry->basis, "observed") == 0;
  usb->last_ok_at = entry->last_ok_generated_at;

  if (entry->status == GSM_SMART_DRIVE_OK) {
    usb->have_figures = gsm_smart_file_fill_info (file, entry, info);
  } else if (entry->status == GSM_SMART_DRIVE_ASLEEP) {
    /* fill_info takes figures only from an ok entry; an asleep entry's
     * figures are the last good ones, so ask it about a copy that says ok. */
    GsmSmartFileDrive as_last_good = *entry;

    as_last_good.status = GSM_SMART_DRIVE_OK;
    usb->have_figures = gsm_smart_file_fill_info (file, &as_last_good, info);
  }
}


/* The local calendar day of a timestamp, as a comparable number. */
static guint
local_day_key (int64_t ts)
{
  g_autoptr (GDateTime) when = g_date_time_new_from_unix_local (ts);

  return when != NULL
         ? (guint) (g_date_time_get_year (when) * 1000 + g_date_time_get_day_of_year (when))
         : 0;
}


/*
 * "Recorded since <date>; N days with data": the first record, and how many
 * calendar days actually hold one. The span alone ("107 days of history")
 * would call a single leftover row from months ago a long history.
 */
static void
update_history_row (GsmHealthView *self)
{
  uint64_t bytes = gsm_health_db_get_size_bytes (self->db);
  int64_t oldest = gsm_health_db_get_oldest_sample (self->db);
  g_autofree char *size = g_format_size (bytes);
  g_autofree char *subtitle = NULL;

  if (oldest > 0) {
    g_autoptr (GHashTable) days = g_hash_table_new (g_direct_hash, g_direct_equal);
    g_autoptr (GDateTime) since = g_date_time_new_from_unix_local (oldest);
    g_autofree char *since_text = since != NULL ? g_date_time_format (since, "%x") : NULL;
    GHashTableIter iter;
    gpointer id;
    guint with_data;

    g_hash_table_iter_init (&iter, self->cards);
    while (g_hash_table_iter_next (&iter, &id, NULL)) {
      g_autoptr (GArray) history =
        gsm_health_db_get_history (self->db, id, GSM_HISTORY_RANGE_YEAR);

      for (guint i = 0; i < history->len; i++)
        g_hash_table_add (days,
                          GUINT_TO_POINTER (local_day_key (
                            g_array_index (history, GsmHistoryPoint, i).ts)));
    }
    with_data = g_hash_table_size (days);

    subtitle = g_strdup_printf (
      ngettext ("Recorded since %s; %u day with data · %s",
                "Recorded since %s; %u days with data · %s", with_data),
      since_text != NULL ? since_text : "?", with_data, size);
  } else {
    subtitle = g_strdup_printf (_("No history yet · %s"), size);
  }

  adw_action_row_set_subtitle (self->history_row, subtitle);
}


/*
 * Apply a completed snapshot to the cards. Main thread only — it touches
 * widgets.
 */
static void
apply_drives (GsmHealthView  *self,
              const Snapshot *snapshot)
{
  GPtrArray *infos = snapshot->infos;
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

    /* A USB disk udisks cannot read gets its figures, or the reason it has
     * none, from the system SMART reader's snapshot. */
    usb_reading_update (card, info, snapshot);

    /* The merge rules in GsmDrive refuse to let a thinner reading overwrite
     * a richer one, so a source that gave less never downgrades a card. */
    gsm_drive_apply_info (card->drive, info);
    gsm_drive_resolve_endurance (
      card->drive, gsm_health_db_get_user_tbw (self->db, info->serial));

    /* Both the headline and the chart's projection are built from this, so
     * measure it once rather than running the same query twice. */
    card->write_rate = gsm_health_db_get_write_rate (self->db, info->serial,
                                                 RATE_WINDOW_DAYS);

    update_card_title (card);
    update_headline (card);
    update_source_note (card);
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

  update_history_row (self);
}


static void
refresh_finished (GObject      *source,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  GsmHealthView *self = GSM_HEALTH_VIEW (source);
  g_autoptr (Snapshot) snapshot = NULL;
  g_autoptr (GError) error = NULL;

  (void) user_data;

  self->refreshing = FALSE;

  snapshot = g_task_propagate_pointer (G_TASK (result), &error);

  if (snapshot == NULL) {
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning ("health: %s", error->message);
    return;
  }

  /* The GTask kept us alive, but the view may have been disposed while the
   * worker was in flight — in which case the widgets and the database are
   * already gone and there is nothing to update. */
  if (self->db == NULL)
    return;

  apply_drives (self, snapshot);
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

  if (gsm_dev_sandbox_enabled ()) {
    adw_banner_set_title (self->io_accounting_banner, gsm_dev_sandbox_reason ());
    return;
  }

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
