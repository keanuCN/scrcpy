#include "screen.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <SDL3/SDL.h>

#ifdef _WIN32
# include <winsock2.h>
# include <windows.h>
#endif

#include "events.h"
#include "icon.h"
#include "options.h"
#include "util/log.h"
#include "util/sdl.h"

#define DISPLAY_MARGINS 96

#define DOWNCAST(SINK) container_of(SINK, struct sc_screen, frame_sink)

#define CAMERA_BAR_X 12
#define CAMERA_BAR_Y 12
#define CAMERA_BUTTON_W 96
#define CAMERA_BUTTON_H 46
#define CAMERA_BUTTON_COUNT 5

#ifdef _WIN32
// Rasterize the toolbar once with a Windows Unicode font, then render it as a
// texture. SDL's built-in debug font cannot display Chinese glyphs.
static SDL_Texture *
create_camera_toolbar(struct sc_screen *screen) {
    const int width = CAMERA_BUTTON_W * CAMERA_BUTTON_COUNT;
    const int height = CAMERA_BUTTON_H;
    BITMAPINFO info = {0};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *pixels = NULL;
    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) return NULL;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, NULL, 0);
    if (!bitmap) { DeleteDC(dc); return NULL; }
    HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
    memset(pixels, 0, width * height * 4);
    HFONT font = CreateFontW(-15, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei");
    HGDIOBJ old_font = font ? SelectObject(dc, font) : NULL;
    HBRUSH tray = CreateSolidBrush(RGB(19, 28, 37));
    HPEN border = CreatePen(PS_SOLID, 1, RGB(62, 80, 91));
    HGDIOBJ old_brush = SelectObject(dc, tray);
    HGDIOBJ old_pen = SelectObject(dc, border);
    RoundRect(dc, 0, 0, width, height, 16, 16);
    SetBkMode(dc, TRANSPARENT);
    const wchar_t *labels[] = {L"\u5207\u6362\u6444\u50cf\u5934", L"\u955c\u50cf", L"\u65cb\u8f6c", L"\u5168\u5c4f", L"\u622a\u56fe"};
    for (int i = 0; i < CAMERA_BUTTON_COUNT; ++i) {
        bool selected = i == 1 && sc_orientation_is_mirror(screen->orientation);
        bool disabled = i == 0 && !screen->camera_switchable;
        COLORREF color = disabled ? RGB(28, 39, 47)
                       : i == screen->camera_toolbar_hover ? RGB(52, 90, 97)
                       : selected ? RGB(31, 100, 95) : RGB(38, 52, 61);
        HBRUSH button_brush = CreateSolidBrush(color);
        HPEN button_pen = CreatePen(PS_SOLID, 1, selected ? RGB(93, 198, 177) : RGB(58, 77, 85));
        SelectObject(dc, button_brush);
        SelectObject(dc, button_pen);
        int left = i * CAMERA_BUTTON_W + 4;
        RoundRect(dc, left, 4, (i + 1) * CAMERA_BUTTON_W - 4, height - 4, 12, 12);
        SelectObject(dc, tray);
        SelectObject(dc, border);
        DeleteObject(button_brush);
        DeleteObject(button_pen);
        SetTextColor(dc, disabled ? RGB(135, 151, 154) : RGB(242, 248, 247));
        RECT rect = {left, 4, (i + 1) * CAMERA_BUTTON_W - 4, height - 4};
        DrawTextW(dc, labels[i], -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    unsigned char *bytes = pixels;
    for (int i = 0; i < width * height; ++i) {
        bytes[i * 4 + 3] = (bytes[i * 4] || bytes[i * 4 + 1] || bytes[i * 4 + 2]) ? 255 : 0;
    }
    SDL_Surface *surface = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_BGRA32, pixels, width * 4);
    SDL_Texture *texture = surface ? SDL_CreateTextureFromSurface(screen->renderer, surface) : NULL;
    if (surface) SDL_DestroySurface(surface);
    if (old_font) SelectObject(dc, old_font);
    if (font) DeleteObject(font);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(tray);
    DeleteObject(border);
    SelectObject(dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return texture;
}
#endif

static float
camera_bar_scale(struct sc_screen *screen) {
    int width, height;
    SDL_GetWindowSize(screen->window, &width, &height);
    (void) height;
    float available = (float) (width - 2 * CAMERA_BAR_X);
    float scale = available / (CAMERA_BUTTON_W * CAMERA_BUTTON_COUNT);
    return scale < 1 ? SDL_max(scale, 0.1f) : 1;
}

static void
set_aspect_ratio(struct sc_screen *screen, struct sc_size content_size) {
    assert(content_size.width && content_size.height);

    if (screen->window_aspect_ratio_lock) {
        float ar = (float) content_size.width / content_size.height;
        bool ok = SDL_SetWindowAspectRatio(screen->window, ar, ar);
        if (!ok) {
            LOGW("Could not set window aspect ratio: %s", SDL_GetError());
        }
    }
}

static inline struct sc_size
get_oriented_size(struct sc_size size, enum sc_orientation orientation) {
    struct sc_size oriented_size;
    if (sc_orientation_is_swap(orientation)) {
        oriented_size.width = size.height;
        oriented_size.height = size.width;
    } else {
        oriented_size.width = size.width;
        oriented_size.height = size.height;
    }
    return oriented_size;
}

static inline bool
is_windowed(struct sc_screen *screen) {
    return !(SDL_GetWindowFlags(screen->window) & (SDL_WINDOW_FULLSCREEN
                                                 | SDL_WINDOW_MINIMIZED
                                                 | SDL_WINDOW_MAXIMIZED));
}

// get the preferred display bounds (i.e. the screen bounds with some margins)
static bool
get_preferred_display_bounds(struct sc_size *bounds) {
    SDL_Rect rect;
    SDL_DisplayID display = SDL_GetPrimaryDisplay();
    if (!display) {
        LOGW("Could not get primary display: %s", SDL_GetError());
        return false;
    }

    bool ok = SDL_GetDisplayUsableBounds(display, &rect);
    if (!ok) {
        LOGW("Could not get display usable bounds: %s", SDL_GetError());
        return false;
    }

    bounds->width = MAX(0, rect.w - DISPLAY_MARGINS);
    bounds->height = MAX(0, rect.h - DISPLAY_MARGINS);
    return true;
}

static bool
is_optimal_size(struct sc_size current_size, struct sc_size content_size) {
    // The size is optimal if we can recompute one dimension of the current
    // size from the other
    return current_size.height == (uint32_t) current_size.width
                                * content_size.height / content_size.width
        || current_size.width == (uint32_t) current_size.height
                               * content_size.width / content_size.height;
}

// return the optimal size of the window, with the following constraints:
//  - it attempts to keep at least one dimension of the current_size (i.e. it
//    crops the black borders)
//  - it keeps the aspect ratio
//  - it scales down to make it fit in the display_size
static struct sc_size
get_optimal_size(struct sc_size current_size, struct sc_size content_size,
                 bool within_display_bounds) {
    if (content_size.width == 0 || content_size.height == 0) {
        // avoid division by 0
        return current_size;
    }

    struct sc_size window_size;

    struct sc_size display_size;
    if (!within_display_bounds ||
            !get_preferred_display_bounds(&display_size)) {
        // do not constraint the size
        window_size = current_size;
    } else {
        window_size.width = MIN(current_size.width, display_size.width);
        window_size.height = MIN(current_size.height, display_size.height);
    }

    if (is_optimal_size(window_size, content_size)) {
        return window_size;
    }

    bool keep_width = (uint32_t) content_size.width * window_size.height
                    > (uint32_t) content_size.height * window_size.width;
    if (keep_width) {
        // remove black borders on top and bottom
        window_size.height = (uint32_t) content_size.height * window_size.width
                           / content_size.width;
    } else {
        // remove black borders on left and right (or none at all if it already
        // fits)
        window_size.width = (uint32_t) content_size.width * window_size.height
                          / content_size.height;
    }

    return window_size;
}

// initially, there is no current size, so use the frame size as current size
// req_width and req_height, if not 0, are the sizes requested by the user
static inline struct sc_size
get_initial_optimal_size(struct sc_size content_size, uint16_t req_width,
                         uint16_t req_height) {
    struct sc_size window_size;
    if (!req_width && !req_height) {
        window_size = get_optimal_size(content_size, content_size, true);
    } else {
        if (req_width) {
            window_size.width = req_width;
        } else {
            // compute from the requested height
            window_size.width = (uint32_t) req_height * content_size.width
                              / content_size.height;
        }
        if (req_height) {
            window_size.height = req_height;
        } else {
            // compute from the requested width
            window_size.height = (uint32_t) req_width * content_size.height
                               / content_size.width;
        }
    }
    return window_size;
}

static inline void
sc_screen_track_resize(struct sc_screen *screen, struct sc_size size) {
    LOGV("Track resize: %" PRIu16 "x%" PRIu16, size.width, size.height);
    screen->resize_tracker.time = sc_tick_now();
    screen->resize_tracker.size = size;
}

static bool
sc_screen_set_texture_from_surface(struct sc_screen *screen,
                                   SDL_Surface *surface) {
    // The current "frame" texture can be discarded
    sc_texture_reset(&screen->tex);

    if (screen->icon_tex) {
        SDL_DestroyTexture(screen->icon_tex);
    }

    screen->icon_tex = SDL_CreateTextureFromSurface(screen->renderer, surface);
    if (!screen->icon_tex) {
        LOGE("Could not create texture from surface: %s", SDL_GetError());
        return false;
    }

    // Once is_icon_active is true, it may never become false again
    screen->is_icon_active = true;

    return true;
}

static inline bool
sc_screen_is_relative_mode(struct sc_screen *screen) {
    // screen->im.mp may be NULL if --no-control
    return screen->im.mp && screen->im.mp->relative_mode;
}

static void
compute_content_rect(struct sc_size window_size, struct sc_size content_size,
                     bool is_icon, enum sc_render_fit render_fit,
                     SDL_FRect *rect) {
    // Only upscale video frames, not icon
    if (is_icon) {
        if (content_size.width <= window_size.width
                && content_size.height <= window_size.height) {
            // Center without upscaling
            rect->x = (window_size.width - content_size.width) / 2.f;
            rect->y = (window_size.height - content_size.height) / 2.f;
            rect->w = content_size.width;
            rect->h = content_size.height;
            return;
        }
    } else if (render_fit == SC_RENDER_FIT_UNSCALED) {
        // Cast to float first because input sizes are unsigned
        float x = ((float) window_size.width - content_size.width) / 2.f;
        float y = ((float) window_size.height - content_size.height) / 2.f;
        rect->x = MAX(0, x);
        rect->y = MAX(0, y);
        rect->w = content_size.width;
        rect->h = content_size.height;
        return;
    } else if (render_fit == SC_RENDER_FIT_STRETCHED) {
        rect->x = 0;
        rect->y = 0;
        rect->w = window_size.width;
        rect->h = window_size.height;
        return;
    }

    assert(is_icon || render_fit == SC_RENDER_FIT_LETTERBOX);

    if (is_optimal_size(window_size, content_size)) {
        rect->x = 0;
        rect->y = 0;
        rect->w = window_size.width;
        rect->h = window_size.height;
        return;
    }

    bool keep_width = (uint32_t) content_size.width * window_size.height
                    > (uint32_t) content_size.height * window_size.width;
    if (keep_width) {
        rect->x = 0;
        rect->w = window_size.width;
        rect->h = (float) window_size.width * content_size.height
                                            / content_size.width;
        rect->y = (window_size.height - rect->h) / 2.f;
    } else {
        rect->y = 0;
        rect->h = window_size.height;
        rect->w = (float) window_size.height * content_size.width
                                             / content_size.height;
        rect->x = (window_size.width - rect->w) / 2.f;
    }
}

static void
sc_screen_update_content_rect(struct sc_screen *screen) {
    struct sc_size window_size = sc_sdl_get_window_size(screen->window);
    compute_content_rect(window_size, screen->content_size,
                         screen->is_icon_active, screen->render_fit,
                         &screen->rect);
}

// render the texture to the renderer
//
// Set the update_content_rect flag if the window or content size may have
// changed, so that the content rectangle is recomputed
static void
sc_screen_render(struct sc_screen *screen, bool update_content_rect) {
    assert(screen->window_shown);

    if (update_content_rect) {
        sc_screen_update_content_rect(screen);
    }

    SDL_Renderer *renderer = screen->renderer;
    struct sc_screen_bg_color bg = screen->bg;
    SDL_SetRenderDrawColor(renderer, bg.r, bg.g, bg.b, 0);
    sc_sdl_render_clear(renderer);

    SDL_Texture *texture = screen->is_icon_active
                         ? screen->icon_tex
                         : sc_texture_get(&screen->tex);
    if (!texture) {
        goto end;
    }

    float scale = SDL_GetWindowPixelDensity(screen->window);
    if (scale == 0) {
        // Just in case, but in practice the function can only fail when window
        // is invalid
        LOGE("Cannot get scale value: %s", SDL_GetError());
        scale = 1;
    }

    SDL_FRect geometry = {
        .x = screen->rect.x * scale,
        .y = screen->rect.y * scale,
        .w = screen->rect.w * scale,
        .h = screen->rect.h * scale,
    };
    enum sc_orientation orientation = screen->orientation;

    const SDL_FRect *srcrect = NULL;
    SDL_FRect frame_rect;
    if (!screen->is_icon_active) {
        struct sc_size frame_size = sc_texture_get_frame_size(&screen->tex);
        frame_rect.x = 0;
        frame_rect.y = 0;
        frame_rect.w = frame_size.width;
        frame_rect.h = frame_size.height;
        srcrect = &frame_rect;
    }

    bool ok = false;
    if (orientation == SC_ORIENTATION_0) {
        // always align to a physical pixel
        geometry.x = (int32_t) geometry.x;
        geometry.y = (int32_t) geometry.y;
        ok = SDL_RenderTexture(renderer, texture, srcrect, &geometry);
    } else {
        unsigned cw_rotation = sc_orientation_get_rotation(orientation);
        double angle = 90 * cw_rotation;

        SDL_FRect *dstrect = NULL;
        SDL_FRect rect;
        if (sc_orientation_is_swap(orientation)) {
            rect.x = geometry.x + (geometry.w - geometry.h) / 2.f;
            rect.y = geometry.y + (geometry.h - geometry.w) / 2.f;
            rect.w = geometry.h;
            rect.h = geometry.w;
            dstrect = &rect;
        } else {
            dstrect = &geometry;
        }

        SDL_FlipMode flip = sc_orientation_is_mirror(orientation)
                              ? SDL_FLIP_HORIZONTAL : 0;

        // always align to a physical pixel
        dstrect->x = (int32_t) dstrect->x;
        dstrect->y = (int32_t) dstrect->y;
        ok = SDL_RenderTextureRotated(renderer, texture, srcrect, dstrect,
                                      angle, NULL, flip);
    }

    if (!ok) {
        LOGE("Could not render texture: %s", SDL_GetError());
    }

end:
    if (screen->screenshot_requested) {
        screen->screenshot_requested = false;
        SDL_Surface *shot = SDL_RenderReadPixels(renderer, NULL);
        const char *folder = SDL_GetUserFolder(SDL_FOLDER_PICTURES);
        if (!folder) folder = SDL_GetBasePath();
        char path[4096];
        time_t now = time(NULL);
        struct tm *local = localtime(&now);
        if (shot && folder && local) {
            SDL_snprintf(path, sizeof(path), "%sscrcpy-%04d%02d%02d-%02d%02d%02d-%03" SDL_PRIu64 ".bmp",
                         folder, local->tm_year + 1900, local->tm_mon + 1,
                         local->tm_mday, local->tm_hour, local->tm_min,
                         local->tm_sec, SDL_GetTicks() % 1000);
            if (SDL_SaveBMP(shot, path)) {
                LOGI("Screenshot saved: %s", path);
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "scrcpy", path, screen->window);
            } else {
                LOGE("Could not save screenshot: %s", SDL_GetError());
            }
        } else {
            LOGE("Could not capture screenshot: %s", SDL_GetError());
        }
        if (shot) SDL_DestroySurface(shot);
    }
    if (screen->camera && !screen->disconnected) {
        float density = SDL_GetWindowPixelDensity(screen->window);
        if (density <= 0) {
            density = 1;
        }
        float scale = camera_bar_scale(screen);
        SDL_FRect bar = { CAMERA_BAR_X * density, CAMERA_BAR_Y * density,
                          CAMERA_BUTTON_W * CAMERA_BUTTON_COUNT * scale * density,
                          CAMERA_BUTTON_H * scale * density };
#ifdef _WIN32
        if (!screen->camera_toolbar) screen->camera_toolbar = create_camera_toolbar(screen);
        if (screen->camera_toolbar) SDL_RenderTexture(renderer, screen->camera_toolbar, NULL, &bar);
#else
        SDL_SetRenderDrawColor(renderer, 31, 39, 49, 255);
        SDL_RenderFillRect(renderer, &bar);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_SetRenderScale(renderer, density * scale, density * scale);
        const char *labels[] = {"Switch", "Mirror", "Rotate", "Fullscreen", "Screenshot"};
        for (int i = 0; i < CAMERA_BUTTON_COUNT; ++i)
            SDL_RenderDebugText(renderer, CAMERA_BAR_X / scale + i * CAMERA_BUTTON_W + 5,
                                CAMERA_BAR_Y / scale + 13, labels[i]);
        SDL_SetRenderScale(renderer, 1, 1);
#endif
    }
    sc_sdl_render_present(renderer);
}

static void
sc_screen_request_resize_display(struct sc_screen *screen, uint16_t width,
                                 uint16_t height) {
    assert(screen->flex_display);
    assert(!screen->camera);
    if (sc_orientation_is_swap(screen->orientation)) {
        uint16_t tmp = width;
        width = height;
        height = tmp;
    }

    LOGV("resize_display(%" PRIu16 ", %" PRIu16 ")", width, height);
    sc_controller_resize_display(screen->controller, width, height);
}

static void
sc_screen_on_resize(struct sc_screen *screen, const SDL_WindowEvent *event) {
    // This event can be triggered before the window is shown
    if (!screen->window_shown) {
        return;
    }

    if (event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
        sc_screen_render(screen, true);
    } else {
        assert(event->type == SDL_EVENT_WINDOW_RESIZED);
        if (screen->flex_display) {
            assert(!(event->data1 & ~0xFFFF));
            assert(!(event->data2 & ~0xFFFF));
            uint16_t width = event->data1;
            uint16_t height = event->data2;

            struct sc_resize_tracker *tracker = &screen->resize_tracker;
            if (tracker->time
                    && sc_tick_now() >= tracker->time + SC_TICK_FROM_MS(3000)) {
                // Remove obsolete request
                tracker->time = 0;
            }
            if (tracker->time && tracker->size.width == width
                              && tracker->size.height == height) {
                // This resize event is the result of a previous (recent) resize
                // request triggered by a change in the frame's dimensions.
                LOGV("Ignore local resize: %" PRIu16 "x%" PRIu16,
                     width, height);
                tracker->time = 0;
            } else {
                sc_screen_request_resize_display(screen, width, height);
            }
        }
    }
}

#if defined(__APPLE__) || defined(_WIN32)
# define CONTINUOUS_RESIZING_WORKAROUND
#endif

#ifdef CONTINUOUS_RESIZING_WORKAROUND
// On Windows and MacOS, resizing blocks the event loop, so resizing events are
// not triggered. As a workaround, handle them in an event handler.
//
// <https://bugzilla.libsdl.org/show_bug.cgi?id=2077>
// <https://stackoverflow.com/a/40693139/1987178>
static bool
event_watcher(void *data, SDL_Event *event) {
    struct sc_screen *screen = data;
    assert(screen->video);

    if (event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED
            || event->type == SDL_EVENT_WINDOW_RESIZED) {
        // In practice, it seems to always be called from the same thread in
        // that specific case. Anyway, it's just a workaround.
        sc_screen_on_resize(screen, &event->window);
    }

    return true;
}
#endif

static bool
sc_screen_frame_sink_open(struct sc_frame_sink *sink,
                          const AVCodecContext *ctx,
                          const struct sc_stream_session *session) {
    assert(ctx->pix_fmt == AV_PIX_FMT_YUV420P);

    struct sc_screen *screen = DOWNCAST(sink);

    if (ctx->width <= 0 || ctx->width > 0xFFFF
            || ctx->height <= 0 || ctx->height > 0xFFFF) {
        LOGE("Invalid video size: %dx%d", ctx->width, ctx->height);
        return false;
    }

    screen->current_session = *session;

    assert(session->video.width && session->video.height);
    if (session->video.width > 0xFFFF || session->video.height > 0xFFFF) {
        LOGE("Size too large: %" PRIu32 "x%" PRIu32, session->video.width,
                                                     session->video.height);
        return false;
    }

    struct sc_size *size = malloc(sizeof(*size));
    if (!size) {
        LOG_OOM();
        return false;
    }
    size->width = session->video.width;
    size->height = session->video.height;

    bool ok = sc_push_event_with_data(SC_EVENT_OPEN_WINDOW, size);
    if (!ok) {
        free(size);
        return false;
    }

#ifndef NDEBUG
    screen->open = true;
#endif

    // nothing to do, the screen is already open on the main thread
    return true;
}

static void
sc_screen_frame_sink_close(struct sc_frame_sink *sink) {
    struct sc_screen *screen = DOWNCAST(sink);
    (void) screen;
#ifndef NDEBUG
    screen->open = false;
#endif

    // nothing to do, the screen lifecycle is not managed by the frame producer
}

static enum sc_sink_result
sc_screen_frame_sink_push(struct sc_frame_sink *sink, const AVFrame *frame) {
    struct sc_screen *screen = DOWNCAST(sink);
    assert(screen->video);

    sc_mutex_lock(&screen->mutex);
    bool previous_skipped = sc_frame_buffer_has_frame(&screen->fb);
    bool ok = sc_frame_buffer_push(&screen->fb, frame);
    screen->prevent_auto_resize = screen->current_session.video.client_resized;
    sc_mutex_unlock(&screen->mutex);
    if (!ok) {
        return SC_SINK_KO;
    }

    if (previous_skipped) {
        sc_fps_counter_add_skipped_frame(&screen->fps_counter);
        // The SC_EVENT_NEW_FRAME triggered for the previous frame will consume
        // this new frame instead
    } else {
        // Post the event on the UI thread
        bool ok = sc_push_event(SC_EVENT_NEW_FRAME);
        if (!ok) {
            return SC_SINK_KO;
        }
    }

    return SC_SINK_OK;
}

static enum sc_sink_result
sc_screen_frame_sink_push_session(struct sc_frame_sink *sink,
                                  const struct sc_stream_session *session) {
    struct sc_screen *screen = DOWNCAST(sink);
    screen->current_session = *session;
    return SC_SINK_OK;
}

bool
sc_screen_init(struct sc_screen *screen,
               const struct sc_screen_params *params) {
    screen->controller = params->controller;

    screen->resize_pending = false;
    screen->window_shown = false;
    screen->paused = false;
    screen->resume_frame = NULL;
    screen->orientation = SC_ORIENTATION_0;
    screen->disconnected = false;
    screen->disconnect_started = false;

    screen->video = params->video;
    screen->camera = params->camera;
    screen->camera_switchable = params->camera_switchable;
    screen->camera_switch_requested = false;
    screen->screenshot_requested = false;
    screen->camera_toolbar = NULL;
    screen->camera_toolbar_hover = -1;
    screen->windowed_geometry_valid = false;
    // Camera frames may have different aspect ratios. Letterbox them inside
    // the existing window instead of letting the OS resize it on a switch.
    screen->window_aspect_ratio_lock = params->window_aspect_ratio_lock && !params->camera;
    screen->render_fit = params->render_fit;
    screen->flex_display = params->flex_display;

    screen->bg.r = (params->background_color >> 16) & 0xFF;
    screen->bg.g = (params->background_color >> 8) & 0xFF;
    screen->bg.b = params->background_color & 0xFF;

    screen->req.x = params->window_x;
    screen->req.y = params->window_y;
    screen->req.width = params->window_width;
    screen->req.height = params->window_height;
    screen->req.fullscreen = params->fullscreen;
    screen->req.start_fps_counter = params->start_fps_counter;

    screen->prevent_auto_resize = false;

    screen->resize_tracker.time = 0;
    screen->resize_tracker.size.width = 0;
    screen->resize_tracker.size.height = 0;

    bool ok = sc_mutex_init(&screen->mutex);
    if (!ok) {
        return false;
    }

    ok = sc_frame_buffer_init(&screen->fb);
    if (!ok) {
        goto error_destroy_mutex;
    }

    if (!sc_fps_counter_init(&screen->fps_counter)) {
        goto error_destroy_frame_buffer;
    }

    if (screen->video) {
        screen->orientation = params->orientation;
        if (screen->orientation != SC_ORIENTATION_0) {
            LOGI("Initial display orientation set to %s",
                 sc_orientation_get_name(screen->orientation));
        }
    }

    // Always create the window hidden to prevent blinking during initialization
    uint32_t window_flags = SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
    if (params->always_on_top) {
        window_flags |= SDL_WINDOW_ALWAYS_ON_TOP;
    }
    if (params->window_borderless) {
        window_flags |= SDL_WINDOW_BORDERLESS;
    }
    if (params->video) {
        // The window will be shown on first frame
        window_flags |= SDL_WINDOW_RESIZABLE;
    }

    const char *title = params->window_title;
    assert(title);

    int x = SDL_WINDOWPOS_UNDEFINED;
    int y = SDL_WINDOWPOS_UNDEFINED;
    int width = 256;
    int height = 256;
    if (params->window_x != SC_WINDOW_POSITION_UNDEFINED) {
        x = params->window_x;
    }
    if (params->window_y != SC_WINDOW_POSITION_UNDEFINED) {
        y = params->window_y;
    }
    if (params->window_width) {
        width = params->window_width;
    }
    if (params->window_height) {
        height = params->window_height;
    }

    // The window will be positioned and sized on first video frame
    screen->window =
        sc_sdl_create_window(title, x, y, width, height, window_flags);
    if (!screen->window) {
        LOGE("Could not create window: %s", SDL_GetError());
        goto error_destroy_fps_counter;
    }

    screen->renderer = SDL_CreateRenderer(screen->window, NULL);
    if (!screen->renderer) {
        LOGE("Could not create renderer: %s", SDL_GetError());
        goto error_destroy_window;
    }

#ifdef SC_DISPLAY_FORCE_OPENGL_CORE_PROFILE
    screen->gl_context = NULL;

    // starts with "opengl"
    const char *renderer_name = SDL_GetRendererName(screen->renderer);
    bool use_opengl = renderer_name && !strncmp(renderer_name, "opengl", 6);
    if (use_opengl) {
        // Persuade macOS to give us something better than OpenGL 2.1.
        // If we create a Core Profile context, we get the best OpenGL version.
        bool ok = SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                                      SDL_GL_CONTEXT_PROFILE_CORE);
        if (!ok) {
            LOGW("Could not set a GL Core Profile Context");
        }

        LOGD("Creating OpenGL Core Profile context");
        screen->gl_context = SDL_GL_CreateContext(screen->window);
        if (!screen->gl_context) {
            LOGE("Could not create OpenGL context: %s", SDL_GetError());
            goto error_destroy_renderer;
        }
    }
#endif

    bool mipmaps = params->video && params->mipmaps;
    ok = sc_texture_init(&screen->tex, screen->renderer, mipmaps,
                         params->hwdec_mode);
    if (!ok) {
        goto error_destroy_renderer;
    }

    ok = SDL_StartTextInput(screen->window);
    if (!ok) {
        LOGE("Could not enable text input: %s", SDL_GetError());
        goto error_destroy_texture;
    }

    screen->icon_tex = NULL;
    screen->is_icon_active = false;
    SDL_Surface *icon = sc_icon_load(SC_ICON_FILENAME_SCRCPY);
    if (icon) {
        if (!SDL_SetWindowIcon(screen->window, icon)) {
            LOGW("Could not set window icon: %s", SDL_GetError());
        }

        if (!params->video) {
            screen->content_size.width = icon->w;
            screen->content_size.height = icon->h;
            ok = sc_screen_set_texture_from_surface(screen, icon);
            if (!ok) {
                LOGE("Could not set icon: %s", SDL_GetError());
            }
        }

        sc_icon_destroy(icon);
    } else {
        // not fatal
        LOGE("Could not load icon");

        if (!params->video) {
            // Make sure the content size is initialized
            screen->content_size.width = 256;
            screen->content_size.height = 256;
        }
    }

    screen->frame = av_frame_alloc();
    if (!screen->frame) {
        LOG_OOM();
        goto error_destroy_texture;
    }

    struct sc_input_manager_params im_params = {
        .controller = params->controller,
        .fp = params->fp,
        .screen = screen,
        .kp = params->kp,
        .mp = params->mp,
        .gp = params->gp,
        .camera = params->camera,
        .mouse_bindings = params->mouse_bindings,
        .legacy_paste = params->legacy_paste,
        .clipboard_autosync = params->clipboard_autosync,
        .shortcut_mods = params->shortcut_mods,
    };

    sc_input_manager_init(&screen->im, &im_params);

    // Initialize even if not used for simplicity
    sc_mouse_capture_init(&screen->mc, screen->window, params->shortcut_mods);

#ifdef CONTINUOUS_RESIZING_WORKAROUND
    if (screen->video) {
        ok = SDL_AddEventWatch(event_watcher, screen);
        if (!ok) {
            LOGW("Could not add event watcher for continuous resizing: %s",
                 SDL_GetError());
        }
    }
#endif

    memset(&screen->current_session, 0, sizeof(screen->current_session));

    static const struct sc_frame_sink_ops ops = {
        .open = sc_screen_frame_sink_open,
        .close = sc_screen_frame_sink_close,
        .push = sc_screen_frame_sink_push,
        .push_session = sc_screen_frame_sink_push_session,
    };

    screen->frame_sink.ops = &ops;

#ifndef NDEBUG
    screen->open = false;
#endif

    if (!screen->video) {
        // Show the window immediately
        screen->window_shown = true;
        sc_sdl_show_window(screen->window);

        if (sc_screen_is_relative_mode(screen)) {
            // Capture mouse immediately if video mirroring is disabled
            sc_mouse_capture_set_active(&screen->mc, true);
        }
    }

    return true;

error_destroy_texture:
    sc_texture_destroy(&screen->tex);
    if (screen->camera_toolbar) SDL_DestroyTexture(screen->camera_toolbar);
error_destroy_renderer:
#ifdef SC_DISPLAY_FORCE_OPENGL_CORE_PROFILE
    if (screen->gl_context) {
        SDL_GL_DestroyContext(screen->gl_context);
    }
#endif
    SDL_DestroyRenderer(screen->renderer);
error_destroy_window:
    SDL_DestroyWindow(screen->window);
error_destroy_fps_counter:
    sc_fps_counter_destroy(&screen->fps_counter);
error_destroy_frame_buffer:
    sc_frame_buffer_destroy(&screen->fb);
error_destroy_mutex:
    sc_mutex_destroy(&screen->mutex);

    return false;
}

