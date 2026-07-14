/*
 * Per-device write attribution via cgroup v2 io.stat.
 *
 * cgroup v2 gives us, for free and without any privilege, exactly what we
 * need and what /proc/pid/io cannot provide: bytes written broken down by
 * *both* cgroup and block device. That is what lets us say "Chromium wrote
 * this much to *this specific SSD*" rather than just "Chromium writes a lot".
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>

#include "gsm-blame.h"

#define CGROUP_ROOT "/sys/fs/cgroup"

/* Guard against a pathological cgroup tree eating the sampler's wakeup. */
#define MAX_DEPTH   12


void
gsm_blame_entry_free (GsmBlameEntry *entry)
{
  if (entry == NULL)
    return;

  g_free (entry->cgroup);
  g_free (entry->name);
  g_free (entry);
}


/*
 * systemd mints a fresh cgroup for every launch, tagged with the PID:
 *
 *   app-org.chromium.Chromium-4126.scope    -> org.chromium.Chromium
 *   app-codium-875820.scope                 -> codium
 *   session-2.scope                         -> session
 *   docker-9f2c...a1.scope                  -> docker
 *   nginx.service                           -> nginx.service
 *
 * Without collapsing these, every restart would look like a brand new app and
 * the thirty-day totals would be meaningless confetti.
 */
char *
gsm_blame_normalize_name (const char *basename)
{
  const char *start = basename;
  const char *dot;
  g_autofree char *stem = NULL;
  char *dash;
  gboolean is_scope;

  g_return_val_if_fail (basename != NULL, NULL);

  dot = strrchr (basename, '.');
  is_scope = dot != NULL && g_strcmp0 (dot, ".scope") == 0;

  /* Services keep their suffix — "nginx.service" is how the user refers to
   * it, and systemd never appends an instance number to one. */
  if (!is_scope)
    return g_strdup (basename);

  if (g_str_has_prefix (start, "app-"))
    start += 4;

  stem = dot ? g_strndup (start, (gsize) (dot - start)) : g_strdup (start);

  /*
   * Strip a trailing instance tag: "-4126" (a PID) or "-9f2c3a..." (a
   * container hash). Only when what follows the last dash is *entirely*
   * alphanumeric-with-digits and long enough to not be part of a real name —
   * "gnome-shell" must survive, "codium-875820" must not.
   */
  dash = strrchr (stem, '-');
  if (dash != NULL && dash != stem) {
    const char *tag = dash + 1;
    gboolean all_digits = *tag != '\0';

    for (const char *p = tag; *p; p++) {
      if (!g_ascii_isdigit (*p)) {
        all_digits = FALSE;
        break;
      }
    }

    /* A pure number is always an instance tag. A long hex blob is a container
     * id. Anything else is part of the application's actual name. */
    if (all_digits) {
      *dash = '\0';
    } else if (strlen (tag) >= 12) {
      gboolean all_hex = TRUE;

      for (const char *p = tag; *p; p++) {
        if (!g_ascii_isxdigit (*p)) {
          all_hex = FALSE;
          break;
        }
      }
      if (all_hex)
        *dash = '\0';
    }
  }

  return g_steal_pointer (&stem);
}


/*
 * io.stat lines look like:
 *   259:0 rbytes=652146522624 wbytes=527674819584 rios=... wios=... dbytes=...
 */
static void
parse_io_stat (const char *contents,
               const char *cgroup_path,
               const char *name,
               GPtrArray  *out)
{
  g_auto (GStrv) lines = g_strsplit (contents, "\n", -1);

  for (guint i = 0; lines[i] != NULL; i++) {
    g_auto (GStrv) fields = NULL;
    GsmBlameEntry *entry;
    guint major, minor;
    uint64_t rbytes = 0, wbytes = 0;

    if (lines[i][0] == '\0')
      continue;

    fields = g_strsplit (lines[i], " ", -1);
    if (fields[0] == NULL)
      continue;

    if (sscanf (fields[0], "%u:%u", &major, &minor) != 2)
      continue;

    for (guint f = 1; fields[f] != NULL; f++) {
      if (g_str_has_prefix (fields[f], "rbytes="))
        rbytes = g_ascii_strtoull (fields[f] + 7, NULL, 10);
      else if (g_str_has_prefix (fields[f], "wbytes="))
        wbytes = g_ascii_strtoull (fields[f] + 7, NULL, 10);
    }

    /* A cgroup that has touched a device but written nothing to it is noise. */
    if (wbytes == 0 && rbytes == 0)
      continue;

    entry = g_new0 (GsmBlameEntry, 1);
    entry->cgroup = g_strdup (cgroup_path);
    entry->name = g_strdup (name);
    entry->major = major;
    entry->minor = minor;
    entry->rbytes = rbytes;
    entry->wbytes = wbytes;

    g_ptr_array_add (out, entry);
  }
}


