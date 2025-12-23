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

    // Hover handling
    std::vector<int> hovered_path;
};

PangoFontDescription *desc;
const int button_height = 40;
const int button_spacing = 5;
const int text_padding = 10;
const int min_width = 200;
const int submenu_pad = 0;

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

// Returns the path through the menu tree that should be open, given pointer coords
static std::vector<int> find_hovered_path(const std::vector<MenuItem>& items, int px, int py) {
    std::vector<int> path;
    const std::vector<MenuItem>* current = &items;
    int x = px, y = py;

    while (true) {
        int found = -1;
        for (size_t i = 0; i < current->size(); ++i) {
            const auto& item = (*current)[i];
            // If the pointer is inside this item, descend into its submenu if present
            if (x >= item.x && x < item.x + item.w &&
                y >= item.y && y < item.y + item.h) {
                found = (int)i;
                break;
            }
        }
        if (found == -1) {
            // If not inside any item at this level, but our parent item had a submenu, check that
            // If the pointer is inside any submenu at this level, descend into it
            // (This part is new!)
            bool in_submenu = false;
            for (size_t i = 0; i < current->size(); ++i) {
                const auto& item = (*current)[i];
                if (!item.submenu.empty()) {
                    // Check all submenu items for pointer hit
                    for (const auto& subitem : item.submenu) {
                        if (x >= subitem.x && x < subitem.x + subitem.w &&
                            y >= subitem.y && y < subitem.y + subitem.h) {
                            path.push_back((int)i);
                            current = &item.submenu;
                            found = -2; // found in submenu
                            in_submenu = true;
                            break;
                        }
                    }
                }
                if (in_submenu) break;
            }
            if (!in_submenu) break;
            // else, continue the while loop for the submenu
        } else {
            path.push_back(found);
            // go deeper if submenu exists
            const auto& item = (*current)[found];
            if (item.submenu.empty())
                break;
            current = &item.submenu;
            // continue the while loop
        }
    }
    return path;
}

