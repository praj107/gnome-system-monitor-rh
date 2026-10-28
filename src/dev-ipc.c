/*
 * Developer control socket — see dev-ipc.h for the two locks that keep this
 * out of shipped builds.
 *
 * Protocol: newline-delimited text over a Unix socket. One command per line,
 * one reply line back, starting with "OK" or "ERR". Deliberately dumb, so a
 * harness can be twenty lines of Python with no dependencies.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#ifdef HAVE_DEV_IPC

#include <adwaita.h>
#include <errno.h>
#include <glib/gstdio.h>
#include <sys/stat.h>

#include "dev-ipc.h"
#include "settings-keys.h"

typedef struct {
  GtkWindow *window;
  GSocketService *service;
  char *socket_path;
} DevIpc;

static DevIpc *the_ipc = NULL;


/* ------------------------------------------------------------------ */
/* Widget tree helpers                                                 */
/* ------------------------------------------------------------------ */

static GtkWidget *
find_descendant (GtkWidget *root,
                 GType      type)
{
  if (root == NULL)
    return NULL;

  if (G_TYPE_CHECK_INSTANCE_TYPE (root, type))
    return root;

  for (GtkWidget *child = gtk_widget_get_first_child (root);
       child != NULL;
       child = gtk_widget_get_next_sibling (child)) {
    GtkWidget *found = find_descendant (child, type);

    if (found != NULL)
      return found;
  }

  return NULL;
}


/*
 * Find a widget by the text a human would see on it. Matching on labels rather
 * than on internal ids means the harness scripts read like what a user does
 * ("click Unlock Full SMART…"), and they break loudly if the button they name
 * stops existing — which is usually the bug you wanted to catch anyway.
 */
static GtkWidget *
find_by_label (GtkWidget  *root,
               const char *needle,
               GType       type)
{
  const char *text = NULL;

  if (root == NULL)
    return NULL;

  if (G_TYPE_CHECK_INSTANCE_TYPE (root, type)) {
    if (GTK_IS_BUTTON (root))
      text = gtk_button_get_label (GTK_BUTTON (root));
    else if (GTK_IS_LABEL (root))
      text = gtk_label_get_text (GTK_LABEL (root));
    else if (ADW_IS_PREFERENCES_ROW (root))
      text = adw_preferences_row_get_title (ADW_PREFERENCES_ROW (root));

    if (text != NULL && strstr (text, needle) != NULL)
      return root;
  }

  for (GtkWidget *child = gtk_widget_get_first_child (root);
       child != NULL;
       child = gtk_widget_get_next_sibling (child)) {
    GtkWidget *found = find_by_label (child, needle, type);

    if (found != NULL)
      return found;
  }

  return NULL;
}


static AdwViewStack *
get_stack (DevIpc *self)
{
  return ADW_VIEW_STACK (find_descendant (GTK_WIDGET (self->window),
                                          ADW_TYPE_VIEW_STACK));
}


/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

/*
 * Render the window straight from its widget tree into a PNG.
 *
 * Deliberately not an X11/Wayland screen grab: this reads the same render
 * nodes GTK is about to draw, so it captures the true current UI state even
 * when the window is occluded, unfocused, or on another workspace — and it
 * cannot accidentally capture the developer's other windows.
 */
static char *
cmd_shot (DevIpc     *self,
          const char *path)
{
  GtkWidget *widget = GTK_WIDGET (self->window);
  g_autoptr (GdkPaintable) paintable = NULL;
  g_autoptr (GskRenderNode) node = NULL;
  g_autoptr (GdkTexture) texture = NULL;
  GtkSnapshot *snapshot;
  GskRenderer *renderer;
  int width, height;

  if (path == NULL || *path == '\0')
    return g_strdup ("ERR shot needs a path\n");

  width = gtk_widget_get_width (widget);
  height = gtk_widget_get_height (widget);

  if (width <= 0 || height <= 0)
    return g_strdup ("ERR window has no size yet\n");

  renderer = gtk_native_get_renderer (GTK_NATIVE (self->window));
  if (renderer == NULL)
    return g_strdup ("ERR window has no renderer\n");

  /* A widget paintable draws the live widget tree on demand — the same nodes
   * GTK itself is about to render — which is what makes this work while the
   * window is occluded or unfocused. */
  paintable = gtk_widget_paintable_new (widget);

  snapshot = gtk_snapshot_new ();
  gdk_paintable_snapshot (paintable, snapshot, width, height);

  /* An empty node means GTK had nothing to draw — usually the window has not
   * been mapped yet. Saying so beats writing a blank PNG and calling it a
   * pass. */
  node = gtk_snapshot_free_to_node (snapshot);
  if (node == NULL)
    return g_strdup ("ERR nothing to render\n");

  texture = gsk_renderer_render_texture (renderer, node, NULL);
  if (texture == NULL)
    return g_strdup ("ERR render failed\n");

  if (!gdk_texture_save_to_png (texture, path))
    return g_strdup_printf ("ERR could not write %s\n", path);

  return g_strdup_printf ("OK %dx%d %s\n", width, height, path);
}


