/*
 * gsm-smart-file — parse the system SMART reader's smart.ini.
 *
 * Figures follow the reader's definitions: bytes_written and bytes_read are
 * NVMe data units times 512,000 bytes (NVMe base specification, SMART /
 * Health Information log), percent_used is the log's "Percentage Used",
 * power_on_hours is the drive's own count, temperature_kelvin the composite
 * temperature. A key that is absent or does not parse as a number is an
 * unknown figure.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <errno.h>
#include <string.h>

#include "gsm-smart-file.h"

#define SUPPORTED_SCHEMA        1
#define MAX_FILE_BYTES          (256 * 1024)
#define CLOCK_SKEW_SECONDS      3600
#define KELVIN_OFFSET           273
#define TEMPERATURE_MIN_KELVIN  233u
#define TEMPERATURE_MAX_KELVIN  400u

static const char *const figure_keys[GSM_SMART_FIGURE_COUNT] = {
  [GSM_SMART_FIGURE_BYTES_WRITTEN] = "bytes_written",
  [GSM_SMART_FIGURE_BYTES_READ] = "bytes_read",
  [GSM_SMART_FIGURE_PERCENT_USED] = "percent_used",
  [GSM_SMART_FIGURE_POWER_ON_HOURS] = "power_on_hours",
  [GSM_SMART_FIGURE_TEMPERATURE_KELVIN] = "temperature_kelvin",
  [GSM_SMART_FIGURE_MEDIA_ERRORS] = "media_errors",
  [GSM_SMART_FIGURE_UNSAFE_SHUTDOWNS] = "unsafe_shutdowns",
};

static const struct
{
  const char          *word;
  GsmSmartDriveStatus  status;
} status_words[] = {
  { "ok", GSM_SMART_DRIVE_OK },
  { "asleep", GSM_SMART_DRIVE_ASLEEP },
  { "unsupported", GSM_SMART_DRIVE_UNSUPPORTED },
  { "failed", GSM_SMART_DRIVE_FAILED },
  { "garbled", GSM_SMART_DRIVE_GARBLED },
};


static void
drive_free (GsmSmartFileDrive *drive)
{
  g_free (drive->name);
  g_free (drive->model);
  g_free (drive->serial);
  g_free (drive->usb_serial);
  g_free (drive->usb_id);
  g_free (drive->bridge);
  g_free (drive->basis);
  g_free (drive);
}


void
gsm_smart_file_free (GsmSmartFile *file)
{
  if (file == NULL)
    return;

  g_free (file->reader_version);
  g_clear_pointer (&file->drives, g_ptr_array_unref);
  g_free (file);
}


const char *
gsm_smart_file_state_text (GsmSmartFileState state)
{
  switch (state)
    {
    case GSM_SMART_FILE_OK:
      return "fresh";
    case GSM_SMART_FILE_MISSING:
      return "the reader has not written a snapshot";
    case GSM_SMART_FILE_UNREADABLE:
      return "the snapshot cannot be read";
    case GSM_SMART_FILE_MALFORMED:
      return "the snapshot is malformed";
    case GSM_SMART_FILE_UNKNOWN_SCHEMA:
      return "the snapshot is a newer format than this program reads";
    case GSM_SMART_FILE_STALE:
      return "the snapshot is out of date";
    default:
      return "unknown";
    }
}


gboolean
gsm_smart_figure_known (const GsmSmartFileDrive *drive,
                        GsmSmartFigure           figure)
{
  g_return_val_if_fail (drive != NULL, FALSE);
  g_return_val_if_fail (figure < GSM_SMART_FIGURE_COUNT, FALSE);

  return (drive->known >> figure) & 1u;
}


/* A key that is not a plain unsigned number leaves the figure unknown. */
static gboolean
read_unsigned (GKeyFile   *keys,
               const char *group,
               const char *key,
               uint64_t   *out)
{
  g_autofree char *text = g_key_file_get_string (keys, group, key, NULL);
  char *end = NULL;

  if (text == NULL || text[0] == '\0' || !g_ascii_isdigit (text[0]))
    return FALSE;

  errno = 0;
  *out = g_ascii_strtoull (text, &end, 10);

  return errno == 0 && end != text && *end == '\0';
}


