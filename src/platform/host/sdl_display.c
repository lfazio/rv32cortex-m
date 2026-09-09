/* SPDX-License-Identifier: Apache-2.0 */
/*
 * sdl_display.c - the host's window, behind the framebuffer's `present`.
 *
 * **All the SDL in this tree is in this file.** The device that a guest
 * talks to is portable C in src/emu/emu_fb.c and knows nothing about
 * windows; what a platform supplies is a pixel buffer and a function
 * that shows it. That is the console UART's arrangement -- the device
 * owns the register semantics, the platform owns the wire -- and it is
 * what keeps src/emu/ buildable for a board that has no display and no
 * SDL to link against.
 *
 * The whole file is behind EMU_SDL. Without it the host builds exactly
 * as before, `board_fb()` still returns a framebuffer, and a guest still
 * draws and counts frames into memory nobody looks at. That is not a
 * degraded mode: it is what the board does, and it is how the device is
 * tested in CI.
 *
 * Indexed pixels are handed over as they are. SDL3 takes an INDEX8
 * texture with a palette attached, so the conversion the guest's format
 * would otherwise need does not happen -- which was the reason for
 * passing the format and palette to `present` rather than resolving them
 * in the device. Checked before relying on it: creating an INDEX8
 * streaming texture succeeds, and SDL_SetTexturePalette is the API that
 * feeds it.
 */

#include "board.h"
#include "board_api.h"

#include "emu/emu_dev.h"
#include "emu_console.h"

#if EMU_SDL

#include <SDL3/SDL.h>
#include <string.h>

static struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    SDL_Palette *palette;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    /* What the palette was last set from, so a frame that did not change
     * it does not pay for 256 colour conversions. */
    uint32_t cached[EMU_FB_PALETTE_ENTRIES];
    bool cached_valid;
    bool failed;
} g_disp;

/*
 * Give up on the display, permanently, and say so once.
 *
 * A window that cannot be created is not a reason to stop emulating --
 * the guest is still running and its output is still on the console --
 * so this reports and disables rather than exiting. Reporting *once*
 * matters: a present that failed will fail again next frame, and sixty
 * lines a second of the same message would bury the run.
 */
static void disable(const char *what)
{
    if (!g_disp.failed) {
        emu_console_printf("emu: display: %s: %s\n", what, SDL_GetError());
        emu_console_printf("emu: continuing without a window\n");
    }
    g_disp.failed = true;
}

static uint32_t sdl_format_for(uint32_t fb_format)
{
    switch ((emu_fb_format_t)fb_format) {
    case EMU_FB_FMT_IDX8:
        return SDL_PIXELFORMAT_INDEX8;
    case EMU_FB_FMT_XRGB8888:
        return SDL_PIXELFORMAT_XRGB8888;
    default:
        return SDL_PIXELFORMAT_UNKNOWN;
    }
}

/*
 * Build the window and texture on the first frame rather than at
 * start-up.
 *
 * The geometry comes from the frame, which is the only thing that knows
 * it: the device is told its mode by the platform, but a *later* guest
 * could be given a different one, and a window sized from a compile-time
 * constant would be wrong exactly then. It also means a run whose guest
 * never draws opens no window at all, which is what the test suite
 * wants.
 */
static bool ensure_window(const emu_fb_frame_t *f)
{
    const uint32_t want = sdl_format_for(f->format);

    if (want == SDL_PIXELFORMAT_UNKNOWN) {
        emu_console_printf("emu: display: no SDL format for fb format %u\n",
                           (unsigned)f->format);
        g_disp.failed = true;
        return false;
    }

    if (g_disp.texture != NULL && g_disp.width == f->width &&
        g_disp.height == f->height && g_disp.format == f->format) {
        return true;
    }

    /* A mode change tears down what was there: a texture is bound to its
     * dimensions and its format, and neither can be altered in place. */
    if (g_disp.texture != NULL) {
        SDL_DestroyTexture(g_disp.texture);
        g_disp.texture = NULL;
    }

    if (g_disp.window == NULL) {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            disable("SDL_Init");
            return false;
        }
        if (!SDL_CreateWindowAndRenderer("rv32cortex-m", (int)f->width,
                                         (int)f->height, SDL_WINDOW_RESIZABLE,
                                         &g_disp.window, &g_disp.renderer)) {
            disable("SDL_CreateWindowAndRenderer");
            return false;
        }
        /*
         * Let SDL letterbox a resized window rather than doing the
         * arithmetic here. A guest's 320x200 is not the shape of any
         * modern screen, and stretching it to fit is the wrong answer.
         */
        SDL_SetRenderLogicalPresentation(g_disp.renderer, (int)f->width,
                                         (int)f->height,
                                         SDL_LOGICAL_PRESENTATION_LETTERBOX);
    }

    g_disp.texture =
        SDL_CreateTexture(g_disp.renderer, want, SDL_TEXTUREACCESS_STREAMING,
                          (int)f->width, (int)f->height);
    if (g_disp.texture == NULL) {
        disable("SDL_CreateTexture");
        return false;
    }

    /*
     * Nearest-neighbour, which is what a 320x200 guest wants: this is a
     * framebuffer with hard pixel edges, and smoothing it is not
     * faithfulness, it is blur.
     */
    SDL_SetTextureScaleMode(g_disp.texture, SDL_SCALEMODE_NEAREST);

    if (f->format == (uint32_t)EMU_FB_FMT_IDX8 && g_disp.palette == NULL) {
        g_disp.palette = SDL_CreatePalette(EMU_FB_PALETTE_ENTRIES);
        if (g_disp.palette == NULL) {
            disable("SDL_CreatePalette");
            return false;
        }
    }

    g_disp.width = f->width;
    g_disp.height = f->height;
    g_disp.format = f->format;
    g_disp.cached_valid = false;
    return true;
}

