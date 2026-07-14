/*
 * Per-process block I/O from /proc/<pid>/io.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include "gsm-procio.h"

void
gsm_proc_io_entry_free (GsmProcIoEntry *entry)
{
  if (entry == NULL)
    return;

  g_free (entry->name);
  g_free (entry);
}


/*
 * /sys/dev/block/<maj>:<min> is a symlink into the device tree. For a
 * partition it lands inside its parent disk's directory and carries a
 * "partition" file; for a whole disk it doesn't. That gives us a cheap,
 * driver-agnostic way to walk from a mounted filesystem back to the physical
 * drive, without hardcoding naming rules like "strip pN from nvme0n1p2".
 */
gboolean
gsm_disk_for_devt (guint  dev_major,
                   guint  dev_minor,
                   guint *out_major,
                   guint *out_minor)
{
  g_autofree char *sys_path = NULL;
  g_autofree char *real_path = NULL;
  g_autofree char *partition_marker = NULL;
  g_autofree char *parent = NULL;
  g_autofree char *parent_dev = NULL;
  g_autofree char *contents = NULL;
  guint pmaj, pmin;

  g_return_val_if_fail (out_major != NULL && out_minor != NULL, FALSE);

  /* Anonymous device (btrfs, overlayfs, tmpfs, ZFS): no block device exists,
   * so there is nothing honest to attribute to. */
  if (dev_major == 0)
    return FALSE;

  sys_path = g_strdup_printf ("/sys/dev/block/%u:%u", dev_major, dev_minor);

  real_path = realpath (sys_path, NULL);
  if (real_path == NULL)
    return FALSE;

  partition_marker = g_build_filename (real_path, "partition", NULL);

  if (!g_file_test (partition_marker, G_FILE_TEST_EXISTS)) {
    /* Already a whole disk. */
    *out_major = dev_major;
    *out_minor = dev_minor;
    return TRUE;
  }

  parent = g_path_get_dirname (real_path);
  parent_dev = g_build_filename (parent, "dev", NULL);

  if (!g_file_get_contents (parent_dev, &contents, NULL, NULL))
    return FALSE;

  if (sscanf (contents, "%u:%u", &pmaj, &pmin) != 2)
    return FALSE;

  *out_major = pmaj;
  *out_minor = pmin;

  return TRUE;
}


/*
 * Which drive is this process most likely writing to? Its working directory
 * is the best cheap signal we have — a build in ~/src writes to the drive
 * holding ~, a container writing into /var/lib/docker writes to whatever
 * holds /var. It is a guess, and the caller labels it as one.
 */
static void
guess_device (int             pid,
              GsmProcIoEntry *entry)
{
  g_autofree char *cwd = g_strdup_printf ("/proc/%d/cwd", pid);
  struct stat st;
  guint disk_major, disk_minor;

  if (stat (cwd, &st) != 0)
    return;

  if (gsm_disk_for_devt (major (st.st_dev), minor (st.st_dev),
                         &disk_major, &disk_minor)) {
    entry->major = disk_major;
    entry->minor = disk_minor;
  }
}


static char *
read_comm (int pid)
{
  g_autofree char *path = g_strdup_printf ("/proc/%d/comm", pid);
  g_autofree char *contents = NULL;

  if (!g_file_get_contents (path, &contents, NULL, NULL))
    return NULL;

  return g_strchomp (g_steal_pointer (&contents));
}


static gboolean
read_io (int             pid,
         GsmProcIoEntry *entry)
{
  g_autofree char *path = g_strdup_printf ("/proc/%d/io", pid);
  g_autofree char *contents = NULL;
  g_auto (GStrv) lines = NULL;
  gboolean found = FALSE;

  /* Not readable => not ours. That is expected for most of the process table
   * and is not an error; we simply cannot see other users' I/O without root,
   * and the sampler is deliberately unprivileged. */
  if (!g_file_get_contents (path, &contents, NULL, NULL))
    return FALSE;

  lines = g_strsplit (contents, "\n", -1);

  for (guint i = 0; lines[i] != NULL; i++) {
    /* write_bytes is what actually reached the block layer, unlike wchar
     * which counts writes into the page cache that may never hit the disk.
     * Wear is caused by the former. */
    if (g_str_has_prefix (lines[i], "write_bytes:")) {
      entry->wbytes = g_ascii_strtoull (lines[i] + 12, NULL, 10);
      found = TRUE;
    } else if (g_str_has_prefix (lines[i], "read_bytes:")) {
      entry->rbytes = g_ascii_strtoull (lines[i] + 11, NULL, 10);
    } else if (g_str_has_prefix (lines[i], "cancelled_write_bytes:")) {
      /* Pages that were dirtied and then truncated before writeback never
       * reached the platter. Charging them as wear would overstate it. */
      uint64_t cancelled = g_ascii_strtoull (lines[i] + 22, NULL, 10);

      entry->wbytes = entry->wbytes > cancelled ? entry->wbytes - cancelled : 0;
    }
  }

  return found;
}


GPtrArray *
gsm_proc_io_collect (void)
{
  GPtrArray *out =
    g_ptr_array_new_with_free_func ((GDestroyNotify) gsm_proc_io_entry_free);
  g_autoptr (GDir) proc = g_dir_open ("/proc", 0, NULL);
  const char *name;

  if (proc == NULL)
    return out;

  while ((name = g_dir_read_name (proc)) != NULL) {
    GsmProcIoEntry *entry;
    char *end = NULL;
    int64_t pid;

    pid = g_ascii_strtoll (name, &end, 10);
    if (pid <= 0 || end == NULL || *end != '\0')
      continue;

    entry = g_new0 (GsmProcIoEntry, 1);
    entry->pid = (int) pid;
    entry->exact = FALSE;

    if (!read_io ((int) pid, entry) || entry->wbytes == 0) {
      gsm_proc_io_entry_free (entry);
      continue;
    }

    entry->name = read_comm ((int) pid);
    if (entry->name == NULL) {
      /* Process exited between readdir and now — normal, just drop it. */
      gsm_proc_io_entry_free (entry);
      continue;
    }

    guess_device ((int) pid, entry);

    g_ptr_array_add (out, entry);
  }

  return out;
}
