/*
 * Per-device write attribution via cgroup v2 io.stat.
 *
 * cgroup v2 gives us, for free and without any privilege, exactly what we
 * need and what /proc/pid/io cannot provide: bytes written broken down by
 * *both* cgroup and block device. That is what lets us say "Chromium wrote
 * this much to *this specific SSD*" rather than just "Chromium writes a lot".
 *
 * What io.stat means (Documentation/admin-guide/cgroup-v2.rst, "IO";
 * block/blk-cgroup.c, blkcg_iostat_update and __blkg_release): a node's
 * counters are a running sum of every delta that ever passed through its
 * children plus its own tasks' I/O, and nothing is subtracted when a child
 * is removed (its last bytes are flushed into the parent on release). So,
 * for a node and one device over one interval,
 *
 *   delta(node) = own writes + final writes of children that exited
 *               + deltas of children that lived through the interval
 *               + the whole counter of children born in it,
 *
 * and everything but the last two terms is the node's *residue*. Giving each
 * child its own delta and the node its residue accounts for every byte once:
 * per device, the rows of an interval sum to the root cgroup's delta, which
 * is the whole device. A cgroup is identified by (path, directory inode), so
 * a unit recreated under the same name is a new cgroup and starts from zero.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gsm-blame.h"

#define CGROUP_ROOT "/sys/fs/cgroup"

/* Guard against a pathological cgroup tree eating the sampler's wakeup. */
#define MAX_DEPTH   12

/* io.stat is a line per device; a megabyte is far past any real machine. */
#define MAX_IO_STAT_BYTES (1024 * 1024)

#define ROOT_NAME     "kernel and unattributed"
#define RESIDUE_SUFFIX " (own and exited units)"


void
gsm_blame_entry_free (GsmBlameEntry *entry)
{
  if (entry == NULL)
    return;

  g_free (entry->cgroup);
  g_free (entry->name);
  g_free (entry);
}


void
gsm_blame_delta_free (GsmBlameDelta *delta)
{
  if (delta == NULL)
    return;

  g_free (delta->cgroup);
  g_free (delta->name);
  g_free (delta);
}


/* ------------------------------------------------------------------ */
/* Names                                                               */
/* ------------------------------------------------------------------ */

/* systemd escapes a character that is not allowed in a unit name as \xNN
 * (systemd.unit(5), "STRING ESCAPING FOR INCLUSION IN UNIT NAMES"). */
static char *
decode_unit_escapes (const char *name)
{
  GString *out = g_string_sized_new (strlen (name));

  for (const char *p = name; *p; p++)
    {
      if (p[0] == '\\' && p[1] == 'x' &&
          g_ascii_isxdigit (p[2]) && g_ascii_isxdigit (p[3]))
        {
          g_string_append_c (out, (char) (g_ascii_xdigit_value (p[2]) * 16 +
                                          g_ascii_xdigit_value (p[3])));
          p += 3;
          continue;
        }
      g_string_append_c (out, *p);
    }

  return g_string_free (out, FALSE);
}


/*
 * "systemd-coredump@5213-4047792-0.service" is one handler per crash; left
 * alone, every crash would be a row of its own. An instance that is only
 * digits and dashes, with at least one dash, is such a counter and collapses
 * to "systemd-coredump@.service". "user@1000.service" (no dash) and
 * "getty@tty1.service" (letters) are real names and stay.
 */
static char *
collapse_template_instance (const char *service_name)
{
  const char *at = strchr (service_name, '@');
  const char *suffix = strrchr (service_name, '.');
  gboolean has_dash = FALSE;

  if (at == NULL || suffix == NULL || suffix <= at + 1)
    return NULL;

  for (const char *p = at + 1; p < suffix; p++)
    {
      if (*p == '-')
        has_dash = TRUE;
      else if (!g_ascii_isdigit (*p))
        return NULL;
    }

  if (!has_dash)
    return NULL;

  return g_strdup_printf ("%.*s@%s", (int) (at - service_name), service_name, suffix);
}


