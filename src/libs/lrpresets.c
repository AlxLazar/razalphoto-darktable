/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
 * LR Presets panel
 * ─────────────────
 * Maintains a personal library of Lightroom / Camera Raw XMP preset files
 * stored in  ~/.config/darktable/lr_presets/.
 *
 * Presets can be organised into folders (sub-directories of lr_presets/).
 * A hierarchical tree view shows the folder/preset structure.
 *
 * Features
 *   • Import individual .xmp files into the library (copies them in)
 *   • Import an entire preset folder preserving its sub-folder structure
 *   • Organise presets into sub-folders ("new folder" button)
 *   • Move presets or folders by dragging them in the tree
 *   • Shift/Ctrl+click to select multiple presets; drag or remove them all at once
 *   • Browse the library with a live search filter
 *   • Apply a preset to the selected / current image(s)
 *   • Remove a preset or folder from the library
 *   • Reset image(s) to the original unprocessed state
 */

#include "common/act_on.h"
#include "common/darktable.h"
#include "common/database.h"
#include "common/debug.h"
#include "common/file_location.h"
#include "common/history.h"
#include "common/image.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/lightroom.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <string.h>

DT_MODULE(1)

typedef enum _col_t
{
  COL_NAME = 0,   /* display name (filename without extension, or folder name) */
  COL_PATH,       /* full filesystem path (directory or .xmp file)             */
  COL_IS_DIR,     /* TRUE if this node represents a directory                  */
  COL_VISIBLE,    /* used by GtkTreeModelFilter — set by search                */
  COL_NUM
} _col_t;

typedef struct dt_lib_lrpresets_t
{
  GtkTreeView        *tree;
  GtkTreeStore       *store;
  GtkTreeModelFilter *filter;
  GtkWidget          *search_entry;
  GtkWidget          *apply_btn;
  GtkWidget          *remove_btn;
  GtkWidget          *reset_btn;
  GtkWidget          *new_folder_btn;
  GtkWidget          *import_folder_btn;
  char                presets_dir[PATH_MAX];
  char               *drag_sources; /* newline-separated list of paths captured in drag-begin */
  /* hover preview state (darkroom only) */
  gboolean            preview_active;
  dt_imgid_t          preview_imgid;
  int                 preview_hist_count;   /* COUNT(*) from history before preview */
  int                 preview_history_end;  /* images.history_end before preview    */
  char               *preview_path;         /* currently previewed preset           */
  guint               preview_timeout_id;   /* debounce timer (0 = none)            */
  char               *preview_pending_path; /* preset waiting for debounce          */
} dt_lib_lrpresets_t;

/* drag-and-drop target definition (within-widget moves only) */
static const GtkTargetEntry DND_TARGETS[] = {
  { "LRPRESETS_PATH", GTK_TARGET_SAME_WIDGET, 0 }
};

/* ── Module metadata ──────────────────────────────────────────────────── */

const char *name(dt_lib_module_t *self)
{
  return _("LR Presets");
}

const char *description(dt_lib_module_t *self)
{
  return _("browse and apply Lightroom / Camera Raw XMP presets\n"
           "to selected images, or reset images to original");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_LIGHTTABLE | DT_VIEW_DARKROOM;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}

int position(const dt_lib_module_t *self)
{
  return 598; /* just above the styles panel */
}

/* ── Helpers ──────────────────────────────────────────────────────────── */

static void _ensure_presets_dir(dt_lib_lrpresets_t *d)
{
  char configdir[PATH_MAX];
  dt_loc_get_user_config_dir(configdir, sizeof(configdir));
  snprintf(d->presets_dir, sizeof(d->presets_dir), "%s/lr_presets", configdir);
  g_mkdir_with_parents(d->presets_dir, 0755);
}

/* Recursively populate the tree under parent (NULL = root level).
   Directories are inserted before files; both are sorted alphabetically. */
static void _populate_tree_dir(dt_lib_lrpresets_t *d, const char *dir_path, GtkTreeIter *parent)
{
  GDir *gdir = g_dir_open(dir_path, 0, NULL);
  if(!gdir) return;

  GSList *dirs_list  = NULL;
  GSList *files_list = NULL;
  const char *entry;

  while((entry = g_dir_read_name(gdir)) != NULL)
  {
    if(entry[0] == '.') continue; /* skip hidden / dotfiles */

    char full_path[PATH_MAX];
    snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry);

    if(g_file_test(full_path, G_FILE_TEST_IS_DIR))
      dirs_list = g_slist_insert_sorted(dirs_list, g_strdup(entry), (GCompareFunc)g_strcmp0);
    else
    {
      const char *ext = strrchr(entry, '.');
      if(ext && g_ascii_strcasecmp(ext, ".xmp") == 0)
        files_list = g_slist_insert_sorted(files_list, g_strdup(entry), (GCompareFunc)g_strcmp0);
    }
  }
  g_dir_close(gdir);

  /* Insert sub-directory nodes first */
  for(GSList *item = dirs_list; item; item = g_slist_next(item))
  {
    const char *dname = (const char *)item->data;
    char full_path[PATH_MAX];
    snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, dname);

    GtkTreeIter iter;
    gtk_tree_store_append(d->store, &iter, parent);
    gtk_tree_store_set(d->store, &iter,
                       COL_NAME,    dname,
                       COL_PATH,    full_path,
                       COL_IS_DIR,  TRUE,
                       COL_VISIBLE, TRUE,
                       -1);
    _populate_tree_dir(d, full_path, &iter); /* recurse */
  }

  /* Then insert preset files */
  for(GSList *item = files_list; item; item = g_slist_next(item))
  {
    const char *xmp_fname = (const char *)item->data;
    char full_path[PATH_MAX];
    snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, xmp_fname);

    char *display = g_strdup(xmp_fname);
    char *dot = strrchr(display, '.');
    if(dot) *dot = '\0';

    GtkTreeIter iter;
    gtk_tree_store_append(d->store, &iter, parent);
    gtk_tree_store_set(d->store, &iter,
                       COL_NAME,    display,
                       COL_PATH,    full_path,
                       COL_IS_DIR,  FALSE,
                       COL_VISIBLE, TRUE,
                       -1);
    g_free(display);
  }

  g_slist_free_full(dirs_list,  g_free);
  g_slist_free_full(files_list, g_free);
}