static char *
cmd_tab (DevIpc     *self,
         const char *name)
{
  AdwViewStack *stack = get_stack (self);

  if (stack == NULL)
    return g_strdup ("ERR no view stack\n");

  if (name == NULL || *name == '\0') {
    /* No argument: report what is currently showing, and what else exists. */
    GString *out = g_string_new ("OK ");
    GtkSelectionModel *pages = adw_view_stack_get_pages (stack);
    guint n = g_list_model_get_n_items (G_LIST_MODEL (pages));

    g_string_append_printf (out, "current=%s available=",
                            adw_view_stack_get_visible_child_name (stack));

    for (guint i = 0; i < n; i++) {
      g_autoptr (AdwViewStackPage) page =
        g_list_model_get_item (G_LIST_MODEL (pages), i);

      g_string_append_printf (out, "%s%s",
                              i > 0 ? "," : "",
                              adw_view_stack_page_get_name (page));
    }
    g_string_append_c (out, '\n');

    return g_string_free (out, FALSE);
  }

  if (adw_view_stack_get_child_by_name (stack, name) == NULL)
    return g_strdup_printf ("ERR no such tab: %s\n", name);

  adw_view_stack_set_visible_child_name (stack, name);

  return g_strdup_printf ("OK tab=%s\n", name);
}


static char *
cmd_scroll (DevIpc     *self,
            const char *arg)
{
  AdwViewStack *stack = get_stack (self);
  GtkWidget *visible;
  GtkWidget *scroller;
  GtkAdjustment *adjustment;
  double value;

  if (stack == NULL)
    return g_strdup ("ERR no view stack\n");

  visible = adw_view_stack_get_visible_child (stack);
  scroller = find_descendant (visible, GTK_TYPE_SCROLLED_WINDOW);

  if (scroller == NULL)
    return g_strdup ("ERR visible tab does not scroll\n");

  adjustment =
    gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (scroller));

  if (g_strcmp0 (arg, "top") == 0)
    value = gtk_adjustment_get_lower (adjustment);
  else if (g_strcmp0 (arg, "bottom") == 0)
    value = gtk_adjustment_get_upper (adjustment) -
            gtk_adjustment_get_page_size (adjustment);
  else
    value = g_ascii_strtod (arg, NULL);

  gtk_adjustment_set_value (adjustment, value);

  return g_strdup_printf ("OK scroll=%.0f of %.0f\n",
                          gtk_adjustment_get_value (adjustment),
                          gtk_adjustment_get_upper (adjustment) -
                          gtk_adjustment_get_page_size (adjustment));
}


static char *
cmd_click (DevIpc     *self,
           const char *label)
{
  GtkWidget *button;

  if (label == NULL || *label == '\0')
    return g_strdup ("ERR click needs a label\n");

  button = find_by_label (GTK_WIDGET (self->window), label, GTK_TYPE_BUTTON);
  if (button == NULL)
    return g_strdup_printf ("ERR no button labelled: %s\n", label);

  if (!gtk_widget_get_sensitive (button))
    return g_strdup_printf ("ERR button is insensitive: %s\n", label);

  /* Emit the action rather than faking a pointer event: it is what the button
   * would do anyway, and it cannot miss. */
  gtk_widget_activate (button);

  return g_strdup_printf ("OK clicked %s\n", label);
}


static char *
cmd_toggle (DevIpc     *self,
            const char *title)
{
  GtkWidget *row;
  gboolean active;

  if (title == NULL || *title == '\0')
    return g_strdup ("ERR toggle needs a row title\n");

  row = find_by_label (GTK_WIDGET (self->window), title, ADW_TYPE_SWITCH_ROW);
  if (row == NULL)
    return g_strdup_printf ("ERR no switch row titled: %s\n", title);

  active = !adw_switch_row_get_active (ADW_SWITCH_ROW (row));
  adw_switch_row_set_active (ADW_SWITCH_ROW (row), active);

  return g_strdup_printf ("OK %s=%s\n", title, active ? "on" : "off");
}


