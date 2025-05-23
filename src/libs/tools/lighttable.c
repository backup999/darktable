/*
    This file is part of darktable,
    Copyright (C) 2011-2025 darktable developers.

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

#include <gdk/gdkkeysyms.h>

#include "common/collection.h"
#include "common/debug.h"
#include "common/selection.h"
#include "control/conf.h"
#include "control/control.h"
#include "dtgtk/thumbtable.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

typedef struct dt_lib_tool_lighttable_t
{
  GtkWidget *zoom;
  GtkWidget *layout_box;
  GtkWidget *layout_filemanager;
  GtkWidget *layout_zoomable;
  GtkWidget *layout_culling_dynamic;
  GtkWidget *layout_culling_fix;
  GtkWidget *layout_culling_restricted;
  GtkWidget *layout_preview;
  dt_lighttable_layout_t layout, base_layout;
  int current_zoom;
  gboolean fullpreview_focus;
  dt_lighttable_culling_restriction_t culling_init_restriction;
} dt_lib_tool_lighttable_t;

/* set zoom proxy function */
static void _lib_lighttable_set_zoom(dt_lib_module_t *self, gint zoom);
static gint _lib_lighttable_get_zoom(dt_lib_module_t *self);

/* get/set layout proxy function */
static dt_lighttable_layout_t _lib_lighttable_get_layout(dt_lib_module_t *self);

/* zoom slider change callback */
static void _lib_lighttable_zoom_slider_changed(GtkWidget *widget, dt_lib_module_t *self);

static void _set_zoom(dt_lib_module_t *self, int zoom);

const char *name(dt_lib_module_t *self)
{
  return _("lighttable");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_LIGHTTABLE;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_CENTER;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position(const dt_lib_module_t *self)
{
  return 1001;
}

static void _lib_lighttable_update_btn(dt_lib_module_t *self)
{
  dt_lib_tool_lighttable_t *d = self->data;

  gboolean fullpreview = dt_view_lighttable_preview_state(darktable.view_manager);

  // which btn should be active ?
  GtkWidget *active = d->layout_filemanager;
  if(fullpreview)
    active = d->layout_preview;
  else if(d->layout == DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC)
    active = d->layout_culling_dynamic;
  else if(d->layout == DT_LIGHTTABLE_LAYOUT_CULLING)
    active = d->layout_culling_fix;
  else if(d->layout == DT_LIGHTTABLE_LAYOUT_ZOOMABLE)
    active = d->layout_zoomable;

  GList *children = gtk_container_get_children(GTK_CONTAINER(d->layout_box));
  for(GList *l = children; l; l = g_list_delete_link(l, l))
  {
    GtkWidget *w = l->data;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), (w == active));
    gtk_widget_queue_draw(w); // force redraw even if state not changed
  }

  // and now we set the tooltips
  if(fullpreview)
    gtk_widget_set_tooltip_text(d->layout_preview, _("click to exit from full preview layout."));
  else
    gtk_widget_set_tooltip_text(d->layout_preview, _("click to enter full preview layout."));

  if(d->layout != DT_LIGHTTABLE_LAYOUT_CULLING || fullpreview)
    gtk_widget_set_tooltip_text(d->layout_culling_fix, _("click to enter culling layout in fixed mode."));
  else
    gtk_widget_set_tooltip_text(d->layout_culling_fix, _("click to exit culling layout."));

  if(d->layout != DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC || fullpreview)
    gtk_widget_set_tooltip_text(d->layout_culling_dynamic, _("click to enter culling layout in dynamic mode."));
  else
    gtk_widget_set_tooltip_text(d->layout_culling_dynamic, _("click to exit culling layout."));

  gtk_widget_set_sensitive(d->zoom, (d->layout != DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC && !fullpreview));
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(d->zoom), d->current_zoom);

  // culling restricted button configuration
  if(d->layout == DT_LIGHTTABLE_LAYOUT_CULLING || fullpreview)
  {
    if(dt_view_lighttable_culling_restricted_state(darktable.view_manager) == DT_LIGHTTABLE_CULLING_RESTRICTION_SELECTION)
    {
      gtk_widget_set_tooltip_text(d->layout_culling_restricted, _("click to allow browsing all images from the collection."));
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->layout_culling_restricted), TRUE);
    }
    else
    {
      gtk_widget_set_tooltip_text(d->layout_culling_restricted, _("click to limit browsing to the selection."));
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->layout_culling_restricted), FALSE);
    }

    gtk_widget_set_visible(d->layout_culling_restricted, TRUE);
  }
  else
  {
    gtk_widget_set_visible(d->layout_culling_restricted, FALSE);
    // limit the filckering on next show : it's less visible to do inactive->active
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->layout_culling_restricted), FALSE);
  }
}