/*
 * systemd mints a fresh cgroup for every launch, tagged with the PID:
 *
 *   app-org.chromium.Chromium-4126.scope    -> org.chromium.Chromium
 *   app-codium-875820.scope                 -> codium
 *   session-2.scope                         -> session
 *   docker-9f2c...a1.scope                  -> docker
 *   nginx.service                           -> nginx.service
 *   systemd-coredump@5213-4047792-0.service -> systemd-coredump@.service
 *
 * Without collapsing these, every restart would look like a brand new app and
 * the thirty-day totals would be meaningless confetti.
 */
char *
gsm_blame_normalize_name (const char *basename)
{
  const char *start = basename;
  const char *dot;
  g_autofree char *stem = NULL;
  char *dash;
  gboolean is_scope;

  g_return_val_if_fail (basename != NULL, NULL);

  dot = strrchr (basename, '.');
  is_scope = dot != NULL && g_strcmp0 (dot, ".scope") == 0;

  /* Services keep their suffix — "nginx.service" is how the user refers to
   * it, and systemd never appends an instance number to one. Only a template
   * instance that is a bare counter is folded away. */
  if (!is_scope)
    {
      char *collapsed = NULL;

      if (dot != NULL && g_strcmp0 (dot, ".service") == 0)
        collapsed = collapse_template_instance (basename);

      return collapsed ? collapsed : g_strdup (basename);
    }

  if (g_str_has_prefix (start, "app-"))
    start += 4;

  stem = dot ? g_strndup (start, (gsize) (dot - start)) : g_strdup (start);

  /*
   * Strip a trailing instance tag: "-4126" (a PID) or "-9f2c3a..." (a
   * container hash). Only when what follows the last dash is *entirely*
   * alphanumeric-with-digits and long enough to not be part of a real name —
   * "gnome-shell" must survive, "codium-875820" must not.
   */
  dash = strrchr (stem, '-');
  if (dash != NULL && dash != stem)
    {
      const char *tag = dash + 1;
      gboolean all_digits = *tag != '\0';

      for (const char *p = tag; *p; p++)
        if (!g_ascii_isdigit (*p))
          {
            all_digits = FALSE;
            break;
          }

      /* A pure number is always an instance tag. A long hex blob is a container
       * id. Anything else is part of the application's actual name. */
      if (all_digits)
        {
          *dash = '\0';
        }
      else if (strlen (tag) >= 12)
        {
          gboolean all_hex = TRUE;

          for (const char *p = tag; *p; p++)
            if (!g_ascii_isxdigit (*p))
              {
                all_hex = FALSE;
                break;
              }
          if (all_hex)
            *dash = '\0';
        }
    }

  return g_steal_pointer (&stem);
}


static gboolean
is_unit_name (const char *name)
{
  return g_str_has_suffix (name, ".service") || g_str_has_suffix (name, ".scope");
}


static gboolean
path_is_slice (const char *relative)
{
  const char *last = strrchr (relative, '/');

  return g_str_has_suffix (last ? last + 1 : relative, ".slice");
}


char *
gsm_blame_node_name (const char *relative_path)
{
  g_auto (GStrv) parts = NULL;
  g_autofree char *own = NULL;
  guint count;

  g_return_val_if_fail (relative_path != NULL, NULL);

  if (relative_path[0] == '\0')
    return g_strdup (ROOT_NAME);

  parts = g_strsplit (relative_path, "/", -1);
  count = g_strv_length (parts);
  own = decode_unit_escapes (parts[count - 1]);

  if (g_str_has_suffix (own, ".slice"))
    return g_steal_pointer (&own);

  if (is_unit_name (own))
    return gsm_blame_normalize_name (own);

  /* A plain sub-cgroup (a browser's per-tab directory) is part of the unit
   * it lives in, or failing that of the slice. */
  for (guint i = count - 1; i-- > 0;)
    {
      g_autofree char *ancestor = decode_unit_escapes (parts[i]);

      if (is_unit_name (ancestor))
        return gsm_blame_normalize_name (ancestor);
      if (g_str_has_suffix (ancestor, ".slice"))
        return g_steal_pointer (&ancestor);
    }

  return g_steal_pointer (&own);
}


/* ------------------------------------------------------------------ */
/* Reading the tree                                                    */
/* ------------------------------------------------------------------ */