static void _populate_tree(dt_lib_lrpresets_t *d)
{
  gtk_tree_store_clear(d->store);
  _populate_tree_dir(d, d->presets_dir, NULL);
  gtk_tree_model_filter_refilter(d->filter);
  gtk_tree_view_expand_all(d->tree);
}

/* Recursively set COL_VISIBLE on the store based on search text.
   Returns TRUE if this node should be visible.
   Directories are visible if any descendant preset matches. */
static gboolean _set_visibility_recursive(dt_lib_lrpresets_t *d, GtkTreeIter *iter,
                                          const char *text)
{
  gboolean is_dir = FALSE;
  char    *name   = NULL;
  gtk_tree_model_get(GTK_TREE_MODEL(d->store), iter,
                     COL_IS_DIR, &is_dir,
                     COL_NAME,   &name,
                     -1);

  gboolean visible = FALSE;

  if(is_dir)
  {
    GtkTreeIter child;
    if(gtk_tree_model_iter_children(GTK_TREE_MODEL(d->store), &child, iter))
    {
      do {
        if(_set_visibility_recursive(d, &child, text))
          visible = TRUE;
      } while(gtk_tree_model_iter_next(GTK_TREE_MODEL(d->store), &child));
    }
  }
  else
  {
    if(!text || !*text)
    {
      visible = TRUE;
    }
    else
    {
      char *lower_name = g_ascii_strdown(name ? name : "", -1);
      char *lower_text = g_ascii_strdown(text, -1);
      visible = (strstr(lower_name, lower_text) != NULL);
      g_free(lower_name);
      g_free(lower_text);
    }
  }

  g_free(name);
  gtk_tree_store_set(d->store, iter, COL_VISIBLE, visible, -1);
  return visible;
}

/* Returns the target directory for the current tree selection (uses first selected row):
   - folder selected  → that folder path
   - preset selected  → parent directory
   - nothing selected → root presets_dir */
static void _get_selected_dir(dt_lib_lrpresets_t *d, char *out, gsize out_size)
{
  GtkTreeSelection *sel  = gtk_tree_view_get_selection(d->tree);
  GtkTreeModel     *model = gtk_tree_view_get_model(d->tree);
  GList *rows = gtk_tree_selection_get_selected_rows(sel, &model);

  if(rows)
  {
    GtkTreeIter filter_iter;
    gtk_tree_model_get_iter(model, &filter_iter, (GtkTreePath *)rows->data);

    GtkTreeIter store_iter;
    gtk_tree_model_filter_convert_iter_to_child_iter(d->filter, &store_iter, &filter_iter);

    gboolean is_dir = FALSE;
    char    *path   = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(d->store), &store_iter,
                       COL_IS_DIR, &is_dir,
                       COL_PATH,   &path,
                       -1);

    if(is_dir)
    {
      g_strlcpy(out, path, out_size);
    }
    else
    {
      char *parent = g_path_get_dirname(path);
      g_strlcpy(out, parent, out_size);
      g_free(parent);
    }
    g_free(path);
    g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
    return;
  }

  g_strlcpy(out, d->presets_dir, out_size);
}

/* Recursively copy all XMP files from src_dir into dest_dir,
   preserving sub-folder structure.  Returns number of files copied. */
static int _import_folder_recursive(const char *src_dir, const char *dest_dir)
{
  g_mkdir_with_parents(dest_dir, 0755);

  GDir *gdir = g_dir_open(src_dir, 0, NULL);
  if(!gdir) return 0;

  int count = 0;
  const char *entry;
  while((entry = g_dir_read_name(gdir)) != NULL)
  {
    if(entry[0] == '.') continue;

    char src_path[PATH_MAX], dest_path[PATH_MAX];
    snprintf(src_path,  sizeof(src_path),  "%s/%s", src_dir,  entry);
    snprintf(dest_path, sizeof(dest_path), "%s/%s", dest_dir, entry);

    if(g_file_test(src_path, G_FILE_TEST_IS_DIR))
    {
      count += _import_folder_recursive(src_path, dest_path);
    }
    else
    {
      const char *ext = strrchr(entry, '.');
      if(ext && g_ascii_strcasecmp(ext, ".xmp") == 0)
      {
        gsize  len      = 0;
        char  *contents = NULL;
        if(g_file_get_contents(src_path, &contents, &len, NULL))
        {
          if(g_file_set_contents(dest_path, contents, (gssize)len, NULL))
            count++;
          g_free(contents);
        }
      }
    }
  }
  g_dir_close(gdir);
  return count;
}

/* Count XMP files recursively under dir_path */
static int _count_files_recursive(const char *dir_path)
{
  int   count = 0;
  GDir *gdir  = g_dir_open(dir_path, 0, NULL);
  if(!gdir) return 0;

  const char *entry;
  while((entry = g_dir_read_name(gdir)) != NULL)
  {
    if(entry[0] == '.') continue;
    char full[PATH_MAX];
    snprintf(full, sizeof(full), "%s/%s", dir_path, entry);
    if(g_file_test(full, G_FILE_TEST_IS_DIR))
      count += _count_files_recursive(full);
    else
    {
      const char *ext = strrchr(entry, '.');
      if(ext && g_ascii_strcasecmp(ext, ".xmp") == 0) count++;
    }
  }
  g_dir_close(gdir);
  return count;
}

