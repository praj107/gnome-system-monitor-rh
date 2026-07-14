/*
 * gsm-smart — privileged SMART reader for GNOME System Monitor (RH fork)
 *
 * Runs as root via pkexec, reads SMART/health data for exactly one block
 * device, prints it to stdout as key=value lines, and exits. It never stays
 * resident and never accepts anything but a device path.
 *
 * Talks to the hardware directly rather than shelling out to smartctl, so
 * there is no runtime dependency on smartmontools:
 *
 *   - real NVMe            : NVME_IOCTL_ADMIN_CMD
 *   - SATA, and SAT bridges: SG_IO ATA PASS-THROUGH(16)
 *   - USB-NVMe bridges     : NVMe admin commands tunnelled in vendor SCSI CDBs
 *                            (ASMedia 0xE6, Realtek 0xE4, JMicron 0xA1)
 *   - anything else        : SCSI Informational Exceptions log page, which
 *                            yields a pass/fail verdict but no wear data
 *
 * The USB-NVMe tunnels matter because most modern portable SSDs are an NVMe
 * drive behind such a bridge: they answer neither the NVMe ioctl (the kernel
 * sees a SCSI disk) nor ATA passthrough (there is no ATA device in there).
 * The vendor CDB layouts below follow smartmontools' scsinvme.cpp, which is
 * GPL-2.0-or-later, as is this file.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <scsi/sg.h>

/* --- NVMe uapi (avoid depending on linux/nvme_ioctl.h being installed) --- */

struct gsm_nvme_passthru_cmd {
  uint8_t opcode;
  uint8_t flags;
  uint16_t rsvd1;
  uint32_t nsid;
  uint32_t cdw2;
  uint32_t cdw3;
  uint64_t metadata;
  uint64_t addr;
  uint32_t metadata_len;
  uint32_t data_len;
  uint32_t cdw10;
  uint32_t cdw11;
  uint32_t cdw12;
  uint32_t cdw13;
  uint32_t cdw14;
  uint32_t cdw15;
  uint32_t timeout_ms;
  uint32_t result;
};

#define GSM_NVME_IOCTL_ADMIN_CMD _IOWR ('N', 0x41, struct gsm_nvme_passthru_cmd)

#define NVME_ADMIN_IDENTIFY     0x06
#define NVME_ADMIN_GET_LOG_PAGE 0x02
#define NVME_LOG_SMART          0x02
#define NVME_BROADCAST_NSID     0xffffffffu

/* NVMe reports I/O in "data units" of 1000 * 512 bytes. */
#define NVME_DATA_UNIT_BYTES    (1000ULL * 512ULL)

#define NVME_IDENTIFY_LEN       4096
#define NVME_SMART_LOG_LEN      512

/* --- ATA --- */

#define ATA_IDENTIFY_DEVICE     0xEC
#define ATA_SMART               0xB0
#define ATA_SMART_READ_DATA     0xD0
#define ATA_SMART_READ_THRESH   0xD1

#define SG_ATA_PASSTHRU_16      0x85
#define SG_ATA_PROTO_PIO_DATA_IN (4 << 1)
/* T_DIR=1 (dev->host), BYT_BLOK=1, T_LENGTH=2 (in sector_count) */
#define SG_ATA_FLAGS_IN         0x2E

#define ATA_SECTOR_SIZE         512
#define ATA_MAX_ATTRS           30

/* --- SCSI --- */

#define SCSI_LOG_SENSE          0x4D
#define SCSI_LOG_PAGE_IE        0x2F   /* Informational Exceptions */

#define SG_TIMEOUT_MS           15000

#define SCSI_STATUS_GOOD        0x00

struct ata_attr {
  uint8_t id;
  uint8_t value;
  uint8_t worst;
  uint8_t threshold;
  bool has_threshold;
  uint64_t raw;
};

/* Which tunnel a USB bridge speaks. */
typedef enum {
  BRIDGE_UNKNOWN,
  BRIDGE_ASMEDIA,
  BRIDGE_REALTEK,
  BRIDGE_JMICRON,
} BridgeKind;

/* Carries one NVMe admin command through whichever tunnel we settled on. */
typedef int (*NvmeTunnel) (int       fd,
                           uint8_t   opcode,
                           uint32_t  nsid,
                           uint32_t  cdw10,
                           void     *buf,
                           uint32_t  len);