static bool pointer_in_submenu(const std::vector<MenuItem>& items, int hovered_index, int px, int py) {
    if (hovered_index < 0 || hovered_index >= (int)items.size() || items[hovered_index].submenu.empty())
        return false;
    const auto& submenu = items[hovered_index].submenu;
    if (submenu.empty()) return false;

    int min_x = submenu[0].x, min_y = submenu[0].y;
    int max_x = submenu[0].x + submenu[0].w, max_y = submenu[0].y + submenu[0].h;
    for (const auto& it : submenu) {
        if (it.x < min_x) min_x = it.x;
        if (it.y < min_y) min_y = it.y;
        if (it.x + it.w > max_x) max_x = it.x + it.w;
        if (it.y + it.h > max_y) max_y = it.y + it.h;
    }
    return px >= min_x && px < max_x && py >= min_y && py < max_y;
}

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

    int logical_width = max_text_width;
    if (logical_width < min_width) logical_width = min_width;
    int logical_height = items.size() * (button_height + button_spacing) - button_spacing;

    // Assign geometry to each MenuItem
    for (size_t i = 0; i < items.size(); ++i) {
        items[i].x = base_x;
        items[i].y = base_y + i * (button_height + button_spacing);
        items[i].w = logical_width;
        items[i].h = button_height;
    }

    // Recursively assign for submenus
    for (size_t i = 0; i < items.size(); ++i) {
        if (!items[i].submenu.empty()) {
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

static void pointer_motion(void *data, struct wl_pointer *, uint32_t, wl_fixed_t sx, wl_fixed_t sy) {
    wl_state *state = static_cast<wl_state*>(data);
    state->pointer_x = wl_fixed_to_double(sx);
    state->pointer_y = wl_fixed_to_double(sy);

    auto new_hovered_path = find_hovered_path(state->menu_items, state->pointer_x, state->pointer_y);
    if (state->hovered_path != new_hovered_path) {
        state->hovered_path = std::move(new_hovered_path);
        if (state->buffer) wl_buffer_destroy(state->buffer);
        state->buffer = create_buffer(state);
        wl_surface_attach(state->surface, state->buffer, 0, 0);
        wl_surface_damage_buffer(state->surface, 0, 0, state->width, state->height);
        wl_surface_commit(state->surface);
    }
}

static void pointer_enter(void *data, struct wl_pointer *, uint32_t, struct wl_surface *, wl_fixed_t sx, wl_fixed_t sy) {
    wl_state *state = static_cast<wl_state*>(data);
    state->pointer_inside = true;
    state->pointer_x = wl_fixed_to_double(sx);
    state->pointer_y = wl_fixed_to_double(sy);

    auto new_hovered_path = find_hovered_path(state->menu_items, state->pointer_x, state->pointer_y);
    if (state->hovered_path != new_hovered_path) {
        state->hovered_path = std::move(new_hovered_path);
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
    if (!state->hovered_path.empty()) {
        state->hovered_path.clear();
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

// Helper: recursively render menus/submenus along hovered_path
static void render_menu_branch(
    cairo_t* cr,
    const std::vector<MenuItem>& items,
    const std::vector<int>& hovered_path,
    size_t level
) {
    if (items.empty()) return;

    // Compute bounding rect for this menu
    int min_x = items[0].x, min_y = items[0].y, max_x = items[0].x + items[0].w, max_y = items[0].y + items[0].h;
    for (const auto& it : items) {
        if (it.x < min_x) min_x = it.x;
        if (it.y < min_y) min_y = it.y;
        if (it.x + it.w > max_x) max_x = it.x + it.w;
        if (it.y + it.h > max_y) max_y = it.y + it.h;
    }
    int menu_width = max_x - min_x;
    int menu_height = max_y - min_y;

    // Draw menu background
    cairo_set_source_rgb(cr, 0.20, 0.22, 0.28);
    cairo_rectangle(cr, min_x, min_y, menu_width, menu_height);
    cairo_fill(cr);

    // Draw all menu items
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& item = items[i];

        // Highlight hovered item at this level
        bool is_hovered = (hovered_path.size() > level && hovered_path[level] == (int)i);

        // Button background color
        if (is_hovered)
            cairo_set_source_rgb(cr, 0.35, 0.35, 0.40);
        else
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

    // If a submenu should be open, recursively render it
    if (hovered_path.size() > level) {
        int idx = hovered_path[level];
        if (idx >= 0 && idx < (int)items.size() && !items[idx].submenu.empty()) {
            render_menu_branch(cr, items[idx].submenu, hovered_path, level + 1);
        }
    }
}

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
    int /*logical_width*/, int /*menu_height*/,
    const std::vector<int>& hovered_path
) {
    render_menu_branch(cr, items, hovered_path, 0);
}

static struct wl_buffer *create_buffer(struct wl_state *state) {
      int scale = state->chosen_scale;

    cairo_surface_t *temp_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *temp_cr = cairo_create(temp_surface);

    // Assign geometry to all menu items (including submenus)
    RenderedMenuGeometry geom = measure_menu_items(state->menu_items, temp_cr);

    int logical_width = geom.width;
    int logical_height = geom.height;

    // Compute max right and bottom edge for all open menu levels
    int total_width = logical_width;
    int total_height = logical_height;

    // Traverse the hovered_path to determine the furthest right/bottom coordinate
    const std::vector<MenuItem> *current_items = &state->menu_items;
    int level = 0;
    int submenu_pad = 0; // Set to your submenu_pad if needed

    // Start with main menu bounding box
    int min_x = 0, min_y = 0, max_x = logical_width, max_y = logical_height;

    while (state->hovered_path.size() > (size_t)level) {
        int idx = state->hovered_path[level];
        if (idx < 0 || idx >= (int)current_items->size())
            break;
        const MenuItem &item = (*current_items)[idx];
        if (item.submenu.empty())
            break;

        // Compute bounding box for this submenu
        const auto &submenu = item.submenu;
        if (!submenu.empty()) {
            int sub_min_x = submenu[0].x, sub_min_y = submenu[0].y;
            int sub_max_x = submenu[0].x + submenu[0].w, sub_max_y = submenu[0].y + submenu[0].h;
            for (const auto &it : submenu) {
                if (it.x < sub_min_x) sub_min_x = it.x;
                if (it.y < sub_min_y) sub_min_y = it.y;
                if (it.x + it.w > sub_max_x) sub_max_x = it.x + it.w;
                if (it.y + it.h > sub_max_y) sub_max_y = it.y + it.h;
            }
            if (sub_max_x > max_x) max_x = sub_max_x;
            if (sub_max_y > max_y) max_y = sub_max_y;
        }

        current_items = &item.submenu;
        ++level;
    }

    total_width = max_x;
    total_height = max_y;

    cairo_destroy(temp_cr);
    cairo_surface_destroy(temp_surface);

    state->width = total_width * scale;
    state->height = total_height * scale;
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

    render_menu_items(cr, state->menu_items, logical_width, logical_height, state->hovered_path);

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