/* Recursively delete a directory and all its contents */
static void _delete_dir_recursive(const char *dir_path)
{
  GDir *gdir = g_dir_open(dir_path, 0, NULL);
  if(!gdir) return;

  const char *entry;
  while((entry = g_dir_read_name(gdir)) != NULL)
  {
    char full[PATH_MAX];
    snprintf(full, sizeof(full), "%s/%s", dir_path, entry);
    if(g_file_test(full, G_FILE_TEST_IS_DIR))
      _delete_dir_recursive(full);
    else
      g_remove(full);
  }
  g_dir_close(gdir);
  g_rmdir(dir_path);
}

/* ── Cell data function: icon based on node type ─────────────────────── */

static void _icon_cell_func(GtkTreeViewColumn *col, GtkCellRenderer *renderer,
                             GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
  gboolean is_dir = FALSE;
  gtk_tree_model_get(model, iter, COL_IS_DIR, &is_dir, -1);
  g_object_set(renderer, "icon-name", is_dir ? "folder" : "image-x-generic", NULL);
}

/* ── Hover preview (darkroom only) ────────────────────────────────────── */

#define PREVIEW_DEBOUNCE_MS 200

/* Cancel any pending debounce timer */
static void _preview_cancel_timer(dt_lib_lrpresets_t *d)
{
  if(d->preview_timeout_id)
  {
    g_source_remove(d->preview_timeout_id);
    d->preview_timeout_id = 0;
  }
  g_free(d->preview_pending_path);
  d->preview_pending_path = NULL;
}

/* Revert an active preview: delete the history entries we added and reload */
static void _preview_revert(dt_lib_lrpresets_t *d)
{
  if(!d->preview_active) return;
  if(!darktable.develop || !darktable.develop->gui_attached) { d->preview_active = FALSE; return; }

  dt_develop_t *dev         = darktable.develop;
  const dt_imgid_t imgid    = d->preview_imgid;

  /* delete the history rows that the preview inserted (num >= count_before) */
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
      "DELETE FROM main.history WHERE imgid = ?1 AND num >= ?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, d->preview_hist_count);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  /* restore original history_end */
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
      "UPDATE main.images SET history_end = ?1 WHERE id = ?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, d->preview_history_end);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  dt_dev_reload_history_items(dev);
  dt_control_queue_redraw_center();

  d->preview_active = FALSE;
  d->preview_imgid  = NO_IMGID;
  g_free(d->preview_path);
  d->preview_path = NULL;
}

/* Apply a preset as a temporary preview */
static void _preview_apply(dt_lib_module_t *self, const char *xmp_path)
{
  dt_lib_lrpresets_t *d = self->data;

  if(!darktable.develop || !darktable.develop->gui_attached) return;
  dt_develop_t *dev = darktable.develop;

  const dt_imgid_t imgid = dev->image_storage.id;
  if(!dt_is_valid_imgid(imgid)) return;

  /* revert any existing preview first */
  _preview_revert(d);

  /* snapshot history state before applying */
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
      "SELECT COUNT(*) FROM main.history WHERE imgid = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  d->preview_hist_count = 0;
  if(sqlite3_step(stmt) == SQLITE_ROW)
    d->preview_hist_count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  d->preview_history_end = dev->history_end;
  d->preview_imgid       = imgid;
  d->preview_path        = g_strdup(xmp_path);

  /* apply silently (no log, no signal, no xmp sync) */
  dt_lightroom_apply_preset(imgid, dev, xmp_path, TRUE);
  d->preview_active = TRUE;
}

/* Debounce timer callback */
static gboolean _preview_timeout_cb(gpointer data)
{
  dt_lib_module_t   *self = data;
  dt_lib_lrpresets_t *d   = self->data;

  d->preview_timeout_id = 0;

  if(d->preview_pending_path && *d->preview_pending_path)
    _preview_apply(self, d->preview_pending_path);

  g_free(d->preview_pending_path);
  d->preview_pending_path = NULL;
  return G_SOURCE_REMOVE;
}

/* motion-notify on the tree: schedule preview when cursor enters a preset row */
static gboolean _tree_motion(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_lib_module_t   *self = user_data;
  dt_lib_lrpresets_t *d   = self->data;

  /* preview only works in darkroom */
  if(!darktable.develop || !darktable.develop->gui_attached) return FALSE;

  GtkTreePath *tree_path = NULL;
  if(!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget),
       (gint)event->x, (gint)event->y, &tree_path, NULL, NULL, NULL))
  {
    /* cursor is on empty space — revert */
    _preview_cancel_timer(d);
    _preview_revert(d);
    return FALSE;
  }

  /* resolve the row */
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(widget));
  GtkTreeIter   filter_iter;
  if(!gtk_tree_model_get_iter(model, &filter_iter, tree_path))
  {
    gtk_tree_path_free(tree_path);
    _preview_cancel_timer(d);
    _preview_revert(d);
    return FALSE;
  }
  gtk_tree_path_free(tree_path);

  GtkTreeIter store_iter;
  gtk_tree_model_filter_convert_iter_to_child_iter(d->filter, &store_iter, &filter_iter);

  gboolean is_dir = FALSE;
  char    *path   = NULL;
  gtk_tree_model_get(GTK_TREE_MODEL(d->store), &store_iter,
                     COL_IS_DIR, &is_dir,
                     COL_PATH,   &path,
                     -1);

  if(is_dir || !path)
  {
    g_free(path);
    _preview_cancel_timer(d);
    _preview_revert(d);
    return FALSE;
  }

  /* already previewing this exact preset — nothing to do */
  if(d->preview_active && d->preview_path && g_strcmp0(d->preview_path, path) == 0)
  {
    g_free(path);
    return FALSE;
  }
  /* already pending for this preset */
  if(d->preview_pending_path && g_strcmp0(d->preview_pending_path, path) == 0)
  {
    g_free(path);
    return FALSE;
  }

  /* new preset — (re)start debounce timer */
  _preview_cancel_timer(d);
  d->preview_pending_path = path; /* takes ownership */
  d->preview_timeout_id   = g_timeout_add(PREVIEW_DEBOUNCE_MS, _preview_timeout_cb, self);

  return FALSE;
}

