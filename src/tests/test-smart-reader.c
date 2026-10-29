/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The SMART reader's core against fake sysfs trees and a fake SCSI transport.
 * Nothing here opens a device node. Every serial number, model and counter is
 * made up. The fake drive answers only the CDBs of the protocol it is set up
 * for and says CHECK CONDITION to any other, so a test that sent the wrong
 * tunnel sees a failure, and every CDB sent is recorded.
 */

#include <glib.h>
#include <glib/gstdio.h>

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gsm-smart-bridges.h"
#include "gsm-smart-reader-core.h"

/* ------------------------------------------------------------------ */
/* Scratch directories                                                 */
/* ------------------------------------------------------------------ */

typedef struct
{
  char *base;
  char *sys;
  char *out;
} Env;


/*
 * Removes a tree this test created. Symlinks are removed as links, never
 * followed, and nothing outside `path` is touched.
 */
static void
remove_tree (const char *path)
{
  if (!g_file_test (path, G_FILE_TEST_IS_SYMLINK) &&
      g_file_test (path, G_FILE_TEST_IS_DIR))
    {
      GDir *dir;
      const char *name;

      g_chmod (path, 0700);
      dir = g_dir_open (path, 0, NULL);
      while (dir != NULL && (name = g_dir_read_name (dir)) != NULL)
        {
          char *child = g_build_filename (path, name, NULL);

          remove_tree (child);
          g_free (child);
        }
      if (dir != NULL)
        g_dir_close (dir);
    }

  g_remove (path);
}


static Env *
env_new (void)
{
  Env *e = g_new0 (Env, 1);
  const char *runtime = g_get_user_runtime_dir ();
  char *template;

  /* RAM when there is a runtime directory; the temp directory otherwise. */
  if (runtime == NULL || !g_file_test (runtime, G_FILE_TEST_IS_DIR))
    runtime = g_get_tmp_dir ();

  template = g_build_filename (runtime, "gsm-smart-reader-test-XXXXXX", NULL);
  e->base = g_mkdtemp (template);
  g_assert_nonnull (e->base);
  e->sys = g_build_filename (e->base, "sys", NULL);
  e->out = g_build_filename (e->base, "run", NULL);
  g_assert_cmpint (g_mkdir_with_parents (e->sys, 0755), ==, 0);
  g_assert_cmpint (g_mkdir_with_parents (e->out, 0755), ==, 0);

  return e;
}


static void
env_free (Env *e)
{
  g_assert_true (g_str_has_prefix (e->base, g_get_user_runtime_dir ()) ||
                 g_str_has_prefix (e->base, g_get_tmp_dir ()));
  remove_tree (e->base);
  g_free (e->base);
  g_free (e->sys);
  g_free (e->out);
  g_free (e);
}


static void
write_file (const char *root,
            const char *relative,
            const char *content)
{
  char *path = g_build_filename (root, relative, NULL);
  char *dir = g_path_get_dirname (path);

  g_assert_cmpint (g_mkdir_with_parents (dir, 0755), ==, 0);
  g_assert_true (g_file_set_contents (path, content, -1, NULL));
  g_free (dir);
  g_free (path);
}


/*
 * A SCSI disk `name` behind USB device 3-`port`, laid out as sysfs does:
 * /sys/block/NAME/device -> the SCSI device, which sits below the USB device
 * that carries idVendor, idProduct and bcdDevice.
 */
static void
add_usb_disk (Env        *e,
              const char *name,
              int         port,
              unsigned    vendor,
              unsigned    product,
              unsigned    bcd,
              const char *model,
              const char *serial,
              const char *runtime_status)
{
  char *usb = g_strdup_printf ("devices/pci0000:00/usb3/3-%d", port);
  char *scsi = g_strdup_printf ("%s/3-%d:1.0/host6/target6:0:0/6:0:0:0", usb, port);
  char *rel;
  char *link_target;
  char *link_path;
  char *text;

  text = g_strdup_printf ("%04x\n", vendor);
  rel = g_build_filename (usb, "idVendor", NULL);
  write_file (e->sys, rel, text);
  g_free (rel);
  g_free (text);

  text = g_strdup_printf ("%04x\n", product);
  rel = g_build_filename (usb, "idProduct", NULL);
  write_file (e->sys, rel, text);
  g_free (rel);
  g_free (text);

  text = g_strdup_printf ("%04x\n", bcd);
  rel = g_build_filename (usb, "bcdDevice", NULL);
  write_file (e->sys, rel, text);
  g_free (rel);
  g_free (text);

  rel = g_build_filename (usb, "serial", NULL);
  write_file (e->sys, rel, serial);
  g_free (rel);

  rel = g_build_filename (usb, "power/runtime_status", NULL);
  write_file (e->sys, rel, runtime_status);
  g_free (rel);

  rel = g_build_filename (scsi, "type", NULL);
  write_file (e->sys, rel, "0\n");
  g_free (rel);

  rel = g_build_filename (scsi, "model", NULL);
  write_file (e->sys, rel, model);
  g_free (rel);

  rel = g_build_filename (scsi, "power/runtime_status", NULL);
  write_file (e->sys, rel, "active\n");
  g_free (rel);

  rel = g_strdup_printf ("block/%s/.keep", name);
  write_file (e->sys, rel, "");
  g_free (rel);

  link_path = g_strdup_printf ("%s/block/%s/device", e->sys, name);
  link_target = g_strdup_printf ("../../%s", scsi);
  g_assert_cmpint (symlink (link_target, link_path), ==, 0);
  g_free (link_path);
  g_free (link_target);
  g_free (usb);
  g_free (scsi);
}


/* A SCSI disk on a SATA controller: no USB device anywhere above it. */
static void
add_internal_disk (Env        *e,
                   const char *name,
                   const char *model)
{
  char *scsi = g_strdup_printf ("devices/pci0000:00/0000:00:17.0/ata1/host0/target0:0:0/0:0:0:0");
  char *rel;
  char *link_path;
  char *link_target;

  rel = g_build_filename (scsi, "type", NULL);
  write_file (e->sys, rel, "0\n");
  g_free (rel);
  rel = g_build_filename (scsi, "model", NULL);
  write_file (e->sys, rel, model);
  g_free (rel);

  rel = g_strdup_printf ("block/%s/.keep", name);
  write_file (e->sys, rel, "");
  g_free (rel);

  link_path = g_strdup_printf ("%s/block/%s/device", e->sys, name);
  link_target = g_strdup_printf ("../../%s", scsi);
  g_assert_cmpint (symlink (link_target, link_path), ==, 0);
  g_free (link_path);
  g_free (link_target);
  g_free (scsi);
}


/* ------------------------------------------------------------------ */
/* The fake drives and transport                                       */
/* ------------------------------------------------------------------ */

typedef enum
{
  FAKE_REJECTS_EVERYTHING,   /* CHECK CONDITION for any command */
  FAKE_ASMEDIA,
  FAKE_REALTEK,
  FAKE_JMICRON,
  FAKE_SAT,
} FakeKind;

