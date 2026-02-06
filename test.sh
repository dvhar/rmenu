#!/bin/bash
cat <<EOF | ./rmenu
Item 1

Item 2
	Item 2.1
		Item 2.1.a
		#comment
		Item 2.1.b	hidden output
		Item 2.1.c	hidden output 2
		Item 2.1.d	hidden output 3
		Item 2.1.e	hidden output 4
	Item 2.2
		Item 2.2.a
	Item 2.3
	Item 2.4
	Item 2.5
		Item 2.5.a
		Item 2.5.b
	Item 2.6
		Item 2.6.a

Item 3
	Item 3.1
		Item 3.1.a
#comment
Item 4
IMG:./icons/web.png	Item 5
EOF