static void
sc_screen_show_initial_window(struct sc_screen *screen) {
    int x = screen->req.x != SC_WINDOW_POSITION_UNDEFINED
          ? screen->req.x : (int) SDL_WINDOWPOS_CENTERED;
    int y = screen->req.y != SC_WINDOW_POSITION_UNDEFINED
          ? screen->req.y : (int) SDL_WINDOWPOS_CENTERED;
    struct sc_point position = {
        .x = x,
        .y = y,
    };

    struct sc_size window_size =
        get_initial_optimal_size(screen->content_size, screen->req.width,
                                                       screen->req.height);

    if (screen->flex_display
            && window_size.width == screen->content_size.width
            && window_size.height == screen->content_size.height) {
        // Avoid sending an unnecessary initial "resize display" request to the
        // server if the size has not changed.
        sc_screen_track_resize(screen, window_size);
    }

    assert(is_windowed(screen));
    set_aspect_ratio(screen, screen->content_size);
    sc_sdl_set_window_size(screen->window, window_size);
    sc_sdl_set_window_position(screen->window, position);

    if (screen->req.fullscreen) {
        sc_screen_toggle_fullscreen(screen);
    }

    if (screen->req.start_fps_counter) {
        sc_fps_counter_start(&screen->fps_counter);
    }

    screen->window_shown = true;
    sc_sdl_show_window(screen->window);
    sc_screen_update_content_rect(screen);
}