typedef struct
{
  const char *name;
  FakeKind kind;
  uint8_t identify[4096];           /* NVMe Identify Controller */
  uint8_t smart_log[512];           /* NVMe SMART / Health log page */
  uint8_t ata_identify[512];
  uint8_t ata_smart[512];
  uint8_t power_sector_count;       /* answer to CHECK POWER MODE */
  gboolean jmicron_bad_status;
  uint8_t jmicron_opcode;
} FakeDrive;

typedef struct
{
  uint8_t cdb[16];
  uint8_t cdb_len;
  uint32_t data_len;
  char name[32];
} Call;

typedef struct
{
  FakeDrive drives[8];
  int n_drives;
  Call calls[128];
  int n_calls;
  int n_opens;
} Fake;


static FakeDrive *
fake_add (Fake       *f,
          const char *name,
          FakeKind    kind)
{
  FakeDrive *d = &f->drives[f->n_drives++];

  memset (d, 0, sizeof *d);
  d->name = name;
  d->kind = kind;
  d->power_sector_count = 0xff;

  return d;
}


static intptr_t
fake_open (void       *user,
           const char *name)
{
  Fake *f = user;

  f->n_opens++;
  for (int i = 0; i < f->n_drives; i++)
    if (strcmp (f->drives[i].name, name) == 0)
      return i;

  return -1;
}


static void
fake_close (void    *user,
            intptr_t device)
{
  (void) user;
  (void) device;
}


static void
fake_check_condition (GsmScsiResult *r)
{
  r->scsi_status = 0x02;
  r->driver_status = 0x08;
  r->sense_len = 18;
  r->sense[0] = 0x70;
  r->sense[2] = 0x05;      /* ILLEGAL REQUEST */
  r->sense[12] = 0x20;     /* INVALID COMMAND OPERATION CODE */
}


static int
fake_scsi_cmd (void            *user,
               intptr_t         device,
               const uint8_t   *cdb,
               uint8_t          cdb_len,
               GsmScsiDirection direction,
               void            *data,
               uint32_t         data_len,
               GsmScsiResult   *r)
{
  Fake *f = user;
  FakeDrive *d = &f->drives[device];
  Call *c = &f->calls[f->n_calls++];
  uint8_t opcode = cdb[0];
  uint8_t nvme_opcode = 0;
  gboolean handled = FALSE;

  g_assert_cmpint (f->n_calls, <=, 128);
  memset (c, 0, sizeof *c);
  memcpy (c->cdb, cdb, MIN (cdb_len, sizeof c->cdb));
  c->cdb_len = cdb_len;
  c->data_len = data_len;
  g_strlcpy (c->name, d->name, sizeof c->name);

  if (d->kind == FAKE_ASMEDIA && opcode == 0xE6)
    {
      nvme_opcode = cdb[1];
      handled = TRUE;
    }
  else if (d->kind == FAKE_REALTEK && opcode == 0xE4)
    {
      nvme_opcode = cdb[3];
      handled = TRUE;
    }
  else if (d->kind == FAKE_JMICRON && opcode == 0xA1 && (cdb[1] & 0x80))
    {
      uint8_t phase = cdb[1] & 0x0f;

      if (phase == 0)
        {
          d->jmicron_opcode = ((uint8_t *) data)[8];
          return 0;
        }
      if (phase == 2)
        {
          nvme_opcode = d->jmicron_opcode;
          handled = TRUE;
        }
      if (phase == 0xf)
        {
          memset (data, 0, data_len);
          memcpy (data, "NVME", 4);
          if (d->jmicron_bad_status)
            ((uint8_t *) data)[22] = 0x02;  /* status bits in dword 5 */
          return 0;
        }
    }

  if (handled)
    {
      if (nvme_opcode == 0x06)
        memcpy (data, d->identify, MIN (data_len, sizeof d->identify));
      else if (nvme_opcode == 0x02)
        memcpy (data, d->smart_log, MIN (data_len, sizeof d->smart_log));
      else
        fake_check_condition (r);
      return 0;
    }

  if (d->kind == FAKE_SAT && opcode == 0x85)
    {
      uint8_t ata_command = cdb[14];

      if (ata_command == 0xE5)
        {
          r->scsi_status = 0x02;
          r->driver_status = 0x08;
          r->sense_len = 22;
          r->sense[0] = 0x72;
          r->sense[1] = 0x01;
          r->sense[3] = 0x1d;
          r->sense[7] = 14;
          r->sense[8] = 0x09;
          r->sense[9] = 12;
          r->sense[13] = d->power_sector_count;
          return 0;
        }
      if (ata_command == 0xEC)
        {
          memcpy (data, d->ata_identify, 512);
          return 0;
        }
      if (ata_command == 0xB0 && cdb[4] == 0xD0)
        {
          memcpy (data, d->ata_smart, 512);
          return 0;
        }
    }

  (void) direction;
  fake_check_condition (r);

  return 0;
}


/* ------------------------------------------------------------------ */
/* Canned drive answers                                                */
/* ------------------------------------------------------------------ */

static void
put_le (uint8_t *p,
        uint64_t value,
        int      bytes)
{
  for (int i = 0; i < bytes; i++)
    p[i] = (uint8_t) (value >> (8 * i));
}


static void
fill_nvme_identify (FakeDrive  *d,
                    const char *model,
                    const char *serial)
{
  memset (d->identify, 0, sizeof d->identify);
  memset (d->identify + 4, ' ', 20);
  memset (d->identify + 24, ' ', 40);
  memcpy (d->identify + 4, serial, strlen (serial));
  memcpy (d->identify + 24, model, strlen (model));
  put_le (d->identify + 80, 0x00010400, 4);
}


typedef struct
{
  unsigned kelvin;
  unsigned spare;
  unsigned percent_used;
  uint64_t units_read;
  uint64_t units_written;
  uint64_t hours;
  uint64_t unsafe_shutdowns;
  uint64_t media_errors;
} NvmeLog;

static const NvmeLog good_log = {
  .kelvin = 318, .spare = 100, .percent_used = 3,
  .units_read = 31200100, .units_written = 5400002,
  .hours = 1234, .unsafe_shutdowns = 7, .media_errors = 0,
};

static void
fill_nvme_log (FakeDrive     *d,
               const NvmeLog *l)
{
  memset (d->smart_log, 0, sizeof d->smart_log);
  put_le (d->smart_log + 1, l->kelvin, 2);
  d->smart_log[3] = (uint8_t) l->spare;
  d->smart_log[4] = 10;
  d->smart_log[5] = (uint8_t) l->percent_used;
  put_le (d->smart_log + 32, l->units_read, 8);
  put_le (d->smart_log + 48, l->units_written, 8);
  put_le (d->smart_log + 128, l->hours, 8);
  put_le (d->smart_log + 144, l->unsafe_shutdowns, 8);
  put_le (d->smart_log + 160, l->media_errors, 8);
}


static void
seal_sector (uint8_t *sector)
{
  uint8_t sum = 0;

  sector[511] = 0;
  for (int i = 0; i < 511; i++)
    sum = (uint8_t) (sum + sector[i]);
  sector[511] = (uint8_t) (0x100 - sum);
}