/* leave-notify: cursor left the tree widget — revert any preview */
static gboolean _tree_leave(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  /* ignore leave events caused by grabs (e.g. scrollbar, popup) */
  if(event->detail == GDK_NOTIFY_INFERIOR) return FALSE;

  dt_lib_module_t   *self = user_data;
  dt_lib_lrpresets_t *d   = self->data;
  _preview_cancel_timer(d);
  _preview_revert(d);
  return FALSE;
}

/* ── Drag-and-drop ────────────────────────────────────────────────────── */

/* Snapshot the current selection on every left button press.
   drag-begin fires AFTER GtkTreeView's class handler has already collapsed a
   multi-selection down to the single dragged row, so querying the selection
   there gives only 1 path.  By capturing here — before the class handler runs
   (connected handlers fire before the default/RUN_LAST handler) — we preserve
   the full multi-selection that the user built with Shift/Ctrl+click. */
static gboolean _tree_button_press(GtkWidget *widget, GdkEventButton *event,
                                    gpointer user_data)
{
  if(event->button != 1) return FALSE; /* only track left button */

  dt_lib_module_t   *self = user_data;
  dt_lib_lrpresets_t *d   = self->data;

  g_free(d->drag_sources);
  d->drag_sources = NULL;

  GtkTreeSelection *sel   = gtk_tree_view_get_selection(GTK_TREE_VIEW(widget));
  GtkTreeModel     *model = gtk_tree_view_get_model(GTK_TREE_VIEW(widget));
  GList *rows = gtk_tree_selection_get_selected_rows(sel, &model);
  if(!rows) return FALSE;

  GString *buf = g_string_new("");
  for(GList *item = rows; item; item = g_list_next(item))
  {
    GtkTreeIter filter_iter;
    if(!gtk_tree_model_get_iter(model, &filter_iter, (GtkTreePath *)item->data)) continue;

    GtkTreeIter store_iter;
    gtk_tree_model_filter_convert_iter_to_child_iter(d->filter, &store_iter, &filter_iter);

    char *path = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(d->store), &store_iter, COL_PATH, &path, -1);
    if(path)
    {
      if(buf->len) g_string_append_c(buf, '\n');
      g_string_append(buf, path);
      g_free(path);
    }
  }
  g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);

  d->drag_sources = g_string_free(buf, FALSE);
  return FALSE; /* let GTK handle selection and drag-detection normally */
}

/* drag-begin: drag_sources was already populated by _tree_button_press.
   Nothing to do here; keeping the callback only as a safety fallback. */
static void _dnd_begin(GtkWidget *widget, GdkDragContext *ctx, gpointer user_data)
{
  (void)widget; (void)ctx; (void)user_data;
}

/* Provide all selected paths as newline-delimited drag data */
static void _dnd_data_get(GtkWidget *widget, GdkDragContext *ctx,
                           GtkSelectionData *sel, guint info, guint time,
                           gpointer user_data)
{
  dt_lib_module_t   *self = user_data;
  dt_lib_lrpresets_t *d   = self->data;

  if(d->drag_sources && *d->drag_sources)
    gtk_selection_data_set(sel,
                           gtk_selection_data_get_target(sel),
                           8,
                           (const guchar *)d->drag_sources,
                           strlen(d->drag_sources) + 1);
}

/* Highlight the target row during drag-over */
static gboolean _dnd_motion(GtkWidget *widget, GdkDragContext *ctx,
                              gint x, gint y, guint time, gpointer user_data)
{
  GtkTreePath            *path;
  GtkTreeViewDropPosition pos;

  if(gtk_tree_view_get_dest_row_at_pos(GTK_TREE_VIEW(widget), x, y, &path, &pos))
  {
    gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(widget), path,
                                    GTK_TREE_VIEW_DROP_INTO_OR_BEFORE);
    gtk_tree_path_free(path);
  }
  else
  {
    gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(widget), NULL,
                                    GTK_TREE_VIEW_DROP_BEFORE);
  }
  gdk_drag_status(ctx, GDK_ACTION_MOVE, time);
  return TRUE;
}

