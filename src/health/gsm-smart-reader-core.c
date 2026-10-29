/*
 * gsm-smart-reader-core — read SMART from USB-attached disks, safely.
 *
 * For each whole disk under /sys/block whose sysfs ancestry contains a USB
 * device, the core looks up that device's exact vendor:product[:bcdDevice]
 * in the bridge table and speaks the one protocol the table names:
 *
 *   - SAT      ATA PASS-THROUGH(16), after CHECK POWER MODE (T10 SAT-3 and
 *              ACS-4); a drive in standby is left alone.
 *   - ASMedia  one vendor CDB (0xE6) per NVMe admin command
 *   - Realtek  one vendor CDB (0xE4), answers capped at 512 bytes
 *   - JMicron  three 0xA1 CDBs per NVMe admin command (command, data,
 *              completion)
 *
 * The vendor CDB layouts follow smartmontools' scsinvme.cpp (GPL-2.0-or-later,
 * as is this file); the NVMe structures follow the NVMe base specification
 * (Identify Controller, SMART / Health Information log page 02h); the ATA
 * structures follow ACS-4 (IDENTIFY DEVICE, SMART READ DATA). A bridge that
 * is not in the table gets no command at all, and a disk whose runtime PM
 * status is "suspended" gets none either: waking it is not this program's
 * business.
 *
 * Definitions of what lands in smart.ini (a GLib key file):
 *   bytes_written / bytes_read   NVMe data units written / read times
 *                                512000 bytes (NVMe base spec, SMART log
 *                                bytes 32-63)
 *   percent_used                 NVMe "Percentage Used", byte 5
 *   power_on_hours               NVMe bytes 128-143, or ATA attribute 9 raw
 *                                (low 32 bits), as the drive counts them
 *   temperature_kelvin           NVMe composite temperature, bytes 1-2; or
 *                                ATA attribute 194 raw low byte plus 273
 *   media_errors, unsafe_shutdowns  NVMe bytes 160-175 and 144-159
 * A figure the drive does not give, or gives implausibly, is absent: never
 * zero. ATA wear and volume attributes are vendor specific, so none of them
 * is reported as bytes or percent.
 *
 * Kernel and drive input is untrusted: every answer is size-checked, and a
 * vanished disk is a status, not a crash.
 */

#include "gsm-smart-reader-core.h"
#include "gsm-smart-bridges.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_DRIVES          64
#define NAME_LEN            32
#define TEXT_LEN            64
#define PATH_LEN            (PATH_MAX + 64)
#define MAX_PREVIOUS_BYTES  (256 * 1024)
#define OUTPUT_FILE         "smart.ini"
#define OUTPUT_TMP_FILE     "smart.ini.tmp"
#define USB_WALK_DEPTH      16

/* --- NVMe (NVMe base specification) --- */

#define NVME_ADMIN_GET_LOG_PAGE 0x02
#define NVME_ADMIN_IDENTIFY     0x06
#define NVME_LOG_SMART          0x02
#define NVME_BROADCAST_NSID     0xffffffffu
#define NVME_IDENTIFY_LEN       4096
#define NVME_SMART_LOG_LEN      512
#define NVME_DATA_UNIT_BYTES    (1000ULL * 512ULL)

#define TEMPERATURE_MIN_KELVIN  233u
#define TEMPERATURE_MAX_KELVIN  400u
#define POWER_ON_HOURS_MAX      (100ULL * 366ULL * 24ULL)

/* --- ATA (ACS-4) --- */

#define ATA_IDENTIFY_DEVICE     0xEC
#define ATA_CHECK_POWER_MODE    0xE5
#define ATA_SMART               0xB0
#define ATA_SMART_READ_DATA     0xD0
#define ATA_SECTOR_SIZE         512
#define ATA_MAX_ATTRS           30
#define ATA_ATTR_POWER_ON_HOURS 9
#define ATA_ATTR_TEMPERATURE    194

/* --- SCSI --- */

#define SG_ATA_PASSTHRU_16      0x85
#define SG_ATA_PROTO_NON_DATA   (3 << 1)
#define SG_ATA_PROTO_PIO_DATA_IN (4 << 1)
#define SG_ATA_CK_COND          0x20
#define SG_ATA_FLAGS_IN         0x2E
#define SCSI_SENSE_DESCRIPTOR_CURRENT 0x72
#define SCSI_SENSE_DESCRIPTOR_DEFERRED 0x73
#define SCSI_DESCRIPTOR_ATA_RETURN 0x09

/* The figures smart.ini can carry, as table rows. */
typedef enum
{
  FIG_BYTES_WRITTEN,
  FIG_BYTES_READ,
  FIG_PERCENT_USED,
  FIG_POWER_ON_HOURS,
  FIG_TEMPERATURE_KELVIN,
  FIG_MEDIA_ERRORS,
  FIG_UNSAFE_SHUTDOWNS,
  FIG_COUNT,
} Figure;

typedef struct
{
  const char *key;
  bool only_grows;          /* a lower value than last time is not believable */
} FigureInfo;

/* *INDENT-OFF* */
static const FigureInfo figure_info[FIG_COUNT] = {
  [FIG_BYTES_WRITTEN] = { "bytes_written", true },
  [FIG_BYTES_READ] = { "bytes_read", true },
  [FIG_PERCENT_USED] = { "percent_used", false },
  [FIG_POWER_ON_HOURS] = { "power_on_hours", true },
  [FIG_TEMPERATURE_KELVIN] = { "temperature_kelvin", false },
  [FIG_MEDIA_ERRORS] = { "media_errors", true },
  [FIG_UNSAFE_SHUTDOWNS] = { "unsafe_shutdowns", true },
};
/* *INDENT-ON* */