void
sc_screen_hide_window(struct sc_screen *screen) {
    sc_sdl_hide_window(screen->window);
    screen->window_shown = false;
}

void
sc_screen_interrupt(struct sc_screen *screen) {
    sc_fps_counter_interrupt(&screen->fps_counter);
}

static void
sc_screen_interrupt_disconnect(struct sc_screen *screen) {
    if (screen->disconnect_started) {
        sc_disconnect_interrupt(&screen->disconnect);
    }
}

void
sc_screen_join(struct sc_screen *screen) {
    sc_fps_counter_join(&screen->fps_counter);
    if (screen->disconnect_started) {
        sc_disconnect_join(&screen->disconnect);
    }
}

void
sc_screen_destroy(struct sc_screen *screen) {
#ifndef NDEBUG
    assert(!screen->open);
#endif
    if (screen->disconnect_started) {
        sc_disconnect_destroy(&screen->disconnect);
    }
    sc_texture_destroy(&screen->tex);
    if (screen->camera_toolbar) SDL_DestroyTexture(screen->camera_toolbar);
    av_frame_free(&screen->frame);
#ifdef SC_DISPLAY_FORCE_OPENGL_CORE_PROFILE
    SDL_GL_DestroyContext(screen->gl_context);
#endif
    SDL_DestroyRenderer(screen->renderer);
    SDL_DestroyWindow(screen->window);
    sc_fps_counter_destroy(&screen->fps_counter);
    sc_frame_buffer_destroy(&screen->fb);
    sc_mutex_destroy(&screen->mutex);

    SDL_Event event;
    bool has_event =
        sc_dequeue_event(SC_EVENT_DISCONNECTED_ICON_LOADED, &event);
    if (has_event) {
        assert(event.type == SC_EVENT_DISCONNECTED_ICON_LOADED);
        // The event was posted, but not handled, the icon must be freed
        SDL_Surface *dangling_icon = event.user.data1;
        sc_icon_destroy(dangling_icon);
    }

    has_event = sc_dequeue_event(SC_EVENT_OPEN_WINDOW, &event);
    if (has_event) {
        assert(event.type == SC_EVENT_OPEN_WINDOW);
        // The event was posted, but not handled, the size must be freed
        struct sc_size * size = event.user.data1;
        free(size);
    }
}

