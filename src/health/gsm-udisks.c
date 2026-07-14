/*
 * udisks2 client — the unprivileged half of SMART collection.
 *
 * udisks2 already reads SMART as root and republishes it on the system bus,
 * so for NVMe and most internal SATA drives we get full health data with no
 * privilege prompt at all. Drives it cannot reach (USB bridges, SAS) are
 * reported as LOCKED and picked up by the privileged helper on demand.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <sys/sysmacros.h>

#include "gsm-udisks.h"

#define UDISKS_NAME       "org.freedesktop.UDisks2"
#define UDISKS_PATH       "/org/freedesktop/UDisks2"
#define IFACE_DRIVE       "org.freedesktop.UDisks2.Drive"
#define IFACE_ATA         "org.freedesktop.UDisks2.Drive.Ata"
#define IFACE_NVME        "org.freedesktop.UDisks2.NVMe.Controller"
#define IFACE_BLOCK       "org.freedesktop.UDisks2.Block"
#define IFACE_PARTITION   "org.freedesktop.UDisks2.Partition"

/* NVMe and ATA both report temperature in Kelvin (ATA via millikelvin). */
#define KELVIN_OFFSET     273

void
gsm_drive_info_free (GsmDriveInfo *info)
{
  if (info == NULL)
    return;

  gsm_drive_info_clear (info);
  g_free (info);
}


static char *
dup_string_prop (GVariant   *props,
                 const char *key)
{
  g_autoptr (GVariant) v = g_variant_lookup_value (props, key, G_VARIANT_TYPE_STRING);

  if (v == NULL)
    return NULL;

  return g_variant_dup_string (v, NULL);
}


static uint64_t
uint64_prop (GVariant   *props,
             const char *key)
{
  uint64_t out = 0;

  g_variant_lookup (props, key, "t", &out);

  return out;
}


static gboolean
bool_prop (GVariant   *props,
           const char *key)
{
  gboolean out = FALSE;

  g_variant_lookup (props, key, "b", &out);

  return out;
}


static GVariant *
get_iface_props (GVariant   *ifaces,
                 const char *iface)
{
  return g_variant_lookup_value (ifaces, iface, G_VARIANT_TYPE_VARDICT);
}


/*
 * A drive's stable identity. Serial first — it survives replug, renaming and
 * moving the drive to another machine. WWN is the fallback. We never key on
 * sda/nvme0n1, which are assigned in probe order and shuffle between boots.
 */
static char *
make_drive_id (GVariant *drive_props)
{
  g_autofree char *serial = dup_string_prop (drive_props, "Serial");
  g_autofree char *wwn = dup_string_prop (drive_props, "WWN");
  g_autofree char *id = dup_string_prop (drive_props, "Id");

  if (serial && *serial)
    return g_strdup (serial);
  if (wwn && *wwn)
    return g_strdup (wwn);
  if (id && *id)
    return g_strdup (id);

  return NULL;
}


/* ------------------------------------------------------------------ */
/* NVMe                                                                */
/* ------------------------------------------------------------------ */

