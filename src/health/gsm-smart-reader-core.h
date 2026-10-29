/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The core of the SMART reader for USB-attached disks: enumerate /sys/block,
 * identify the USB bridge, ask the drive through the one protocol the bridge
 * table names for it, check the answer, and write smart.ini.
 *
 * The sysfs root, the output directory and the SCSI transport are parameters
 * so a test can run the whole thing against a fake tree and a fake drive.
 * The installed reader passes the real ones and has no other input. No GLib:
 * the reader runs as root.
 */

#ifndef GSM_SMART_READER_CORE_H
#define GSM_SMART_READER_CORE_H

#include <stdint.h>

#define GSM_SMART_SCHEMA 1
#define GSM_SMART_SENSE_LEN 32

typedef enum
{
  GSM_SMART_OK,
  GSM_SMART_ASLEEP,
  GSM_SMART_UNSUPPORTED,
  GSM_SMART_FAILED,
  GSM_SMART_GARBLED,
} GsmSmartStatus;

typedef enum
{
  GSM_SCSI_NO_DATA,
  GSM_SCSI_FROM_DEVICE,
  GSM_SCSI_TO_DEVICE,
} GsmScsiDirection;

/* What one SCSI command came back with (the fields of sg_io_hdr_t we use). */
typedef struct
{
  uint8_t scsi_status;
  uint8_t host_status;
  uint8_t driver_status;
  uint8_t sense_len;
  int32_t resid;
  uint8_t sense[GSM_SMART_SENSE_LEN];
} GsmScsiResult;

typedef struct
{
  /* A handle for the disk called `name` (as in /sys/block), or -1. */
  intptr_t (*open_device) (void       *user,
                           const char *name);

  /* Sends one CDB. 0 when the command was issued and `result` is filled in,
   * even if the device answered CHECK CONDITION; -1 when it was not. */
  int (*scsi_cmd) (void            *user,
                   intptr_t         device,
                   const uint8_t   *cdb,
                   uint8_t          cdb_len,
                   GsmScsiDirection direction,
                   void            *data,
                   uint32_t         data_len,
                   GsmScsiResult   *result);

  void (*close_device) (void    *user,
                        intptr_t device);

  void *user;
} GsmSmartTransport;

typedef struct
{
  const char              *sys_root;        /* "/sys" */
  const char              *out_dir;         /* smart.ini goes here */
  const GsmSmartTransport *transport;
  int64_t now_unix_s;
  const char              *reader_version;
} GsmSmartRun;

/* Reads every USB disk and replaces smart.ini. 0, or -1 if the file could
 * not be written (the old one is then left as it was). */
int gsm_smart_run (const GsmSmartRun *run);

#endif