static char *
cmd_size (DevIpc     *self,
          const char *arg)
{
  int width = 0, height = 0;

  if (arg == NULL || sscanf (arg, "%dx%d", &width, &height) != 2 ||
      width <= 0 || height <= 0)
    return g_strdup ("ERR size needs WxH\n");

  gtk_window_set_default_size (self->window, width, height);

  return g_strdup_printf ("OK size=%dx%d\n", width, height);
}


/*
 * Dump the visible text of the current tab. Lets a harness assert on what the
 * UI actually says without a human having to read a screenshot — which matters
 * for the claims this app makes, where the difference between "measured" and
 * "estimated" is the whole point.
 */
static void
collect_text (GtkWidget *widget,
              GString   *out)
{
  if (widget == NULL || !gtk_widget_get_visible (widget))
    return;

  if (GTK_IS_LABEL (widget)) {
    const char *text = gtk_label_get_text (GTK_LABEL (widget));

    if (text != NULL && *text != '\0')
      g_string_append_printf (out, "%s\\n", text);
  }

  for (GtkWidget *child = gtk_widget_get_first_child (widget);
       child != NULL;
       child = gtk_widget_get_next_sibling (child)) {
    collect_text (child, out);
  }
}


static char *
cmd_text (DevIpc *self)
{
  AdwViewStack *stack = get_stack (self);
  GString *out = g_string_new ("OK ");

  if (stack == NULL)
    return g_strdup ("ERR no view stack\n");

  collect_text (adw_view_stack_get_visible_child (stack), out);
  g_string_append_c (out, '\n');

  return g_string_free (out, FALSE);
}


static gboolean
close_window_idle_quit (G_GNUC_UNUSED gpointer user_data)
{
  /* Quit the application rather than destroy the window: destroying it runs
   * the shutdown handler inside a widget teardown and segfaults in GObject. */
  g_application_quit (g_application_get_default ());
  return G_SOURCE_REMOVE;
}


gboolean
gsm_dev_sandbox_enabled (void)
{
  return g_strcmp0 (g_getenv ("GSM_DEV_SANDBOX"), "1") == 0;
}


const char *
gsm_dev_sandbox_reason (void)
{
  return "disabled in a sandboxed developer session";
}


static gboolean
close_window_idle (gpointer user_data)
{
  gtk_window_close (GTK_WINDOW (user_data));
  return G_SOURCE_REMOVE;
}


/* action NAME [PARAM]: activate an app. or win. action the way a menu item
 * does. PARAM may also follow NAME after "::" (win.show-page::processes). */
static char *
cmd_action (DevIpc     *self,
            const char *arg)
{
  g_autofree char *name = NULL;
  g_autoptr (GVariant) parameter = NULL;
  const char *param_text = NULL;
  char *sep;

  if (arg == NULL || *arg == '\0')
    return g_strdup ("ERR action needs a name\n");

  name = g_strdup (arg);
  sep = strstr (name, "::");
  if (sep == NULL)
    sep = strchr (name, ' ');
  if (sep != NULL) {
    param_text = sep + (sep[0] == ':' ? 2 : 1);
    *sep = '\0';
  }

  if (param_text != NULL && *param_text != '\0') {
    g_autoptr (GError) error = NULL;

    parameter = g_variant_parse (NULL, param_text, NULL, NULL, &error);
    if (parameter == NULL)
      parameter = g_variant_new_string (param_text);
    g_variant_ref_sink (parameter);
  }

  if (!gtk_widget_activate_action_variant (GTK_WIDGET (self->window), name,
                                           parameter))
    return g_strdup_printf ("ERR no such action: %s\n", name);

  return g_strdup_printf ("OK action %s\n", name);
}


