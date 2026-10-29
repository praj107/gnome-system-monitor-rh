/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Developer builds only: the `rows` verb of the control socket. It writes the
 * Processes tab's rows as TSV with the values the rows hold (the ProcInfo),
 * not the text the cells render, so that two sources of the figures can be
 * compared field by field. A figure the source did not provide is written as
 * the word `unknown`, never as 0.
 */

#include "config.h"

#ifdef HAVE_DEV_IPC

#include <glib/gstdio.h>

#include <cerrno>
#include <cstdio>
#include <string>

#include "dev-ipc-rows.h"
#include "proctable.h"
#include "settings-keys.h"

namespace {

// A tab or a newline in a process name would break the table.
std::string
tsv_text (const std::string &text)
{
  std::string clean = text;

  for (char &c : clean)
    if (c == '\t' || c == '\n' || c == '\r')
      c = ' ';
  return clean;
}

std::string
bytes_or_unknown (bool known, guint64 value)
{
  return known ? std::to_string (value) : std::string ("unknown");
}

std::string
setting_text (GSettings *settings, const char *key)
{
  g_autofree char *value = g_settings_get_string (settings, key);

  return value != NULL ? value : "";
}

struct RowsDump
{
  FILE *out;
  int   rows;
};

gboolean
write_row (GtkTreeModel *model,
           GtkTreePath  *,
           GtkTreeIter  *iter,
           gpointer      user_data)
{
  auto *dump = static_cast<RowsDump *>(user_data);
  gpointer pointer = NULL;

  gtk_tree_model_get (model, iter, COL_POINTER, &pointer, -1);
  if (pointer == NULL)
    return FALSE;

  const ProcInfo *info = static_cast<const ProcInfo *>(pointer);
  char cpu[32] = "unknown";

  if (info->cpu_known)
    g_snprintf (cpu, sizeof cpu, "%.4f", info->pcpu);

  std::fprintf (dump->out,
                "%d\t%d\t%s\t%s\t%u\t%u\t%d\t%u\t%s\t%" G_GUINT64_FORMAT
                "\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n",
                (int) info->pid, (int) info->ppid,
                bytes_or_unknown (info->start_time_known, info->start_time).c_str (),
                tsv_text (info->name).c_str (),
                info->uid, info->status, info->nice, info->num_threads,
                cpu, info->cpu_time,
                bytes_or_unknown (info->mem_known, info->mem).c_str (),
                bytes_or_unknown (info->memres_known, info->memres).c_str (),
                bytes_or_unknown (info->memshared_known, info->memshared).c_str (),
                bytes_or_unknown (info->vmsize_known, info->vmsize).c_str (),
                bytes_or_unknown (info->io_readable, info->disk_read_bytes_total).c_str (),
                bytes_or_unknown (info->io_readable, info->disk_write_bytes_total).c_str (),
                tsv_text (info->cgroup_name).c_str (),
                tsv_text (info->unit).c_str (),
                info->class_known ? tsv_text (info->class_name).c_str () : "unknown",
                info->class_known ? tsv_text (info->class_rule).c_str () : "unknown",
                tsv_text (info->user_unit).c_str (),
                tsv_text (info->session).c_str (),
                bytes_or_unknown (info->swap_known, info->swap).c_str ());
  dump->rows++;
  return FALSE;
}

}  // namespace

extern "C" int
gsm_dev_ipc_pid_column (void)
{
  return COL_PID;
}

extern "C" GtkTreeModel *
gsm_dev_ipc_base_store (GtkTreeView *view)
{
  GtkTreeModel *sorted = gtk_tree_view_get_model (view);

  if (!GTK_IS_TREE_MODEL_SORT (sorted))
    return NULL;

  GtkTreeModel *filtered = gtk_tree_model_sort_get_model (GTK_TREE_MODEL_SORT (sorted));

  if (!GTK_IS_TREE_MODEL_FILTER (filtered))
    return NULL;
  return gtk_tree_model_filter_get_model (GTK_TREE_MODEL_FILTER (filtered));
}

extern "C" int
gsm_dev_ipc_write_rows (GtkTreeModel *store,
                        const char   *path,
                        GError      **error)
{
  RowsDump dump = { NULL, 0 };

  dump.out = g_fopen (path, "w");
  if (dump.out == NULL)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "could not open %s: %s", path, g_strerror (errno));
      return -1;
    }

  g_autoptr (GSettings) settings = g_settings_new (GSM_GSETTINGS_SCHEMA);

  std::fprintf (dump.out, "# process-source=%s show-whose-processes=%s\n",
                setting_text (settings, GSM_SETTING_PROCESS_SOURCE).c_str (),
                setting_text (settings, GSM_SETTING_SHOW_WHOSE_PROCESSES).c_str ());
  std::fputs ("pid\tppid\tstart_time_s\tname\tuid\tstatus\tnice\tthreads\t"
              "cpu_percent\tcpu_time_ticks\tprivate_bytes\tresident_bytes\t"
              "shared_bytes\tvirtual_bytes\tdisk_read_bytes\tdisk_write_bytes\t"
              "cgroup\tunit\tclass\trule\tuser_unit\tsession\tswap_bytes\n", dump.out);

  gtk_tree_model_foreach (store, write_row, &dump);

  if (std::fclose (dump.out) != 0)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "could not write %s: %s", path, g_strerror (errno));
      return -1;
    }
  return dump.rows;
}

#endif /* HAVE_DEV_IPC */
