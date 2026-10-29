/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Writes done for someone else: who caused the bytes that two proxy writers
 * put on a drive. The kernel charges a core dump to the systemd-coredump
 * handler's cgroup and journald's syncs to journald's, so "charged to" rows
 * name the proxy. This module reassigns those bytes, from the journal, to
 * the units whose crashes and log lines caused them, and says on what basis.
 *
 * The journal records follow systemd.journal-fields(7) (the trusted fields
 * _SYSTEMD_UNIT, _SYSTEMD_USER_UNIT and the unit-scoped UNIT=, USER_UNIT=)
 * and the catalog ids of systemd's message catalog: a crash handled by
 * systemd-coredump(8) carries COREDUMP_* fields, and a unit's resource
 * summary carries IO_METRIC_WRITE_BYTES only when the unit has IO
 * accounting on.
 */

#pragma once

#include <stdint.h>

#include <glib.h>

G_BEGIN_DECLS

/* The proxy writers whose bytes are reassigned. */
typedef enum
{
  GSM_PROXY_COREDUMP,
  GSM_PROXY_JOURNALD,
  GSM_PROXY_COUNT,
} GsmProxyWriter;

/* The `via` column of a reassigned row: the proxy writer's unit name. For
 * journald that is also its charged-to row's name; systemd-coredump's bytes
 * are charged to its instances (`systemd-coredump@.service`) and to the
 * residue of `system-systemd-coredump.slice`, so readers group the
 * reassigned rows under the writer by this name, not by row name. */
const char *gsm_proxy_writer_name (GsmProxyWriter writer);

/* Is this cgroup (path relative to the cgroup root, as a delta names it) the
 * subtree of one of the proxy writers? */
gboolean    gsm_on_behalf_proxy_for_cgroup (const char     *cgroup,
                                            GsmProxyWriter *writer);

/* Names of the rows that cannot say who: they are explicit, never zero. */
#define GSM_ON_BEHALF_NO_ACCESS         "unknown: no journal access"
#define GSM_ON_BEHALF_NO_CRASH_RECORD   "unknown: no crash record in this interval"
#define GSM_ON_BEHALF_NO_JOURNAL_ENTRY  "unknown: no journal entries in this interval"
#define GSM_ON_BEHALF_NO_UNIT           "other journal sources"

#define GSM_MESSAGE_ID_COREDUMP "fc2e22bc6ee647b6b90729ab34a250b1"
#define GSM_MESSAGE_ID_RESOURCES "ae8f7b866b0347b9af31fe1c80b127c0"

/*
 * One journal entry, reduced to the fields this module reads. Any may be
 * NULL. The COREDUMP_* fields and the maps are filled only for a crash
 * record, IO_METRIC_WRITE_BYTES only for a resource summary.
 */
typedef struct
{
  char *message_id;
  char *unit;                /* _SYSTEMD_UNIT: who logged it */
  char *user_unit;           /* _SYSTEMD_USER_UNIT */
  char *about_unit;          /* UNIT=: the unit PID 1 is writing about */
  char *about_user_unit;     /* USER_UNIT=: the same from a user manager */
  char *coredump_unit;
  char *coredump_user_unit;
  char *coredump_cgroup;
  char *coredump_proc_maps;
  char *io_write_bytes;      /* decimal text, as journald stores it */
} GsmJournalRecord;

void gsm_journal_record_free (GsmJournalRecord *record);

/*
 * Where records come from. Tests feed synthetic ones; the sampler uses the
 * sd-journal reader below. `next` returns the next record or NULL at the
 * end; `end_cursor` returns a cursor to resume after what was read (NULL if
 * the source has none); `close` frees `data`.
 */
typedef struct
{
  GsmJournalRecord * (*next) (gpointer data);
  char *             (*end_cursor) (gpointer data);
  void               (*close) (gpointer data);
  gpointer data;
} GsmJournalSource;

void gsm_journal_source_free (GsmJournalSource *source);

/*
 * Open the journal of this machine through the sd-journal API (no
 * subprocess). With `after_cursor` the records after it are returned; with
 * `since_us` (microseconds since the epoch) the records from that time;
 * with neither the source returns nothing and only its end cursor is useful
 * (the baseline). NULL when the journal cannot be read (the user is not in
 * the `adm` or `systemd-journal` group, or systemd support is compiled out),
 * with a reason in `reason`.
 */
GsmJournalSource * gsm_journal_open_system (const char *after_cursor,
                                            int64_t     since_us,
                                            char      **reason);

/*
 * The bytes a default coredump_filter (0x33, proc_pid_coredump_filter(5)
 * and core(5)) dumps from a process whose /proc/<pid>/maps text is given:
 * anonymous private mappings, anonymous shared ones (/dev/zero, SysV and
 * memfd), and one page of each file-backed mapping that starts at offset 0
 * (the ELF headers, bit 4). Lines that do not parse add nothing.
 */
uint64_t gsm_on_behalf_estimate_core_bytes (const char *proc_maps);

typedef struct _GsmOnBehalf GsmOnBehalf;

GsmOnBehalf * gsm_on_behalf_new (void);
void          gsm_on_behalf_free (GsmOnBehalf *self);

/* Bytes the kernel charged to a proxy writer on a drive in this interval. */
void          gsm_on_behalf_charge (GsmOnBehalf    *self,
                                    const char     *drive_id,
                                    GsmProxyWriter  writer,
                                    uint64_t        wbytes);

gboolean      gsm_on_behalf_has_charges (GsmOnBehalf *self);

typedef void (*GsmOnBehalfSink) (const char *drive_id,
                                 const char *name,
                                 const char *via,
                                 const char *basis,
                                 uint64_t    wbytes,
                                 gpointer    user_data);

/*
 * Close the interval: split every proxy writer's charged bytes over
 * originators and hand each row to `sink`. Per drive and writer the rows
 * sum exactly to what was charged. `source` is NULL when the journal could
 * not be opened. The source is read only when something was charged.
 */
void          gsm_on_behalf_settle (GsmOnBehalf     *self,
                                    GsmJournalSource *source,
                                    GsmOnBehalfSink   sink,
                                    gpointer          user_data);

G_END_DECLS