typedef struct
{
  guint major;
  guint minor;
  uint64_t rbytes;
  uint64_t wbytes;
  uint64_t delta;       /* wbytes since the previous read, filled by update */
} NodeDev;

typedef struct _Node
{
  char       *relative;
  uint64_t inode;
  GArray     *devs;       /* NodeDev */
  GPtrArray  *children;   /* Node* */
} Node;


static void
node_free (Node *node)
{
  if (node == NULL)
    return;

  g_free (node->relative);
  g_clear_pointer (&node->devs, g_array_unref);
  g_clear_pointer (&node->children, g_ptr_array_unref);
  g_free (node);
}


/* Decimal digits only, saturating; NULL when there is no digit at all. */
static const char *
parse_u64 (const char *text,
           uint64_t   *out)
{
  uint64_t value = 0;
  const char *p = text;

  if (!g_ascii_isdigit (*p))
    return NULL;

  for (; g_ascii_isdigit (*p); p++)
    {
      uint64_t digit = (uint64_t) (*p - '0');

      value = value > (UINT64_MAX - digit) / 10 ? UINT64_MAX : value * 10 + digit;
    }

  *out = value;
  return p;
}


/*
 * io.stat lines look like:
 *   259:0 rbytes=652146522624 wbytes=527674819584 rios=... wios=... dbytes=...
 */
static void
parse_io_stat (const char *contents,
               GArray     *devs)
{
  const char *p = contents;

  while (*p)
    {
      uint64_t major, minor;
      NodeDev dev = {
        0
      };
      const char *next;
      gboolean duplicate = FALSE;

      next = parse_u64 (p, &major);
      if (next != NULL && *next == ':')
        next = parse_u64 (next + 1, &minor);
      else
        next = NULL;

      if (next == NULL || major > G_MAXUINT || minor > G_MAXUINT)
        {
          p = strchr (p, '\n');
          if (p == NULL)
            break;
          p++;
          continue;
        }

      dev.major = (guint) major;
      dev.minor = (guint) minor;

      for (p = next; *p && *p != '\n'; p++)
        {
          if (g_str_has_prefix (p, " rbytes="))
            parse_u64 (p + 8, &dev.rbytes);
          else if (g_str_has_prefix (p, " wbytes="))
            parse_u64 (p + 8, &dev.wbytes);
        }

      for (guint i = 0; i < devs->len; i++)
        {
          NodeDev *seen = &g_array_index (devs, NodeDev, i);

          if (seen->major == dev.major && seen->minor == dev.minor)
            duplicate = TRUE;
        }
      if (!duplicate)
        g_array_append_val (devs, dev);

      if (*p == '\n')
        p++;
    }
}


/*
 * Only these mean the cgroup (or its io controller) is gone: that is a normal
 * outcome, and its bytes are accounted in an ancestor. Every other failure
 * (EACCES, EIO, ENOMEM, ...) says nothing about the cgroup, so the whole
 * update is abandoned rather than treat a live cgroup as exited.
 */
static gboolean
errno_means_gone (int error)
{
  return error == ENOENT || error == ENODEV || error == ENOTDIR || error == ESRCH;
}


/* Whole file into buf; FALSE with errno set when it could not be read. */
static gboolean
read_fd (int      fd,
         GString *buf)
{
  g_string_truncate (buf, 0);

  for (;;)
    {
      ssize_t got;

      g_string_set_size (buf, buf->len + 4096);
      buf->len -= 4096;
      got = read (fd, buf->str + buf->len, 4096);
      if (got < 0)
        {
          if (errno == EINTR)
            continue;
          return FALSE;   /* ENODEV: the cgroup was removed mid-read */
        }
      if (got == 0)
        break;

      buf->len += (gsize) got;
      if (buf->len > MAX_IO_STAT_BYTES)
        {
          errno = EFBIG;
          return FALSE;
        }
    }

  buf->str[buf->len] = '\0';
  return TRUE;
}


typedef struct
{
  char    *name;
  uint64_t inode;
} SubDir;

static void
subdir_clear (SubDir *sub)
{
  g_free (sub->name);
}


