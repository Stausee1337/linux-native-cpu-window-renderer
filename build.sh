
CFLAGS="-Wall -Wextra -Wno-missing-braces -ggdb"
cc $CFLAGS -o bouncing-ball ./bouncing-ball.c -lX11 -lX11-xcb -lxcb -lm

