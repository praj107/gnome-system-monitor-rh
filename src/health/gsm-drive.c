/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <glib/gi18n.h>

#include "gsm-drive.h"

struct _GsmDrive {
  GObject parent_instance;

  char *id;
  char *model;
  char *serial;
  char *firmware;
  char *device;
  char *connection;

  gboolean present;
  gboolean removable;
  gboolean rotational;
  int64_t last_seen;

  guint major;
  guint minor;

  uint64_t size;
  uint64_t bytes_written;
  uint64_t bytes_read;
  uint64_t power_on_hours;
  uint64_t power_cycles;
  uint64_t unsafe_shutdowns;
  uint64_t media_errors;
  uint64_t reallocated;

  int temperature;
  int percent_used;
  int spare_percent;
  int spare_threshold;

  gboolean smart_failing;
  gboolean wear_unavailable;
  GsmSmartSource smart_source;

  uint64_t rated_tbw;
  GsmEnduranceSource endurance_source;
};

G_DEFINE_FINAL_TYPE (GsmDrive, gsm_drive, G_TYPE_OBJECT)

enum {
  PROP_0,
  PROP_ID,
  PROP_MODEL,
  PROP_DEVICE,
  PROP_PRESENT,
  PROP_HEALTH,
  PROP_PERCENT_USED,
  PROP_TEMPERATURE,
  PROP_BYTES_WRITTEN,
  PROP_SMART_SOURCE,
  N_PROPS
};
static GParamSpec *properties[N_PROPS];


static void
gsm_drive_finalize (GObject *object)
{
  GsmDrive *self = GSM_DRIVE (object);

  g_clear_pointer (&self->id, g_free);
  g_clear_pointer (&self->model, g_free);
  g_clear_pointer (&self->serial, g_free);
  g_clear_pointer (&self->firmware, g_free);
  g_clear_pointer (&self->device, g_free);
  g_clear_pointer (&self->connection, g_free);

  G_OBJECT_CLASS (gsm_drive_parent_class)->finalize (object);
}


static void
gsm_drive_get_property (GObject    *object,
                        guint       prop_id,
                        GValue     *value,
                        GParamSpec *pspec)
{
  GsmDrive *self = GSM_DRIVE (object);

  switch (prop_id) {
    case PROP_ID:
      g_value_set_string (value, self->id);
      break;
    case PROP_MODEL:
      g_value_set_string (value, self->model);
      break;
    case PROP_DEVICE:
      g_value_set_string (value, self->device);
      break;
    case PROP_PRESENT:
      g_value_set_boolean (value, self->present);
      break;
    case PROP_HEALTH:
      g_value_set_int (value, gsm_drive_get_health (self));
      break;
    case PROP_PERCENT_USED:
      g_value_set_int (value, self->percent_used);
      break;
    case PROP_TEMPERATURE:
      g_value_set_int (value, self->temperature);
      break;
    case PROP_BYTES_WRITTEN:
      g_value_set_uint64 (value, self->bytes_written);
      break;
    case PROP_SMART_SOURCE:
      g_value_set_int (value, self->smart_source);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
  }
}


static void
gsm_drive_class_init (GsmDriveClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gsm_drive_finalize;
  object_class->get_property = gsm_drive_get_property;

  properties[PROP_ID] =
    g_param_spec_string ("id", NULL, NULL, NULL, G_PARAM_READABLE);
  properties[PROP_MODEL] =
    g_param_spec_string ("model", NULL, NULL, NULL, G_PARAM_READABLE);
  properties[PROP_DEVICE] =
    g_param_spec_string ("device", NULL, NULL, NULL, G_PARAM_READABLE);
  properties[PROP_PRESENT] =
    g_param_spec_boolean ("present", NULL, NULL, FALSE, G_PARAM_READABLE);
  properties[PROP_HEALTH] =
    g_param_spec_int ("health", NULL, NULL, 0, 3, GSM_DRIVE_HEALTH_UNKNOWN,
                      G_PARAM_READABLE);
  properties[PROP_PERCENT_USED] =
    g_param_spec_int ("percent-used", NULL, NULL, -1, 255, -1, G_PARAM_READABLE);
  properties[PROP_TEMPERATURE] =
    g_param_spec_int ("temperature", NULL, NULL, -1, 200, -1, G_PARAM_READABLE);
  properties[PROP_BYTES_WRITTEN] =
    g_param_spec_uint64 ("bytes-written", NULL, NULL, 0, G_MAXUINT64, 0,
                         G_PARAM_READABLE);
  properties[PROP_SMART_SOURCE] =
    g_param_spec_int ("smart-source", NULL, NULL, 0, 4, GSM_SMART_SOURCE_NONE,
                      G_PARAM_READABLE);

  g_object_class_install_properties (object_class, N_PROPS, properties);
}


