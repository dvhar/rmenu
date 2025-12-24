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
#include <functional>
//#include "debug.cc"

#ifndef BTN_LEFT
#define BTN_LEFT 0x110
#endif

class wl_state;

struct MenuList {
    std::vector<class MenuItem> items;
    int last_rendered = 0;

    MenuItem& operator[](size_t i) { return items[i]; }
    const MenuItem& operator[](size_t i) const { return items[i]; }
    size_t size() const { return items.size(); }
    bool empty() const { return items.empty(); }
    auto begin() { return items.begin(); }
    auto end() { return items.end(); }
    auto begin() const { return items.begin(); }
    auto end() const { return items.end(); }
    auto cbegin() const { return items.cbegin(); }
    auto cend() const { return items.cend(); }
};

struct wl_output_data {
    struct wl_output *output;
    int32_t scale;
    uint32_t name;
};

class wl_state {
  public:
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wl_buffer *buffer;

    MenuList menu;
    bool running;
    int width;
    int height;
    int current_frame = 0;

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

    std::vector<int> find_hovered_path();
    std::vector<int> find_submenu_path();
    bool handle_menu_click(MenuList& menu_list);
};

class MenuItem {
  public:
    std::string label;
    std::string output;
    MenuList submenu;
    wl_state* state;
    int last_rendered = 0;
    int x = 0, y = 0, w = 0, h = 0;
    bool is_separator = false;
    int min_x() const { return x; }
    int max_x() const { return x+w; }
    int min_y() const { return y; }
    int max_y() const { return y+h; }
    bool in_x(int px) const { return px >= x && px <= max_x(); }
    bool in_y(int py) const { return py >= y && py <= max_y(); }
    bool in_box() const { return !is_separator &&
      last_rendered == state->current_frame &&
      in_x(state->pointer_x) && in_y(state->pointer_y); }
};


PangoFontDescription *desc;
const int button_height = 30;
const int button_spacing = 5;
const int text_padding = 50;
const int min_width = 100;
const int separator_size = 4;

static void output_geometry(void*, struct wl_output*, int, int, int, int, int, const char*, const char*, int) {}
static void output_mode(void*, struct wl_output*, uint32_t, int, int, int) {}
static void output_done(void*, struct wl_output*) {}
static void output_name(void*, struct wl_output*, const char*) {}
static void output_description(void*, struct wl_output*, const char*) {}

static struct wl_buffer *create_buffer(wl_state *state);

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

std::tuple<int,int,int,int> submenu_geometry(const MenuList& submenu) {
  if (submenu.empty()) {
    return {0,0,0,0};
  }
  return {submenu[0].min_x(), submenu[0].min_y(), submenu[submenu.size()-1].max_x(), submenu[submenu.size()-1].max_y()};
}

std::vector<int> wl_state::find_submenu_path() {
    std::vector<int> path;
    const MenuList* current = &menu;
    while (true) {
        bool found = false;
        for (size_t i = 0; i < current->size(); ++i) {
            const auto& item = (*current)[i];
            if (!item.submenu.empty() && current_frame == item.submenu.last_rendered) {
                auto [min_x, min_y, max_x, max_y] = submenu_geometry(item.submenu);
                if (pointer_x >= min_x && pointer_x <= max_x && pointer_y >= min_y && pointer_y <= max_y) {
                    path.push_back(i);
                    current = &item.submenu;
                    found = true;
                    break;
                }
            }
        }
        if (!found) break;
    }
    return path;
}

std::vector<int> wl_state::find_hovered_path() {
    std::vector<int> path;
    std::function<bool(const MenuList&, std::vector<int>&)> helper;
    helper = [&](const MenuList& menu_list, std::vector<int>& path) -> bool {
        for (size_t i = 0; i < menu_list.size(); ++i) {
            const MenuItem& item = menu_list[i];
            if (item.in_box()) {
                path.push_back((int)i);
                return true;
            }
        }
        for (size_t i = 0; i < menu_list.size(); ++i) {
            const MenuItem& item = menu_list[i];
            if (!item.submenu.empty()) {
                std::vector<int> subpath;
                if (helper(item.submenu, subpath)) {
                    path.push_back((int)i);
                    path.insert(path.end(), subpath.begin(), subpath.end());
                    return true;
                }
            }
        }
        return false;
    };
    helper(menu, path);
    return path;
}

