// Copyright (c) 2024 Stausee1337
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#ifndef X_NATIVE_WINDOW_C_
#define X_NATIVE_WINDOW_C_

#include <stdint.h>
#include <stdbool.h>

#ifndef XNWINDEF
#define XNWINDEF static
#endif

typedef enum {
    E_CLOSE = 1,
    E_KEYBOARD,
    E_MOUSEMOVE,
    E_MOUSEBUTTON,
    E_MOUSEWHEEL,
    E_RESIZE,
    E_MOVE
} EventType;

typedef struct {
    int window;
    EventType type;
    union {
        struct { // E_KEYBOARD
            bool key_pressed;
            uint32_t keycode;
        };
        struct { // E_MOUSEMOVE
            int mouse_x, mouse_y;
        };
        struct { // E_MOUSEBUTTON
            bool mouse_down;
            enum {
                MB_LEFT = 1, MB_MIDDLE, MB_RIGHT
            } mouse_button;
        };
        struct { // E_MOUSEWHEEL
            float wheel_delta;
        };
        struct { // E_RESIZE
            int new_width, new_height;
        };
        struct { // E_MOVE
            int new_x, new_y;
        };
    };
} Event;

XNWINDEF Event begin_event();
XNWINDEF bool next_event(Event *);

XNWINDEF uint64_t get_time();
XNWINDEF int create_window(int, int, const char *);
XNWINDEF bool event_loop_poll(int);
XNWINDEF void close_window(int);

#endif

#ifdef X_NATIVE_WINDOW_IMPLEMENTATION
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>
#include <sys/epoll.h>

#include "stb_ds.h"

static int _root_window = -1;
static Display *_display;
static xcb_connection_t *_connection;
static int _epoll_fd;
static Atom _wm_delete_message;
static Atom _wm_protocols;

typedef struct {
    int x, y, width, height;
} WindowStub;

static struct {
    int key;
    WindowStub *value;
} *_window_stubs;

#define DA_INIT_CAP 256

#define _xwin_da_append(da, item)                                                        \
    do {                                                                                 \
        if ((da)->count >= (da)->capacity) {                                             \
            (da)->capacity = (da)->capacity == 0 ? DA_INIT_CAP : (da)->capacity*2;       \
            (da)->items = realloc((da)->items, (da)->capacity*sizeof(*(da)->items));     \
            assert((da)->items != NULL && "Buy more RAM lol");                           \
        }                                                                                \
                                                                                         \
        (da)->items[(da)->count++] = (item);                                             \
    } while (0)

static struct {
    Event *items;
    size_t count;
    size_t capacity;
} _current_frame_events = {0};


static
bool window_pos_and_size(int window, int *x, int *y, int *width, int *height) {
    XWindowAttributes attrs;
    if (XGetWindowAttributes(_display, window, &attrs) == 0) {
        return false;
    }
    *x = attrs.x;
    *y = attrs.y;
    *width = attrs.width;
    *height = attrs.height;

    return true;
}

static
int predicate(Display * _0, XEvent * _1, XPointer _2) {
    (void)_0;
    (void)_1;
    (void)_2;
    return 1;
}