static void
gsm_drive_init (GsmDrive *self)
{
  self->temperature = -1;
  self->percent_used = -1;
  self->spare_percent = -1;
  self->spare_threshold = -1;
  self->smart_source = GSM_SMART_SOURCE_NONE;
  self->endurance_source = GSM_ENDURANCE_UNKNOWN;
}


GsmDrive *
gsm_drive_new (const char *id)
{
  GsmDrive *self = g_object_new (GSM_TYPE_DRIVE, NULL);

  self->id = g_strdup (id);

  return self;
}


void
gsm_drive_info_clear (GsmDriveInfo *info)
{
  g_return_if_fail (info != NULL);

  g_clear_pointer (&info->model, g_free);
  g_clear_pointer (&info->serial, g_free);
  g_clear_pointer (&info->firmware, g_free);
  g_clear_pointer (&info->device, g_free);
  g_clear_pointer (&info->connection, g_free);
}


/*
 * The privileged helper sees more than udisks does, so once a drive has been
 * unlocked we must not let the next unprivileged poll quietly downgrade its
 * numbers back to the thinner set. Ranking the sources keeps that from
 * happening without the collectors having to coordinate.
 */
static int
source_rank (GsmSmartSource source)
{
  switch (source) {
    case GSM_SMART_SOURCE_HELPER:      return 3;
    case GSM_SMART_SOURCE_UDISKS:      return 2;
    case GSM_SMART_SOURCE_LOCKED:      return 1;
    case GSM_SMART_SOURCE_UNSUPPORTED: return 1;
    case GSM_SMART_SOURCE_NONE:
    default:                           return 0;
  }
}


