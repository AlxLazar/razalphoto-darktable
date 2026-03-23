/*
    This file is part of darktable,
    Copyright (C) 2025 darktable developers.

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
 * Albums panel — Lightroom-style virtual collections for darktable.
 *
 * Three kinds of node in the tree:
 *   Collection Set  – hierarchical folder, holds other albums; no photos
 *   Manual album    – user drags photos in; filtered via DT_COLLECTION_PROP_ALBUM
 *   Smart album     – filter rules serialised as dt_collection params
 */

#include "common/albums.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/selection.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/signal.h"
#include "dtgtk/button.h"
#include "gui/gtk.h"
#include "gui/drag_and_drop.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

#include <gdk/gdkkeysyms.h>

DT_MODULE(1)

/* ------------------------------------------------------------------ */
/* Column indices                                                       */
/* ------------------------------------------------------------------ */

enum
{
  COL_ID = 0,     /* int32  : album id (0 for roots) */
  COL_NAME,       /* string : display name */
  COL_COUNT,      /* string : "42" or "" for sets, "?" for smart */
  COL_IS_SET,     /* bool   : TRUE = Collection Set */
  COL_IS_SMART,   /* bool   : TRUE = Smart Collection */
  COL_NUM_COLS
};

/* ------------------------------------------------------------------ */
/* Private data                                                         */
/* ------------------------------------------------------------------ */

typedef struct dt_lib_albums_t
{
  GtkTreeStore  *store;
  GtkTreeView   *view;
  GtkWidget     *new_set_button;
  GtkWidget     *new_album_button;
  GtkWidget     *new_smart_button;
  GtkWidget     *delete_button;

  int32_t        active_id;   /* currently active album id, 0 = none */
} dt_lib_albums_t;

/* ------------------------------------------------------------------ */
/* Module registration                                                  */
/* ------------------------------------------------------------------ */

const char *name(dt_lib_module_t *self)
{
  return _("albums");
}

const char *description(dt_lib_module_t *self)
{
  return _("organise photos into virtual collections\n"
           "independent of their location on disk");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_LIGHTTABLE;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int position(const dt_lib_module_t *self)
{
  return 850;
}

/* ------------------------------------------------------------------ */
/* Tree helpers                                                         */
/* ------------------------------------------------------------------ */

static void _tree_populate(dt_lib_module_t *self, GtkTreeIter *parent, int32_t parent_id)
{
  dt_lib_albums_t *d = self->data;

  GList *children = dt_album_get_children(parent_id);
  for(GList *l = children; l; l = g_list_next(l))
  {
    dt_album_t *a = l->data;

    /* Compute count string */
    char count_str[32] = "";
    if(!a->is_set)
    {
      dt_album_compute_count(a);
      if(a->is_smart)
        g_strlcpy(count_str, "?", sizeof(count_str));
      else if(a->count >= 0)
        snprintf(count_str, sizeof(count_str), "%d", a->count);
    }

    GtkTreeIter iter;
    gtk_tree_store_append(d->store, &iter, parent);
    gtk_tree_store_set(d->store, &iter,
                       COL_ID,       a->id,
                       COL_NAME,     a->name,
                       COL_COUNT,    count_str,
                       COL_IS_SET,   a->is_set,
                       COL_IS_SMART, a->is_smart,
                       -1);

    /* Recurse into children (sets can contain albums / other sets) */
    _tree_populate(self, &iter, a->id);
  }
  dt_album_list_free(children);
}

static void _tree_refresh(dt_lib_module_t *self)
{
  dt_lib_albums_t *d = self->data;

  gtk_tree_store_clear(d->store);
  _tree_populate(self, NULL, 0);
  gtk_tree_view_expand_all(d->view);
}

/* Return the album id of the currently selected tree row, or -1. */
static int32_t _selected_album_id(dt_lib_albums_t *d)
{
  GtkTreeSelection *sel = gtk_tree_view_get_selection(d->view);
  GtkTreeModel *model;
  GtkTreeIter iter;
  if(!gtk_tree_selection_get_selected(sel, &model, &iter))
    return -1;
  int32_t id;
  gtk_tree_model_get(model, &iter, COL_ID, &id, -1);
  return id;
}

/* ------------------------------------------------------------------ */
/* Name entry dialog helper                                             */
/* ------------------------------------------------------------------ */

static char *_ask_name(GtkWindow *parent, const char *title, const char *default_text)
{
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *dialog = gtk_dialog_new_with_buttons(
    title, GTK_WINDOW(win),
    GTK_DIALOG_DESTROY_WITH_PARENT,
    _("_cancel"), GTK_RESPONSE_NONE,
    _("_ok"),     GTK_RESPONSE_YES,
    NULL);
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_YES);

  GtkWidget *entry = gtk_entry_new();
  gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
  if(default_text)
  {
    gtk_entry_set_text(GTK_ENTRY(entry), default_text);
    gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);
  }

  dt_gui_dialog_add(GTK_DIALOG(dialog),
                    dt_gui_hbox(gtk_label_new(_("name: ")), dt_gui_expand(entry)));
  gtk_widget_show_all(dialog);

  char *result = NULL;
  if(gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_YES)
  {
    const char *text = gtk_entry_get_text(GTK_ENTRY(entry));
    if(text && text[0])
      result = g_strdup(text);
  }
  gtk_widget_destroy(dialog);
  return result;
}

