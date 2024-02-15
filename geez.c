#ifndef GEEZ_C_
#define GEEZ_C_

#include <stdbool.h>
#include "olive.c"

#ifndef GEEZDEF
#define GEEZDEF static
#endif

GEEZDEF void geez_set_render_target(int drawable, int width, int height);
GEEZDEF void geez_update_target_dimensions(int drawable, int width, int height);
GEEZDEF Olivec_Canvas geez_get_canvas();
GEEZDEF void geez_blit();

#endif

#ifdef GEEZ_IMPLEMENTATION

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <xcb/xcb.h>
#include <xcb/shm.h>
#include <xcb/xproto.h>

static void begin_wait();
static void finish_wait();
static uint32_t next_power_of_two(uint32_t);
static bool load_xcb_shm();

static int _drawable = -1;
static uint8_t _depth;
static xcb_connection_t *_connection;
static xcb_gcontext_t _gcontext;
static bool _using_shm;

typedef struct {
    uint32_t size;
    int id;
    uint8_t *ptr;
} Shm_Segment;

static
int create_shm_id() {
    char name[32] = {0};
    sprintf(name, "xcb-render-%d", (uint32_t)random());
    int id = shm_open(name, O_RDWR | O_CREAT | O_EXCL, S_IRWXU);
    if (id < 0) {
        fprintf(stderr, "ERROR: Could not open shared memory segment\n");
        return -1;
    }
    if (shm_unlink(name) < 0) {
        fprintf(stderr, "ERROR: Could not unlink shm\n");
        return -1;
    }
    return id;
}

static
bool make_shm_segment(uint32_t size, Shm_Segment *shm_out) {
    int id = create_shm_id();
    if (id == -1) {
        return false;
    }
    if (ftruncate(id, size) < 0) {
        fprintf(stderr, "ERROR: ftruncate() on shm file failed\n");
        return false;
    }

    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, id, 0);
    if (ptr == NULL) {
        fprintf(stderr, "ERROR: mmap() for shm segment failed\n");
        return false;
    }
    *shm_out = (Shm_Segment) {
        .id = id,
        .ptr = ptr,
        .size = size
    };
    return true;
}

static
void dispose_shm_segment(Shm_Segment seg) {
    if (munmap(seg.ptr, seg.size) < 0) {
        fprintf(stderr, "ERROR: munmap() on shm segment failed\n");
    }
    if (close(seg.id) < 0) {
        fprintf(stderr, "ERROR: close() on shm file failed\n");
    }
}

static Shm_Segment _current_segment = {0};
static xcb_shm_seg_t _current_shmseg = 0;

static
void associate_segment(Shm_Segment new_segment) {
    uint32_t new_id = xcb_generate_id(_connection);
    xcb_shm_attach_fd(_connection, new_id, new_segment.id, true);

    if (_current_segment.ptr != NULL && _current_segment.id != new_segment.id) {
        finish_wait();
        xcb_shm_detach(_connection, _current_shmseg);
        dispose_shm_segment(_current_segment);
    }
    _current_segment = new_segment;
    _current_shmseg = new_id;
}

static
bool alloc_segment(uint32_t buffer_size) {
    uint32_t new_size = next_power_of_two(buffer_size);

    bool needs_realloc = new_size > _current_segment.size;
    if (!needs_realloc)
        return true;

    Shm_Segment new_seg;
    if (!make_shm_segment(new_size, &new_seg)) {
        return false;
    }
    associate_segment(new_seg);
    return true;
}

static
bool check_shm_available() {
#ifdef DISABLE_XCB_SHM
    return false;
#endif
    if (!load_xcb_shm()) {
        return false;
    }
    Shm_Segment new_seg;
    if (!make_shm_segment(0x1000, &new_seg)) {
        return false;
    }
    dispose_shm_segment(new_seg);
    return true;
}

void geez_init() {
    _connection = xcb_connect(NULL, NULL);

    xcb_get_geometry_cookie_t geometry_token = xcb_get_geometry_unchecked(_connection, _drawable);
    xcb_get_geometry_reply_t* geometry_reply = xcb_get_geometry_reply(_connection, geometry_token, NULL);
    _depth = geometry_reply->depth;

    _gcontext = xcb_generate_id(_connection);
    xcb_create_gc_value_list_t list = {
        .graphics_exposures = 0
    };

    xcb_create_gc_aux(_connection, _gcontext, _drawable, XCB_GC_GRAPHICS_EXPOSURES, &list);

    if (!check_shm_available()) {
        fprintf(stderr, "SHM not available: performance will be poor\n");
        return;
    }

    _using_shm = check_shm_available();
}

static Olivec_Canvas _root_canvas = {0};

GEEZDEF void geez_set_render_target(int drawable, int width, int height) {
    if (_drawable == -1) {
        _drawable = drawable;
        geez_init();
        return;
    }
    _drawable = drawable;
    if (!_using_shm) {
        uint32_t *pixels = malloc(width * height * 4);
        _root_canvas = (Olivec_Canvas) {
            .pixels = pixels,
            .width = width,
            .height = height,
            .stride = width
        };
    } else {
        assert(alloc_segment(width * height * 4) && "Needs to allocate SHM segment");
        _root_canvas = (Olivec_Canvas) {
            .width = width,
            .height = height,
            .stride = width,
            .pixels = (uint32_t*)_current_segment.ptr
        };
    }
}