/* set KEY VALUE: write a key of the app's own schema. */
static char *
cmd_set (G_GNUC_UNUSED DevIpc *self,
         const char           *arg)
{
  g_autoptr (GSettingsSchema) schema = NULL;
  g_autoptr (GSettingsSchemaKey) key = NULL;
  g_autoptr (GSettings) settings = NULL;
  g_autoptr (GVariant) value = NULL;
  g_autofree char *name = NULL;
  const char *text;
  char *space;

  if (arg == NULL || (space = strchr (arg, ' ')) == NULL)
    return g_strdup ("ERR set needs KEY VALUE\n");

  name = g_strndup (arg, space - arg);
  text = g_strstrip (space + 1);

  schema = g_settings_schema_source_lookup (g_settings_schema_source_get_default (),
                                            GSM_GSETTINGS_SCHEMA, TRUE);
  if (schema == NULL || !g_settings_schema_has_key (schema, name))
    return g_strdup_printf ("ERR no such key: %s\n", name);

  key = g_settings_schema_get_key (schema, name);
  value = g_variant_parse (g_settings_schema_key_get_value_type (key), text,
                           NULL, NULL, NULL);
  if (value == NULL && g_variant_type_equal (g_settings_schema_key_get_value_type (key),
                                             G_VARIANT_TYPE_STRING))
    value = g_variant_new_string (text);
  if (value == NULL || !g_settings_schema_key_range_check (key, value))
    return g_strdup_printf ("ERR bad value for %s: %s\n", name, text);

  settings = g_settings_new (GSM_GSETTINGS_SCHEMA);
  g_variant_ref_sink (value);
  if (!g_settings_set_value (settings, name, value))
    return g_strdup_printf ("ERR could not write %s\n", name);

  return g_strdup_printf ("OK %s=%s\n", name, text);
}


typedef struct {
  const char *needle;
  GtkTreeView *view;
  gboolean found;
} SelectSearch;


static gboolean
select_row_matching (GtkTreeModel *model,
                     GtkTreePath  *path,
                     GtkTreeIter  *iter,
                     gpointer      user_data)
{
  SelectSearch *search = user_data;
  int n_columns = gtk_tree_model_get_n_columns (model);

  for (int column = 0; column < n_columns; column++) {
    g_auto (GValue) value = G_VALUE_INIT;
    const char *text;

    if (gtk_tree_model_get_column_type (model, column) != G_TYPE_STRING)
      continue;

    gtk_tree_model_get_value (model, iter, column, &value);
    text = g_value_get_string (&value);
    if (text == NULL || strstr (text, search->needle) == NULL)
      continue;

    gtk_tree_view_expand_to_path (search->view, path);
    gtk_tree_selection_select_iter (gtk_tree_view_get_selection (search->view),
                                    iter);
    gtk_tree_view_scroll_to_cell (search->view, path, NULL, FALSE, 0, 0);
    search->found = TRUE;
    return TRUE;
  }

  return FALSE;
}


/* select TEXT: select the first process row with a text column containing
 * TEXT. Selecting is all it does; acting on the row is a separate verb. */
static char *
cmd_select (DevIpc     *self,
            const char *text)
{
  GtkWidget *tree;
  SelectSearch search;

  if (text == NULL || *text == '\0')
    return g_strdup ("ERR select needs text\n");

  tree = find_descendant (GTK_WIDGET (self->window), GTK_TYPE_TREE_VIEW);
  if (tree == NULL)
    return g_strdup ("ERR no process list\n");

  search.needle = text;
  search.view = GTK_TREE_VIEW (tree);
  search.found = FALSE;
  gtk_tree_model_foreach (gtk_tree_view_get_model (search.view),
                          select_row_matching, &search);

  if (!search.found)
    return g_strdup_printf ("ERR no row matching: %s\n", text);

  return g_strdup_printf ("OK selected %s\n", text);
}


static char *
handle_command (DevIpc *self,
                char   *line)
{
  char *arg;

  g_strstrip (line);

  arg = strchr (line, ' ');
  if (arg != NULL) {
    *arg = '\0';
    arg++;
    g_strstrip (arg);
  }

  if (g_strcmp0 (line, "ping") == 0)
    return g_strdup ("OK pong\n");
  if (g_strcmp0 (line, "tab") == 0)
    return cmd_tab (self, arg);
  if (g_strcmp0 (line, "shot") == 0)
    return cmd_shot (self, arg);
  if (g_strcmp0 (line, "scroll") == 0)
    return cmd_scroll (self, arg);
  if (g_strcmp0 (line, "click") == 0)
    return cmd_click (self, arg);
  if (g_strcmp0 (line, "toggle") == 0)
    return cmd_toggle (self, arg);
  if (g_strcmp0 (line, "size") == 0)
    return cmd_size (self, arg);
  if (g_strcmp0 (line, "text") == 0)
    return cmd_text (self);
  if (g_strcmp0 (line, "action") == 0)
    return cmd_action (self, arg);
  if (g_strcmp0 (line, "set") == 0)
    return cmd_set (self, arg);
  if (g_strcmp0 (line, "select") == 0)
    return cmd_select (self, arg);
  if (g_strcmp0 (line, "close") == 0) {
    g_idle_add (close_window_idle, self->window);
    return g_strdup ("OK closing\n");
  }
  if (g_strcmp0 (line, "quit") == 0) {
    /* Closing destroys the window and the objects this handler's caller
     * still uses to send the reply, so close from an idle callback. */
    g_idle_add (close_window_idle_quit, NULL);
    return g_strdup ("OK closing\n");
  }

  return g_strdup_printf ("ERR unknown command: %s\n", line);
}