static void
fill_ata (FakeDrive  *d,
          const char *model,
          const char *serial,
          uint32_t    hours,
          uint8_t     celsius)
{
  uint8_t *attr;

  memset (d->ata_identify, 0, sizeof d->ata_identify);
  /* Two characters per word, first character in the high byte. */
  for (size_t i = 0; i < 40; i++)
    {
      char c = i < strlen (model) ? model[i] : ' ';
      d->ata_identify[27 * 2 + (i ^ 1)] = (uint8_t) c;
    }
  for (size_t i = 0; i < 20; i++)
    {
      char c = i < strlen (serial) ? serial[i] : ' ';
      d->ata_identify[10 * 2 + (i ^ 1)] = (uint8_t) c;
    }
  d->ata_identify[510] = 0xA5;
  seal_sector (d->ata_identify);

  memset (d->ata_smart, 0, sizeof d->ata_smart);
  attr = d->ata_smart + 2;
  attr[0] = 9;
  attr[3] = 99;
  put_le (attr + 5, hours, 4);
  attr += 12;
  attr[0] = 194;
  attr[3] = 70;
  put_le (attr + 5, celsius, 1);
  seal_sector (d->ata_smart);
}


/* ------------------------------------------------------------------ */
/* Running the core and reading the result                             */
/* ------------------------------------------------------------------ */

static GsmSmartTransport
transport_for (Fake *f)
{
  GsmSmartTransport t = {
    fake_open, fake_scsi_cmd, fake_close, f
  };

  return t;
}


static int
run_core (Env    *e,
          Fake   *f,
          int64_t now)
{
  GsmSmartTransport t = transport_for (f);
  GsmSmartRun run = {
    .sys_root = e->sys,
    .out_dir = e->out,
    .transport = &t,
    .now_unix_s = now,
    .reader_version = "test",
  };

  return gsm_smart_run (&run);
}


static GKeyFile *
load_result (Env *e)
{
  GKeyFile *kf = g_key_file_new ();
  char *path = g_build_filename (e->out, "smart.ini", NULL);
  GError *error = NULL;

  g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, &error);
  g_assert_no_error (error);
  g_free (path);

  return kf;
}


static char *
drive_string (GKeyFile   *kf,
              const char *name,
              const char *key)
{
  char *group = g_strdup_printf ("drive %s", name);
  char *value = g_key_file_get_string (kf, group, key, NULL);

  g_free (group);

  return value;
}


static gboolean
drive_has (GKeyFile   *kf,
           const char *name,
           const char *key)
{
  char *group = g_strdup_printf ("drive %s", name);
  gboolean has = g_key_file_has_key (kf, group, key, NULL);

  g_free (group);

  return has;
}


static guint64
drive_u64 (GKeyFile   *kf,
           const char *name,
           const char *key)
{
  char *group = g_strdup_printf ("drive %s", name);
  GError *error = NULL;
  guint64 value = g_key_file_get_uint64 (kf, group, key, &error);

  g_assert_no_error (error);
  g_free (group);

  return value;
}


static void
assert_status (GKeyFile   *kf,
               const char *name,
               const char *expected)
{
  char *status = drive_string (kf, name, "status");

  g_assert_cmpstr (status, ==, expected);
  g_free (status);
}


static const char *const figure_keys[] = {
  "bytes_written", "bytes_read", "percent_used", "power_on_hours",
  "temperature_kelvin", "media_errors", "unsafe_shutdowns",
};

static void
assert_no_figures (GKeyFile   *kf,
                   const char *name)
{
  for (size_t i = 0; i < G_N_ELEMENTS (figure_keys); i++)
    g_assert_false (drive_has (kf, name, figure_keys[i]));
}


static gboolean
sent_any_vendor_or_nvme_cdb (const Fake *f,
                             const char *name)
{
  for (int i = 0; i < f->n_calls; i++)
    {
      uint8_t op = f->calls[i].cdb[0];

      if (strcmp (f->calls[i].name, name) == 0 && (op == 0xE6 || op == 0xE4 || op == 0xA1))
        return TRUE;
    }

  return FALSE;
}


/* One X9-like drive behind ASMedia, ready to be read. */
static FakeDrive *
add_nvme_over_asmedia (Env        *e,
                       Fake       *f,
                       const char *name,
                       int         port)
{
  FakeDrive *d = fake_add (f, name, FAKE_ASMEDIA);

  add_usb_disk (e, name, port, 0x174c, 0x2362, 0x0100, "TESTNVME  \n", "USBSERIAL1\n", "active\n");
  fill_nvme_identify (d, "TESTNVME 1TB", "TN0000000001");
  fill_nvme_log (d, &good_log);

  return d;
}


/* ------------------------------------------------------------------ */
/* Rules                                                               */
/* ------------------------------------------------------------------ */

static void
test_unknown_bridge_gets_no_command_and_is_unsupported (void)
{
  Env *e = env_new ();
  Fake f = {
    0
  };
  GKeyFile *kf;
  char *usb_id;

  fake_add (&f, "sda", FAKE_ASMEDIA);
  add_usb_disk (e, "sda", 2, 0x0634, 0x0655, 0x0100, "Micron USB SSD\n", "MADEUP1\n", "active\n");

  g_assert_cmpint (run_core (e, &f, 1000), ==, 0);
  kf = load_result (e);

  g_assert_cmpint (f.n_calls, ==, 0);
  g_assert_cmpint (f.n_opens, ==, 0);
  assert_status (kf, "sda", "unsupported");
  usb_id = drive_string (kf, "sda", "usb_id");
  g_assert_cmpstr (usb_id, ==, "0634:0655");
  g_assert_false (drive_has (kf, "sda", "bridge"));
  assert_no_figures (kf, "sda");

  g_free (usb_id);
  g_key_file_free (kf);
  env_free (e);
}


static void
test_a_vendor_id_alone_does_not_select_a_bridge (void)
{
  Env *e = env_new ();
  Fake f = {
    0
  };
  GKeyFile *kf;

  /* Micron and ASMedia ids the table does not name for this product */
  fake_add (&f, "sda", FAKE_ASMEDIA);
  fake_add (&f, "sdb", FAKE_ASMEDIA);
  add_usb_disk (e, "sda", 2, 0x0634, 0x1234, 0x0100, "A\n", "S1\n", "active\n");
  add_usb_disk (e, "sdb", 3, 0x174c, 0xffff, 0x0100, "B\n", "S2\n", "active\n");

  g_assert_cmpint (run_core (e, &f, 1000), ==, 0);
  kf = load_result (e);

  g_assert_cmpint (f.n_calls, ==, 0);
  assert_status (kf, "sda", "unsupported");
  assert_status (kf, "sdb", "unsupported");

  g_key_file_free (kf);
  env_free (e);
}


