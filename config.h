#pragma once
#define COLOR(hex)    { ((hex >> 16) & 0xFF) / 255.0f, \
                        ((hex >> 8) & 0xFF) / 255.0f, \
                        (hex & 0xFF) / 255.0f }

static const int button_height = 30;
static const int button_spacing = 3;
static const int text_padding = 20;
static const int min_width = 100;
static const int separator_size = 1;

static const float button_grad_left[3]  = COLOR(0x38211a); // #38211a
static const float button_grad_right[3] = COLOR(0x52332e); // #52332e
static const float hovered_grad_left[3]  = COLOR(0x6e4c42); // #6e4c42
static const float hovered_grad_right[3] = COLOR(0x916e61); // #916e61
static const float sep_color[] = COLOR(0xf09000); // #f09000
static const float menu_back[] = COLOR(0x0a0900); // #0a0900
static const float text_color[] = COLOR(0xf0f0f0); // #f0f0f0
static const float border_color[] = COLOR(0x505055); // #505055


static const char* const font  = "Sans 12";

static const int icon_size       = button_height;
static const int icon_text_gap   = 8;
static const int icon_left_pad   = text_padding;