/* ------------------------------------------------------------------ */
/* Little helpers                                                      */
/* ------------------------------------------------------------------ */

static uint64_t
le48 (const uint8_t *p)
{
  uint64_t v = 0;

  for (int i = 5; i >= 0; i--)
    v = (v << 8) | p[i];

  return v;
}


/*
 * NVMe counters are 128-bit little-endian. Anything past 64 bits is far
 * beyond any real drive's lifetime, so fold the high half in only to detect
 * (and saturate on) overflow rather than silently truncating.
 */
static uint64_t
le128_saturating (const uint8_t *p)
{
  uint64_t lo = 0;

  for (int i = 7; i >= 0; i--)
    lo = (lo << 8) | p[i];

  for (int i = 8; i < 16; i++) {
    if (p[i] != 0)
      return UINT64_MAX;
  }

  return lo;
}


/* ATA string fields are byte-swapped within each 16-bit word. */
static void
ata_string (const uint8_t *id, int word_off, int word_len, char *out, size_t out_len)
{
  size_t n = 0;

  for (int i = 0; i < word_len && n + 2 < out_len; i++) {
    const uint8_t *w = id + (word_off + i) * 2;

    out[n++] = (char) w[1];
    out[n++] = (char) w[0];
  }
  out[n] = '\0';

  while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t'))
    out[--n] = '\0';
}


/* NVMe Identify strings are plain ASCII, space padded, not word-swapped. */
static void
nvme_string (const uint8_t *src, size_t len, char *out, size_t out_len)
{
  size_t n = len < out_len - 1 ? len : out_len - 1;

  memcpy (out, src, n);
  out[n] = '\0';

  while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\0'))
    out[--n] = '\0';
}


/*
 * Print a value with anything that could forge our own key=value framing
 * stripped out. The parent parses this stream, and a hostile model string
 * must not be able to inject extra keys.
 */
static void
print_str (const char *key, const char *val)
{
  printf ("%s=", key);
  for (const char *p = val; *p; p++) {
    unsigned char c = (unsigned char) *p;

    if (c == '\n' || c == '\r' || c < 0x20 || c == 0x7f)
      continue;
    putchar (c);
  }
  putchar ('\n');
}


static bool
all_zero (const uint8_t *buf, size_t len)
{
  for (size_t i = 0; i < len; i++) {
    if (buf[i] != 0)
      return false;
  }

  return true;
}


/* ------------------------------------------------------------------ */
/* SCSI                                                                */
/* ------------------------------------------------------------------ */

/*
 * One SG_IO round trip.
 *
 * Returns 0 ONLY on a GOOD SCSI status with no host/driver error. This is the
 * whole ballgame: a bridge that does not implement a command answers CHECK
 * CONDITION (0x02) with an ILLEGAL REQUEST sense and leaves the data buffer
 * untouched. Treating that as success — as an earlier version of this file did
 * — means parsing a zero-filled buffer and cheerfully reporting a drive with
 * no errors and no wear. A false clean bill of health is worse than no health
 * data at all, so anything short of GOOD is a hard failure here.
 */
static int
scsi_cmd (int            fd,
          const uint8_t *cdb,
          uint8_t        cdb_len,
          int            direction,
          void          *buf,
          uint32_t       len)
{
  sg_io_hdr_t io;
  uint8_t sense[32];

  memset (&io, 0, sizeof io);
  memset (sense, 0, sizeof sense);

  io.interface_id = 'S';
  io.dxfer_direction = direction;
  io.cmd_len = cdb_len;
  io.cmdp = (uint8_t *) cdb;
  io.sbp = sense;
  io.mx_sb_len = sizeof sense;
  io.dxfer_len = len;
  io.dxferp = buf;
  io.timeout = SG_TIMEOUT_MS;

  if (ioctl (fd, SG_IO, &io) < 0)
    return -1;

  if (io.status != SCSI_STATUS_GOOD)
    return -1;

  if (io.host_status != 0 || io.driver_status != 0)
    return -1;

  /* A short read means the device did not give us the structure we asked for,
   * and the tail would be uninitialised. */
  if (len > 0 && io.resid > 0 && (uint32_t) io.resid >= len)
    return -1;

  return 0;
}