static const char *const status_names[] = {
  [GSM_SMART_OK] = "ok",
  [GSM_SMART_ASLEEP] = "asleep",
  [GSM_SMART_UNSUPPORTED] = "unsupported",
  [GSM_SMART_FAILED] = "failed",
  [GSM_SMART_GARBLED] = "garbled",
};

typedef struct
{
  uint64_t value[FIG_COUNT];
  unsigned known;                 /* bit n set: value[n] is a real reading */
} Figures;

typedef struct
{
  char name[NAME_LEN];
  char model[TEXT_LEN];
  char serial[TEXT_LEN];
  char usb_serial[TEXT_LEN];
  char usb_id[16];
  char bcd_device[8];
  GsmSmartStatus status;
  Figures figures;
  int64_t last_ok_generated_at;          /* 0: never read */
  const GsmBridge *bridge;
  bool have_usb_id;
  bool asleep_by_pm;
} Drive;

typedef struct
{
  Drive drives[MAX_DRIVES];
  size_t count;
  int64_t generated_at;
} Snapshot;

typedef struct
{
  const GsmSmartTransport *transport;
  intptr_t handle;
} Device;

typedef int (*NvmeTunnel) (Device  *dev,
                           uint8_t  opcode,
                           uint32_t nsid,
                           uint32_t cdw10,
                           void    *buf,
                           uint32_t len);

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static uint32_t
get_le32 (const uint8_t *p)
{
  return (uint32_t) p[0] | ((uint32_t) p[1] << 8) |
         ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}


static void
put_le32 (uint8_t *p,
          uint32_t v)
{
  p[0] = (uint8_t) v;
  p[1] = (uint8_t) (v >> 8);
  p[2] = (uint8_t) (v >> 16);
  p[3] = (uint8_t) (v >> 24);
}


static void
put_be24 (uint8_t *p,
          uint32_t v)
{
  p[0] = (uint8_t) (v >> 16);
  p[1] = (uint8_t) (v >> 8);
  p[2] = (uint8_t) v;
}


static uint64_t
le48 (const uint8_t *p)
{
  uint64_t v = 0;

  for (int i = 5; i >= 0; i--)
    v = (v << 8) | p[i];

  return v;
}


/* NVMe counters are 128-bit little endian; true when they fit in 64 bits. */
static bool
le128_to_u64 (const uint8_t *p,
              uint64_t      *out)
{
  uint64_t lo = 0;

  for (int i = 15; i >= 8; i--)
    if (p[i] != 0)
      return false;

  for (int i = 7; i >= 0; i--)
    lo = (lo << 8) | p[i];

  *out = lo;

  return true;
}


static bool
all_zero (const uint8_t *buf,
          size_t         len)
{
  for (size_t i = 0; i < len; i++)
    if (buf[i] != 0)
      return false;

  return true;
}


/*
 * Copy a string from a drive or from sysfs into a field of smart.ini: only
 * printable ASCII, nothing that is syntax in a key file ([ ] = \ ; newline),
 * leading and trailing blanks gone.
 */
static void
copy_clean (char       *dst,
            size_t      dst_len,
            const char *src,
            size_t      src_len)
{
  size_t n = 0;

  for (size_t i = 0; i < src_len && src[i] != '\0' && n + 1 < dst_len; i++)
    {
      unsigned char c = (unsigned char) src[i];

      if (n == 0 && c == ' ')
        continue;

      if (c < 0x20 || c > 0x7e || c == '[' || c == ']' || c == '=' ||
          c == '\\' || c == ';')
        c = '?';

      dst[n++] = (char) c;
    }

  while (n > 0 && dst[n - 1] == ' ')
    {
      n--;
    }

  dst[n] = '\0';
}


static bool
join_path (char       *out,
           size_t      out_len,
           const char *a,
           const char *b)
{
  int n = snprintf (out, out_len, "%s/%s", a, b);

  return n > 0 && (size_t) n < out_len;
}


/* A whole small text file, trailing white space stripped. -1 if unreadable. */
static int
read_text (const char *path,
           char       *buf,
           size_t      buf_len)
{
  int fd = open (path, O_RDONLY | O_CLOEXEC);
  ssize_t n;

  if (fd < 0)
    return -1;

  n = read (fd, buf, buf_len - 1);
  close (fd);

  if (n < 0)
    return -1;

  while (n > 0 && isspace ((unsigned char) buf[n - 1]))
    {
      n--;
    }
  buf[n] = '\0';

  return (int) n;
}


static bool
read_hex16 (const char *path,
            uint16_t   *out)
{
  char text[16];
  char *end;
  unsigned long value;

  if (read_text (path, text, sizeof text) <= 0)
    return false;

  errno = 0;
  value = strtoul (text, &end, 16);
  if (errno != 0 || *end != '\0' || value > 0xffff)
    return false;

  *out = (uint16_t) value;

  return true;
}


/* ------------------------------------------------------------------ */
/* One SCSI command                                                    */
/* ------------------------------------------------------------------ */

/*
 * Success only on a GOOD status with no host or driver error, no sense data
 * and some data back. A bridge that does not implement a command answers
 * CHECK CONDITION and leaves the buffer alone; parsing that buffer is how a
 * zero-filled table once read as a healthy drive.
 */
static int
scsi_good (Device          *dev,
           const uint8_t   *cdb,
           uint8_t          cdb_len,
           GsmScsiDirection direction,
           void            *buf,
           uint32_t         len)
{
  GsmScsiResult r;

  memset (&r, 0, sizeof r);

  if (dev->transport->scsi_cmd (dev->transport->user, dev->handle, cdb, cdb_len,
                                direction, buf, len, &r) != 0)
    return -1;

  if (r.scsi_status != 0 || r.host_status != 0 || r.driver_status != 0 ||
      r.sense_len != 0)
    return -1;

  if (len > 0 && r.resid > 0 && (uint32_t) r.resid >= len)
    return -1;

  return 0;
}


