#define COLOR(hex)    { ((hex >> 16) & 0xFF) / 255.0f, \
                        ((hex >> 8) & 0xFF) / 255.0f, \
                        (hex & 0xFF) / 255.0f }

const int button_height = 30;
const int button_spacing = 3;
const int text_padding = 20;
const int min_width = 100;
const int separator_size = 1;

const float sep_color[] = COLOR(0xf09000);
const float menu_back[] = COLOR(0x0a0900);
const float button_color[] = COLOR(0x303030);
const float hovered_color[] = COLOR(0x505050);
const float text_color[] = COLOR(0xf0f0f0);
const float border_color[] = COLOR(0x505055);


const char* const font  = "Sans 12";