/* ------------------------------------------------------------------ */
/* NVMe transports                                                     */
/* ------------------------------------------------------------------ */

/* Native NVMe: the kernel exposes an admin passthrough ioctl. */
static int
nvme_direct (int       fd,
             uint8_t   opcode,
             uint32_t  nsid,
             uint32_t  cdw10,
             void     *buf,
             uint32_t  len)
{
  struct gsm_nvme_passthru_cmd cmd;

  memset (&cmd, 0, sizeof cmd);
  cmd.opcode = opcode;
  cmd.nsid = nsid;
  cmd.addr = (uint64_t) (uintptr_t) buf;
  cmd.data_len = len;
  cmd.cdw10 = cdw10;
  cmd.timeout_ms = SG_TIMEOUT_MS;

  return ioctl (fd, GSM_NVME_IOCTL_ADMIN_CMD, &cmd);
}


/*
 * ASMedia (ASM236x and friends) — a single vendor CDB carrying the admin
 * opcode and CDW10. This is what the Crucial/Micron X9 speaks.
 */
static int
nvme_asmedia (int       fd,
              uint8_t   opcode,
              uint32_t  nsid,
              uint32_t  cdw10,
              void     *buf,
              uint32_t  len)
{
  uint8_t cdb[16];
  uint32_t cdw10_hi = cdw10 >> 16;

  (void) nsid;   /* the bridge derives it; there is no field for it */

  memset (cdb, 0, sizeof cdb);
  memset (buf, 0, len);

  cdb[0] = 0xE6;
  cdb[1] = opcode;
  cdb[3] = (uint8_t) cdw10;
  cdb[6] = (uint8_t) (cdw10_hi >> 8);
  cdb[7] = (uint8_t) cdw10_hi;
  /* cdw13 and cdw12 are big-endian in the tail; we never set either. */

  return scsi_cmd (fd, cdb, sizeof cdb, SG_DXFER_FROM_DEV, buf, len);
}


/* Realtek (RTL9210 and friends). Cannot return more than 0x200 bytes. */
static int
nvme_realtek (int       fd,
              uint8_t   opcode,
              uint32_t  nsid,
              uint32_t  cdw10,
              void     *buf,
              uint32_t  len)
{
  uint8_t cdb[16];
  uint32_t size = len;

  (void) nsid;

  /* Asking for more than this returns stale data from the previous command
   * rather than an error, so clamp instead and leave the tail zeroed. */
  if (size > 0x200)
    size = 0x200;

  memset (cdb, 0, sizeof cdb);
  memset (buf, 0, len);

  cdb[0] = 0xE4;
  cdb[1] = (uint8_t) size;
  cdb[2] = (uint8_t) (size >> 8);
  cdb[3] = opcode;
  cdb[4] = (uint8_t) cdw10;

  return scsi_cmd (fd, cdb, sizeof cdb, SG_DXFER_FROM_DEV, buf, size);
}


/*
 * JMicron (JMS583 and friends) — three SCSI commands: hand over the NVMe
 * command, do the data phase, then collect the completion.
 */
#define JMICRON_CDB_LEN     12
#define JMICRON_CMD_LEN     512
#define JMICRON_SIGNATURE   0x454d564eu   /* "NVME" */

#define JMICRON_PROTO_CMD      0x0
#define JMICRON_PROTO_DMA_IN   0x2
#define JMICRON_PROTO_RESPONSE 0xF
#define JMICRON_ADMIN          0x80

static void
jmicron_put_be24 (uint8_t *p, uint32_t value)
{
  p[0] = (uint8_t) (value >> 16);
  p[1] = (uint8_t) (value >> 8);
  p[2] = (uint8_t) value;
}


