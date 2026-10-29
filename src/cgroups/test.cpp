/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <string.h>

#include <glib.h>

#include "gsm-cgroups.cpp"


static void
test_cgroups_v2 (void)
{
  g_assert_cmpstr (extract_name ("0::/\n"), ==, "");
  g_assert_cmpstr (extract_name ("0::/\n"), ==, "");
}


static void
test_cgroups_v1_one_entry (void)
{
  g_assert_cmpstr (extract_name ("0:a1:/b1\n"), ==, "/b1 (a1)");
  g_assert_cmpstr (extract_name ("0:name=a2:/b2\n"), ==, "/b2 (a2)");
}


static void
test_cgroups_v1_multiple_entry (void)
{
  g_assert_cmpstr (extract_name ("1:a1:/b1\n0:a0:/b0\n"), ==, "/b0 (a0), /b1 (a1)");
}


static void
test_name_from_path_matches_the_file_reader (void)
{
  const char *paths[] = { "/", "/user.slice/user-1000.slice/session-2.scope", "/system.slice/ssh.service" };

  for (const char *path : paths)
    {
      g_autofree char *line = g_strdup_printf ("0::%s\n", path);
      g_autofree char *name = gsm_cgroups_name_from_path (path, strlen (path));

      g_assert_cmpstr (name, ==, extract_name (line));
    }

  g_autofree char *empty = gsm_cgroups_name_from_path ("", 0);

  g_assert_cmpstr (empty, ==, "");

  // the length bounds the name: the path is not NUL-terminated in the snapshot
  g_autofree char *cut = gsm_cgroups_name_from_path ("/a/b/c", 4);

  g_assert_cmpstr (cut, ==, "/a/b");
}


int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/gnome-system-monitor/cgroups/v2", test_cgroups_v2);
  g_test_add_func ("/gnome-system-monitor/cgroups/v1/one-entry",
                   test_cgroups_v1_one_entry);
  g_test_add_func ("/gnome-system-monitor/cgroups/v1/multiple-entry",
                   test_cgroups_v1_multiple_entry);
  g_test_add_func ("/gnome-system-monitor/cgroups/name-from-path",
                   test_name_from_path_matches_the_file_reader);

  return g_test_run ();
}