XNWINDEF bool event_loop_poll(int timeout) {
#define _xwin_emit_event(...)\
    _xwin_da_append(&_current_frame_events, (__VA_ARGS__));

    int pending_events = XPending(_display);

    if (pending_events == 0) {
        if (timeout == 0)
            return false;

        static struct epoll_event out_epoll_events;
        bool did_timeout = epoll_wait(_epoll_fd, &out_epoll_events, 1, timeout) == 0;

        if (did_timeout) return false;
    }

    _current_frame_events.count = 0;

    XEvent event;
    while(XCheckIfEvent(_display, &event, predicate, NULL)) {
        switch (event.type) {
            case ClientMessage: {
                if (event.xclient.data.l[0] == (int)_wm_delete_message) {
                    _xwin_emit_event((Event) { .type = E_CLOSE, .window = event.xclient.window });
                }
            } break;
            case KeyPress: {
                KeySym keysym = XLookupKeysym(&event.xkey, 0);
                _xwin_emit_event((Event) {
                    .type = E_KEYBOARD,
                    .keycode = keysym,
                    .key_pressed = true,
                    .window = event.xkey.window
                });
            } break;
            case KeyRelease: {
                KeySym keysym = XLookupKeysym(&event.xkey, 0);
                _xwin_emit_event((Event) {
                    .type = E_KEYBOARD,
                    .keycode = keysym,
                    .key_pressed = false,
                    .window = event.xkey.window
                });
            } break;
            case MotionNotify: { 
                _xwin_emit_event((Event) {
                    .type = E_MOUSEMOVE,
                    .mouse_x = event.xmotion.x,
                    .mouse_y = event.xmotion.y,
                    .window = event.xmotion.window
                });
            } break;
            case ButtonPress: { 
                if (event.xbutton.button >= 4 && event.xbutton.button <= 5) {
                    _xwin_emit_event((Event) {
                        .type = E_MOUSEWHEEL,
                        .wheel_delta = event.xbutton.button == 4 ? 1.0 : -1.0,
                        .window = event.xbutton.window
                    });
                } else if (event.xbutton.button >= 1 && event.xbutton.button <= 3) {
                    _xwin_emit_event((Event) {
                        .type = E_MOUSEBUTTON,
                        .mouse_down = true,
                        .mouse_button = event.xbutton.button,
                        .window = event.xbutton.window
                    });
                }
            } break;
            case ButtonRelease: { 
                if (event.xbutton.button >= 1 && event.xbutton.button <= 3) {
                    _xwin_emit_event((Event) {
                        .type = E_MOUSEBUTTON,
                        .mouse_down = false,
                        .mouse_button = event.xbutton.button,
                        .window = event.xbutton.window
                    });
                }
            } break;
            case ConfigureNotify: {
                int window = event.xconfigure.window;

                /*int new_x, new_y, new_width, new_height;
                window_pos_and_size(window, &new_x, &new_y, &new_width, &new_height);*/

                int new_width = event.xconfigure.width, new_height = event.xconfigure.height;
                int new_x = event.xconfigure.x, new_y = event.xconfigure.y;

                WindowStub *stub = hmget(_window_stubs, window);

                if (stub->width != new_width || stub->height != new_height) {
                    _xwin_emit_event((Event) {
                        .type = E_RESIZE,
                        .new_width = new_width,
                        .new_height = new_height,
                        .window = window
                    });
                    stub->width = new_width;
                    stub->height = new_height;
                }
                if (stub->x != new_x || stub->y != new_y) {
                    _xwin_emit_event((Event) {
                        .type = E_MOVE,
                        .new_x = new_x, .new_y = new_y,
                        .window = window
                    });
                    stub->x = new_x;
                    stub->y = new_y;
                }
            } break;
        }
    }

    return true;
#undef _xwin_emit_event
}

static int _event_iterator_idx;

XNWINDEF Event begin_event() {
    _event_iterator_idx = 0;
    return (Event) { .type = 0 };
}

XNWINDEF bool next_event(Event *e) {
    bool has_next = _event_iterator_idx < (int)_current_frame_events.count;
    if (has_next) {
        *e = _current_frame_events.items[_event_iterator_idx];
        _event_iterator_idx += 1;
    } else {
        *e = (Event) { .type = 0 };
        _current_frame_events.count = 0;
    }
    return has_next;
}