static int
nvme_jmicron (int       fd,
              uint8_t   opcode,
              uint32_t  nsid,
              uint32_t  cdw10,
              void     *buf,
              uint32_t  len)
{
  uint8_t cdb[JMICRON_CDB_LEN];
  uint32_t cmd[JMICRON_CMD_LEN / sizeof (uint32_t)];
  uint32_t reply[JMICRON_CMD_LEN / sizeof (uint32_t)];

  /* 1. the NVMe command itself */
  memset (cdb, 0, sizeof cdb);
  memset (cmd, 0, sizeof cmd);

  cdb[0] = 0xA1;
  cdb[1] = JMICRON_ADMIN | JMICRON_PROTO_CMD;
  jmicron_put_be24 (&cdb[3], JMICRON_CMD_LEN);

  cmd[0] = JMICRON_SIGNATURE;
  cmd[2] = opcode;
  cmd[3] = nsid;
  cmd[12] = cdw10;

  if (scsi_cmd (fd, cdb, sizeof cdb, SG_DXFER_TO_DEV, cmd, JMICRON_CMD_LEN) != 0)
    return -1;

  /* 2. the data phase */
  memset (cdb, 0, sizeof cdb);
  memset (buf, 0, len);

  cdb[0] = 0xA1;
  cdb[1] = JMICRON_ADMIN | JMICRON_PROTO_DMA_IN;
  jmicron_put_be24 (&cdb[3], len);

  if (scsi_cmd (fd, cdb, sizeof cdb, SG_DXFER_FROM_DEV, buf, len) != 0)
    return -1;

  /* 3. the completion. We do not decode it, but a bridge that refuses this
   * phase did not really run the command, so the data above is not to be
   * trusted. */
  memset (cdb, 0, sizeof cdb);
  memset (reply, 0, sizeof reply);

  cdb[0] = 0xA1;
  cdb[1] = JMICRON_ADMIN | JMICRON_PROTO_RESPONSE;
  jmicron_put_be24 (&cdb[3], JMICRON_CMD_LEN);

  if (scsi_cmd (fd, cdb, sizeof cdb, SG_DXFER_FROM_DEV, reply, JMICRON_CMD_LEN) != 0)
    return -1;

  return 0;
}


/* ------------------------------------------------------------------ */
/* NVMe commands, over whichever tunnel                                */
/* ------------------------------------------------------------------ */

static int
nvme_identify_controller (NvmeTunnel tunnel, int fd, uint8_t *out)
{
  /* CNS=1: identify the controller. */
  return tunnel (fd, NVME_ADMIN_IDENTIFY, 0, 1, out, NVME_IDENTIFY_LEN);
}


static int
nvme_get_smart_log (NvmeTunnel tunnel, int fd, uint8_t *out)
{
  /* NUMD is the number of dwords, zero based: 512/4 - 1 = 127. */
  uint32_t cdw10 = NVME_LOG_SMART | (127u << 16);

  return tunnel (fd, NVME_ADMIN_GET_LOG_PAGE, NVME_BROADCAST_NSID, cdw10,
                 out, NVME_SMART_LOG_LEN);
}


/*
 * A bridge can answer GOOD to a command it silently ignored. Identify Controller
 * always carries a printable model number, so an all-zero or unprintable buffer
 * means we were not really talking to the drive — refuse it rather than report
 * a nameless, wearless, perfectly healthy disk.
 */
static bool
nvme_identify_looks_real (const uint8_t *id)
{
  const uint8_t *model = id + 24;

  if (all_zero (id, NVME_IDENTIFY_LEN))
    return false;

  for (int i = 0; i < 40; i++) {
    if (model[i] == ' ')
      continue;
    if (isprint (model[i]))
      return true;
  }

  return false;
}


static void
print_nvme_smart (const uint8_t *log)
{
  uint16_t kelvin = (uint16_t) (log[1] | (log[2] << 8));

  printf ("smart_ok=1\n");
  printf ("critical_warning=%u\n", log[0]);
  if (kelvin > 0)
    printf ("temperature_c=%d\n", (int) kelvin - 273);
  printf ("available_spare=%u\n", log[3]);
  printf ("available_spare_threshold=%u\n", log[4]);
  printf ("percent_used=%u\n", log[5]);
  printf ("data_units_read_bytes=%llu\n",
          (unsigned long long) (le128_saturating (log + 32) * NVME_DATA_UNIT_BYTES));
  printf ("data_units_written_bytes=%llu\n",
          (unsigned long long) (le128_saturating (log + 48) * NVME_DATA_UNIT_BYTES));
  printf ("power_cycles=%llu\n",
          (unsigned long long) le128_saturating (log + 112));
  printf ("power_on_hours=%llu\n",
          (unsigned long long) le128_saturating (log + 128));
  printf ("unsafe_shutdowns=%llu\n",
          (unsigned long long) le128_saturating (log + 144));
  printf ("media_errors=%llu\n",
          (unsigned long long) le128_saturating (log + 160));

  /* The controller's own verdict: spare exhausted (bit 0), reliability
   * degraded (bit 2), or media gone read-only (bit 3). */
  printf ("smart_failing=%d\n", (log[0] & 0x0d) ? 1 : 0);
}