/* ------------------------------------------------------------------ */
/* NVMe through the vendor tunnels                                     */
/* ------------------------------------------------------------------ */

/*
 * The only two admin commands the reader ever builds: Identify Controller
 * (CNS 1) and Get Log Page, SMART / Health, 512 bytes. Anything else is a
 * bug, and refused here rather than sent to a bridge.
 */
static bool
nvme_command_allowed (uint8_t  opcode,
                      uint32_t cdw10,
                      uint32_t len)
{
  if (opcode == NVME_ADMIN_IDENTIFY)
    return cdw10 == 1 && len == NVME_IDENTIFY_LEN;

  if (opcode == NVME_ADMIN_GET_LOG_PAGE)
    return cdw10 == (NVME_LOG_SMART | (127u << 16)) && len == NVME_SMART_LOG_LEN;

  return false;
}


static int
tunnel_asmedia (Device  *dev,
                uint8_t  opcode,
                uint32_t nsid,
                uint32_t cdw10,
                void    *buf,
                uint32_t len)
{
  uint8_t cdb[16];
  uint32_t cdw10_hi = cdw10 >> 16;

  (void) nsid;   /* the bridge has no field for it */

  if (!nvme_command_allowed (opcode, cdw10, len))
    return -1;

  memset (cdb, 0, sizeof cdb);
  memset (buf, 0, len);

  cdb[0] = 0xE6;
  cdb[1] = opcode;
  cdb[3] = (uint8_t) cdw10;
  cdb[6] = (uint8_t) (cdw10_hi >> 8);
  cdb[7] = (uint8_t) cdw10_hi;

  return scsi_good (dev, cdb, sizeof cdb, GSM_SCSI_FROM_DEVICE, buf, len);
}


/*
 * Realtek returns the previous command's data, with no error, when asked
 * for more than 512 bytes, so the transfer is clamped and the tail stays
 * zero; both answers we read keep what we need in the first 512 bytes.
 */
static int
tunnel_realtek (Device  *dev,
                uint8_t  opcode,
                uint32_t nsid,
                uint32_t cdw10,
                void    *buf,
                uint32_t len)
{
  uint8_t cdb[16];
  uint32_t size = len > 0x200 ? 0x200 : len;

  (void) nsid;

  if (!nvme_command_allowed (opcode, cdw10, len))
    return -1;

  memset (cdb, 0, sizeof cdb);
  memset (buf, 0, len);

  cdb[0] = 0xE4;
  cdb[1] = (uint8_t) size;
  cdb[2] = (uint8_t) (size >> 8);
  cdb[3] = opcode;
  cdb[4] = (uint8_t) cdw10;

  return scsi_good (dev, cdb, sizeof cdb, GSM_SCSI_FROM_DEVICE, buf, size);
}


#define JMICRON_CDB_LEN         12
#define JMICRON_CMD_LEN         512
#define JMICRON_SIGNATURE       0x454d564eu   /* "NVME", little endian */
#define JMICRON_PROTO_CMD       0x0
#define JMICRON_PROTO_DMA_IN    0x2
#define JMICRON_PROTO_RESPONSE  0xF
#define JMICRON_ADMIN           0x80

/*
 * JMicron JMS58x/583: hand over the command, do the data phase, collect the
 * completion. The completion must carry the signature and a zero NVMe status
 * (completion dword 3 bits 31:17 sit in response dword 5); a bridge that
 * answers GOOD without running the command is otherwise believed.
 */
static int
tunnel_jmicron (Device  *dev,
                uint8_t  opcode,
                uint32_t nsid,
                uint32_t cdw10,
                void    *buf,
                uint32_t len)
{
  uint8_t cdb[JMICRON_CDB_LEN];
  uint8_t cmd[JMICRON_CMD_LEN];
  uint8_t reply[JMICRON_CMD_LEN];

  if (!nvme_command_allowed (opcode, cdw10, len))
    return -1;

  memset (cdb, 0, sizeof cdb);
  memset (cmd, 0, sizeof cmd);

  cdb[0] = 0xA1;
  cdb[1] = JMICRON_ADMIN | JMICRON_PROTO_CMD;
  put_be24 (&cdb[3], JMICRON_CMD_LEN);

  put_le32 (cmd + 0, JMICRON_SIGNATURE);
  put_le32 (cmd + 8, opcode);
  put_le32 (cmd + 12, nsid);
  put_le32 (cmd + 48, cdw10);

  if (scsi_good (dev, cdb, sizeof cdb, GSM_SCSI_TO_DEVICE, cmd, JMICRON_CMD_LEN) != 0)
    return -1;

  memset (cdb, 0, sizeof cdb);
  memset (buf, 0, len);

  cdb[0] = 0xA1;
  cdb[1] = JMICRON_ADMIN | JMICRON_PROTO_DMA_IN;
  put_be24 (&cdb[3], len);

  if (scsi_good (dev, cdb, sizeof cdb, GSM_SCSI_FROM_DEVICE, buf, len) != 0)
    return -1;

  memset (cdb, 0, sizeof cdb);
  memset (reply, 0, sizeof reply);

  cdb[0] = 0xA1;
  cdb[1] = JMICRON_ADMIN | JMICRON_PROTO_RESPONSE;
  put_be24 (&cdb[3], JMICRON_CMD_LEN);

  if (scsi_good (dev, cdb, sizeof cdb, GSM_SCSI_FROM_DEVICE, reply, JMICRON_CMD_LEN) != 0)
    return -1;

  if (get_le32 (reply) != JMICRON_SIGNATURE)
    return -1;

  if ((get_le32 (reply + 20) >> 17) != 0)
    return -1;

  return 0;
}


