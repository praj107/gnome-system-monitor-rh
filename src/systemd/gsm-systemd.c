/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <errno.h>
#include <unistd.h>

#include <glib/gstdio.h>
#include <systemd/sd-login.h>

#include "gsm-systemd.h"


gboolean
gsm_systemd_is_running (void)
{
  enum {
    UNINIT = 0,
    RUNNING,
    NOT_RUNNING,
  };
  static size_t status = UNINIT;

  if (g_once_init_enter (&status)) {
    gboolean is_running = g_access ("/run/systemd/seats/", F_OK) >= 0;

    g_once_init_leave (&status, is_running ? RUNNING : NOT_RUNNING);
  }

  return status == RUNNING;
}


/* The seat of a login session never changes while the session exists (logind
 * assigns it at creation, sd_session_get_seat(3)), so one lookup per session
 * id serves the life of the program. A session without a seat (a remote
 * login) is remembered as the empty string. A failed lookup (the session
 * ended, logind is busy) is not remembered, so the next refresh asks again.
 * The caller runs on the main thread, so the table needs no lock. */
const char *
gsm_systemd_get_session_seat (const char *session)
{
  static GHashTable *seats = NULL;
  char *seat = NULL;
  const char *known;
  int r;

  if (session == NULL || session[0] == '\0' || !gsm_systemd_is_running ())
    return "";

  if (seats == NULL)
    seats = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  if (g_hash_table_lookup_extended (seats, session, NULL, (gpointer *) &known))
    return known;

  r = sd_session_get_seat (session, &seat);
  if (r < 0 && r != -ENODATA)
    return "";

  g_hash_table_insert (seats, g_strdup (session), g_strdup (r >= 0 ? seat : ""));
  free (seat);

  g_hash_table_lookup_extended (seats, session, NULL, (gpointer *) &known);
  return known;
}


gboolean
gsm_systemd_get_process_info (pid_t     pid,
                              char    **sd_unit,
                              char    **sd_session,
                              char    **sd_seat,
                              uid_t    *sd_owner)
{
  char *unit = NULL;
  char *session = NULL;
  char *seat = NULL;
  uid_t uid;

  if (!gsm_systemd_is_running ()) {
    g_set_str (sd_unit, NULL);
    g_set_str (sd_session, NULL);
    g_set_str (sd_seat, NULL);

    return FALSE;
  }

  sd_pid_get_unit (pid, &unit);
  g_set_str (sd_unit, unit);

  sd_pid_get_session (pid, &session);
  g_set_str (sd_session, session);

  if (session) {
    sd_session_get_seat (session, &seat);
    g_set_str (sd_seat, seat);
  }

  if (sd_pid_get_owner_uid (pid, &uid) >= 0) {
    *sd_owner = uid;
  }

  /* These are raw libc values, not glib, so we must manually free them */
  g_clear_pointer (&unit, free);
  g_clear_pointer (&session, free);
  g_clear_pointer (&seat, free);

  return TRUE;
}