/* ------------------------------------------------------------------ */
/* Smart collection rule editor                                         */
/* ------------------------------------------------------------------ */

/*
 * The smart collection rule editor reuses the existing "collect" module
 * conf keys.  We snapshot them before opening, let the user edit via
 * the normal collect panel, then save the result into the album.
 *
 * A simpler approach: just use a text entry for a raw serialised string
 * so we don't need to embed the collect widget.  We can improve this later.
 */
static char *_ask_smart_rules(GtkWindow *parent, const char *existing_rules)
{
  GtkWidget *dialog = gtk_dialog_new_with_buttons(
    _("Smart collection rules"), parent,
    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
    _("_Cancel"), GTK_RESPONSE_CANCEL,
    _("_OK"),     GTK_RESPONSE_OK,
    NULL);

  GtkWidget *label = gtk_label_new(
    _("Enter serialised filter rules\n(copy from the Collections panel preset export)"));
  GtkWidget *entry = gtk_entry_new();
  gtk_entry_set_width_chars(GTK_ENTRY(entry), 60);
  if(existing_rules)
    gtk_entry_set_text(GTK_ENTRY(entry), existing_rules);
  gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);

  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_container_add(GTK_CONTAINER(content), label);
  gtk_container_add(GTK_CONTAINER(content), entry);
  gtk_widget_show_all(dialog);

  char *result = NULL;
  if(gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK)
  {
    const char *text = gtk_entry_get_text(GTK_ENTRY(entry));
    if(text && text[0])
      result = g_strdup(text);
  }
  gtk_widget_destroy(dialog);
  return result;
}

/* ------------------------------------------------------------------ */
/* Button callbacks                                                     */
/* ------------------------------------------------------------------ */

/* Determine where to create a new item:
 * - If a set is selected → create inside that set
 * - If a regular album is selected → create as sibling (inside its parent set)
 * - If nothing selected → create at top level */
static int32_t _parent_for_new_item(dt_lib_albums_t *d)
{
  const int32_t sel_id = _selected_album_id(d);
  if(sel_id <= 0) return 0;

  dt_album_t *sel = dt_album_get(sel_id);
  if(!sel) return 0;

  const int32_t parent = sel->is_set ? sel_id : sel->parent_id;
  dt_album_free(sel);
  return parent;
}

static void _on_new_set_clicked(GtkWidget *w, dt_lib_module_t *self)
{
  dt_lib_albums_t *d = self->data;
  const int32_t parent = _parent_for_new_item(d);

  char *name = _ask_name(NULL, _("New Collection Set"), _("New Set"));
  if(name)
  {
    dt_album_create(name, parent, TRUE, FALSE, NULL);
    g_free(name);
    _tree_refresh(self);
  }
}

static void _on_new_album_clicked(GtkWidget *w, dt_lib_module_t *self)
{
  dt_lib_albums_t *d = self->data;
  const int32_t parent = _parent_for_new_item(d);

  char *name = _ask_name(NULL, _("New Album"), _("New Album"));
  if(name)
  {
    dt_album_create(name, parent, FALSE, FALSE, NULL);
    g_free(name);
    _tree_refresh(self);
  }
}

