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
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
}

#include <vector>
#include <string>
#include <map>

struct wl_output_data {
    struct wl_output *output;
    int32_t scale;
    uint32_t name;
};

struct wl_state {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wl_buffer *buffer;

    std::vector<std::string> menu_items;
    bool running;
    int width;
    int height;

    // HiDPI related
    std::map<uint32_t, wl_output_data> outputs_by_name;
    struct wl_output *chosen_output;
    int chosen_scale;
};

static void output_geometry(void*, struct wl_output*, int, int, int, int, int, const char*, const char*, int) {}
static void output_mode(void*, struct wl_output*, uint32_t, int, int, int) {}
static void output_done(void*, struct wl_output*) {}
static void output_name(void*, struct wl_output*, const char*) {}
static void output_description(void*, struct wl_output*, const char*) {}

static void output_scale(void *data, struct wl_output *output, int32_t factor) {
    wl_state* state = (wl_state*)data;
    for (auto& pair : state->outputs_by_name) {
        if (pair.second.output == output) {
            pair.second.scale = factor;
        }
    }
}

// Use the newer output listener if available, fallback otherwise
static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
#if WL_OUTPUT_NAME_SINCE_VERSION
    .name = output_name,
    .description = output_description,
#endif
};

static void registry_global(void *data, struct wl_registry *registry,
                           uint32_t name, const char *interface, uint32_t version) {
    wl_state *state = static_cast<wl_state *>(data);

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        state->compositor = static_cast<struct wl_compositor *>(wl_registry_bind(
            registry, name, &wl_compositor_interface, 4));
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        state->shm = static_cast<struct wl_shm *>(wl_registry_bind(
            registry, name, &wl_shm_interface, 1));
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        state->layer_shell = static_cast<struct zwlr_layer_shell_v1 *>(wl_registry_bind(
            registry, name, &zwlr_layer_shell_v1_interface, 1));
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        struct wl_output *output = static_cast<struct wl_output*>(wl_registry_bind(
            registry, name, &wl_output_interface, (version >= 2) ? 2 : 1));
        wl_output_data od;
        od.output = output;
        od.scale = 1;
        od.name = name;
        wl_output_add_listener(output, &output_listener, state);
        state->outputs_by_name[name] = od;
    }
}

