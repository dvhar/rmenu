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
#include <algorithm>

#ifndef BTN_LEFT
#define BTN_LEFT 0x110
#endif

struct MenuItem {
    std::string label;
    std::vector<MenuItem> submenu;

    // Geometry in logical coordinates
    int x = 0, y = 0, w = 0, h = 0;
};

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

    std::vector<MenuItem> menu_items;
    bool running;
    int width;
    int height;

    // HiDPI related
    std::map<uint32_t, wl_output_data> outputs_by_name;
    struct wl_output *chosen_output;
    int chosen_scale;

    // Pointer/seat
    struct wl_seat *seat = nullptr;
    struct wl_pointer *pointer = nullptr;
    int pointer_x = 0; // in logical coords
    int pointer_y = 0; // in logical coords
    bool pointer_inside = false;

    // Hover handling for proof-of-concept
    int hovered_index = -1; // top-level menu index hovered, or -1 if none
};

PangoFontDescription *desc;
const int padding = 10;
const int button_height = 40;
const int button_spacing = 5;
const int text_padding = 10;
const int min_width = 200;
const int submenu_pad = 12;

static void output_geometry(void*, struct wl_output*, int, int, int, int, int, const char*, const char*, int) {}
static void output_mode(void*, struct wl_output*, uint32_t, int, int, int) {}
static void output_done(void*, struct wl_output*) {}
static void output_name(void*, struct wl_output*, const char*) {}
static void output_description(void*, struct wl_output*, const char*) {}

static struct wl_buffer *create_buffer(struct wl_state *state);

static void output_scale(void *data, struct wl_output *output, int32_t factor) {
    wl_state* state = (wl_state*)data;
    for (auto& pair : state->outputs_by_name) {
        if (pair.second.output == output) {
            pair.second.scale = factor;
        }
    }
}

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

//=====================
// Geometry helpers
//=====================

struct RenderedMenuGeometry {
    int width;
    int height;
};

// Recursive geometry assignment
static RenderedMenuGeometry measure_menu_items(
    std::vector<MenuItem>& items,
    cairo_t* cr,
    int base_x = 0,
    int base_y = 0
) {
    int max_text_width = 0;
    std::vector<std::pair<int, int>> text_sizes;

    for (auto& item : items) {
        PangoLayout *layout = pango_cairo_create_layout(cr);
        pango_layout_set_font_description(layout, desc);
        pango_layout_set_text(layout, item.label.c_str(), -1);

        int text_width, text_height;
        pango_layout_get_pixel_size(layout, &text_width, &text_height);
        text_sizes.push_back({text_width, text_height});
        g_object_unref(layout);

        int total_width = text_width + 2 * text_padding;
        if (!item.submenu.empty()) total_width += 20; // space for arrow
        if (total_width > max_text_width) max_text_width = total_width;
    }

    int logical_width = max_text_width + 2 * padding;
    if (logical_width < min_width) logical_width = min_width;
    int logical_height = items.size() * (button_height + button_spacing) + padding * 2 - button_spacing;

    // Assign geometry to each MenuItem
    for (size_t i = 0; i < items.size(); ++i) {
        items[i].x = base_x + padding;
        items[i].y = base_y + padding + i * (button_height + button_spacing);
        items[i].w = logical_width - 2 * padding;
        items[i].h = button_height;
    }

    // Recursively assign for submenus
    for (size_t i = 0; i < items.size(); ++i) {
        if (!items[i].submenu.empty()) {
            // Submenu x & y relative to parent
            // They appear to the right of the main menu, starting at the item position
            cairo_surface_t *temp_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
            cairo_t *temp_cr = cairo_create(temp_surface);
            RenderedMenuGeometry subgeom = measure_menu_items(
                items[i].submenu,
                temp_cr,
                base_x + logical_width + submenu_pad, // right of this menu
                items[i].y // vertical position aligned with item
            );
            cairo_destroy(temp_cr);
            cairo_surface_destroy(temp_surface);
        }
    }

    RenderedMenuGeometry geom = { logical_width, logical_height };
    return geom;
}

//=====================
// Pointer event helpers
//=====================

static int find_hovered_index(const std::vector<MenuItem>& items, int px, int py) {
    for (size_t i = 0; i < items.size(); ++i) {
        const MenuItem& item = items[i];
        if (px >= item.x && px < item.x + item.w &&
            py >= item.y && py < item.y + item.h) {
            if (!item.submenu.empty())
                return (int)i;
            break;
        }
    }
    return -1;
}

