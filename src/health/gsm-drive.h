/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <stdint.h>

#include <glib-object.h>

G_BEGIN_DECLS

/* Where a drive's health numbers came from. Shown in the UI: a projection
 * built on a guess must never look like one built on a measurement. */
typedef enum {
  GSM_SMART_SOURCE_NONE,        /* nothing could be read                  */
  GSM_SMART_SOURCE_UDISKS,      /* udisks2, no privilege needed           */
  GSM_SMART_SOURCE_HELPER,      /* privileged gsm-smart helper            */
  GSM_SMART_SOURCE_LOCKED,      /* needs the helper; user hasn't unlocked */
  GSM_SMART_SOURCE_UNSUPPORTED, /* drive reports no SMART at all          */
} GsmSmartSource;

/* Where "rated endurance" came from, in descending order of trust. */
typedef enum {
  GSM_ENDURANCE_UNKNOWN,
  GSM_ENDURANCE_USER,        /* user typed it in                          */
  GSM_ENDURANCE_DERIVED,     /* implied by host writes vs reported wear   */
  GSM_ENDURANCE_SPEC_DB,     /* our shipped model -> rated TBW table      */
} GsmEnduranceSource;

typedef enum {
  GSM_DRIVE_HEALTH_OK,
  GSM_DRIVE_HEALTH_ATTENTION,   /* worth telling the user about           */
  GSM_DRIVE_HEALTH_FAILING,     /* back up now                            */
  GSM_DRIVE_HEALTH_UNKNOWN,
} GsmDriveHealth;

#define GSM_TYPE_DRIVE (gsm_drive_get_type ())
G_DECLARE_FINAL_TYPE (GsmDrive, gsm_drive, GSM, DRIVE, GObject)

GsmDrive       *gsm_drive_new                   (const char *id);

/* Stable across reboots, renames and replug — serial or WWN, never sdX. */
const char     *gsm_drive_get_id                (GsmDrive *self);
const char     *gsm_drive_get_model             (GsmDrive *self);
const char     *gsm_drive_get_device            (GsmDrive *self);
const char     *gsm_drive_get_connection        (GsmDrive *self);

gboolean        gsm_drive_get_present           (GsmDrive *self);
gboolean        gsm_drive_get_removable         (GsmDrive *self);
gboolean        gsm_drive_get_rotational        (GsmDrive *self);
int64_t         gsm_drive_get_last_seen         (GsmDrive *self);

/* dev_t major/minor of the whole-disk device, as cgroup io.stat keys it. */
guint           gsm_drive_get_major             (GsmDrive *self);
guint           gsm_drive_get_minor             (GsmDrive *self);

GsmSmartSource  gsm_drive_get_smart_source      (GsmDrive *self);
GsmDriveHealth  gsm_drive_get_health            (GsmDrive *self);

/* We have a verdict for this drive, but its transport reports no wear data. */
gboolean        gsm_drive_get_wear_unavailable  (GsmDrive *self);

uint64_t        gsm_drive_get_size              (GsmDrive *self);
uint64_t        gsm_drive_get_bytes_written     (GsmDrive *self);
uint64_t        gsm_drive_get_bytes_read        (GsmDrive *self);
uint64_t        gsm_drive_get_power_on_hours    (GsmDrive *self);
uint64_t        gsm_drive_get_power_cycles      (GsmDrive *self);
uint64_t        gsm_drive_get_unsafe_shutdowns  (GsmDrive *self);
uint64_t        gsm_drive_get_media_errors      (GsmDrive *self);
uint64_t        gsm_drive_get_reallocated       (GsmDrive *self);
int             gsm_drive_get_temperature       (GsmDrive *self);  /* C, -1 unknown */
int             gsm_drive_get_percent_used      (GsmDrive *self);  /* -1 unknown    */
int             gsm_drive_get_spare_percent     (GsmDrive *self);  /* -1 unknown    */

/*
 * Rated total-bytes-written for this drive. Reports which source it came
 * from so the UI can label it; see gsm_drive_get_endurance_source().
 */
uint64_t           gsm_drive_get_rated_tbw          (GsmDrive *self);
GsmEnduranceSource gsm_drive_get_endurance_source   (GsmDrive *self);

/*
 * True when the controller's own wear estimate and the datasheet rating tell
 * materially different stories (as they do on e.g. a WD SN740). The UI says
 * so out loud rather than quietly picking one.
 */
gboolean        gsm_drive_endurance_is_conflicted (GsmDrive *self);

/*
 * Collectors fill one of these and hand it over; GsmDrive owns the merge so
 * that a lower-trust source can never clobber a field a better one already
 * supplied. Every numeric field uses -1 / 0 to mean "I don't know", never a
 * plausible-looking zero.
 */
typedef struct {
  char          *model;
  char          *serial;
  char          *firmware;
  char          *device;
  char          *connection;

  gboolean       present;
  gboolean       removable;
  gboolean       rotational;

  guint          major;
  guint          minor;

  uint64_t       size;
  uint64_t       bytes_written;
  uint64_t       bytes_read;
  uint64_t       power_on_hours;
  uint64_t       power_cycles;
  uint64_t       unsafe_shutdowns;
  uint64_t       media_errors;
  uint64_t       reallocated;

  int            temperature;      /* C,  -1 unknown */
  int            percent_used;     /* %,  -1 unknown */
  int            spare_percent;    /* %,  -1 unknown */
  int            spare_threshold;  /* %,  -1 unknown */

  gboolean       smart_failing;

  /*
   * TRUE when we reached the drive and it gave us a pass/fail verdict, but the
   * transport cannot report wear or bytes written — a plain SCSI enclosure, for
   * instance. Distinct from "we could not read it", and distinct from "it has
   * written nothing": the UI must say which of the three it is.
   */
  gboolean       wear_unavailable;

  GsmSmartSource smart_source;
} GsmDriveInfo;

void  gsm_drive_info_clear   (GsmDriveInfo *info);
void  gsm_drive_apply_info   (GsmDrive     *self,
                              const GsmDriveInfo *info);

void  gsm_drive_set_present  (GsmDrive *self,
                              gboolean  present);
void  gsm_drive_set_last_seen (GsmDrive *self,
                               int64_t   when);

/* Endurance is resolved separately because it needs the spec DB and the
 * drive's own measurements together. */
void  gsm_drive_set_endurance (GsmDrive           *self,
                               uint64_t            rated_tbw,
                               GsmEnduranceSource  source);

G_END_DECLS