void
gsm_drive_apply_info (GsmDrive           *self,
                      const GsmDriveInfo *info)
{
  gboolean health_may_change = FALSE;

  g_return_if_fail (GSM_IS_DRIVE (self));
  g_return_if_fail (info != NULL);

  /* Identity and topology always come from the most recent enumeration —
   * a drive really can be replugged onto a different node. */
  if (info->model && *info->model)
    g_set_str (&self->model, info->model);
  if (info->serial && *info->serial)
    g_set_str (&self->serial, info->serial);
  if (info->firmware && *info->firmware)
    g_set_str (&self->firmware, info->firmware);
  if (info->device && *info->device)
    g_set_str (&self->device, info->device);
  if (info->connection && *info->connection)
    g_set_str (&self->connection, info->connection);

  self->removable = info->removable;
  self->rotational = info->rotational;

  if (info->major || info->minor) {
    self->major = info->major;
    self->minor = info->minor;
  }
  if (info->size)
    self->size = info->size;

  if (info->present != self->present) {
    self->present = info->present;
    g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_PRESENT]);
  }

  if (self->present)
    self->last_seen = g_get_real_time () / G_USEC_PER_SEC;

  /* Health numbers: only accept from a source at least as good as the one we
   * already have. */
  if (source_rank (info->smart_source) < source_rank (self->smart_source))
    return;

  if (info->smart_source != self->smart_source) {
    self->smart_source = info->smart_source;
    g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_SMART_SOURCE]);
    health_may_change = TRUE;
  }

  /*
   * These are monotonic lifetime counters. A drive's written-bytes figure
   * going *down* means we misread it, not that the drive un-wrote data, so
   * refuse to record the regression — it would corrupt every delta and every
   * projection computed from the history.
   */
  if (info->bytes_written > self->bytes_written) {
    self->bytes_written = info->bytes_written;
    g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_BYTES_WRITTEN]);
  }
  if (info->bytes_read > self->bytes_read)
    self->bytes_read = info->bytes_read;
  if (info->power_on_hours > self->power_on_hours)
    self->power_on_hours = info->power_on_hours;
  if (info->power_cycles > self->power_cycles)
    self->power_cycles = info->power_cycles;
  if (info->unsafe_shutdowns > self->unsafe_shutdowns)
    self->unsafe_shutdowns = info->unsafe_shutdowns;

  /* Error counters may legitimately be re-read as equal, but a *drop* is
   * again a misread. Keep the high-water mark. */
  if (info->media_errors > self->media_errors) {
    self->media_errors = info->media_errors;
    health_may_change = TRUE;
  }
  if (info->reallocated > self->reallocated) {
    self->reallocated = info->reallocated;
    health_may_change = TRUE;
  }

  if (info->temperature >= 0 && info->temperature != self->temperature) {
    self->temperature = info->temperature;
    g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_TEMPERATURE]);
  }

  if (info->percent_used >= 0 && info->percent_used != self->percent_used) {
    self->percent_used = info->percent_used;
    g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_PERCENT_USED]);
    health_may_change = TRUE;
  }

  if (info->spare_percent >= 0) {
    self->spare_percent = info->spare_percent;
    health_may_change = TRUE;
  }
  if (info->spare_threshold >= 0)
    self->spare_threshold = info->spare_threshold;

  if (info->smart_failing != self->smart_failing) {
    self->smart_failing = info->smart_failing;
    health_may_change = TRUE;
  }

  self->wear_unavailable = info->wear_unavailable;

  if (health_may_change)
    g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_HEALTH]);
}


void
gsm_drive_set_present (GsmDrive *self,
                       gboolean  present)
{
  g_return_if_fail (GSM_IS_DRIVE (self));

  if (self->present == present)
    return;

  self->present = present;
  if (present)
    self->last_seen = g_get_real_time () / G_USEC_PER_SEC;

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_PRESENT]);
}


void
gsm_drive_set_last_seen (GsmDrive *self,
                         int64_t   when)
{
  g_return_if_fail (GSM_IS_DRIVE (self));

  self->last_seen = when;
}


void
gsm_drive_set_endurance (GsmDrive           *self,
                         uint64_t            rated_tbw,
                         GsmEnduranceSource  source)
{
  g_return_if_fail (GSM_IS_DRIVE (self));

  self->rated_tbw = rated_tbw;
  self->endurance_source = source;
}


/* ------------------------------------------------------------------ */
/* Health verdict                                                      */
/* ------------------------------------------------------------------ */

GsmDriveHealth
gsm_drive_get_health (GsmDrive *self)
{
  g_return_val_if_fail (GSM_IS_DRIVE (self), GSM_DRIVE_HEALTH_UNKNOWN);

  if (self->smart_source == GSM_SMART_SOURCE_NONE ||
      self->smart_source == GSM_SMART_SOURCE_LOCKED ||
      self->smart_source == GSM_SMART_SOURCE_UNSUPPORTED)
    return GSM_DRIVE_HEALTH_UNKNOWN;

  /* The drive's own verdict outranks anything we infer. */
  if (self->smart_failing)
    return GSM_DRIVE_HEALTH_FAILING;

  /* Spare blocks below the manufacturer's own threshold is the NVMe spec's
   * definition of a drive in trouble. */
  if (self->spare_percent >= 0 && self->spare_threshold >= 0 &&
      self->spare_percent <= self->spare_threshold)
    return GSM_DRIVE_HEALTH_FAILING;

  if (self->percent_used >= 100)
    return GSM_DRIVE_HEALTH_FAILING;

  /* Reallocated sectors are never normal on a healthy drive: a spinning disk
   * that has started remapping is usually on its way out, and an SSD that
   * reports media errors has already lost data it could not recover. */
  if (self->media_errors > 0 || self->reallocated > 0)
    return GSM_DRIVE_HEALTH_ATTENTION;

  if (self->percent_used >= 90)
    return GSM_DRIVE_HEALTH_ATTENTION;

  return GSM_DRIVE_HEALTH_OK;
}