static int
read_nvme (NvmeTunnel tunnel, int fd)
{
  uint8_t id[NVME_IDENTIFY_LEN];
  uint8_t log[NVME_SMART_LOG_LEN];
  char buf[64];

  if (nvme_identify_controller (tunnel, fd, id) != 0)
    return 1;

  if (!nvme_identify_looks_real (id))
    return 1;

  if (nvme_get_smart_log (tunnel, fd, log) != 0)
    return 1;

  /* The SMART log legitimately contains zeros on a factory-fresh drive, but
   * never *all* zeros — power-on hours and temperature are always populated. */
  if (all_zero (log, NVME_SMART_LOG_LEN))
    return 1;

  print_str ("kind", "nvme");

  nvme_string (id + 4, 20, buf, sizeof buf);
  print_str ("serial", buf);
  nvme_string (id + 24, 40, buf, sizeof buf);
  print_str ("model", buf);
  nvme_string (id + 64, 8, buf, sizeof buf);
  print_str ("firmware", buf);

  print_nvme_smart (log);

  return 0;
}


/* ------------------------------------------------------------------ */
/* ATA / SAT                                                           */
/* ------------------------------------------------------------------ */

static int
ata_pass_through (int fd, uint8_t command, uint8_t features,
                  uint8_t lba_mid, uint8_t lba_high, uint8_t *out)
{
  uint8_t cdb[16];

  memset (cdb, 0, sizeof cdb);
  memset (out, 0, ATA_SECTOR_SIZE);

  cdb[0] = SG_ATA_PASSTHRU_16;
  cdb[1] = SG_ATA_PROTO_PIO_DATA_IN;
  cdb[2] = SG_ATA_FLAGS_IN;
  cdb[4] = features;
  cdb[6] = 1;                    /* one 512-byte block */
  cdb[10] = lba_mid;
  cdb[12] = lba_high;
  cdb[14] = command;

  return scsi_cmd (fd, cdb, sizeof cdb, SG_DXFER_FROM_DEV, out, ATA_SECTOR_SIZE);
}


