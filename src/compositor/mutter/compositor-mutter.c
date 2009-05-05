/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

#define _GNU_SOURCE
#define _XOPEN_SOURCE 500 /* for usleep() */

#include <config.h>

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#include <gdk/gdk.h>

#include "../../core/window-private.h"
#include "display.h"
#include "screen.h"
#include "frame.h"
#include "errors.h"
#include "window.h"
#include "compositor-private.h"
#include "compositor-mutter.h"
#include "mutter-plugin-manager.h"
#include "tidy/tidy-texture-frame.h"
#include "xprops.h"
#include "prefs.h"
#include "mutter-shaped-texture.h"
#include <X11/Xatom.h>
#include <X11/Xlibint.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrender.h>

#include <clutter/clutter.h>
#include <clutter/x11/clutter-x11.h>

#include <cogl/cogl.h>
#define SHADOW_RADIUS 8
#define SHADOW_OPACITY	0.9
#define SHADOW_OFFSET_X	(SHADOW_RADIUS)
#define SHADOW_OFFSET_Y	(SHADOW_RADIUS)

#define MAX_TILE_SZ 8 	/* Must be <= shaddow radius */
#define TILE_WIDTH  (3*MAX_TILE_SZ)
#define TILE_HEIGHT (3*MAX_TILE_SZ)

#define CHECK_LIST_INTEGRITY_START(list)	\
  {int len2__; int len__ = g_list_length(list);

#define CHECK_LIST_INTEGRITY_END(list)			    \
  len2__ = g_list_length(list);				    \
  if (len__ != len2__)					    \
    g_warning ("Integrity check of list failed at %s:%d\n", \
	       __FILE__, __LINE__); }

/* #define DEBUG_TRACE g_print */
#define DEBUG_TRACE(X)

/*
 * Register GType wrapper for XWindowAttributes, so we do not have to
 * query window attributes in the MutterWindow constructor but can pass
 * them as a property to the constructor (so we can gracefully handle the case
 * where no attributes can be retrieved).
 *
 * NB -- we only need a subset of the attributes; at some point we might want
 * to just store the relevant values rather than the whole struct.
 */
#define META_TYPE_XATTRS (meta_xattrs_get_type ())

GType meta_xattrs_get_type   (void) G_GNUC_CONST;

static XWindowAttributes *
meta_xattrs_copy (const XWindowAttributes *attrs)
{
  XWindowAttributes *result;

  g_return_val_if_fail (attrs != NULL, NULL);

  result = (XWindowAttributes*) Xmalloc (sizeof (XWindowAttributes));
  *result = *attrs;

  return result;
}

static void
meta_xattrs_free (XWindowAttributes *attrs)
{
  g_return_if_fail (attrs != NULL);

  XFree (attrs);
}

GType
meta_xattrs_get_type (void)
{
  static GType our_type = 0;

  if (!our_type)
    our_type = g_boxed_type_register_static ("XWindowAttributes",
		                     (GBoxedCopyFunc) meta_xattrs_copy,
				     (GBoxedFreeFunc) meta_xattrs_free);
  return our_type;
}

static unsigned char* shadow_gaussian_make_tile (void);

#ifdef HAVE_COMPOSITE_EXTENSIONS
static inline gboolean
composite_at_least_version (MetaDisplay *display, int maj, int min)
{
  static int major = -1;
  static int minor = -1;

  if (major == -1)
    meta_display_get_compositor_version (display, &major, &minor);

  return (major > maj || (major == maj && minor >= min));
}
#endif

typedef struct _Mutter
{
  MetaCompositor  compositor;
  MetaDisplay    *display;

  Atom            atom_x_root_pixmap;
  Atom            atom_x_set_root;
  Atom            atom_net_wm_window_opacity;

  ClutterActor   *shadow_src;

  gboolean        show_redraw : 1;
  gboolean        debug       : 1;
  gboolean        no_mipmaps  : 1;
} Mutter;

typedef struct _MetaCompScreen
{
  MetaScreen            *screen;

  ClutterActor          *stage, *window_group, *overlay_group;
  ClutterActor		*hidden_group;
  GList                 *windows;
  GHashTable            *windows_by_xid;
  MetaWindow            *focus_window;
  Window                 output;
  GSList                *dock_windows;

  /* Before we create the output window */
  XserverRegion     pending_input_region;

  gint                   switch_workspace_in_progress;

  MutterPluginManager *plugin_mgr;
} MetaCompScreen;

/*
 * MutterWindow implementation
 */
struct _MutterWindowPrivate
{
  XWindowAttributes attrs;

  MetaWindow       *window;
  Window            xwindow;
  MetaScreen       *screen;

  ClutterActor     *actor;
  ClutterActor     *shadow;
  Pixmap            back_pixmap;

  MetaCompWindowType  type;
  Damage            damage;

  guint8            opacity;

  gchar *           desc;

  /*
   * These need to be counters rather than flags, since more plugins
   * can implement same effect; the practicality of stacking effects
   * might be dubious, but we have to at least handle it correctly.
   */
  gint              minimize_in_progress;
  gint              maximize_in_progress;
  gint              unmaximize_in_progress;
  gint              map_in_progress;
  gint              destroy_in_progress;

  guint		    shaped                 : 1;
  guint		    destroy_pending        : 1;
  guint		    argb32                 : 1;
  guint		    disposed               : 1;
  guint		    is_minimized           : 1;
  guint		    hide_after_effect      : 1;
  guint             redecorating           : 1;

  /* Desktop switching flags */
  guint		    needs_map              : 1;
  guint		    needs_unmap            : 1;
  guint		    needs_repair           : 1;

  guint		    needs_destroy	   : 1;

  guint             no_shadow              : 1;

  guint             no_more_x_calls        : 1;
};

enum
{
  PROP_MCW_META_WINDOW = 1,
  PROP_MCW_META_SCREEN,
  PROP_MCW_X_WINDOW,
  PROP_MCW_X_WINDOW_ATTRIBUTES,
  PROP_MCW_NO_SHADOW,
};

static void mutter_window_class_init (MutterWindowClass *klass);
static void mutter_window_init       (MutterWindow *self);
static void mutter_window_dispose    (GObject *object);
static void mutter_window_finalize   (GObject *object);
static void mutter_window_constructed (GObject *object);
static void mutter_window_set_property (GObject       *object,
					   guint         prop_id,
					   const GValue *value,
					   GParamSpec   *pspec);
static void mutter_window_get_property (GObject      *object,
					   guint         prop_id,
					   GValue       *value,
					   GParamSpec   *pspec);
static void mutter_window_query_window_type (MutterWindow *self);
static void mutter_window_detach (MutterWindow *self);

G_DEFINE_TYPE (MutterWindow, mutter_window, CLUTTER_TYPE_GROUP);

static void
mutter_window_class_init (MutterWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GParamSpec   *pspec;

  g_type_class_add_private (klass, sizeof (MutterWindowPrivate));

  object_class->dispose      = mutter_window_dispose;
  object_class->finalize     = mutter_window_finalize;
  object_class->set_property = mutter_window_set_property;
  object_class->get_property = mutter_window_get_property;
  object_class->constructed  = mutter_window_constructed;

  pspec = g_param_spec_object ("meta-window",
                               "MetaWindow",
                               "The displayed MetaWindow",
                               META_TYPE_WINDOW,
                               G_PARAM_READWRITE | G_PARAM_CONSTRUCT);

  g_object_class_install_property (object_class,
                                   PROP_MCW_META_WINDOW,
                                   pspec);

  pspec = g_param_spec_pointer ("meta-screen",
				"MetaScreen",
				"MetaScreen",
				G_PARAM_READWRITE | G_PARAM_CONSTRUCT);

  g_object_class_install_property (object_class,
                                   PROP_MCW_META_SCREEN,
                                   pspec);

  pspec = g_param_spec_ulong ("x-window",
			      "Window",
			      "Window",
			      0,
			      G_MAXULONG,
			      0,
			      G_PARAM_READWRITE | G_PARAM_CONSTRUCT);

  g_object_class_install_property (object_class,
                                   PROP_MCW_X_WINDOW,
                                   pspec);

  pspec = g_param_spec_boxed ("x-window-attributes",
			      "XWindowAttributes",
			      "XWindowAttributes",
			      META_TYPE_XATTRS,
			      G_PARAM_READWRITE | G_PARAM_CONSTRUCT);

  g_object_class_install_property (object_class,
                                   PROP_MCW_X_WINDOW_ATTRIBUTES,
                                   pspec);

  pspec = g_param_spec_boolean ("no-shadow",
                                "No shadow",
                                "Do not add shaddow to this window",
                                FALSE,
                                G_PARAM_READWRITE | G_PARAM_CONSTRUCT);

  g_object_class_install_property (object_class,
                                   PROP_MCW_NO_SHADOW,
                                   pspec);
}

static void
mutter_window_init (MutterWindow *self)
{
  MutterWindowPrivate *priv;

  priv = self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
						   MUTTER_TYPE_COMP_WINDOW,
						   MutterWindowPrivate);
  priv->opacity = 0xff;
}

static gboolean is_shaped (MetaDisplay *display, Window xwindow);
static gboolean mutter_window_has_shadow (MutterWindow *self);
static void update_shape (Mutter *compositor,
                          MutterWindow *cw);

static void
mutter_meta_window_decorated_notify (MetaWindow *mw,
                                     GParamSpec *arg1,
                                     gpointer    data)
{
  MutterWindow        *mcw      = MUTTER_WINDOW (data);
  MutterWindowPrivate *priv     = mcw->priv;
  MetaFrame           *frame    = meta_window_get_frame (mw);
  MetaScreen          *screen   = priv->screen;
  MetaDisplay         *display  = meta_screen_get_display (screen);
  Display             *xdisplay = meta_display_get_xdisplay (display);
  Window               new_xwindow;
  MetaCompScreen      *info;
  XWindowAttributes    attrs;

  /*
   * Basically, we have to reconstruct the the internals of this object
   * from scratch, as everything has changed.
   */
  priv->redecorating = TRUE;

  if (frame)
    new_xwindow = meta_frame_get_xwindow (frame);
  else
    new_xwindow = meta_window_get_xwindow (mw);

  mutter_window_detach (mcw);

  info = meta_screen_get_compositor_data (screen);

  /*
   * First of all, clean up any resources we are currently using and will
   * be replacing.
   */
  if (priv->damage != None)
    {
      meta_error_trap_push (display);
      XDamageDestroy (xdisplay, priv->damage);
      meta_error_trap_pop (display, FALSE);
      priv->damage = None;
    }

  g_hash_table_remove (info->windows_by_xid, (gpointer) priv->xwindow);
  g_hash_table_insert (info->windows_by_xid, (gpointer) new_xwindow, mcw);

  g_free (priv->desc);
  priv->desc = NULL;

  priv->xwindow = new_xwindow;

  if (!XGetWindowAttributes (xdisplay, new_xwindow, &attrs))
    {
      g_warning ("Could not obtain attributes for window 0x%x after "
                 "decoration change",
                 (guint) new_xwindow);
      return;
    }

  g_object_set (mcw, "x-window-attributes", &attrs, NULL);

  if (priv->shadow)
    {
      ClutterActor *p = clutter_actor_get_parent (priv->shadow);

      if (CLUTTER_IS_CONTAINER (p))
        clutter_container_remove_actor (CLUTTER_CONTAINER (p), priv->shadow);
      else
        clutter_actor_unparent (priv->shadow);

      priv->shadow = NULL;
    }

  /*
   * Recreate the contents.
   */
  mutter_window_constructed (G_OBJECT (mcw));
}