static void _lib_lighttable_set_layout(dt_lib_module_t *self,
                                       const dt_lighttable_layout_t layout)
{
  dt_lib_tool_lighttable_t *d = self->data;

  // we deal with fullpreview first.
  if((layout == DT_LIGHTTABLE_LAYOUT_PREVIEW) ^ dt_view_lighttable_preview_state(darktable.view_manager))
    dt_view_lighttable_set_preview_state(darktable.view_manager,
                                         layout == DT_LIGHTTABLE_LAYOUT_PREVIEW,
                                         TRUE,
                                         d->fullpreview_focus,
                                         DT_LIGHTTABLE_CULLING_RESTRICTION_AUTO);

  if(layout == DT_LIGHTTABLE_LAYOUT_PREVIEW)
  {
     // special case for preview : we don't change previous values,
     // just show full preview and update buttons
    _lib_lighttable_update_btn(self);
    return;
  }

  const int current_layout = dt_conf_get_int("plugins/lighttable/layout");
  d->layout = layout;

  if(current_layout != layout)
  {
    if(d->layout == DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC)
    {
      d->current_zoom = MAX(1, MIN(30, dt_collection_get_selected_count()));
      if(d->current_zoom == 1)
        d->current_zoom = dt_conf_get_int("plugins/lighttable/culling_num_images");
    }
    else if(d->layout == DT_LIGHTTABLE_LAYOUT_CULLING)
    {
      d->current_zoom = dt_conf_get_int("plugins/lighttable/culling_num_images");
    }
    else
    {
      d->current_zoom = dt_conf_get_int("plugins/lighttable/images_in_row");
    }

    dt_conf_set_int("plugins/lighttable/layout", layout);
    if(layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER || layout == DT_LIGHTTABLE_LAYOUT_ZOOMABLE)
    {
      d->base_layout = layout;
      dt_conf_set_int("plugins/lighttable/base_layout", layout);
    }

    dt_control_queue_redraw_center();
  }
  else
  {
    dt_control_queue_redraw_center();
  }

  _lib_lighttable_update_btn(self);
}

static gboolean _lib_lighttable_layout_btn_release(GtkWidget *w, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_lib_tool_lighttable_t *d = self->data;

  const gboolean active
      = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w)); // note : this is the state before the change
  dt_lighttable_layout_t new_layout = DT_LIGHTTABLE_LAYOUT_FILEMANAGER;
  if(!active)
  {
    // that means we want to activate the button
    if(w == d->layout_preview)
    {
      d->fullpreview_focus = dt_modifier_is(event->state, GDK_CONTROL_MASK);
      new_layout = DT_LIGHTTABLE_LAYOUT_PREVIEW;
    }
    else if(w == d->layout_culling_fix)
    {
      if(dt_modifier_is(event->state, GDK_CONTROL_MASK))
        d->culling_init_restriction = DT_LIGHTTABLE_CULLING_RESTRICTION_COLLECTION;
      else
        d->culling_init_restriction = DT_LIGHTTABLE_CULLING_RESTRICTION_AUTO;
      new_layout = DT_LIGHTTABLE_LAYOUT_CULLING;
    }
    else if(w == d->layout_culling_dynamic)
      new_layout = DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC;
    else if(w == d->layout_zoomable)
      new_layout = DT_LIGHTTABLE_LAYOUT_ZOOMABLE;
  }
  else
  {
    // that means we want to deactivate the button
    if(w == d->layout_preview)
      new_layout = d->layout;
    else if(w == d->layout_culling_dynamic || w == d->layout_culling_fix)
      new_layout = d->base_layout;
    else
    {
      // we can't exit from filemanager or zoomable
      return TRUE;
    }
  }

  _lib_lighttable_set_layout(self, new_layout);
  return TRUE;
}