static void
read_nvme_health (GDBusConnection *bus,
                  const char      *object_path,
                  GVariant        *nvme_props,
                  GsmDriveInfo    *info)
{
  g_autoptr (GVariant) reply = NULL;
  g_autoptr (GVariant) attrs = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) warning = NULL;
  guint16 kelvin = 0;
  guint8 u8 = 0;

  info->smart_source = GSM_SMART_SOURCE_UDISKS;

  info->power_on_hours = uint64_prop (nvme_props, "SmartPowerOnHours");

  if (g_variant_lookup (nvme_props, "SmartTemperature", "q", &kelvin) && kelvin > 0)
    info->temperature = (int) kelvin - KELVIN_OFFSET;

  /* SmartCriticalWarning is an array of strings; a non-empty one is the
   * controller saying something is actually wrong. */
  warning = g_variant_lookup_value (nvme_props, "SmartCriticalWarning",
                                    G_VARIANT_TYPE_STRING_ARRAY);
  if (warning != NULL)
    info->smart_failing = g_variant_n_children (warning) > 0;

  /* The detail — wear, endurance, error counts — comes from a method call. */
  reply = g_dbus_connection_call_sync (bus,
                                       UDISKS_NAME,
                                       object_path,
                                       IFACE_NVME,
                                       "SmartGetAttributes",
                                       g_variant_new ("(a{sv})", NULL),
                                       G_VARIANT_TYPE ("(a{sv})"),
                                       G_DBUS_CALL_FLAGS_NONE,
                                       -1,
                                       NULL,
                                       &error);
  if (reply == NULL) {
    g_debug ("udisks: NVMe SmartGetAttributes on %s failed: %s",
             object_path, error->message);
    return;
  }

  attrs = g_variant_get_child_value (reply, 0);

  info->bytes_written = uint64_prop (attrs, "total_data_written");
  info->bytes_read = uint64_prop (attrs, "total_data_read");
  info->power_cycles = uint64_prop (attrs, "power_cycles");
  info->unsafe_shutdowns = uint64_prop (attrs, "unsafe_shutdowns");
  info->media_errors = uint64_prop (attrs, "media_errors");

  if (g_variant_lookup (attrs, "percent_used", "y", &u8))
    info->percent_used = u8;
  if (g_variant_lookup (attrs, "avail_spare", "y", &u8))
    info->spare_percent = u8;
  if (g_variant_lookup (attrs, "spare_thresh", "y", &u8))
    info->spare_threshold = u8;
}


/* ------------------------------------------------------------------ */
/* ATA                                                                 */
/* ------------------------------------------------------------------ */

/* udisks normalises each ATA attribute into a "pretty" value plus a unit,
 * which spares us the vendor-specific raw-field decoding. */
enum {
  PRETTY_UNKNOWN       = 0,
  PRETTY_DIMENSIONLESS = 1,
  PRETTY_MILLISECONDS  = 2,
  PRETTY_SECTORS       = 3,
  PRETTY_MILLIKELVIN   = 4,
};