static void
test_a_suspended_drive_gets_no_command_and_keeps_its_last_good_values (void)
{
  Env *e = env_new ();
  Fake f = {
    0
  };
  GKeyFile *kf;
  char *path;
  int calls_after_first;

  add_nvme_over_asmedia (e, &f, "sda", 2);

  g_assert_cmpint (run_core (e, &f, 1000), ==, 0);
  kf = load_result (e);
  assert_status (kf, "sda", "ok");
  g_assert_cmpuint (drive_u64 (kf, "sda", "bytes_written"), ==, 5400002ULL * 512000ULL);
  g_key_file_free (kf);
  calls_after_first = f.n_calls;
  g_assert_cmpint (calls_after_first, >, 0);

  path = g_build_filename (e->sys, "devices/pci0000:00/usb3/3-2/power/runtime_status", NULL);
  g_assert_true (g_file_set_contents (path, "suspended\n", -1, NULL));
  g_free (path);

  g_assert_cmpint (run_core (e, &f, 2000), ==, 0);
  kf = load_result (e);

  g_assert_cmpint (f.n_calls, ==, calls_after_first);
  assert_status (kf, "sda", "asleep");
  g_assert_cmpuint (drive_u64 (kf, "sda", "bytes_written"), ==, 5400002ULL * 512000ULL);
  g_assert_cmpuint (drive_u64 (kf, "sda", "power_on_hours"), ==, 1234);
  g_assert_cmpuint (drive_u64 (kf, "sda", "last_ok_generated_at"), ==, 1000);

  g_key_file_free (kf);
  env_free (e);
}


static void
test_a_sleeping_drive_never_read_has_no_figures_not_zeros (void)
{
  Env *e = env_new ();
  Fake f = {
    0
  };
  GKeyFile *kf;

  add_nvme_over_asmedia (e, &f, "sda", 2);
  {
    char *path = g_build_filename (e->sys, "devices/pci0000:00/usb3/3-2/power/runtime_status", NULL);

    g_assert_true (g_file_set_contents (path, "suspended\n", -1, NULL));
    g_free (path);
  }

  g_assert_cmpint (run_core (e, &f, 1000), ==, 0);
  kf = load_result (e);

  g_assert_cmpint (f.n_calls, ==, 0);
  assert_status (kf, "sda", "asleep");
  assert_no_figures (kf, "sda");
  g_assert_false (drive_has (kf, "sda", "last_ok_generated_at"));

  g_key_file_free (kf);
  env_free (e);
}


static void
test_a_sat_drive_in_standby_gets_check_power_mode_and_nothing_else (void)
{
  Env *e = env_new ();
  Fake f = {
    0
  };
  FakeDrive *d = fake_add (&f, "sda", FAKE_SAT);
  GKeyFile *kf;

  add_usb_disk (e, "sda", 2, 0x152d, 0x0578, 0x0100, "SATA SSD\n", "SATASER1\n", "active\n");
  fill_ata (d, "TEST SATA SSD", "SATA0001", 4321, 35);
  d->power_sector_count = 0x00;

  g_assert_cmpint (run_core (e, &f, 1000), ==, 0);
  kf = load_result (e);

  g_assert_cmpint (f.n_calls, ==, 1);
  g_assert_cmpuint (f.calls[0].cdb[0], ==, 0x85);
  g_assert_cmpuint (f.calls[0].cdb[14], ==, 0xE5);
  assert_status (kf, "sda", "asleep");
  assert_no_figures (kf, "sda");

  g_key_file_free (kf);
  env_free (e);
}


static void
test_a_sat_drive_that_is_awake_is_read_after_check_power_mode (void)
{
  Env *e = env_new ();
  Fake f = {
    0
  };
  FakeDrive *d = fake_add (&f, "sda", FAKE_SAT);
  GKeyFile *kf;
  char *model;

  add_usb_disk (e, "sda", 2, 0x152d, 0x0578, 0x0100, "SATA SSD\n", "SATASER1\n", "active\n");
  fill_ata (d, "TEST SATA SSD", "SATA0001", 4321, 35);

  g_assert_cmpint (run_core (e, &f, 1000), ==, 0);
  kf = load_result (e);

  g_assert_cmpint (f.n_calls, ==, 3);
  g_assert_cmpuint (f.calls[0].cdb[14], ==, 0xE5);
  g_assert_cmpuint (f.calls[1].cdb[14], ==, 0xEC);
  g_assert_cmpuint (f.calls[2].cdb[14], ==, 0xB0);
  assert_status (kf, "sda", "ok");
  model = drive_string (kf, "sda", "model");
  g_assert_cmpstr (model, ==, "TEST SATA SSD");
  g_assert_cmpuint (drive_u64 (kf, "sda", "power_on_hours"), ==, 4321);
  g_assert_cmpuint (drive_u64 (kf, "sda", "temperature_kelvin"), ==, 308);
  /* ATA volume and wear attributes are vendor specific: not reported */
  g_assert_false (drive_has (kf, "sda", "bytes_written"));
  g_assert_false (drive_has (kf, "sda", "percent_used"));

  g_free (model);
  g_key_file_free (kf);
  env_free (e);
}


static void
test_a_jmicron_sata_bridge_never_receives_the_nvme_tunnel (void)
{
  Env *e = env_new ();
  Fake f = {
    0
  };
  GKeyFile *kf;

  /* Rejects everything, including SAT: the reader must still not try 0xA1,
   * which a SAT bridge reads as a reset. */
  fake_add (&f, "sda", FAKE_REJECTS_EVERYTHING);
  add_usb_disk (e, "sda", 2, 0x152d, 0x0578, 0x0100, "SATA SSD\n", "SATASER1\n", "active\n");

  g_assert_cmpint (run_core (e, &f, 1000), ==, 0);
  kf = load_result (e);

  g_assert_cmpint (f.n_calls, >, 0);
  g_assert_false (sent_any_vendor_or_nvme_cdb (&f, "sda"));
  assert_status (kf, "sda", "failed");
  assert_no_figures (kf, "sda");

  g_key_file_free (kf);
  env_free (e);
}


static void
test_a_non_good_scsi_status_is_failed_never_zeros (void)
{
  Env *e = env_new ();
  Fake f = {
    0
  };
  FakeDrive *d = add_nvme_over_asmedia (e, &f, "sda", 2);
  GKeyFile *kf;

  d->kind = FAKE_REJECTS_EVERYTHING;

  g_assert_cmpint (run_core (e, &f, 1000), ==, 0);
  kf = load_result (e);

  assert_status (kf, "sda", "failed");
  assert_no_figures (kf, "sda");

  g_key_file_free (kf);
  env_free (e);
}


static void
test_a_tunnel_answered_by_the_wrong_protocol_is_failed (void)
{
  Env *e = env_new ();
  Fake f = {
    0
  };
  FakeDrive *d = add_nvme_over_asmedia (e, &f, "sda", 2);
  GKeyFile *kf;

  /* the table says ASMedia; this drive only speaks Realtek */
  d->kind = FAKE_REALTEK;

  g_assert_cmpint (run_core (e, &f, 1000), ==, 0);
  kf = load_result (e);

  assert_status (kf, "sda", "failed");
  assert_no_figures (kf, "sda");
  for (int i = 0; i < f.n_calls; i++)
    g_assert_cmpuint (f.calls[i].cdb[0], ==, 0xE6);

  g_key_file_free (kf);
  env_free (e);
}


static void
test_an_all_zero_smart_log_is_garbled_not_a_healthy_drive (void)
{
  Env *e = env_new ();
  Fake f = {
    0
  };
  FakeDrive *d = add_nvme_over_asmedia (e, &f, "sda", 2);
  GKeyFile *kf;

  memset (d->smart_log, 0, sizeof d->smart_log);

  g_assert_cmpint (run_core (e, &f, 1000), ==, 0);
  kf = load_result (e);

  assert_status (kf, "sda", "garbled");
  assert_no_figures (kf, "sda");

  g_key_file_free (kf);
  env_free (e);
}