static gboolean _lib_lighttable_restricted_btn_release(GtkWidget *w, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_lighttable_culling_restriction_t restriction = DT_LIGHTTABLE_CULLING_RESTRICTION_SELECTION;
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w)))
    restriction = DT_LIGHTTABLE_CULLING_RESTRICTION_COLLECTION; // note : this is the state before the change

  dt_view_lighttable_set_culling_restricted_state(darktable.view_manager, restriction);
  _lib_lighttable_update_btn(self);
  return TRUE;
}

static void _lib_lighttable_key_accel_toggle_filemanager(dt_action_t *action)
{
  dt_lib_module_t *self = darktable.view_manager->proxy.lighttable.module;
  _lib_lighttable_set_layout(self, DT_LIGHTTABLE_LAYOUT_FILEMANAGER);
}

static void _lib_lighttable_key_accel_toggle_zoomable(dt_action_t *action)
{
  dt_lib_module_t *self = darktable.view_manager->proxy.lighttable.module;
  _lib_lighttable_set_layout(self, DT_LIGHTTABLE_LAYOUT_ZOOMABLE);
}

static void _lib_lighttable_key_accel_toggle_culling_dynamic_mode(dt_action_t *action)
{
  dt_lib_module_t *self = darktable.view_manager->proxy.lighttable.module;
  dt_lib_tool_lighttable_t *d = self->data;

  // if we are already in any culling layout, we return to the base layout
  if(d->layout != DT_LIGHTTABLE_LAYOUT_CULLING && d->layout != DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC)
  {
    _lib_lighttable_set_layout(self, DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC);
  }
  else
    _lib_lighttable_set_layout(self, d->base_layout);

  dt_control_queue_redraw_center();
}

static void _lib_lighttable_key_accel_toggle_culling_zoom_mode(dt_action_t *action)
{
  dt_lib_module_t *self = darktable.view_manager->proxy.lighttable.module;
  dt_lib_tool_lighttable_t *d = self->data;

  if(d->layout == DT_LIGHTTABLE_LAYOUT_CULLING)
    _lib_lighttable_set_layout(self, DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC);
  else if(d->layout == DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC)
  {
    d->culling_init_restriction = DT_LIGHTTABLE_CULLING_RESTRICTION_AUTO;
    _lib_lighttable_set_layout(self, DT_LIGHTTABLE_LAYOUT_CULLING);
  }
}

static void _lib_lighttable_key_accel_toggle_restricted_mode(dt_action_t *action)
{
  dt_lib_module_t *self = darktable.view_manager->proxy.lighttable.module;
  dt_lib_tool_lighttable_t *d = self->data;

  if(d->layout == DT_LIGHTTABLE_LAYOUT_CULLING || dt_view_lighttable_preview_state(darktable.view_manager))
  {
    // if we are already in culling layout or fullpreview, we switch between restricted and unrestricted
    _lib_lighttable_restricted_btn_release(d->layout_culling_restricted, NULL, self);
  }
}

static void _lib_lighttable_key_accel_exit_layout(dt_action_t *action)
{
  dt_lib_module_t *self = darktable.view_manager->proxy.lighttable.module;
  dt_lib_tool_lighttable_t *d = self->data;

  if(dt_view_lighttable_preview_state(darktable.view_manager))
    _lib_lighttable_set_layout(self, d->layout);
  else if(d->layout != d->base_layout)
    _lib_lighttable_set_layout(self, d->base_layout);
}

static dt_lighttable_culling_restriction_t _lib_lighttable_get_culling_initial_restriction(dt_lib_module_t *self)
{
  dt_lib_tool_lighttable_t *d = self->data;
  return d ? d->culling_init_restriction : DT_LIGHTTABLE_CULLING_RESTRICTION_AUTO;
}