static void
resize_for_content(struct sc_screen *screen, struct sc_size old_content_size,
                   struct sc_size new_content_size) {
    assert(screen->video);

    struct sc_size target_size = new_content_size;
    if (!screen->flex_display) {
        struct sc_size window_size = sc_sdl_get_window_size(screen->window);
        // Scale proportionally
        target_size.width = (uint32_t) window_size.width * target_size.width
                          / old_content_size.width;
        target_size.height = (uint32_t) window_size.height * target_size.height
                           / old_content_size.height;
    }
    target_size = get_optimal_size(target_size, new_content_size, true);
    assert(is_windowed(screen));
    set_aspect_ratio(screen, new_content_size);
    sc_sdl_set_window_size(screen->window, target_size);
}

static void
set_content_size(struct sc_screen *screen, struct sc_size new_content_size,
                 bool resize) {
    assert(screen->video);

    if (resize) {
        if (is_windowed(screen)) {
            resize_for_content(screen, screen->content_size, new_content_size);
        } else if (screen->flex_display) {
            // Force a display resize, the client cannot resize in fullscreen
            struct sc_size size = sc_sdl_get_window_size(screen->window);
            sc_screen_request_resize_display(screen, size.width, size.height);
        } else if (!screen->resize_pending) {
            // Store the windowed size to be able to compute the optimal size
            // once fullscreen/maximized/minimized are disabled
            screen->windowed_content_size = screen->content_size;
            screen->resize_pending = true;
        }
    }

    screen->content_size = new_content_size;
}

