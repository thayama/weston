/*
 * Copyright © 2010-2012 Intel Corporation
 * Copyright © 2011-2012 Collabora, Ltd.
 * Copyright © 2013 Raspberry Pi Foundation
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "shell.h"
#include "desktop-shell-server-protocol.h"
#include "input-method-server-protocol.h"

struct input_panel_surface {
	struct wl_resource *resource;
	struct wl_signal destroy_signal;

	struct desktop_shell *shell;

	struct wl_list link;
	struct weston_surface *surface;
	struct weston_view *view;
	struct wl_listener surface_destroy_listener;

	struct weston_output *output;
	uint32_t panel;
};

static void
show_input_panels(struct wl_listener *listener, void *data)
{
	struct desktop_shell *shell =
		container_of(listener, struct desktop_shell,
			     show_input_panel_listener);
	struct input_panel_surface *ipsurf, *next;

	shell->text_input.surface = (struct weston_surface*)data;

	if (shell->showing_input_panels)
		return;

	shell->showing_input_panels = true;

	if (!shell->locked)
		wl_list_insert(&shell->panel_layer.link,
			       &shell->input_panel_layer.link);

	wl_list_for_each_safe(ipsurf, next,
			      &shell->input_panel.surfaces, link) {
		if (ipsurf->surface->width == 0)
			continue;
		wl_list_insert(&shell->input_panel_layer.view_list,
			       &ipsurf->view->layer_link);
		weston_view_geometry_dirty(ipsurf->view);
		weston_view_update_transform(ipsurf->view);
		weston_surface_damage(ipsurf->surface);
		weston_slide_run(ipsurf->view, ipsurf->surface->height * 0.9,
				 0, NULL, NULL);
	}
}

static void
hide_input_panels(struct wl_listener *listener, void *data)
{
	struct desktop_shell *shell =
		container_of(listener, struct desktop_shell,
			     hide_input_panel_listener);
	struct weston_view *view, *next;

	if (!shell->showing_input_panels)
		return;

	shell->showing_input_panels = false;

	if (!shell->locked)
		wl_list_remove(&shell->input_panel_layer.link);

	wl_list_for_each_safe(view, next,
			      &shell->input_panel_layer.view_list, layer_link)
		weston_view_unmap(view);
}

static void
update_input_panels(struct wl_listener *listener, void *data)
{
	struct desktop_shell *shell =
		container_of(listener, struct desktop_shell,
			     update_input_panel_listener);

	memcpy(&shell->text_input.cursor_rectangle, data, sizeof(pixman_box32_t));
}

static void
input_panel_configure(struct weston_surface *surface, int32_t sx, int32_t sy)
{
	struct input_panel_surface *ip_surface = surface->configure_private;
	struct desktop_shell *shell = ip_surface->shell;
	float x, y;

	if (surface->width == 0)
		return;

	fprintf(stderr, "%s panel: %d, output: %p\n", __FUNCTION__, ip_surface->panel, ip_surface->output);

	if (ip_surface->panel) {
		x = get_default_view(shell->text_input.surface)->geometry.x + shell->text_input.cursor_rectangle.x2;
		y = get_default_view(shell->text_input.surface)->geometry.y + shell->text_input.cursor_rectangle.y2;
	} else {
		x = ip_surface->output->x + (ip_surface->output->width - surface->width) / 2;
		y = ip_surface->output->y + ip_surface->output->height - surface->height;
	}

	weston_view_set_position(ip_surface->view, x, y);

	if (!weston_surface_is_mapped(surface) && shell->showing_input_panels) {
		wl_list_insert(&shell->input_panel_layer.view_list,
			       &ip_surface->view->layer_link);
		weston_view_update_transform(ip_surface->view);
		weston_surface_damage(surface);
		weston_slide_run(ip_surface->view, ip_surface->view->surface->height * 0.9, 0, NULL, NULL);
	}
}

static void
destroy_input_panel_surface(struct input_panel_surface *input_panel_surface)
{
	wl_signal_emit(&input_panel_surface->destroy_signal, input_panel_surface);

	wl_list_remove(&input_panel_surface->surface_destroy_listener.link);
	wl_list_remove(&input_panel_surface->link);

	input_panel_surface->surface->configure = NULL;
	weston_view_destroy(input_panel_surface->view);

	free(input_panel_surface);
}

static struct input_panel_surface *
get_input_panel_surface(struct weston_surface *surface)
{
	if (surface->configure == input_panel_configure) {
		return surface->configure_private;
	} else {
		return NULL;
	}
}

static void
input_panel_handle_surface_destroy(struct wl_listener *listener, void *data)
{
	struct input_panel_surface *ipsurface = container_of(listener,
							     struct input_panel_surface,
							     surface_destroy_listener);

	if (ipsurface->resource) {
		wl_resource_destroy(ipsurface->resource);
	} else {
		destroy_input_panel_surface(ipsurface);
	}
}

static struct input_panel_surface *
create_input_panel_surface(struct desktop_shell *shell,
			   struct weston_surface *surface)
{
	struct input_panel_surface *input_panel_surface;

	input_panel_surface = calloc(1, sizeof *input_panel_surface);
	if (!input_panel_surface)
		return NULL;

	surface->configure = input_panel_configure;
	surface->configure_private = input_panel_surface;

	input_panel_surface->shell = shell;

	input_panel_surface->surface = surface;
	input_panel_surface->view = weston_view_create(surface);

	wl_signal_init(&input_panel_surface->destroy_signal);
	input_panel_surface->surface_destroy_listener.notify = input_panel_handle_surface_destroy;
	wl_signal_add(&surface->destroy_signal,
		      &input_panel_surface->surface_destroy_listener);

	wl_list_init(&input_panel_surface->link);

	return input_panel_surface;
}

static void
input_panel_surface_set_toplevel(struct wl_client *client,
				 struct wl_resource *resource,
				 struct wl_resource *output_resource,
				 uint32_t position)
{
	struct input_panel_surface *input_panel_surface =
		wl_resource_get_user_data(resource);
	struct desktop_shell *shell = input_panel_surface->shell;

	wl_list_insert(&shell->input_panel.surfaces,
		       &input_panel_surface->link);

	input_panel_surface->output = wl_resource_get_user_data(output_resource);
	input_panel_surface->panel = 0;
}

static void
input_panel_surface_set_overlay_panel(struct wl_client *client,
				      struct wl_resource *resource)
{
	struct input_panel_surface *input_panel_surface =
		wl_resource_get_user_data(resource);
	struct desktop_shell *shell = input_panel_surface->shell;

	wl_list_insert(&shell->input_panel.surfaces,
		       &input_panel_surface->link);

	input_panel_surface->panel = 1;
}

static const struct wl_input_panel_surface_interface input_panel_surface_implementation = {
	input_panel_surface_set_toplevel,
	input_panel_surface_set_overlay_panel
};

static void
destroy_input_panel_surface_resource(struct wl_resource *resource)
{
	struct input_panel_surface *ipsurf =
		wl_resource_get_user_data(resource);

	destroy_input_panel_surface(ipsurf);
}

static void
input_panel_get_input_panel_surface(struct wl_client *client,
				    struct wl_resource *resource,
				    uint32_t id,
				    struct wl_resource *surface_resource)
{
	struct weston_surface *surface =
		wl_resource_get_user_data(surface_resource);
	struct desktop_shell *shell = wl_resource_get_user_data(resource);
	struct input_panel_surface *ipsurf;

	if (get_input_panel_surface(surface)) {
		wl_resource_post_error(surface_resource,
				       WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "wl_input_panel::get_input_panel_surface already requested");
		return;
	}

	ipsurf = create_input_panel_surface(shell, surface);
	if (!ipsurf) {
		wl_resource_post_error(surface_resource,
				       WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "surface->configure already set");
		return;
	}

	ipsurf->resource =
		wl_resource_create(client,
				   &wl_input_panel_surface_interface, 1, id);
	wl_resource_set_implementation(ipsurf->resource,
				       &input_panel_surface_implementation,
				       ipsurf,
				       destroy_input_panel_surface_resource);
}

static const struct wl_input_panel_interface input_panel_implementation = {
	input_panel_get_input_panel_surface
};

static void
unbind_input_panel(struct wl_resource *resource)
{
	struct desktop_shell *shell = wl_resource_get_user_data(resource);

	shell->input_panel.binding = NULL;
}

static void
bind_input_panel(struct wl_client *client,
	      void *data, uint32_t version, uint32_t id)
{
	struct desktop_shell *shell = data;
	struct wl_resource *resource;

	resource = wl_resource_create(client,
				      &wl_input_panel_interface, 1, id);

	if (shell->input_panel.binding == NULL) {
		wl_resource_set_implementation(resource,
					       &input_panel_implementation,
					       shell, unbind_input_panel);
		shell->input_panel.binding = resource;
		return;
	}

	wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
			       "interface object already bound");
	wl_resource_destroy(resource);
}

void
input_panel_destroy(struct desktop_shell *shell)
{
	wl_list_remove(&shell->show_input_panel_listener.link);
	wl_list_remove(&shell->hide_input_panel_listener.link);
}

int
input_panel_setup(struct desktop_shell *shell)
{
	struct weston_compositor *ec = shell->compositor;

	shell->show_input_panel_listener.notify = show_input_panels;
	wl_signal_add(&ec->show_input_panel_signal,
		      &shell->show_input_panel_listener);
	shell->hide_input_panel_listener.notify = hide_input_panels;
	wl_signal_add(&ec->hide_input_panel_signal,
		      &shell->hide_input_panel_listener);
	shell->update_input_panel_listener.notify = update_input_panels;
	wl_signal_add(&ec->update_input_panel_signal,
		      &shell->update_input_panel_listener);

	wl_list_init(&shell->input_panel.surfaces);

	if (wl_global_create(shell->compositor->wl_display,
			     &wl_input_panel_interface, 1,
			     shell, bind_input_panel) == NULL)
		return -1;

	return 0;
}