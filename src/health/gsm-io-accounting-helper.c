/*
 * gsm-io-accounting — enable per-app I/O accounting for user sessions.
 *
 * Runs as root via pkexec, writes two systemd drop-ins, reloads the manager,
 * and exits.
 *
 * Why this is needed: systemd delegates cpu, memory and pids into user.slice
 * but not io. So io.stat exists at /sys/fs/cgroup/user.slice and nowhere below
 * it, and the entire desktop session — every browser, editor and compiler —
 * collapses into a single opaque number that cannot be attributed to any app.
 * Turning on IOAccounting and adding io to the delegated controllers makes the
 * kernel keep per-app, per-device counters, which is the difference between
 * "your session wrote 460 GB" and "Chromium wrote 340 GB to your NVMe".
 *
 * This is the only thing in the project that modifies system configuration, so
 * it does exactly this, writes only files it owns, and is idempotent.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * Both drop-ins are needed and neither is sufficient alone:
 *
 *  - user@.service must *delegate* the io controller into the user manager,
 *    otherwise the user manager may not touch io at all;
 *  - user-.slice must *account* for io, which is what makes systemd enable the
 *    controller in the slice's subtree_control in the first place.
 */
static const struct {
  const char *dir;
  const char *file;
  const char *contents;
} drop_ins[] = {
  {
    "/etc/systemd/system/user@.service.d",
    "50-gnome-system-monitor-io-accounting.conf",
    "# Written by gnome-system-monitor-rh so that disk writes can be\n"
    "# attributed to individual applications. Delete this file to undo.\n"
    "[Service]\n"
    "IOAccounting=yes\n"
    "Delegate=cpu cpuset io memory pids\n"
  },
  {
    "/etc/systemd/system/user-.slice.d",
    "50-gnome-system-monitor-io-accounting.conf",
    "# Written by gnome-system-monitor-rh so that disk writes can be\n"
    "# attributed to individual applications. Delete this file to undo.\n"
    "[Slice]\n"
    "IOAccounting=yes\n"
  },
};


static int
write_drop_in (const char *dir,
               const char *file,
               const char *contents)
{
  char path[512];
  FILE *out;

  if (mkdir (dir, 0755) != 0 && errno != EEXIST) {
    fprintf (stderr, "gsm-io-accounting: cannot create %s: %s\n",
             dir, strerror (errno));
    return 1;
  }

  if (snprintf (path, sizeof path, "%s/%s", dir, file) >= (int) sizeof path)
    return 1;

  out = fopen (path, "w");
  if (out == NULL) {
    fprintf (stderr, "gsm-io-accounting: cannot write %s: %s\n",
             path, strerror (errno));
    return 1;
  }

  if (fputs (contents, out) == EOF) {
    fclose (out);
    return 1;
  }

  if (fclose (out) != 0)
    return 1;

  if (chmod (path, 0644) != 0)
    return 1;

  printf ("wrote %s\n", path);

  return 0;
}


static int
daemon_reload (void)
{
  pid_t pid = fork ();
  int status;

  if (pid < 0)
    return 1;

  if (pid == 0) {
    execl ("/usr/bin/systemctl", "systemctl", "daemon-reload", (char *) NULL);
    execl ("/bin/systemctl", "systemctl", "daemon-reload", (char *) NULL);
    _exit (127);
  }

  if (waitpid (pid, &status, 0) < 0)
    return 1;

  return (WIFEXITED (status) && WEXITSTATUS (status) == 0) ? 0 : 1;
}


int
main (int argc,
      char **argv)
{
  (void) argv;

  /* Takes no arguments at all. There is nothing for a caller to influence, so
   * there is nothing for a caller to get wrong. */
  if (argc != 1) {
    fprintf (stderr, "usage: gsm-io-accounting\n");
    return 2;
  }

  for (size_t i = 0; i < sizeof drop_ins / sizeof drop_ins[0]; i++) {
    if (write_drop_in (drop_ins[i].dir, drop_ins[i].file,
                       drop_ins[i].contents) != 0)
      return 1;
  }

  if (daemon_reload () != 0) {
    fprintf (stderr, "gsm-io-accounting: daemon-reload failed\n");
    return 1;
  }

  /*
   * Deliberately does NOT restart user@.service: that would tear down the
   * caller's entire desktop session. The delegation change lands on the next
   * login, and the UI says so rather than pretending it took effect now.
   */
  printf ("ok\n");

  return 0;
}