/* Indexed by GsmBridgeProtocol; SAT has no NVMe tunnel. */
static const NvmeTunnel nvme_tunnels[] = {
  [GSM_BRIDGE_SAT] = NULL,
  [GSM_BRIDGE_ASMEDIA] = tunnel_asmedia,
  [GSM_BRIDGE_JMICRON] = tunnel_jmicron,
  [GSM_BRIDGE_REALTEK] = tunnel_realtek,
};


/* ------------------------------------------------------------------ */
/* Reading and checking an NVMe drive                                  */
/* ------------------------------------------------------------------ */

static bool
text_has_printable (const uint8_t *p,
                    size_t         len)
{
  for (size_t i = 0; i < len; i++)
    if (p[i] > 0x20 && p[i] < 0x7f)
      return true;

  return false;
}


static void
figure_set (Figures *f,
            Figure   which,
            uint64_t value)
{
  f->value[which] = value;
  f->known |= 1u << which;
}


static bool
figure_known (const Figures *f,
              Figure         which)
{
  return (f->known & (1u << which)) != 0;
}


/*
 * Turns a SMART / Health log page into figures, or says why it is not
 * believable. Ranges: temperature 233-400 K (0 means no sensor, and is left
 * out), available spare at most 100, the critical-warning bits above bit 5
 * reserved and clear, counters that fit 64 bits and do not overflow when
 * converted to bytes, power-on hours below a century.
 */
static bool
nvme_log_to_figures (const uint8_t *log,
                     Figures       *out)
{
  uint64_t units_read, units_written, hours, unsafe, media;
  unsigned kelvin = (unsigned) (log[1] | (log[2] << 8));

  if (all_zero (log, NVME_SMART_LOG_LEN))
    return false;

  if (log[0] & 0xc0)
    return false;

  if (log[3] > 100)
    return false;

  if (kelvin != 0 && (kelvin < TEMPERATURE_MIN_KELVIN || kelvin > TEMPERATURE_MAX_KELVIN))
    return false;

  if (!le128_to_u64 (log + 32, &units_read) ||
      !le128_to_u64 (log + 48, &units_written) ||
      !le128_to_u64 (log + 128, &hours) ||
      !le128_to_u64 (log + 144, &unsafe) ||
      !le128_to_u64 (log + 160, &media))
    return false;

  if (units_read > UINT64_MAX / NVME_DATA_UNIT_BYTES ||
      units_written > UINT64_MAX / NVME_DATA_UNIT_BYTES)
    return false;

  if (hours > POWER_ON_HOURS_MAX)
    return false;

  memset (out, 0, sizeof *out);
  figure_set (out, FIG_BYTES_READ, units_read * NVME_DATA_UNIT_BYTES);
  figure_set (out, FIG_BYTES_WRITTEN, units_written * NVME_DATA_UNIT_BYTES);
  figure_set (out, FIG_PERCENT_USED, log[5]);
  figure_set (out, FIG_POWER_ON_HOURS, hours);
  figure_set (out, FIG_UNSAFE_SHUTDOWNS, unsafe);
  figure_set (out, FIG_MEDIA_ERRORS, media);
  if (kelvin != 0)
    figure_set (out, FIG_TEMPERATURE_KELVIN, kelvin);

  return true;
}


static GsmSmartStatus
read_nvme (Device          *dev,
           const GsmBridge *bridge,
           Drive           *drive,
           Figures         *figures)
{
  NvmeTunnel tunnel = nvme_tunnels[bridge->protocol];
  uint8_t id[NVME_IDENTIFY_LEN];
  uint8_t log[NVME_SMART_LOG_LEN];

  if (tunnel == NULL)
    return GSM_SMART_FAILED;

  if (tunnel (dev, NVME_ADMIN_IDENTIFY, 0, 1, id, NVME_IDENTIFY_LEN) != 0)
    return GSM_SMART_FAILED;

  /* Identify Controller: serial (bytes 4-23), model (24-63), VER (80-83). */
  if (!text_has_printable (id + 24, 40) || !text_has_printable (id + 4, 20) ||
      get_le32 (id + 80) == 0)
    return GSM_SMART_GARBLED;

  if (tunnel (dev, NVME_ADMIN_GET_LOG_PAGE, NVME_BROADCAST_NSID,
              NVME_LOG_SMART | (127u << 16), log, NVME_SMART_LOG_LEN) != 0)
    return GSM_SMART_FAILED;

  if (!nvme_log_to_figures (log, figures))
    return GSM_SMART_GARBLED;

  copy_clean (drive->serial, sizeof drive->serial, (const char *) id + 4, 20);
  copy_clean (drive->model, sizeof drive->model, (const char *) id + 24, 40);

  return GSM_SMART_OK;
}


/* ------------------------------------------------------------------ */
/* Reading and checking a SATA drive behind a SAT bridge               */
/* ------------------------------------------------------------------ */

static int
ata_pass_through_in (Device  *dev,
                     uint8_t  command,
                     uint8_t  features,
                     uint8_t  lba_mid,
                     uint8_t  lba_high,
                     uint8_t *out)
{
  uint8_t cdb[16];

  memset (cdb, 0, sizeof cdb);
  memset (out, 0, ATA_SECTOR_SIZE);

  cdb[0] = SG_ATA_PASSTHRU_16;
  cdb[1] = SG_ATA_PROTO_PIO_DATA_IN;
  cdb[2] = SG_ATA_FLAGS_IN;
  cdb[4] = features;
  cdb[6] = 1;
  cdb[10] = lba_mid;
  cdb[12] = lba_high;
  cdb[14] = command;

  return scsi_good (dev, cdb, sizeof cdb, GSM_SCSI_FROM_DEVICE, out, ATA_SECTOR_SIZE);
}