/*
 * Resolve which unit a leaf cgroup belongs to by walking up to the nearest
 * ancestor that is a real unit. A service with its own sub-cgroups (docker,
 * a browser with per-tab scopes) should report as one thing, not as a dozen
 * anonymous leaves.
 */
static char *
resolve_unit_name (const char *relative_path)
{
  g_auto (GStrv) parts = g_strsplit (relative_path, "/", -1);
  int last = -1;

  for (int i = 0; parts[i] != NULL; i++) {
    if (g_str_has_suffix (parts[i], ".service") ||
        g_str_has_suffix (parts[i], ".scope"))
      last = i;
  }

  if (last >= 0)
    return gsm_blame_normalize_name (parts[last]);

  /* No unit ancestor: fall back to the outermost slice, so kernel and
   * init-owned I/O still lands somewhere honest rather than being dropped. */
  if (parts[0] != NULL && parts[0][0] != '\0')
    return g_strdup (parts[0]);

  return g_strdup ("kernel");
}


/*
 * Sample the *deepest* cgroups that still carry io.stat, and no shallower.
 *
 * io.stat is recursive — a parent's counters already include everything its
 * descendants did — so reading both a parent and its children would double
 * count. But we cannot simply read leaves either: the io controller is only
 * enabled where systemd delegated it, so on a stock desktop /user.slice has
 * io.stat while none of the app scopes beneath it do. A leaf-only walk would
 * silently drop every byte the user's own session wrote.
 *
 * So: recurse first, and only fall back to this cgroup's own io.stat if no
 * descendant supplied one. Returns TRUE if this subtree contributed anything.
 */
static gboolean
walk (const char *dir,
      const char *relative,
      int         depth,
      GPtrArray  *out)
{
  g_autoptr (GDir) handle = NULL;
  g_autoptr (GPtrArray) subdirs = g_ptr_array_new_with_free_func (g_free);
  g_autofree char *stat_path = NULL;
  g_autofree char *contents = NULL;
  g_autofree char *name = NULL;
  const char *child;
  gboolean descendants_reported = FALSE;

  if (depth > MAX_DEPTH)
    return FALSE;

  handle = g_dir_open (dir, 0, NULL);
  if (handle == NULL)
    return FALSE;

  while ((child = g_dir_read_name (handle)) != NULL) {
    g_autofree char *path = g_build_filename (dir, child, NULL);

    if (g_file_test (path, G_FILE_TEST_IS_DIR))
      g_ptr_array_add (subdirs, g_strdup (child));
  }

  for (guint i = 0; i < subdirs->len; i++) {
    const char *sub = g_ptr_array_index (subdirs, i);
    g_autofree char *path = g_build_filename (dir, sub, NULL);
    g_autofree char *rel = relative[0] == '\0'
                           ? g_strdup (sub)
                           : g_build_filename (relative, sub, NULL);

    if (walk (path, rel, depth + 1, out))
      descendants_reported = TRUE;
  }

  if (descendants_reported)
    return TRUE;

  stat_path = g_build_filename (dir, "io.stat", NULL);
  if (!g_file_get_contents (stat_path, &contents, NULL, NULL))
    return FALSE;

  /* The root cgroup's io.stat is the machine total. Attributing that to
   * anything would just be double counting everything at once. */
  if (relative[0] == '\0')
    return FALSE;

  name = resolve_unit_name (relative);

  {
    guint before = out->len;

    parse_io_stat (contents, relative, name, out);

    return out->len > before;
  }
}


GPtrArray *
gsm_blame_collect (void)
{
  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) gsm_blame_entry_free);

  walk (CGROUP_ROOT, "", 0, out);

  return out;
}


gboolean
gsm_blame_user_io_is_delegated (GPtrArray *cgroups)
{
  g_return_val_if_fail (cgroups != NULL, FALSE);

  /*
   * Anything *below* user.slice carrying io.stat means the controller was
   * delegated into the session. Without delegation the deepest cgroup with
   * io.stat is "user.slice" itself, with no trailing path.
   */
  for (guint i = 0; i < cgroups->len; i++) {
    GsmBlameEntry *entry = g_ptr_array_index (cgroups, i);

    if (g_str_has_prefix (entry->cgroup, "user.slice/"))
      return TRUE;
  }

  return FALSE;
}
