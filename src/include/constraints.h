/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Mutter size/position constraints */

/*
 * Copyright (C) 2002 Red Hat, Inc.
 * Copyright (C) 2005 Elijah Newren
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#ifndef META_CONSTRAINTS_H
#define META_CONSTRAINTS_H

#include "boxes.h"
#include "frame.h"

typedef enum
{
  PRIORITY_MINIMUM = 0, /* Dummy value used for loop start = min(all priorities) */
  PRIORITY_ASPECT_RATIO = 0,
  PRIORITY_ENTIRELY_VISIBLE_ON_SINGLE_MONITOR = 0,
  PRIORITY_ENTIRELY_VISIBLE_ON_WORKAREA = 1,
  PRIORITY_SIZE_HINTS_INCREMENTS = 1,
  PRIORITY_MAXIMIZATION = 2,
  PRIORITY_FULLSCREEN = 2,
  PRIORITY_SIZE_HINTS_LIMITS = 3,
  PRIORITY_TITLEBAR_VISIBLE = 4,
  PRIORITY_PARTIALLY_VISIBLE_ON_WORKAREA = 4,
  PRIORITY_MAXIMUM = 5 /* Value used for loop end = max(all priorities)
                        * Compositor plugins can use this value to override
                        * all other constraints
                        */
} ConstraintPriority;

typedef enum
{
  ACTION_MOVE,
  ACTION_RESIZE,
  ACTION_MOVE_AND_RESIZE
} ConstraintActionType;

typedef struct
{
  MetaRectangle        orig;
  MetaRectangle        current;
  MetaFrameGeometry   *fgeom;
  ConstraintActionType action_type;
  gboolean             is_user_action;

  /* I know that these two things probably look similar at first, but they
   * have much different uses.  See doc/how-constraints-works.txt for for
   * explanation of the differences and similarity between resize_gravity
   * and fixed_directions
   */
  int                  resize_gravity;
  FixedDirections      fixed_directions;

  /* work_area_monitor - current monitor region minus struts
   * entire_monitor    - current monitor, including strut regions
   */
  MetaRectangle        work_area_monitor;
  MetaRectangle        entire_monitor;

  /* Spanning rectangles for the non-covered (by struts) region of the
   * screen and also for just the current monitor
   */
  GList  *usable_screen_region;
  GList  *usable_monitor_region;
} ConstraintInfo;

void meta_constraints_unextend_by_frame (MetaRectangle           *rect,
                                         const MetaFrameGeometry *fgeom);

void meta_constraints_extend_by_frame (MetaRectangle           *rect,
                                       const MetaFrameGeometry *fgeom);

void meta_constraints_get_size_limits (const MetaWindow        *window,
                                       const MetaFrameGeometry *fgeom,
                                       gboolean include_frame,
                                       MetaRectangle *min_size,
                                       MetaRectangle *max_size);

#endif