/* Push the guest's palette into SDL's, when it has changed. */
static void sync_palette(const emu_fb_frame_t *f)
{
    SDL_Color colours[EMU_FB_PALETTE_ENTRIES];

    if (f->palette == NULL || g_disp.palette == NULL) {
        return;
    }
    if (g_disp.cached_valid &&
        memcmp(g_disp.cached, f->palette, sizeof(g_disp.cached)) == 0) {
        return;
    }

    for (uint32_t i = 0; i < EMU_FB_PALETTE_ENTRIES; i++) {
        const uint32_t c = f->palette[i];

        colours[i].r = (uint8_t)(c >> 16);
        colours[i].g = (uint8_t)(c >> 8);
        colours[i].b = (uint8_t)c;
        colours[i].a = 0xFFu;
    }
    SDL_SetPaletteColors(g_disp.palette, colours, 0, EMU_FB_PALETTE_ENTRIES);
    SDL_SetTexturePalette(g_disp.texture, g_disp.palette);

    memcpy(g_disp.cached, f->palette, sizeof(g_disp.cached));
    g_disp.cached_valid = true;
}

/*
 * SDL scancodes to evdev codes.
 *
 * **A table, not arithmetic.** The two numberings agree over stretches
 * and then diverge -- evdev's alphabet starts at 30 in QWERTY order
 * where SDL's starts at 4 in alphabetical order, and the modifiers,
 * arrows and function keys are each their own arrangement. Deriving one
 * from the other works for exactly as long as the test presses letters.
 *
 * Only what a game needs is mapped. An unmapped key posts nothing rather
 * than posting a wrong code, because a guest acting on a code it did not
 * expect is worse than one that never hears the key.
 */
static uint32_t evdev_for(SDL_Scancode sc)
{
    static const struct {
        uint16_t sdl;
        uint16_t ev;
    } k_map[] = {
        {SDL_SCANCODE_ESCAPE, 1},    {SDL_SCANCODE_1, 2},
        {SDL_SCANCODE_2, 3},         {SDL_SCANCODE_3, 4},
        {SDL_SCANCODE_4, 5},         {SDL_SCANCODE_5, 6},
        {SDL_SCANCODE_6, 7},         {SDL_SCANCODE_7, 8},
        {SDL_SCANCODE_8, 9},         {SDL_SCANCODE_9, 10},
        {SDL_SCANCODE_0, 11},        {SDL_SCANCODE_MINUS, 12},
        {SDL_SCANCODE_EQUALS, 13},   {SDL_SCANCODE_BACKSPACE, 14},
        {SDL_SCANCODE_TAB, 15},      {SDL_SCANCODE_Q, 16},
        {SDL_SCANCODE_W, 17},        {SDL_SCANCODE_E, 18},
        {SDL_SCANCODE_R, 19},        {SDL_SCANCODE_T, 20},
        {SDL_SCANCODE_Y, 21},        {SDL_SCANCODE_U, 22},
        {SDL_SCANCODE_I, 23},        {SDL_SCANCODE_O, 24},
        {SDL_SCANCODE_P, 25},        {SDL_SCANCODE_RETURN, 28},
        {SDL_SCANCODE_LCTRL, 29},    {SDL_SCANCODE_A, 30},
        {SDL_SCANCODE_S, 31},        {SDL_SCANCODE_D, 32},
        {SDL_SCANCODE_F, 33},        {SDL_SCANCODE_G, 34},
        {SDL_SCANCODE_H, 35},        {SDL_SCANCODE_J, 36},
        {SDL_SCANCODE_K, 37},        {SDL_SCANCODE_L, 38},
        {SDL_SCANCODE_LSHIFT, 42},   {SDL_SCANCODE_Z, 44},
        {SDL_SCANCODE_X, 45},        {SDL_SCANCODE_C, 46},
        {SDL_SCANCODE_V, 47},        {SDL_SCANCODE_B, 48},
        {SDL_SCANCODE_N, 49},        {SDL_SCANCODE_M, 50},
        {SDL_SCANCODE_COMMA, 51},    {SDL_SCANCODE_PERIOD, 52},
        {SDL_SCANCODE_SLASH, 53},    {SDL_SCANCODE_RSHIFT, 54},
        {SDL_SCANCODE_LALT, 56},     {SDL_SCANCODE_SPACE, 57},
        {SDL_SCANCODE_F1, 59},       {SDL_SCANCODE_F2, 60},
        {SDL_SCANCODE_F3, 61},       {SDL_SCANCODE_F4, 62},
        {SDL_SCANCODE_F5, 63},       {SDL_SCANCODE_F6, 64},
        {SDL_SCANCODE_F7, 65},       {SDL_SCANCODE_F8, 66},
        {SDL_SCANCODE_F9, 67},       {SDL_SCANCODE_F10, 68},
        {SDL_SCANCODE_UP, 103},      {SDL_SCANCODE_LEFT, 105},
        {SDL_SCANCODE_RIGHT, 106},   {SDL_SCANCODE_DOWN, 108},
        {SDL_SCANCODE_RCTRL, 97},    {SDL_SCANCODE_RALT, 100},
    };

    for (size_t i = 0; i < sizeof(k_map) / sizeof(k_map[0]); i++) {
        if (k_map[i].sdl == (uint16_t)sc) {
            return k_map[i].ev;
        }
    }
    return 0u;
}

