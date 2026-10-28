/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Which network interfaces are physical, for the Resources headline. An
 * interface is physical when the kernel gives it a backing device, that is
 * when /sys/class/net/<name>/device exists (Documentation/ABI/testing/
 * sysfs-class-net lists the files of an interface; virtual ones such as
 * loopback, bridges, veth pairs, tun/tap and VPN tunnels have no device).
 * Counting those too would count the same bytes twice: once on the veth or
 * tunnel and again on the physical link they travel over.
 *
 * Pure function, no GTK: the sysfs root is a parameter so tests use fixture
 * trees.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Whether ifname is a physical interface under sysfs_root (normally
 * "/sys"). A name that is empty or could escape the directory is not. */
gboolean gsm_netif_is_physical (const char *sysfs_root,
                                const char *ifname);

G_END_DECLS
