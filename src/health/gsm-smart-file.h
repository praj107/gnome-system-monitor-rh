/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Reads smart.ini, the snapshot the system SMART reader writes for
 * USB-attached disks (a GLib key file: a [meta] group with schema,
 * generated_at and reader_version, and one [drive NAME] group per disk with
 * a status and the figures the drive gave). Every outcome is explicit: a
 * missing, unreadable, malformed, unknown-schema or stale file is a state,
 * and a figure the drive did not give is absent, never zero.
 */

#pragma once

#include <stdint.h>

#include <glib.h>

#include "gsm-drive.h"

G_BEGIN_DECLS

#define GSM_SMART_FILE_DEFAULT_PATH "/run/gnome-system-monitor-rh/smart.ini"

/* The reader runs every ten minutes; a file older than three runs is stale. */
#define GSM_SMART_FILE_MAX_AGE_SECONDS (30 * 60)

typedef enum
{
  GSM_SMART_FILE_OK,
  GSM_SMART_FILE_MISSING,           /* no such file: the reader has not run */
  GSM_SMART_FILE_UNREADABLE,        /* exists, cannot be read */
  GSM_SMART_FILE_MALFORMED,         /* not a key file, or no usable [meta] */
  GSM_SMART_FILE_UNKNOWN_SCHEMA,    /* a schema this code does not know */
  GSM_SMART_FILE_STALE,             /* generated too long ago to describe now */
} GsmSmartFileState;

typedef enum
{
  GSM_SMART_DRIVE_OK,
  GSM_SMART_DRIVE_ASLEEP,
  GSM_SMART_DRIVE_UNSUPPORTED,
  GSM_SMART_DRIVE_FAILED,
  GSM_SMART_DRIVE_GARBLED,
  GSM_SMART_DRIVE_UNKNOWN_STATUS,   /* a status word this code does not know */
} GsmSmartDriveStatus;

typedef enum
{
  GSM_SMART_FIGURE_BYTES_WRITTEN,
  GSM_SMART_FIGURE_BYTES_READ,
  GSM_SMART_FIGURE_PERCENT_USED,
  GSM_SMART_FIGURE_POWER_ON_HOURS,
  GSM_SMART_FIGURE_TEMPERATURE_KELVIN,
  GSM_SMART_FIGURE_MEDIA_ERRORS,
  GSM_SMART_FIGURE_UNSAFE_SHUTDOWNS,
  GSM_SMART_FIGURE_COUNT,
} GsmSmartFigure;

typedef struct
{
  char               *name;            /* the kernel's name, e.g. sda */
  char               *model;
  char               *serial;
  char               *usb_serial;
  char               *usb_id;
  char               *bridge;
  char               *basis;
  GsmSmartDriveStatus status;
  unsigned            known;           /* bit n set: value[n] is a reading */
  uint64_t            value[GSM_SMART_FIGURE_COUNT];
  int64_t             last_ok_generated_at;   /* 0 when never */
} GsmSmartFileDrive;

typedef struct
{
  GsmSmartFileState state;
  int64_t           generated_at;      /* 0 unless the meta group parsed */
  char             *reader_version;
  GPtrArray        *drives;            /* GsmSmartFileDrive*, empty unless parsed */
} GsmSmartFile;

/* Never NULL. `now_unix` and `max_age_seconds` decide staleness. */
GsmSmartFile *gsm_smart_file_load (const char *path,
                                   int64_t     now_unix,
                                   int64_t     max_age_seconds);
void          gsm_smart_file_free (GsmSmartFile *file);

const char *  gsm_smart_file_state_text (GsmSmartFileState state);

gboolean      gsm_smart_figure_known (const GsmSmartFileDrive *drive,
                                      GsmSmartFigure           figure);

/* The disk whose serial (or USB serial) equals `serial`, or NULL. */
const GsmSmartFileDrive * gsm_smart_file_find (const GsmSmartFile *file,
                                               const char         *serial);

/*
 * Copy a drive's figures into `info`, only those the drive gave, only from a
 * fresh file and a drive whose status is ok. FALSE (info untouched) otherwise.
 */
gboolean      gsm_smart_file_fill_info (const GsmSmartFile      *file,
                                        const GsmSmartFileDrive *drive,
                                        GsmDriveInfo            *info);

G_END_DECLS