/* Handle the actual drop: move all dragged files/folders to the target directory */
static void _dnd_data_received(GtkWidget *widget, GdkDragContext *ctx,
                                gint x, gint y, GtkSelectionData *sel,
                                guint info, guint time, gpointer user_data)
{
  dt_lib_module_t   *self = user_data;
  dt_lib_lrpresets_t *d   = self->data;

  const guchar *raw = gtk_selection_data_get_data(sel);
  if(!raw || !*raw) { gtk_drag_finish(ctx, FALSE, FALSE, time); return; }

  /* determine target directory from drop position */
  char target_dir[PATH_MAX];
  GtkTreePath            *drop_path = NULL;
  GtkTreeViewDropPosition drop_pos;

  if(gtk_tree_view_get_dest_row_at_pos(GTK_TREE_VIEW(widget), x, y, &drop_path, &drop_pos))
  {
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(widget));
    GtkTreeIter   filter_iter;
    gtk_tree_model_get_iter(model, &filter_iter, drop_path);
    gtk_tree_path_free(drop_path);

    GtkTreeIter store_iter;
    gtk_tree_model_filter_convert_iter_to_child_iter(d->filter, &store_iter, &filter_iter);

    gboolean is_dir = FALSE;
    char    *dest   = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(d->store), &store_iter,
                       COL_IS_DIR, &is_dir,
                       COL_PATH,   &dest,
                       -1);

    if(is_dir)
      g_strlcpy(target_dir, dest, sizeof(target_dir));
    else
    {
      char *parent = g_path_get_dirname(dest);
      g_strlcpy(target_dir, parent, sizeof(target_dir));
      g_free(parent);
    }
    g_free(dest);
  }
  else
  {
    g_strlcpy(target_dir, d->presets_dir, sizeof(target_dir));
  }

  /* iterate over newline-separated source paths */
  char *sources = g_strdup((const char *)raw);
  int   n_moved = 0;
  char *line     = strtok(sources, "\n");

  while(line)
  {
    const char *src_path = line;

    /* refuse to move a folder into itself or a descendant */
    if(g_file_test(src_path, G_FILE_TEST_IS_DIR))
    {
      char src_slash[PATH_MAX];
      snprintf(src_slash, sizeof(src_slash), "%s/", src_path);
      if(g_strcmp0(target_dir, src_path) == 0 ||
         g_str_has_prefix(target_dir, src_slash))
      {
        line = strtok(NULL, "\n");
        continue;
      }
    }

    char *bname = g_path_get_basename(src_path);
    char  dest_path[PATH_MAX];
    snprintf(dest_path, sizeof(dest_path), "%s/%s", target_dir, bname);
    g_free(bname);

    if(g_strcmp0(src_path, dest_path) != 0 && g_rename(src_path, dest_path) == 0)
      n_moved++;

    line = strtok(NULL, "\n");
  }
  g_free(sources);

  if(n_moved > 0)
  {
    _populate_tree(d);
    gtk_drag_finish(ctx, TRUE, FALSE, time);
  }
  else
  {
    gtk_drag_finish(ctx, FALSE, FALSE, time);
  }
}

/* ── Button callbacks ─────────────────────────────────────────────────── */

static void _import_clicked(GtkWidget *w, dt_lib_module_t *self)
{
  dt_lib_lrpresets_t *d = self->data;

  GtkWidget *win    = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *dialog = gtk_file_chooser_dialog_new(
      _("import Lightroom / Camera Raw presets"),
      GTK_WINDOW(win),
      GTK_FILE_CHOOSER_ACTION_OPEN,
      _("_cancel"), GTK_RESPONSE_CANCEL,
      _("_import"), GTK_RESPONSE_ACCEPT,
      NULL);
  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dialog), TRUE);

  GtkFileFilter *filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, _("XMP presets (*.xmp)"));
  gtk_file_filter_add_pattern(filter, "*.xmp");
  gtk_file_filter_add_pattern(filter, "*.XMP");
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

  char *last_dir = dt_conf_get_string("plugins/lighttable/lrpresets/import_dir");
  if(last_dir && *last_dir)
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), last_dir);
  g_free(last_dir);

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  gtk_widget_show_all(dialog);

  if(gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
  {
    GSList *files = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(dialog));
    if(files)
    {
      char *first_dir = g_path_get_dirname((const char *)files->data);
      dt_conf_set_string("plugins/lighttable/lrpresets/import_dir", first_dir);
      g_free(first_dir);

      /* copy into the currently selected folder (or root) */
      char target_dir[PATH_MAX];
      _get_selected_dir(d, target_dir, sizeof(target_dir));

      int n_imported = 0;
      for(const GSList *f = files; f; f = g_slist_next(f))
      {
        const char *src_path = (const char *)f->data;
        char       *bname    = g_path_get_basename(src_path);
        char        dest_path[PATH_MAX];
        snprintf(dest_path, sizeof(dest_path), "%s/%s", target_dir, bname);
        g_free(bname);

        gsize  len      = 0;
        char  *contents = NULL;
        if(g_file_get_contents(src_path, &contents, &len, NULL))
        {
          if(g_file_set_contents(dest_path, contents, (gssize)len, NULL))
            n_imported++;
          g_free(contents);
        }
      }
      g_slist_free_full(files, g_free);
      _populate_tree(d);

      if(n_imported > 0)
        dt_control_log(ngettext("%d preset imported", "%d presets imported", n_imported), n_imported);
    }
  }
  gtk_widget_destroy(dialog);
}

/* Import an entire folder tree, preserving sub-folder structure */
static void _import_folder_clicked(GtkWidget *w, dt_lib_module_t *self)
{
  dt_lib_lrpresets_t *d = self->data;

  GtkWidget *win    = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *dialog = gtk_file_chooser_dialog_new(
      _("select preset folder to import"),
      GTK_WINDOW(win),
      GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
      _("_cancel"), GTK_RESPONSE_CANCEL,
      _("_import"), GTK_RESPONSE_ACCEPT,
      NULL);

  char *last_dir = dt_conf_get_string("plugins/lighttable/lrpresets/import_dir");
  if(last_dir && *last_dir)
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), last_dir);
  g_free(last_dir);

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  gtk_widget_show_all(dialog);

  if(gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
  {
    char *src_folder = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    if(src_folder)
    {
      /* save parent of selected folder for next browse */
      char *parent_of_src = g_path_get_dirname(src_folder);
      dt_conf_set_string("plugins/lighttable/lrpresets/import_dir", parent_of_src);
      g_free(parent_of_src);

      /* target: selected folder in library (or root) + the imported folder's name */
      char target_parent[PATH_MAX];
      _get_selected_dir(d, target_parent, sizeof(target_parent));

      char *folder_name = g_path_get_basename(src_folder);
      char  dest_root[PATH_MAX];
      snprintf(dest_root, sizeof(dest_root), "%s/%s", target_parent, folder_name);
      g_free(folder_name);

      const int n = _import_folder_recursive(src_folder, dest_root);
      g_free(src_folder);

      _populate_tree(d);

      if(n > 0)
        dt_control_log(ngettext("%d preset imported", "%d presets imported", n), n);
      else
        dt_control_log(_("no XMP presets found in the selected folder"));
    }
  }
  gtk_widget_destroy(dialog);
}

