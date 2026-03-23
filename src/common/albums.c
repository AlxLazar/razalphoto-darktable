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

#include "common/albums.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "control/conf.h"
#include "control/signal.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* CRUD                                                                 */
/* ------------------------------------------------------------------ */

int32_t dt_album_create(const char *name,
                        const int32_t parent_id,
                        const gboolean is_set,
                        const gboolean is_smart,
                        const char *rules)
{
  if(!name || name[0] == '\0') return -1;

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(
    dt_database_get(darktable.db),
    "INSERT INTO main.albums (name, parent_id, is_set, is_smart, smart_rules, position)"
    " VALUES (?1, ?2, ?3, ?4, ?5,"
    "  COALESCE((SELECT MAX(position) FROM main.albums WHERE parent_id IS ?2), -1) + 1)",
    -1, &stmt, NULL);
  sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
  if(parent_id > 0)
    sqlite3_bind_int(stmt, 2, parent_id);
  else
    sqlite3_bind_null(stmt, 2);
  sqlite3_bind_int(stmt, 3, is_set ? 1 : 0);
  sqlite3_bind_int(stmt, 4, is_smart ? 1 : 0);
  if(is_smart && rules)
    sqlite3_bind_text(stmt, 5, rules, -1, SQLITE_TRANSIENT);
  else
    sqlite3_bind_null(stmt, 5);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  const int32_t new_id = (int32_t)sqlite3_last_insert_rowid(dt_database_get(darktable.db));

  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_ALBUMS_CHANGED);

  return new_id;
}

void dt_album_rename(const int32_t id, const char *new_name)
{
  if(id <= 0 || !new_name || new_name[0] == '\0') return;

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(
    dt_database_get(darktable.db),
    "UPDATE main.albums SET name = ?1 WHERE id = ?2",
    -1, &stmt, NULL);
  sqlite3_bind_text(stmt, 1, new_name, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 2, id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_ALBUMS_CHANGED);
}

void dt_album_delete(const int32_t id)
{
  if(id <= 0) return;

  sqlite3_stmt *stmt;
  // CASCADE in the schema will remove child albums and collection_images rows
  DT_DEBUG_SQLITE3_PREPARE_V2(
    dt_database_get(darktable.db),
    "DELETE FROM main.albums WHERE id = ?1",
    -1, &stmt, NULL);
  sqlite3_bind_int(stmt, 1, id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_ALBUMS_CHANGED);
}

void dt_album_set_rules(const int32_t id, const char *rules)
{
  if(id <= 0) return;

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(
    dt_database_get(darktable.db),
    "UPDATE main.albums SET smart_rules = ?1 WHERE id = ?2",
    -1, &stmt, NULL);
  if(rules)
    sqlite3_bind_text(stmt, 1, rules, -1, SQLITE_TRANSIENT);
  else
    sqlite3_bind_null(stmt, 1);
  sqlite3_bind_int(stmt, 2, id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_ALBUMS_CHANGED);
}

void dt_album_reparent(const int32_t id, const int32_t new_parent_id)
{
  if(id <= 0) return;

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(
    dt_database_get(darktable.db),
    "UPDATE main.albums SET parent_id = ?1,"
    " position = COALESCE((SELECT MAX(position) FROM main.albums WHERE parent_id IS ?1), -1) + 1"
    " WHERE id = ?2",
    -1, &stmt, NULL);
  if(new_parent_id > 0)
    sqlite3_bind_int(stmt, 1, new_parent_id);
  else
    sqlite3_bind_null(stmt, 1);
  sqlite3_bind_int(stmt, 2, id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_ALBUMS_CHANGED);
}

/* ------------------------------------------------------------------ */
/* Membership                                                           */
/* ------------------------------------------------------------------ */

void dt_album_add_images(const int32_t id, const GList *imgids)
{
  if(id <= 0 || !imgids) return;

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(
    dt_database_get(darktable.db),
    "INSERT OR IGNORE INTO main.collection_images (collection_id, imgid) VALUES (?1, ?2)",
    -1, &stmt, NULL);

  for(const GList *l = imgids; l; l = g_list_next(l))
  {
    const dt_imgid_t imgid = GPOINTER_TO_INT(l->data);
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_bind_int(stmt, 2, imgid);
    sqlite3_step(stmt);
    sqlite3_reset(stmt);
  }
  sqlite3_finalize(stmt);

  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_ALBUMS_CHANGED);
}

void dt_album_remove_images(const int32_t id, const GList *imgids)
{
  if(id <= 0 || !imgids) return;

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(
    dt_database_get(darktable.db),
    "DELETE FROM main.collection_images WHERE collection_id = ?1 AND imgid = ?2",
    -1, &stmt, NULL);

  for(const GList *l = imgids; l; l = g_list_next(l))
  {
    const dt_imgid_t imgid = GPOINTER_TO_INT(l->data);
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_bind_int(stmt, 2, imgid);
    sqlite3_step(stmt);
    sqlite3_reset(stmt);
  }
  sqlite3_finalize(stmt);

  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_ALBUMS_CHANGED);
}

gboolean dt_album_contains_image(const int32_t id, const dt_imgid_t imgid)
{
  if(id <= 0 || !dt_is_valid_imgid(imgid)) return FALSE;

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(
    dt_database_get(darktable.db),
    "SELECT 1 FROM main.collection_images WHERE collection_id = ?1 AND imgid = ?2 LIMIT 1",
    -1, &stmt, NULL);
  sqlite3_bind_int(stmt, 1, id);
  sqlite3_bind_int(stmt, 2, imgid);
  const gboolean found = (sqlite3_step(stmt) == SQLITE_ROW);
  sqlite3_finalize(stmt);
  return found;
}

