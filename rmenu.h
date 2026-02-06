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
#define namespace namespace_
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace
}

#include <vector>
#include <string>
#include <map>
#include <functional>
#include "config.h"
#include "incbin.h"

#ifndef BTN_LEFT
#define BTN_LEFT 0x110
#endif


struct MenuList {
    std::vector<class MenuItem> items;
    int last_rendered = 0;
    bool has_icons = false;
    int text_left_padding = text_padding;

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

    // For click-away background layer
    struct wl_surface *bg_surface = nullptr;
    struct zwlr_layer_surface_v1 *bg_layer_surface = nullptr;
    struct wl_buffer *bg_buffer = nullptr;
    struct wl_pointer *bg_pointer = nullptr;

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
    std::string file;
    cairo_surface_t* icon_surface = nullptr;
    MenuList submenu;
    wl_state* state;
    int icon_width;
    int icon_height;
    int last_rendered = 0;
    int x = 0, y = 0, w = 0, h = 0;
    bool is_separator = false;
    int max_x() const { return x+w; }
    int max_y() const { return y+h; }
    bool in_x(int px) const { return px >= x && px <= max_x(); }
    bool in_y(int py) const { return py >= y && py <= max_y(); }
    bool in_box() const { return !is_separator &&
      last_rendered == state->current_frame &&
      in_x(state->pointer_x) && in_y(state->pointer_y); }
};

struct wl_buffer *create_buffer(wl_state *state);
void registry_global(void *data, struct wl_registry *registry,
                           uint32_t name, const char *interface, uint32_t version);
void registry_global_remove(void *data, struct wl_registry *, uint32_t name);
void bg_layer_surface_configure(void *data,
                                      struct zwlr_layer_surface_v1 *layer_surface,
                                      uint32_t serial, uint32_t width, uint32_t height);
void bg_layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *);
void layer_surface_configure(void *, struct zwlr_layer_surface_v1 *layer_surface,
                                   uint32_t serial, uint32_t, uint32_t);

void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *);

const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static const struct zwlr_layer_surface_v1_listener bg_layer_surface_listener = {
    .configure = bg_layer_surface_configure,
    .closed = bg_layer_surface_closed,
};

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

static void bg_pointer_enter(void *data, struct wl_pointer *, uint32_t, struct wl_surface *, wl_fixed_t, wl_fixed_t) {}
static void bg_pointer_leave(void *data, struct wl_pointer *, uint32_t, struct wl_surface *) {}
static void bg_pointer_motion(void *data, struct wl_pointer *, uint32_t, wl_fixed_t, wl_fixed_t) {}
static void bg_pointer_axis(void *data, struct wl_pointer *, uint32_t, uint32_t, wl_fixed_t) {}
static void bg_pointer_button(void *data, struct wl_pointer *, uint32_t, uint32_t, uint32_t, uint32_t state_wl) {
    wl_state *state = static_cast<wl_state *>(data);
    if (state_wl == WL_POINTER_BUTTON_STATE_PRESSED) {
        state->running = false;
    }
}
static void bg_pointer_frame(void *, struct wl_pointer *) {}
static void bg_pointer_axis_source(void *, struct wl_pointer *, uint32_t) {}
static void bg_pointer_axis_stop(void *, struct wl_pointer *, uint32_t, uint32_t) {}
static void bg_pointer_axis_discrete(void *, struct wl_pointer *, uint32_t, int32_t) {}
static const struct wl_pointer_listener bg_pointer_listener = {
    .enter = bg_pointer_enter,
    .leave = bg_pointer_leave,
    .motion = bg_pointer_motion,
    .button = bg_pointer_button,
    .axis = bg_pointer_axis,
    .frame = bg_pointer_frame,
    .axis_source = bg_pointer_axis_source,
    .axis_stop = bg_pointer_axis_stop,
    .axis_discrete = bg_pointer_axis_discrete,
    .axis_value120 = nullptr,
    .axis_relative_direction = 0,
};