static int
read_ata (int fd)
{
  uint8_t data[ATA_SECTOR_SIZE];
  uint8_t thresh[ATA_SECTOR_SIZE];
  uint8_t ident[ATA_SECTOR_SIZE];
  struct ata_attr attrs[ATA_MAX_ATTRS];
  int n_attrs = 0;
  bool have_ident;
  bool have_thresh;
  bool failing = false;
  char buf[64];

  /* IDENTIFY first: it is the cheapest way to find out whether there is an
   * ATA device behind this bridge at all. */
  have_ident = ata_pass_through (fd, ATA_IDENTIFY_DEVICE, 0, 0, 0, ident) == 0;
  if (!have_ident || all_zero (ident, ATA_SECTOR_SIZE))
    return 1;

  if (ata_pass_through (fd, ATA_SMART, ATA_SMART_READ_DATA,
                        0x4f, 0xc2, data) != 0)
    return 1;

  /* Attribute table starts at offset 2; 30 entries of 12 bytes. An all-zero
   * table means SMART is not really there. */
  for (int i = 0; i < ATA_MAX_ATTRS; i++) {
    const uint8_t *e = data + 2 + (i * 12);

    if (e[0] == 0)
      continue;                   /* id 0 = unused slot */

    struct ata_attr *a = &attrs[n_attrs++];

    a->id = e[0];
    a->value = e[3];
    a->worst = e[4];
    a->raw = le48 (e + 5);
    a->threshold = 0;
    a->has_threshold = false;
  }

  if (n_attrs == 0)
    return 1;

  have_thresh = ata_pass_through (fd, ATA_SMART, ATA_SMART_READ_THRESH,
                                  0x4f, 0xc2, thresh) == 0;

  if (have_thresh) {
    for (int i = 0; i < n_attrs; i++) {
      for (int j = 0; j < ATA_MAX_ATTRS; j++) {
        const uint8_t *t = thresh + 2 + (j * 12);

        if (t[0] == attrs[i].id) {
          attrs[i].threshold = t[1];
          attrs[i].has_threshold = true;
          break;
        }
      }
    }
  }

  print_str ("kind", "ata");

  ata_string (ident, 27, 20, buf, sizeof buf);   /* model,    words 27-46 */
  print_str ("model", buf);
  ata_string (ident, 10, 10, buf, sizeof buf);   /* serial,   words 10-19 */
  print_str ("serial", buf);
  ata_string (ident, 23, 4, buf, sizeof buf);    /* firmware, words 23-26 */
  print_str ("firmware", buf);

  printf ("smart_ok=1\n");

  for (int i = 0; i < n_attrs; i++) {
    struct ata_attr *a = &attrs[i];

    printf ("attr.%u.value=%u\n", a->id, a->value);
    printf ("attr.%u.worst=%u\n", a->id, a->worst);
    printf ("attr.%u.raw=%llu\n", a->id, (unsigned long long) a->raw);

    if (a->has_threshold) {
      printf ("attr.%u.threshold=%u\n", a->id, a->threshold);

      /* A threshold of 0 means "no failure level defined", not "always fails". */
      if (a->threshold > 0 && a->value <= a->threshold)
        failing = true;
    }

    /* Lift the attributes with a stable cross-vendor meaning into the same key
     * namespace the NVMe path uses, so the caller need not know ATA numbering. */
    switch (a->id) {
      case 9:                     /* Power_On_Hours */
        printf ("power_on_hours=%llu\n", (unsigned long long) (a->raw & 0xffffffff));
        break;
      case 12:                    /* Power_Cycle_Count */
        printf ("power_cycles=%llu\n", (unsigned long long) (a->raw & 0xffffffff));
        break;
      case 194:                   /* Temperature_Celsius */
        printf ("temperature_c=%llu\n", (unsigned long long) (a->raw & 0xff));
        break;
      case 231:                   /* SSD_Life_Left (normalised: 100 -> 0) */
      case 202:                   /* Percent_Lifetime_Remain (Micron/Crucial) */
        printf ("percent_used=%u\n", a->value <= 100 ? 100 - a->value : 0);
        break;
      case 241:                   /* Total_LBAs_Written */
        printf ("data_units_written_bytes=%llu\n",
                (unsigned long long) (a->raw * 512ULL));
        break;
      case 242:                   /* Total_LBAs_Read */
        printf ("data_units_read_bytes=%llu\n",
                (unsigned long long) (a->raw * 512ULL));
        break;
      case 246:                   /* Total_Host_Sector_Write (Micron/Crucial) */
        printf ("host_sectors_written=%llu\n", (unsigned long long) a->raw);
        break;
      default:
        break;
    }
  }

  printf ("smart_failing=%d\n", failing ? 1 : 0);

  return 0;
}


/* ------------------------------------------------------------------ */
/* Plain SCSI: a verdict, but no wear                                  */
/* ------------------------------------------------------------------ */

/*
 * The Informational Exceptions log page is the SCSI equivalent of "is this
 * disk about to die": it carries an ASC/ASCQ pair and, usually, a temperature.
 * It says nothing about wear or bytes written — but a drive's own failure
 * prediction is the single most valuable thing here, and it costs one command.
 */
static int
read_scsi_health (int fd)
{
  uint8_t cdb[10];
  uint8_t page[64];
  uint16_t page_len;

  memset (cdb, 0, sizeof cdb);
  memset (page, 0, sizeof page);

  cdb[0] = SCSI_LOG_SENSE;
  cdb[2] = 0x40 | SCSI_LOG_PAGE_IE;   /* PC=01b (current cumulative values) */
  cdb[7] = (uint8_t) (sizeof page >> 8);
  cdb[8] = (uint8_t) (sizeof page);

  if (scsi_cmd (fd, cdb, sizeof cdb, SG_DXFER_FROM_DEV, page, sizeof page) != 0)
    return 1;

  page_len = (uint16_t) ((page[2] << 8) | page[3]);
  if (page_len < 4)
    return 1;

  /* Page header is 4 bytes, then a parameter: 4-byte header, then ASC, ASCQ,
   * and (optionally) the most recent temperature. */
  {
    const uint8_t *param = page + 4;
    uint8_t param_len = param[3];
    uint8_t asc, ascq;

    if (param_len < 2)
      return 1;

    asc = param[4];
    ascq = param[5];

    print_str ("kind", "scsi");
    printf ("smart_ok=1\n");

    /* ASC 0x5D is "failure prediction threshold exceeded" — the drive telling
     * us it expects to fail. Anything nonzero is worth surfacing. */
    printf ("smart_failing=%d\n", asc != 0 ? 1 : 0);
    printf ("scsi_asc=%u\n", asc);
    printf ("scsi_ascq=%u\n", ascq);

    if (param_len >= 3 && param[6] != 0xff && param[6] != 0)
      printf ("temperature_c=%u\n", param[6]);

    /* Be explicit that this transport cannot tell us about wear, so the UI can
     * say so rather than implying a drive with no writes. */
    printf ("wear_unavailable=1\n");
  }

  return 0;
}