GEEZDEF void geez_update_target_dimensions(int drawable, int width, int height) {
    if (!_using_shm) {
        free(_root_canvas.pixels);
        uint32_t *pixels = malloc(width * height * 4);
        _root_canvas = (Olivec_Canvas) {
            .pixels = pixels,
            .width = width,
            .height = height,
            .stride = width
        };
    } else {
        assert(alloc_segment(width * height * 4) && "Needs to allocate SHM segment");
        _root_canvas = (Olivec_Canvas) {
            .width = width,
            .height = height,
            .stride = width,
            .pixels = (uint32_t*)_current_segment.ptr
        };
    }
}

GEEZDEF Olivec_Canvas geez_get_canvas() {
    finish_wait();
    return _root_canvas;
}

GEEZDEF void geez_blit() {
    if (!_using_shm) {
        xcb_put_image(
            _connection,
            XCB_IMAGE_FORMAT_Z_PIXMAP,
            _drawable,
            _gcontext,
            _root_canvas.width,
            _root_canvas.height,
            0,
            0,
            0,
            _depth,
            _root_canvas.width * _root_canvas.height * 4, 
            (uint8_t*)_root_canvas.pixels);
    } else {
        xcb_shm_put_image(
                _connection,
                _drawable,
                _gcontext,
                _root_canvas.width, 
                _root_canvas.height,
                /*src_pos=*/0, 0,
                _root_canvas.width, 
                _root_canvas.height,
                /*dst_pos=*/0, 0,
                _depth,
                XCB_IMAGE_FORMAT_Z_PIXMAP,
                false,
                _current_shmseg,
                /*offfset=*/0);
        begin_wait();
    }
}

static uint32_t processing_cookie = 0;

static
void begin_wait() {
    processing_cookie = xcb_get_input_focus_unchecked(_connection).sequence;
}

static
void finish_wait() {
    if (processing_cookie != 0) {
        xcb_get_input_focus_reply(
                _connection, 
                (xcb_get_input_focus_cookie_t) { .sequence = processing_cookie }, 
                NULL);
        processing_cookie = 0;
    }
}

static
uint32_t next_power_of_two(uint32_t __n) {
    __n--;
    __n |= __n >> 1;
    __n |= __n >> 2;
    __n |= __n >> 4;
    __n |= __n >> 8;
    __n |= __n >> 16;
    __n++;
    return __n;
}

#define _GEEZ_XGOT(name) _xcb_shm_got.name
#define XCB_SHM_LIBNAME "libxcb-shm.so"
#define _GEEZ_XGOT_RESOLVE(name) \
_xcb_shm_got.name = dlsym(_xcb_shm_got.handle, #name);            \
if (_xcb_shm_got.name == NULL) {                                  \
    fputs("Could not find symbol: "#name" in library\n", stderr); \
    return false;                                                 \
}

struct {
    void *handle;
    xcb_void_cookie_t
    (*xcb_shm_attach_fd) (xcb_connection_t *c,
                          xcb_shm_seg_t     shmseg,
                          int32_t           shm_fd,
                          uint8_t           read_only);


    xcb_void_cookie_t
    (*xcb_shm_detach) (xcb_connection_t *c,
                       xcb_shm_seg_t     shmseg);


    xcb_void_cookie_t
    (*xcb_shm_put_image) (xcb_connection_t *c,
                          xcb_drawable_t    drawable,
                          xcb_gcontext_t    gc,
                          uint16_t          total_width,
                          uint16_t          total_height,
                          uint16_t          src_x,
                          uint16_t          src_y,
                          uint16_t          src_width,
                          uint16_t          src_height,
                          int16_t           dst_x,
                          int16_t           dst_y,
                          uint8_t           depth,
                          uint8_t           format,
                          uint8_t           send_event,
                          xcb_shm_seg_t     shmseg,
                          uint32_t          offset);

} _xcb_shm_got;

static
bool load_xcb_shm() {
    _xcb_shm_got.handle = dlopen(XCB_SHM_LIBNAME, RTLD_LAZY);
    if (_xcb_shm_got.handle == NULL) {
        fputs("Could not load: "XCB_SHM_LIBNAME "\n", stderr);
        return false;
    }

    _GEEZ_XGOT_RESOLVE(xcb_shm_attach_fd);
    _GEEZ_XGOT_RESOLVE(xcb_shm_detach);
    _GEEZ_XGOT_RESOLVE(xcb_shm_put_image);

    return true;
}

xcb_void_cookie_t
xcb_shm_attach_fd (xcb_connection_t *c,
                   xcb_shm_seg_t     shmseg,
                   int32_t           shm_fd,
                   uint8_t           read_only) {
    return _GEEZ_XGOT(xcb_shm_attach_fd)(c, shmseg, shm_fd, read_only);
}

xcb_void_cookie_t
xcb_shm_detach (xcb_connection_t *c,
                xcb_shm_seg_t     shmseg) {
    return _GEEZ_XGOT(xcb_shm_detach)(c, shmseg);
}

xcb_void_cookie_t
xcb_shm_put_image (xcb_connection_t *c,
                   xcb_drawable_t    drawable,
                   xcb_gcontext_t    gc,
                   uint16_t          total_width,
                   uint16_t          total_height,
                   uint16_t          src_x,
                   uint16_t          src_y,
                   uint16_t          src_width,
                   uint16_t          src_height,
                   int16_t           dst_x,
                   int16_t           dst_y,
                   uint8_t           depth,
                   uint8_t           format,
                   uint8_t           send_event,
                   xcb_shm_seg_t     shmseg,
                   uint32_t          offset) {
    return _GEEZ_XGOT(xcb_shm_put_image)(
            c,
            drawable,
            gc,
            total_width,
            total_height,
            src_x,
            src_y,
            src_width,
            src_height,
            dst_x,
            dst_y,
            depth,
            format,
            send_event,
            shmseg,
            offset);
}

#undef _GEEZ_XGOT
#undef _GEEZ_XGOT_RESOLVE

#endif