enum
{
  DT_ACTION_ELEMENT_PREVIEW_FOCUS_DETECT = 1,
  DT_ACTION_ELEMENT_PREVIEW_NO_RESTRICTION = 2,
};
enum
{
  DT_ACTION_ELEMENT_CULLING_NO_RESTRICTION = 1,
};

static float _action_process_culling(gpointer target, dt_action_element_t element, dt_action_effect_t effect, float move_size)
{
  dt_lib_module_t *self = darktable.view_manager->proxy.lighttable.module;
  dt_lib_tool_lighttable_t *d = self->data;

  if(DT_PERFORM_ACTION(move_size))
  {
    if(d->layout != DT_LIGHTTABLE_LAYOUT_CULLING
       && d->layout != DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC
       && effect != DT_ACTION_EFFECT_ON)
    {
      // if we are not in culling layout, we enter this mode
      if(element == DT_ACTION_ELEMENT_CULLING_NO_RESTRICTION)
        d->culling_init_restriction = DT_LIGHTTABLE_CULLING_RESTRICTION_COLLECTION;
      else
        d->culling_init_restriction = DT_LIGHTTABLE_CULLING_RESTRICTION_AUTO;
      _lib_lighttable_set_layout(self, DT_LIGHTTABLE_LAYOUT_CULLING);
    }
    else if(effect != DT_ACTION_EFFECT_ON)
    {
      // if we are already in culling layout we fallback to the base layout
      _lib_lighttable_set_layout(self, d->base_layout);
    }

    _lib_lighttable_update_btn(self);
  }

  return (d->layout == DT_LIGHTTABLE_LAYOUT_CULLING);
}

static float _action_process_preview(gpointer target, dt_action_element_t element, dt_action_effect_t effect, float move_size)
{
  dt_lib_module_t *self = darktable.view_manager->proxy.lighttable.module;
  dt_lib_tool_lighttable_t *d = self->data;

  if(DT_PERFORM_ACTION(move_size))
  {
    if(dt_view_lighttable_preview_state(darktable.view_manager))
    {
      if(effect != DT_ACTION_EFFECT_ON)
        _lib_lighttable_set_layout(self, d->layout);
    }
    else
    {
      if(effect != DT_ACTION_EFFECT_OFF)
      {
        const gboolean sticky = effect == DT_ACTION_EFFECT_HOLD_TOGGLE;
        const gboolean focus = element == DT_ACTION_ELEMENT_PREVIEW_FOCUS_DETECT;
        dt_lighttable_culling_restriction_t restriction = DT_LIGHTTABLE_CULLING_RESTRICTION_AUTO;
        if(sticky && element == DT_ACTION_ELEMENT_PREVIEW_NO_RESTRICTION)
          restriction = DT_LIGHTTABLE_CULLING_RESTRICTION_COLLECTION;
        dt_view_lighttable_set_preview_state(darktable.view_manager, TRUE, sticky, focus, restriction);
      }
    }

    _lib_lighttable_update_btn(self);
  }

  return dt_view_lighttable_preview_state(darktable.view_manager);
}

const dt_action_element_def_t _action_elements_preview[]
  = { { N_("normal"), dt_action_effect_hold },
      { N_("focus detection"), dt_action_effect_hold },
      { N_("no restriction"), dt_action_effect_hold },
      { NULL } };

const dt_action_def_t _action_def_preview
  = { N_("preview"),
      _action_process_preview,
      _action_elements_preview,
      NULL };

const dt_action_element_def_t _action_elements_culling[]
  = { { N_("normal"), dt_action_effect_hold },
      { N_("no restriction"), dt_action_effect_hold },
      { NULL } };

