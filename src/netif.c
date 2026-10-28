/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * See netif.h. Source: Documentation/ABI/testing/sysfs-class-net.
 */

#include "netif.h"

#include <string.h>

gboolean
gsm_netif_is_physical (const char *sysfs_root,
                       const char *ifname)
{
  gboolean found;
  char *path;

  if (ifname == NULL || ifname[0] == '\0' || strchr (ifname, '/') != NULL
      || g_str_equal (ifname, ".") || g_str_equal (ifname, ".."))
    return FALSE;

  /* The interface can vanish between listing and this check: not found is
   * a normal outcome, and the interface is then simply not counted. */
  path = g_build_filename (sysfs_root, "class", "net", ifname, "device", NULL);
  found = g_file_test (path, G_FILE_TEST_EXISTS);
  g_free (path);
  return found;
}
