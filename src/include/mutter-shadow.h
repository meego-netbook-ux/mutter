#ifndef MUTTER_SHADOW_H
#define MUTTER_SHADOW_H

#include <clutter/clutter.h>
#include "compositor.h"

/**
 * MutterShadowType:
 * @MUTTER_SHADOW_AUTOMATIC: The compositor decides whether window should
 * have shadow based on window type
 * @MUTTER_SHADOW_ALWAYS: The window should have shadow
 * @MUTTER_SHADOW_NEWER:  The window should not have a shadow
 */
typedef enum
{
  MUTTER_SHADOW_AUTOMATIC,
  MUTTER_SHADOW_ALWAYS,
  MUTTER_SHADOW_NEVER
} MutterShadowType;

typedef struct _MutterShadow MutterShadow;

struct _MutterShadow
{
  ClutterActor *actor;
  gint          attach_left;
  gint          attach_top;
  gint          attach_right;
  gint          attach_bottom;
};

MutterShadow * mutter_shadow_new               (void);
void           mutter_shadow_destroy           (MutterShadow *shadow);
MutterShadow * mutter_shadow_create_for_window (MetaCompositor *compositor,
                                                MutterWindow   *window);

#endif /* MUTTER_SHADOW_H */
