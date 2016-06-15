/*
 * Copyright © 2016 Sergiy Kibrik
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
/*#define NDEBUG*/
#include <errno.h>
#include <assert.h>
#include <stdbool.h>

#include <compositor.h>

#include "xdg-shell-server-protocol.h"

/* #define dbg_log(...) weston_log(__VA_ARGS__) */
#define dbg_log(...)

struct eink_shell {
	struct weston_compositor *compositor;
	struct weston_layer panel;
	struct weston_layer bg;
	struct weston_surface *bg_surface;
	struct weston_view *bg_view;

	struct wl_listener destroy_listener;
	struct wl_list focus_stack;
};

struct eink_shell_client {
	struct wl_client *client;
	struct wl_resource *resource;
	struct wl_listener destroy_listener;
	struct eink_shell *shell;
	uint32_t id;
};

struct eink_shell_surface {
	struct wl_resource *resource;
	struct wl_listener destroy_listener;
	/*FIXME: dirty workaround*/
	struct wl_listener tmp_listener;

	struct weston_surface *surface;
	struct weston_view *view;

	struct eink_shell *shell;
	struct eink_shell_client *owner;
	uint32_t id;
	struct wl_list focus_link;
};

/*
 * Returns the bounding box of a surface and all its sub-surfaces,
 * in the surface coordinates system.
 * FIXME: boilerplate from desktop-shell
*/

static void
surface_subsurfaces_boundingbox(struct weston_surface *surface, int32_t *x,
				int32_t *y, int32_t *w, int32_t *h) {
	pixman_region32_t region;
	pixman_box32_t *box;
	struct weston_subsurface *subsurface;

	pixman_region32_init_rect(&region, 0, 0,
	                          surface->width,
	                          surface->height);

	wl_list_for_each(subsurface, &surface->subsurface_list, parent_link) {
		pixman_region32_union_rect(&region, &region,
		                           subsurface->position.x,
		                           subsurface->position.y,
		                           subsurface->surface->width,
		                           subsurface->surface->height);
	}

	box = pixman_region32_extents(&region);
	if (x)
		*x = box->x1;
	if (y)
		*y = box->y1;
	if (w)
		*w = box->x2 - box->x1;
	if (h)
		*h = box->y2 - box->y1;

	pixman_region32_fini(&region);
}

static void
center_on_output(struct weston_view *view, struct weston_output *output)
{
	int32_t surf_x, surf_y, width, height;
	float x, y;

	surface_subsurfaces_boundingbox(view->surface, &surf_x, &surf_y, &width, &height);

	x = output->x + (output->width - width) / 2 - surf_x / 2;
	y = output->y + (output->height - height) / 2 - surf_y / 2;

	weston_view_set_position(view, x, y);
}

static void
shell_destroy(struct wl_listener *listener, void *data)
{
	struct eink_shell *shell =
		container_of(listener, struct eink_shell, destroy_listener);
	wl_list_remove(&shell->destroy_listener.link);
	free(shell);
}

static void
shell_client_destroy(struct wl_listener *listener, void *data)
{
	struct eink_shell_client *sc =
		container_of(listener, struct eink_shell_client,
			     destroy_listener);
	wl_list_remove(&sc->destroy_listener.link);
	dbg_log("E-Ink: client %u unbound\n", sc->id);
	free(sc);
}

static void
shell_surface_destroy(struct wl_listener *listener, void *data)
{
	struct eink_shell_surface *shsurf = container_of(listener,
						    struct eink_shell_surface,
						    destroy_listener);
	struct eink_shell *shell = shsurf->shell;
	bool surf_is_active = false;

	assert(!wl_list_empty(&shell->focus_stack));

	if (&shsurf->focus_link == shell->focus_stack.next)
		surf_is_active = true;

	wl_list_remove(&shsurf->focus_link);
	if (!wl_list_empty(&shell->focus_stack) && surf_is_active) {
		struct weston_compositor *ec = shsurf->shell->compositor;
		struct weston_seat *seat =
			container_of(ec->seat_list.next,
				     struct weston_seat, link);
		struct eink_shell_surface *focus =
			container_of(shell->focus_stack.next,
				     struct eink_shell_surface, focus_link);
		weston_surface_activate(focus->surface, seat);
	}

	weston_view_destroy(shsurf->view);

	if (shsurf->resource)
		wl_resource_destroy(shsurf->resource);
	wl_list_remove(&shsurf->destroy_listener.link);

	dbg_log("%s: surface %u destroyed\n",
			__func__, shsurf->id);
	free(shsurf);
}