/*
 * Consider the drive's own wear estimate and the datasheet rating to be
 * telling different stories when the endurance they imply differs by more
 * than 2x. Below ~5% wear the controller's figure is too coarsely quantised
 * to draw any conclusion from, so we say nothing rather than cry wolf.
 */
gboolean
gsm_drive_endurance_is_conflicted (GsmDrive *self)
{
  double implied, rated, ratio;

  g_return_val_if_fail (GSM_IS_DRIVE (self), FALSE);

  if (self->percent_used < 5 || self->bytes_written == 0 || self->rated_tbw == 0)
    return FALSE;

  implied = (double) self->bytes_written / ((double) self->percent_used / 100.0);
  rated = (double) self->rated_tbw;

  ratio = implied > rated ? implied / rated : rated / implied;

  return ratio >= 2.0;
}


/* ------------------------------------------------------------------ */
/* Trivial accessors                                                   */
/* ------------------------------------------------------------------ */

#define GSM_DRIVE_GETTER(type, name, field, fallback)  \
  type                                                 \
  gsm_drive_get_ ## name (GsmDrive *self)              \
  {                                                    \
    g_return_val_if_fail (GSM_IS_DRIVE (self), fallback); \
    return self->field;                                \
  }

GSM_DRIVE_GETTER (const char *, id, id, NULL)
GSM_DRIVE_GETTER (const char *, model, model, NULL)
GSM_DRIVE_GETTER (const char *, device, device, NULL)
GSM_DRIVE_GETTER (const char *, connection, connection, NULL)
GSM_DRIVE_GETTER (gboolean, present, present, FALSE)
GSM_DRIVE_GETTER (gboolean, wear_unavailable, wear_unavailable, FALSE)
GSM_DRIVE_GETTER (gboolean, removable, removable, FALSE)
GSM_DRIVE_GETTER (gboolean, rotational, rotational, FALSE)
GSM_DRIVE_GETTER (int64_t, last_seen, last_seen, 0)
GSM_DRIVE_GETTER (guint, major, major, 0)
GSM_DRIVE_GETTER (guint, minor, minor, 0)
GSM_DRIVE_GETTER (GsmSmartSource, smart_source, smart_source, GSM_SMART_SOURCE_NONE)
GSM_DRIVE_GETTER (uint64_t, size, size, 0)
GSM_DRIVE_GETTER (uint64_t, bytes_written, bytes_written, 0)
GSM_DRIVE_GETTER (uint64_t, bytes_read, bytes_read, 0)
GSM_DRIVE_GETTER (uint64_t, power_on_hours, power_on_hours, 0)
GSM_DRIVE_GETTER (uint64_t, power_cycles, power_cycles, 0)
GSM_DRIVE_GETTER (uint64_t, unsafe_shutdowns, unsafe_shutdowns, 0)
GSM_DRIVE_GETTER (uint64_t, media_errors, media_errors, 0)
GSM_DRIVE_GETTER (uint64_t, reallocated, reallocated, 0)
GSM_DRIVE_GETTER (int, temperature, temperature, -1)
GSM_DRIVE_GETTER (int, percent_used, percent_used, -1)
GSM_DRIVE_GETTER (int, spare_percent, spare_percent, -1)
GSM_DRIVE_GETTER (uint64_t, rated_tbw, rated_tbw, 0)
GSM_DRIVE_GETTER (GsmEnduranceSource, endurance_source, endurance_source,
                  GSM_ENDURANCE_UNKNOWN)