static GsmSmartDriveStatus
parse_status (const char *word)
{
  for (gsize i = 0; i < G_N_ELEMENTS (status_words); i++)
    {
      if (g_strcmp0 (word, status_words[i].word) == 0)
        return status_words[i].status;
    }

  return GSM_SMART_DRIVE_UNKNOWN_STATUS;
}


static GsmSmartFileDrive *
parse_drive (GKeyFile   *keys,
             const char *group)
{
  GsmSmartFileDrive *drive = g_new0 (GsmSmartFileDrive, 1);
  g_autofree char *status = g_key_file_get_string (keys, group, "status", NULL);
  uint64_t value;

  drive->name = g_strdup (group + strlen ("drive "));
  drive->model = g_key_file_get_string (keys, group, "model", NULL);
  drive->serial = g_key_file_get_string (keys, group, "serial", NULL);
  drive->usb_serial = g_key_file_get_string (keys, group, "usb_serial", NULL);
  drive->usb_id = g_key_file_get_string (keys, group, "usb_id", NULL);
  drive->bridge = g_key_file_get_string (keys, group, "bridge", NULL);
  drive->basis = g_key_file_get_string (keys, group, "basis", NULL);
  drive->status = parse_status (status);

  for (int i = 0; i < GSM_SMART_FIGURE_COUNT; i++)
    {
      if (read_unsigned (keys, group, figure_keys[i], &value))
        {
          drive->value[i] = value;
          drive->known |= 1u << i;
        }
    }

  if (read_unsigned (keys, group, "last_ok_generated_at", &value))
    drive->last_ok_generated_at = (int64_t) value;

  return drive;
}


GsmSmartFile *
gsm_smart_file_load (const char *path,
                     int64_t     now_unix,
                     int64_t     max_age_seconds)
{
  GsmSmartFile *file = g_new0 (GsmSmartFile, 1);
  g_autoptr (GKeyFile) keys = g_key_file_new ();
  g_autoptr (GError) error = NULL;
  g_autofree char *contents = NULL;
  g_auto (GStrv) groups = NULL;
  gsize length = 0;
  uint64_t schema;
  uint64_t generated_at;

  file->drives = g_ptr_array_new_with_free_func ((GDestroyNotify) drive_free);

  if (!g_file_get_contents (path, &contents, &length, &error))
    {
      file->state = g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT)
                    ? GSM_SMART_FILE_MISSING : GSM_SMART_FILE_UNREADABLE;
      return file;
    }

  if (length > MAX_FILE_BYTES ||
      !g_key_file_load_from_data (keys, contents, length, G_KEY_FILE_NONE, NULL) ||
      !read_unsigned (keys, "meta", "schema", &schema) ||
      !read_unsigned (keys, "meta", "generated_at", &generated_at) ||
      generated_at == 0)
    {
      file->state = GSM_SMART_FILE_MALFORMED;
      return file;
    }

  if (schema != SUPPORTED_SCHEMA)
    {
      file->state = GSM_SMART_FILE_UNKNOWN_SCHEMA;
      return file;
    }

  file->generated_at = (int64_t) generated_at;
  file->reader_version = g_key_file_get_string (keys, "meta", "reader_version", NULL);

  /* A time well in the future is a broken clock or a broken file, and either
   * way the figures cannot be called fresh. */
  if (file->generated_at > now_unix + CLOCK_SKEW_SECONDS)
    {
      file->state = GSM_SMART_FILE_MALFORMED;
      return file;
    }

  groups = g_key_file_get_groups (keys, NULL);
  for (guint i = 0; groups[i] != NULL; i++)
    {
      if (g_str_has_prefix (groups[i], "drive ") && groups[i][strlen ("drive ")] != '\0')
        g_ptr_array_add (file->drives, parse_drive (keys, groups[i]));
    }

  file->state = now_unix - file->generated_at > max_age_seconds
                ? GSM_SMART_FILE_STALE : GSM_SMART_FILE_OK;

  return file;
}