static void _on_new_smart_clicked(GtkWidget *w, dt_lib_module_t *self)
{
  dt_lib_albums_t *d = self->data;
  const int32_t parent = _parent_for_new_item(d);

  char *name = _ask_name(NULL, _("New Smart Album"), _("New Smart Album"));
  if(!name) return;

  char *rules = _ask_smart_rules(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), NULL);
  if(!rules)
  {
    g_free(name);
    return;
  }

  dt_album_create(name, parent, FALSE, TRUE, rules);
  g_free(name);
  g_free(rules);
  _tree_refresh(self);
}

static void _on_delete_clicked(GtkWidget *w, dt_lib_module_t *self)
{
  dt_lib_albums_t *d = self->data;
  const int32_t id = _selected_album_id(d);
  if(id <= 0) return;

  dt_album_t *album = dt_album_get(id);
  if(!album) return;

  GtkWidget *dialog = gtk_message_dialog_new(
    GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)),
    GTK_DIALOG_DESTROY_WITH_PARENT,
    GTK_MESSAGE_QUESTION,
    GTK_BUTTONS_YES_NO,
    album->is_set
      ? _("Delete collection set \"%s\"?\nAll albums inside will also be deleted.")
      : _("Delete album \"%s\"?\nPhotos will not be deleted from disk."),
    album->name);
  dt_album_free(album);

  const gint response = gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);

  if(response == GTK_RESPONSE_YES)
  {
    if(d->active_id == id) d->active_id = 0;
    dt_album_delete(id);
    _tree_refresh(self);
  }
}

/* ------------------------------------------------------------------ */
/* Tree row activation (single click)                                   */
/* ------------------------------------------------------------------ */

static void _on_row_activated(GtkTreeView *view,
                               GtkTreePath *path,
                               GtkTreeViewColumn *col,
                               dt_lib_module_t *self)
{
  dt_lib_albums_t *d = self->data;
  GtkTreeModel *model = gtk_tree_view_get_model(view);
  GtkTreeIter iter;
  if(!gtk_tree_model_get_iter(model, &iter, path)) return;

  int32_t id;
  gboolean is_set;
  gtk_tree_model_get(model, &iter,
                     COL_ID,     &id,
                     COL_IS_SET, &is_set,
                     -1);

  if(is_set) return; /* sets don't filter the library */

  d->active_id = id;
  dt_album_activate(id);
}

/* ------------------------------------------------------------------ */
/* Context menu                                                         */
/* ------------------------------------------------------------------ */

static void _ctx_rename(GtkMenuItem *item, dt_lib_module_t *self)
{
  dt_lib_albums_t *d = self->data;
  const int32_t id = _selected_album_id(d);
  if(id <= 0) return;

  dt_album_t *album = dt_album_get(id);
  if(!album) return;
  char *name = _ask_name(NULL, _("Rename"), album->name);
  dt_album_free(album);

  if(name)
  {
    dt_album_rename(id, name);
    g_free(name);
    _tree_refresh(self);
  }
}

static void _ctx_add_selected(GtkMenuItem *item, dt_lib_module_t *self)
{
  dt_lib_albums_t *d = self->data;
  const int32_t id = _selected_album_id(d);
  if(id <= 0) return;

  GList *imgs = dt_selection_get_list(darktable.selection, FALSE, FALSE);
  if(imgs)
  {
    dt_album_add_images(id, imgs);
    g_list_free(imgs);
    _tree_refresh(self);
  }
}

static void _ctx_edit_rules(GtkMenuItem *item, dt_lib_module_t *self)
{
  dt_lib_albums_t *d = self->data;
  const int32_t id = _selected_album_id(d);
  if(id <= 0) return;

  dt_album_t *album = dt_album_get(id);
  if(!album || !album->is_smart)
  {
    dt_album_free(album);
    return;
  }

  char *rules = _ask_smart_rules(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)),
                                  album->smart_rules);
  dt_album_free(album);

  if(rules)
  {
    dt_album_set_rules(id, rules);
    g_free(rules);
    _tree_refresh(self);
  }
}