static void registry_global_remove(void *data, struct wl_registry *, uint32_t name) {
    wl_state *state = static_cast<wl_state *>(data);
    auto it = state->outputs_by_name.find(name);
    if (it != state->outputs_by_name.end()) {
        wl_output_destroy(it->second.output);
        state->outputs_by_name.erase(it);
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void layer_surface_configure(void *, struct zwlr_layer_surface_v1 *layer_surface,
                                   uint32_t serial, uint32_t, uint32_t) {
    zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
}

static void layer_surface_closed(void *data,
                                struct zwlr_layer_surface_v1 *) {
    wl_state *state = static_cast<wl_state *>(data);
    state->running = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

static struct wl_buffer *create_buffer(struct wl_state *state) {
    const int button_height = 40;
    const int padding = 10;
    const int button_spacing = 5;
    const int text_padding = 10;
    const int min_width = 200;

    int scale = state->chosen_scale;
    // --- Everything below is in logical coordinates, scale at the end! ---

    // Calculate width based on widest text
    PangoFontDescription *desc = pango_font_description_from_string("Sans 12");
    cairo_surface_t *temp_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *temp_cr = cairo_create(temp_surface);

    int max_text_width = 0;
    for (const auto& item : state->menu_items) {
        PangoLayout *layout = pango_cairo_create_layout(temp_cr);
        pango_layout_set_font_description(layout, desc);
        pango_layout_set_text(layout, item.c_str(), -1);

        int text_width, text_height;
        pango_layout_get_pixel_size(layout, &text_width, &text_height);

        if (text_width > max_text_width) {
            max_text_width = text_width;
        }

        g_object_unref(layout);
    }

    cairo_destroy(temp_cr);
    cairo_surface_destroy(temp_surface);

    // Logical size
    int logical_width = max_text_width + 2 * padding + 2 * text_padding;
    if (logical_width < min_width) {
        logical_width = min_width;
    }
    int logical_height = state->menu_items.size() * (button_height + button_spacing) + padding * 2 - button_spacing;

    // Physical size (actual buffer)
    state->width = logical_width * scale;
    state->height = logical_height * scale;
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

    // Scale Cairo context so all drawing is in logical coordinates
    cairo_scale(cr, scale, scale);

    // Draw background
    cairo_set_source_rgb(cr, 0.15, 0.15, 0.15);
    cairo_rectangle(cr, 0, 0, logical_width, logical_height);
    cairo_fill(cr);

    for (size_t i = 0; i < state->menu_items.size(); i++) {
        int y = padding + i * (button_height + button_spacing);

        // Draw button background
        cairo_set_source_rgb(cr, 0.3, 0.3, 0.35);
        cairo_rectangle(cr, padding, y, logical_width - 2 * padding, button_height);
        cairo_fill(cr);

        // Draw button border
        cairo_set_source_rgb(cr, 0.5, 0.5, 0.55);
        cairo_set_line_width(cr, 1.0);
        cairo_rectangle(cr, padding, y, logical_width - 2 * padding, button_height);
        cairo_stroke(cr);

        // Draw text
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        PangoLayout *layout = pango_cairo_create_layout(cr);
        pango_layout_set_font_description(layout, desc);
        pango_layout_set_text(layout, state->menu_items[i].c_str(), -1);

        int text_width, text_height;
        pango_layout_get_pixel_size(layout, &text_width, &text_height);

        cairo_move_to(cr, padding + 10, y + (button_height - text_height) / 2);
        pango_cairo_show_layout(cr, layout);

        g_object_unref(layout);
    }

    pango_font_description_free(desc);
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
    state.width = 200;
    state.height = 100;
    state.chosen_output = nullptr;
    state.chosen_scale = 1;

    // Read menu items from stdin
    char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        // Remove newline
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
        }
        if (strlen(line) > 0) {
            state.menu_items.push_back(std::string(line));
        }
    }

    if (state.menu_items.empty()) {
        fprintf(stderr, "No menu items provided on stdin\n");
        return 1;
    }

    // Connect to Wayland display
    state.display = wl_display_connect(nullptr);
    if (!state.display) {
        fprintf(stderr, "Failed to connect to Wayland display\n");
        return 1;
    }

    // Get registry and bind globals
    state.registry = wl_display_get_registry(state.display);
    wl_registry_add_listener(state.registry, &registry_listener, &state);
    wl_display_roundtrip(state.display); // Discover globals
    wl_display_roundtrip(state.display); // Ensure all output events delivered

    // Choose output and scale
    if (!state.outputs_by_name.empty()) {
        // Pick the first output
        auto it = state.outputs_by_name.begin();
        state.chosen_output = it->second.output;
        state.chosen_scale = (it->second.scale > 0) ? it->second.scale : 1;
    } else {
        state.chosen_scale = 1;
    }

    if (!state.compositor || !state.shm || !state.layer_shell) {
        fprintf(stderr, "Failed to bind required Wayland interfaces\n");
        return 1;
    }

    // Create surface and layer surface
    state.surface = wl_compositor_create_surface(state.compositor);
    // Set buffer scale BEFORE attaching any buffer!
    wl_surface_set_buffer_scale(state.surface, state.chosen_scale);

    state.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        state.layer_shell, state.surface, state.chosen_output,
        ZWLR_LAYER_SHELL_V1_LAYER_TOP, "menu");

    zwlr_layer_surface_v1_set_size(state.layer_surface,
        state.width / state.chosen_scale, state.height / state.chosen_scale); // Logical size
    zwlr_layer_surface_v1_set_anchor(state.layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
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
    wl_surface_damage_buffer(state.surface, 0, 0, state.width, state.height);
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
    for (auto& pair : state.outputs_by_name) {
        if (pair.second.output) wl_output_destroy(pair.second.output);
    }
    if (state.registry) wl_registry_destroy(state.registry);
    if (state.display) wl_display_disconnect(state.display);

    return 0;
}