static void
test_an_implausible_temperature_or_spare_is_garbled (void)
{
  Env *e = env_new ();
  Fake f = {
    0
  };
  FakeDrive *d = add_nvme_over_asmedia (e, &f, "sda", 2);
  NvmeLog bad = good_log;
  GKeyFile *kf;

  bad.kelvin = 90;
  fill_nvme_log (d, &bad);
  g_assert_cmpint (run_core (e, &f, 1000), ==, 0);
  kf = load_result (e);
  assert_status (kf, "sda", "garbled");
  g_key_file_free (kf);

  bad = good_log;
  bad.spare = 180;
  fill_nvme_log (d, &bad);
  g_assert_cmpint (run_core (e, &f, 2000), ==, 0);
  kf = load_result (e);
  assert_status (kf, "sda", "garbled");
  assert_no_figures (kf, "sda");
  g_key_file_free (kf);

  env_free (e);
}


static void
test_a_temperature_of_zero_is_absent_not_zero_kelvin (void)
{
  Env *e = env_new ();
  Fake f = {
    0
  };
  FakeDrive *d = add_nvme_over_asmedia (e, &f, "sda", 2);
  NvmeLog log = good_log;
  GKeyFile *kf;

  log.kelvin = 0;
  fill_nvme_log (d, &log);

  g_assert_cmpint (run_core (e, &f, 1000), ==, 0);
  kf = load_result (e);

  assert_status (kf, "sda", "ok");
  g_assert_false (drive_has (kf, "sda", "temperature_kelvin"));
  g_assert_true (drive_has (kf, "sda", "bytes_written"));

  g_key_file_free (kf);
  env_free (e);
}


static void
test_a_counter_below_the_last_good_value_is_garbled (void)
{
  Env *e = env_new ();
  Fake f = {
    0
  };
  FakeDrive *d = add_nvme_over_asmedia (e, &f, "sda", 2);
  NvmeLog log = good_log;
  GKeyFile *kf;

  g_assert_cmpint (run_core (e, &f, 1000), ==, 0);

  log.units_written = good_log.units_written - 1;
  fill_nvme_log (d, &log);
  g_assert_cmpint (run_core (e, &f, 2000), ==, 0);
  kf = load_result (e);

  assert_status (kf, "sda", "garbled");
  assert_no_figures (kf, "sda");
  g_assert_cmpuint (drive_u64 (kf, "sda", "last_ok_generated_at"), ==, 1000);

  g_key_file_free (kf);
  env_free (e);
}


static void
test_a_counter_that_overflows_bytes_is_garbled (void)
{
  Env *e = env_new ();
  Fake f = {
    0
  };
  FakeDrive *d = add_nvme_over_asmedia (e, &f, "sda", 2);
  NvmeLog log = good_log;
  GKeyFile *kf;

  log.units_written = G_MAXUINT64 / 1000;
  fill_nvme_log (d, &log);
  g_assert_cmpint (run_core (e, &f, 1000), ==, 0);
  kf = load_result (e);

  assert_status (kf, "sda", "garbled");

  g_key_file_free (kf);
  env_free (e);
}


static void
test_a_bad_ata_checksum_is_garbled (void)
{
  Env *e = env_new ();
  Fake f = {
    0
  };
  FakeDrive *d = fake_add (&f, "sda", FAKE_SAT);
  GKeyFile *kf;

  add_usb_disk (e, "sda", 2, 0x152d, 0x0578, 0x0100, "SATA SSD\n", "SATASER1\n", "active\n");
  fill_ata (d, "TEST SATA SSD", "SATA0001", 4321, 35);
  d->ata_smart[100] ^= 0x55;

  g_assert_cmpint (run_core (e, &f, 1000), ==, 0);
  kf = load_result (e);

  assert_status (kf, "sda", "garbled");
  assert_no_figures (kf, "sda");

  g_key_file_free (kf);
  env_free (e);
}


static void
test_nvme_disks_and_internal_scsi_disks_are_skipped (void)
{
  Env *e = env_new ();
  Fake f = {
    0
  };
  GKeyFile *kf;
  gchar **groups;

  fake_add (&f, "sdb", FAKE_ASMEDIA);
  add_internal_disk (e, "sda", "INTERNAL SSD\n");
  add_internal_disk (e, "nvme0n1", "NVME SSD\n");
  add_usb_disk (e, "nvme1n1", 5, 0x174c, 0x2362, 0x0100, "TRAP\n", "X\n", "active\n");
  add_usb_disk (e, "loop0", 6, 0x174c, 0x2362, 0x0100, "TRAP\n", "X\n", "active\n");
  add_usb_disk (e, "sdb", 4, 0x0634, 0x0655, 0x0100, "Micron USB SSD\n", "X\n", "active\n");

  g_assert_cmpint (run_core (e, &f, 1000), ==, 0);
  kf = load_result (e);

  g_assert_cmpint (f.n_calls, ==, 0);
  g_assert_cmpint (f.n_opens, ==, 0);
  groups = g_key_file_get_groups (kf, NULL);
  g_assert_cmpuint (g_strv_length (groups), ==, 2);   /* meta and sdb */
  g_assert_true (g_strv_contains ((const char *const *) groups, "drive sdb"));

  g_strfreev (groups);
  g_key_file_free (kf);
  env_free (e);
}


static void
test_a_usb_cd_drive_is_not_read (void)
{
  Env *e = env_new ();
  Fake f = {
    0
  };
  GKeyFile *kf;
  char *path;

  fake_add (&f, "sr0", FAKE_ASMEDIA);
  add_usb_disk (e, "sr0", 2, 0x174c, 0x2362, 0x0100, "DVD\n", "X\n", "active\n");
  path = g_build_filename (e->sys, "devices/pci0000:00/usb3/3-2/3-2:1.0/host6/target6:0:0/6:0:0:0/type", NULL);
  g_assert_true (g_file_set_contents (path, "5\n", -1, NULL));
  g_free (path);

  g_assert_cmpint (run_core (e, &f, 1000), ==, 0);
  kf = load_result (e);

  g_assert_cmpint (f.n_opens, ==, 0);
  g_assert_false (g_key_file_has_group (kf, "drive sr0"));

  g_key_file_free (kf);
  env_free (e);
}


/* ------------------------------------------------------------------ */
/* The tunnels and the key file                                        */
/* ------------------------------------------------------------------ */