static void _ctx_remove_selected(GtkMenuItem *item, dt_lib_module_t *self)
{
  dt_lib_albums_t *d = self->data;
  const int32_t id = _selected_album_id(d);
  if(id <= 0) return;

  /* Build list of currently selected images in the lighttable */
  GList *imgs = dt_selection_get_list(darktable.selection, FALSE, FALSE);
  if(imgs)
  {
    dt_album_remove_images(id, imgs);
    g_list_free(imgs);
    _tree_refresh(self);
  }
}

static gboolean _on_button_press(GtkWidget *w, GdkEventButton *event, dt_lib_module_t *self)
{
  if(event->button != GDK_BUTTON_SECONDARY) return FALSE;

  /* Find out what was clicked */
  GtkTreePath *path = NULL;
  gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w),
                                (gint)event->x, (gint)event->y,
                                &path, NULL, NULL, NULL);

  int32_t id = -1;
  gboolean is_set = FALSE, is_smart = FALSE;
  if(path)
  {
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(w));
    GtkTreeIter iter;
    if(gtk_tree_model_get_iter(model, &iter, path))
    {
      gtk_tree_model_get(model, &iter,
                         COL_ID,       &id,
                         COL_IS_SET,   &is_set,
                         COL_IS_SMART, &is_smart,
                         -1);
      GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(w));
      gtk_tree_selection_select_iter(sel, &iter);
    }
    gtk_tree_path_free(path);
  }

  GtkWidget *menu = gtk_menu_new();

  /* Common: new set / new album / new smart album */
  GtkWidget *mi;
  mi = gtk_menu_item_new_with_label(_("New Collection Set"));
  g_signal_connect(mi, "activate", G_CALLBACK(_on_new_set_clicked), self);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);

  mi = gtk_menu_item_new_with_label(_("New Album"));
  g_signal_connect(mi, "activate", G_CALLBACK(_on_new_album_clicked), self);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);

  mi = gtk_menu_item_new_with_label(_("New Smart Album"));
  g_signal_connect(mi, "activate", G_CALLBACK(_on_new_smart_clicked), self);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);

  if(id > 0)
  {
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    mi = gtk_menu_item_new_with_label(_("Rename…"));
    g_signal_connect(mi, "activate", G_CALLBACK(_ctx_rename), self);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);

    if(is_smart)
    {
      mi = gtk_menu_item_new_with_label(_("Edit rules…"));
      g_signal_connect(mi, "activate", G_CALLBACK(_ctx_edit_rules), self);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }

    if(!is_set && !is_smart)
    {
      mi = gtk_menu_item_new_with_label(_("Add selected photos to album"));
      g_signal_connect(mi, "activate", G_CALLBACK(_ctx_add_selected), self);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);

      mi = gtk_menu_item_new_with_label(_("Remove selected photos from album"));
      g_signal_connect(mi, "activate", G_CALLBACK(_ctx_remove_selected), self);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    mi = gtk_menu_item_new_with_label(_("Delete…"));
    g_signal_connect(mi, "activate", G_CALLBACK(_on_delete_clicked), self);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
  }

  gtk_widget_show_all(menu);
  gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
  return TRUE;
}

/* ------------------------------------------------------------------ */
/* Drag-and-drop                                                        */
/* ------------------------------------------------------------------ */

static gboolean _dnd_motion(GtkWidget *w,
                             GdkDragContext *context,
                             gint x, gint y,
                             guint time,
                             gpointer user_data)
{
  GtkTreePath *path = NULL;
  if(gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w), x, y, &path, NULL, NULL, NULL))
  {
    gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(w), path, GTK_TREE_VIEW_DROP_INTO_OR_BEFORE);
    gtk_tree_path_free(path);
    gdk_drag_status(context, GDK_ACTION_MOVE, time);
    return TRUE;
  }
  gdk_drag_status(context, 0, time);
  return FALSE;
}