/* ------------------------------------------------------------------ */
/* Tree traversal                                                       */
/* ------------------------------------------------------------------ */

static dt_album_t *_row_to_album(sqlite3_stmt *stmt)
{
  dt_album_t *a = g_malloc0(sizeof(dt_album_t));
  a->id         = sqlite3_column_int(stmt, 0);
  a->name       = g_strdup((const char *)sqlite3_column_text(stmt, 1));
  a->parent_id  = sqlite3_column_int(stmt, 2);  /* 0 if NULL */
  a->is_set     = sqlite3_column_int(stmt, 3) != 0;
  a->is_smart   = sqlite3_column_int(stmt, 4) != 0;
  const char *rules = (const char *)sqlite3_column_text(stmt, 5);
  a->smart_rules = rules ? g_strdup(rules) : NULL;
  a->position   = sqlite3_column_int(stmt, 6);
  a->count      = -1;
  return a;
}

GList *dt_album_get_children(const int32_t parent_id)
{
  sqlite3_stmt *stmt;
  if(parent_id > 0)
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT id, name, COALESCE(parent_id, 0), is_set, is_smart, smart_rules, position"
      " FROM main.albums WHERE parent_id = ?1 ORDER BY position, name",
      -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, parent_id);
  }
  else
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT id, name, COALESCE(parent_id, 0), is_set, is_smart, smart_rules, position"
      " FROM main.albums WHERE parent_id IS NULL ORDER BY position, name",
      -1, &stmt, NULL);
  }

  GList *result = NULL;
  while(sqlite3_step(stmt) == SQLITE_ROW)
    result = g_list_append(result, _row_to_album(stmt));
  sqlite3_finalize(stmt);
  return result;
}

dt_album_t *dt_album_get(const int32_t id)
{
  if(id <= 0) return NULL;

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(
    dt_database_get(darktable.db),
    "SELECT id, name, COALESCE(parent_id, 0), is_set, is_smart, smart_rules, position"
    " FROM main.albums WHERE id = ?1",
    -1, &stmt, NULL);
  sqlite3_bind_int(stmt, 1, id);

  dt_album_t *a = NULL;
  if(sqlite3_step(stmt) == SQLITE_ROW)
    a = _row_to_album(stmt);
  sqlite3_finalize(stmt);
  return a;
}

void dt_album_compute_count(dt_album_t *album)
{
  if(!album || album->is_set) return;

  sqlite3_stmt *stmt;
  if(album->is_smart)
  {
    /* For smart collections we'd need to re-run their filter query.
       For now, mark as unknown (UI can show "?"). */
    album->count = -1;
    return;
  }

  DT_DEBUG_SQLITE3_PREPARE_V2(
    dt_database_get(darktable.db),
    "SELECT COUNT(*) FROM main.collection_images WHERE collection_id = ?1",
    -1, &stmt, NULL);
  sqlite3_bind_int(stmt, 1, album->id);
  if(sqlite3_step(stmt) == SQLITE_ROW)
    album->count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
}

void dt_album_free(dt_album_t *a)
{
  if(!a) return;
  g_free(a->name);
  g_free(a->smart_rules);
  g_free(a);
}

void dt_album_list_free(GList *list)
{
  g_list_free_full(list, (GDestroyNotify)dt_album_free);
}

/* ------------------------------------------------------------------ */
/* Activation                                                           */
/* ------------------------------------------------------------------ */

void dt_album_activate(const int32_t id)
{
  if(id <= 0) return;

  dt_album_t *album = dt_album_get(id);
  if(!album) return;

  if(album->is_set)
  {
    // Collection Sets hold no photos themselves — do nothing.
    dt_album_free(album);
    return;
  }

  if(album->is_smart && album->smart_rules)
  {
    // Deserialize the saved filter rules and update the current collection.
    dt_collection_deserialize(album->smart_rules, FALSE);
    // dt_collection_deserialize already calls dt_collection_update_query internally.
  }
  else
  {
    // Manual collection: set a single "album" filter rule.
    char album_id_str[32];
    snprintf(album_id_str, sizeof(album_id_str), "%d", id);

    dt_conf_set_int("plugins/lighttable/collect/num_rules", 1);
    dt_conf_set_int("plugins/lighttable/collect/mode0", 0);   // AND
    dt_conf_set_int("plugins/lighttable/collect/item0", DT_COLLECTION_PROP_ALBUM);
    dt_conf_set_string("plugins/lighttable/collect/string0", album_id_str);

    dt_collection_update_query(darktable.collection,
                               DT_COLLECTION_CHANGE_NEW_QUERY,
                               DT_COLLECTION_PROP_UNDEF, NULL);
  }

  dt_album_free(album);
}

void dt_album_deactivate(void)
{
  // Reset to "show all" (filmroll = %)
  dt_conf_set_int("plugins/lighttable/collect/num_rules", 1);
  dt_conf_set_int("plugins/lighttable/collect/mode0", 0);
  dt_conf_set_int("plugins/lighttable/collect/item0", DT_COLLECTION_PROP_FILMROLL);
  dt_conf_set_string("plugins/lighttable/collect/string0", "%");

  dt_collection_update_query(darktable.collection,
                             DT_COLLECTION_CHANGE_NEW_QUERY,
                             DT_COLLECTION_PROP_UNDEF, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
