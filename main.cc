extern "C" {
#include <wayland-client.h>
#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include <glib-object.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/mman.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
}

struct wl_state {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wl_buffer *buffer;
    bool running;
    int width;
    int height;
};

static void registry_global(void *data, struct wl_registry *registry,
                           uint32_t name, const char *interface, uint32_t version) {
    struct wl_state *state = static_cast<struct wl_state *>(data);

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        state->compositor = static_cast<struct wl_compositor *>(wl_registry_bind(
            registry, name, &wl_compositor_interface, 4));
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        state->shm = static_cast<struct wl_shm *>(wl_registry_bind(
            registry, name, &wl_shm_interface, 1));
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        state->layer_shell = static_cast<struct zwlr_layer_shell_v1 *>(wl_registry_bind(
            registry, name, &zwlr_layer_shell_v1_interface, 1));
    }
}

static void registry_global_remove(void * /*data*/, struct wl_registry * /*registry*/,
                                   uint32_t /*name*/) {
    // Not handling removal for this simple example
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void layer_surface_configure(void * /*data*/,
                                   struct zwlr_layer_surface_v1 *layer_surface,
                                   uint32_t serial, uint32_t /*width*/, uint32_t /*height*/) {
    zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
}

static void layer_surface_closed(void *data,
                                struct zwlr_layer_surface_v1 * /*layer_surface*/) {
    struct wl_state *state = static_cast<struct wl_state *>(data);
    state->running = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

static struct wl_buffer *create_buffer(struct wl_state *state) {
    int stride = state->width * 4;
    int size = stride * state->height;

    // Create shared memory file
    int fd = memfd_create("wayland-shm", 0);
    if (fd < 0) {
        return nullptr;
    }

    if (ftruncate(fd, size) < 0) {
        close(fd);
        return nullptr;
    }

    void *data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return nullptr;
    }

    // Create cairo surface and draw
    cairo_surface_t *cairo_surface = cairo_image_surface_create_for_data(
        static_cast<unsigned char *>(data), CAIRO_FORMAT_ARGB32, state->width, state->height, stride);
    cairo_t *cr = cairo_create(cairo_surface);

    // Draw blue rectangle background
    cairo_set_source_rgb(cr, 0.2, 0.4, 0.8);
    cairo_rectangle(cr, 0, 0, state->width, state->height);
    cairo_fill(cr);

    // Draw "Hello World" text
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_from_string("Sans Bold 24");
    pango_layout_set_font_description(layout, desc);
    pango_layout_set_text(layout, "Hello World", -1);

    int text_width, text_height;
    pango_layout_get_pixel_size(layout, &text_width, &text_height);

    cairo_move_to(cr, (state->width - text_width) / 2, (state->height - text_height) / 2);
    pango_cairo_show_layout(cr, layout);

    pango_font_description_free(desc);
    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(cairo_surface);

    // Create wayland buffer
    struct wl_shm_pool *pool = wl_shm_create_pool(state->shm, fd, size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(
        pool, 0, state->width, state->height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);

    munmap(data, size);
    close(fd);

    return buffer;
}

int main() {
    struct wl_state state = {};
    state.running = true;
    state.width = 300;
    state.height = 100;

    // Connect to Wayland display
    state.display = wl_display_connect(nullptr);
    if (!state.display) {
        fprintf(stderr, "Failed to connect to Wayland display\n");
        return 1;
    }

    // Get registry and bind globals
    state.registry = wl_display_get_registry(state.display);
    wl_registry_add_listener(state.registry, &registry_listener, &state);
    wl_display_roundtrip(state.display);

    if (!state.compositor || !state.shm || !state.layer_shell) {
        fprintf(stderr, "Failed to bind required Wayland interfaces\n");
        return 1;
    }

    // Create surface and layer surface
    state.surface = wl_compositor_create_surface(state.compositor);
    state.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        state.layer_shell, state.surface, nullptr,
        ZWLR_LAYER_SHELL_V1_LAYER_TOP, "hello-world");

    // Configure layer surface
    zwlr_layer_surface_v1_set_size(state.layer_surface, state.width, state.height);
    zwlr_layer_surface_v1_set_anchor(state.layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_add_listener(state.layer_surface, &layer_surface_listener, &state);

    wl_surface_commit(state.surface);
    wl_display_roundtrip(state.display);

    // Create and attach buffer
    state.buffer = create_buffer(&state);
    if (!state.buffer) {
        fprintf(stderr, "Failed to create buffer\n");
        return 1;
    }

    wl_surface_attach(state.surface, state.buffer, 0, 0);
    wl_surface_commit(state.surface);

    // Main loop
    while (state.running && wl_display_dispatch(state.display) != -1) {
        // Event loop
    }

    // Cleanup
    if (state.buffer) wl_buffer_destroy(state.buffer);
    if (state.layer_surface) zwlr_layer_surface_v1_destroy(state.layer_surface);
    if (state.surface) wl_surface_destroy(state.surface);
    if (state.layer_shell) zwlr_layer_shell_v1_destroy(state.layer_shell);
    if (state.compositor) wl_compositor_destroy(state.compositor);
    if (state.shm) wl_shm_destroy(state.shm);
    if (state.registry) wl_registry_destroy(state.registry);
    if (state.display) wl_display_disconnect(state.display);

    return 0;
}