static void _new_folder_clicked(GtkWidget *w, dt_lib_module_t *self)
{
  dt_lib_lrpresets_t *d = self->data;

  char parent_dir[PATH_MAX];
  _get_selected_dir(d, parent_dir, sizeof(parent_dir));

  GtkWidget *win    = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *dialog = gtk_dialog_new_with_buttons(
      _("new folder"),
      GTK_WINDOW(win),
      GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
      _("_cancel"), GTK_RESPONSE_CANCEL,
      _("_create"), GTK_RESPONSE_ACCEPT,
      NULL);
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);

  GtkWidget *entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(entry), _("folder name"));
  gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);

  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_container_add(GTK_CONTAINER(content), entry);
  gtk_widget_show_all(dialog);

  if(gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
  {
    const char *folder_name = gtk_entry_get_text(GTK_ENTRY(entry));
    if(folder_name && *folder_name)
    {
      char new_dir[PATH_MAX];
      snprintf(new_dir, sizeof(new_dir), "%s/%s", parent_dir, folder_name);
      if(g_mkdir_with_parents(new_dir, 0755) == 0)
      {
        _populate_tree(d);
        dt_control_log(_("folder '%s' created"), folder_name);
      }
      else
      {
        dt_control_log(_("could not create folder '%s'"), folder_name);
      }
    }
  }
  gtk_widget_destroy(dialog);
}

static void _apply_clicked(GtkWidget *w, dt_lib_module_t *self)
{
  dt_lib_lrpresets_t *d = self->data;

  /* stop any pending preview */
  _preview_cancel_timer(d);

  /* collect all selected preset paths (skip folders) */
  GtkTreeSelection *sel   = gtk_tree_view_get_selection(d->tree);
  GtkTreeModel     *model = gtk_tree_view_get_model(d->tree);
  GList *rows = gtk_tree_selection_get_selected_rows(sel, &model);
  if(!rows) { dt_control_log(_("no preset selected")); return; }

  GList *preset_paths = NULL;
  for(GList *item = rows; item; item = g_list_next(item))
  {
    GtkTreeIter filter_iter;
    gtk_tree_model_get_iter(model, &filter_iter, (GtkTreePath *)item->data);

    GtkTreeIter store_iter;
    gtk_tree_model_filter_convert_iter_to_child_iter(d->filter, &store_iter, &filter_iter);

    gboolean is_dir = FALSE;
    char    *path   = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(d->store), &store_iter,
                       COL_IS_DIR, &is_dir,
                       COL_PATH,   &path,
                       -1);

    if(!is_dir && path)
      preset_paths = g_list_append(preset_paths, path);
    else
      g_free(path);
  }
  g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);

  if(!preset_paths) { dt_control_log(_("select a preset, not a folder")); return; }

  /* if the preview is already showing the exact same single preset, just commit it */
  if(d->preview_active
     && g_list_length(preset_paths) == 1
     && d->preview_path
     && g_strcmp0(d->preview_path, (const char *)preset_paths->data) == 0)
  {
    /* commit: keep the changes, sync xmp, raise signal */
    const dt_imgid_t committed_imgid = d->preview_imgid;
    d->preview_active = FALSE;
    g_free(d->preview_path);
    d->preview_path = NULL;
    d->preview_imgid = NO_IMGID;

    dt_image_synch_xmp(committed_imgid);
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_DEVELOP_HISTORY_CHANGE);

    g_list_free_full(preset_paths, g_free);
    return;
  }

  /* different preset or multiple presets — revert preview first */
  _preview_revert(d);

  GList *imgs = dt_act_on_get_images(TRUE, TRUE, FALSE);
  if(!imgs)
  {
    g_list_free_full(preset_paths, g_free);
    dt_control_log(_("no images selected"));
    return;
  }

  dt_develop_t *dev = (darktable.develop && darktable.develop->gui_attached)
                      ? darktable.develop : NULL;

  for(const GList *preset = preset_paths; preset; preset = g_list_next(preset))
  {
    const char *preset_path = (const char *)preset->data;
    for(const GList *img = imgs; img; img = g_list_next(img))
    {
      const dt_imgid_t imgid = GPOINTER_TO_INT(img->data);
      dt_lightroom_apply_preset(imgid, dev, preset_path, FALSE);
    }
  }
  g_list_free_full(preset_paths, g_free);
  g_list_free(imgs);

  if(dev)
  {
    dt_dev_modulegroups_set(darktable.develop, dt_dev_modulegroups_get(darktable.develop));
    dt_control_queue_redraw_center();
  }
}