// Recursive geometry assignment
static RenderedMenuGeometry measure_menu_items(
    MenuList& menu_list,
    cairo_t* cr,
    int base_x = 0,
    int base_y = 0
) {
    int max_text_width = 0;
    std::vector<std::pair<int, int>> text_sizes;

    for (auto& item : menu_list) {
        if (item.is_separator) {
            text_sizes.push_back({0, 0});
            continue;
        }
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

    // Compute the y position for each item, accounting for separators
    int y = base_y;
    for (size_t i = 0; i < menu_list.size(); ++i) {
        auto& item = menu_list[i];
        item.x = base_x;
        item.y = y;
        if (item.is_separator) {
            item.w = logical_width;
            item.h = separator_size/2;
        } else {
            item.w = logical_width;
            item.h = button_height;
        }
        y += item.h + button_spacing;
    }

    int logical_height = y - base_y - button_spacing;

    // Recursively assign for submenus
    for (size_t i = 0; i < menu_list.size(); ++i) {
        if (!menu_list[i].submenu.empty()) {
            cairo_surface_t *temp_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
            cairo_t *temp_cr = cairo_create(temp_surface);
            measure_menu_items(
                menu_list[i].submenu,
                temp_cr,
                base_x + logical_width, // right of this menu
                menu_list[i].y // vertical position aligned with item
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

bool wl_state::handle_menu_click(MenuList& menu_list) {
    for (auto& item : menu_list) {
        if (item.is_separator) continue;
        if (item.in_box() && item.submenu.empty()) {
            if (item.output.empty()) {
              printf("%s\n", item.label.c_str());
            } else {
              printf("%s\n", item.output.c_str());
            }
            fflush(stdout);
            running = false;
            return true;
        } else if (!item.submenu.empty() && handle_menu_click(item.submenu)) {
            return true;
        }
    }
    return false;
}

//=====================
// Pointer event listeners
//=====================

static void pointer_motion(void *data, struct wl_pointer *, uint32_t, wl_fixed_t sx, wl_fixed_t sy) {
    wl_state *state = static_cast<wl_state*>(data);
    state->pointer_x = wl_fixed_to_double(sx);
    state->pointer_y = wl_fixed_to_double(sy);

    auto new_hovered_path = state->find_hovered_path();
    if (state->hovered_path != new_hovered_path && new_hovered_path.size()) {
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

    auto new_hovered_path = state->find_hovered_path();
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
        state->handle_menu_click(state->menu);
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
    .axis_value120 = nullptr,
    .axis_relative_direction = 0
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
    MenuList& menu_list,
    wl_state* state,
    size_t level
) {
    if (menu_list.empty()) return;
    menu_list.last_rendered = state->current_frame;
    auto& hovered_path = state->hovered_path;

    // Compute bounding rect for this menu
    int min_x = menu_list[0].x, min_y = menu_list[0].y, max_x = menu_list[0].x + menu_list[0].w, max_y = menu_list[0].y + menu_list[0].h;
    for (const auto& it : menu_list) {
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
    for (size_t i = 0; i < menu_list.size(); ++i) {
        auto& item = menu_list[i];
        item.last_rendered = state->current_frame;

        if (item.is_separator) {
             //Draw horizontal line in the center of the separator box
            double sep_y = item.y;
            cairo_set_source_rgb(cr, 0.5, 0.5, 0.55); // color for separator
            cairo_set_line_width(cr, separator_size);
            cairo_move_to(cr, item.x + 5, sep_y);
            cairo_line_to(cr, item.x + item.w - 5, sep_y);
            cairo_stroke(cr);
            continue;
        }

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
        if (idx >= 0 && idx < (int)menu_list.size() && !menu_list[idx].submenu.empty()) {
            render_menu_branch(cr, menu_list[idx].submenu, state, level + 1);
        }
    } else if (auto sm_path = state->find_submenu_path(); sm_path.size() > level) {
        int idx = sm_path[level];
        if (idx >= 0 && idx < (int)menu_list.size() && !menu_list[idx].submenu.empty()) {
            render_menu_branch(cr, menu_list[idx].submenu, state, level + 1);
        }
    }
}
static void render_menu_items(
    cairo_t* cr,
    wl_state* state
) {
    state->current_frame++;
    render_menu_branch(cr, state->menu, state, 0);
}

static struct wl_buffer *create_buffer(wl_state *state) {
    int scale = state->chosen_scale;

    cairo_surface_t *temp_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *temp_cr = cairo_create(temp_surface);

    // Assign geometry to all menu items (including submenus)
    RenderedMenuGeometry geom = measure_menu_items(state->menu, temp_cr);

    int logical_width = geom.width;
    int logical_height = geom.height;

    // Compute max right and bottom edge for all open menu levels
    int total_width = logical_width;
    int total_height = logical_height;

    // Traverse the hovered_path to determine the furthest right/bottom coordinate
    const MenuList *current = &state->menu;
    int level = 0;

    // Start with main menu bounding box
    int max_x = logical_width, max_y = logical_height;

    while (state->hovered_path.size() > (size_t)level) {
        int idx = state->hovered_path[level];
        if (idx < 0 || idx >= (int)current->size())
            break;
        const MenuItem &item = (*current)[idx];
        if (item.submenu.empty())
            break;

        // Compute bounding box for this submenu
        const MenuList &submenu = item.submenu;
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

        current = &item.submenu;
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

    render_menu_items(cr, state);

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

static void parse_menu(wl_state* state) {
    std::vector<MenuList*> stack;
    stack.push_back(&state->menu);
    bool prev_was_empty = false;
    char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
        }

        int tabs = 0;
        while (line[tabs] == '\t') ++tabs;

        char* start = line + tabs;

        // Handle empty line: add a separator
        if (*start == '\0') {
            prev_was_empty = true;
            MenuItem sep;
            sep.state = state;
            sep.is_separator = true;
            sep.submenu = MenuList();
            while ((int)stack.size() <= tabs)
                stack.push_back(&stack.back()->items.back().submenu);
            while ((int)stack.size() > tabs + 1)
                stack.pop_back();
            stack.back()->items.push_back(sep);
            continue;
        }
        if (tabs > 0 && prev_was_empty) {
            fprintf(stderr, "No separators in submenus\n");
            exit(1);
        }

        MenuItem item;
        item.state = state;

        char* midtab = strchr(start, '\t');
        if (midtab) {
            // Split into label and output
            *midtab = '\0';
            item.label = std::string(start);
            item.output = std::string(midtab + 1);
        } else {
            item.label = std::string(start);
        }
        item.submenu = MenuList();

        while ((int)stack.size() <= tabs)
            stack.push_back(&stack.back()->items.back().submenu);

        while ((int)stack.size() > tabs + 1)
            stack.pop_back();

        stack.back()->items.push_back(item);
        prev_was_empty = false;
    }
}

//=====================
// Main
//=====================

int main() {
    wl_state state = {};
    state.running = true;
    state.width = min_width;
    state.height = 100;
    state.chosen_output = nullptr;
    state.chosen_scale = 1;

    parse_menu(&state);

    if (state.menu.empty()) {
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