static void
test_asmedia_nvme_figures_are_read_and_written_with_units (void)
{
  Env *e = env_new ();
  Fake f = {
    0
  };
  GKeyFile *kf;
  char *serial;
  char *model;
  char *bridge;
  char *basis;

  add_nvme_over_asmedia (e, &f, "sda", 2);

  g_assert_cmpint (run_core (e, &f, 1700000000), ==, 0);
  kf = load_result (e);

  g_assert_cmpint (g_key_file_get_integer (kf, "meta", "schema", NULL), ==, 1);
  g_assert_cmpuint (g_key_file_get_uint64 (kf, "meta", "generated_at", NULL), ==, 1700000000);
  assert_status (kf, "sda", "ok");
  model = drive_string (kf, "sda", "model");
  serial = drive_string (kf, "sda", "serial");
  bridge = drive_string (kf, "sda", "bridge");
  basis = drive_string (kf, "sda", "basis");
  g_assert_cmpstr (model, ==, "TESTNVME 1TB");
  g_assert_cmpstr (serial, ==, "TN0000000001");
  g_assert_cmpstr (bridge, ==, "asmedia");
  g_assert_cmpstr (basis, ==, "upstream");
  g_assert_cmpuint (drive_u64 (kf, "sda", "bytes_written"), ==, 5400002ULL * 512000ULL);
  g_assert_cmpuint (drive_u64 (kf, "sda", "bytes_read"), ==, 31200100ULL * 512000ULL);
  g_assert_cmpuint (drive_u64 (kf, "sda", "percent_used"), ==, 3);
  g_assert_cmpuint (drive_u64 (kf, "sda", "power_on_hours"), ==, 1234);
  g_assert_cmpuint (drive_u64 (kf, "sda", "temperature_kelvin"), ==, 318);
  g_assert_cmpuint (drive_u64 (kf, "sda", "media_errors"), ==, 0);
  g_assert_cmpuint (drive_u64 (kf, "sda", "unsafe_shutdowns"), ==, 7);
  g_assert_cmpuint (drive_u64 (kf, "sda", "last_ok_generated_at"), ==, 1700000000);
  /* only the two NVMe admin commands went out */
  g_assert_cmpint (f.n_calls, ==, 2);
  g_assert_cmpuint (f.calls[0].cdb[1], ==, 0x06);
  g_assert_cmpuint (f.calls[1].cdb[1], ==, 0x02);

  g_free (model);
  g_free (serial);
  g_free (bridge);
  g_free (basis);
  g_key_file_free (kf);
  env_free (e);
}


static void
test_the_unlisted_x9_is_read_through_asmedia_and_marked_observed (void)
{
  Env *e = env_new ();
  Fake f = {
    0
  };
  FakeDrive *d = fake_add (&f, "sda", FAKE_ASMEDIA);
  GKeyFile *kf;
  char *basis;
  char *bcd;

  add_usb_disk (e, "sda", 2, 0x0634, 0x5605, 0x0110, "CT-MADEUP\n", "MADEUP\n", "active\n");
  fill_nvme_identify (d, "MADEUP X9 1TB", "XX0001");
  fill_nvme_log (d, &good_log);

  g_assert_cmpint (run_core (e, &f, 1000), ==, 0);
  kf = load_result (e);

  assert_status (kf, "sda", "ok");
  basis = drive_string (kf, "sda", "basis");
  bcd = drive_string (kf, "sda", "bcd_device");
  g_assert_cmpstr (basis, ==, "observed");
  g_assert_cmpstr (bcd, ==, "0110");

  g_free (basis);
  g_free (bcd);
  g_key_file_free (kf);
  env_free (e);
}


static void
test_the_x9_entry_does_not_match_another_firmware_revision (void)
{
  g_assert_nonnull (gsm_bridge_lookup (0x0634, 0x5605, 0x0110));
  g_assert_null (gsm_bridge_lookup (0x0634, 0x5605, 0x0111));
  g_assert_cmpint (gsm_bridge_lookup (0x0634, 0x5605, 0x0110)->basis, ==, GSM_BRIDGE_OBSERVED);
}


static void
test_realtek_reads_never_ask_for_more_than_512_bytes (void)
{
  Env *e = env_new ();
  Fake f = {
    0
  };
  FakeDrive *d = fake_add (&f, "sda", FAKE_REALTEK);
  GKeyFile *kf;

  add_usb_disk (e, "sda", 2, 0x0bda, 0x9210, 0x2100, "RTK\n", "R1\n", "active\n");
  fill_nvme_identify (d, "MADEUP RTK 512GB", "RT0001");
  fill_nvme_log (d, &good_log);

  g_assert_cmpint (run_core (e, &f, 1000), ==, 0);
  kf = load_result (e);

  assert_status (kf, "sda", "ok");
  g_assert_cmpint (f.n_calls, ==, 2);
  for (int i = 0; i < f.n_calls; i++)
    {
      g_assert_cmpuint (f.calls[i].cdb[0], ==, 0xE4);
      g_assert_cmpuint (f.calls[i].data_len, <=, 512);
    }

  g_key_file_free (kf);
  env_free (e);
}


static void
test_jmicron_nvme_runs_three_phases_and_checks_the_completion (void)
{
  Env *e = env_new ();
  Fake f = {
    0
  };
  FakeDrive *d = fake_add (&f, "sda", FAKE_JMICRON);
  GKeyFile *kf;

  add_usb_disk (e, "sda", 2, 0x152d, 0xa583, 0x0100, "JMS\n", "J1\n", "active\n");
  fill_nvme_identify (d, "MADEUP JM 256GB", "JM0001");
  fill_nvme_log (d, &good_log);

  g_assert_cmpint (run_core (e, &f, 1000), ==, 0);
  kf = load_result (e);
  assert_status (kf, "sda", "ok");
  g_assert_cmpint (f.n_calls, ==, 6);
  g_key_file_free (kf);

  /* a completion with a non-zero NVMe status: the data is not believed */
  d->jmicron_bad_status = TRUE;
  g_assert_cmpint (run_core (e, &f, 2000), ==, 0);
  kf = load_result (e);
  assert_status (kf, "sda", "failed");
  assert_no_figures (kf, "sda");
  g_key_file_free (kf);

  env_free (e);
}


static void
test_strings_from_drives_cannot_forge_keys_or_groups (void)
{
  Env *e = env_new ();
  Fake f = {
    0
  };
  GKeyFile *kf;
  char *model;
  char *serial;
  char *raw;
  char *path;

  fake_add (&f, "sda", FAKE_ASMEDIA);
  add_usb_disk (e, "sda", 2, 0x0634, 0x0655, 0x0100, "Evil\n[drive x]\nstatus=ok\n", "S[1]=\\n;2\n", "active\n");

  g_assert_cmpint (run_core (e, &f, 1000), ==, 0);
  kf = load_result (e);

  g_assert_false (g_key_file_has_group (kf, "drive x"));
  assert_status (kf, "sda", "unsupported");
  model = drive_string (kf, "sda", "model");
  serial = drive_string (kf, "sda", "serial");
  g_assert_nonnull (model);
  g_assert_null (strpbrk (model, "[]=\n\\"));
  g_assert_null (strpbrk (serial, "[]=\n\\;"));

  path = g_build_filename (e->out, "smart.ini", NULL);
  g_assert_true (g_file_get_contents (path, &raw, NULL, NULL));
  g_assert_null (strstr (raw, "[drive x]"));

  g_free (raw);
  g_free (path);
  g_free (model);
  g_free (serial);
  g_key_file_free (kf);
  env_free (e);
}


/* ------------------------------------------------------------------ */
/* The file                                                            */
/* ------------------------------------------------------------------ */