static int find_clicked_index(const std::vector<MenuItem>& items, int px, int py) {
    for (size_t i = 0; i < items.size(); ++i) {
        const MenuItem& item = items[i];
        if (px >= item.x && px < item.x + item.w &&
            py >= item.y && py < item.y + item.h) {
            return (int)i;
        }
    }
    return -1;
}

//=====================
// Pointer event listeners
//=====================

static void pointer_enter(void *data, struct wl_pointer *, uint32_t, struct wl_surface *, wl_fixed_t sx, wl_fixed_t sy) {
    wl_state *state = static_cast<wl_state*>(data);
    state->pointer_inside = true;
    state->pointer_x = wl_fixed_to_double(sx);
    state->pointer_y = wl_fixed_to_double(sy);

    int new_hovered = find_hovered_index(state->menu_items, state->pointer_x, state->pointer_y);
    if (state->hovered_index != new_hovered) {
        state->hovered_index = new_hovered;
        if (state->buffer) wl_buffer_destroy(state->buffer);
        state->buffer = create_buffer(state);
        wl_surface_attach(state->surface, state->buffer, 0, 0);
        wl_surface_damage_buffer(state->surface, 0, 0, state->width, state->height);
        wl_surface_commit(state->surface);
    }
}

static void pointer_leave(void *data, struct wl_pointer *, uint32_t, struct wl_surface *) {
    wl_state *state = static_cast<wl_state*>(data);
    state->pointer_inside = false;
    if (state->hovered_index != -1) {
        state->hovered_index = -1;
        if (state->buffer) wl_buffer_destroy(state->buffer);
        state->buffer = create_buffer(state);
        wl_surface_attach(state->surface, state->buffer, 0, 0);
        wl_surface_damage_buffer(state->surface, 0, 0, state->width, state->height);
        wl_surface_commit(state->surface);
    }
}

static void pointer_motion(void *data, struct wl_pointer *, uint32_t, wl_fixed_t sx, wl_fixed_t sy) {
    wl_state *state = static_cast<wl_state*>(data);
    state->pointer_x = wl_fixed_to_double(sx);
    state->pointer_y = wl_fixed_to_double(sy);

    int new_hovered = find_hovered_index(state->menu_items, state->pointer_x, state->pointer_y);
    if (state->hovered_index != new_hovered) {
        state->hovered_index = new_hovered;
        if (state->buffer) wl_buffer_destroy(state->buffer);
        state->buffer = create_buffer(state);
        wl_surface_attach(state->surface, state->buffer, 0, 0);
        wl_surface_damage_buffer(state->surface, 0, 0, state->width, state->height);
        wl_surface_commit(state->surface);
    }
}

static void pointer_button(void *data, struct wl_pointer *, uint32_t, uint32_t, uint32_t button, uint32_t state_wl) {
    wl_state *state = static_cast<wl_state*>(data);
    if (button == BTN_LEFT && state_wl == WL_POINTER_BUTTON_STATE_PRESSED) {
        int clicked = find_clicked_index(state->menu_items, state->pointer_x, state->pointer_y);
        if (clicked >= 0 && (int)state->menu_items.size() > clicked) {
            if (state->menu_items[clicked].submenu.empty()) {
                printf("%s\n", state->menu_items[clicked].label.c_str());
                fflush(stdout);
                state->running = false;
            }
        }
    }
}

static void pointer_axis(void *, struct wl_pointer *, uint32_t, uint32_t, wl_fixed_t) {}
static void pointer_frame(void *, struct wl_pointer *) {}
static void pointer_axis_source(void *, struct wl_pointer *, uint32_t) {}
static void pointer_axis_stop(void *, struct wl_pointer *, uint32_t, uint32_t) {}
static void pointer_axis_discrete(void *, struct wl_pointer *, uint32_t, int32_t) {}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    wl_state *state = static_cast<wl_state*>(data);
    if (caps & WL_SEAT_CAPABILITY_POINTER) {
        if (!state->pointer) {
            state->pointer = wl_seat_get_pointer(seat);
            wl_pointer_add_listener(state->pointer, &pointer_listener, state);
        }
    } else {
        if (state->pointer) {
            wl_pointer_destroy(state->pointer);
            state->pointer = nullptr;
        }
    }
}
static void seat_name(void *, struct wl_seat *, const char *) {}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
#if WL_SEAT_NAME_SINCE_VERSION
    .name = seat_name,
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
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        state->seat = static_cast<struct wl_seat*>(wl_registry_bind(
            registry, name, &wl_seat_interface, 5));
        wl_seat_add_listener(state->seat, &seat_listener, state);
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

