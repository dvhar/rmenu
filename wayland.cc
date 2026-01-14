#include "rmenu.h"

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

void registry_global(void *data, struct wl_registry *registry,
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

void registry_global_remove(void *data, struct wl_registry *, uint32_t name) {
    wl_state *state = static_cast<wl_state *>(data);
    auto it = state->outputs_by_name.find(name);
    if (it != state->outputs_by_name.end()) {
        wl_output_destroy(it->second.output);
        state->outputs_by_name.erase(it);
    }
}

void layer_surface_configure(void *, struct zwlr_layer_surface_v1 *layer_surface,
                                   uint32_t serial, uint32_t, uint32_t) {
    zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
}

void layer_surface_closed(void *data,
                                struct zwlr_layer_surface_v1 *) {
    wl_state *state = static_cast<wl_state *>(data);
    state->running = false;
}

// transparent background layer implements close-on-click-away
void bg_layer_surface_configure(void *data,
                                      struct zwlr_layer_surface_v1 *layer_surface,
                                      uint32_t serial, uint32_t width, uint32_t height) {
    zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
}
void bg_layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *) {
    wl_state *state = static_cast<wl_state *>(data);
    state->running = false;
}