static void
test_the_file_is_replaced_atomically_with_mode_0644 (void)
{
  Env *e = env_new ();
  Fake f = {
    0
  };
  char *path = g_build_filename (e->out, "smart.ini", NULL);
  char *tmp_path = g_build_filename (e->out, "smart.ini.tmp", NULL);
  char *old_text = NULL;
  char *new_text = NULL;
  char *still_old = NULL;
  GStatBuf st;
  FILE *old_handle;
  char buffer[256] = {
    0
  };

  add_nvme_over_asmedia (e, &f, "sda", 2);
  g_assert_cmpint (run_core (e, &f, 1000), ==, 0);
  g_assert_true (g_file_get_contents (path, &old_text, NULL, NULL));

  /* a reader that opened the file before the next run keeps the old, whole file */
  old_handle = fopen (path, "r");
  g_assert_nonnull (old_handle);

  g_assert_cmpint (run_core (e, &f, 2000), ==, 0);
  g_assert_true (g_file_get_contents (path, &new_text, NULL, NULL));
  g_assert_nonnull (strstr (new_text, "generated_at=2000"));
  g_assert_nonnull (strstr (old_text, "generated_at=1000"));

  g_assert_cmpuint (fread (buffer, 1, sizeof buffer - 1, old_handle), >, 0);
  still_old = g_strdup (buffer);
  g_assert_true (g_str_has_prefix (old_text, still_old));
  g_assert_nonnull (strstr (still_old, "generated_at=1000"));
  fclose (old_handle);

  g_assert_false (g_file_test (tmp_path, G_FILE_TEST_EXISTS));
  g_assert_cmpint (g_stat (path, &st), ==, 0);
  g_assert_cmpuint (st.st_mode & 07777, ==, 0644);

  g_free (still_old);
  g_free (old_text);
  g_free (new_text);
  g_free (path);
  g_free (tmp_path);
  env_free (e);
}


static void
test_a_failed_write_leaves_the_old_file_untouched (void)
{
  Env *e = env_new ();
  Fake f = {
    0
  };
  char *path = g_build_filename (e->out, "smart.ini", NULL);
  char *before = NULL;
  char *after = NULL;

  add_nvme_over_asmedia (e, &f, "sda", 2);
  g_assert_cmpint (run_core (e, &f, 1000), ==, 0);
  g_assert_true (g_file_get_contents (path, &before, NULL, NULL));

  /* A directory in the temporary file's place makes the write fail. */
  {
    char *tmp_path = g_build_filename (e->out, "smart.ini.tmp", NULL);

    g_assert_cmpint (g_mkdir (tmp_path, 0755), ==, 0);
    g_free (tmp_path);
  }

  g_assert_cmpint (run_core (e, &f, 2000), ==, -1);
  g_assert_true (g_file_get_contents (path, &after, NULL, NULL));
  g_assert_cmpstr (before, ==, after);

  g_free (before);
  g_free (after);
  g_free (path);
  env_free (e);
}


static void
test_a_failed_drive_keeps_the_time_it_last_read_ok (void)
{
  Env *e = env_new ();
  Fake f = {
    0
  };
  FakeDrive *d = add_nvme_over_asmedia (e, &f, "sda", 2);
  GKeyFile *kf;

  g_assert_cmpint (run_core (e, &f, 1000), ==, 0);
  d->kind = FAKE_REJECTS_EVERYTHING;
  g_assert_cmpint (run_core (e, &f, 2000), ==, 0);
  kf = load_result (e);

  assert_status (kf, "sda", "failed");
  assert_no_figures (kf, "sda");
  g_assert_cmpuint (drive_u64 (kf, "sda", "last_ok_generated_at"), ==, 1000);

  g_key_file_free (kf);
  env_free (e);
}


static void
test_a_different_drive_on_the_same_name_inherits_nothing (void)
{
  Env *e = env_new ();
  Fake f = {
    0
  };
  FakeDrive *d = add_nvme_over_asmedia (e, &f, "sda", 2);
  NvmeLog other = good_log;
  GKeyFile *kf;
  char *serial_path = g_build_filename (e->sys, "devices/pci0000:00/usb3/3-2/serial", NULL);
  char *pm_path = g_build_filename (e->sys, "devices/pci0000:00/usb3/3-2/power/runtime_status", NULL);

  g_assert_cmpint (run_core (e, &f, 1000), ==, 0);

  /* unplugged; another drive, with lower counters, becomes sda */
  g_assert_true (g_file_set_contents (serial_path, "OTHERSERIAL9\n", -1, NULL));
  other.units_written = 100;
  other.hours = 5;
  fill_nvme_log (d, &other);
  g_assert_cmpint (run_core (e, &f, 2000), ==, 0);
  kf = load_result (e);
  assert_status (kf, "sda", "ok");
  g_assert_cmpuint (drive_u64 (kf, "sda", "bytes_written"), ==, 100ULL * 512000ULL);
  g_key_file_free (kf);

  /* a third one, asleep from the start: nothing of the others is carried */
  g_assert_true (g_file_set_contents (serial_path, "THIRDSERIAL3\n", -1, NULL));
  g_assert_true (g_file_set_contents (pm_path, "suspended\n", -1, NULL));
  g_assert_cmpint (run_core (e, &f, 3000), ==, 0);
  kf = load_result (e);
  assert_status (kf, "sda", "asleep");
  assert_no_figures (kf, "sda");
  g_assert_false (drive_has (kf, "sda", "last_ok_generated_at"));

  g_key_file_free (kf);
  g_free (serial_path);
  g_free (pm_path);
  env_free (e);
}


static void
test_the_same_drive_is_matched_though_its_own_serial_differs_from_the_usb_one (void)
{
  Env *e = env_new ();
  Fake f = {
    0
  };
  NvmeLog lower = good_log;
  FakeDrive *d = add_nvme_over_asmedia (e, &f, "sda", 2);
  GKeyFile *kf;
  char *serial;
  char *pm_path = g_build_filename (e->sys, "devices/pci0000:00/usb3/3-2/power/runtime_status", NULL);

  g_assert_cmpint (run_core (e, &f, 1000), ==, 0);
  kf = load_result (e);
  serial = drive_string (kf, "sda", "serial");
  g_assert_cmpstr (serial, ==, "TN0000000001");   /* the drive's, not USBSERIAL1 */
  g_free (serial);
  g_key_file_free (kf);

  /* same drive, a counter that went down: still recognised, so garbled */
  lower.units_read = 1;
  fill_nvme_log (d, &lower);
  g_assert_cmpint (run_core (e, &f, 2000), ==, 0);
  kf = load_result (e);
  assert_status (kf, "sda", "garbled");
  g_key_file_free (kf);

  /* and asleep it carries what it last gave */
  fill_nvme_log (d, &good_log);
  g_assert_cmpint (run_core (e, &f, 3000), ==, 0);
  g_assert_true (g_file_set_contents (pm_path, "suspended\n", -1, NULL));
  g_assert_cmpint (run_core (e, &f, 4000), ==, 0);
  kf = load_result (e);
  assert_status (kf, "sda", "asleep");
  g_assert_cmpuint (drive_u64 (kf, "sda", "bytes_read"), ==, 31200100ULL * 512000ULL);
  g_assert_cmpuint (drive_u64 (kf, "sda", "last_ok_generated_at"), ==, 3000);

  g_key_file_free (kf);
  g_free (pm_path);
  env_free (e);
}


/* ------------------------------------------------------------------ */
/* The table                                                           */
/* ------------------------------------------------------------------ */

