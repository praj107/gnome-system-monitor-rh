/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * See psi.h. Source: Documentation/accounting/psi.rst.
 */

#include "psi.h"

#include <stdlib.h>
#include <string.h>

/* Parse "avg10=1.43 avg60=1.15 avg300=0.41 total=73843470" (after the
 * "some "/"full " word). Both avg10 and total must be there. */
static gboolean
parse_line_fields (const char *fields,
                   GsmPsiLine *line)
{
  gboolean have_avg10 = FALSE;
  gboolean have_total = FALSE;
  gchar **tokens = g_strsplit_set (fields, " \t\n", -1);

  for (guint i = 0; tokens[i] != NULL; i++)
    {
      char *end = NULL;

      if (g_str_has_prefix (tokens[i], "avg10="))
        {
          line->avg10 = g_ascii_strtod (tokens[i] + strlen ("avg10="), &end);
          have_avg10 = end != tokens[i] + strlen ("avg10=") && *end == '\0';
        }
      else if (g_str_has_prefix (tokens[i], "total="))
        {
          const char *digits = tokens[i] + strlen ("total=");
          line->total_us = g_ascii_strtoull (digits, &end, 10);
          have_total = end != digits && *end == '\0';
        }
    }
  g_strfreev (tokens);

  line->present = have_avg10 && have_total;
  return line->present;
}

gboolean
gsm_psi_parse (const char *text,
               GsmPsi     *out)
{
  gchar **lines;

  memset (out, 0, sizeof (*out));
  if (text == NULL)
    return FALSE;

  lines = g_strsplit (text, "\n", -1);
  for (guint i = 0; lines[i] != NULL; i++)
    {
      if (g_str_has_prefix (lines[i], "some "))
        parse_line_fields (lines[i] + strlen ("some "), &out->some);
      else if (g_str_has_prefix (lines[i], "full "))
        parse_line_fields (lines[i] + strlen ("full "), &out->full);
    }
  g_strfreev (lines);

  return out->some.present;
}

gboolean
gsm_psi_read (const char *path,
              GsmPsi     *out)
{
  char *text = NULL;
  gboolean ok;

  memset (out, 0, sizeof (*out));
  if (!g_file_get_contents (path, &text, NULL, NULL))
    return FALSE;
  ok = gsm_psi_parse (text, out);
  g_free (text);
  return ok;
}

gboolean
gsm_psi_window_share (const GsmPsiLine *before,
                      const GsmPsiLine *after,
                      gint64            elapsed_us,
                      double           *percent_out)
{
  double percent;

  if (!before->present || !after->present || elapsed_us <= 0)
    return FALSE;
  if (after->total_us < before->total_us)
    return FALSE;

  percent = 100.0 * (double) (after->total_us - before->total_us) / (double) elapsed_us;
  *percent_out = MIN (percent, 100.0);
  return TRUE;
}
