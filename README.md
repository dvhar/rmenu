# Simple wayland recursive menu

Basically [xmenu](https://github.com/phillbush/xmenu) but for wayland.

## Build
install libwayland-client, pango, and cairo
```
make
sudo make install
```

## Run
The menu is defined by text on stdin. Each line is a menu item that prints to stdout when clicked. Tab-indented lines are submenus. Tab-separated lines can print text other than what is on the label. See test.sh for an example of how to use it.

<img src="https://github.com/user-attachments/assets/32fc5b23-4d81-4c95-9b78-f19782ed9611" height="500px"/>
