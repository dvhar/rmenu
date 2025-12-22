#!/bin/bash
cat <<EOF | ./rmenu
Item 1
Item 2
	Item 2.1
		Item 2.1.a
	Item 2.2
Item 3
EOF