static void
eink_shell_surface_configure(struct weston_surface *es, int32_t sx, int32_t sy)
{
	struct eink_shell_surface *shsurf;
	struct eink_shell *shell;
	struct weston_output *output =
			container_of(es->compositor->output_list.next,
				     struct weston_output, link);

	if (es->configure == eink_shell_surface_configure)
		shsurf = es->configure_private;
	else
		return;

	shell = shsurf->shell;
	if (wl_list_empty(&shsurf->view->layer_link))
		wl_list_insert(&shell->panel.view_list,
			       &shsurf->view->layer_link);
	center_on_output(shsurf->view, output);
	weston_surface_damage(shsurf->surface);
	weston_view_update_transform(shsurf->view);
}

static void
stub_destroy_handler(struct wl_listener *listener, void *data)
{
	dbg_log("%s entered\n", __func__);
	wl_list_remove(&listener->link);
}

static struct eink_shell_surface *
create_surface(struct wl_client *client, struct wl_resource *resource,
		uint32_t id, struct weston_surface *surface)
{
	struct eink_shell_surface *shsurf;
	struct eink_shell_client *sc =
		wl_resource_get_user_data(resource);
	struct weston_compositor *ec = surface->compositor;
	struct weston_seat *seat = container_of(ec->seat_list.next,
				   struct weston_seat, link);

	shsurf = zalloc(sizeof(*shsurf));
	if (shsurf == NULL) {
		wl_client_post_no_memory(client);
		return NULL;
	}

	shsurf->destroy_listener.notify = shell_surface_destroy;
	wl_resource_add_destroy_listener(surface->resource,
					 &shsurf->destroy_listener);

	shsurf->tmp_listener.notify = stub_destroy_handler;
	wl_resource_add_destroy_listener(surface->resource,
					 &shsurf->tmp_listener);
	shsurf->surface = surface;
	shsurf->view = weston_view_create(shsurf->surface);
	shsurf->shell = sc->shell;
	shsurf->owner = sc;
	shsurf->id = id;

	dbg_log("%s: client %u created surface %u\n",
			__func__, sc->id, shsurf->id);

	weston_surface_activate(surface, seat);
	wl_list_insert(&shsurf->shell->focus_stack,
			&shsurf->focus_link);

	surface->configure = eink_shell_surface_configure;
	surface->configure_private = shsurf;
	return shsurf;
}

/*   ===== XDG shell =====   */