static void
mutter_window_constructed (GObject *object)
{
  MutterWindow        *self     = MUTTER_WINDOW (object);
  MutterWindowPrivate *priv     = self->priv;
  MetaScreen          *screen   = priv->screen;
  MetaDisplay         *display  = meta_screen_get_display (screen);
  Window               xwindow  = priv->xwindow;
  Display             *xdisplay = meta_display_get_xdisplay (display);
  XRenderPictFormat   *format;
  gulong               value;
  Mutter              *compositor;
  Window               xwin_child;

  compositor = (Mutter*) meta_display_get_compositor (display);
  xwin_child = meta_window_get_xwindow (priv->window);

  mutter_window_query_window_type (self);

#ifdef HAVE_SHAPE
  /* Listen for ShapeNotify events on the window */
  if (meta_display_has_shape (display))
    XShapeSelectInput (xdisplay, xwindow, ShapeNotifyMask);
#endif

  priv->shaped = is_shaped (display, xwindow);

  if (priv->attrs.class == InputOnly)
    priv->damage = None;
  else
    priv->damage = XDamageCreate (xdisplay, xwindow, XDamageReportNonEmpty);

  format = XRenderFindVisualFormat (xdisplay, priv->attrs.visual);

  if (format && format->type == PictTypeDirect && format->direct.alphaMask)
    priv->argb32 = TRUE;

  if (meta_prop_get_cardinal (display, xwin_child,
                              compositor->atom_net_wm_window_opacity,
                              &value))
    {
      guint8 opacity;

      opacity = (guint8)((gfloat)value * 255.0 / ((gfloat)0xffffffff));

      priv->opacity = opacity;
      clutter_actor_set_opacity (CLUTTER_ACTOR (self), opacity);
    }

  if (mutter_window_has_shadow (self))
    {
      priv->shadow =
	tidy_texture_frame_new (CLUTTER_TEXTURE (compositor->shadow_src),
				MAX_TILE_SZ,
				MAX_TILE_SZ,
				MAX_TILE_SZ,
				MAX_TILE_SZ);

      clutter_actor_set_position (priv->shadow,
				  SHADOW_OFFSET_X , SHADOW_OFFSET_Y);
      clutter_container_add_actor (CLUTTER_CONTAINER (self), priv->shadow);
    }

  if (!priv->actor)
    {
      priv->actor = mutter_shaped_texture_new ();

      if (!clutter_glx_texture_pixmap_using_extension (
                                  CLUTTER_GLX_TEXTURE_PIXMAP (priv->actor)))
        g_warning ("NOTE: Not using GLX TFP!\n");

      clutter_container_add_actor (CLUTTER_CONTAINER (self), priv->actor);

      g_signal_connect (priv->window, "notify::decorated",
                        G_CALLBACK (mutter_meta_window_decorated_notify), self);
    }
  else
    {
      /*
       * This is the case where existing window is gaining/loosing frame.
       * Just ensure the actor is top most (i.e., above shadow).
       */
      clutter_actor_raise_top (priv->actor);
    }


  update_shape (compositor, self);
}

static void
mutter_window_dispose (GObject *object)
{
  MutterWindow        *self = MUTTER_WINDOW (object);
  MutterWindowPrivate *priv = self->priv;
  MetaScreen            *screen;
  MetaDisplay           *display;
  Display               *xdisplay;
  MetaCompScreen        *info;

  if (priv->disposed)
    return;

  priv->disposed = TRUE;

  screen   = priv->screen;
  display  = meta_screen_get_display (screen);
  xdisplay = meta_display_get_xdisplay (display);
  info     = meta_screen_get_compositor_data (screen);

  mutter_window_detach (self);

  if (priv->damage != None)
    {
      meta_error_trap_push (display);
      XDamageDestroy (xdisplay, priv->damage);
      meta_error_trap_pop (display, FALSE);

      priv->damage = None;
    }

  /*
   * Check we are not in the dock list -- FIXME (do this in a cleaner way)
   */
  if (priv->type == META_COMP_WINDOW_DOCK)
    info->dock_windows = g_slist_remove (info->dock_windows, self);

  info->windows = g_list_remove (info->windows, (gconstpointer) self);
  g_hash_table_remove (info->windows_by_xid, (gpointer) priv->xwindow);

  g_free (priv->desc);

  G_OBJECT_CLASS (mutter_window_parent_class)->dispose (object);
}

static void
mutter_window_finalize (GObject *object)
{
  G_OBJECT_CLASS (mutter_window_parent_class)->finalize (object);
}