/*
 * Read one cgroup and, first, its children: a child read before its parent
 * can only be behind it, so a parent's counter is never short of what its
 * children report. A cgroup without io.stat has no io-carrying descendants
 * (a controller is enabled in a child only through its parent), so its
 * subtree is not entered. One that vanishes while being read takes its
 * subtree with it: those bytes then fall into its ancestor's residue instead
 * of being counted in two places. Takes ownership of dir_fd. Any other
 * failure sets *failed and ends the walk.
 */
static Node *
walk (int         dir_fd,
      const char *relative,
      uint64_t    inode,
      int         depth,
      GString    *buf,
      gboolean   *failed)
{
  g_autoptr (GArray) subdirs = g_array_new (FALSE, FALSE, sizeof (SubDir));
  Node *node;
  int io_fd;
  DIR *dir;

  g_array_set_clear_func (subdirs, (GDestroyNotify) subdir_clear);

  io_fd = openat (dir_fd, "io.stat", O_RDONLY | O_CLOEXEC);
  if (io_fd < 0)
    {
      *failed = !errno_means_gone (errno);
      close (dir_fd);
      return NULL;
    }

  node = g_new0 (Node, 1);
  node->relative = g_strdup (relative);
  node->inode = inode;
  node->devs = g_array_new (FALSE, FALSE, sizeof (NodeDev));
  node->children = g_ptr_array_new_with_free_func ((GDestroyNotify) node_free);

  dir = fdopendir (dir_fd);
  if (dir == NULL)
    {
      *failed = !errno_means_gone (errno);
      close (dir_fd);
      close (io_fd);
      node_free (node);
      return NULL;
    }

  if (depth < MAX_DEPTH)
    {
      struct dirent *entry;

      for (;;)
        {
          SubDir sub;
          gboolean is_dir;

          errno = 0;
          entry = readdir (dir);
          if (entry == NULL)
            break;

          is_dir = entry->d_type == DT_DIR;

          if (entry->d_name[0] == '.' &&
              (entry->d_name[1] == '\0' ||
               (entry->d_name[1] == '.' && entry->d_name[2] == '\0')))
            continue;

          if (entry->d_type == DT_UNKNOWN)
            {
              struct stat st;

              is_dir = fstatat (dirfd (dir), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
                       S_ISDIR (st.st_mode);
            }
          if (!is_dir)
            continue;

          sub.name = g_strdup (entry->d_name);
          sub.inode = (uint64_t) entry->d_ino;
          g_array_append_val (subdirs, sub);
        }

      if (errno != 0 && !errno_means_gone (errno))
        *failed = TRUE;

      for (guint i = 0; i < subdirs->len && !*failed; i++)
        {
          SubDir *sub = &g_array_index (subdirs, SubDir, i);
          g_autofree char *child_relative =
            relative[0] == '\0' ? g_strdup (sub->name)
                            : g_strdup_printf ("%s/%s", relative, sub->name);
          int child_fd = openat (dirfd (dir), sub->name,
                                 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
          Node *child;

          if (child_fd < 0)
            {
              /* ENOENT: gone since the listing. */
              if (!errno_means_gone (errno))
                *failed = TRUE;
              continue;
            }

          child = walk (child_fd, child_relative, sub->inode, depth + 1, buf, failed);
          if (child != NULL)
            g_ptr_array_add (node->children, child);
        }
    }

  closedir (dir);

  if (*failed || !read_fd (io_fd, buf))
    {
      if (!*failed)
        *failed = !errno_means_gone (errno);
      close (io_fd);
      node_free (node);
      return NULL;
    }
  close (io_fd);

  parse_io_stat (buf->str, node->devs);

  return node;
}


/* NULL with *failed unset: there is no tree (or it vanished). NULL with
 * *failed set: it could not be read this time, which says nothing. */
static Node *
read_tree (const char *root,
           gboolean   *failed)
{
  g_autoptr (GString) buf = g_string_sized_new (4096);
  int fd = open (root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

  *failed = FALSE;
  if (fd < 0)
    return NULL;

  return walk (fd, "", 0, 0, buf, failed);
}


/* ------------------------------------------------------------------ */
/* A snapshot, flat                                                    */
/* ------------------------------------------------------------------ */

static void
collect_node (const Node *node,
              GPtrArray  *out)
{
  for (guint i = 0; i < node->children->len; i++)
    collect_node (g_ptr_array_index (node->children, i), out);

  /* The root's io.stat is the machine total; it is no writer. */
  if (node->relative[0] == '\0')
    return;

  for (guint i = 0; i < node->devs->len; i++)
    {
      const NodeDev *dev = &g_array_index (node->devs, NodeDev, i);
      GsmBlameEntry *entry;

      /* A cgroup that has touched a device but written nothing to it is noise. */
      if (dev->wbytes == 0 && dev->rbytes == 0)
        continue;

      entry = g_new0 (GsmBlameEntry, 1);
      entry->cgroup = g_strdup (node->relative);
      entry->name = gsm_blame_node_name (node->relative);
      entry->major = dev->major;
      entry->minor = dev->minor;
      entry->rbytes = dev->rbytes;
      entry->wbytes = dev->wbytes;
      g_ptr_array_add (out, entry);
    }
}


GPtrArray *
gsm_blame_collect (void)
{
  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) gsm_blame_entry_free);
  gboolean failed;
  Node *root = read_tree (CGROUP_ROOT, &failed);

  if (root != NULL)
    collect_node (root, out);
  node_free (root);

  return out;
}


gboolean
gsm_blame_user_io_is_delegated (GPtrArray *cgroups)
{
  g_return_val_if_fail (cgroups != NULL, FALSE);

  /*
   * Anything *below* user.slice carrying io.stat means the controller was
   * delegated into the session. Without delegation the deepest cgroup with
   * io.stat is "user.slice" itself, with no trailing path.
   */
  for (guint i = 0; i < cgroups->len; i++)
    {
      GsmBlameEntry *entry = g_ptr_array_index (cgroups, i);

      if (g_str_has_prefix (entry->cgroup, "user.slice/"))
        return TRUE;
    }

  return FALSE;
}


/* ------------------------------------------------------------------ */
/* The tracker                                                         */
/* ------------------------------------------------------------------ */

/* What was seen of one device in one cgroup at the previous update. */
typedef struct
{
  guint major;
  guint minor;
  uint64_t wbytes;
  uint64_t carry;       /* shortfall to hold back from the next residue */
} DevState;

typedef struct
{
  uint64_t inode;
  GArray  *devs;        /* DevState */
} NodeState;

struct _GsmBlameTracker
{
  char       *root;
  GHashTable *state;        /* relative path -> NodeState */
  gboolean primed;
  gboolean user_io_delegated;
};


static void
node_state_free (NodeState *state)
{
  g_clear_pointer (&state->devs, g_array_unref);
  g_free (state);
}


GsmBlameTracker *
gsm_blame_tracker_new (const char *cgroup_root)
{
  GsmBlameTracker *tracker = g_new0 (GsmBlameTracker, 1);

  tracker->root = g_strdup (cgroup_root ? cgroup_root : CGROUP_ROOT);
  tracker->state = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                          (GDestroyNotify) node_state_free);

  return tracker;
}