const dt_action_def_t _action_def_culling
  = { N_("culling"),
      _action_process_culling,
      _action_elements_culling,
      NULL };

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_tool_lighttable_t *d = g_malloc0(sizeof(dt_lib_tool_lighttable_t));
  self->data = (void *)d;

  d->layout = MIN(DT_LIGHTTABLE_LAYOUT_LAST - 1, dt_conf_get_int("plugins/lighttable/layout"));
  d->base_layout = MIN(DT_LIGHTTABLE_LAYOUT_LAST - 1, dt_conf_get_int("plugins/lighttable/base_layout"));

  if(d->layout == DT_LIGHTTABLE_LAYOUT_CULLING)
    d->current_zoom = dt_conf_get_int("plugins/lighttable/culling_num_images");
  else if(d->layout == DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC)
  {
    d->current_zoom = MAX(1, MIN(DT_LIGHTTABLE_MAX_ZOOM, dt_collection_get_selected_count()));
    if(d->current_zoom == 1)
      d->current_zoom = dt_conf_get_int("plugins/lighttable/culling_num_images");
  }
  else
    d->current_zoom = dt_conf_get_int("plugins/lighttable/images_in_row");

  // create the layouts icon list
  dt_action_t *ltv = &darktable.view_manager->proxy.lighttable.view->actions;
  dt_action_t *ac = NULL;

  d->layout_filemanager = dtgtk_togglebutton_new(dtgtk_cairo_paint_lt_mode_grid, 0, NULL);
  ac = dt_action_define(ltv, NULL, N_("toggle filemanager layout"), d->layout_filemanager, NULL);
  dt_action_register(ac, NULL, _lib_lighttable_key_accel_toggle_filemanager, 0, 0);
  dt_gui_add_help_link(d->layout_filemanager, "layout_filemanager");
  gtk_widget_set_tooltip_text(d->layout_filemanager, _("click to enter filemanager layout."));
  g_signal_connect(G_OBJECT(d->layout_filemanager), "button-release-event",
                   G_CALLBACK(_lib_lighttable_layout_btn_release), self);

  d->layout_zoomable = dtgtk_togglebutton_new(dtgtk_cairo_paint_lt_mode_zoom, 0, NULL);
  ac = dt_action_define(ltv, NULL, N_("toggle zoomable lighttable layout"), d->layout_zoomable, NULL);
  dt_action_register(ac, NULL, _lib_lighttable_key_accel_toggle_zoomable, 0, 0);
  dt_gui_add_help_link(d->layout_zoomable, "layout_zoomable");
  gtk_widget_set_tooltip_text(d->layout_zoomable, _("click to enter zoomable lighttable layout."));
  g_signal_connect(G_OBJECT(d->layout_zoomable), "button-release-event",
                   G_CALLBACK(_lib_lighttable_layout_btn_release), self);

  d->layout_culling_fix = dtgtk_togglebutton_new(dtgtk_cairo_paint_lt_mode_culling_fixed, 0, NULL);
  ac = dt_action_define(ltv, NULL, N_("toggle culling mode"), d->layout_culling_fix, &_action_def_culling);
  dt_shortcut_register(ac, DT_ACTION_ELEMENT_DEFAULT, DT_ACTION_EFFECT_HOLD_TOGGLE, GDK_KEY_x, 0);
  dt_shortcut_register(ac, DT_ACTION_ELEMENT_CULLING_NO_RESTRICTION, DT_ACTION_EFFECT_HOLD_TOGGLE, GDK_KEY_x, GDK_SHIFT_MASK);
  dt_gui_add_help_link(d->layout_culling_fix, "layout_culling");
  g_signal_connect(G_OBJECT(d->layout_culling_fix), "button-release-event",
                   G_CALLBACK(_lib_lighttable_layout_btn_release), self);

  d->layout_culling_dynamic = dtgtk_togglebutton_new(dtgtk_cairo_paint_lt_mode_culling_dynamic, 0, NULL);
  ac = dt_action_define(ltv, NULL, N_("toggle culling dynamic mode"), d->layout_culling_dynamic, NULL);
  dt_action_register(ac, NULL, _lib_lighttable_key_accel_toggle_culling_dynamic_mode, GDK_KEY_x, GDK_CONTROL_MASK);
  dt_gui_add_help_link(d->layout_culling_dynamic, "layout_culling");
  g_signal_connect(G_OBJECT(d->layout_culling_dynamic), "button-release-event",
                   G_CALLBACK(_lib_lighttable_layout_btn_release), self);

  d->layout_preview = dtgtk_togglebutton_new(dtgtk_cairo_paint_lt_mode_fullpreview, 0, NULL);
  ac = dt_action_define(ltv, NULL, N_("preview"), d->layout_preview, &_action_def_preview);
  dt_shortcut_register(ac, DT_ACTION_ELEMENT_DEFAULT, DT_ACTION_EFFECT_HOLD_TOGGLE, GDK_KEY_f, 0);
  dt_shortcut_register(ac, DT_ACTION_ELEMENT_PREVIEW_NO_RESTRICTION, DT_ACTION_EFFECT_HOLD_TOGGLE, GDK_KEY_f, GDK_SHIFT_MASK);
  dt_shortcut_register(ac, DT_ACTION_ELEMENT_DEFAULT, DT_ACTION_EFFECT_HOLD, GDK_KEY_w, 0);
  dt_shortcut_register(ac, DT_ACTION_ELEMENT_PREVIEW_FOCUS_DETECT, DT_ACTION_EFFECT_HOLD, GDK_KEY_w, GDK_CONTROL_MASK);
  dt_gui_add_help_link(d->layout_preview, "layout_preview");
  g_signal_connect(G_OBJECT(d->layout_preview), "button-release-event",
                   G_CALLBACK(_lib_lighttable_layout_btn_release), self);

  d->layout_box = dt_gui_hbox(d->layout_filemanager, d->layout_zoomable,
                              d->layout_culling_fix, d->layout_culling_dynamic,
                              d->layout_preview);
  gtk_widget_set_name(d->layout_box, "lighttable-layouts-box");

  /* create horizontal zoom slider */
  d->zoom = gtk_spin_button_new_with_range(1, DT_LIGHTTABLE_MAX_ZOOM, 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(d->zoom), d->current_zoom);
  gtk_widget_set_margin_start(d->zoom, 24);
  gtk_widget_set_tooltip_text(d->zoom,
                              _("set the number of thumbnails per row in filemanager layout,\n"
                                "or the total number of thumbnails shown in culling layouts."));

  /* culling restricted icon */
  d->layout_culling_restricted = dtgtk_togglebutton_new(dtgtk_cairo_paint_lock, 0, NULL);
  ac = dt_action_define(ltv, NULL, N_("toggle culling restricted"), d->layout_culling_restricted, NULL);
  dt_action_register(ac, NULL, _lib_lighttable_key_accel_toggle_restricted_mode, GDK_KEY_r, GDK_CONTROL_MASK);
  dt_gui_add_help_link(d->layout_culling_restricted, "layout_culling");
  gtk_widget_set_no_show_all(d->layout_culling_restricted, TRUE);
  g_signal_connect(G_OBJECT(d->layout_culling_restricted), "button-release-event",
                   G_CALLBACK(_lib_lighttable_restricted_btn_release), self);

  self->widget = dt_gui_hbox(d->layout_box, d->zoom, d->layout_culling_restricted);

  _lib_lighttable_update_btn(self);

  g_signal_connect(G_OBJECT(d->zoom), "value-changed", G_CALLBACK(_lib_lighttable_zoom_slider_changed), self);

  darktable.view_manager->proxy.lighttable.module = self;
  darktable.view_manager->proxy.lighttable.set_zoom = _lib_lighttable_set_zoom;
  darktable.view_manager->proxy.lighttable.get_zoom = _lib_lighttable_get_zoom;
  darktable.view_manager->proxy.lighttable.get_layout = _lib_lighttable_get_layout;
  darktable.view_manager->proxy.lighttable.set_layout = _lib_lighttable_set_layout;
  darktable.view_manager->proxy.lighttable.update_layout_btn = _lib_lighttable_update_btn;
  darktable.view_manager->proxy.lighttable.get_culling_initial_restriction = _lib_lighttable_get_culling_initial_restriction;

  dt_action_register(ltv, N_("toggle culling zoom mode"), _lib_lighttable_key_accel_toggle_culling_zoom_mode,
                     GDK_KEY_less, 0);
  dt_action_register(ltv, N_("exit current layout"), _lib_lighttable_key_accel_exit_layout,
                     GDK_KEY_Escape, 0);
}