static void
mutter_window_set_property (GObject      *object,
                            guint         prop_id,
                            const GValue *value,
                            GParamSpec   *pspec)
{
  MutterWindow        *mw   = MUTTER_WINDOW (object);
  MutterWindowPrivate *priv = mw->priv;

  switch (prop_id)
    {
    case PROP_MCW_META_WINDOW:
      priv->window = g_value_get_object (value);
      break;
    case PROP_MCW_META_SCREEN:
      priv->screen = g_value_get_pointer (value);
      break;
    case PROP_MCW_X_WINDOW:
      priv->xwindow = g_value_get_ulong (value);
      break;
    case PROP_MCW_X_WINDOW_ATTRIBUTES:
      priv->attrs = *((XWindowAttributes*)g_value_get_boxed (value));
      break;
    case PROP_MCW_NO_SHADOW:
      {
        gboolean oldv = priv->no_shadow ? TRUE : FALSE;
        gboolean newv = g_value_get_boolean (value);

        if (oldv == newv)
          return;

        priv->no_shadow = newv;

        if (newv && priv->shadow)
          {
            clutter_container_remove_actor (CLUTTER_CONTAINER (object),
                                            priv->shadow);
            priv->shadow = NULL;
          }
        else if (!newv && !priv->shadow && mutter_window_has_shadow (mw))
          {
            guint        w, h;
            MetaDisplay *display = meta_screen_get_display (priv->screen);
            Mutter      *compositor;

            compositor = (Mutter*) meta_display_get_compositor (display);

            clutter_actor_get_size (CLUTTER_ACTOR (mw), &w, &h);

            priv->shadow =
              tidy_texture_frame_new (CLUTTER_TEXTURE (compositor->shadow_src),
                                      MAX_TILE_SZ,
                                      MAX_TILE_SZ,
                                      MAX_TILE_SZ,
                                      MAX_TILE_SZ);

            clutter_actor_set_position (priv->shadow,
                                        SHADOW_OFFSET_X , SHADOW_OFFSET_Y);

            clutter_actor_set_size (priv->shadow, w, h);

            clutter_container_add_actor (CLUTTER_CONTAINER (mw), priv->shadow);
          }
      }
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
mutter_window_get_property (GObject      *object,
			       guint         prop_id,
			       GValue       *value,
			       GParamSpec   *pspec)
{
  MutterWindowPrivate *priv = MUTTER_WINDOW (object)->priv;

  switch (prop_id)
    {
    case PROP_MCW_META_WINDOW:
      g_value_set_object (value, priv->window);
      break;
    case PROP_MCW_META_SCREEN:
      g_value_set_pointer (value, priv->screen);
      break;
    case PROP_MCW_X_WINDOW:
      g_value_set_ulong (value, priv->xwindow);
      break;
    case PROP_MCW_X_WINDOW_ATTRIBUTES:
      g_value_set_boxed (value, &priv->attrs);
      break;
    case PROP_MCW_NO_SHADOW:
      g_value_set_boolean (value, priv->no_shadow);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static MutterWindow*
find_window_for_screen (MetaScreen *screen, Window xwindow)
{
  MetaCompScreen *info = meta_screen_get_compositor_data (screen);

  if (info == NULL)
    return NULL;

  return g_hash_table_lookup (info->windows_by_xid, (gpointer) xwindow);
}

static MutterWindow *
find_window_in_display (MetaDisplay *display, Window xwindow)
{
  GSList *index;
  MetaWindow *window = meta_display_lookup_x_window (display, xwindow);

  if (window)
    {
      void *priv = meta_window_get_compositor_private (window);
      if (priv)
	return priv;
    }

  for (index = meta_display_get_screens (display);
       index;
       index = index->next)
    {
      MutterWindow *cw = find_window_for_screen (index->data, xwindow);

      if (cw != NULL)
        return cw;
    }

  return NULL;
}

static MutterWindow *
find_window_for_child_window_in_display (MetaDisplay *display, Window xwindow)
{
  Window ignored1, *ignored2, parent;
  guint  ignored_children;

  XQueryTree (meta_display_get_xdisplay (display), xwindow, &ignored1,
              &parent, &ignored2, &ignored_children);

  if (parent != None)
    return find_window_in_display (display, parent);

  return NULL;
}

static void
mutter_window_query_window_type (MutterWindow *self)
{
  MutterWindowPrivate *priv = self->priv;
  priv->type = (MetaCompWindowType) meta_window_get_window_type (priv->window);
}

static gboolean
is_shaped (MetaDisplay *display, Window xwindow)
{
  Display *xdisplay = meta_display_get_xdisplay (display);
  gint     xws, yws, xbs, ybs;
  guint    wws, hws, wbs, hbs;
  gint     bounding_shaped, clip_shaped;

  if (meta_display_has_shape (display))
    {
      XShapeQueryExtents (xdisplay, xwindow, &bounding_shaped,
                          &xws, &yws, &wws, &hws, &clip_shaped,
                          &xbs, &ybs, &wbs, &hbs);
      return (bounding_shaped != 0);
    }

  return FALSE;
}

static gboolean
mutter_window_has_shadow (MutterWindow *self)
{
  MutterWindowPrivate * priv = self->priv;

  if (priv->no_shadow)
    return FALSE;

  /*
   * Always put a shadow around windows with a frame - This should override
   * the restriction about not putting a shadow around shaped windows
   * as the frame might be the reason the window is shaped
   */
  if (priv->window)
    {
      if (meta_window_get_frame (priv->window))
	{
	  meta_verbose ("Window 0x%x has shadow because it has a frame\n",
			(guint)priv->xwindow);
	  return TRUE;
	}
    }

  /*
   * Do not add shadows to ARGB windows (since they are probably transparent)
   */
  if (priv->argb32 || priv->opacity != 0xff)
    {
      meta_verbose ("Window 0x%x has no shadow as it is ARGB\n",
		    (guint)priv->xwindow);
      return FALSE;
    }

  /*
   * Never put a shadow around shaped windows
   */
  if (priv->shaped)
    {
      meta_verbose ("Window 0x%x has no shadow as it is shaped\n",
		    (guint)priv->xwindow);
      return FALSE;
    }

  /*
   * Add shadows to override redirect windows (e.g., Gtk menus).
   * This must have lower priority than window shape test.
   */
  if (priv->attrs.override_redirect)
    {
      meta_verbose ("Window 0x%x has shadow because it is override redirect.\n",
		    (guint)priv->xwindow);
      return TRUE;
    }

  /*
   * Don't put shadow around DND icon windows
   */
  if (priv->type == META_COMP_WINDOW_DND ||
      priv->type == META_COMP_WINDOW_DESKTOP)
    {
      meta_verbose ("Window 0x%x has no shadow as it is DND or Desktop\n",
		    (guint)priv->xwindow);
      return FALSE;
    }

  if (priv->type == META_COMP_WINDOW_MENU
#if 0
      || priv->type == META_COMP_WINDOW_DROPDOWN_MENU
#endif
      )
    {
      meta_verbose ("Window 0x%x has shadow as it is a menu\n",
		    (guint)priv->xwindow);
      return TRUE;
    }

#if 0
  if (priv->type == META_COMP_WINDOW_TOOLTIP)
    {
      meta_verbose ("Window 0x%x has shadow as it is a tooltip\n",
		    (guint)priv->xwindow);
      return TRUE;
    }
#endif

  meta_verbose ("Window 0x%x has no shadow as it fell through\n",
		(guint)priv->xwindow);
  return FALSE;
}

Window
mutter_window_get_x_window (MutterWindow *mcw)
{
  if (!mcw)
    return None;

  return mcw->priv->xwindow;
}

/**
 * mutter_window_get_meta_window:
 *
 * Gets the MetaWindow object that the the MutterWindow is displaying
 *
 * Return value: (transfer none): the displayed MetaWindow
 */
MetaWindow *
mutter_window_get_meta_window (MutterWindow *mcw)
{
  return mcw->priv->window;
}

/**
 * mutter_window_get_texture:
 *
 * Gets the ClutterActor that is used to display the contents of the window
 *
 * Return value: (transfer none): the ClutterActor for the contents
 */
ClutterActor *
mutter_window_get_texture (MutterWindow *mcw)
{
  return mcw->priv->actor;
}

MetaCompWindowType
mutter_window_get_window_type (MutterWindow *mcw)
{
  if (!mcw)
    return 0;

  return mcw->priv->type;
}

gboolean
mutter_window_is_override_redirect (MutterWindow *mcw)
{
  if (mcw->priv->window->override_redirect)
    return TRUE;

  return FALSE;
}

const char *mutter_window_get_description (MutterWindow *mcw)
{
  /*
   * For windows managed by the WM, we just defer to the WM for the window
   * description. For override-redirect windows, we create the description
   * ourselves, but only on demand.
   */
  if (mcw->priv->window)
    return meta_window_get_description (mcw->priv->window);

  if (G_UNLIKELY (mcw->priv->desc == NULL))
    {
      mcw->priv->desc = g_strdup_printf ("Override Redirect (0x%x)",
                                         (guint) mcw->priv->xwindow);
    }

  return mcw->priv->desc;
}

gint
mutter_window_get_workspace (MutterWindow *mcw)
{
  MutterWindowPrivate *priv;
  MetaWorkspace       *workspace;

  if (!mcw)
    return -1;

  priv = mcw->priv;

  if (!priv->window || meta_window_is_on_all_workspaces (priv->window))
    return -1;

  workspace = meta_window_get_workspace (priv->window);

  return meta_workspace_index (workspace);
}

gboolean
mutter_window_showing_on_its_workspace (MutterWindow *mcw)
{
  if (!mcw)
    return FALSE;

  /* If override redirect: */
  if (!mcw->priv->window)
    return TRUE;

  return meta_window_showing_on_its_workspace (mcw->priv->window);
}

static void repair_win (MutterWindow *cw);
static void map_win    (MutterWindow *cw);
static void unmap_win  (MutterWindow *cw);
static void sync_actor_stacking (GList *windows);

static void
mutter_finish_workspace_switch (MetaCompScreen *info)
{
#ifdef FIXME
  GList *last = g_list_last (info->windows);
  GList *l;

/*   printf ("FINISHING DESKTOP SWITCH\n"); */

  if (!meta_prefs_get_live_hidden_windows ())
    {
      /* When running in the traditional mode where hidden windows get
       * unmapped, we need to fix up the map status for each window, since
       * we are ignoring unmap requests during the effect.
       */
      l = last;

      while (l)
	{
	  MutterWindow        *cw   = l->data;
	  MutterWindowPrivate *priv = cw->priv;

	  if (priv->needs_map && !priv->needs_unmap)
	    {
	      map_win (cw);
	    }

	  if (priv->needs_unmap)
	    {
	      unmap_win (cw);
	    }

	  l = l->prev;
	}
    }
#endif

  /*
   * Fix up stacking order in case the plugin messed it up.
   */
  sync_actor_stacking (info->windows);

/*   printf ("... FINISHED DESKTOP SWITCH\n"); */

}

static gboolean
effect_in_progress (MutterWindow *cw, gboolean include_destroy)
{
  return (cw->priv->minimize_in_progress ||
	  cw->priv->maximize_in_progress ||
	  cw->priv->unmaximize_in_progress ||
	  cw->priv->map_in_progress ||
	  (include_destroy && cw->priv->destroy_in_progress));
}

void
mutter_window_effect_completed (MutterWindow *cw, gulong event)
{
  MutterWindowPrivate *priv   = cw->priv;
  MetaScreen          *screen = priv->screen;
  MetaCompScreen      *info   = meta_screen_get_compositor_data (screen);
  ClutterActor        *actor  = CLUTTER_ACTOR (cw);
  gboolean             effect_done = FALSE;

  /* NB: Keep in mind that when effects get completed it possible
   * that the corresponding MetaWindow may have be been destroyed.
   * In this case priv->window will == NULL */

  switch (event)
  {
  case MUTTER_PLUGIN_MINIMIZE:
    {
      ClutterActor *a = CLUTTER_ACTOR (cw);

      priv->minimize_in_progress--;
      if (priv->minimize_in_progress < 0)
	{
	  g_warning ("Error in minimize accounting.");
	  priv->minimize_in_progress = 0;
	}

      if (!priv->minimize_in_progress)
	{
	  priv->is_minimized = TRUE;

	  /*
	   * We must ensure that the minimized actor is pushed down the stack
	   * (the XConfigureEvent has 'above' semantics, i.e., when a window
	   * is lowered, we get a bunch of 'raise' notifications, but might
	   * not get any notification for the window that has been lowered.
	   */
	  clutter_actor_lower_bottom (a);

	  /* Make sure that after the effect finishes, the actor is
	   * made visible for sake of live previews.
	   */
	  clutter_actor_show (a);

	  effect_done = TRUE;
	}
    }
    break;
  case MUTTER_PLUGIN_MAP:
    /*
     * Make sure that the actor is at the correct place in case
     * the plugin fscked.
     */
    priv->map_in_progress--;

    if (priv->map_in_progress < 0)
      {
	g_warning ("Error in map accounting.");
	priv->map_in_progress = 0;
      }

    if (!priv->map_in_progress && priv->window && !priv->no_more_x_calls)
      {
	MetaRectangle rect;
	meta_window_get_outer_rect (priv->window, &rect);
	priv->is_minimized = FALSE;
	clutter_actor_set_anchor_point (actor, 0, 0);
	clutter_actor_set_position (actor, rect.x, rect.y);
	clutter_actor_show_all (actor);
	effect_done = TRUE;
      }
    break;
  case MUTTER_PLUGIN_DESTROY:
    priv->destroy_in_progress--;

    if (priv->destroy_in_progress < 0)
      {
	g_warning ("Error in destroy accounting.");
	priv->destroy_in_progress = 0;
      }

    if (!priv->destroy_in_progress)
      {
	priv->needs_destroy = TRUE;
	effect_done = TRUE;
      }
    break;
  case MUTTER_PLUGIN_UNMAXIMIZE:
    priv->unmaximize_in_progress--;
    if (priv->unmaximize_in_progress < 0)
      {
	g_warning ("Error in unmaximize accounting.");
	priv->unmaximize_in_progress = 0;
      }

    if (!priv->unmaximize_in_progress && priv->window && !priv->no_more_x_calls)
      {
	MetaRectangle rect;
	meta_window_get_outer_rect (priv->window, &rect);
	clutter_actor_set_position (actor, rect.x, rect.y);
	mutter_window_detach (cw);
	repair_win (cw);
        effect_done = TRUE;
      }
    break;
  case MUTTER_PLUGIN_MAXIMIZE:
    priv->maximize_in_progress--;
    if (priv->maximize_in_progress < 0)
      {
	g_warning ("Error in maximize accounting.");
	priv->maximize_in_progress = 0;
      }

    if (!priv->maximize_in_progress && priv->window && !priv->no_more_x_calls)
      {
	MetaRectangle rect;
	meta_window_get_outer_rect (priv->window, &rect);
	clutter_actor_set_position (actor, rect.x, rect.y);
	mutter_window_detach (cw);
	repair_win (cw);
        effect_done = TRUE;
      }
    break;
  case MUTTER_PLUGIN_SWITCH_WORKSPACE:
    /* FIXME -- must redo stacking order */
    info->switch_workspace_in_progress--;
    if (info->switch_workspace_in_progress < 0)
      {
	g_warning ("Error in workspace_switch accounting!");
	info->switch_workspace_in_progress = 0;
      }

    if (!info->switch_workspace_in_progress)
      mutter_finish_workspace_switch (info);
    break;
  default:
    break;
  }

  switch (event)
  {
  case MUTTER_PLUGIN_MINIMIZE:
  case MUTTER_PLUGIN_MAP:
  case MUTTER_PLUGIN_DESTROY:
  case MUTTER_PLUGIN_UNMAXIMIZE:
  case MUTTER_PLUGIN_MAXIMIZE:

    if (effect_done &&
	priv->hide_after_effect &&
	effect_in_progress (cw, TRUE) == FALSE)
      {
	if (clutter_actor_get_parent (CLUTTER_ACTOR (cw)) != info->hidden_group)
	  {
	    clutter_actor_reparent (CLUTTER_ACTOR (cw),
				    info->hidden_group);
	  }
	priv->hide_after_effect = FALSE;
      }

    if (priv->needs_destroy && effect_in_progress (cw, TRUE) == FALSE)
      {
        ClutterActor *cwa = CLUTTER_ACTOR (cw);
        ClutterActor *parent = clutter_actor_get_parent (cwa);

        if (CLUTTER_IS_CONTAINER (parent))
          clutter_container_remove_actor (CLUTTER_CONTAINER (parent), cwa);
        else
          clutter_actor_unparent (cwa);

	return;
      }

  default:
    break;
  }
}


static void
clutter_cmp_destroy (MetaCompositor *compositor)
{
#ifdef HAVE_COMPOSITE_EXTENSIONS

#endif
}

static void
mutter_window_detach (MutterWindow *self)
{
  MutterWindowPrivate *priv     = self->priv;
  MetaScreen            *screen   = priv->screen;
  MetaDisplay           *display  = meta_screen_get_display (screen);
  Display               *xdisplay = meta_display_get_xdisplay (display);

  if (!priv->back_pixmap)
    return;

  XFreePixmap (xdisplay, priv->back_pixmap);
  priv->back_pixmap = None;
}

static void
destroy_win (MutterWindow *cw)
{
  MetaWindow	      *window;
  MetaCompScreen      *info;
  MutterWindowPrivate *priv;

  priv = cw->priv;

  window = priv->window;
  meta_window_set_compositor_private (window, NULL);

  /*
   * We remove the window from internal lookup hashes and thus any other
   * unmap events etc fail
   */
  info = meta_screen_get_compositor_data (priv->screen);
  info->windows = g_list_remove (info->windows, (gconstpointer) cw);
  g_hash_table_remove (info->windows_by_xid, (gpointer)priv->xwindow);

  if (priv->type == META_COMP_WINDOW_DROPDOWN_MENU ||
      priv->type == META_COMP_WINDOW_POPUP_MENU ||
      priv->type == META_COMP_WINDOW_TOOLTIP ||
      priv->type == META_COMP_WINDOW_NOTIFICATION ||
      priv->type == META_COMP_WINDOW_COMBO ||
      priv->type == META_COMP_WINDOW_DND ||
      priv->type == META_COMP_WINDOW_OVERRIDE_OTHER)
    {
      /*
       * No effects, just kill it.
       */
      ClutterActor *cwa = CLUTTER_ACTOR (cw);
      ClutterActor *parent = clutter_actor_get_parent (cwa);

      if (CLUTTER_IS_CONTAINER (parent))
        clutter_container_remove_actor (CLUTTER_CONTAINER (parent), cwa);
      else
        clutter_actor_unparent (cwa);

      return;
    }

  /*
   * If a plugin manager is present, try to run an effect; if no effect of this
   * type is present, destroy the actor.
   */
  priv->destroy_in_progress++;

  /*
   * Once the window destruction is initiated we can no longer perform any
   * furter X-based operations. For example, if we have a Map effect running,
   * we cannot query the window geometry once the effect completes. So, flag
   * this.
   */
  priv->no_more_x_calls = TRUE;

  if (!info->plugin_mgr ||
      !mutter_plugin_manager_event_simple (info->plugin_mgr,
					   cw,
					   MUTTER_PLUGIN_DESTROY))
    {
      priv->destroy_in_progress--;

      if (effect_in_progress (cw, FALSE))
	{
	  priv->needs_destroy = TRUE;
	}
      else
        {
          ClutterActor *cwa = CLUTTER_ACTOR (cw);
          ClutterActor *parent = clutter_actor_get_parent (cwa);

          if (CLUTTER_IS_CONTAINER (parent))
            clutter_container_remove_actor (CLUTTER_CONTAINER (parent), cwa);
          else
            clutter_actor_unparent (cwa);
        }
    }
}

static void
sync_actor_position (MutterWindow *cw)
{
  MutterWindowPrivate *priv = cw->priv;
  MetaRectangle window_rect;

  meta_window_get_outer_rect (priv->window, &window_rect);

  if (priv->attrs.width != window_rect.width ||
      priv->attrs.height != window_rect.height)
    mutter_window_detach (cw);

  /* XXX deprecated: please use meta_window_get_outer_rect instead */
  priv->attrs.width = window_rect.width;
  priv->attrs.height = window_rect.height;
  priv->attrs.x = window_rect.x;
  priv->attrs.y = window_rect.y;

  if (effect_in_progress (cw, FALSE))
    return;

  clutter_actor_set_position (CLUTTER_ACTOR (cw),
			      window_rect.x, window_rect.y);
}

static void
map_win (MutterWindow *cw)
{
  MutterWindowPrivate *priv;
  MetaCompScreen      *info;

  if (!cw)
    return;

  priv = cw->priv;
  info = meta_screen_get_compositor_data (priv->screen);

  if (priv->attrs.map_state == IsViewable)
    return;

  priv->attrs.map_state = IsViewable;

  /*
   * Now repair the window; this ensures that the actor is correctly sized
   * before we run any effects on it.
   */
  priv->needs_map = FALSE;
  mutter_window_detach (cw);
  repair_win (cw);

  /*
   * Make sure the position is set correctly (we might have got moved while
   * unmapped.
   */
  if (!info->switch_workspace_in_progress)
    {
      MetaRectangle rect;
      meta_window_get_outer_rect (priv->window, &rect);
      clutter_actor_set_anchor_point (CLUTTER_ACTOR (cw), 0, 0);
      clutter_actor_set_position (CLUTTER_ACTOR (cw), rect.x, rect.y);
    }

  priv->map_in_progress++;

  /*
   * If a plugin manager is present, try to run an effect; if no effect of this
   * type is present, destroy the actor.
   */
  if (priv->redecorating ||
      info->switch_workspace_in_progress || !info->plugin_mgr ||
      !mutter_plugin_manager_event_simple (info->plugin_mgr,
				cw,
                                MUTTER_PLUGIN_MAP))
    {
      clutter_actor_show_all (CLUTTER_ACTOR (cw));
      priv->map_in_progress--;
      priv->is_minimized = FALSE;
      priv->redecorating = FALSE;
    }
}

static void
unmap_win (MutterWindow *cw)
{
  MutterWindowPrivate *priv;
  MetaCompScreen      *info;

  if (!cw)
    return;

  priv = cw->priv;
  info = meta_screen_get_compositor_data (priv->screen);

  /*
   * If the needs_unmap flag is set, we carry on even if the winow is
   * already marked as unmapped; this is necessary so windows temporarily
   * shown during an effect (like desktop switch) are properly hidden again.
   */
  if (priv->attrs.map_state == IsUnmapped && !priv->needs_unmap)
    return;

  if (priv->window && priv->window == info->focus_window)
    info->focus_window = NULL;

  if (info->switch_workspace_in_progress)
    {
      /*
       * Cannot unmap windows while switching desktops effect is in progress.
       */
      priv->needs_unmap = TRUE;
      return;
    }

  priv->attrs.map_state = IsUnmapped;
  priv->needs_unmap = FALSE;
  priv->needs_map   = FALSE;

  if (!priv->minimize_in_progress &&
      (!meta_prefs_get_live_hidden_windows () ||
       priv->type == META_COMP_WINDOW_DROPDOWN_MENU ||
       priv->type == META_COMP_WINDOW_POPUP_MENU ||
       priv->type == META_COMP_WINDOW_TOOLTIP ||
       priv->type == META_COMP_WINDOW_NOTIFICATION ||
       priv->type == META_COMP_WINDOW_COMBO ||
       priv->type == META_COMP_WINDOW_DND ||
       priv->type == META_COMP_WINDOW_OVERRIDE_OTHER))
    {
      clutter_actor_hide (CLUTTER_ACTOR (cw));
    }
}

static void
add_win (MetaWindow *window)
{
  MetaScreen		*screen = meta_window_get_screen (window);
  MetaDisplay           *display = meta_screen_get_display (screen);
  MetaCompScreen        *info = meta_screen_get_compositor_data (screen);
  MutterWindow          *cw;
  MutterWindowPrivate   *priv;
  MetaFrame		*frame;
  Window		 top_window;
  XWindowAttributes	 attrs;

  g_return_if_fail (info != NULL);

  frame = meta_window_get_frame (window);
  if (frame)
    top_window = meta_frame_get_xwindow (frame);
  else
    top_window = meta_window_get_xwindow (window);

  meta_verbose ("add window: Meta %p, xwin 0x%x\n", window, (guint)top_window);

  /* FIXME: Remove the redundant data we store in cw->priv->attrs, and
   * simply query metacity core for the data. */
  if (!XGetWindowAttributes (display->xdisplay, top_window, &attrs))
    return;

  cw = g_object_new (MUTTER_TYPE_COMP_WINDOW,
		     "meta-window",         window,
		     "x-window",            top_window,
		     "meta-screen",         screen,
		     "x-window-attributes", &attrs,
		     NULL);

  priv = cw->priv;

  clutter_actor_set_position (CLUTTER_ACTOR (cw),
			      priv->attrs.x, priv->attrs.y);

  clutter_container_add_actor (CLUTTER_CONTAINER (info->window_group),
			       CLUTTER_ACTOR (cw));
  clutter_actor_hide (CLUTTER_ACTOR (cw));

  if (priv->type == META_COMP_WINDOW_DOCK)
    {
      meta_verbose ("Appending 0x%x to dock windows\n", (guint)top_window);
      info->dock_windows = g_slist_append (info->dock_windows, cw);
    }

  meta_verbose ("added 0x%x (%p) type:", (guint)top_window, cw);

  /* Hang our compositor window state off the MetaWindow for fast retrieval */
  meta_window_set_compositor_private (window, G_OBJECT (cw));

  /*
   * Add this to the list at the top of the stack before it is mapped so that
   * map_win can find it again
   */
  info->windows = g_list_append (info->windows, cw);
  g_hash_table_insert (info->windows_by_xid, (gpointer) top_window, cw);

  if (priv->attrs.map_state == IsViewable)
    {
      /* Need to reset the map_state for map_win() to work */
      priv->attrs.map_state = IsUnmapped;
      map_win (cw);
    }

  sync_actor_stacking (info->windows);
}

static void
repair_win (MutterWindow *cw)
{
  MutterWindowPrivate *priv     = cw->priv;
  MetaScreen          *screen   = priv->screen;
  MetaDisplay         *display  = meta_screen_get_display (screen);
  Display             *xdisplay = meta_display_get_xdisplay (display);
  MetaCompScreen      *info     = meta_screen_get_compositor_data (screen);
  Mutter              *compositor;
  Window               xwindow  = priv->xwindow;
  gboolean             full     = FALSE;

  if (xwindow == meta_screen_get_xroot (screen) ||
      xwindow == clutter_x11_get_stage_window (CLUTTER_STAGE (info->stage)))
    return;

  compositor = (Mutter*)meta_display_get_compositor (display);

  meta_error_trap_push (display);

  if (priv->back_pixmap == None)
    {
      gint pxm_width, pxm_height;

      meta_error_trap_push (display);

      priv->back_pixmap = XCompositeNameWindowPixmap (xdisplay, xwindow);

      if (meta_error_trap_pop_with_return (display, FALSE) != Success)
        {
          /* Probably a BadMatch if the window isn't viewable; we could
           * GrabServer/GetWindowAttributes/NameWindowPixmap/UngrabServer/Sync
           * to avoid this, but there's no reason to take two round trips
           * when one will do. (We need that Sync if we want to handle failures
           * for any reason other than !viewable. That's unlikely, but maybe
           * we'll BadAlloc or something.)
           */
          priv->back_pixmap = None;
        }

      if (priv->back_pixmap == None)
        {
          meta_verbose ("Unable to get named pixmap for %p\n", cw);
          return;
        }

      /* MUST call before setting pixmap or serious performance issues
       * seemingly caused by cogl_texture_set_filters() in set_filter
       * Not sure if that call is actually needed.
       */
      if (!compositor->no_mipmaps)
        clutter_texture_set_filter_quality (CLUTTER_TEXTURE (priv->actor),
                                            CLUTTER_TEXTURE_QUALITY_HIGH );

      clutter_x11_texture_pixmap_set_pixmap
                       (CLUTTER_X11_TEXTURE_PIXMAP (priv->actor),
                        priv->back_pixmap);

      g_object_get (priv->actor,
                    "pixmap-width", &pxm_width,
                    "pixmap-height", &pxm_height,
                    NULL);

      clutter_actor_set_size (priv->actor, pxm_width, pxm_height);

      if (priv->shadow)
        clutter_actor_set_size (priv->shadow, pxm_width, pxm_height);

      full = TRUE;
    }

 /*
   * TODO -- on some gfx hardware updating the whole texture instead of
   * the individual rectangles is actually quicker, so we might want to
   * make this a configurable option (on desktop HW with multiple pipelines
   * it is usually quicker to just update the damaged parts).
   *
   * If we are using TFP we update the whole texture (this simply trigers
   * the texture rebind).
   */
  if (full
#ifdef HAVE_GLX_TEXTURE_PIXMAP
      || (CLUTTER_GLX_IS_TEXTURE_PIXMAP (priv->actor) &&
          clutter_glx_texture_pixmap_using_extension
                  (CLUTTER_GLX_TEXTURE_PIXMAP (priv->actor)))
#endif /* HAVE_GLX_TEXTURE_PIXMAP */
      )
    {
      XDamageSubtract (xdisplay, priv->damage, None, None);

      clutter_x11_texture_pixmap_update_area
	(CLUTTER_X11_TEXTURE_PIXMAP (priv->actor),
	 0,
	 0,
	 clutter_actor_get_width (priv->actor),
	 clutter_actor_get_height (priv->actor));
    }
  else
    {
      XRectangle   *r_damage;
      XRectangle    r_bounds;
      XserverRegion parts;
      int           i, r_count;

      parts = XFixesCreateRegion (xdisplay, 0, 0);
      XDamageSubtract (xdisplay, priv->damage, None, parts);

      r_damage = XFixesFetchRegionAndBounds (xdisplay,
					     parts,
					     &r_count,
					     &r_bounds);

      if (r_damage)
	{
	  for (i = 0; i < r_count; ++i)
	    {
	      clutter_x11_texture_pixmap_update_area
		(CLUTTER_X11_TEXTURE_PIXMAP (priv->actor),
		 r_damage[i].x,
		 r_damage[i].y,
		 r_damage[i].width,
		 r_damage[i].height);
	    }
	}

      XFree (r_damage);
      XFixesDestroyRegion (xdisplay, parts);
    }

  meta_error_trap_pop (display, FALSE);

  priv->needs_repair = FALSE;
}

static void
process_damage (Mutter *compositor,
                XDamageNotifyEvent    *event)
{
  XEvent   next;
  Display *dpy = event->display;
  Drawable drawable = event->drawable;
  MutterWindowPrivate *priv;
  MutterWindow *cw = find_window_in_display (compositor->display, drawable);

  if (!cw)
    return;

  priv = cw->priv;

  if (priv->destroy_pending        ||
      priv->maximize_in_progress   ||
      priv->unmaximize_in_progress)
    {
      priv->needs_repair = TRUE;
      return;
    }

  /*
   * Check if the event queue does not already contain DetstroyNotify for this
   * window -- if it does, we need to stop updating the pixmap (to avoid damage
   * notifications that come from the window teardown), and process the destroy
   * immediately.
   */
  if (XCheckTypedWindowEvent (dpy, drawable, DestroyNotify, &next))
    {
      priv->destroy_pending = TRUE;
      destroy_win (cw);
      return;
    }

  repair_win (cw);
}

static void
update_shape (Mutter *compositor,
              MutterWindow *cw)
{
  MutterWindowPrivate *priv = cw->priv;

  mutter_shaped_texture_clear_rectangles (MUTTER_SHAPED_TEXTURE (priv->actor));

#ifdef HAVE_SHAPE
  if (priv->shaped)
    {
      Display *xdisplay = meta_display_get_xdisplay (compositor->display);
      XRectangle *rects;
      int n_rects, ordering;

      rects = XShapeGetRectangles (xdisplay,
                                   priv->xwindow,
                                   ShapeBounding,
                                   &n_rects,
                                   &ordering);

      if (rects)
        {
          mutter_shaped_texture_add_rectangles (MUTTER_SHAPED_TEXTURE (priv->actor),
                                              n_rects, rects);

          XFree (rects);
        }
    }
#endif
}

#ifdef HAVE_SHAPE
static void
process_shape (Mutter	    *compositor,
               XShapeEvent  *event)
{
  MutterWindow *cw = find_window_in_display (compositor->display,
                                             event->window);
  MutterWindowPrivate *priv;

  if (cw == NULL)
    return;

  priv = cw->priv;

  if (event->kind == ShapeBounding)
    {
      priv->shaped = event->shaped;
      update_shape (compositor, cw);
    }
}
#endif

static void
process_property_notify (Mutter		*compositor,
                         XPropertyEvent *event)
{
  MetaDisplay *display = compositor->display;

  /* Check for the opacity changing */
  if (event->atom == compositor->atom_net_wm_window_opacity)
    {
      MutterWindow *cw = find_window_in_display (display, event->window);
      gulong        value;

      if (!cw)
        {
          /* Applications can set this for their toplevel windows, so
           * this must be propagated to the window managed by the compositor
           */
          cw = find_window_for_child_window_in_display (display,
                                                        event->window);
        }

      if (!cw)
	{
	  DEBUG_TRACE ("process_property_notify: opacity, early exit\n");
	  return;
	}

      if (meta_prop_get_cardinal (display, event->window,
                                  compositor->atom_net_wm_window_opacity,
                                  &value))
	{
	  guint8 opacity;

	  opacity = (guint8)((gfloat)value * 255.0 / ((gfloat)0xffffffff));

	  cw->priv->opacity = opacity;
	  clutter_actor_set_opacity (CLUTTER_ACTOR (cw), opacity);
	}

      return;
    }
  else if (event->atom == meta_display_get_atom (display,
					       META_ATOM__NET_WM_WINDOW_TYPE))
    {
      MutterWindow *cw = find_window_in_display (display, event->window);

      if (!cw)
	{
	  DEBUG_TRACE ("process_property_notify: net_wm_type, early exit\n");
	  return;
	}

      mutter_window_query_window_type (cw);
      DEBUG_TRACE ("process_property_notify: net_wm_type\n");
      return;
    }
  DEBUG_TRACE ("process_property_notify: unknown\n");
}

static void
show_overlay_window (Display *xdisplay, Window xstage, Window xoverlay)
{
  XserverRegion  region;

  region = XFixesCreateRegion (xdisplay, NULL, 0);

  XFixesSetWindowShapeRegion (xdisplay, xoverlay, ShapeBounding, 0, 0, 0);

  XFixesSetWindowShapeRegion (xdisplay, xoverlay, ShapeInput, 0, 0, region);
  XFixesSetWindowShapeRegion (xdisplay, xstage,   ShapeInput, 0, 0, region);

  XFixesDestroyRegion (xdisplay, region);
}

static Window
get_output_window (MetaScreen *screen)
{
  MetaDisplay *display = meta_screen_get_display (screen);
  Display     *xdisplay = meta_display_get_xdisplay (display);
  Window       output, xroot;
  XWindowAttributes attr;
  long         event_mask;

  xroot = meta_screen_get_xroot (screen);

  event_mask = FocusChangeMask |
               ExposureMask |
	       PointerMotionMask |
               PropertyChangeMask |
               ButtonPressMask | ButtonReleaseMask |
               KeyPressMask | KeyReleaseMask;

  output = XCompositeGetOverlayWindow (xdisplay, xroot);

  if (XGetWindowAttributes (xdisplay, output, &attr))
      {
        event_mask |= attr.your_event_mask;
      }

  XSelectInput (xdisplay, output, event_mask);

  return output;
}

ClutterActor *
mutter_get_stage_for_screen (MetaScreen *screen)
{
  MetaCompScreen *info = meta_screen_get_compositor_data (screen);

  if (!info)
    return NULL;

  return info->stage;
}

ClutterActor *
mutter_get_overlay_group_for_screen (MetaScreen *screen)
{
  MetaCompScreen *info = meta_screen_get_compositor_data (screen);

  if (!info)
    return NULL;

  return info->overlay_group;
}

ClutterActor *
mutter_get_window_group_for_screen (MetaScreen *screen)
{
  MetaCompScreen *info = meta_screen_get_compositor_data (screen);

  if (!info)
    return NULL;

  return info->window_group;
}

GList *
mutter_get_windows (MetaScreen *screen)
{
  MetaCompScreen *info = meta_screen_get_compositor_data (screen);

  if (!info)
    return NULL;

  return info->windows;
}

void
mutter_set_stage_input_region (MetaScreen *screen,
                               XserverRegion region)
{
  MetaCompScreen *info = meta_screen_get_compositor_data (screen);
  MetaDisplay  *display = meta_screen_get_display (screen);
  Display      *xdpy    = meta_display_get_xdisplay (display);

  if (info->stage && info->output)
    {
      Window xstage = clutter_x11_get_stage_window (CLUTTER_STAGE (info->stage));

      XFixesSetWindowShapeRegion (xdpy, xstage, ShapeInput, 0, 0, region);
      XFixesSetWindowShapeRegion (xdpy, info->output, ShapeInput, 0, 0, region);
    }
  else if (region != None)
    {
      info->pending_input_region = XFixesCreateRegion (xdpy, NULL, 0);
      XFixesCopyRegion (xdpy, info->pending_input_region, region);
    }
}

void
mutter_empty_stage_input_region (MetaScreen *screen)
{
  /* Using a static region here is a bit hacky, but Metacity never opens more than
   * one XDisplay, so it works fine. */
  static XserverRegion region = None;

  if (region == None)
    {
      MetaDisplay  *display = meta_screen_get_display (screen);
      Display      *xdpy    = meta_display_get_xdisplay (display);
      region = XFixesCreateRegion (xdpy, NULL, 0);
    }

  mutter_set_stage_input_region (screen, region);
}

static void
clutter_cmp_manage_screen (MetaCompositor *compositor,
                           MetaScreen     *screen)
{
#ifdef HAVE_COMPOSITE_EXTENSIONS
  MetaCompScreen *info;
  MetaDisplay    *display       = meta_screen_get_display (screen);
  Display        *xdisplay      = meta_display_get_xdisplay (display);
  int             screen_number = meta_screen_get_screen_number (screen);
  Window          xroot         = meta_screen_get_xroot (screen);
  Window          xwin;
  gint            width, height;
  XWindowAttributes attr;
  long            event_mask;

  /* Check if the screen is already managed */
  if (meta_screen_get_compositor_data (screen))
    return;

  meta_error_trap_push_with_return (display);
  XCompositeRedirectSubwindows (xdisplay, xroot, CompositeRedirectManual);
  XSync (xdisplay, FALSE);

  if (meta_error_trap_pop_with_return (display, FALSE))
    {
      g_warning ("Another compositing manager is running on screen %i",
                 screen_number);
      return;
    }

  info = g_new0 (MetaCompScreen, 1);
  info->screen = screen;

  meta_screen_set_compositor_data (screen, info);

  info->output = None;
  info->windows = NULL;
  info->windows_by_xid = g_hash_table_new (g_direct_hash, g_direct_equal);

  info->focus_window = meta_display_get_focus_window (display);

  meta_screen_set_cm_selection (screen);

  info->stage = clutter_stage_get_default ();

  meta_screen_get_size (screen, &width, &height);
  clutter_actor_set_size (info->stage, width, height);

  xwin = clutter_x11_get_stage_window (CLUTTER_STAGE (info->stage));

  event_mask = FocusChangeMask |
               ExposureMask |
               PointerMotionMask |
               PropertyChangeMask |
               ButtonPressMask | ButtonReleaseMask |
               KeyPressMask | KeyReleaseMask |
               StructureNotifyMask;

  if (XGetWindowAttributes (xdisplay, xwin, &attr))
      {
        event_mask |= attr.your_event_mask;
      }

  XSelectInput (xdisplay, xwin, event_mask);

  info->window_group = clutter_group_new ();
  info->overlay_group = clutter_group_new ();
  info->hidden_group = clutter_group_new ();

  clutter_container_add (CLUTTER_CONTAINER (info->stage),
                         info->window_group,
                         info->overlay_group,
			 info->hidden_group,
                         NULL);

  clutter_actor_hide (info->hidden_group);

  info->plugin_mgr =
    mutter_plugin_manager_new (screen);
  if (!mutter_plugin_manager_load (info->plugin_mgr))
    g_critical ("failed to load plugins");
  if (!mutter_plugin_manager_initialize (info->plugin_mgr))
    g_critical ("failed to initialize plugins");

  /*
   * Delay the creation of the overlay window as long as we can, to avoid
   * blanking out the screen. This means that during the plugin loading, the
   * overlay window is not accessible; if the plugin needs to access it
   * directly, it should hook into the "show" signal on stage, and do
   * its stuff there.
   */
  info->output = get_output_window (screen);
  XReparentWindow (xdisplay, xwin, info->output, 0, 0);

  show_overlay_window (xdisplay, xwin, info->output);

  if (info->pending_input_region != None)
    {
      mutter_set_stage_input_region (screen, info->pending_input_region);
      XFixesDestroyRegion (xdisplay, info->pending_input_region);
      info->pending_input_region = None;
    }

  clutter_actor_show (info->overlay_group);
  clutter_actor_show (info->stage);
#endif
}

static void
clutter_cmp_unmanage_screen (MetaCompositor *compositor,
                             MetaScreen     *screen)
{
#ifdef HAVE_COMPOSITE_EXTENSIONS

#endif
}

static void
clutter_cmp_add_window (MetaCompositor    *compositor,
                        MetaWindow        *window)
{
#ifdef HAVE_COMPOSITE_EXTENSIONS
  MetaScreen *screen = meta_window_get_screen (window);
  MetaDisplay *display = meta_screen_get_display (screen);

  DEBUG_TRACE ("clutter_cmp_add_window\n");
  meta_error_trap_push (display);

  add_win (window);

  meta_error_trap_pop (display, FALSE);
#endif
}

static void
clutter_cmp_remove_window (MetaCompositor *compositor,
                           MetaWindow     *window)
{
#ifdef HAVE_COMPOSITE_EXTENSIONS
  MutterWindow         *cw     = NULL;

  DEBUG_TRACE ("clutter_cmp_remove_window\n");
  cw = MUTTER_WINDOW (meta_window_get_compositor_private (window));
  if (!cw)
    return;

  destroy_win (cw);
#endif
}

static void
clutter_cmp_set_updates (MetaCompositor *compositor,
                         MetaWindow     *window,
                         gboolean        update)
{
#ifdef HAVE_COMPOSITE_EXTENSIONS

#endif
}

static gboolean
clutter_cmp_process_event (MetaCompositor *compositor,
                           XEvent         *event,
                           MetaWindow     *window)
{
#ifdef HAVE_COMPOSITE_EXTENSIONS
  Mutter *xrc = (Mutter *) compositor;

  if (window)
    {
      MetaCompScreen *info;
      MetaScreen     *screen;

      screen = meta_window_get_screen (window);
      info = meta_screen_get_compositor_data (screen);

      if (mutter_plugin_manager_xevent_filter (info->plugin_mgr, event))
	{
	  DEBUG_TRACE ("clutter_cmp_process_event (filtered,window==NULL)\n");
	  return TRUE;
	}
    }
  else
    {
      GSList *l;
      Mutter *clc = (Mutter*)compositor;

      l = meta_display_get_screens (clc->display);

      while (l)
	{
	  MetaScreen     *screen = l->data;
	  MetaCompScreen *info;

	  info = meta_screen_get_compositor_data (screen);

	  if (mutter_plugin_manager_xevent_filter (info->plugin_mgr, event))
	    {
	      DEBUG_TRACE ("clutter_cmp_process_event (filtered,window==NULL)\n");
	      return TRUE;
	    }

	  l = l->next;
	}
    }

  /*
   * This trap is so that none of the compositor functions cause
   * X errors. This is really a hack, but I'm afraid I don't understand
   * enough about Metacity/X to know how else you are supposed to do it
   */


  meta_error_trap_push (xrc->display);
  switch (event->type)
    {
    case PropertyNotify:
      process_property_notify (xrc, (XPropertyEvent *) event);
      break;

    default:
      if (event->type == meta_display_get_damage_event_base (xrc->display) + XDamageNotify)
        {
	  DEBUG_TRACE ("clutter_cmp_process_event (process_damage)\n");
          process_damage (xrc, (XDamageNotifyEvent *) event);
        }
#ifdef HAVE_SHAPE
      else if (event->type == meta_display_get_shape_event_base (xrc->display) + ShapeNotify)
	{
	  DEBUG_TRACE ("clutter_cmp_process_event (process_shape)\n");
	  process_shape (xrc, (XShapeEvent *) event);
	}
#endif /* HAVE_SHAPE */
      break;
    }

  meta_error_trap_pop (xrc->display, FALSE);

  /* Clutter needs to know about MapNotify events otherwise it will
     think the stage is invisible */
  if (event->type == MapNotify)
    clutter_x11_handle_event (event);

  /* The above handling is basically just "observing" the events, so we return
   * FALSE to indicate that the event should not be filtered out; if we have
   * GTK+ windows in the same process, GTK+ needs the ConfigureNotify event, for example.
   */
  return FALSE;
#endif
}

static Pixmap
clutter_cmp_get_window_pixmap (MetaCompositor *compositor,
                               MetaWindow     *window)
{
#ifdef HAVE_COMPOSITE_EXTENSIONS
  return None;
#else
  return None;
#endif
}

static void
clutter_cmp_set_active_window (MetaCompositor *compositor,
                               MetaScreen     *screen,
                               MetaWindow     *window)
{
#ifdef HAVE_COMPOSITE_EXTENSIONS

#endif
}

static void
clutter_cmp_map_window (MetaCompositor *compositor, MetaWindow *window)
{
#ifdef HAVE_COMPOSITE_EXTENSIONS
  MutterWindow *cw = MUTTER_WINDOW (meta_window_get_compositor_private (window));
  DEBUG_TRACE ("clutter_cmp_map_window\n");
  if (!cw)
    return;

  map_win (cw);
#endif
}

static void
clutter_cmp_unmap_window (MetaCompositor *compositor, MetaWindow *window)
{
#ifdef HAVE_COMPOSITE_EXTENSIONS
  MutterWindow *cw = MUTTER_WINDOW (meta_window_get_compositor_private (window));
  DEBUG_TRACE ("clutter_cmp_unmap_window\n");
  if (!cw)
    return;

  unmap_win (cw);
#endif
}

static void
clutter_cmp_minimize_window (MetaCompositor *compositor,
			     MetaWindow	    *window,
			     MetaRectangle  *window_rect,
			     MetaRectangle  *icon_rect)
{
#ifdef HAVE_COMPOSITE_EXTENSIONS
  MutterWindow	 *cw = MUTTER_WINDOW (meta_window_get_compositor_private (window));
  MetaScreen	 *screen = meta_window_get_screen (window);
  MetaCompScreen *info = meta_screen_get_compositor_data (screen);

  DEBUG_TRACE ("clutter_cmp_minimize_window\n");

  g_return_if_fail (info);

  if (!cw)
    return;

  /*
   * If there is a plugin manager, try to run an effect; if no effect is
   * executed, hide the actor.
   */
  cw->priv->minimize_in_progress++;

  if (!info->plugin_mgr ||
      !mutter_plugin_manager_event_simple (info->plugin_mgr,
					   cw,
					   MUTTER_PLUGIN_MINIMIZE))
    {
      cw->priv->is_minimized = TRUE;
      cw->priv->minimize_in_progress--;
    }
#endif
}

static void
clutter_cmp_unminimize_window (MetaCompositor *compositor,
			       MetaWindow     *window,
			       MetaRectangle  *window_rect,
			       MetaRectangle  *icon_rect)
{
#ifdef HAVE_COMPOSITE_EXTENSIONS
#if 0
  MutterWindow	 *cw = MUTTER_WINDOW (meta_window_get_compositor_private (window));
  MetaScreen	 *screen = meta_window_get_screen (window);
  MetaCompScreen *info = meta_screen_get_compositor_data (screen);

  g_return_if_fail (info);

  if (!cw)
    return;

  /*
   * If there is a plugin manager, try to run an effect; if no effect is
   * executed, hide the actor.
   */
  cw->priv->unminimize_in_progress++;

  if (!info->plugin_mgr ||
      !mutter_plugin_manager_event_simple (info->plugin_mgr,
					   cw,
					   MUTTER_PLUGIN_UNMINIMIZE))
    {
      cw->priv->is_minimized = TRUE;
      cw->priv->minimize_in_progress--;
    }
#else
  MutterWindow	 *cw = MUTTER_WINDOW (meta_window_get_compositor_private (window));
  DEBUG_TRACE ("clutter_cmp_unminimize_window\n");
  if (!cw)
    return;

  map_win (cw);
#endif
#endif
}


static void
clutter_cmp_maximize_window (MetaCompositor *compositor,
			     MetaWindow	    *window,
			     MetaRectangle  *rect)
{
#ifdef HAVE_COMPOSITE_EXTENSIONS
  MutterWindow	 *cw = MUTTER_WINDOW (meta_window_get_compositor_private (window));
  MetaScreen	 *screen = meta_window_get_screen (window);
  MetaCompScreen *info = meta_screen_get_compositor_data (screen);

  DEBUG_TRACE ("clutter_cmp_maximize_window\n");
  g_return_if_fail (info);

  if (!cw)
    return;

  cw->priv->maximize_in_progress++;

  if (!info->plugin_mgr ||
      !mutter_plugin_manager_event_maximize (info->plugin_mgr,
					     cw,
					     MUTTER_PLUGIN_MAXIMIZE,
					     rect->x, rect->y,
					     rect->width, rect->height))
    {
      cw->priv->maximize_in_progress--;
    }
#endif
}

static void
clutter_cmp_unmaximize_window (MetaCompositor *compositor,
			       MetaWindow     *window,
			       MetaRectangle  *rect)
{
#ifdef HAVE_COMPOSITE_EXTENSIONS
  MutterWindow	 *cw = MUTTER_WINDOW (meta_window_get_compositor_private (window));
  MetaScreen	 *screen = meta_window_get_screen (window);
  MetaCompScreen *info = meta_screen_get_compositor_data (screen);

  g_return_if_fail (info);

  DEBUG_TRACE ("clutter_cmp_unmaximize_window\n");
  if (!cw)
    return;

  cw->priv->unmaximize_in_progress++;

  if (!info->plugin_mgr ||
      !mutter_plugin_manager_event_maximize (info->plugin_mgr,
					     cw,
					     MUTTER_PLUGIN_UNMAXIMIZE,
					     rect->x, rect->y,
					     rect->width, rect->height))
    {
      cw->priv->unmaximize_in_progress--;
    }
#endif
}

static void
clutter_cmp_update_workspace_geometry (MetaCompositor *compositor,
				       MetaWorkspace  *workspace)
{
#ifdef HAVE_COMPOSITE_EXTENSIONS
#if 0
  /* FIXME -- should do away with this function in favour of MetaWorkspace
   * signal.
   */
  MetaScreen     *screen = meta_workspace_get_screen (workspace);
  MetaCompScreen *info;
  MutterPluginManager *mgr;

  DEBUG_TRACE ("clutter_cmp_update_workspace_geometry\n");
  info = meta_screen_get_compositor_data (screen);
  mgr  = info->plugin_mgr;

  if (!mgr || !workspace)
    return;

  mutter_plugin_manager_update_workspace (mgr, workspace);
#endif
#endif
}

static void
clutter_cmp_switch_workspace (MetaCompositor *compositor,
			      MetaScreen     *screen,
			      MetaWorkspace  *from,
			      MetaWorkspace  *to,
			      MetaMotionDirection direction)
{
#ifdef HAVE_COMPOSITE_EXTENSIONS
  MetaCompScreen *info;
  gint            to_indx, from_indx;

  info      = meta_screen_get_compositor_data (screen);
  to_indx   = meta_workspace_index (to);
  from_indx = meta_workspace_index (from);

  DEBUG_TRACE ("clutter_cmp_switch_workspace\n");
  if (!meta_prefs_get_live_hidden_windows ())
    {
      GList *l;

      /*
       * We are in the traditional mode where hidden windows get unmapped,
       * we need to pre-calculate the map status of each window so that once
       * the effect finishes we can put everything into proper order
       * (we need to ignore the map notifications during the effect so that
       * actors do not just disappear while the effect is running).
       */
      for (l = info->windows; l != NULL; l = l->next)
	{
	  MutterWindow *cw = l->data;
	  MetaWindow   *mw = cw->priv->window;
	  gboolean      sticky;
	  gint          workspace = -1;

	  sticky = (!mw || meta_window_is_on_all_workspaces (mw));

	  if (!sticky)
	    {
	      MetaWorkspace *w;

	      w = meta_window_get_workspace (cw->priv->window);
	      workspace = meta_workspace_index (w);

	      /*
	       * If the window is not on the target workspace, mark it for
	       * unmap.
	       */
	      if (to_indx != workspace)
		{
		  cw->priv->needs_unmap = TRUE;
		}
	      else
		{
		  cw->priv->needs_map = TRUE;
		  cw->priv->needs_unmap = FALSE;
		}
	    }
	}
    }

  info->switch_workspace_in_progress++;

  if (!info->plugin_mgr ||
      !mutter_plugin_manager_switch_workspace (info->plugin_mgr,
					       (const GList **)&info->windows,
					       from_indx,
					       to_indx,
					       direction))
    {
      info->switch_workspace_in_progress--;

      /* We have to explicitely call this to fix up stacking order of the
       * actors; this is because the abs stacking position of actors does not
       * necessarily change during the window hiding/unhiding, only their
       * relative position toward the destkop window.
       */
      mutter_finish_workspace_switch (info);
    }
#endif
}

static void
sync_actor_stacking (GList *windows)
{
  GList *tmp;

  /* NB: The first entry in the list is stacked the lowest */

  for (tmp = g_list_last (windows); tmp != NULL; tmp = tmp->prev)
    {
      MutterWindow *cw = tmp->data;

      clutter_actor_lower_bottom (CLUTTER_ACTOR (cw));
    }
}

static void
clutter_cmp_sync_stack (MetaCompositor *compositor,
			MetaScreen     *screen,
			GList	       *stack)
{
  GList *tmp;
  MetaCompScreen *info = meta_screen_get_compositor_data (screen);

  DEBUG_TRACE ("clutter_cmp_sync_stack\n");
  /* NB: The first entry in stack, is stacked the highest */

  for (tmp = stack; tmp != NULL; tmp = tmp->next)
    {
      MetaWindow    *window = tmp->data;
      MutterWindow  *cw = MUTTER_WINDOW (meta_window_get_compositor_private (window));

      if (!cw)
	{
	  meta_verbose ("Failed to find corresponding MutterWindow "
			"for window %p\n", window);
	  continue;
	}

      info->windows = g_list_remove (info->windows, (gconstpointer)cw);
      info->windows = g_list_prepend (info->windows, cw);
    }

  sync_actor_stacking (info->windows);
}

static void
clutter_cmp_set_window_hidden (MetaCompositor *compositor,
			       MetaScreen     *screen,
			       MetaWindow     *window,
			       gboolean	       hidden)
{
  MutterWindow *cw = MUTTER_WINDOW (meta_window_get_compositor_private (window));
  MutterWindowPrivate *priv = cw->priv;
  MetaCompScreen *info = meta_screen_get_compositor_data (screen);

  DEBUG_TRACE ("clutter_cmp_set_window_hidden\n");
  if (!cw)
    return;

  if (hidden)
    {
      if (effect_in_progress (cw, TRUE))
	{
	  priv->hide_after_effect = TRUE;
	}
      else
	{
	  if (clutter_actor_get_parent (CLUTTER_ACTOR (cw)) != info->hidden_group)
	    {
	      clutter_actor_reparent (CLUTTER_ACTOR (cw),
				      info->hidden_group);
	    }
	}
    }
  else
    {
      priv->hide_after_effect = FALSE;
      if (clutter_actor_get_parent (CLUTTER_ACTOR (cw)) != info->window_group)
	clutter_actor_reparent (CLUTTER_ACTOR (cw),
				info->window_group);
    }
}

static void
clutter_cmp_sync_window_geometry (MetaCompositor *compositor,
				  MetaWindow *window)
{
#ifdef HAVE_COMPOSITE_EXTENSIONS
  MutterWindow	 *cw = MUTTER_WINDOW (meta_window_get_compositor_private (window));
  MetaScreen	 *screen = meta_window_get_screen (window);
  MetaCompScreen *info = meta_screen_get_compositor_data (screen);

  DEBUG_TRACE ("clutter_cmp_sync_window_geometry\n");
  g_return_if_fail (info);

  if (!cw)
    return;

  sync_actor_position (cw);

#endif
}

static void
clutter_cmp_sync_screen_size (MetaCompositor *compositor,
			      MetaScreen     *screen,
			      guint	      width,
			      guint	      height)
{
#ifdef HAVE_COMPOSITE_EXTENSIONS
  MetaCompScreen *info = meta_screen_get_compositor_data (screen);

  DEBUG_TRACE ("clutter_cmp_sync_screen_size\n");
  g_return_if_fail (info);

  clutter_actor_set_size (info->stage, width, height);

  meta_verbose ("Changed size for stage on screen %d to %dx%d\n",
		meta_screen_get_screen_number (screen),
		width, height);
#endif
}

static MetaCompositor comp_info = {
  clutter_cmp_destroy,
  clutter_cmp_manage_screen,
  clutter_cmp_unmanage_screen,
  clutter_cmp_add_window,
  clutter_cmp_remove_window,
  clutter_cmp_set_updates,
  clutter_cmp_process_event,
  clutter_cmp_get_window_pixmap,
  clutter_cmp_set_active_window,
  clutter_cmp_map_window,
  clutter_cmp_unmap_window,
  clutter_cmp_minimize_window,
  clutter_cmp_unminimize_window,
  clutter_cmp_maximize_window,
  clutter_cmp_unmaximize_window,
  clutter_cmp_update_workspace_geometry,
  clutter_cmp_switch_workspace,
  clutter_cmp_sync_stack,
  clutter_cmp_set_window_hidden,
  clutter_cmp_sync_window_geometry,
  clutter_cmp_sync_screen_size
};

MetaCompositor *
mutter_new (MetaDisplay *display)
{
#ifdef HAVE_COMPOSITE_EXTENSIONS
  char *atom_names[] = {
    "_XROOTPMAP_ID",
    "_XSETROOT_ID",
    "_NET_WM_WINDOW_OPACITY",
  };
  Atom                   atoms[G_N_ELEMENTS(atom_names)];
  Mutter *clc;
  MetaCompositor        *compositor;
  Display               *xdisplay = meta_display_get_xdisplay (display);
  guchar                *data;

  if (!composite_at_least_version (display, 0, 3))
    return NULL;

  clc = g_new0 (Mutter, 1);
  clc->compositor = comp_info;

  compositor = (MetaCompositor *) clc;

  clc->display = display;

  if (g_getenv("MUTTER_DISABLE_MIPMAPS"))
    clc->no_mipmaps = TRUE;

  meta_verbose ("Creating %d atoms\n", (int) G_N_ELEMENTS (atom_names));
  XInternAtoms (xdisplay, atom_names, G_N_ELEMENTS (atom_names),
                False, atoms);

  clc->atom_x_root_pixmap = atoms[0];
  clc->atom_x_set_root = atoms[1];
  clc->atom_net_wm_window_opacity = atoms[2];

  /* Shadow setup */

  data = shadow_gaussian_make_tile ();

  clc->shadow_src = clutter_texture_new ();

  clutter_texture_set_from_rgb_data (CLUTTER_TEXTURE (clc->shadow_src),
                                     data,
                                     TRUE,
                                     TILE_WIDTH,
                                     TILE_HEIGHT,
                                     TILE_WIDTH*4,
                                     4,
                                     0,
                                     NULL);
  free (data);

  return compositor;
#else
  return NULL;
#endif
}

Window
mutter_get_overlay_window (MetaScreen *screen)
{
  MetaCompScreen *info = meta_screen_get_compositor_data (screen);

  return info->output;
}


/* ------------------------------- */
/* Shadow Generation */

typedef struct GaussianMap
{
  int	   size;
  double * data;
} GaussianMap;

static double
gaussian (double r, double x, double y)
{
  return ((1 / (sqrt (2 * M_PI * r))) *
	  exp ((- (x * x + y * y)) / (2 * r * r)));
}


static GaussianMap *
make_gaussian_map (double r)
{
  GaussianMap  *c;
  int	          size = ((int) ceil ((r * 3)) + 1) & ~1;
  int	          center = size / 2;
  int	          x, y;
  double          t = 0.0;
  double          g;

  c = malloc (sizeof (GaussianMap) + size * size * sizeof (double));
  c->size = size;

  c->data = (double *) (c + 1);

  for (y = 0; y < size; y++)
    for (x = 0; x < size; x++)
      {
	g = gaussian (r, (double) (x - center), (double) (y - center));
	t += g;
	c->data[y * size + x] = g;
      }

  for (y = 0; y < size; y++)
    for (x = 0; x < size; x++)
      c->data[y*size + x] /= t;

  return c;
}

static unsigned char
sum_gaussian (GaussianMap * map, double opacity,
              int x, int y, int width, int height)
{
  int	           fx, fy;
  double         * g_data;
  double         * g_line = map->data;
  int	           g_size = map->size;
  int	           center = g_size / 2;
  int	           fx_start, fx_end;
  int	           fy_start, fy_end;
  double           v;
  unsigned int     r;

  /*
   * Compute set of filter values which are "in range",
   * that's the set with:
   *	0 <= x + (fx-center) && x + (fx-center) < width &&
   *  0 <= y + (fy-center) && y + (fy-center) < height
   *
   *  0 <= x + (fx - center)	x + fx - center < width
   *  center - x <= fx	fx < width + center - x
   */

  fx_start = center - x;
  if (fx_start < 0)
    fx_start = 0;
  fx_end = width + center - x;
  if (fx_end > g_size)
    fx_end = g_size;

  fy_start = center - y;
  if (fy_start < 0)
    fy_start = 0;
  fy_end = height + center - y;
  if (fy_end > g_size)
    fy_end = g_size;

  g_line = g_line + fy_start * g_size + fx_start;

  v = 0;
  for (fy = fy_start; fy < fy_end; fy++)
    {
      g_data = g_line;
      g_line += g_size;

      for (fx = fx_start; fx < fx_end; fx++)
	v += *g_data++;
    }
  if (v > 1)
    v = 1;

  v *= (opacity * 255.0);

  r = (unsigned int) v;

  return (unsigned char) r;
}

static unsigned char *
shadow_gaussian_make_tile ()
{
  unsigned char              * data;
  int		               size;
  int		               center;
  int		               x, y;
  unsigned char                d;
  int                          pwidth, pheight;
  double                       opacity = SHADOW_OPACITY;
  static GaussianMap       * gaussian_map = NULL;

  struct _mypixel
  {
    unsigned char r;
    unsigned char g;
    unsigned char b;
    unsigned char a;
  } * _d;


  if (!gaussian_map)
    gaussian_map =
      make_gaussian_map (SHADOW_RADIUS);

  size   = gaussian_map->size;
  center = size / 2;

  /* Top & bottom */

  pwidth  = MAX_TILE_SZ;
  pheight = MAX_TILE_SZ;

  data = g_malloc0 (4 * TILE_WIDTH * TILE_HEIGHT);

  _d = (struct _mypixel*) data;

  /* N */
  for (y = 0; y < pheight; y++)
    {
      d = sum_gaussian (gaussian_map, opacity,
                        center, y - center,
                        TILE_WIDTH, TILE_HEIGHT);
      for (x = 0; x < pwidth; x++)
	{
	  _d[y*3*pwidth + x + pwidth].r = 0;
	  _d[y*3*pwidth + x + pwidth].g = 0;
	  _d[y*3*pwidth + x + pwidth].b = 0;
	  _d[y*3*pwidth + x + pwidth].a = d;
	}

    }

  /* S */
  pwidth = MAX_TILE_SZ;
  pheight = MAX_TILE_SZ;

  for (y = 0; y < pheight; y++)
    {
      d = sum_gaussian (gaussian_map, opacity,
                        center, y - center,
                        TILE_WIDTH, TILE_HEIGHT);
      for (x = 0; x < pwidth; x++)
	{
	  _d[(pheight-y-1)*3*pwidth + 6*pwidth*pheight + x + pwidth].r = 0;
	  _d[(pheight-y-1)*3*pwidth + 6*pwidth*pheight + x + pwidth].g = 0;
	  _d[(pheight-y-1)*3*pwidth + 6*pwidth*pheight + x + pwidth].b = 0;
	  _d[(pheight-y-1)*3*pwidth + 6*pwidth*pheight + x + pwidth].a = d;
	}

    }


  /* w */
  pwidth = MAX_TILE_SZ;
  pheight = MAX_TILE_SZ;

  for (x = 0; x < pwidth; x++)
    {
      d = sum_gaussian (gaussian_map, opacity,
                        x - center, center,
                        TILE_WIDTH, TILE_HEIGHT);
      for (y = 0; y < pheight; y++)
	{
	  _d[y*3*pwidth + 3*pwidth*pheight + x].r = 0;
	  _d[y*3*pwidth + 3*pwidth*pheight + x].g = 0;
	  _d[y*3*pwidth + 3*pwidth*pheight + x].b = 0;
	  _d[y*3*pwidth + 3*pwidth*pheight + x].a = d;
	}

    }

  /* E */
  for (x = 0; x < pwidth; x++)
    {
      d = sum_gaussian (gaussian_map, opacity,
					       x - center, center,
					       TILE_WIDTH, TILE_HEIGHT);
      for (y = 0; y < pheight; y++)
	{
	  _d[y*3*pwidth + 3*pwidth*pheight + (pwidth-x-1) + 2*pwidth].r = 0;
	  _d[y*3*pwidth + 3*pwidth*pheight + (pwidth-x-1) + 2*pwidth].g = 0;
	  _d[y*3*pwidth + 3*pwidth*pheight + (pwidth-x-1) + 2*pwidth].b = 0;
	  _d[y*3*pwidth + 3*pwidth*pheight + (pwidth-x-1) + 2*pwidth].a = d;
	}

    }

  /* NW */
  pwidth = MAX_TILE_SZ;
  pheight = MAX_TILE_SZ;

  for (x = 0; x < pwidth; x++)
    for (y = 0; y < pheight; y++)
      {
	d = sum_gaussian (gaussian_map, opacity,
                          x-center, y-center,
                          TILE_WIDTH, TILE_HEIGHT);

	_d[y*3*pwidth + x].r = 0;
	_d[y*3*pwidth + x].g = 0;
	_d[y*3*pwidth + x].b = 0;
	_d[y*3*pwidth + x].a = d;
      }

  /* SW */
  for (x = 0; x < pwidth; x++)
    for (y = 0; y < pheight; y++)
      {
	d = sum_gaussian (gaussian_map, opacity,
                          x-center, y-center,
                          TILE_WIDTH, TILE_HEIGHT);

	_d[(pheight-y-1)*3*pwidth + 6*pwidth*pheight + x].r = 0;
	_d[(pheight-y-1)*3*pwidth + 6*pwidth*pheight + x].g = 0;
	_d[(pheight-y-1)*3*pwidth + 6*pwidth*pheight + x].b = 0;
	_d[(pheight-y-1)*3*pwidth + 6*pwidth*pheight + x].a = d;
      }

  /* SE */
  for (x = 0; x < pwidth; x++)
    for (y = 0; y < pheight; y++)
      {
	d = sum_gaussian (gaussian_map, opacity,
                          x-center, y-center,
                          TILE_WIDTH, TILE_HEIGHT);

	_d[(pheight-y-1)*3*pwidth + 6*pwidth*pheight + (pwidth-x-1) +
	   2*pwidth].r = 0;
	_d[(pheight-y-1)*3*pwidth + 6*pwidth*pheight + (pwidth-x-1) +
	   2*pwidth].g = 0;
	_d[(pheight-y-1)*3*pwidth + 6*pwidth*pheight + (pwidth-x-1) +
	   2*pwidth].b = 0;
	_d[(pheight-y-1)*3*pwidth + 6*pwidth*pheight + (pwidth-x-1) +
	   2*pwidth].a = d;
      }

  /* NE */
  for (x = 0; x < pwidth; x++)
    for (y = 0; y < pheight; y++)
      {
	d = sum_gaussian (gaussian_map, opacity,
                          x-center, y-center,
                          TILE_WIDTH, TILE_HEIGHT);

	_d[y*3*pwidth + (pwidth - x - 1) + 2*pwidth].r = 0;
	_d[y*3*pwidth + (pwidth - x - 1) + 2*pwidth].g = 0;
	_d[y*3*pwidth + (pwidth - x - 1) + 2*pwidth].b = 0;
	_d[y*3*pwidth + (pwidth - x - 1) + 2*pwidth].a = d;
      }

  /* center */
  pwidth = MAX_TILE_SZ;
  pheight = MAX_TILE_SZ;

  d = sum_gaussian (gaussian_map, opacity,
                    center, center, TILE_WIDTH, TILE_HEIGHT);

  for (x = 0; x < pwidth; x++)
    for (y = 0; y < pheight; y++)
      {
	_d[y*3*pwidth + 3*pwidth*pheight + x + pwidth].r = 0;
	_d[y*3*pwidth + 3*pwidth*pheight + x + pwidth].g = 0;
	_d[y*3*pwidth + 3*pwidth*pheight + x + pwidth].b = 0;
	_d[y*3*pwidth + 3*pwidth*pheight + x + pwidth].a = 0;
      }

  return data;
}