/*
 * Drain the event queue, posting what the guest's devices care about.
 *
 * Not optional: an SDL window that never polls is one the desktop marks
 * unresponsive, and it cannot be closed. Closing it stops the *display*
 * and leaves the guest running, which is the same decision as the
 * failure path above -- the emulator's job is not over because a window
 * went away.
 *
 * **The window is closed by the close button alone, not by Escape.**
 * Escape is a key a game wants -- it is Doom's menu -- and swallowing it
 * here would make the emulator's convenience beat the guest's function.
 */
static void pump_events(void)
{
    emu_input_t *const kbd = board_keyboard();
    emu_input_t *const mouse = board_mouse();
    SDL_Event e;

    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            emu_console_printf("emu: display closed; the guest continues\n");
            g_disp.failed = true;
            return;

        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            /*
             * Auto-repeat is dropped. It is the *desktop's* idea of a
             * held key, generated by a timer a guest knows nothing
             * about, and a game that tracks its own key state would see
             * a stream of downs with no ups between them.
             */
            if (kbd == NULL || e.key.repeat) {
                break;
            }
            const uint32_t code = evdev_for(e.key.scancode);

            if (code != 0u) {
                emu_input_post(kbd, code,
                               (e.type == SDL_EVENT_KEY_DOWN) ? 1u : 0u);
            }
            break;
        }

        case SDL_EVENT_MOUSE_MOTION:
            if (mouse != NULL) {
                emu_input_motion(mouse, (uint32_t)e.motion.x,
                                 (uint32_t)e.motion.y);
            }
            break;

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            if (mouse == NULL) {
                break;
            }
            uint32_t code = 0u;

            switch (e.button.button) {
            case SDL_BUTTON_LEFT:
                code = EMU_INPUT_BTN_LEFT;
                break;
            case SDL_BUTTON_RIGHT:
                code = EMU_INPUT_BTN_RIGHT;
                break;
            case SDL_BUTTON_MIDDLE:
                code = EMU_INPUT_BTN_MIDDLE;
                break;
            default:
                break;
            }
            if (code != 0u) {
                emu_input_post(mouse, code,
                               (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) ? 1u
                                                                       : 0u);
            }
            break;
        }

        default:
            break;
        }
    }
}

void host_display_present(void *ctx, const emu_fb_frame_t *f)
{
    (void)ctx;

    if (g_disp.failed) {
        return;
    }
    if (!ensure_window(f)) {
        return;
    }

    pump_events();
    if (g_disp.failed) {
        return;
    }

    sync_palette(f);

    if (!SDL_UpdateTexture(g_disp.texture, NULL, f->pixels, (int)f->stride)) {
        disable("SDL_UpdateTexture");
        return;
    }
    SDL_RenderClear(g_disp.renderer);
    SDL_RenderTexture(g_disp.renderer, g_disp.texture, NULL, NULL);
    SDL_RenderPresent(g_disp.renderer);
}

void host_display_shutdown(void)
{
    if (g_disp.texture != NULL) {
        SDL_DestroyTexture(g_disp.texture);
    }
    if (g_disp.palette != NULL) {
        SDL_DestroyPalette(g_disp.palette);
    }
    if (g_disp.renderer != NULL) {
        SDL_DestroyRenderer(g_disp.renderer);
    }
    if (g_disp.window != NULL) {
        SDL_DestroyWindow(g_disp.window);
        SDL_Quit();
    }
    memset(&g_disp, 0, sizeof(g_disp));
}

#else /* !EMU_SDL */

/*
 * No SDL. The framebuffer still exists and a guest still draws into it;
 * nothing is shown. Present rather than absent, so the host's board.c
 * does not need an #if of its own -- this is the same shape as the
 * board's `present`, which is NULL for the same reason.
 */
void host_display_present(void *ctx, const emu_fb_frame_t *f)
{
    (void)ctx;
    (void)f;
}

void host_display_shutdown(void)
{
}

#endif /* EMU_SDL */
