#include "rmenu.h"

INCBIN(wood, "wood3.png");
#define RGB(arr) arr[0], arr[1], arr[2]

class wl_state;


PangoFontDescription *desc;
cairo_surface_t *wood_texture = nullptr;


struct RenderedMenuGeometry {
    int width;
    int height;
};

std::tuple<int,int,int,int> menu_geometry(const MenuList& submenu) {
  if (submenu.empty()) {
    return {0,0,0,0};
  }
  return {submenu[0].x, submenu[0].y, submenu[submenu.size()-1].max_x(), submenu[submenu.size()-1].max_y()};
}

std::vector<int> wl_state::find_submenu_path() {
    std::vector<int> path;
    const MenuList* current = &menu;
    while (true) {
        bool found = false;
        for (size_t i = 0; i < current->size(); ++i) {
            const auto& item = (*current)[i];
            if (!item.submenu.empty() && current_frame == item.submenu.last_rendered) {
                auto [min_x, min_y, max_x, max_y] = menu_geometry(item.submenu);
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


static RenderedMenuGeometry measure_menu_items(
    MenuList& menu_list,
    cairo_t* cr,
    int base_x = 0,
    int base_y = 0
) {
    menu_list.has_icons = false;
    int max_text_width = 0;

    // First pass: check if any item has an icon & measure text widths
    for (const auto& item : menu_list) {
        if (item.is_separator) continue;

        PangoLayout *layout = pango_cairo_create_layout(cr);
        pango_layout_set_font_description(layout, desc);
        pango_layout_set_text(layout, item.label.c_str(), -1);

        int text_width, text_height;
        pango_layout_get_pixel_size(layout, &text_width, &text_height);
        g_object_unref(layout);

        int item_content_width = text_width + 2 * text_padding;

        if (item.icon_surface) {
            menu_list.has_icons = true;
            item_content_width += icon_size + icon_text_gap;
        }

        if (!item.submenu.empty()) {
            item_content_width += 20; // arrow
        }

        if (item_content_width > max_text_width) {
            max_text_width = item_content_width;
        }
    }

    int logical_width = std::max(max_text_width, min_width);

    // If this menu has any icons → increase left padding for ALL items
    menu_list.text_left_padding = text_padding;
    if (menu_list.has_icons) {
        menu_list.text_left_padding = icon_size + icon_text_gap + icon_left_pad;
    }

    // Second pass: assign geometry
    int y = base_y;
    for (auto& item : menu_list) {
        item.x = base_x;
        item.y = y;

        if (item.is_separator) {
            item.w = logical_width;
            item.h = separator_size;
        } else {
            item.w = logical_width;
            item.h = button_height;
        }

        y += item.h + button_spacing;
    }

    int logical_height = y - base_y - button_spacing;

    // Recurse into submenus
    for (auto& item : menu_list) {
        if (!item.submenu.empty()) {
            cairo_surface_t *temp_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
            cairo_t *temp_cr = cairo_create(temp_surface);
            measure_menu_items(item.submenu, temp_cr,
                               base_x + logical_width,  // right of current menu
                               item.y);                 // aligned with item
            cairo_destroy(temp_cr);
            cairo_surface_destroy(temp_surface);
        }
    }

    return {logical_width, logical_height};
}

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
    int min_x = menu_list[0].x, min_y = menu_list[0].y, max_x = menu_list[0].max_x(), max_y = menu_list[0].max_y();
    for (const auto& it : menu_list) {
        min_x = std::min(min_x, it.x);
        min_y = std::min(min_y, it.y);
        max_x = std::max(max_x, it.max_x());
        max_y = std::max(max_y, it.max_y());
    }
    int menu_width = max_x - min_x;
    int menu_height = max_y - min_y;

    // Draw menu background
    cairo_set_source_rgb(cr, RGB(menu_back));
    cairo_rectangle(cr, min_x, min_y, menu_width, menu_height);
    cairo_fill(cr);

    // Draw all menu items
    for (size_t i = 0; i < menu_list.size(); ++i) {
        auto& item = menu_list[i];
        item.last_rendered = state->current_frame;

        if (item.is_separator) {
            // Draw horizontal line in the center of the separator box
            double sep_y = item.y + separator_size / 2.0;
            cairo_set_source_rgb(cr, RGB(sep_color));
            cairo_set_line_width(cr, separator_size);
            cairo_move_to(cr, item.x + 5, sep_y);
            cairo_line_to(cr, item.x + item.w - 5, sep_y);
            cairo_stroke(cr);
            continue;
        }

        // Highlight hovered item at this level
        bool is_hovered = (hovered_path.size() > level && hovered_path[level] == (int)i);

        // === Draw item background ===
        if (wood_texture) {
            cairo_save(cr);
            cairo_translate(cr, item.x, item.y);
            double sx = (double)item.w / cairo_image_surface_get_width(wood_texture);
            double sy = (double)item.h / cairo_image_surface_get_height(wood_texture);
            cairo_scale(cr, sx, sy);
            cairo_set_source_surface(cr, wood_texture, 0, 0);
            cairo_rectangle(cr, 0, 0, cairo_image_surface_get_width(wood_texture), cairo_image_surface_get_height(wood_texture));
            cairo_fill(cr);

            if (is_hovered) {
                // Overlay translucent yellowish highlight on hover
                cairo_set_source_rgba(cr, 1.0, 1.0, 0.7, 0.25);
                cairo_rectangle(cr, 0, 0, cairo_image_surface_get_width(wood_texture), cairo_image_surface_get_height(wood_texture));
                cairo_fill(cr);
            }
            cairo_restore(cr);
        } else {
            // Fallback gradient background
            cairo_pattern_t *pat = cairo_pattern_create_linear(item.x, item.y, item.x + item.w, item.y);
            if (is_hovered) {
                cairo_pattern_add_color_stop_rgb(pat, 0.0, RGB(hovered_grad_left));
                cairo_pattern_add_color_stop_rgb(pat, 1.0, RGB(hovered_grad_right));
            } else {
                cairo_pattern_add_color_stop_rgb(pat, 0.0, RGB(button_grad_left));
                cairo_pattern_add_color_stop_rgb(pat, 1.0, RGB(button_grad_right));
            }
            cairo_rectangle(cr, item.x, item.y, item.w, item.h);
            cairo_set_source(cr, pat);
            cairo_fill(cr);
            cairo_pattern_destroy(pat);
        }

        if (border_enabled) {
          // Draw button border
          cairo_set_source_rgb(cr, RGB(border_color));
          cairo_set_line_width(cr, 1.0);
          cairo_rectangle(cr, item.x, item.y, item.w, item.h);
          cairo_stroke(cr);
        }

        // === Draw icon (if present) ===
        if (item.icon_surface && item.icon_surface != nullptr) {
            double icon_y = item.y + (item.h - icon_size) / 2.0;

            cairo_save(cr);
            // Start icon at the very left edge of the item (no text_padding!)
            cairo_translate(cr, item.x + icon_left_pad, icon_y);

            // Scale to ICON_SIZE
            double sx = (double)icon_size / item.icon_width;
            double sy = (double)icon_size / item.icon_height;
            cairo_scale(cr, sx, sy);

            cairo_set_source_surface(cr, item.icon_surface, 0, 0);
            cairo_paint(cr);
            cairo_restore(cr);
        }

        // === Draw text ===
        cairo_set_source_rgb(cr, RGB(text_color));
        PangoLayout *layout = pango_cairo_create_layout(cr);
        pango_layout_set_font_description(layout, desc);
        pango_layout_set_text(layout, item.label.c_str(), -1);

        int text_width, text_height;
        pango_layout_get_pixel_size(layout, &text_width, &text_height);

        cairo_move_to(cr,
                      item.x + menu_list.text_left_padding,
                      item.y + (item.h - text_height) / 2.0);

        pango_cairo_show_layout(cr, layout);

        // === Draw arrow for submenu (if present) ===
        if (!item.submenu.empty()) {
            double arrow_size = text_height * 0.5;
            double arrow_margin = 4.0;
            double arrow_x = item.x + item.w - arrow_size - arrow_margin;
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

static struct wl_buffer *create_transparent_buffer(wl_state *state, int width, int height) {
    int stride = width * 4;
    int size = stride * height;

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

    memset(data, 0, size);

    struct wl_shm_pool *pool = wl_shm_create_pool(state->shm, fd, size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(
        pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);

    munmap(data, size);
    close(fd);

    return buffer;
}

struct wl_buffer *create_buffer(wl_state *state) {
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
            int sub_max_x = submenu[0].max_x(), sub_max_y = submenu[0].max_y();
            for (const auto &it : submenu) {
                sub_min_x = std::min(sub_min_x, it.x);
                sub_min_y = std::min(sub_min_y, it.y);
                sub_max_x = std::max(sub_max_x, it.max_x());
                sub_max_y = std::max(sub_max_y, it.max_y());
            }
            max_x = std::max(max_x, sub_max_x);
            max_y = std::max(max_y, sub_max_y);
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

static void parse_menu(wl_state* state) {
    std::vector<MenuList*> stack;
    stack.push_back(&state->menu);
    bool prev_was_empty = false;
    char line[1024];
    while (fgets(line, sizeof(line), stdin)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
        }

        int tabs = strspn(line, "\t");
        char* start = line + tabs;

        if (*start == '#') {
          continue;
        }
        // Handle empty line: add a separator
        if (*start == '\0') {
            prev_was_empty = true;
            MenuItem sep;
            sep.state = state;
            sep.is_separator = true;
            stack[0]->items.push_back(std::move(sep));
            continue;
        }
        if (tabs > 0 && prev_was_empty) {
            fprintf(stderr, "No separators in submenus\n");
            exit(1);
        }

        MenuItem item;
        item.state = state;

        if (strncmp(start, "IMG:", 4) == 0) {
            start += 4;
            char* tab = strchr(start, '\t');
            if (!tab) {
                fprintf(stderr, "Missing tab after icon\n");
                exit(1);
            }
            item.file = std::string(start, tab - start);
            start = tab+1;

            cairo_surface_t* icon_surf = cairo_image_surface_create_from_png(item.file.c_str());
            if (cairo_surface_status(icon_surf) != CAIRO_STATUS_SUCCESS) {
                fprintf(stderr, "Failed to load icon: %s\n", item.file.c_str());
                cairo_surface_destroy(icon_surf);
                icon_surf = nullptr;
            } else {
                item.icon_surface = icon_surf;
                item.icon_width  = cairo_image_surface_get_width(icon_surf);
                item.icon_height = cairo_image_surface_get_height(icon_surf);
            }
        }

        char* midtab = strchr(start, '\t');
        if (midtab) {
            *midtab = '\0';
            item.label = std::string(start);
            item.output = std::string(midtab + 1);
        } else {
            item.label = std::string(start);
        }

        while ((int)stack.size() <= tabs)
            stack.push_back(&stack.back()->items.back().submenu);

        while ((int)stack.size() > tabs + 1)
            stack.pop_back();

        stack.back()->items.push_back(std::move(item));
        prev_was_empty = false;
    }
}

struct mem_png {
    const unsigned char *data;
    unsigned int len;
    unsigned int pos;
};

static cairo_status_t
read_png_from_mem(void *closure, unsigned char *data, unsigned int length)
{
    // closure is a pointer to a struct holding the array and a read offset
    struct mem_png *mem = (struct mem_png *)closure;

    if (mem->pos + length > mem->len)
        return CAIRO_STATUS_READ_ERROR;

    memcpy(data, mem->data + mem->pos, length);
    mem->pos += length;
    return CAIRO_STATUS_SUCCESS;
}

static void destroy_menu_icons(MenuList& menu_list) {
    for (auto& item : menu_list) {
        if (item.icon_surface) {
            cairo_surface_destroy(item.icon_surface);
            item.icon_surface = nullptr;
        }
        if (!item.submenu.empty()) {
            destroy_menu_icons(item.submenu);
        }
    }
}

int main(int argc, char** argv) {
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

    // Create background layer (transparent/full screen)
    state.bg_surface = wl_compositor_create_surface(state.compositor);
    state.bg_layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        state.layer_shell, state.bg_surface, nullptr,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "menu-bg");

    zwlr_layer_surface_v1_set_anchor(state.bg_layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_keyboard_interactivity(state.bg_layer_surface, 0);
    zwlr_layer_surface_v1_add_listener(state.bg_layer_surface, &bg_layer_surface_listener, &state);

    wl_surface_commit(state.bg_surface);
    wl_display_roundtrip(state.display);

    // Use a reasonable default size for now; you may want to track this from configure
    state.bg_buffer = create_transparent_buffer(&state, 1920, 1080);
    if (!state.bg_buffer) {
        fprintf(stderr, "Failed to create background buffer\n");
        return 1;
    }
    wl_surface_attach(state.bg_surface, state.bg_buffer, 0, 0);
    wl_surface_commit(state.bg_surface);

    // Create a pointer for the background (click-away)
    state.bg_pointer = wl_seat_get_pointer(state.seat);
    wl_pointer_add_listener(state.bg_pointer, &bg_pointer_listener, &state);

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

    desc = pango_font_description_from_string(font);
    wood_texture = cairo_image_surface_create_from_png("wood1.png");
    if (cairo_surface_status(wood_texture) != CAIRO_STATUS_SUCCESS) {
      fprintf(stderr, "Failed to load wood texture\n");
      wood_texture = nullptr;
    }
    mem_png mem = { gwoodData, gwoodSize, 0 };
    wood_texture = cairo_image_surface_create_from_png_stream(read_png_from_mem, &mem);
    if (cairo_surface_status(wood_texture) != CAIRO_STATUS_SUCCESS) {
      fprintf(stderr, "Failed to load embedded wood texture\n");
    }

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
    destroy_menu_icons(state.menu);
    if (state.bg_pointer) wl_pointer_destroy(state.bg_pointer);
    if (state.bg_buffer) wl_buffer_destroy(state.bg_buffer);
    if (state.bg_layer_surface) zwlr_layer_surface_v1_destroy(state.bg_layer_surface);
    if (state.bg_surface) wl_surface_destroy(state.bg_surface);
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