XNWINDEF int create_window(int width, int height, const char *title) {
    if (_root_window == -1) {
        _display = XOpenDisplay(NULL);
        if (_display == NULL) {
            fprintf(stderr, "Cannot open display\n");
            exit(1);
        }

        _connection = XGetXCBConnection(_display);

        _wm_delete_message = XInternAtom(_display, "WM_DELETE_WINDOW", false);
        _wm_protocols = XInternAtom(_display, "WM_PROTOCOLS", false);
        _window_stubs = NULL;
    
        // setup event loop
        int xcb_conn_fd = xcb_get_file_descriptor(_connection);

        _epoll_fd = epoll_create(1);
        struct epoll_event epoll_event = {
            .events = EPOLLIN
        };
        epoll_ctl(_epoll_fd, EPOLL_CTL_ADD, xcb_conn_fd, &epoll_event);
    }

    Window root = DefaultRootWindow(_display);

    xcb_screen_iterator_t iter = xcb_setup_roots_iterator(xcb_get_setup(_connection));
    xcb_visualid_t visual = CopyFromParent;
    for (; iter.rem; xcb_screen_next(&iter)) {
        xcb_depth_iterator_t riter = xcb_screen_allowed_depths_iterator(iter.data);
        for (; riter.rem; xcb_depth_next(&riter)) {
            if (riter.data->depth != 32)
                continue;

            xcb_visualtype_iterator_t viter = xcb_depth_visuals_iterator(riter.data);
            for (; viter.rem; xcb_visualtype_next(&viter)) {
                if (viter.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR) {
                    visual = viter.data->visual_id;
                    break;
                }
            }

            if (visual != CopyFromParent)
                break;
        }
        if (visual != CopyFromParent)
            break;
    }

    uint8_t depth = CopyFromParent;
    if (visual != CopyFromParent) {
        depth = 32;
    } else {
        fprintf(stderr, "Warning: Transparency could not be enabled\n");
    }

    xcb_colormap_t colormap = 0;
    uint32_t cw_value_mask = XCB_CW_EVENT_MASK | XCB_CW_BORDER_PIXEL;
    if (visual != CopyFromParent) {
        colormap = xcb_generate_id(_connection);
        xcb_create_colormap(
                _connection,
                XCB_COLORMAP_ALLOC_NONE,
                colormap,
                root,
                visual);
        cw_value_mask |= XCB_CW_COLORMAP;
    }
    
    xcb_create_window_value_list_t window_attributes = {
        .event_mask = PropertyChangeMask |
            StructureNotifyMask | VisibilityChangeMask |
            KeyPressMask | KeyReleaseMask |
            ButtonPressMask | ButtonReleaseMask |
            PointerMotionMask,
        .colormap = colormap,
        .border_pixel = 0
    };

    Window window = xcb_generate_id(_connection);
    xcb_create_window_aux(
            _connection,
            depth,
            window,
            root,
            0, 0,
            width, height,
            0,
            XCB_WINDOW_CLASS_INPUT_OUTPUT,
            /*visual=*/visual,
            cw_value_mask,
            &window_attributes);

    if (_root_window == -1) {
        _root_window = window;
    }

    XStoreName(_display, window, title);

    /*XSelectInput(_display, window, 
            PropertyChangeMask |
            StructureNotifyMask | VisibilityChangeMask |
            KeyPressMask | KeyReleaseMask |
            ButtonPressMask | ButtonReleaseMask |
            PointerMotionMask);*/


    int out_x, out_y, out_width, out_height;
    window_pos_and_size(window, &out_x, &out_y, &out_width, &out_height);

    WindowStub *win_stub = malloc(sizeof(WindowStub));
    win_stub->x = out_x, win_stub->y = out_y;
    win_stub->width = out_width, win_stub->height = out_height;

    hmput(_window_stubs, window, win_stub);

    char buffer[50] = {0};
    sprintf(buffer, "x-native-winodw %zu ", hmlen(_window_stubs) + 1);
    xcb_change_property(
        _connection,
        XCB_PROP_MODE_REPLACE,
        window,
        XCB_ATOM_WM_CLASS,
        XCB_ATOM_STRING,
        8,
        strlen(buffer),
        buffer);

    xcb_change_property(
        _connection,
        XCB_PROP_MODE_REPLACE,
        window,
        _wm_protocols,
        XCB_ATOM_ATOM,
        32, 1, &_wm_delete_message);

    XMapWindow(_display, window);

    return window;
}

XNWINDEF void close_window(int win) {
    XDestroyWindow(_display, win);
    free(hmget(_window_stubs, win));
    hmdel(_window_stubs, win);
}

XNWINDEF uint64_t get_time() {
    static struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000 + (uint64_t)ts.tv_nsec;
}

#undef _xwin_da_append
#endif