static void _dnd_data_received(GtkWidget *w,
                                GdkDragContext *context,
                                gint x, gint y,
                                GtkSelectionData *selection_data,
                                guint target_type,
                                guint time,
                                dt_lib_module_t *self)
{
  gboolean success = FALSE;

  if(target_type == DND_TARGET_IMGID && selection_data)
  {
    GtkTreePath *path = NULL;
    const int imgs_nb = gtk_selection_data_get_length(selection_data) / sizeof(dt_imgid_t);
    if(imgs_nb && gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w), x, y, &path, NULL, NULL, NULL))
    {
      GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(w));
      GtkTreeIter iter;
      if(gtk_tree_model_get_iter(model, &iter, path))
      {
        int32_t id;
        gboolean is_set, is_smart;
        gtk_tree_model_get(model, &iter,
                           COL_ID,       &id,
                           COL_IS_SET,   &is_set,
                           COL_IS_SMART, &is_smart,
                           -1);

        if(id > 0 && !is_set && !is_smart)
        {
          const dt_imgid_t *imgt = (const dt_imgid_t *)gtk_selection_data_get_data(selection_data);
          GList *imgs = NULL;
          for(int i = 0; i < imgs_nb; i++)
            imgs = g_list_prepend(imgs, GINT_TO_POINTER(imgt[i]));
          dt_album_add_images(id, imgs);
          g_list_free(imgs);
          success = TRUE;

          /* Refresh count on this row */
          dt_album_t *album = dt_album_get(id);
          if(album)
          {
            dt_album_compute_count(album);
            char count_str[32] = "";
            if(album->count >= 0)
              snprintf(count_str, sizeof(count_str), "%d", album->count);
            gtk_tree_store_set(GTK_TREE_STORE(model), &iter,
                               COL_COUNT, count_str, -1);
            dt_album_free(album);
          }
        }
      }
      gtk_tree_path_free(path);
    }
  }

  gtk_drag_finish(context, success, FALSE, time);
}

/* ------------------------------------------------------------------ */
/* Signal handler: rebuild tree when albums change                      */
/* ------------------------------------------------------------------ */

static void _on_albums_changed(gpointer instance, dt_lib_module_t *self)
{
  _tree_refresh(self);
}

/* ------------------------------------------------------------------ */
/* Name/count cell renderer data functions                              */
/* ------------------------------------------------------------------ */

static void _name_cell_data(GtkTreeViewColumn *col,
                             GtkCellRenderer *renderer,
                             GtkTreeModel *model,
                             GtkTreeIter *iter,
                             gpointer data)
{
  gboolean is_set, is_smart;
  char *name;
  gtk_tree_model_get(model, iter,
                     COL_NAME,     &name,
                     COL_IS_SET,   &is_set,
                     COL_IS_SMART, &is_smart,
                     -1);

  /* Prefix icon via markup */
  const char *prefix = is_set ? "📁 " : (is_smart ? "⚡ " : "🖼 ");
  char *markup = g_markup_printf_escaped("%s%s", prefix, name ? name : "");
  g_object_set(renderer, "markup", markup, NULL);
  g_free(markup);
  g_free(name);
}

/* ------------------------------------------------------------------ */
/* gui_init / gui_cleanup                                               */
/* ------------------------------------------------------------------ */

