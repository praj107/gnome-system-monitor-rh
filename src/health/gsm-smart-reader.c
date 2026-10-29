/*
 * gsm-smart-reader — SMART for USB-attached disks, written to a file.
 *
 * Started by a systemd timer and a udev rule as root with CAP_SYS_RAWIO. It
 * takes no arguments, honours no environment variable and reads no
 * configuration: it lists /sys/block itself, speaks only to bridges named in
 * its table, and replaces /run/gnome-system-monitor-rh/smart.ini atomically.
 * The logic is in gsm-smart-reader-core.c; this file only supplies the real
 * sysfs root, output directory and SG_IO transport.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "gsm-smart-reader-core.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <scsi/sg.h>

#ifndef GSM_SMART_READER_VERSION
#define GSM_SMART_READER_VERSION "unknown"
#endif

/* A bridge that hangs is not worth waiting 15 s for, command after command. */
#define SG_TIMEOUT_MS 8000

/*
 * mutate: ignore — this opens and sends commands to real block devices.
 * Tests link the core with a fake transport and never run this file.
 *
 * The node is opened read-only, non-blocking, without following links, with
 * the capability already held (a descriptor whose credentials change after
 * open is refused by sg.c).
 */
static intptr_t
real_open_device (void       *user,
                  const char *name)
{
  char path[64];
  struct stat st;
  int fd;

  (void) user;

  if (snprintf (path, sizeof path, "/dev/%s", name) >= (int) sizeof path)
    return -1;

  fd = open (path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0)
    return -1;

  if (fstat (fd, &st) != 0 || !S_ISBLK (st.st_mode))
    {
      close (fd);
      return -1;
    }

  return fd;
}


static int
real_scsi_cmd (void            *user,
               intptr_t         device,
               const uint8_t   *cdb,
               uint8_t          cdb_len,
               GsmScsiDirection direction,
               void            *data,
               uint32_t         data_len,
               GsmScsiResult   *result)
{
  sg_io_hdr_t io;
  uint8_t sense[GSM_SMART_SENSE_LEN];

  (void) user;

  memset (&io, 0, sizeof io);
  memset (sense, 0, sizeof sense);

  io.interface_id = 'S';
  io.dxfer_direction = direction == GSM_SCSI_FROM_DEVICE ? SG_DXFER_FROM_DEV :
                                                           direction == GSM_SCSI_TO_DEVICE ? SG_DXFER_TO_DEV :
                                                                                             SG_DXFER_NONE;
  io.cmd_len = cdb_len;
  io.cmdp = (uint8_t *) cdb;
  io.sbp = sense;
  io.mx_sb_len = sizeof sense;
  io.dxfer_len = data_len;
  io.dxferp = data;
  io.timeout = SG_TIMEOUT_MS;

  if (ioctl ((int) device, SG_IO, &io) < 0)
    return -1;

  result->scsi_status = io.status;
  result->host_status = (uint8_t) io.host_status;
  result->driver_status = (uint8_t) io.driver_status;
  result->resid = io.resid;
  result->sense_len = io.sb_len_wr;
  memcpy (result->sense, sense, sizeof sense);

  return 0;
}


static void
real_close_device (void    *user,
                   intptr_t device)
{
  (void) user;
  close ((int) device);
}


int
main (int    argc,
      char **argv)
{
  static const GsmSmartTransport transport = {
    real_open_device, real_scsi_cmd, real_close_device, NULL,
  };
  GsmSmartRun run = {
    .sys_root = "/sys",
    .out_dir = "/run/gnome-system-monitor-rh",
    .transport = &transport,
    .reader_version = GSM_SMART_READER_VERSION,
  };

  (void) argv;

  if (argc != 1)
    {
      fprintf (stderr, "gsm-smart-reader: takes no arguments\n");
      return 2;
    }

  run.now_unix_s = (int64_t) time (NULL);

  if (gsm_smart_run (&run) != 0)
    {
      fprintf (stderr, "gsm-smart-reader: cannot write %s/smart.ini: %s\n",
               run.out_dir, strerror (errno));
      return 1;
    }

  return 0;
}
