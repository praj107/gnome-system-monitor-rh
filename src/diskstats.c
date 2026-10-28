/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * See diskstats.h. Source: Documentation/admin-guide/iostats.rst.
 */

#include "diskstats.h"

#include <string.h>

/* The fields of one diskstats line, 0-based: major, minor, name, then the
 * counters of iostats.rst fields 1 to 11. */
#define FIELD_NAME 2
#define FIELD_SECTORS_READ 5
#define FIELD_SECTORS_WRITTEN 9
#define MIN_FIELDS 14

#define SECTOR_BYTES 512

/* Name prefixes that are never storage on their own. "dm-" and "md" are
 * followed by a number; "loop", "ram" and "zram" are matched as prefixes. */
static const char *const NON_STORAGE_PREFIXES[] = {
  "loop", "ram", "zram", "dm-", "md", NULL
};

gboolean
gsm_diskstats_is_physical_disk (const char *name,
                                const char *sysfs_root)
{
  gboolean found;
  char *path;

  if (name == NULL || name[0] == '\0' || strchr (name, '/') != NULL
      || g_str_equal (name, ".") || g_str_equal (name, ".."))
    return FALSE;

  for (guint i = 0; NON_STORAGE_PREFIXES[i] != NULL; i++)
    {
      const char *prefix = NON_STORAGE_PREFIXES[i];

      if (!g_str_has_prefix (name, prefix))
        continue;
      /* "md" must be followed by a digit: it is the md driver's naming,
       * not a prefix of every name starting with those letters. */
      if (g_str_equal (prefix, "md") && !g_ascii_isdigit (name[2]))
        continue;
      return FALSE;
    }

  path = g_build_filename (sysfs_root, "block", name, NULL);
  found = g_file_test (path, G_FILE_TEST_IS_DIR);
  g_free (path);
  return found;
}

static gboolean
parse_counter (const char *token,
               guint64    *out)
{
  char *end = NULL;

  if (token[0] < '0' || token[0] > '9')
    return FALSE;
  *out = g_ascii_strtoull (token, &end, 10);
  return *end == '\0';
}

/* Fields of one line, split on runs of blanks; NULL-terminated. */
static char **
split_fields (const char *line)
{
  char **raw = g_strsplit_set (line, " \t", -1);
  guint kept = 0;

  for (guint i = 0; raw[i] != NULL; i++)
    {
      if (raw[i][0] == '\0')
        {
          g_free (raw[i]);
          continue;
        }
      raw[kept++] = raw[i];
    }
  raw[kept] = NULL;
  return raw;
}

static guint64
name_hash (const char *name)
{
  /* Fowler-Noll-Vo 64-bit, FNV-1a. Summed over the set so the order of the
   * lines does not matter. */
  guint64 hash = G_GUINT64_CONSTANT (0xcbf29ce484222325);

  for (; *name != '\0'; name++)
    hash = (hash ^ (guchar) *name) * G_GUINT64_CONSTANT (0x00000100000001b3);
  return hash;
}

/* Add one line's counters; FALSE when the line is malformed. */
static gboolean
add_line (const char    *line,
          const char    *sysfs_root,
          GsmDiskTotals *totals)
{
  char **fields = split_fields (line);
  guint64 major, minor, sectors_read, sectors_written;
  gboolean ok = FALSE;

  if (g_strv_length (fields) < MIN_FIELDS)
    goto out;
  if (!parse_counter (fields[0], &major) || !parse_counter (fields[1], &minor)
      || !parse_counter (fields[FIELD_SECTORS_READ], &sectors_read)
      || !parse_counter (fields[FIELD_SECTORS_WRITTEN], &sectors_written))
    goto out;
  /* A count that cannot be turned into bytes is damage, not a figure. */
  if (sectors_read > G_MAXUINT64 / SECTOR_BYTES
      || sectors_written > G_MAXUINT64 / SECTOR_BYTES)
    goto out;
  ok = TRUE;

  if (!gsm_diskstats_is_physical_disk (fields[FIELD_NAME], sysfs_root))
    goto out;

  totals->read_bytes += sectors_read * SECTOR_BYTES;
  totals->written_bytes += sectors_written * SECTOR_BYTES;
  totals->n_disks++;
  totals->device_set_hash += name_hash (fields[FIELD_NAME]);

out:
  g_strfreev (fields);
  return ok;
}

void
gsm_diskstats_parse (const char    *text,
                     const char    *sysfs_root,
                     GsmDiskTotals *out)
{
  char **lines;

  memset (out, 0, sizeof *out);
  if (text == NULL)
    return;

  lines = g_strsplit (text, "\n", -1);
  for (guint i = 0; lines[i] != NULL; i++)
    {
      if (g_strstrip (lines[i])[0] == '\0')
        continue;
      if (!add_line (lines[i], sysfs_root, out))
        out->n_malformed_lines++;
    }
  g_strfreev (lines);
}

gboolean
gsm_diskstats_read (const char    *diskstats_path,
                    const char    *sysfs_root,
                    GsmDiskTotals *out)
{
  char *text = NULL;

  memset (out, 0, sizeof *out);
  /* The file can vanish with the kernel config; ENOENT is a normal outcome. */
  if (!g_file_get_contents (diskstats_path, &text, NULL, NULL))
    return FALSE;
  gsm_diskstats_parse (text, sysfs_root, out);
  g_free (text);
  return TRUE;
}