void gui_cleanup(dt_lib_module_t *self)
{
  g_free(self->data);
  self->data = NULL;
}

static void _set_zoom(dt_lib_module_t *self, int zoom)
{
  dt_lib_tool_lighttable_t *d = self->data;
  if(d->layout == DT_LIGHTTABLE_LAYOUT_CULLING)
  {
    dt_conf_set_int("plugins/lighttable/culling_num_images", zoom);
    dt_control_queue_redraw_center();
  }
  else if(d->layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER || d->layout == DT_LIGHTTABLE_LAYOUT_ZOOMABLE)
  {
    dt_conf_set_int("plugins/lighttable/images_in_row", zoom);
    dt_thumbtable_zoom_changed(dt_ui_thumbtable(darktable.gui->ui), d->current_zoom, zoom);
  }
}

static void _lib_lighttable_zoom_slider_changed(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_tool_lighttable_t *d = self->data;

  const int i = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widget));
  _set_zoom(self, i);
  d->current_zoom = i;
}


static dt_lighttable_layout_t _lib_lighttable_get_layout(dt_lib_module_t *self)
{
  dt_lib_tool_lighttable_t *d = self->data;
  return d ? d->layout : DT_LIGHTTABLE_LAYOUT_FILEMANAGER;
}

static void _lib_lighttable_set_zoom(dt_lib_module_t *self, gint zoom)
{
  dt_lib_tool_lighttable_t *d = self->data;
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(d->zoom), zoom);
  d->current_zoom = zoom;
}