static void
read_ata_health (GDBusConnection *bus,
                 const char      *object_path,
                 GVariant        *ata_props,
                 GsmDriveInfo    *info)
{
  g_autoptr (GVariant) reply = NULL;
  g_autoptr (GVariant) attrs = NULL;
  g_autoptr (GError) error = NULL;
  GVariantIter iter;
  guint8 id;
  const char *name;
  guint16 flags;
  gint32 value, worst, threshold;
  gint64 pretty;
  gint32 pretty_unit;
  g_autoptr (GVariant) expansion = NULL;

  if (!bool_prop (ata_props, "SmartSupported") ||
      !bool_prop (ata_props, "SmartEnabled")) {
    info->smart_source = GSM_SMART_SOURCE_UNSUPPORTED;
    return;
  }

  /* SmartUpdated is a timestamp; zero means udisks has never managed to read
   * this drive, so whatever else it reports is meaningless. */
  if (uint64_prop (ata_props, "SmartUpdated") == 0) {
    info->smart_source = GSM_SMART_SOURCE_LOCKED;
    return;
  }

  info->smart_source = GSM_SMART_SOURCE_UDISKS;
  info->smart_failing = bool_prop (ata_props, "SmartFailing");

  info->power_on_hours = uint64_prop (ata_props, "SmartPowerOnSeconds") / 3600;
  info->reallocated = uint64_prop (ata_props, "SmartNumBadSectors");

  {
    /* One lookup, with a real destination. Probing with a NULL pointer would
     * hand g_variant_get_va somewhere to write a double to. */
    double kelvin = 0;

    if (g_variant_lookup (ata_props, "SmartTemperature", "d", &kelvin) &&
        kelvin > 0)
      info->temperature = (int) (kelvin - KELVIN_OFFSET);
  }

  reply = g_dbus_connection_call_sync (bus,
                                       UDISKS_NAME,
                                       object_path,
                                       IFACE_ATA,
                                       "SmartGetAttributes",
                                       g_variant_new ("(a{sv})", NULL),
                                       G_VARIANT_TYPE ("(a(ysqiiixia{sv}))"),
                                       G_DBUS_CALL_FLAGS_NONE,
                                       -1,
                                       NULL,
                                       &error);
  if (reply == NULL) {
    /* Some configurations gate the attribute list behind polkit even though
     * the summary properties are free. The summary is still usable. */
    g_debug ("udisks: ATA SmartGetAttributes on %s failed: %s",
             object_path, error->message);
    return;
  }

  attrs = g_variant_get_child_value (reply, 0);
  g_variant_iter_init (&iter, attrs);

  while (g_variant_iter_next (&iter, "(y&sqiiixi@a{sv})",
                              &id, &name, &flags, &value, &worst,
                              &threshold, &pretty, &pretty_unit, &expansion)) {
    g_clear_pointer (&expansion, g_variant_unref);

    switch (id) {
      case 5:    /* Reallocated_Sector_Ct */
        if (pretty > 0)
          info->reallocated = MAX (info->reallocated, (uint64_t) pretty);
        break;

      case 194:  /* Temperature_Celsius */
        if (info->temperature < 0 && pretty_unit == PRETTY_MILLIKELVIN && pretty > 0)
          info->temperature = (int) ((pretty / 1000) - KELVIN_OFFSET);
        break;

      case 231:  /* SSD_Life_Left — normalised 100 (new) down to 0 (spent) */
      case 202:  /* Percent_Lifetime_Remain, same convention (Micron/Crucial) */
        if (info->percent_used < 0 && value >= 0 && value <= 100)
          info->percent_used = 100 - value;
        break;

      case 241:  /* Total_LBAs_Written */
        if (pretty_unit == PRETTY_SECTORS && pretty > 0)
          info->bytes_written = (uint64_t) pretty * 512;
        break;

      case 242:  /* Total_LBAs_Read */
        if (pretty_unit == PRETTY_SECTORS && pretty > 0)
          info->bytes_read = (uint64_t) pretty * 512;
        break;

      case 246:  /* Total_Host_Sector_Write (Micron/Crucial spelling of 241) */
        if (info->bytes_written == 0 && pretty > 0)
          info->bytes_written = (uint64_t) pretty * 512;
        break;

      case 9:    /* Power_On_Hours */
        if (info->power_on_hours == 0 && pretty_unit == PRETTY_MILLISECONDS && pretty > 0)
          info->power_on_hours = (uint64_t) pretty / (1000 * 3600);
        break;

      case 12:   /* Power_Cycle_Count */
        if (pretty > 0)
          info->power_cycles = (uint64_t) pretty;
        break;

      default:
        break;
    }
  }
}


/* ------------------------------------------------------------------ */
/* Block device topology                                               */
/* ------------------------------------------------------------------ */

/*
 * Find the whole-disk block device backing a drive: we need both its /dev
 * path (to hand to the privileged helper) and its dev_t (because cgroup
 * io.stat keys write accounting by "major:minor", and that is how we
 * attribute writes to this specific drive).
 */
static void
attach_block_device (GVariant     *managed,
                     const char   *drive_path,
                     GsmDriveInfo *info)
{
  GVariantIter iter;
  const char *obj_path;
  GVariant *raw_ifaces = NULL;

  g_variant_iter_init (&iter, managed);

  while (g_variant_iter_next (&iter, "{&o@a{sa{sv}}}", &obj_path, &raw_ifaces)) {
    g_autoptr (GVariant) ifaces = raw_ifaces;
    g_autoptr (GVariant) block = get_iface_props (ifaces, IFACE_BLOCK);
    g_autoptr (GVariant) partition = get_iface_props (ifaces, IFACE_PARTITION);
    g_autoptr (GVariant) drive_ref = NULL;
    g_autoptr (GVariant) device = NULL;
    uint64_t devnum;

    if (block == NULL)
      continue;

    /* Partitions carry their own Block interface and point at the same
     * drive. We want the whole disk, not /dev/nvme0n1p2. */
    if (partition != NULL)
      continue;

    drive_ref = g_variant_lookup_value (block, "Drive", G_VARIANT_TYPE_OBJECT_PATH);
    if (drive_ref == NULL ||
        g_strcmp0 (g_variant_get_string (drive_ref, NULL), drive_path) != 0)
      continue;

    device = g_variant_lookup_value (block, "PreferredDevice",
                                     G_VARIANT_TYPE_BYTESTRING);
    if (device != NULL)
      info->device = g_strdup (g_variant_get_bytestring (device));

    devnum = uint64_prop (block, "DeviceNumber");
    if (devnum != 0) {
      info->major = major ((dev_t) devnum);
      info->minor = minor ((dev_t) devnum);
    }

    return;
  }
}