typedef enum
{
  POWER_UNKNOWN,
  POWER_STANDBY,
  POWER_AWAKE,
} PowerMode;

/*
 * ATA CHECK POWER MODE with CK_COND set: the device answers CHECK CONDITION,
 * sense key RECOVERED ERROR, ASC/ASCQ 00h/1Dh ("ATA PASS-THROUGH INFORMATION
 * AVAILABLE"), with the ATA return descriptor (09h) carrying the sector
 * count: 00h standby, 80h idle, FFh active or idle (ACS-4). Anything else,
 * including a bridge that refuses the command, is "unknown", and the caller
 * reads nothing more.
 */
static PowerMode
sat_power_mode (Device *dev)
{
  uint8_t cdb[16];
  GsmScsiResult r;
  size_t end;

  memset (cdb, 0, sizeof cdb);
  memset (&r, 0, sizeof r);

  cdb[0] = SG_ATA_PASSTHRU_16;
  cdb[1] = SG_ATA_PROTO_NON_DATA;
  cdb[2] = SG_ATA_CK_COND;
  cdb[14] = ATA_CHECK_POWER_MODE;

  if (dev->transport->scsi_cmd (dev->transport->user, dev->handle, cdb, sizeof cdb,
                                GSM_SCSI_NO_DATA, NULL, 0, &r) != 0)
    return POWER_UNKNOWN;

  if (r.host_status != 0 || (r.driver_status & ~0x08) != 0)
    return POWER_UNKNOWN;

  if (r.scsi_status != 0x02 || r.sense_len < 14)
    return POWER_UNKNOWN;

  if (r.sense[0] != SCSI_SENSE_DESCRIPTOR_CURRENT &&
      r.sense[0] != SCSI_SENSE_DESCRIPTOR_DEFERRED)
    return POWER_UNKNOWN;

  if ((r.sense[1] & 0x0f) != 0x01 || r.sense[2] != 0x00 || r.sense[3] != 0x1d)
    return POWER_UNKNOWN;

  end = 8u + r.sense[7];
  if (end > r.sense_len)
    end = r.sense_len;

  for (size_t pos = 8; pos + 6 <= end; pos += 2u + r.sense[pos + 1])
    {
      if (r.sense[pos] != SCSI_DESCRIPTOR_ATA_RETURN || r.sense[pos + 1] < 12)
        continue;

      return r.sense[pos + 5] == 0x00 ? POWER_STANDBY : POWER_AWAKE;
    }

  return POWER_UNKNOWN;
}


/* ATA string fields hold two characters per 16-bit word, high byte first. */
static void
ata_string (const uint8_t *id,
            int            word_off,
            int            word_len,
            char          *out,
            size_t         out_len)
{
  char raw[64];
  size_t n = 0;

  for (int i = 0; i < word_len && n + 2 < sizeof raw; i++)
    {
      const uint8_t *w = id + (word_off + i) * 2;

      raw[n++] = (char) w[1];
      raw[n++] = (char) w[0];
    }
  raw[n] = '\0';

  copy_clean (out, out_len, raw, n);
}


static bool
sector_checksum_is_zero (const uint8_t *sector)
{
  uint8_t sum = 0;

  for (size_t i = 0; i < ATA_SECTOR_SIZE; i++)
    sum = (uint8_t) (sum + sector[i]);

  return sum == 0;
}


/*
 * Figures from the SMART data structure: attribute 9 and 194 only, because
 * their meaning is the same across vendors. 12 attributes of 12 bytes start
 * at offset 2; a repeated id means the table is not one.
 */
static bool
ata_smart_to_figures (const uint8_t *data,
                      Figures       *out)
{
  bool seen[256] = {
    false
  };

  memset (out, 0, sizeof *out);

  for (int i = 0; i < ATA_MAX_ATTRS; i++)
    {
      const uint8_t *e = data + 2 + (i * 12);
      uint64_t raw;

      if (e[0] == 0)
        continue;

      if (seen[e[0]])
        return false;
      seen[e[0]] = true;

      raw = le48 (e + 5);

      if (e[0] == ATA_ATTR_POWER_ON_HOURS)
        figure_set (out, FIG_POWER_ON_HOURS, raw & 0xffffffffu);

      if (e[0] == ATA_ATTR_TEMPERATURE)
        {
          unsigned kelvin = (unsigned) (raw & 0xff) + 273u;

          if (raw & 0xff)
            {
              if (kelvin < TEMPERATURE_MIN_KELVIN || kelvin > TEMPERATURE_MAX_KELVIN)
                return false;
              figure_set (out, FIG_TEMPERATURE_KELVIN, kelvin);
            }
        }
    }

  if (figure_known (out, FIG_POWER_ON_HOURS) &&
      out->value[FIG_POWER_ON_HOURS] > POWER_ON_HOURS_MAX)
    return false;

  /* No attribute at all is not a drive that is fine; it is no table. */
  for (int i = 0; i < ATA_MAX_ATTRS; i++)
    if (data[2 + i * 12] != 0)
      return true;

  return false;
}


static GsmSmartStatus
read_sat (Device  *dev,
          Drive   *drive,
          Figures *figures)
{
  uint8_t ident[ATA_SECTOR_SIZE];
  uint8_t data[ATA_SECTOR_SIZE];
  PowerMode mode = sat_power_mode (dev);

  if (mode == POWER_STANDBY)
    return GSM_SMART_ASLEEP;

  if (mode == POWER_UNKNOWN)
    return GSM_SMART_FAILED;

  if (ata_pass_through_in (dev, ATA_IDENTIFY_DEVICE, 0, 0, 0, ident) != 0)
    return GSM_SMART_FAILED;

  /* Word 255: low byte A5h means the high byte is a checksum of the sector. */
  if (ident[510] == 0xA5 && !sector_checksum_is_zero (ident))
    return GSM_SMART_GARBLED;

  ata_string (ident, 27, 20, drive->model, sizeof drive->model);
  if (drive->model[0] == '\0')
    return GSM_SMART_GARBLED;
  ata_string (ident, 10, 10, drive->serial, sizeof drive->serial);

  if (ata_pass_through_in (dev, ATA_SMART, ATA_SMART_READ_DATA, 0x4f, 0xc2, data) != 0)
    return GSM_SMART_FAILED;

  if (!sector_checksum_is_zero (data) || !ata_smart_to_figures (data, figures))
    return GSM_SMART_GARBLED;

  return GSM_SMART_OK;
}


