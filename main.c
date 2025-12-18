#define _POSIX_C_SOURCE 200809L
#include <wayland-server.h>
#include <wlr/backend.h>
#include <wlr/backend/wayland.h>
#include <wlr/backend/headless.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_surface.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/util/log.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/render/wlr_allocator.h>
#include <wlr/render/allocator/auto.h>

#include <pixman.h>
#include <fcft/fcft.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MAX_LINES 128
#define MAX_LINE_LEN 256

struct menu_line {
    char text[MAX_LINE_LEN];
    int y, height;
};

struct menu_state {
    struct wl_display *display;
    struct wlr_backend *backend;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;
    struct wlr_output *output;
    struct wlr_compositor *compositor;
    struct wlr_surface *surface;
    struct wlr_xdg_shell *xdg_shell;
    struct wlr_seat *seat;
    struct wlr_pointer *pointer;

    struct fcft_font *font;
    struct menu_line lines[MAX_LINES];
    int line_count;
    int width, height;

    int selected_idx;
};

static void read_lines(struct menu_state *state) {
    char buf[MAX_LINE_LEN];
    while (state->line_count < MAX_LINES && fgets(buf, sizeof(buf), stdin)) {
        size_t len = strlen(buf);
        if (len && buf[len-1] == '\n')
            buf[len-1] = '\0';
        snprintf(state->lines[state->line_count].text, sizeof(state->lines[0].text), "%s", buf);
        state->line_count++;
    }
}

// Dummy render: just print the menu to the terminal.
// Real code would use wlr_renderer, fcft, etc.
static void render_menu(struct menu_state *state) {
    printf("\033[2J\033[H"); // clear terminal
    for (int i = 0; i < state->line_count; ++i) {
        if (i == state->selected_idx)
            printf("> %s <\n", state->lines[i].text);
        else
            printf("  %s\n", state->lines[i].text);
    }
}

static void menu_run(struct menu_state *state) {
    render_menu(state);
    int c;
    while ((c = getchar()) != EOF) {
        if (c == '\n') {
            // Selected
            printf("%s\n", state->lines[state->selected_idx].text);
            break;
        } else if (c == 'j' || c == 'J' || c == 66) { // Down
            if (state->selected_idx + 1 < state->line_count)
                state->selected_idx++;
            render_menu(state);
        } else if (c == 'k' || c == 'K' || c == 65) { // Up
            if (state->selected_idx > 0)
                state->selected_idx--;
            render_menu(state);
        }
    }
}

int main(int argc, char *argv[]) {
    struct menu_state state = {0};
    read_lines(&state);
    if (state.line_count == 0) {
        fprintf(stderr, "No lines to show\n");
        return 1;
    }
    menu_run(&state);
    return 0;
}