static void
apply_pending_resize(struct sc_screen *screen) {
    assert(screen->video);

    assert(is_windowed(screen));
    if (screen->resize_pending) {
        resize_for_content(screen, screen->windowed_content_size,
                                   screen->content_size);
        screen->resize_pending = false;
    }
}

void
sc_screen_set_orientation(struct sc_screen *screen,
                          enum sc_orientation orientation) {
    assert(screen->video);

    if (orientation == screen->orientation) {
        return;
    }

    struct sc_size new_content_size =
        get_oriented_size(screen->frame_size, orientation);

    set_content_size(screen, new_content_size, !screen->camera);

    screen->orientation = orientation;
    LOGI("Display orientation set to %s", sc_orientation_get_name(orientation));

    sc_screen_render(screen, true);
}

static bool
sc_screen_apply_frame(struct sc_screen *screen, bool can_resize) {
    assert(screen->video);
    assert(screen->window_shown);

    sc_fps_counter_add_rendered_frame(&screen->fps_counter);

    AVFrame *frame = screen->frame;
    struct sc_size new_frame_size = {frame->width, frame->height};

    if (!new_frame_size.width || !new_frame_size.height) {
        LOGE("Invalid frame size: %" PRIu32 "x%" PRIu32,
             new_frame_size.width, new_frame_size.height);
        return false;
    }

    if (screen->frame_size.width != new_frame_size.width
            || screen->frame_size.height != new_frame_size.height) {

        // frame dimension changed
        screen->frame_size = new_frame_size;

        struct sc_size new_content_size =
            get_oriented_size(new_frame_size, screen->orientation);

        if (screen->flex_display) {
            sc_screen_track_resize(screen, new_content_size);
        }

        set_content_size(screen, new_content_size, can_resize && !screen->camera);
        sc_screen_update_content_rect(screen);
    }

    bool ok = sc_texture_update(&screen->tex, frame);
    if (!ok) {
        return false;
    }

    sc_screen_render(screen, false);
    return true;
}