/* ------------------------------------------------------------------ */
/* The previous smart.ini (this program's own output)                  */
/* ------------------------------------------------------------------ */

static Drive *
snapshot_add (Snapshot   *s,
              const char *name)
{
  Drive *d;

  if (s->count >= MAX_DRIVES)
    return NULL;

  d = &s->drives[s->count++];
  memset (d, 0, sizeof *d);
  copy_clean (d->name, sizeof d->name, name, strlen (name));
  d->status = GSM_SMART_UNSUPPORTED;

  return d;
}


/*
 * The previous run's record of this same drive: the name, the USB id and the
 * USB serial must all agree. A name alone is not enough (the kernel hands
 * out sda again to whatever is plugged in next), and a drive that gives no
 * USB serial cannot be told from its successor, so it matches nothing.
 */
static const Drive *
snapshot_match (const Snapshot *s,
                const Drive    *d)
{
  if (d->usb_serial[0] == '\0')
    return NULL;

  for (size_t i = 0; i < s->count; i++)
    {
      const Drive *p = &s->drives[i];

      if (strcmp (p->name, d->name) == 0 && strcmp (p->usb_id, d->usb_id) == 0 &&
          strcmp (p->usb_serial, d->usb_serial) == 0)
        return p;
    }

  return NULL;
}


static bool
parse_u64 (const char *text,
           uint64_t   *out)
{
  char *end;

  if (*text < '0' || *text > '9')
    return false;

  errno = 0;
  *out = strtoull (text, &end, 10);

  return errno == 0 && *end == '\0';
}


static void
previous_apply_line (Snapshot *prev,
                     Drive   **current,
                     bool     *in_meta,
                     char     *line)
{
  char *eq;
  uint64_t number;

  if (line[0] == '[')
    {
      size_t n = strlen (line);

      *current = NULL;
      *in_meta = strcmp (line, "[meta]") == 0;

      if (strncmp (line, "[drive ", 7) == 0 && n > 8 && line[n - 1] == ']')
        {
          line[n - 1] = '\0';
          *current = snapshot_add (prev, line + 7);
        }
      return;
    }

  eq = strchr (line, '=');
  if (eq == NULL)
    return;
  *eq++ = '\0';

  if (*in_meta)
    {
      if (strcmp (line, "generated_at") == 0 && parse_u64 (eq, &number))
        prev->generated_at = (int64_t) number;
      return;
    }

  if (*current == NULL)
    return;

  if (strcmp (line, "status") == 0)
    {
      for (size_t i = 0; i < sizeof status_names / sizeof status_names[0]; i++)
        if (strcmp (eq, status_names[i]) == 0)
          (*current)->status = (GsmSmartStatus) i;
      return;
    }

  if (strcmp (line, "usb_id") == 0)
    {
      copy_clean ((*current)->usb_id, sizeof (*current)->usb_id, eq, strlen (eq));
      return;
    }

  if (strcmp (line, "usb_serial") == 0)
    {
      copy_clean ((*current)->usb_serial, sizeof (*current)->usb_serial, eq, strlen (eq));
      return;
    }

  if (strcmp (line, "last_ok_generated_at") == 0 && parse_u64 (eq, &number))
    {
      (*current)->last_ok_generated_at = (int64_t) number;
      return;
    }

  for (int f = 0; f < FIG_COUNT; f++)
    if (strcmp (line, figure_info[f].key) == 0 && parse_u64 (eq, &number))
      figure_set (&(*current)->figures, (Figure) f, number);
}


/*
 * Last run's records. Anything unreadable or odd is simply "no previous
 * value": the file is only a convenience for carrying figures over.
 */