//=====================
// Rendering helpers (use geometry)
//=====================

static void render_single_menu(
    cairo_t* cr,
    const std::vector<MenuItem>& items,
    int menu_x,
    int menu_y,
    int menu_width,
    int menu_height
) {
    // Draw menu background
    cairo_set_source_rgb(cr, 0.20, 0.22, 0.28);
    cairo_rectangle(cr, menu_x, menu_y, menu_width, menu_height);
    cairo_fill(cr);

    for (const auto& item : items) {
        // Draw button background
        cairo_set_source_rgb(cr, 0.3, 0.3, 0.35);
        cairo_rectangle(cr, item.x, item.y, item.w, item.h);
        cairo_fill(cr);

        // Draw button border
        cairo_set_source_rgb(cr, 0.5, 0.5, 0.55);
        cairo_set_line_width(cr, 1.0);
        cairo_rectangle(cr, item.x, item.y, item.w, item.h);
        cairo_stroke(cr);

        // Draw text
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        PangoLayout *layout = pango_cairo_create_layout(cr);
        pango_layout_set_font_description(layout, desc);
        pango_layout_set_text(layout, item.label.c_str(), -1);

        int text_width, text_height;
        pango_layout_get_pixel_size(layout, &text_width, &text_height);

        cairo_move_to(cr, item.x + text_padding, item.y + (item.h - text_height) / 2);
        pango_cairo_show_layout(cr, layout);

        // Draw arrow for submenu
        if (!item.submenu.empty()) {
            double arrow_size = text_height * 0.5;
            double arrow_x = item.x + item.w - text_padding - arrow_size;
            double arrow_y = item.y + (item.h - arrow_size) / 2;
            cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
            cairo_move_to(cr, arrow_x, arrow_y);
            cairo_line_to(cr, arrow_x + arrow_size, arrow_y + arrow_size / 2);
            cairo_line_to(cr, arrow_x, arrow_y + arrow_size);
            cairo_close_path(cr);
            cairo_fill(cr);
        }

        g_object_unref(layout);
    }
}

static void render_menu_items(
    cairo_t* cr,
    const std::vector<MenuItem>& items,
    int logical_width,
    int menu_height,
    int hovered_index
) {
    // Draw main menu background
    cairo_set_source_rgb(cr, 0.15, 0.15, 0.15);
    cairo_rectangle(cr, 0, 0, logical_width, menu_height);
    cairo_fill(cr);

    // Render main menu items
    render_single_menu(cr, items, 0, 0, logical_width, menu_height);

    // Render submenu if hovered
    if (hovered_index >= 0 && hovered_index < (int)items.size() && !items[hovered_index].submenu.empty()) {
        // Submenu geometry
        const std::vector<MenuItem>& submenu = items[hovered_index].submenu;
        if (submenu.empty()) return;

        // Get submenu position and geometry from the first item of the submenu
        // We know measure_menu_items will have already been called recursively
        const MenuItem& parent = items[hovered_index];
        int submenu_x = parent.x + parent.w + submenu_pad;
        int submenu_y = parent.y;

        // Figure out submenu width/height (from geometry of all items)
        int min_x = submenu_x, min_y = submenu_y, max_x = submenu_x, max_y = submenu_y;
        for (const auto& it : submenu) {
            if (it.x < min_x) min_x = it.x;
            if (it.y < min_y) min_y = it.y;
            if (it.x + it.w > max_x) max_x = it.x + it.w;
            if (it.y + it.h > max_y) max_y = it.y + it.h;
        }
        int sub_width = max_x - min_x;
        int sub_height = max_y - min_y;

        render_single_menu(cr, submenu, submenu_x, submenu_y, sub_width, sub_height);
    }
}

static struct wl_buffer *create_buffer(struct wl_state *state) {
    int scale = state->chosen_scale;

    cairo_surface_t *temp_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *temp_cr = cairo_create(temp_surface);

    // Assign geometry to all menu items (including submenus)
    RenderedMenuGeometry geom = measure_menu_items(state->menu_items, temp_cr);

    int logical_width = geom.width;
    int logical_height = geom.height;