static bool
sc_screen_update_frame(struct sc_screen *screen) {
    assert(screen->video);

    if (screen->paused) {
        if (!screen->resume_frame) {
            screen->resume_frame = av_frame_alloc();
            if (!screen->resume_frame) {
                LOG_OOM();
                return false;
            }
        } else {
            av_frame_unref(screen->resume_frame);
        }
        sc_mutex_lock(&screen->mutex);
        sc_frame_buffer_consume(&screen->fb, screen->resume_frame);
        sc_mutex_unlock(&screen->mutex);
        return true;
    }

    av_frame_unref(screen->frame);
    sc_mutex_lock(&screen->mutex);
    sc_frame_buffer_consume(&screen->fb, screen->frame);
    // read with lock held
    bool can_resize = !screen->prevent_auto_resize;
    sc_mutex_unlock(&screen->mutex);
    return sc_screen_apply_frame(screen, can_resize);
}

void
sc_screen_set_paused(struct sc_screen *screen, bool paused) {
    assert(screen->video);

    if (!paused && !screen->paused) {
        // nothing to do
        return;
    }

    if (screen->paused && screen->resume_frame) {
        // If display screen was paused, refresh the frame immediately, even if
        // the new state is also paused.
        av_frame_free(&screen->frame);
        screen->frame = screen->resume_frame;
        screen->resume_frame = NULL;
        bool ok = sc_screen_apply_frame(screen, true);
        if (!ok) {
            LOGE("Resume frame update failed");
        }
    }

    if (!paused) {
        LOGI("Display screen unpaused");
    } else if (!screen->paused) {
        LOGI("Display screen paused");
    } else {
        LOGI("Display screen re-paused");
    }

    screen->paused = paused;
}