static void
test_the_table_has_no_duplicate_keys_and_names_every_entry (void)
{
  size_t count;
  const GsmBridge *t = gsm_bridge_table (&count);

  g_assert_cmpuint (count, >, 20);
  for (size_t i = 0; i < count; i++)
    {
      g_assert_nonnull (t[i].name);
      for (size_t j = i + 1; j < count; j++)
        g_assert_false (t[i].vendor_id == t[j].vendor_id &&
                        t[i].product_id == t[j].product_id &&
                        t[i].bcd_device == t[j].bcd_device);
    }
}


static void
test_ambiguous_or_unsupported_drivedb_ids_are_not_in_the_table (void)
{
  g_assert_null (gsm_bridge_lookup (0x0634, 0x0655, 0x0100));   /* unsupported */
  g_assert_null (gsm_bridge_lookup (0x152d, 0x0583, 0x0100));   /* NVMe or SATA */
  g_assert_null (gsm_bridge_lookup (0x152d, 0x0581, 0x0100));   /* may hang */
  g_assert_null (gsm_bridge_lookup (0x0bda, 0x9211, 0x0100));   /* NVMe or SATA */
  g_assert_null (gsm_bridge_lookup (0x174c, 0x55aa, 0x0100));   /* wildcard row */
}


static void
test_jmicron_sata_ids_map_to_sat_and_never_to_a_tunnel (void)
{
  size_t count;
  const GsmBridge *t = gsm_bridge_table (&count);

  for (size_t i = 0; i < count; i++)
    if (t[i].vendor_id == 0x152d && t[i].product_id != 0xa581 &&
        t[i].product_id != 0xb581 && t[i].product_id != 0xa583)
      g_assert_cmpint (t[i].protocol, ==, GSM_BRIDGE_SAT);
}


int
main (int    argc,
      char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/smart-reader/unknown-bridge-gets-no-command-and-is-unsupported",
                   test_unknown_bridge_gets_no_command_and_is_unsupported);
  g_test_add_func ("/smart-reader/a-vendor-id-alone-does-not-select-a-bridge",
                   test_a_vendor_id_alone_does_not_select_a_bridge);
  g_test_add_func ("/smart-reader/suspended-drive-gets-no-command-and-keeps-last-good-values",
                   test_a_suspended_drive_gets_no_command_and_keeps_its_last_good_values);
  g_test_add_func ("/smart-reader/sleeping-drive-never-read-has-no-figures-not-zeros",
                   test_a_sleeping_drive_never_read_has_no_figures_not_zeros);
  g_test_add_func ("/smart-reader/sat-drive-in-standby-gets-check-power-mode-and-nothing-else",
                   test_a_sat_drive_in_standby_gets_check_power_mode_and_nothing_else);
  g_test_add_func ("/smart-reader/sat-drive-awake-is-read-after-check-power-mode",
                   test_a_sat_drive_that_is_awake_is_read_after_check_power_mode);
  g_test_add_func ("/smart-reader/jmicron-sata-bridge-never-receives-the-nvme-tunnel",
                   test_a_jmicron_sata_bridge_never_receives_the_nvme_tunnel);
  g_test_add_func ("/smart-reader/non-good-scsi-status-is-failed-never-zeros",
                   test_a_non_good_scsi_status_is_failed_never_zeros);
  g_test_add_func ("/smart-reader/tunnel-answered-by-the-wrong-protocol-is-failed",
                   test_a_tunnel_answered_by_the_wrong_protocol_is_failed);
  g_test_add_func ("/smart-reader/all-zero-smart-log-is-garbled-not-healthy",
                   test_an_all_zero_smart_log_is_garbled_not_a_healthy_drive);
  g_test_add_func ("/smart-reader/implausible-temperature-or-spare-is-garbled",
                   test_an_implausible_temperature_or_spare_is_garbled);
  g_test_add_func ("/smart-reader/temperature-of-zero-is-absent-not-zero-kelvin",
                   test_a_temperature_of_zero_is_absent_not_zero_kelvin);
  g_test_add_func ("/smart-reader/counter-below-last-good-value-is-garbled",
                   test_a_counter_below_the_last_good_value_is_garbled);
  g_test_add_func ("/smart-reader/counter-that-overflows-bytes-is-garbled",
                   test_a_counter_that_overflows_bytes_is_garbled);
  g_test_add_func ("/smart-reader/bad-ata-checksum-is-garbled",
                   test_a_bad_ata_checksum_is_garbled);
  g_test_add_func ("/smart-reader/nvme-and-internal-scsi-disks-are-skipped",
                   test_nvme_disks_and_internal_scsi_disks_are_skipped);
  g_test_add_func ("/smart-reader/usb-cd-drive-is-not-read",
                   test_a_usb_cd_drive_is_not_read);
  g_test_add_func ("/smart-reader/asmedia-nvme-figures-are-read-with-units",
                   test_asmedia_nvme_figures_are_read_and_written_with_units);
  g_test_add_func ("/smart-reader/unlisted-x9-is-read-through-asmedia-and-marked-observed",
                   test_the_unlisted_x9_is_read_through_asmedia_and_marked_observed);
  g_test_add_func ("/smart-reader/x9-entry-does-not-match-another-firmware-revision",
                   test_the_x9_entry_does_not_match_another_firmware_revision);
  g_test_add_func ("/smart-reader/realtek-reads-never-ask-for-more-than-512-bytes",
                   test_realtek_reads_never_ask_for_more_than_512_bytes);
  g_test_add_func ("/smart-reader/jmicron-nvme-runs-three-phases-and-checks-the-completion",
                   test_jmicron_nvme_runs_three_phases_and_checks_the_completion);
  g_test_add_func ("/smart-reader/strings-from-drives-cannot-forge-keys-or-groups",
                   test_strings_from_drives_cannot_forge_keys_or_groups);
  g_test_add_func ("/smart-reader/file-is-replaced-atomically-with-mode-0644",
                   test_the_file_is_replaced_atomically_with_mode_0644);
  g_test_add_func ("/smart-reader/failed-write-leaves-the-old-file-untouched",
                   test_a_failed_write_leaves_the_old_file_untouched);
  g_test_add_func ("/smart-reader/failed-drive-keeps-the-time-it-last-read-ok",
                   test_a_failed_drive_keeps_the_time_it_last_read_ok);
  g_test_add_func ("/smart-reader/different-drive-on-the-same-name-inherits-nothing",
                   test_a_different_drive_on_the_same_name_inherits_nothing);
  g_test_add_func ("/smart-reader/same-drive-is-matched-though-its-own-serial-differs-from-the-usb-one",
                   test_the_same_drive_is_matched_though_its_own_serial_differs_from_the_usb_one);
  g_test_add_func ("/smart-reader/table-has-no-duplicate-keys-and-names-every-entry",
                   test_the_table_has_no_duplicate_keys_and_names_every_entry);
  g_test_add_func ("/smart-reader/ambiguous-or-unsupported-ids-are-not-in-the-table",
                   test_ambiguous_or_unsupported_drivedb_ids_are_not_in_the_table);
  g_test_add_func ("/smart-reader/jmicron-sata-ids-map-to-sat-and-never-to-a-tunnel",
                   test_jmicron_sata_ids_map_to_sat_and_never_to_a_tunnel);

  return g_test_run ();
}