static gint _lib_lighttable_get_zoom(dt_lib_module_t *self)
{
  dt_lib_tool_lighttable_t *d = self->data;
  return d->current_zoom;
}

#ifdef USE_LUA
static int layout_cb(lua_State *L)
{
  dt_lib_module_t *self = lua_touserdata(L, lua_upvalueindex(1));
  const dt_lighttable_layout_t tmp = _lib_lighttable_get_layout(self);
  if(lua_gettop(L) > 0){
    dt_lighttable_layout_t value;
    luaA_to(L, dt_lighttable_layout_t, &value, 1);
    _lib_lighttable_set_layout(self, value);
  }
  luaA_push(L, dt_lighttable_layout_t, &tmp);
  return 1;
}
static int zoom_level_cb(lua_State *L)
{
  dt_lib_module_t *self = lua_touserdata(L, lua_upvalueindex(1));
  const gint tmp = _lib_lighttable_get_zoom(self);
  if(lua_gettop(L) > 0){
    int value;
    luaA_to(L, int, &value, 1);
    _lib_lighttable_set_zoom(self, value);
  }
  luaA_push(L, int, &tmp);
  return 1;
}

void init(struct dt_lib_module_t *self)
{
  lua_State *L = darktable.lua_state.state;
  int my_type = dt_lua_module_entry_get_type(L, "lib", self->plugin_name);
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, layout_cb, 1);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, my_type, "layout");
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, zoom_level_cb, 1);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, my_type, "zoom_level");

  luaA_enum(L,dt_lighttable_layout_t);
  luaA_enum_value(L, dt_lighttable_layout_t, DT_LIGHTTABLE_LAYOUT_FIRST);
  luaA_enum_value(L, dt_lighttable_layout_t, DT_LIGHTTABLE_LAYOUT_ZOOMABLE);
  luaA_enum_value(L, dt_lighttable_layout_t, DT_LIGHTTABLE_LAYOUT_FILEMANAGER);
  luaA_enum_value(L, dt_lighttable_layout_t, DT_LIGHTTABLE_LAYOUT_CULLING);
  luaA_enum_value(L, dt_lighttable_layout_t, DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC);
  luaA_enum_value(L, dt_lighttable_layout_t, DT_LIGHTTABLE_LAYOUT_PREVIEW);
  luaA_enum_value(L, dt_lighttable_layout_t, DT_LIGHTTABLE_LAYOUT_LAST);
}
#endif
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