void
sc_screen_toggle_fullscreen(struct sc_screen *screen) {
    assert(screen->video);

    bool req_fullscreen =
        !(SDL_GetWindowFlags(screen->window) & SDL_WINDOW_FULLSCREEN);

    if (screen->camera && req_fullscreen) {
        SDL_GetWindowPosition(screen->window, &screen->windowed_x, &screen->windowed_y);
        SDL_GetWindowSize(screen->window, &screen->windowed_width, &screen->windowed_height);
        screen->windowed_geometry_valid = true;
    }

    bool ok = SDL_SetWindowFullscreen(screen->window, req_fullscreen);
    if (!ok) {
        LOGW("Could not switch fullscreen mode: %s", SDL_GetError());
        return;
    }

    LOGD("Requested %s mode", req_fullscreen ? "fullscreen" : "windowed");
}

void
sc_screen_resize_to_fit(struct sc_screen *screen) {
    assert(screen->video);

    if (!is_windowed(screen)) {
        return;
    }

    if (screen->render_fit == SC_RENDER_FIT_STRETCHED) {
        // nothing to do
        return;
    }

    struct sc_size window_size = sc_sdl_get_window_size(screen->window);

    if (screen->render_fit == SC_RENDER_FIT_UNSCALED) {
        struct sc_size content_size = screen->content_size;
        set_aspect_ratio(screen, content_size);
        sc_sdl_set_window_size(screen->window, content_size);

        int32_t x_offset = 0;
        if (content_size.width < window_size.width) {
            x_offset = (window_size.width - content_size.width) / 2;
        }
        int32_t y_offset = 0;
        if (content_size.height < window_size.height) {
            y_offset = (window_size.height - content_size.height) / 2;
        }
        assert(x_offset >= 0 && y_offset >= 0);
        if (x_offset || y_offset) {
            struct sc_point pos = sc_sdl_get_window_position(screen->window);
            pos.x += x_offset;
            pos.y += y_offset;
            sc_sdl_set_window_position(screen->window, pos);
        }

        LOGD("Resized to content size: %ux%u", content_size.width,
                                               content_size.height);
        return;
    }

    assert(screen->render_fit == SC_RENDER_FIT_LETTERBOX);

    struct sc_point point = sc_sdl_get_window_position(screen->window);

    struct sc_size optimal_size =
        get_optimal_size(window_size, screen->content_size, false);

    // Center the window related to the device screen
    assert(optimal_size.width <= window_size.width);
    assert(optimal_size.height <= window_size.height);

    struct sc_point new_position = {
        .x = point.x + (window_size.width - optimal_size.width) / 2,
        .y = point.y + (window_size.height - optimal_size.height) / 2,
    };

    set_aspect_ratio(screen, screen->content_size);
    sc_sdl_set_window_size(screen->window, optimal_size);
    sc_sdl_set_window_position(screen->window, new_position);
    LOGD("Resized to optimal size: %ux%u", optimal_size.width,
                                           optimal_size.height);
}

void
sc_screen_resize_to_pixel_perfect(struct sc_screen *screen) {
    assert(screen->video);

    if (!is_windowed(screen)) {
        return;
    }

    struct sc_size content_size = screen->content_size;
    set_aspect_ratio(screen, content_size);
    sc_sdl_set_window_size(screen->window, content_size);
    LOGD("Resized to pixel-perfect: %ux%u", content_size.width,
                                            content_size.height);
}

static void
sc_disconnect_on_icon_loaded(struct sc_disconnect *d, SDL_Surface *icon,
                             void *userdata) {
    (void) d;
    (void) userdata;

    bool ok = sc_push_event_with_data(SC_EVENT_DISCONNECTED_ICON_LOADED, icon);
    if (!ok) {
        sc_icon_destroy(icon);
    }
}

static void
sc_disconnect_on_timeout(struct sc_disconnect *d, void *userdata) {
    (void) d;
    (void) userdata;

    bool ok = sc_push_event(SC_EVENT_DISCONNECTED_TIMEOUT);
    (void) ok; // ignore failure
}

