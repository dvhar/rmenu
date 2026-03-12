# Simple wayland recursive menu

Basically [xmenu](https://github.com/phillbush/xmenu) but for wayland. Reads menu structure from stdin and prints clicked item to stdout.

## Build
install libwayland-client, pango, and cairo
```
make
sudo make install
```

## Configure
Configuration is done by editing config.h and recompiling.

### Menu Input Format
Each line in the input defines a menu item, with tab indentation indicating submenu levels. Same format as xmenu but with comments.

- **Basic Item:** `Label`
  - Displays "Label". Clicking it outputs "Label" to stdout.
- **Item with Custom Output:** `Label	OutputCommand`
  - Displays "Label". Clicking it outputs "OutputCommand" to stdout.
  - Tab delimiter between label and output
- **Submenus:**
  - Leading tabs define the submenu hierarchy.
```
Parent Item
	Sub Item 1
	Sub Item 2	SubCommand
```
- **Icons:** `IMG:path/to/icon.png	Label	OutputCommand`
  - `IMG:` prefix followed by the icon file path, then a tab, then the label and optional output.
  - Image must be png format
- **Separators:** An empty line creates a visual separator. Separators are not supported within submenus.
- **Comments:** Lines starting with `#` are ignored.

<img src="https://github.com/user-attachments/assets/396a0b1f-72b3-4216-a78d-346d6ae66c36" height="500px"/>
<img src="https://github.com/user-attachments/assets/fff6b3b6-2f83-4d83-9de6-41b9a4eb05a1" height="500px"/>