void
gsm_blame_tracker_free (GsmBlameTracker *tracker)
{
  if (tracker == NULL)
    return;

  g_clear_pointer (&tracker->state, g_hash_table_unref);
  g_free (tracker->root);
  g_free (tracker);
}


gboolean
gsm_blame_tracker_user_io_is_delegated (GsmBlameTracker *tracker)
{
  g_return_val_if_fail (tracker != NULL, FALSE);

  return tracker->user_io_delegated;
}


static DevState *
find_dev_state (const NodeState *state,
                guint            major,
                guint            minor)
{
  if (state == NULL)
    return NULL;

  for (guint i = 0; i < state->devs->len; i++)
    {
      DevState *dev = &g_array_index (state->devs, DevState, i);

      if (dev->major == major && dev->minor == minor)
        return dev;
    }

  return NULL;
}


static uint64_t
saturating_add (uint64_t a,
                uint64_t b)
{
  return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}


static void
emit_row (GPtrArray     *rows,
          const Node    *node,
          const NodeDev *dev,
          uint64_t       wbytes,
          const char    *basis,
          const char    *name)
{
  GsmBlameDelta *row = g_new0 (GsmBlameDelta, 1);

  row->cgroup = g_strdup (node->relative);
  row->name = g_strdup (name);
  row->major = dev->major;
  row->minor = dev->minor;
  row->wbytes = wbytes;
  row->basis = basis;
  g_ptr_array_add (rows, row);
}