static void
xdg_surface_destroy(struct wl_client *client,
		    struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
xdg_surface_set_transient_for(struct wl_client *client,
                             struct wl_resource *resource,
                             struct wl_resource *parent_resource)
{
	dbg_log("E-Ink trace: %s\n", __func__);
}

static void
xdg_surface_set_margin(struct wl_client *client,
			     struct wl_resource *resource,
			     int32_t left,
			     int32_t right,
			     int32_t top,
			     int32_t bottom)
{
	dbg_log("E-Ink trace: %s\n", __func__);
}

static void
xdg_surface_set_title(struct wl_client *client,
			struct wl_resource *resource, const char *title)
{
	dbg_log("E-Ink trace: %s\n", __func__);
}

static void
xdg_surface_set_app_id(struct wl_client *client,
		       struct wl_resource *resource,
		       const char *app_id)
{
	dbg_log("E-Ink trace: %s\n", __func__);
}

static void
xdg_surface_move(struct wl_client *client, struct wl_resource *resource,
		 struct wl_resource *seat_resource, uint32_t serial)
{
	dbg_log("E-Ink trace: %s\n", __func__);
}

static void
xdg_surface_resize(struct wl_client *client, struct wl_resource *resource,
		   struct wl_resource *seat_resource, uint32_t serial,
		   uint32_t edges)
{
	dbg_log("E-Ink trace: %s\n", __func__);
}

static void
xdg_surface_set_output(struct wl_client *client,
		       struct wl_resource *resource,
		       struct wl_resource *output_resource)
{
	dbg_log("E-Ink trace: %s\n", __func__);
}

static void
xdg_surface_request_change_state(struct wl_client *client,
				 struct wl_resource *resource,
				 uint32_t state,
				 uint32_t value,
				 uint32_t serial)
{
	dbg_log("E-Ink trace: %s\n", __func__);
}

static void
xdg_surface_ack_change_state(struct wl_client *client,
			     struct wl_resource *resource,
			     uint32_t state,
			     uint32_t value,
			     uint32_t serial)
{
	dbg_log("E-Ink trace: %s\n", __func__);
}

static void
xdg_surface_set_minimized(struct wl_client *client,
			    struct wl_resource *resource)
{
	dbg_log("E-Ink trace: %s\n", __func__);
}

static const struct xdg_surface_interface xdg_surface_implementation = {
	xdg_surface_destroy,
	xdg_surface_set_transient_for,
	xdg_surface_set_margin,
	xdg_surface_set_title,
	xdg_surface_set_app_id,
	xdg_surface_move,
	xdg_surface_resize,
	xdg_surface_set_output,
	xdg_surface_request_change_state,
	xdg_surface_ack_change_state,
	xdg_surface_set_minimized
};

static void
xdg_use_unstable_version(struct wl_client *client,
			 struct wl_resource *resource,
			 int32_t version)
{
	dbg_log("E-Ink trace: %s\n", __func__);
}

static void
xdg_get_xdg_surface(struct wl_client *client,
		    struct wl_resource *resource,
		    uint32_t id,
		    struct wl_resource *surface_resource)
{
	struct eink_shell_surface *shsurf;
	struct weston_surface *surface =
		wl_resource_get_user_data(surface_resource);

	shsurf = create_surface(client, resource, id, surface);
	if (!shsurf)
		return;

	shsurf->resource =
		wl_resource_create(client,
				   &xdg_surface_interface, 1, id);
	wl_resource_set_implementation(shsurf->resource,
				       &xdg_surface_implementation,
				       shsurf, NULL);
}

static void
xdg_get_xdg_popup(struct wl_client *client,
		  struct wl_resource *resource,
		  uint32_t id,
		  struct wl_resource *surface_resource,
		  struct wl_resource *parent_resource,
		  struct wl_resource *seat_resource,
		  uint32_t serial,
		  int32_t x, int32_t y, uint32_t flags)
{
	dbg_log("E-Ink trace: %s\n", __func__);
}

static void
xdg_pong(struct wl_client *client,
	 struct wl_resource *resource, uint32_t serial)
{
	dbg_log("E-Ink trace: %s\n", __func__);
}

static const struct xdg_shell_interface xdg_implementation = {
	xdg_use_unstable_version,
	xdg_get_xdg_surface,
	xdg_get_xdg_popup,
	xdg_pong
};

/*   ===== WL shell =====   */

static void
shell_surface_pong(struct wl_client *client,
		   struct wl_resource *resource, uint32_t serial)
{
	dbg_log("E-Ink trace: %s\n", __func__);
}

static void
shell_surface_move(struct wl_client *client, struct wl_resource *resource,
		   struct wl_resource *seat_resource, uint32_t serial)
{
	dbg_log("E-Ink trace: %s\n", __func__);
}

static void
shell_surface_resize(struct wl_client *client, struct wl_resource *resource,
		     struct wl_resource *seat_resource, uint32_t serial,
		     uint32_t edges)
{
	dbg_log("E-Ink trace: %s\n", __func__);
}

static void
shell_surface_set_toplevel(struct wl_client *client,
			   struct wl_resource *resource)
{
	dbg_log("E-Ink trace: %s\n", __func__);
}

static void
shell_surface_set_transient(struct wl_client *client,
			    struct wl_resource *resource,
			    struct wl_resource *parent_resource,
			    int x, int y, uint32_t flags)
{
	dbg_log("E-Ink trace: %s\n", __func__);
}

static void
shell_surface_set_fullscreen(struct wl_client *client,
			     struct wl_resource *resource,
			     uint32_t method,
			     uint32_t framerate,
			     struct wl_resource *output_resource)
{
	dbg_log("E-Ink trace: %s\n", __func__);
}

static void
shell_surface_set_popup(struct wl_client *client,
			struct wl_resource *resource,
			struct wl_resource *seat_resource,
			uint32_t serial,
			struct wl_resource *parent_resource,
			int32_t x, int32_t y, uint32_t flags)
{
	dbg_log("E-Ink trace: %s\n", __func__);
}

static void
shell_surface_set_maximized(struct wl_client *client,
                            struct wl_resource *resource,
                            struct wl_resource *output_resource)
{
	dbg_log("E-Ink trace: %s\n", __func__);
}

static void
shell_surface_set_title(struct wl_client *client,
			struct wl_resource *resource, const char *title)
{
	dbg_log("E-Ink trace: %s\n", __func__);
}

static void
shell_surface_set_class(struct wl_client *client,
			struct wl_resource *resource, const char *class)
{
	dbg_log("E-Ink trace: %s\n", __func__);
}

static const struct wl_shell_surface_interface shell_surface_implementation = {
	shell_surface_pong,
	shell_surface_move,
	shell_surface_resize,
	shell_surface_set_toplevel,
	shell_surface_set_transient,
	shell_surface_set_fullscreen,
	shell_surface_set_popup,
	shell_surface_set_maximized,
	shell_surface_set_title,
	shell_surface_set_class
};

static void
shell_get_shell_surface(struct wl_client *client,
			struct wl_resource *resource,
			uint32_t id,
			struct wl_resource *surface_resource)
{
	struct eink_shell_surface *shsurf;
	struct weston_surface *surface =
		wl_resource_get_user_data(surface_resource);

	shsurf = create_surface(client, resource, id, surface);
	if (!shsurf)
		return;

	shsurf->resource =
		wl_resource_create(client,
				   &wl_shell_surface_interface, 1, id);
	wl_resource_set_implementation(shsurf->resource,
				       &shell_surface_implementation,
				       shsurf, NULL);
}

static const struct wl_shell_interface shell_implementation = {
	shell_get_shell_surface
};

static int eink_bg_init(struct eink_shell *shell)
{
	struct weston_surface *surface;
	struct weston_view *view;
	struct weston_compositor *ec = shell->compositor;
	struct weston_output *output = container_of(ec->output_list.next,
					 	struct weston_output, link);

	surface = weston_surface_create(ec);
	if (surface == NULL)
		return -ENOMEM;
	view = weston_view_create(surface);
	if (view == NULL) {
		weston_surface_destroy(surface);
		return -ENOMEM;
	}

	weston_surface_set_size(surface, output->width, output->height);
	weston_view_set_position(view, output->x, output->y);
	weston_surface_set_color(surface, 1.0, 1.0, 1.0, 1.0);

	wl_list_insert(&shell->bg.view_list,
		       &view->layer_link);

	shell->bg_surface = surface;
	shell->bg_view = view;
	return 0;
}

static struct eink_shell_client *
create_shell_client(struct wl_client *client, struct eink_shell *shell,
		    const struct wl_interface *interface, uint32_t id)
{
	struct eink_shell_client *sc;

	sc = zalloc(sizeof(*sc));
	if (sc == NULL) {
		wl_client_post_no_memory(client);
		return NULL;
	}

	sc->resource = wl_resource_create(client, interface, 1, id);
	if (sc->resource == NULL) {
		free(sc);
		wl_client_post_no_memory(client);
		return NULL;
	}
	sc->client = client;
	sc->shell = shell;
	sc->id = id;
	sc->destroy_listener.notify = shell_client_destroy;
	wl_client_add_destroy_listener(client, &sc->destroy_listener);

	return sc;
}

static void
bind_xdg_shell(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct eink_shell *shell = data;
	struct eink_shell_client *sc;

	sc = create_shell_client(client, shell, &xdg_shell_interface, id);
	if (!sc)
		return;

	wl_resource_set_implementation(sc->resource, &xdg_implementation,
				       sc, NULL);
	dbg_log("E-Ink: client %u bound\n", sc->id);

	if (!shell->bg_surface && !shell->bg_view) {
		eink_bg_init(shell);
		dbg_log("E-Ink: bg initialized\n");
	}
}

static void
bind_shell(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct eink_shell *shell = data;
	struct eink_shell_client *sc;

	sc = create_shell_client(client, shell, &wl_shell_interface, id);
	if (!sc)
		return;
	wl_resource_set_implementation(sc->resource, &shell_implementation,
				       sc, NULL);
	dbg_log("E-Ink: client %u bound\n", sc->id);
	/*FIXME: boilerplate */
	if (!shell->bg_surface && !shell->bg_view) {
		eink_bg_init(shell);
		dbg_log("E-Ink: bg initialized\n");
	}
}

WL_EXPORT int
module_init(struct weston_compositor *ec,
	    int *argc, char *argv[])
{
	struct eink_shell *shell;

	shell = zalloc(sizeof(*shell));
	if (shell == NULL)
		return -ENOMEM;
	shell->compositor = ec;

	weston_layer_init(&shell->panel, &ec->layer_list);
	weston_layer_init(&shell->bg, &shell->panel.link);

	shell->destroy_listener.notify = shell_destroy;
	wl_signal_add(&ec->destroy_signal, &shell->destroy_listener);
	wl_list_init(&shell->focus_stack);

	if (wl_global_create(ec->wl_display, &xdg_shell_interface, 1,
				  shell, bind_xdg_shell) == NULL)
		return -EINVAL;
	if (wl_global_create(ec->wl_display, &wl_shell_interface, 1,
				  shell, bind_shell) == NULL)
		return -EINVAL;

	return 0;
}
