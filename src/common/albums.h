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

#pragma once

#include <glib.h>
#include <stdint.h>

#include "common/darktable.h"

/* Represents a single album (manual collection, smart collection, or collection set). */
typedef struct dt_album_t
{
  int32_t  id;           /* primary key in albums table */
  char    *name;         /* display name */
  int32_t  parent_id;   /* 0 = top level */
  gboolean is_set;       /* TRUE = Collection Set (holds other albums, no direct photos) */
  gboolean is_smart;     /* TRUE = Smart Collection (filter rules, not manual membership) */
  char    *smart_rules;  /* serialized dt_collection filter params (is_smart only) */
  int32_t  position;     /* sort order among siblings */
  int32_t  count;        /* photo count; -1 = not yet computed */
} dt_album_t;

/* --- CRUD --- */

/** Create a new album. Returns the new album id, or -1 on failure.
 *  parent_id = 0 for a top-level album.
 *  rules is only meaningful when is_smart is TRUE; pass NULL otherwise. */
int32_t dt_album_create(const char *name,
                        int32_t parent_id,
                        gboolean is_set,
                        gboolean is_smart,
                        const char *rules);

/** Rename an existing album. */
void dt_album_rename(int32_t id, const char *new_name);

/** Delete an album (cascades to child albums and collection_images rows). */
void dt_album_delete(int32_t id);

/** Update the smart_rules string for a smart collection. */
void dt_album_set_rules(int32_t id, const char *rules);

/** Move an album to a different parent. Pass 0 to make it top-level. */
void dt_album_reparent(int32_t id, int32_t new_parent_id);

/* --- Membership (manual collections only) --- */

/** Add images (GList of GINT_TO_POINTER(imgid)) to a manual collection. */
void dt_album_add_images(int32_t id, const GList *imgids);

/** Remove images (GList of GINT_TO_POINTER(imgid)) from a manual collection. */
void dt_album_remove_images(int32_t id, const GList *imgids);

/** Returns TRUE if the given image is a member of the given manual collection. */
gboolean dt_album_contains_image(int32_t id, dt_imgid_t imgid);

/* --- Tree traversal --- */

/** Return a GList of dt_album_t* for all children of parent_id (0 = top level),
 *  ordered by position then name.  Caller must free with dt_album_list_free(). */
GList *dt_album_get_children(int32_t parent_id);

/** Return the dt_album_t for the given id, or NULL. Caller must free with dt_album_free(). */
dt_album_t *dt_album_get(int32_t id);

/** Compute and fill in the count field for an album. */
void dt_album_compute_count(dt_album_t *album);

void dt_album_free(dt_album_t *a);
void dt_album_list_free(GList *list);

/* --- Activation --- */

/** Activate an album as the current lighttable filter.
 *  For manual collections: sets DT_COLLECTION_PROP_ALBUM.
 *  For smart collections:  deserializes smart_rules into the current collection. */
void dt_album_activate(int32_t id);

/** Clear any active album filter (return to normal filter state). */
void dt_album_deactivate(void);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