/* ------------------------------------------------------------------ */

GPtrArray *
gsm_udisks_collect (GDBusConnection  *bus,
                    GError          **error)
{
  g_autoptr (GVariant) reply = NULL;
  g_autoptr (GVariant) managed = NULL;
  g_autoptr (GPtrArray) drives = NULL;
  GVariantIter iter;
  const char *obj_path;
  GVariant *raw_ifaces = NULL;

  g_return_val_if_fail (G_IS_DBUS_CONNECTION (bus), NULL);

  reply = g_dbus_connection_call_sync (bus,
                                       UDISKS_NAME,
                                       UDISKS_PATH,
                                       "org.freedesktop.DBus.ObjectManager",
                                       "GetManagedObjects",
                                       NULL,
                                       G_VARIANT_TYPE ("(a{oa{sa{sv}}})"),
                                       G_DBUS_CALL_FLAGS_NONE,
                                       -1,
                                       NULL,
                                       error);
  if (reply == NULL)
    return NULL;

  managed = g_variant_get_child_value (reply, 0);
  drives = g_ptr_array_new_with_free_func ((GDestroyNotify) gsm_drive_info_free);

  g_variant_iter_init (&iter, managed);

  while (g_variant_iter_next (&iter, "{&o@a{sa{sv}}}", &obj_path, &raw_ifaces)) {
    g_autoptr (GVariant) ifaces = raw_ifaces;
    g_autoptr (GVariant) drive_props = get_iface_props (ifaces, IFACE_DRIVE);
    g_autoptr (GVariant) ata_props = NULL;
    g_autoptr (GVariant) nvme_props = NULL;
    g_autofree char *id = NULL;
    GsmDriveInfo *info;
    gint32 rotation_rate = 0;

    if (drive_props == NULL)
      continue;

    id = make_drive_id (drive_props);
    if (id == NULL) {
      /* Without a stable identity we cannot honestly attribute history to
       * this drive across reboots, so we decline to track it at all rather
       * than key it on a device node that will move. */
      g_debug ("udisks: skipping %s, no serial or WWN", obj_path);
      continue;
    }

    info = g_new0 (GsmDriveInfo, 1);
    info->temperature = -1;
    info->percent_used = -1;
    info->spare_percent = -1;
    info->spare_threshold = -1;
    info->present = TRUE;
    info->smart_source = GSM_SMART_SOURCE_LOCKED;

    info->serial = g_strdup (id);
    info->model = dup_string_prop (drive_props, "Model");
    info->firmware = dup_string_prop (drive_props, "Revision");
    info->connection = dup_string_prop (drive_props, "ConnectionBus");
    info->removable = bool_prop (drive_props, "Removable") ||
                      bool_prop (drive_props, "MediaRemovable");
    info->size = uint64_prop (drive_props, "Size");

    /* RotationRate is 0 for SSDs, -1 for "unknown", >0 RPM for spinning rust. */
    if (g_variant_lookup (drive_props, "RotationRate", "i", &rotation_rate))
      info->rotational = rotation_rate > 0;

    attach_block_device (managed, obj_path, info);

    nvme_props = get_iface_props (ifaces, IFACE_NVME);
    ata_props = get_iface_props (ifaces, IFACE_ATA);

    if (nvme_props != NULL)
      read_nvme_health (bus, obj_path, nvme_props, info);
    else if (ata_props != NULL)
      read_ata_health (bus, obj_path, ata_props, info);
    /* else: udisks sees the drive but exposes no health interface at all —
     * typical of USB bridges. It stays LOCKED, and the UI offers the helper. */

    g_ptr_array_add (drives, info);
  }

  return g_steal_pointer (&drives);
}