static gboolean
same_serial (const char *a,
             const char *b)
{
  g_autofree char *left = a != NULL ? g_strstrip (g_strdup (a)) : NULL;
  g_autofree char *right = b != NULL ? g_strstrip (g_strdup (b)) : NULL;

  return left != NULL && right != NULL && left[0] != '\0' && strcmp (left, right) == 0;
}


const GsmSmartFileDrive *
gsm_smart_file_find (const GsmSmartFile *file,
                     const char         *serial)
{
  g_return_val_if_fail (file != NULL, NULL);

  for (guint i = 0; i < file->drives->len; i++)
    {
      const GsmSmartFileDrive *drive = g_ptr_array_index (file->drives, i);

      if (same_serial (drive->serial, serial) || same_serial (drive->usb_serial, serial))
        return drive;
    }

  return NULL;
}


gboolean
gsm_smart_file_fill_info (const GsmSmartFile      *file,
                          const GsmSmartFileDrive *drive,
                          GsmDriveInfo            *info)
{
  gboolean filled = FALSE;

  g_return_val_if_fail (file != NULL && drive != NULL && info != NULL, FALSE);

  if (file->state != GSM_SMART_FILE_OK || drive->status != GSM_SMART_DRIVE_OK)
    return FALSE;

  if (gsm_smart_figure_known (drive, GSM_SMART_FIGURE_BYTES_WRITTEN))
    {
      info->bytes_written = drive->value[GSM_SMART_FIGURE_BYTES_WRITTEN];
      filled = TRUE;
    }
  if (gsm_smart_figure_known (drive, GSM_SMART_FIGURE_BYTES_READ))
    {
      info->bytes_read = drive->value[GSM_SMART_FIGURE_BYTES_READ];
      filled = TRUE;
    }
  if (gsm_smart_figure_known (drive, GSM_SMART_FIGURE_PERCENT_USED) &&
      drive->value[GSM_SMART_FIGURE_PERCENT_USED] <= 100)
    {
      info->percent_used = (int) drive->value[GSM_SMART_FIGURE_PERCENT_USED];
      filled = TRUE;
    }
  if (gsm_smart_figure_known (drive, GSM_SMART_FIGURE_POWER_ON_HOURS))
    {
      info->power_on_hours = drive->value[GSM_SMART_FIGURE_POWER_ON_HOURS];
      filled = TRUE;
    }
  if (gsm_smart_figure_known (drive, GSM_SMART_FIGURE_TEMPERATURE_KELVIN) &&
      drive->value[GSM_SMART_FIGURE_TEMPERATURE_KELVIN] >= TEMPERATURE_MIN_KELVIN &&
      drive->value[GSM_SMART_FIGURE_TEMPERATURE_KELVIN] <= TEMPERATURE_MAX_KELVIN)
    {
      info->temperature = (int) drive->value[GSM_SMART_FIGURE_TEMPERATURE_KELVIN] - KELVIN_OFFSET;
      filled = TRUE;
    }
  if (gsm_smart_figure_known (drive, GSM_SMART_FIGURE_MEDIA_ERRORS))
    {
      info->media_errors = drive->value[GSM_SMART_FIGURE_MEDIA_ERRORS];
      filled = TRUE;
    }
  if (gsm_smart_figure_known (drive, GSM_SMART_FIGURE_UNSAFE_SHUTDOWNS))
    {
      info->unsafe_shutdowns = drive->value[GSM_SMART_FIGURE_UNSAFE_SHUTDOWNS];
      filled = TRUE;
    }

  if (filled)
    info->smart_source = GSM_SMART_SOURCE_HELPER;   /* the privileged reader's snapshot */

  return filled;
}