static void
previous_load (const char *path,
               Snapshot   *prev)
{
  static char text[MAX_PREVIOUS_BYTES + 1];
  Drive *current = NULL;
  bool in_meta = false;
  ssize_t n;
  char *line;
  char *save = NULL;
  int fd = open (path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

  memset (prev, 0, sizeof *prev);

  if (fd < 0)
    return;

  n = read (fd, text, MAX_PREVIOUS_BYTES);
  close (fd);
  if (n <= 0)
    return;
  text[n] = '\0';

  for (line = strtok_r (text, "\n", &save); line != NULL; line = strtok_r (NULL, "\n", &save))
    previous_apply_line (prev, &current, &in_meta, line);

  /* A record's own last good time: when it was ok, that is the file's. */
  for (size_t i = 0; i < prev->count; i++)
    {
      Drive *d = &prev->drives[i];

      if (d->status == GSM_SMART_OK && d->last_ok_generated_at == 0)
        d->last_ok_generated_at = prev->generated_at;
    }
}


/* ------------------------------------------------------------------ */
/* Enumerating the USB disks                                           */
/* ------------------------------------------------------------------ */

static const char *const skipped_prefixes[] = {
  "loop", "ram", "zram", "dm-", "md", "nvme"
};

static bool
name_is_skipped (const char *name)
{
  if (name[0] == '.' || strlen (name) >= NAME_LEN)
    return true;

  for (size_t i = 0; i < sizeof skipped_prefixes / sizeof skipped_prefixes[0]; i++)
    if (strncmp (name, skipped_prefixes[i], strlen (skipped_prefixes[i])) == 0)
      return true;

  return false;
}


static int
compare_names (const void *a,
               const void *b)
{
  return strcmp ((const char *) a, (const char *) b);
}


static bool
runtime_status_is_suspended (const char *dir)
{
  char path[PATH_LEN];
  char text[32];

  if (!join_path (path, sizeof path, dir, "power/runtime_status"))
    return false;

  return read_text (path, text, sizeof text) > 0 && strcmp (text, "suspended") == 0;
}


/*
 * Walks up from the SCSI device to the nearest directory with idVendor: the
 * USB device. Never above the sysfs root, never more than USB_WALK_DEPTH
 * levels. Fills `usb_dir` and the ids.
 */
static bool
find_usb_device (const char *scsi_dir,
                 const char *root_real,
                 char       *usb_dir,
                 size_t      usb_dir_len,
                 uint16_t   *vendor,
                 uint16_t   *product,
                 uint16_t   *bcd_device)
{
  char dir[PATH_LEN];
  size_t root_len = strlen (root_real);

  if (realpath (scsi_dir, dir) == NULL)
    return false;

  for (int depth = 0; depth < USB_WALK_DEPTH; depth++)
    {
      char file[PATH_LEN];
      char *slash;

      if (strncmp (dir, root_real, root_len) != 0 || strlen (dir) <= root_len)
        return false;

      if (join_path (file, sizeof file, dir, "idVendor") && read_hex16 (file, vendor))
        {
          if (!join_path (file, sizeof file, dir, "idProduct") || !read_hex16 (file, product))
            return false;
          if (!join_path (file, sizeof file, dir, "bcdDevice") || !read_hex16 (file, bcd_device))
            return false;
          if (strlen (dir) >= usb_dir_len)
            return false;
          memcpy (usb_dir, dir, strlen (dir) + 1);

          return true;
        }

      slash = strrchr (dir, '/');
      if (slash == NULL)
        return false;
      *slash = '\0';
    }

  return false;
}


static bool
disk_is_scsi_disk (const char *scsi_dir)
{
  char path[PATH_LEN];
  char text[16];

  /* SCSI peripheral device type 0 is a direct-access block device. */
  return join_path (path, sizeof path, scsi_dir, "type") &&
         read_text (path, text, sizeof text) > 0 && strcmp (text, "0") == 0;
}


static void
read_sysfs_identity (Drive      *d,
                     const char *scsi_dir,
                     const char *usb_dir)
{
  char path[PATH_LEN];
  char text[TEXT_LEN];

  if (join_path (path, sizeof path, scsi_dir, "model") &&
      read_text (path, text, sizeof text) > 0)
    copy_clean (d->model, sizeof d->model, text, strlen (text));

  if (join_path (path, sizeof path, usb_dir, "serial") &&
      read_text (path, text, sizeof text) > 0)
    {
      copy_clean (d->serial, sizeof d->serial, text, strlen (text));
      copy_clean (d->usb_serial, sizeof d->usb_serial, text, strlen (text));
    }
}


/* ------------------------------------------------------------------ */
/* One drive                                                           */
/* ------------------------------------------------------------------ */

/* Counters below the last good value mean the answer is not the drive's. */
static bool
figures_regress (const Figures *now,
                 const Figures *before)
{
  for (int f = 0; f < FIG_COUNT; f++)
    {
      if (!figure_info[f].only_grows)
        continue;
      if (figure_known (now, (Figure) f) && figure_known (before, (Figure) f) &&
          now->value[f] < before->value[f])
        return true;
    }

  return false;
}


static void
drive_read (const GsmSmartRun *run,
            Drive             *d,
            const Drive       *previous)
{
  Device dev;
  Figures figures;
  GsmSmartStatus status;

  memset (&figures, 0, sizeof figures);

  if (d->bridge == NULL)
    {
      d->status = GSM_SMART_UNSUPPORTED;
      return;
    }

  if (d->asleep_by_pm)
    {
      d->status = GSM_SMART_ASLEEP;
      return;
    }

  dev.transport = run->transport;
  dev.handle = run->transport->open_device (run->transport->user, d->name);
  if (dev.handle < 0)
    {
      d->status = GSM_SMART_FAILED;
      return;
    }

  if (d->bridge->protocol == GSM_BRIDGE_SAT)
    status = read_sat (&dev, d, &figures);
  else
    status = read_nvme (&dev, d->bridge, d, &figures);

  run->transport->close_device (run->transport->user, dev.handle);

  if (status == GSM_SMART_OK && previous != NULL && figures_regress (&figures, &previous->figures))
    status = GSM_SMART_GARBLED;

  d->status = status;
  if (status == GSM_SMART_OK)
    d->figures = figures;
}


/* What carries over from last run into this one, whatever the status. */
static void
drive_apply_previous (Drive       *d,
                      const Drive *previous,
                      int64_t      now)
{
  if (d->status == GSM_SMART_OK)
    {
      d->last_ok_generated_at = now;
      return;
    }

  if (previous == NULL)
    return;

  d->last_ok_generated_at = previous->last_ok_generated_at;

  /* An asleep drive keeps the figures it last gave; the age says how old. */
  if (d->status == GSM_SMART_ASLEEP && d->last_ok_generated_at != 0)
    d->figures = previous->figures;
}


static void
drive_log_change (const Drive *d,
                  const Drive *previous)
{
  if (previous != NULL && previous->status == d->status)
    return;

  fprintf (stderr, "gsm-smart-reader: %s: %s%s%s\n", d->name,
           status_names[d->status],
           d->usb_id[0] != '\0' ? " usb " : "",
           d->usb_id);
}


/* ------------------------------------------------------------------ */
/* Output                                                              */
/* ------------------------------------------------------------------ */

static void
emit_drive (FILE        *f,
            const Drive *d)
{
  fprintf (f, "\n[drive %s]\n", d->name);
  if (d->model[0] != '\0')
    fprintf (f, "model=%s\n", d->model);
  if (d->serial[0] != '\0')
    fprintf (f, "serial=%s\n", d->serial);
  if (d->have_usb_id)
    {
      fprintf (f, "usb_id=%s\n", d->usb_id);
      if (d->usb_serial[0] != '\0')
        fprintf (f, "usb_serial=%s\n", d->usb_serial);
      fprintf (f, "bcd_device=%s\n", d->bcd_device);
    }
  if (d->bridge != NULL)
    {
      fprintf (f, "bridge=%s\n", gsm_bridge_protocol_name (d->bridge->protocol));
      fprintf (f, "basis=%s\n", gsm_bridge_basis_name (d->bridge->basis));
    }
  fprintf (f, "status=%s\n", status_names[d->status]);

  for (int i = 0; i < FIG_COUNT; i++)
    if (figure_known (&d->figures, (Figure) i))
      fprintf (f, "%s=%llu\n", figure_info[i].key, (unsigned long long) d->figures.value[i]);

  if (d->last_ok_generated_at != 0)
    fprintf (f, "last_ok_generated_at=%lld\n", (long long) d->last_ok_generated_at);
}


/*
 * mutate: ignore — creates, renames and unlinks files only under
 * run->out_dir, which tests point at a scratch directory.
 *
 * The new file is written next to the old one and renamed over it, so a
 * reader sees the old file or the new one. No fsync: the directory is RAM.
 */
static int
write_snapshot (const GsmSmartRun *run,
                const Snapshot    *s)
{
  char tmp_path[PATH_LEN];
  char final_path[PATH_LEN];
  FILE *f;
  int fd;
  bool ok;

  if (!join_path (tmp_path, sizeof tmp_path, run->out_dir, OUTPUT_TMP_FILE) ||
      !join_path (final_path, sizeof final_path, run->out_dir, OUTPUT_FILE))
    return -1;

  fd = open (tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
  if (fd < 0)
    return -1;

  if (fchmod (fd, 0644) != 0)
    {
      close (fd);
      unlink (tmp_path);
      return -1;
    }

  f = fdopen (fd, "w");
  if (f == NULL)
    {
      close (fd);
      unlink (tmp_path);
      return -1;
    }

  fprintf (f, "[meta]\nschema=%d\ngenerated_at=%lld\nreader_version=",
           GSM_SMART_SCHEMA, (long long) s->generated_at);
  {
    char version[TEXT_LEN];

    copy_clean (version, sizeof version, run->reader_version, strlen (run->reader_version));
    fprintf (f, "%s\n", version);
  }

  for (size_t i = 0; i < s->count; i++)
    emit_drive (f, &s->drives[i]);

  ok = fflush (f) == 0 && !ferror (f);
  ok = fclose (f) == 0 && ok;

  if (!ok || rename (tmp_path, final_path) != 0)
    {
      unlink (tmp_path);
      return -1;
    }

  return 0;
}


/* ------------------------------------------------------------------ */

int
gsm_smart_run (const GsmSmartRun *run)
{
  static Snapshot current;
  static Snapshot previous;
  char block_dir[PATH_LEN];
  char final_path[PATH_LEN];
  char root_real[PATH_MAX];
  char names[MAX_DRIVES][NAME_LEN];
  size_t n_names = 0;
  DIR *dir;
  struct dirent *entry;

  if (realpath (run->sys_root, root_real) == NULL ||
      !join_path (block_dir, sizeof block_dir, run->sys_root, "block") ||
      !join_path (final_path, sizeof final_path, run->out_dir, OUTPUT_FILE))
    return -1;

  previous_load (final_path, &previous);

  memset (&current, 0, sizeof current);
  current.generated_at = run->now_unix_s;

  dir = opendir (block_dir);
  if (dir != NULL)
    {
      while ((entry = readdir (dir)) != NULL && n_names < MAX_DRIVES)
        {
          if (!name_is_skipped (entry->d_name))
            memcpy (names[n_names++], entry->d_name, strlen (entry->d_name) + 1);
        }
      closedir (dir);
    }

  qsort (names, n_names, NAME_LEN, compare_names);

  for (size_t i = 0; i < n_names; i++)
    {
      char scsi_dir[PATH_LEN];
      char usb_dir[PATH_LEN];
      uint16_t vendor, product, bcd;
      Drive *d;
      const Drive *before;

      if (snprintf (scsi_dir, sizeof scsi_dir, "%s/%s/device", block_dir, names[i]) >= (int) sizeof scsi_dir)
        continue;

      if (!disk_is_scsi_disk (scsi_dir))
        continue;

      if (!find_usb_device (scsi_dir, root_real, usb_dir, sizeof usb_dir,
                            &vendor, &product, &bcd))
        continue;

      d = snapshot_add (&current, names[i]);
      if (d == NULL)
        break;

      d->have_usb_id = true;
      snprintf (d->usb_id, sizeof d->usb_id, "%04x:%04x", vendor, product);
      snprintf (d->bcd_device, sizeof d->bcd_device, "%04x", bcd);
      d->bridge = gsm_bridge_lookup (vendor, product, bcd);
      d->asleep_by_pm = runtime_status_is_suspended (usb_dir) ||
                        runtime_status_is_suspended (scsi_dir);
      read_sysfs_identity (d, scsi_dir, usb_dir);

      before = snapshot_match (&previous, d);
      drive_read (run, d, before);
      drive_apply_previous (d, before, run->now_unix_s);
      drive_log_change (d, before);
    }

  return write_snapshot (run, &current);
}
