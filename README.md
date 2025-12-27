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

<img src="https://github.com/user-attachments/assets/fff6b3b6-2f83-4d83-9de6-41b9a4eb05a1" height="500px"/>