void gui_init(dt_lib_module_t *self)
{
  dt_lib_albums_t *d = calloc(1, sizeof(dt_lib_albums_t));
  self->data = d;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  /* ---- Tree view ---- */
  d->store = gtk_tree_store_new(COL_NUM_COLS,
                                G_TYPE_INT,     /* COL_ID */
                                G_TYPE_STRING,  /* COL_NAME */
                                G_TYPE_STRING,  /* COL_COUNT */
                                G_TYPE_BOOLEAN, /* COL_IS_SET */
                                G_TYPE_BOOLEAN  /* COL_IS_SMART */
                                );

  d->view = GTK_TREE_VIEW(gtk_tree_view_new_with_model(GTK_TREE_MODEL(d->store)));
  g_object_unref(d->store);
  gtk_tree_view_set_headers_visible(d->view, FALSE);
  gtk_tree_view_set_enable_search(d->view, FALSE);
  gtk_tree_selection_set_mode(gtk_tree_view_get_selection(d->view), GTK_SELECTION_SINGLE);

  /* Name column */
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
  GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
    _("Name"), renderer, "text", COL_NAME, NULL);
  gtk_tree_view_column_set_expand(col, TRUE);
  gtk_tree_view_column_set_cell_data_func(col, renderer, _name_cell_data, NULL, NULL);
  gtk_tree_view_append_column(d->view, col);

  /* Count column */
  GtkCellRenderer *count_renderer = gtk_cell_renderer_text_new();
  g_object_set(count_renderer, "xalign", 1.0, NULL);
  GtkTreeViewColumn *count_col = gtk_tree_view_column_new_with_attributes(
    _("Count"), count_renderer, "text", COL_COUNT, NULL);
  gtk_tree_view_column_set_sizing(count_col, GTK_TREE_VIEW_COLUMN_AUTOSIZE);
  gtk_tree_view_append_column(d->view, count_col);

  /* Scroll + resize wrap */
  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                 GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_container_add(GTK_CONTAINER(scroll), GTK_WIDGET(d->view));
  GtkWidget *wrapped = dt_ui_resize_wrap(scroll, 150,
                                         "plugins/lighttable/albums/height");
  gtk_box_pack_start(GTK_BOX(self->widget), wrapped, TRUE, TRUE, 0);

  /* Signals on tree view */
  g_signal_connect(d->view, "row-activated",
                   G_CALLBACK(_on_row_activated), self);
  g_signal_connect(d->view, "button-press-event",
                   G_CALLBACK(_on_button_press), self);

  /* Drag-and-drop: receive image ids from lighttable.
     Thumbtable sources with GDK_ACTION_MOVE so we must accept MOVE here too. */
  gtk_drag_dest_set(GTK_WIDGET(d->view),
                    GTK_DEST_DEFAULT_ALL,
                    target_list_internal,
                    n_targets_internal,
                    GDK_ACTION_MOVE | GDK_ACTION_COPY);
  g_signal_connect(d->view, "drag-motion",
                   G_CALLBACK(_dnd_motion), NULL);
  g_signal_connect(d->view, "drag-data-received",
                   G_CALLBACK(_dnd_data_received), self);

  /* ---- Button bar ---- */
  GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), btn_box, FALSE, FALSE, 0);

  d->new_set_button = dtgtk_button_new(dtgtk_cairo_paint_square_plus, 0, NULL);
  gtk_widget_set_tooltip_text(d->new_set_button, _("New Collection Set"));
  g_signal_connect(d->new_set_button, "clicked", G_CALLBACK(_on_new_set_clicked), self);
  gtk_box_pack_start(GTK_BOX(btn_box), d->new_set_button, FALSE, FALSE, 0);

  d->new_album_button = dtgtk_button_new(dtgtk_cairo_paint_plus, 0, NULL);
  gtk_widget_set_tooltip_text(d->new_album_button, _("New Album"));
  g_signal_connect(d->new_album_button, "clicked", G_CALLBACK(_on_new_album_clicked), self);
  gtk_box_pack_start(GTK_BOX(btn_box), d->new_album_button, FALSE, FALSE, 0);

  d->new_smart_button = dtgtk_button_new(dtgtk_cairo_paint_filtering_menu, 0, NULL);
  gtk_widget_set_tooltip_text(d->new_smart_button, _("New Smart Album"));
  g_signal_connect(d->new_smart_button, "clicked", G_CALLBACK(_on_new_smart_clicked), self);
  gtk_box_pack_start(GTK_BOX(btn_box), d->new_smart_button, FALSE, FALSE, 0);

  d->delete_button = dtgtk_button_new(dtgtk_cairo_paint_remove, 0, NULL);
  gtk_widget_set_tooltip_text(d->delete_button, _("Delete selected album"));
  g_signal_connect(d->delete_button, "clicked", G_CALLBACK(_on_delete_clicked), self);
  gtk_box_pack_end(GTK_BOX(btn_box), d->delete_button, FALSE, FALSE, 0);

  /* ---- Populate tree and connect signal ---- */
  _tree_refresh(self);

  DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_ALBUMS_CHANGED, _on_albums_changed, self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  DT_CONTROL_SIGNAL_DISCONNECT(_on_albums_changed, self);
  free(self->data);
  self->data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