/* ------------------------------------------------------------------ */
/* Socket                                                              */
/* ------------------------------------------------------------------ */

static gboolean
on_incoming (G_GNUC_UNUSED GSocketService    *service,
             GSocketConnection              *connection,
             G_GNUC_UNUSED GObject          *source,
             gpointer                        user_data)
{
  DevIpc *self = user_data;
  g_autoptr (GDataInputStream) input = NULL;
  GOutputStream *output;
  g_autofree char *line = NULL;
  g_autofree char *reply = NULL;
  g_autoptr (GError) error = NULL;

  input = g_data_input_stream_new (
    g_io_stream_get_input_stream (G_IO_STREAM (connection)));
  output = g_io_stream_get_output_stream (G_IO_STREAM (connection));

  line = g_data_input_stream_read_line (input, NULL, NULL, &error);
  if (line == NULL)
    return TRUE;

  /* Runs on the main loop, so touching widgets here is safe. */
  reply = handle_command (self, line);

  g_output_stream_write_all (output, reply, strlen (reply), NULL, NULL, &error);
  g_output_stream_close (output, NULL, NULL);

  return TRUE;
}


void
gsm_dev_ipc_init (GtkWindow *window)
{
  g_autoptr (GSocketAddress) address = NULL;
  g_autoptr (GError) error = NULL;
  DevIpc *self;
  const char *runtime_dir;
  const char *socket_override;

  g_return_if_fail (GTK_IS_WINDOW (window));

  /* Second lock: built in, but still dormant unless explicitly asked for. */
  if (g_strcmp0 (g_getenv ("GSM_DEV_IPC"), "1") != 0)
    return;

  if (the_ipc != NULL)
    return;

  /* Several instances (one per headless session) need distinct sockets. */
  socket_override = g_getenv ("GSM_DEV_IPC_SOCKET");
  runtime_dir = g_get_user_runtime_dir ();
  if ((socket_override == NULL || *socket_override == '\0') &&
      runtime_dir == NULL) {
    g_warning ("dev-ipc: no XDG_RUNTIME_DIR, refusing to create a socket "
               "anywhere else");
    return;
  }

  self = g_new0 (DevIpc, 1);
  self->window = window;
  if (socket_override != NULL && *socket_override != '\0')
    self->socket_path = g_strdup (socket_override);
  else
    self->socket_path = g_build_filename (runtime_dir,
                                          "gnome-system-monitor-rh-dev.sock",
                                          NULL);

  /* A stale socket from a crashed run would otherwise block binding forever. */
  g_unlink (self->socket_path);

  address = g_unix_socket_address_new (self->socket_path);
  self->service = g_socket_service_new ();

  if (!g_socket_listener_add_address (G_SOCKET_LISTENER (self->service),
                                      address,
                                      G_SOCKET_TYPE_STREAM,
                                      G_SOCKET_PROTOCOL_DEFAULT,
                                      NULL, NULL, &error)) {
    g_warning ("dev-ipc: cannot listen on %s: %s",
               self->socket_path, error->message);
    g_clear_object (&self->service);
    g_free (self->socket_path);
    g_free (self);
    return;
  }

  /*
   * XDG_RUNTIME_DIR is already 0700 and per-user, but this socket can drive
   * the UI, so narrow it to the owner explicitly rather than inheriting
   * whatever umask happened to be in force.
   */
  if (g_chmod (self->socket_path, 0600) != 0)
    g_warning ("dev-ipc: could not restrict socket permissions: %s",
               g_strerror (errno));

  g_signal_connect (self->service, "incoming", G_CALLBACK (on_incoming), self);
  g_socket_service_start (self->service);

  the_ipc = self;

  g_message ("dev-ipc: listening on %s — DEVELOPER BUILD ONLY",
             self->socket_path);
}

#endif /* HAVE_DEV_IPC */