static void _remove_clicked(GtkWidget *w, dt_lib_module_t *self)
{
  dt_lib_lrpresets_t *d = self->data;

  GtkTreeSelection *sel   = gtk_tree_view_get_selection(d->tree);
  GtkTreeModel     *model = gtk_tree_view_get_model(d->tree);
  GList *rows = gtk_tree_selection_get_selected_rows(sel, &model);
  if(!rows) return;

  /* collect selected items */
  typedef struct { char path[PATH_MAX]; gboolean is_dir; } _item_t;
  GArray *items = g_array_new(FALSE, FALSE, sizeof(_item_t));

  for(GList *row = rows; row; row = g_list_next(row))
  {
    GtkTreeIter filter_iter;
    gtk_tree_model_get_iter(model, &filter_iter, (GtkTreePath *)row->data);

    GtkTreeIter store_iter;
    gtk_tree_model_filter_convert_iter_to_child_iter(d->filter, &store_iter, &filter_iter);

    _item_t it = { .is_dir = FALSE };
    char *path = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(d->store), &store_iter,
                       COL_IS_DIR, &it.is_dir,
                       COL_PATH,   &path,
                       -1);
    if(path)
    {
      g_strlcpy(it.path, path, PATH_MAX);
      g_free(path);
      g_array_append_val(items, it);
    }
  }
  g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);

  if(items->len == 0) { g_array_free(items, TRUE); return; }

  /* count presets that will be deleted (for the confirmation message) */
  int n_presets = 0;
  for(guint i = 0; i < items->len; i++)
  {
    _item_t *it = &g_array_index(items, _item_t, i);
    if(it->is_dir)
      n_presets += _count_files_recursive(it->path);
    else
      n_presets++;
  }

  /* confirmation dialog */
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *dlg = gtk_message_dialog_new(
      GTK_WINDOW(win),
      GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
      GTK_MESSAGE_QUESTION,
      GTK_BUTTONS_NONE,
      ngettext("Remove %d item from the library?",
               "Remove %d items from the library?", (int)items->len),
      (int)items->len);
  gtk_message_dialog_format_secondary_text(
      GTK_MESSAGE_DIALOG(dlg),
      ngettext("This will permanently delete %d preset file.",
               "This will permanently delete %d preset files.", n_presets),
      n_presets);
  gtk_dialog_add_buttons(GTK_DIALOG(dlg),
                         _("_cancel"), GTK_RESPONSE_CANCEL,
                         _("_remove"), GTK_RESPONSE_ACCEPT,
                         NULL);
  gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_CANCEL);
  const gint res = gtk_dialog_run(GTK_DIALOG(dlg));
  gtk_widget_destroy(dlg);

  if(res == GTK_RESPONSE_ACCEPT)
  {
    for(guint i = 0; i < items->len; i++)
    {
      _item_t *it = &g_array_index(items, _item_t, i);
      if(it->is_dir)
        _delete_dir_recursive(it->path);
      else
        g_remove(it->path);
    }
    _populate_tree(d);
    dt_control_log(ngettext("%d item removed", "%d items removed", (int)items->len),
                   (int)items->len);
  }

  g_array_free(items, TRUE);
}

static void _reset_clicked(GtkWidget *w, dt_lib_module_t *self)
{
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *dlg = gtk_message_dialog_new(
      GTK_WINDOW(win),
      GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
      GTK_MESSAGE_QUESTION,
      GTK_BUTTONS_NONE,
      _("Reset to original?"));
  gtk_message_dialog_format_secondary_text(
      GTK_MESSAGE_DIALOG(dlg),
      _("This will remove all edits and restore the image to its unprocessed original.\nThis operation can be undone."));
  gtk_dialog_add_buttons(GTK_DIALOG(dlg),
                         _("_cancel"), GTK_RESPONSE_CANCEL,
                         _("_reset"),  GTK_RESPONSE_ACCEPT,
                         NULL);
  gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_CANCEL);
  const gint res = gtk_dialog_run(GTK_DIALOG(dlg));
  gtk_widget_destroy(dlg);
  if(res != GTK_RESPONSE_ACCEPT) return;

  GList *imgs = dt_act_on_get_images(TRUE, TRUE, FALSE);
  if(!imgs) return;

  dt_dev_undo_start_record(darktable.develop);

  for(const GList *img = imgs; img; img = g_list_next(img))
  {
    const dt_imgid_t imgid = GPOINTER_TO_INT(img->data);
    dt_history_delete_on_image(imgid);
  }
  g_list_free(imgs);

  dt_dev_undo_end_record(darktable.develop);

  if(darktable.develop && darktable.develop->gui_attached)
  {
    dt_dev_modulegroups_set(darktable.develop, dt_dev_modulegroups_get(darktable.develop));
    dt_control_queue_redraw_center();
  }
}

/* Search entry changed: update VISIBLE column and refilter */
static void _search_changed(GtkEntry *entry, dt_lib_lrpresets_t *d)
{
  const char *text = gtk_entry_get_text(entry);

  GtkTreeIter iter;
  if(gtk_tree_model_get_iter_first(GTK_TREE_MODEL(d->store), &iter))
  {
    do {
      _set_visibility_recursive(d, &iter, text);
    } while(gtk_tree_model_iter_next(GTK_TREE_MODEL(d->store), &iter));
  }

  gtk_tree_model_filter_refilter(d->filter);

  if(text && *text)
    gtk_tree_view_expand_all(d->tree);
}

/* Double-click / Enter: toggle folders, apply presets */
static void _row_activated(GtkTreeView *tree, GtkTreePath *path, GtkTreeViewColumn *col,
                            dt_lib_module_t *self)
{
  dt_lib_lrpresets_t *d = self->data;

  GtkTreeModel *model = gtk_tree_view_get_model(tree);
  GtkTreeIter   filter_iter;
  if(!gtk_tree_model_get_iter(model, &filter_iter, path)) return;

  GtkTreeIter store_iter;
  gtk_tree_model_filter_convert_iter_to_child_iter(d->filter, &store_iter, &filter_iter);

  gboolean is_dir = FALSE;
  gtk_tree_model_get(GTK_TREE_MODEL(d->store), &store_iter, COL_IS_DIR, &is_dir, -1);

  if(is_dir)
  {
    if(gtk_tree_view_row_expanded(tree, path))
      gtk_tree_view_collapse_row(tree, path);
    else
      gtk_tree_view_expand_row(tree, path, FALSE);
  }
  else
  {
    _apply_clicked(NULL, self);
  }
}

/* ── Module lifecycle ─────────────────────────────────────────────────── */