/* ------------------------------------------------------------------ */
/* Which bridge is this?                                               */
/* ------------------------------------------------------------------ */

static bool
read_sysfs_hex (const char *path, unsigned *out)
{
  FILE *f = fopen (path, "r");
  unsigned value;

  if (f == NULL)
    return false;

  if (fscanf (f, "%x", &value) != 1) {
    fclose (f);
    return false;
  }

  fclose (f);
  *out = value;

  return true;
}


/*
 * Walk up the sysfs device tree from the block device until we find the USB
 * device node (the one carrying idVendor/idProduct).
 */
static bool
find_usb_ids (const char *name, unsigned *vendor, unsigned *product)
{
  char path[PATH_MAX];
  char resolved[PATH_MAX];
  /* Room for the longest attribute name we append below. */
  char candidate[PATH_MAX + 16];

  snprintf (path, sizeof path, "/sys/block/%s/device", name);

  if (realpath (path, resolved) == NULL)
    return false;

  for (int depth = 0; depth < 12; depth++) {
    unsigned v, p;
    char *slash;

    snprintf (candidate, sizeof candidate, "%s/idVendor", resolved);

    if (read_sysfs_hex (candidate, &v)) {
      snprintf (candidate, sizeof candidate, "%s/idProduct", resolved);

      if (!read_sysfs_hex (candidate, &p))
        return false;

      *vendor = v;
      *product = p;

      return true;
    }

    slash = strrchr (resolved, '/');
    if (slash == NULL || slash == resolved)
      return false;

    *slash = '\0';
  }

  return false;
}


/*
 * Known USB-NVMe bridges.
 *
 * We match the enclosure before speaking to it, because these are
 * vendor-specific SCSI opcodes: 0xE6 means "NVMe passthrough" to an ASMedia
 * bridge and could mean something else entirely to a different one. Sending a
 * misidentified vendor command to a device holding someone's data is not a
 * risk worth taking for a monitoring feature, so an unlisted bridge is probed
 * only with commands that are read-only under every interpretation.
 */
static BridgeKind
bridge_for_usb_id (unsigned vendor, unsigned product)
{
  switch (vendor) {
    case 0x174c:              /* ASMedia */
      return BRIDGE_ASMEDIA;
    case 0x0634:              /* Micron — the X9 family uses an ASMedia bridge */
      return BRIDGE_ASMEDIA;
    case 0x0bda:              /* Realtek */
      return BRIDGE_REALTEK;
    case 0x152d:              /* JMicron */
      return BRIDGE_JMICRON;
    default:
      break;
  }

  (void) product;

  return BRIDGE_UNKNOWN;
}


static const char *
bridge_name (BridgeKind kind)
{
  switch (kind) {
    case BRIDGE_ASMEDIA: return "asmedia";
    case BRIDGE_REALTEK: return "realtek";
    case BRIDGE_JMICRON: return "jmicron";
    case BRIDGE_UNKNOWN:
    default:             return "unknown";
  }
}


static NvmeTunnel
tunnel_for_bridge (BridgeKind kind)
{
  switch (kind) {
    case BRIDGE_ASMEDIA: return nvme_asmedia;
    case BRIDGE_REALTEK: return nvme_realtek;
    case BRIDGE_JMICRON: return nvme_jmicron;
    case BRIDGE_UNKNOWN:
    default:             return NULL;
  }
}