void
sc_screen_handle_event(struct sc_screen *screen, const SDL_Event *event) {
    float bar_scale = screen->camera ? camera_bar_scale(screen) : 1;
    if (screen->camera && screen->window_shown
            && (event->type == SDL_EVENT_MOUSE_MOTION
                || event->type == SDL_EVENT_WINDOW_MOUSE_LEAVE)) {
        int hover = -1;
        if (event->type == SDL_EVENT_MOUSE_MOTION
                && event->motion.x >= CAMERA_BAR_X
                && event->motion.x < CAMERA_BAR_X + CAMERA_BUTTON_W * CAMERA_BUTTON_COUNT * bar_scale
                && event->motion.y >= CAMERA_BAR_Y
                && event->motion.y < CAMERA_BAR_Y + CAMERA_BUTTON_H * bar_scale) {
            hover = (event->motion.x - CAMERA_BAR_X) / (CAMERA_BUTTON_W * bar_scale);
        }
        if (hover != screen->camera_toolbar_hover) {
            screen->camera_toolbar_hover = hover;
            if (screen->camera_toolbar) SDL_DestroyTexture(screen->camera_toolbar);
            screen->camera_toolbar = NULL;
            sc_screen_render(screen, false);
        }
    }
    if (screen->camera && !screen->disconnected
            && event->type == SDL_EVENT_MOUSE_BUTTON_DOWN
            && event->button.button == SDL_BUTTON_LEFT
            && event->button.x >= CAMERA_BAR_X
            && event->button.x < CAMERA_BAR_X + CAMERA_BUTTON_W * CAMERA_BUTTON_COUNT * bar_scale
            && event->button.y >= CAMERA_BAR_Y
            && event->button.y < CAMERA_BAR_Y + CAMERA_BUTTON_H * bar_scale) {
        int button = (event->button.x - CAMERA_BAR_X) / (CAMERA_BUTTON_W * bar_scale);
        switch (button) {
            case 0:
                if (screen->camera_switchable) {
                    screen->camera_switch_requested = true;
                    sc_push_event(SDL_EVENT_QUIT);
                }
                break;
            case 1:
                sc_screen_set_orientation(screen, sc_orientation_apply(screen->orientation, SC_ORIENTATION_FLIP_0));
                if (screen->camera_toolbar) SDL_DestroyTexture(screen->camera_toolbar);
                screen->camera_toolbar = NULL;
                sc_screen_render(screen, false);
                break;
            case 2:
                sc_screen_set_orientation(screen, sc_orientation_apply(screen->orientation, SC_ORIENTATION_90));
                break;
            case 3:
                sc_screen_toggle_fullscreen(screen);
                break;
            case 4:
                screen->screenshot_requested = true;
                sc_screen_render(screen, false);
                break;
        }
        return;
    }
    switch (event->type) {
        case SC_EVENT_OPEN_WINDOW: {
            struct sc_size *size = event->user.data1;
            assert(size);

            screen->frame_size = *size;
            free(size);
            screen->content_size = get_oriented_size(screen->frame_size,
                                                     screen->orientation);
            sc_screen_show_initial_window(screen);

            if (sc_screen_is_relative_mode(screen)) {
                // Capture mouse on start
                sc_mouse_capture_set_active(&screen->mc, true);
            }

            sc_screen_render(screen, false);
            return;
        }
        case SC_EVENT_NEW_FRAME: {
            bool ok = sc_screen_update_frame(screen);
            if (!ok) {
                LOGE("Frame update failed\n");
            }
            return;
        }
        case SDL_EVENT_WINDOW_EXPOSED:
            sc_screen_render(screen, true);
            return;
// If defined, then the actions are already performed by the event watcher
#ifndef CONTINUOUS_RESIZING_WORKAROUND
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            sc_screen_on_resize(screen, &event->window);
            return;
#endif
        case SDL_EVENT_WINDOW_RESTORED:
            if (screen->video && is_windowed(screen)) {
                apply_pending_resize(screen);
                sc_screen_render(screen, true);
            }
            return;
        case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
            LOGD("Switched to fullscreen mode");
            assert(screen->video);
            return;
        case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
            LOGD("Switched to windowed mode");
            assert(screen->video);
            if (is_windowed(screen)) {
                apply_pending_resize(screen);
                sc_screen_render(screen, true);
            }
            return;
        case SC_EVENT_DEVICE_DISCONNECTED:
            assert(!screen->disconnected);
            screen->disconnected = true;
            if (!screen->window_shown) {
                // No window open
                return;
            }

            sc_input_manager_handle_event(&screen->im, event);

            sc_texture_reset(&screen->tex);
            sc_screen_render(screen, true);

            sc_tick deadline = sc_tick_now() + SC_TICK_FROM_SEC(2);
            static const struct sc_disconnect_callbacks cbs = {
                .on_icon_loaded = sc_disconnect_on_icon_loaded,
                .on_timeout = sc_disconnect_on_timeout,
            };
            bool ok =
                sc_disconnect_start(&screen->disconnect, deadline, &cbs, NULL);
            if (ok) {
                screen->disconnect_started = true;
            }

            return;
    }

    if (sc_screen_is_relative_mode(screen)
            && sc_mouse_capture_handle_event(&screen->mc, event)) {
        // The mouse capture handler consumed the event
        return;
    }

    sc_input_manager_handle_event(&screen->im, event);
}

void
sc_screen_handle_disconnection(struct sc_screen *screen) {
    if (!screen->window_shown) {
        // No window open, quit immediately
        return;
    }

    if (!screen->disconnect_started) {
        // If sc_disconnect_start() failed, quit immediately
        return;
    }

    SDL_Event event;
    while (SDL_WaitEvent(&event)) {
        switch (event.type) {
            case SDL_EVENT_WINDOW_EXPOSED:
                sc_screen_render(screen, true);
                break;
            case SC_EVENT_DISCONNECTED_ICON_LOADED: {
                SDL_Surface *icon_disconnected = event.user.data1;
                assert(icon_disconnected);

                bool ok = sc_screen_set_texture_from_surface(screen,
                                                             icon_disconnected);
                if (ok) {
                    screen->content_size.width = icon_disconnected->w;
                    screen->content_size.height = icon_disconnected->h;
                    sc_screen_render(screen, true);
                } else {
                    // not fatal
                    LOGE("Could not set disconnected icon");
                }

                sc_icon_destroy(icon_disconnected);
                break;
            }
            case SC_EVENT_DISCONNECTED_TIMEOUT:
                LOGD("Closing after device disconnection");
                return;
            case SDL_EVENT_QUIT:
                LOGD("User requested to quit");
                sc_screen_interrupt_disconnect(screen);
                return;
            default:
                sc_input_manager_handle_event(&screen->im, &event);
        }
    }
}

struct sc_point
sc_screen_convert_window_to_frame_coords(struct sc_screen *screen,
                                         int32_t x, int32_t y) {
    assert(screen->video);

    enum sc_orientation orientation = screen->orientation;

    int32_t w = screen->content_size.width;
    int32_t h = screen->content_size.height;

    // screen->rect must be initialized to avoid a division by zero
    assert(screen->rect.w && screen->rect.h);

    x = (int64_t) (x - screen->rect.x) * w / screen->rect.w;
    y = (int64_t) (y - screen->rect.y) * h / screen->rect.h;

    struct sc_point result;
    switch (orientation) {
        case SC_ORIENTATION_0:
            result.x = x;
            result.y = y;
            break;
        case SC_ORIENTATION_90:
            result.x = y;
            result.y = w - x;
            break;
        case SC_ORIENTATION_180:
            result.x = w - x;
            result.y = h - y;
            break;
        case SC_ORIENTATION_270:
            result.x = h - y;
            result.y = x;
            break;
        case SC_ORIENTATION_FLIP_0:
            result.x = w - x;
            result.y = y;
            break;
        case SC_ORIENTATION_FLIP_90:
            result.x = h - y;
            result.y = w - x;
            break;
        case SC_ORIENTATION_FLIP_180:
            result.x = x;
            result.y = h - y;
            break;
        default:
            assert(orientation == SC_ORIENTATION_FLIP_270);
            result.x = y;
            result.y = x;
            break;
    }

    return result;
}