/*
 * Children first, so a node can subtract what they account for. Fills
 * `delta` on every device of every node, appends rows, and records the new
 * state of every node it visits.
 */
static void
split_node (GsmBlameTracker *tracker,
            GHashTable      *next_state,
            Node            *node,
            GPtrArray       *rows)
{
  const NodeState *old = g_hash_table_lookup (tracker->state, node->relative);
  NodeState *fresh = g_new0 (NodeState, 1);
  gboolean same_cgroup = old != NULL && old->inode == node->inode;
  gboolean is_root = node->relative[0] == '\0';
  g_autofree char *name = NULL;
  const char *basis = GSM_BLAME_BASIS_RESIDUE;
  gboolean leaf;

  for (guint i = 0; i < node->children->len; i++)
    split_node (tracker, next_state, g_ptr_array_index (node->children, i), rows);

  /* Only a cgroup that holds no others and is not a slice is "the writer";
   * a slice, or a unit with sub-cgroups, only has what is left over. */
  leaf = node->children->len == 0 && !is_root && !path_is_slice (node->relative);
  if (leaf)
    basis = GSM_BLAME_BASIS_KERNEL;

  fresh->inode = node->inode;
  fresh->devs = g_array_new (FALSE, FALSE, sizeof (DevState));

  for (guint d = 0; d < node->devs->len; d++)
    {
      NodeDev *dev = &g_array_index (node->devs, NodeDev, d);
      const DevState *before = same_cgroup ? find_dev_state (old, dev->major, dev->minor) : NULL;
      DevState after = {
        dev->major, dev->minor, dev->wbytes, 0
      };
      uint64_t children_sum = 0;
      uint64_t residue;

      /* Born in the interval (new path, new inode, or a device it had not
       * touched before): its counter started at zero, so all of it is new.
       * A counter that went backwards is a fresh instance too. */
      if (!tracker->primed)
        dev->delta = 0;
      else if (before == NULL || dev->wbytes < before->wbytes)
        dev->delta = dev->wbytes;
      else
        dev->delta = dev->wbytes - before->wbytes;

      for (guint c = 0; c < node->children->len; c++)
        {
          const Node *child = g_ptr_array_index (node->children, c);

          for (guint k = 0; k < child->devs->len; k++)
            {
              const NodeDev *theirs = &g_array_index (child->devs, NodeDev, k);

              if (theirs->major == dev->major && theirs->minor == dev->minor)
                children_sum = saturating_add (children_sum, theirs->delta);
            }
        }

      after.carry = before != NULL ? before->carry : 0;

      if (children_sum > dev->delta)
        {
          after.carry = saturating_add (after.carry, children_sum - dev->delta);
          residue = 0;
        }
      else
        {
          uint64_t repay;

          residue = dev->delta - children_sum;
          repay = MIN (after.carry, residue);
          residue -= repay;
          after.carry -= repay;
        }

      g_array_append_val (fresh->devs, after);

      if (residue == 0)
        continue;

      if (name == NULL)
        {
          g_autofree char *base = gsm_blame_node_name (node->relative);

          name = (leaf || is_root) ? g_steal_pointer (&base)
                               : g_strconcat (base, RESIDUE_SUFFIX, NULL);
        }
      emit_row (rows, node, dev, residue, basis, name);
    }

  g_hash_table_replace (next_state, g_strdup (node->relative), fresh);
}


static gboolean
tree_has_user_children (const Node *node)
{
  if (g_str_has_prefix (node->relative, "user.slice/"))
    return TRUE;

  for (guint i = 0; i < node->children->len; i++)
    if (tree_has_user_children (g_ptr_array_index (node->children, i)))
      return TRUE;

  return FALSE;
}


