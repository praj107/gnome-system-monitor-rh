/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Fixture sysfs-net/class/net lists the interfaces of the development
 * machine (2026-10-28): two physical ones (enp63s0, wlp62s0), which have a
 * device directory, and virtual ones (loopback, docker and libvirt bridges,
 * a veth, a tun and a WireGuard tunnel), which do not. argv[1] is the
 * fixtures directory.
 */

#include <glib.h>

#include "../netif.h"

static const char *fixtures_dir;

static gboolean
physical (const char *ifname)
{
  char *root = g_build_filename (fixtures_dir, "sysfs-net", NULL);
  gboolean result = gsm_netif_is_physical (root, ifname);

  g_free (root);
  return result;
}

static void
test_ethernet_and_wifi_with_a_device_are_physical (void)
{
  g_assert_true (physical ("enp63s0"));
  g_assert_true (physical ("wlp62s0"));
}

static void
test_loopback_bridges_veth_tun_and_vpn_are_not_physical (void)
{
  const char *virtual_names[] = {
    "lo", "docker0", "br-75fa31d95c87", "veth1a2b3c", "tun0", "virbr0", "wg0", NULL
  };

  for (guint i = 0; virtual_names[i] != NULL; i++)
    g_assert_false (physical (virtual_names[i]));
}

static void
test_an_interface_that_vanished_is_not_physical (void)
{
  g_assert_false (physical ("enp99s0"));
}

static void
test_names_that_could_escape_the_directory_are_not_physical (void)
{
  g_assert_false (physical (""));
  g_assert_false (physical (".."));
  g_assert_false (physical ("enp63s0/device/.."));
  g_assert_false (physical (NULL));
}

int
main (int    argc,
      char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_assert_cmpint (argc, >=, 2);
  fixtures_dir = argv[1];

  g_test_add_func ("/netif/ethernet-and-wifi-with-a-device-are-physical", test_ethernet_and_wifi_with_a_device_are_physical);
  g_test_add_func ("/netif/loopback-bridges-veth-tun-and-vpn-are-not", test_loopback_bridges_veth_tun_and_vpn_are_not_physical);
  g_test_add_func ("/netif/vanished-interface-is-not-physical", test_an_interface_that_vanished_is_not_physical);
  g_test_add_func ("/netif/escaping-names-are-not-physical", test_names_that_could_escape_the_directory_are_not_physical);
  return g_test_run ();
}