    // If a submenu might be needed, add room to the right
    int total_width = logical_width;
    int submenu_extra = 0;
    if (state->hovered_index >= 0 && state->hovered_index < (int)state->menu_items.size() &&
        !state->menu_items[state->hovered_index].submenu.empty()) {
        const std::vector<MenuItem>& submenu = state->menu_items[state->hovered_index].submenu;
        // Use the assigned geometry of the submenu
        int min_x = submenu.empty() ? 0 : submenu[0].x;
        int max_x = min_x;
        for (const auto& it : submenu) {
            if (it.x + it.w > max_x) max_x = it.x + it.w;
        }
        submenu_extra = (max_x - logical_width) + submenu_pad + 10;
        total_width += submenu_extra;
    }

    cairo_destroy(temp_cr);
    cairo_surface_destroy(temp_surface);

    state->width = total_width * scale;
    state->height = logical_height * scale;
    int stride = state->width * 4;
    int size = stride * state->height;

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

    cairo_surface_t *cairo_surface = cairo_image_surface_create_for_data(
        static_cast<unsigned char *>(data), CAIRO_FORMAT_ARGB32, state->width, state->height, stride);
    cairo_t *cr = cairo_create(cairo_surface);

    cairo_scale(cr, scale, scale);

    render_menu_items(cr, state->menu_items, logical_width, logical_height, state->hovered_index);

    cairo_destroy(cr);
    cairo_surface_destroy(cairo_surface);

    struct wl_shm_pool *pool = wl_shm_create_pool(state->shm, fd, size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(
        pool, 0, state->width, state->height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);

    munmap(data, size);
    close(fd);

    return buffer;
}

//=====================
// Menu parsing
//=====================

static void parse_menu(std::vector<MenuItem>& out) {
    std::vector<std::vector<MenuItem>*> stack;
    stack.push_back(&out);

    char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
        }

        int tabs = 0;
        while (line[tabs] == '\t') ++tabs;

        char* start = line + tabs;
        if (*start == '\0') continue;

        MenuItem item;
        item.label = std::string(start);

        while ((int)stack.size() <= tabs)
            stack.push_back(&stack.back()->back().submenu);

        while ((int)stack.size() > tabs + 1)
            stack.pop_back();

        stack.back()->push_back(item);
    }
}

//=====================
// Main
//=====================

int main() {
    struct wl_state state = {};
    state.running = true;
    state.width = 200;
    state.height = 100;
    state.chosen_output = nullptr;
    state.chosen_scale = 1;
    state.hovered_index = -1;

    parse_menu(state.menu_items);

    if (state.menu_items.empty()) {
        fprintf(stderr, "No menu items provided on stdin\n");
        return 1;
    }

    state.display = wl_display_connect(nullptr);
    if (!state.display) {
        fprintf(stderr, "Failed to connect to Wayland display\n");
        return 1;
    }

    state.registry = wl_display_get_registry(state.display);
    wl_registry_add_listener(state.registry, &registry_listener, &state);
    wl_display_roundtrip(state.display);
    wl_display_roundtrip(state.display);

    if (!state.outputs_by_name.empty()) {
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

    state.surface = wl_compositor_create_surface(state.compositor);
    wl_surface_set_buffer_scale(state.surface, state.chosen_scale);

    state.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        state.layer_shell, state.surface, state.chosen_output,
        ZWLR_LAYER_SHELL_V1_LAYER_TOP, "menu");

    zwlr_layer_surface_v1_set_size(state.layer_surface,
        state.width / state.chosen_scale, state.height / state.chosen_scale);
    zwlr_layer_surface_v1_set_anchor(state.layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
    zwlr_layer_surface_v1_add_listener(state.layer_surface, &layer_surface_listener, &state);

    wl_surface_commit(state.surface);
    wl_display_roundtrip(state.display);

    desc = pango_font_description_from_string("Sans 12");

    state.buffer = create_buffer(&state);
    if (!state.buffer) {
        fprintf(stderr, "Failed to create buffer\n");
        return 1;
    }

    wl_surface_attach(state.surface, state.buffer, 0, 0);
    wl_surface_damage_buffer(state.surface, 0, 0, state.width, state.height);
    wl_surface_commit(state.surface);

    while (state.running && wl_display_dispatch(state.display) != -1) {
        // Event loop
    }

    pango_font_description_free(desc);
    if (state.pointer) wl_pointer_destroy(state.pointer);
    if (state.seat) wl_seat_destroy(state.seat);
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