void gui_init(dt_lib_module_t *self)
{
  dt_lib_lrpresets_t *d = calloc(1, sizeof(*d));
  self->data = d;

  _ensure_presets_dir(d);

  /* search / filter entry */
  d->search_entry = gtk_search_entry_new();
  gtk_widget_set_tooltip_text(d->search_entry, _("filter preset list"));

  /* tree store: name, path, is_dir, visible */
  d->store = gtk_tree_store_new(COL_NUM,
                                G_TYPE_STRING,  /* COL_NAME    */
                                G_TYPE_STRING,  /* COL_PATH    */
                                G_TYPE_BOOLEAN, /* COL_IS_DIR  */
                                G_TYPE_BOOLEAN  /* COL_VISIBLE */);

  /* filter model driven by COL_VISIBLE */
  d->filter = GTK_TREE_MODEL_FILTER(
      gtk_tree_model_filter_new(GTK_TREE_MODEL(d->store), NULL));
  gtk_tree_model_filter_set_visible_column(d->filter, COL_VISIBLE);

  /* tree view using the filter model */
  d->tree = GTK_TREE_VIEW(gtk_tree_view_new_with_model(GTK_TREE_MODEL(d->filter)));
  g_object_unref(d->filter); /* tree holds the reference */
  g_object_unref(d->store);  /* filter holds the reference */
  gtk_tree_view_set_headers_visible(d->tree, FALSE);
  gtk_tree_view_set_activate_on_single_click(d->tree, FALSE);
  gtk_tree_selection_set_mode(gtk_tree_view_get_selection(d->tree), GTK_SELECTION_MULTIPLE);

  /* single column: icon + name */
  GtkTreeViewColumn *col = gtk_tree_view_column_new();
  gtk_tree_view_column_set_expand(col, TRUE);

  GtkCellRenderer *pix = gtk_cell_renderer_pixbuf_new();
  gtk_tree_view_column_pack_start(col, pix, FALSE);
  gtk_tree_view_column_set_cell_data_func(col, pix, _icon_cell_func, NULL, NULL);

  GtkCellRenderer *txt = gtk_cell_renderer_text_new();
  g_object_set(txt, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
  gtk_tree_view_column_pack_start(col, txt, TRUE);
  gtk_tree_view_column_add_attribute(col, txt, "text", COL_NAME);

  gtk_tree_view_append_column(d->tree, col);

  /* populate and expand all top-level folders */
  _populate_tree(d);

  /* drag-and-drop: presets and folders can be dragged to other folders */
  gtk_tree_view_enable_model_drag_source(d->tree,
      GDK_BUTTON1_MASK, DND_TARGETS, G_N_ELEMENTS(DND_TARGETS), GDK_ACTION_MOVE);
  /* GTK_DEST_DEFAULT_DROP: automatically requests data on drop,
     triggering drag-data-received.  Motion is handled manually. */
  gtk_drag_dest_set(GTK_WIDGET(d->tree),
      GTK_DEST_DEFAULT_DROP, DND_TARGETS, G_N_ELEMENTS(DND_TARGETS), GDK_ACTION_MOVE);

  /* ensure we receive motion and leave events for hover preview */
  gtk_widget_add_events(GTK_WIDGET(d->tree), GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK);

  g_signal_connect(GTK_EDITABLE(d->search_entry), "changed", G_CALLBACK(_search_changed), d);
  g_signal_connect(d->tree, "row-activated",        G_CALLBACK(_row_activated),     self);
  g_signal_connect(d->tree, "button-press-event",   G_CALLBACK(_tree_button_press), self);
  g_signal_connect(d->tree, "motion-notify-event",  G_CALLBACK(_tree_motion),       self);
  g_signal_connect(d->tree, "leave-notify-event",   G_CALLBACK(_tree_leave),        self);
  g_signal_connect(d->tree, "drag-begin",           G_CALLBACK(_dnd_begin),         self);
  g_signal_connect(d->tree, "drag-data-get",        G_CALLBACK(_dnd_data_get),      self);
  g_signal_connect(d->tree, "drag-motion",          G_CALLBACK(_dnd_motion),        NULL);
  g_signal_connect(d->tree, "drag-data-received",   G_CALLBACK(_dnd_data_received), self);

  /* scrolled container */
  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                 GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_size_request(scroll, -1, 200);
  gtk_container_add(GTK_CONTAINER(scroll), GTK_WIDGET(d->tree));

  /* buttons */
  GtkWidget *import_btn = dt_action_button_new(self, N_("import files..."),
                                               _import_clicked, self,
                                               _("import XMP preset files into the library"), 0, 0);

  d->import_folder_btn = dt_action_button_new(self, N_("import folder..."),
                                              _import_folder_clicked, self,
                                              _("import an entire preset folder preserving its structure"), 0, 0);

  d->new_folder_btn = dt_action_button_new(self, N_("new folder"),
                                           _new_folder_clicked, self,
                                           _("create a sub-folder at the selected location"), 0, 0);

  d->remove_btn = dt_action_button_new(self, N_("remove"),
                                       _remove_clicked, self,
                                       _("remove selected preset or folder from the library"), 0, 0);

  d->apply_btn = dt_action_button_new(self, N_("apply preset"),
                                      _apply_clicked, self,
                                      _("apply selected preset to the selected / current image(s)"), 0, 0);

  d->reset_btn = dt_action_button_new(self, N_("reset to original"),
                                      _reset_clicked, self,
                                      _("discard all edits and restore image(s) to the unprocessed original"), 0, 0);

  /* layout:
     [ import files...  |  import folder... ]
     [ new folder       |  remove           ]
     [ apply preset                         ]
     [ reset to original                    ]
  */
  GtkWidget *btn_row1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(btn_row1), import_btn,           TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(btn_row1), d->import_folder_btn, TRUE, TRUE, 0);

  GtkWidget *btn_row2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(btn_row2), d->new_folder_btn, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(btn_row2), d->remove_btn,     TRUE, TRUE, 0);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(vbox), d->search_entry, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), scroll,          TRUE,  TRUE,  0);
  gtk_box_pack_start(GTK_BOX(vbox), btn_row1,        FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), btn_row2,        FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), d->apply_btn,    FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), d->reset_btn,    FALSE, FALSE, 0);

  self->widget = vbox;
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_lrpresets_t *d = self->data;
  _preview_cancel_timer(d);
  _preview_revert(d);
  g_free(d->preview_path);
  g_free(d->drag_sources);
  free(d);
  self->data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