/* ------------------------------------------------------------------ */

/*
 * We are root here. The only thing we accept from the caller is a device path,
 * so it has to be pinned down hard: a bare name directly under /dev, no
 * traversal, no symlinks, and it must actually be a block device.
 */
static bool
device_path_is_acceptable (const char *path)
{
  const char *name;

  if (strncmp (path, "/dev/", 5) != 0)
    return false;

  name = path + 5;

  if (*name == '\0' || strlen (name) > 32)
    return false;

  for (const char *p = name; *p; p++) {
    if (!isalnum ((unsigned char) *p) && *p != '-' && *p != '_')
      return false;
  }

  return true;
}


int
main (int argc, char **argv)
{
  struct stat st;
  const char *name;
  unsigned vendor = 0, product = 0;
  BridgeKind bridge = BRIDGE_UNKNOWN;
  int fd;

  if (argc != 2) {
    fprintf (stderr, "usage: gsm-smart /dev/DEVICE\n");
    return 2;
  }

  if (!device_path_is_acceptable (argv[1])) {
    fprintf (stderr, "gsm-smart: refusing device path '%s'\n", argv[1]);
    return 2;
  }

  /* O_NOFOLLOW: the path passed validation, but /dev is writable by root and
   * we will not be talked into following a symlink out of it. */
  fd = open (argv[1], O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    fprintf (stderr, "gsm-smart: cannot open %s: %s\n", argv[1], strerror (errno));
    return 1;
  }

  if (fstat (fd, &st) != 0 || !S_ISBLK (st.st_mode)) {
    fprintf (stderr, "gsm-smart: %s is not a block device\n", argv[1]);
    close (fd);
    return 2;
  }

  name = argv[1] + 5;

  print_str ("device", argv[1]);

  /* 1. A real NVMe device: the kernel gives us the admin ioctl directly. */
  if (strncmp (name, "nvme", 4) == 0) {
    int ret = read_nvme (nvme_direct, fd);

    close (fd);

    if (ret != 0)
      fprintf (stderr, "gsm-smart: NVMe SMART log unavailable\n");

    return ret;
  }

  /* 2. A SCSI disk. It may be SATA (SAT), or an NVMe drive hiding behind a
   *    USB bridge — most modern portable SSDs are the latter. */
  if (find_usb_ids (name, &vendor, &product)) {
    bridge = bridge_for_usb_id (vendor, product);
    printf ("usb_id=%04x:%04x\n", vendor, product);
    print_str ("bridge", bridge_name (bridge));
  }

  /* Try SAT first: it is standardised, and a plain SATA drive or SATA bridge
   * will answer it. It is also harmless to a bridge that does not implement
   * it — the command is rejected, and scsi_cmd now treats that as a failure
   * rather than as an empty success. */
  if (read_ata (fd) == 0) {
    close (fd);
    return 0;
  }

  /* Then the NVMe tunnel this enclosure is known to speak. */
  if (bridge != BRIDGE_UNKNOWN) {
    NvmeTunnel tunnel = tunnel_for_bridge (bridge);

    if (tunnel != NULL && read_nvme (tunnel, fd) == 0) {
      close (fd);
      return 0;
    }
  }

  /*
   * An unlisted bridge. Every tunnel below issues only Identify Controller and
   * Get Log Page — reads, under any reading of the opcode — and each is
   * rejected outright by a bridge that does not implement it. We still prefer
   * not to guess, so this only runs when the allowlist had nothing to say.
   */
  if (bridge == BRIDGE_UNKNOWN) {
    const NvmeTunnel probes[] = { nvme_asmedia, nvme_realtek, nvme_jmicron };
    const char *names[] = { "asmedia", "realtek", "jmicron" };

    for (size_t i = 0; i < sizeof probes / sizeof probes[0]; i++) {
      if (read_nvme (probes[i], fd) == 0) {
        print_str ("bridge_detected", names[i]);
        close (fd);
        return 0;
      }
    }
  }

  /* 3. Nothing understood the drive. Fall back to the drive's own pass/fail,
   *    which is worth having even without wear figures. */
  if (read_scsi_health (fd) == 0) {
    close (fd);
    return 0;
  }

  fprintf (stderr, "gsm-smart: no supported way to read health from %s\n", argv[1]);
  close (fd);

  return 1;
}