GPtrArray *
gsm_blame_tracker_update (GsmBlameTracker *tracker)
{
  GPtrArray *rows = g_ptr_array_new_with_free_func ((GDestroyNotify) gsm_blame_delta_free);
  GHashTable *next_state;
  gboolean failed;
  Node *root;

  g_return_val_if_fail (tracker != NULL, rows);

  /* No readable tree (no cgroup v2, the root vanished, or one read failed
   * for a reason that says nothing about the cgroups): say nothing and keep
   * what was known. The state is untouched, so the next good read covers
   * this interval and the next exactly, instead of crediting a cgroup that
   * was only unreadable as if it were new. */
  root = read_tree (tracker->root, &failed);
  if (root == NULL)
    return rows;

  next_state = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                      (GDestroyNotify) node_state_free);
  split_node (tracker, next_state, root, rows);
  tracker->user_io_delegated = tree_has_user_children (root);

  /* Whatever this read did not see is gone: its state is dropped here, which
   * is all the pruning the table needs. */
  g_hash_table_unref (tracker->state);
  tracker->state = next_state;
  tracker->primed = TRUE;

  node_free (root);

  return rows;
}


/* ------------------------------------------------------------------ */
/* Rows waiting for a drive                                            */
/* ------------------------------------------------------------------ */

struct _GsmBlameHold
{
  GHashTable *held;       /* "major:minor\nname\nbasis" -> GsmBlameDelta */
};


GsmBlameHold *
gsm_blame_hold_new (void)
{
  GsmBlameHold *hold = g_new0 (GsmBlameHold, 1);

  hold->held = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                      (GDestroyNotify) gsm_blame_delta_free);

  return hold;
}


void
gsm_blame_hold_free (GsmBlameHold *hold)
{
  if (hold == NULL)
    return;

  g_clear_pointer (&hold->held, g_hash_table_unref);
  g_free (hold);
}


guint
gsm_blame_hold_count (GsmBlameHold *hold)
{
  g_return_val_if_fail (hold != NULL, 0);

  return g_hash_table_size (hold->held);
}


static void
hold_add (GsmBlameHold        *hold,
          const GsmBlameDelta *delta)
{
  g_autofree char *key = g_strdup_printf ("%u:%u\n%s\n%s", delta->major,
                                          delta->minor, delta->name, delta->basis);
  GsmBlameDelta *held = g_hash_table_lookup (hold->held, key);

  if (held == NULL)
    {
      held = g_new0 (GsmBlameDelta, 1);
      held->cgroup = g_strdup (delta->cgroup);
      held->name = g_strdup (delta->name);
      held->major = delta->major;
      held->minor = delta->minor;
      held->basis = delta->basis;
      g_hash_table_insert (hold->held, g_steal_pointer (&key), held);
    }

  held->wbytes = saturating_add (held->wbytes, delta->wbytes);
}


void
gsm_blame_hold_resolve (GsmBlameHold       *hold,
                        GPtrArray          *deltas,
                        GsmBlameDriveLookup lookup,
                        GsmBlameSink        sink,
                        gpointer            user_data,
                        gboolean            discard_unmapped)
{
  GHashTableIter iter;
  gpointer value;

  g_return_if_fail (hold != NULL);

  /* What was waiting goes first: the map may know its device by now. */
  g_hash_table_iter_init (&iter, hold->held);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      const GsmBlameDelta *delta = value;
      const char *drive_id = lookup (delta->major, delta->minor, user_data);

      if (drive_id != NULL)
        {
          sink (drive_id, delta, user_data);
          g_hash_table_iter_remove (&iter);
        }
      else if (discard_unmapped)
        {
          g_hash_table_iter_remove (&iter);
        }
    }

  for (guint i = 0; deltas != NULL && i < deltas->len; i++)
    {
      const GsmBlameDelta *delta = g_ptr_array_index (deltas, i);
      const char *drive_id = lookup (delta->major, delta->minor, user_data);

      if (drive_id != NULL)
        sink (drive_id, delta, user_data);
      else if (!discard_unmapped)
        hold_add (hold, delta);
    }
}
