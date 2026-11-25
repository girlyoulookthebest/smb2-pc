/* PC port settings: settings.dat persistence, first-run defaults, command-
 * line overrides, live apply, and the item API used by the settings screens.
 *
 * settings.dat is a small text file ("SMB2PC 2" header, key=value lines) in
 * platform_get_save_dir().  The legacy binary format (a raw GameSettings
 * struct with the ISO path appended) is read too, for its ISO path. */
#include "pc_settings.h"
#include "button_prompts.h"
#include "pc_text.h"
#include "platform.h"
#include "render.h"
#include "audio.h"
#include "audio_dsp.h"
#include "../render/gx_gpu.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

PcSettings g_pcs;
static PcSettings s_file;     /* what settings.dat holds (no CLI overrides) */
static int s_loaded;
static int s_overrides;
static int s_active_msaa = -1;
static int s_first_run;      /* no settings.dat when the game started */

#ifdef SMB2_LINK_GAME
extern int pc_game_wide_mode(void);   /* src/game/shim/pc_settings_menu.c */
extern void pc_game_set_wide_mode(int wide);
#else
int pc_game_wide_mode(void) { return 0; }
void pc_game_set_wide_mode(int wide) { (void)wide; }
#endif

static SDL_Window* cur_window(void) { return (SDL_Window*)platform_sdl_window(); }

static int cur_display(void) {
    SDL_Window* w = cur_window();
    int d = w ? SDL_GetWindowDisplayIndex(w) : 0;
    return d < 0 ? 0 : d;
}

/* ---------------------------------------------------------------- defaults */
static void fit_default_size(int* w, int* h) {
    SDL_Rect ub;
    *w = 1280; *h = 720;
    if (!SDL_WasInit(SDL_INIT_VIDEO) && SDL_InitSubSystem(SDL_INIT_VIDEO) < 0)
        return;
    if (SDL_GetDisplayUsableBounds(0, &ub) != 0 || ub.w <= 0 || ub.h <= 0)
        return;
    {   /* The process is per-monitor DPI aware (main.c), so sizes are real
         * pixels: scale the default window by the display scaling (150% ->
         * 1920x1080) so it isn't tiny on high-DPI screens. */
        float ddpi = 0.0f;
        if (SDL_GetDisplayDPI(0, &ddpi, NULL, NULL) == 0 && ddpi > 96.5f) {
            float k = ddpi / 96.0f;
            *w = (int)(1280 * k + 0.5f);
            *h = (int)(720 * k + 0.5f);
        }
    }
    /* leave room for the title bar / borders */
    if (*w <= ub.w && *h + 48 <= ub.h) return;
    {
        int hh = ub.h - 48, ww;
        if (hh * 16 / 9 > ub.w) hh = ub.w * 9 / 16;
        hh -= hh % 9;
        ww = hh * 16 / 9;
        if (hh >= 240) { *w = ww; *h = hh; }
    }
}

static void set_defaults(PcSettings* s) {
    memset(s, 0, sizeof *s);
    fit_default_size(&s->width, &s->height);
    /* First start: borderless fullscreen at the desktop resolution (no mode
     * switch); width/height stay the windowed size used by Alt+Enter. */
    s->display_mode = PCS_BORDERLESS;
    s->vsync = 1;
    s->msaa = 4;
    s->aspect = PCS_ASPECT_AUTO;
    s->volume = 100;
    s->vol_music = s->vol_sfx = s->vol_voice = s->vol_timer = 100;
    s->fps = 60;
    s->update_check = 1;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void sanitize(PcSettings* s) {
    s->width = clampi(s->width, 320, 16384);
    s->height = clampi(s->height, 240, 16384);
    s->display_mode = clampi(s->display_mode, 0, 2);
    s->vsync = s->vsync ? 1 : 0;
    s->msaa = s->msaa >= 8 ? 8 : s->msaa >= 4 ? 4 : s->msaa >= 2 ? 2 : 0;
    s->aspect = clampi(s->aspect, 0, 1);
    s->volume = clampi(s->volume, 0, 100);
    s->vol_music = clampi(s->vol_music, 0, 100);
    s->vol_sfx = clampi(s->vol_sfx, 0, 100);
    s->vol_voice = clampi(s->vol_voice, 0, 100);
    s->vol_timer = clampi(s->vol_timer, 0, 100);
    s->button_style = clampi(s->button_style, 0, PCB_SETTING_MENU_COUNT - 1);
    s->renderer = clampi(s->renderer, 0, PCS_RENDERER_COUNT - 1);
    s->language = clampi(s->language, 0, PCS_LANG_COUNT - 1);
    if (s->fps != PCS_FPS_UNCAPPED && s->fps != PCS_FPS_DISPLAY)
        s->fps = clampi(s->fps, 60, 1000);
}

int pcs_parse_fps(const char* v) {
    if (!v) return 60;
    if (!strcmp(v, "uncapped") || !strcmp(v, "0")) return PCS_FPS_UNCAPPED;
    if (!strcmp(v, "display") || !strcmp(v, "vsync")) return PCS_FPS_DISPLAY;
    return atoi(v);
}

/* ---------------------------------------------------------------- language */
static const char* const s_language_keys[PCS_LANG_COUNT] = {
    "auto", "en", "de", "fr", "es", "it", "ja" };

int pcs_parse_language(const char* v) {
    int i;
    static const struct { const char* name; int lang; } alias[] = {
        { "english", PCS_LANG_EN }, { "german", PCS_LANG_DE }, { "deutsch", PCS_LANG_DE },
        { "french", PCS_LANG_FR }, { "spanish", PCS_LANG_ES }, { "italian", PCS_LANG_IT },
        { "japanese", PCS_LANG_JA }, { "jp", PCS_LANG_JA },
    };
    if (!v) return -1;
    for (i = 0; i < PCS_LANG_COUNT; i++)
        if (!SDL_strcasecmp(v, s_language_keys[i])) return i;
    for (i = 0; i < (int)(sizeof alias / sizeof alias[0]); i++)
        if (!SDL_strcasecmp(v, alias[i].name)) return alias[i].lang;
    return -1;
}

/* Auto: the Windows UI language.  Japanese -> Japanese; everything else ->
 * English.  The US disc's German/French/Spanish/Italian text is unfinished
 * (docs/language.md), so Auto never picks those; they stay selectable. */
static int auto_locale(void) {
#ifdef _WIN32
    LANGID id = GetUserDefaultUILanguage();
    if (PRIMARYLANGID(id) == LANG_JAPANESE) return 5;
#endif
    return 0;
}

int pcs_language_locale(int language) {
    switch (language) {
    case PCS_LANG_EN: return 0;
    case PCS_LANG_DE: return 1;
    case PCS_LANG_FR: return 2;
    case PCS_LANG_ES: return 3;
    case PCS_LANG_IT: return 4;
    case PCS_LANG_JA: return 5;
    default: return auto_locale();
    }
}

static int s_active_locale = -1;
int pcs_active_locale(void) {
    return s_active_locale < 0 ? pcs_language_locale(g_pcs.language) : s_active_locale;
}
void pcs_set_active_locale(int l) { s_active_locale = l; }

/* ------------------------------------------------------------- persistence */
static const char* const s_button_style_keys[PCB_SETTING_MENU_COUNT] = {
    "auto", "gc", "ps", "xbox", "switch" };
static void settings_path(char* out, size_t n) {
    snprintf(out, n, "%ssettings.dat", platform_get_save_dir());
}

static void load_file(PcSettings* s) {
    char path[1100], line[1200];
    FILE* f;
    int version;
    set_defaults(s);
    settings_path(path, sizeof path);
    f = fopen(path, "rb");
    if (!f) {
        s_first_run = 1;
        printf("[settings] no %s - first start, using defaults (borderless fullscreen)\n", path);
        return;
    }
    /* "SMB2PC <version>": 1 up to 1.1.13; 2 adds the sound mixer
     * (vol_music/vol_sfx/vol_voice/vol_timer).  "volume" is the master
     * volume in both, so a version 1 file keeps its volume as Master and the
     * new channels start at 100. */
    if (!fgets(line, sizeof line, f) || strncmp(line, "SMB2PC ", 7) != 0) {
        /* legacy binary file: GameSettings struct, then the ISO path */
        long n;
        fseek(f, 0, SEEK_END);
        n = ftell(f) - (long)sizeof(GameSettings);
        if (n > 0 && n < (long)sizeof s->iso_path) {
            fseek(f, (long)sizeof(GameSettings), SEEK_SET);
            if (fread(s->iso_path, 1, (size_t)n, f) == (size_t)n)
                s->iso_path[n] = 0;
            else
                s->iso_path[0] = 0;
        }
        fclose(f);
        printf("[settings] migrated legacy settings.dat (iso=%s)\n", s->iso_path);
        return;
    }
    version = atoi(line + 7);
    while (fgets(line, sizeof line, f)) {
        char* eq = strchr(line, '=');
        char* v;
        if (!eq) continue;
        *eq = 0;
        v = eq + 1;
        v[strcspn(v, "\r\n")] = 0;
        if      (!strcmp(line, "width"))   s->width = atoi(v);
        else if (!strcmp(line, "height"))  s->height = atoi(v);
        else if (!strcmp(line, "display")) s->display_mode =
            !strcmp(v, "fullscreen") ? PCS_FULLSCREEN :
            !strcmp(v, "borderless") ? PCS_BORDERLESS : PCS_WINDOWED;
        else if (!strcmp(line, "vsync"))   s->vsync = atoi(v);
        else if (!strcmp(line, "msaa"))    s->msaa = atoi(v);
        else if (!strcmp(line, "aspect"))  s->aspect = !strcmp(v, "stretch");
        else if (!strcmp(line, "volume"))  s->volume = atoi(v);
        else if (!strcmp(line, "vol_music")) s->vol_music = atoi(v);
        else if (!strcmp(line, "vol_sfx"))   s->vol_sfx = atoi(v);
        else if (!strcmp(line, "vol_voice")) s->vol_voice = atoi(v);
        else if (!strcmp(line, "vol_timer")) s->vol_timer = atoi(v);
        else if (!strcmp(line, "buttons")) s->button_style =
            !strcmp(v, "gc") ? PCB_SETTING_GC : !strcmp(v, "ps") ? PCB_SETTING_PS :
            !strcmp(v, "xbox") ? PCB_SETTING_XBOX : !strcmp(v, "switch") ? PCB_SETTING_SWITCH :
            PCB_SETTING_AUTO;
        else if (!strcmp(line, "renderer")) s->renderer =
            (!strcmp(v, "d3d12") || !strcmp(v, "dx12")) ? PCS_RENDERER_D3D12 : PCS_RENDERER_GL;
        else if (!strcmp(line, "language")) {
            int l = pcs_parse_language(v);
            s->language = l < 0 ? PCS_LANG_AUTO : l;
        }
        else if (!strcmp(line, "fps"))     s->fps = pcs_parse_fps(v);
        else if (!strcmp(line, "update_check")) s->update_check = atoi(v) ? 1 : 0;
        else if (!strcmp(line, "update_last"))  s->update_last = strtoll(v, NULL, 10);
        else if (!strcmp(line, "update_skip"))
            snprintf(s->update_skip, sizeof s->update_skip, "%s", v);
        else if (!strcmp(line, "iso"))
            snprintf(s->iso_path, sizeof s->iso_path, "%s", v);
    }
    fclose(f);
    sanitize(s);
    printf("[settings] loaded %s (v%d): %dx%d %s vsync=%d msaa=%d aspect=%s vol=%d "
           "music=%d sfx=%d voice=%d timer=%d\n",
           path, version, s->width, s->height,
           s->display_mode == PCS_FULLSCREEN ? "fullscreen" :
           s->display_mode == PCS_BORDERLESS ? "borderless" : "windowed",
           s->vsync, s->msaa, s->aspect ? "stretch" : "auto", s->volume,
           s->vol_music, s->vol_sfx, s->vol_voice, s->vol_timer);
}

void pcs_load(void) {
    if (s_loaded) return;
    s_loaded = 1;
    load_file(&s_file);
    g_pcs = s_file;
}

void pcs_save(void) {
    char path[1100];
    FILE* f;
    if (!s_loaded) pcs_load();
    settings_path(path, sizeof path);
    f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "[settings] cannot write %s\n", path); return; }
    fprintf(f, "SMB2PC 2\n");
    fprintf(f, "width=%d\nheight=%d\n", s_file.width, s_file.height);
    fprintf(f, "display=%s\n", s_file.display_mode == PCS_FULLSCREEN ? "fullscreen" :
                               s_file.display_mode == PCS_BORDERLESS ? "borderless" : "windowed");
    fprintf(f, "vsync=%d\nmsaa=%d\n", s_file.vsync, s_file.msaa);
    fprintf(f, "aspect=%s\nvolume=%d\n", s_file.aspect ? "stretch" : "auto", s_file.volume);
    fprintf(f, "vol_music=%d\nvol_sfx=%d\nvol_voice=%d\nvol_timer=%d\n",
            s_file.vol_music, s_file.vol_sfx, s_file.vol_voice, s_file.vol_timer);
    fprintf(f, "buttons=%s\n", s_button_style_keys[clampi(s_file.button_style, 0, PCB_SETTING_MENU_COUNT - 1)]);
    fprintf(f, "renderer=%s\n", s_file.renderer == PCS_RENDERER_D3D12 ? "d3d12" : "gl");
    fprintf(f, "language=%s\n", s_language_keys[clampi(s_file.language, 0, PCS_LANG_COUNT - 1)]);
    if (s_file.fps == PCS_FPS_UNCAPPED) fprintf(f, "fps=uncapped\n");
    else if (s_file.fps == PCS_FPS_DISPLAY) fprintf(f, "fps=display\n");
    else fprintf(f, "fps=%d\n", s_file.fps);
    fprintf(f, "update_check=%d\nupdate_last=%lld\n", s_file.update_check, s_file.update_last);
    if (s_file.update_skip[0]) fprintf(f, "update_skip=%s\n", s_file.update_skip);
    if (s_file.iso_path[0]) fprintf(f, "iso=%s\n", s_file.iso_path);
    fclose(f);
    printf("[settings] saved %s\n", path);
}

void pcs_cli_override(int which, const PcSettings* v) {
    if (!s_loaded) pcs_load();
    s_overrides |= which;
    if (which & PCS_OVR_SIZE) { g_pcs.width = v->width; g_pcs.height = v->height; }
    if (which & PCS_OVR_MODE) g_pcs.display_mode = v->display_mode;
    if (which & PCS_OVR_VSYNC) g_pcs.vsync = v->vsync;
    if (which & PCS_OVR_MSAA) g_pcs.msaa = v->msaa;
    if (which & PCS_OVR_RENDERER) g_pcs.renderer = v->renderer;
    if (which & PCS_OVR_LANGUAGE) g_pcs.language = v->language;
    if (which & PCS_OVR_FPS) g_pcs.fps = v->fps;
    sanitize(&g_pcs);
}

/* updater (docs/updater.md) */
void pcs_set_update_check(int on) {
    if (!s_loaded) pcs_load();
    s_file.update_check = g_pcs.update_check = on ? 1 : 0;
    pcs_save();
}
void pcs_set_update_last(long long t) {
    if (!s_loaded) pcs_load();
    s_file.update_last = g_pcs.update_last = t;
    pcs_save();
}
void pcs_set_update_skip(const char* version) {
    if (!s_loaded) pcs_load();
    snprintf(s_file.update_skip, sizeof s_file.update_skip, "%s", version ? version : "");
    snprintf(g_pcs.update_skip, sizeof g_pcs.update_skip, "%s", version ? version : "");
    pcs_save();
}

void pcs_set_iso_path(const char* path) {
    if (!s_loaded) pcs_load();
    snprintf(s_file.iso_path, sizeof s_file.iso_path, "%s", path ? path : "");
    snprintf(g_pcs.iso_path, sizeof g_pcs.iso_path, "%s", path ? path : "");
    pcs_save();
}

/* Legacy GameSettings API (platform.h), used by the ISO path flow. */
void settings_get_defaults(GameSettings* settings) {
    PcSettings d;
    if (!settings) return;
    set_defaults(&d);
    memset(settings, 0, sizeof *settings);
    settings->window_width = d.width;
    settings->window_height = d.height;
    settings->vsync = true;
    settings->msaa_samples = d.msaa;
    settings->master_volume = settings->music_volume = settings->sfx_volume = 1.0f;
    settings->rumble_enabled = true;
}

void settings_load(GameSettings* settings) {
    if (!settings) return;
    pcs_load();
    settings_get_defaults(settings);
    settings->window_width = g_pcs.width;
    settings->window_height = g_pcs.height;
    settings->fullscreen = g_pcs.display_mode != PCS_WINDOWED;
    settings->vsync = g_pcs.vsync != 0;
    settings->msaa_samples = g_pcs.msaa;
    settings->master_volume = g_pcs.volume / 100.0f;
    settings->iso_path = s_file.iso_path[0] ? strdup(s_file.iso_path) : NULL;
}

void settings_save(const GameSettings* settings) {
    if (!settings) return;
    pcs_set_iso_path(settings->iso_path);
}

/* ------------------------------------------------------------------- apply */
int pcs_active_msaa(void) { return s_active_msaa < 0 ? g_pcs.msaa : s_active_msaa; }

/* renderer used by this run (set once the renderer is up) */
static int s_active_renderer = -1;
int pcs_active_renderer(void) { return s_active_renderer < 0 ? g_pcs.renderer : s_active_renderer; }
void pcs_set_active_renderer(int r) { s_active_renderer = r; }

/* frame rate (docs/hfr.md): 60 = no interpolation;
 * anything else takes effect at the next tick */
void pcs_set_active_fps(int fps) { (void)fps; }
int g_pcs_vsync_on(void) { return g_pcs.vsync != 0; }
int pcs_debug_set_fps(int fps) {   /* SMB2_HFR_SWITCH test hook */
    printf("[settings] frame rate -> %d (test switch)\n", fps);
    g_pcs.fps = fps;
    return 0;
}
int pcs_effective_fps(void) {
    extern int platform_display_refresh_hz(void);
    static int s_hz, s_hz_age;
    int f = g_pcs.fps, hz;
    if (f == 60) return 60;
    if (s_hz_age-- <= 0) { s_hz = platform_display_refresh_hz(); s_hz_age = 120; }
    hz = s_hz >= 60 ? s_hz : 0;
    if (f == PCS_FPS_DISPLAY) return hz ? hz : 60;
    /* with V-Sync every present waits for a refresh: never ask for more
     * presents than the display shows, or the 60 Hz game tick falls behind */
    if (g_pcs.vsync && hz && (f == PCS_FPS_UNCAPPED || f > hz)) return hz;
    return f;
}

void pcs_on_window_resized(void) {
    SDL_Window* win = cur_window();
    int w = 0, h = 0;
    if (!win) return;
    platform_drawable_size(&w, &h);
    if (w > 0 && h > 0) render_resize(w, h);
}

int platform_test_offscreen(void);

void pcs_apply_video(void) {
    SDL_Window* win = cur_window();
    int disp = cur_display();
    Uint32 flags;
    if (!win) return;
    flags = SDL_GetWindowFlags(win);
    switch (g_pcs.display_mode) {
    case PCS_WINDOWED: {
        int cw = 0, ch = 0;
        if (flags & SDL_WINDOW_FULLSCREEN) SDL_SetWindowFullscreen(win, 0);
        SDL_GetWindowSize(win, &cw, &ch);
        if (cw != g_pcs.width || ch != g_pcs.height) {
            SDL_SetWindowSize(win, g_pcs.width, g_pcs.height);
            if (!platform_test_offscreen())   /* test hook, platform_sdl2.c */
                SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED_DISPLAY(disp),
                                      SDL_WINDOWPOS_CENTERED_DISPLAY(disp));
        }
        break;
    }
    case PCS_FULLSCREEN: {
        SDL_DisplayMode want, got;
        memset(&want, 0, sizeof want);
        want.w = g_pcs.width;
        want.h = g_pcs.height;
        if (SDL_GetClosestDisplayMode(disp, &want, &got))
            SDL_SetWindowDisplayMode(win, &got);
        if ((flags & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP)
            SDL_SetWindowFullscreen(win, 0);
        SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN);
        break;
    }
    case PCS_BORDERLESS:
        SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);
        break;
    }
    render_set_vsync(g_pcs.vsync);
    pcs_on_window_resized();
    {
        int w = 0, h = 0;
        platform_drawable_size(&w, &h);
        printf("[settings] video: %s %dx%d (drawable %dx%d) vsync=%d\n",
               g_pcs.display_mode == PCS_FULLSCREEN ? "fullscreen" :
               g_pcs.display_mode == PCS_BORDERLESS ? "borderless" : "windowed",
               g_pcs.width, g_pcs.height, w, h, g_pcs.vsync);
        fflush(stdout);
    }
}

void pcs_apply_volume(void) {
    audio_set_master_volume(g_pcs.volume / 100.0f);
    audio_set_category_volume(ADSP_CAT_MUSIC, g_pcs.vol_music / 100.0f);
    audio_set_category_volume(ADSP_CAT_SFX, g_pcs.vol_sfx / 100.0f);
    audio_set_category_volume(ADSP_CAT_VOICE, g_pcs.vol_voice / 100.0f);
    audio_set_category_volume(ADSP_CAT_TIMER, g_pcs.vol_timer / 100.0f);
}

void pcs_apply_startup(void) {
    s_active_msaa = g_pcs.msaa;
    if (g_pcs.display_mode == PCS_FULLSCREEN)
        pcs_apply_video();
    else {
        int w = 0, h = 0;
        pcs_on_window_resized();
        platform_drawable_size(&w, &h);
        printf("[settings] video: %s %dx%d (drawable %dx%d) vsync=%d\n",
               g_pcs.display_mode == PCS_BORDERLESS ? "borderless" : "windowed",
               g_pcs.width, g_pcs.height, w, h, g_pcs.vsync);
        fflush(stdout);
    }
    pcs_apply_volume();
}

/* First start only (no settings.dat): pick the game's own TV Monitor option
 * (modeCtrl.wideMode, 4:3 / 16:9) from the desktop's shape, so 16:9, 16:10
 * and ultrawide screens start in 16:9. A memory card save made later stores
 * the player's own choice, which then wins when the card loads. Automated
 * runs keep the GameCube default (4:3) so their captures stay comparable. */
void pcs_first_run_aspect(int automated) {
    SDL_DisplayMode dm;
    if (!s_first_run || automated) return;
    if (SDL_GetDesktopDisplayMode(cur_display(), &dm) != 0 || dm.h <= 0) return;
    if (dm.w * 2 >= dm.h * 3) {          /* aspect >= 1.5 */
        pc_game_set_wide_mode(1);
        printf("[settings] first start: %dx%d desktop -> 16:9 TV mode\n", dm.w, dm.h);
    }
}

void pcs_toggle_fullscreen(void) {
    g_pcs.display_mode = g_pcs.display_mode == PCS_WINDOWED ? PCS_BORDERLESS : PCS_WINDOWED;
    s_file.display_mode = g_pcs.display_mode;
    s_overrides &= ~PCS_OVR_MODE;
    pcs_apply_video();
    pcs_save();
}

float pc_settings_output_aspect(void) {
    if (g_pcs.aspect == PCS_ASPECT_STRETCH) return 0.0f;
    return pc_game_wide_mode() == 1 ? 16.0f / 9.0f : 4.0f / 3.0f;
}

/* ------------------------------------------------------------ resolutions */
typedef struct { int w, h; } Res;
static Res s_res[96];
static int s_nres;

static void res_add(int w, int h) {
    int i, j;
    for (i = 0; i < s_nres; i++)
        if (s_res[i].w == w && s_res[i].h == h) return;
    if (s_nres >= (int)(sizeof s_res / sizeof s_res[0])) return;
    /* keep sorted by width, then height */
    for (i = 0; i < s_nres; i++)
        if (w < s_res[i].w || (w == s_res[i].w && h < s_res[i].h)) break;
    for (j = s_nres; j > i; j--) s_res[j] = s_res[j - 1];
    s_res[i].w = w; s_res[i].h = h;
    s_nres++;
}

/* Display modes of the window's display, deduplicated (refresh rates and
 * pixel formats collapse).  Windowed mode drops sizes that don't fit the
 * usable desktop area. */
static void build_res_list(int mode) {
    int disp = cur_display(), n, i;
    SDL_Rect ub;
    int have_ub = SDL_GetDisplayUsableBounds(disp, &ub) == 0;
    s_nres = 0;
    n = SDL_GetNumDisplayModes(disp);
    for (i = 0; i < n; i++) {
        SDL_DisplayMode m;
        if (SDL_GetDisplayMode(disp, i, &m) != 0) continue;
        if (m.w < 640 || m.h < 480) continue;
        if (mode == PCS_WINDOWED && have_ub && (m.w > ub.w || m.h > ub.h)) continue;
        res_add(m.w, m.h);
    }
    if (mode == PCS_WINDOWED) {
        static const Res extra[] = { {640, 480}, {960, 720}, {1280, 720}, {1600, 900} };
        for (i = 0; i < (int)(sizeof extra / sizeof extra[0]); i++)
            if (!have_ub || (extra[i].w <= ub.w && extra[i].h <= ub.h))
                res_add(extra[i].w, extra[i].h);
    }
    if (s_nres == 0) { res_add(640, 480); res_add(1280, 720); }
}

static int res_index(int w, int h) {
    int i;
    for (i = 0; i < s_nres; i++)
        if (s_res[i].w == w && s_res[i].h == h) return i;
    return -1;
}

/* ------------------------------------------------------------------ items */
/* PC strings: one per game locale (en, de, fr, es, it, ja), UTF-8 here and
 * converted to the game's font encoding by pc_text (kana only for Japanese:
 * the font has no general kanji). */
typedef const char* const Tr[6];
static Tr s_item_names[PCS_ITEM_COUNT] = {
    { "Display", "Anzeige", "Affichage", "Pantalla", "Schermo", "がめんモード" },
    { "Resolution", "Auflösung", "Résolution", "Resolución", "Risoluzione", "かいぞうど" },
    { "V-Sync", "V-Sync", "V-Sync", "V-Sync", "V-Sync", "Vシンク" },
    { "Frame Rate", "Bildrate", "Fréquence d'images", "Velocidad de fotogramas", "Frequenza fotogrammi", "フレームレート" },
    { "Anti-Aliasing", "Kantenglättung", "Anticrénelage", "Antialiasing", "Antialiasing", "アンチエイリアス" },
    { "Renderer", "Renderer", "Rendu", "Renderizador", "Renderer", "レンダラー" },
    { "Aspect Ratio", "Seitenverhältnis", "Format d'image", "Relación de aspecto", "Proporzioni", "アスペクトひ" },
    { "Volume", "Lautstärke", "Volume", "Volumen", "Volume", "おんりょう" },
    { "Buttons", "Tasten", "Boutons", "Botones", "Pulsanti", "ボタンひょうじ" },
    { "Language", "Sprache", "Langue", "Idioma", "Lingua", "げんご" },
    { "Controllers", "Controller", "Manettes", "Mandos", "Controller", "コントローラー" },
    { "Updates", "Updates", "Mises à jour", "Actualizaciones", "Aggiornamenti", "アップデート" },
    { "Memory Card", "Memory Card", "Carte mémoire", "Tarjeta de memoria", "Memory card", "メモリーカード" },
};
static Tr s_item_descs[PCS_ITEM_COUNT] = {
    { "Windowed, fullscreen or borderless fullscreen.",
      "Fenster, Vollbild oder randloses Vollbild.",
      "Fenêtre, plein écran ou plein écran sans bordure.",
      "Ventana, pantalla completa o completa sin bordes.",
      "Finestra, schermo intero o schermo intero senza bordi.",
      "ウインドウ、フルスクリーン、ボーダーレスから えらびます。" },
    { "Window size, or the display mode in fullscreen.",
      "Fenstergröße, im Vollbild der Anzeigemodus.",
      "Taille de la fenêtre, ou mode d'affichage en plein écran.",
      "Tamaño de la ventana, o el modo en pantalla completa.",
      "Dimensioni della finestra, o la modalità a schermo intero.",
      "ウインドウの おおきさ、または フルスクリーンの かいぞうど。" },
    { "Sync frames to the display refresh.",
      "Bilder mit der Bildwiederholrate synchronisieren.",
      "Synchronise les images avec l'écran.",
      "Sincroniza las imágenes con la pantalla.",
      "Sincronizza i fotogrammi con lo schermo.",
      "がめんの こうしんに あわせて ひょうじします。" },
    { "Frames drawn per second; the game runs at 60, frames between are interpolated.",
      "Bilder pro Sekunde; das Spiel läuft mit 60, Zwischenbilder werden interpoliert.",
      "Images par seconde ; le jeu tourne à 60, les images intermédiaires sont interpolées.",
      "Imágenes por segundo; el juego va a 60 y se interpolan las intermedias.",
      "Fotogrammi al secondo; il gioco gira a 60, quelli intermedi sono interpolati.",
      "1びょうの ひょうじかいすう。ゲームは 60で うごき、あいだを ほかんします。" },
    { "Multisample anti-aliasing. Applies on restart.",
      "Multisample-Kantenglättung. Gilt nach Neustart.",
      "Anticrénelage multi-échantillon. Au redémarrage.",
      "Antialiasing multimuestra. Al reiniciar.",
      "Antialiasing multicampione. Al riavvio.",
      "マルチサンプル アンチエイリアス。さいきどうごに はんえい。" },
    { "Graphics API (OpenGL or Direct3D 12). Applies on restart.",
      "Grafik-API (OpenGL oder Direct3D 12). Gilt nach Neustart.",
      "API graphique (OpenGL ou Direct3D 12). Au redémarrage.",
      "API gráfica (OpenGL o Direct3D 12). Al reiniciar.",
      "API grafica (OpenGL o Direct3D 12). Al riavvio.",
      "グラフィックスAPI(OpenGLか Direct3D 12)。さいきどうごに はんえい。" },
    { "Auto keeps 4:3 or 16:9 to match TV monitor.",
      "Auto hält 4:3 oder 16:9 passend zum TV-Monitor.",
      "Auto garde 4:3 ou 16:9 selon le moniteur TV.",
      "Auto mantiene 4:3 o 16:9 según el monitor de TV.",
      "Auto mantiene 4:3 o 16:9 secondo il monitor TV.",
      "オートは TVモニターに あわせて 4:3か 16:9に します。" },
    { "Master volume.", "Gesamtlautstärke.", "Volume général.", "Volumen general.",
      "Volume generale.", "ぜんたいの おんりょう。" },
    { "Button prompts. Auto follows the controller.",
      "Tastensymbole. Auto folgt dem Controller.",
      "Icônes des boutons. Auto suit la manette.",
      "Iconos de botones. Auto sigue al mando.",
      "Icone dei pulsanti. Auto segue il controller.",
      "ボタンの ひょうじ。オートは コントローラに あわせます。" },
    { "Game language. Applies on restart.",
      "Spielsprache. Gilt nach Neustart.",
      "Langue du jeu. Au redémarrage.",
      "Idioma del juego. Al reiniciar.",
      "Lingua del gioco. Al riavvio.",
      "ゲームの げんご。さいきどうごに はんえい。" },
    { "Analog sticks: GameCube gate, dead zone, sensitivity, calibration.",
      "Analog-Sticks: GameCube-Form, Totzone, Empfindlichkeit, Kalibrierung.",
      "Sticks analogiques : forme GameCube, zone morte, sensibilité, calibrage.",
      "Palancas: forma GameCube, zona muerta, sensibilidad, calibración.",
      "Levette: forma GameCube, zona morta, sensibilità, calibrazione.",
      "スティックの かたち、あそび、かんど、キャリブレーション。" },
    { "Check for new versions (automatic check, check now, version info).",
      "Nach neuen Versionen suchen (automatisch, jetzt suchen, Version).",
      "Rechercher une nouvelle version (automatique, maintenant, version).",
      "Buscar versiones nuevas (automático, buscar ahora, versión).",
      "Cerca nuove versioni (automatico, cerca ora, versione).",
      "あたらしい バージョンを さがします（じどう、いますぐ、バージョン）。" },
    { "Import a save from Dolphin or a GameCube memory card, or export yours.",
      "Spielstand aus Dolphin oder von einer GameCube-Memory-Card importieren oder exportieren.",
      "Importer une sauvegarde de Dolphin ou d'une carte mémoire GameCube, ou exporter la vôtre.",
      "Importar una partida de Dolphin o de una tarjeta de GameCube, o exportar la tuya.",
      "Importa un salvataggio da Dolphin o da una memory card GameCube, o esporta il tuo.",
      "Dolphinや ゲームキューブの メモリーカードの セーブを よみこむ、または かきだします。" },
};
static Tr s_tr_windowed = { "Windowed", "Fenster", "Fenêtre", "Ventana", "Finestra", "ウインドウ" };
static Tr s_tr_fullscreen = { "Fullscreen", "Vollbild", "Plein écran", "Completa", "Schermo intero", "フルスクリーン" };
static Tr s_tr_borderless = { "Borderless", "Randlos", "Sans bordure", "Sin bordes", "Senza bordi", "ボーダーレス" };
static Tr s_tr_desktop = { "Desktop", "Desktop", "Bureau", "Escritorio", "Desktop", "デスクトップ" };
static Tr s_tr_on = { "On", "Ein", "Oui", "Sí", "Sì", "オン" };
static Tr s_tr_off = { "Off", "Aus", "Non", "No", "No", "オフ" };
static Tr s_tr_uncapped = { "Uncapped", "Unbegrenzt", "Illimité", "Sin límite", "Illimitato", "むせいげん" };
static Tr s_tr_display = { "Display", "Anzeige", "Écran", "Pantalla", "Schermo", "ディスプレイ" };
static Tr s_tr_restart = { " (restart)", " (Neustart)", " (redémarrage)", " (reiniciar)", " (riavvio)", " (さいきどう)" };
static Tr s_tr_auto = { "Auto", "Auto", "Auto", "Auto", "Auto", "オート" };
static Tr s_tr_stretch = { "Stretch", "Strecken", "Étirer", "Estirar", "Allarga", "ひきのばし" };
/* language names: each in its own language (kana for Japanese) */
static const char* const s_lang_names[PCS_LANG_COUNT] = {
    NULL, "English", "Deutsch", "Français", "Español", "Italiano", "にほんご" };

static const int s_msaa_vals[] = { 0, 2, 4, 8 };
/* Display (match the refresh rate) comes first after 60: the natural choice
 * when turning interpolation on */
static const int s_fps_vals[] = { 60, PCS_FPS_DISPLAY, 120, 144, 165, 240, PCS_FPS_UNCAPPED };
#define FPS_NVALS ((int)(sizeof s_fps_vals / sizeof s_fps_vals[0]))
static PcSettings s_edit_saved;

const char* pcs_item_name(int item) {
    return (item >= 0 && item < PCS_ITEM_COUNT) ? pc_text_pick(s_item_names[item]) : "";
}
const char* pcs_item_desc(int item) {
    return (item >= 0 && item < PCS_ITEM_COUNT) ? pc_text_pick(s_item_descs[item]) : "";
}

bool pcs_item_enabled(int item) {
#ifndef SMB2_HAVE_D3D12
    if (item == PCS_ITEM_RENDERER) return false;   /* OpenGL only */
#endif
    return !(item == PCS_ITEM_RESOLUTION && g_pcs.display_mode == PCS_BORDERLESS);
}

static void add_restart(char* buf, size_t n) {
    strncat(buf, pc_text_pick(s_tr_restart), n - strlen(buf) - 1);
}

const char* pcs_item_value(int item) {
    static char buf[96];
    switch (item) {
    case PCS_ITEM_DISPLAY:
        return pc_text_pick(g_pcs.display_mode == PCS_FULLSCREEN ? s_tr_fullscreen :
                            g_pcs.display_mode == PCS_BORDERLESS ? s_tr_borderless : s_tr_windowed);
    case PCS_ITEM_RESOLUTION:
        if (g_pcs.display_mode == PCS_BORDERLESS) {
            SDL_DisplayMode m;
            if (SDL_GetDesktopDisplayMode(cur_display(), &m) == 0) {
                snprintf(buf, sizeof buf, "%dx%d", m.w, m.h);
                return buf;
            }
            return pc_text_pick(s_tr_desktop);
        }
        snprintf(buf, sizeof buf, "%dx%d", g_pcs.width, g_pcs.height);
        return buf;
    case PCS_ITEM_VSYNC:
        return pc_text_pick(g_pcs.vsync ? s_tr_on : s_tr_off);
    case PCS_ITEM_FPS:
        if (g_pcs.fps == PCS_FPS_UNCAPPED) snprintf(buf, sizeof buf, "%s", pc_text_pick(s_tr_uncapped));
        else if (g_pcs.fps == PCS_FPS_DISPLAY) {
            extern int platform_display_refresh_hz(void);
            int hz = platform_display_refresh_hz();
            if (hz > 0) snprintf(buf, sizeof buf, "%s (%d)", pc_text_pick(s_tr_display), hz);
            else snprintf(buf, sizeof buf, "%s", pc_text_pick(s_tr_display));
        }
        else snprintf(buf, sizeof buf, "%d", g_pcs.fps);
        return buf;
    case PCS_ITEM_MSAA:
        if (g_pcs.msaa) snprintf(buf, sizeof buf, "%dx", g_pcs.msaa);
        else snprintf(buf, sizeof buf, "%s", pc_text_pick(s_tr_off));
        if (g_pcs.msaa != pcs_active_msaa())
            add_restart(buf, sizeof buf);
        return buf;
    case PCS_ITEM_RENDERER:
        snprintf(buf, sizeof buf, "%s", g_pcs.renderer == PCS_RENDERER_D3D12 ? "Direct3D 12" : "OpenGL");
        if (g_pcs.renderer != pcs_active_renderer())
            add_restart(buf, sizeof buf);
        return buf;
    case PCS_ITEM_ASPECT:
        return pc_text_pick(g_pcs.aspect == PCS_ASPECT_STRETCH ? s_tr_stretch : s_tr_auto);
    case PCS_ITEM_VOLUME:
        snprintf(buf, sizeof buf, "%d%%", g_pcs.volume);
        return buf;
    case PCS_ITEM_BUTTONS: {
        static const char* const names[PCB_SETTING_MENU_COUNT] = {
            NULL, "GameCube", "PlayStation", "Xbox", "Switch" };
        int b = clampi(g_pcs.button_style, 0, PCB_SETTING_MENU_COUNT - 1);
        return b == 0 ? pc_text_pick(s_tr_auto) : names[b];
    }
    case PCS_ITEM_LANGUAGE: {
        int l = clampi(g_pcs.language, 0, PCS_LANG_COUNT - 1);
        if (l == PCS_LANG_AUTO) {
            /* "Auto (English)": the language Auto resolves to */
            static const int loc2lang[6] = { PCS_LANG_EN, PCS_LANG_DE, PCS_LANG_FR,
                                             PCS_LANG_ES, PCS_LANG_IT, PCS_LANG_JA };
            snprintf(buf, sizeof buf, "%s (%s)", pc_text_pick(s_tr_auto),
                     pc_text_game(s_lang_names[loc2lang[pcs_language_locale(PCS_LANG_AUTO)]]));
        } else {
            snprintf(buf, sizeof buf, "%s", pc_text_game(s_lang_names[l]));
        }
        if (pcs_language_locale(l) != pcs_active_locale())
            add_restart(buf, sizeof buf);
        return buf;
    }
    case PCS_ITEM_UPDATES:   /* the automatic check; A opens the Updates screen */
        return pc_text_pick(g_pcs.update_check ? s_tr_on : s_tr_off);
    }
    return "";
}

void pcs_item_begin_edit(int item) {
    (void)item;
    s_edit_saved = g_pcs;
    build_res_list(g_pcs.display_mode);
}

bool pcs_item_step(int item, int dir) {
    PcSettings before = g_pcs;
    switch (item) {
    case PCS_ITEM_DISPLAY:
        g_pcs.display_mode = (g_pcs.display_mode + 3 + dir) % 3;
        break;
    case PCS_ITEM_RESOLUTION: {
        int i;
        if (!pcs_item_enabled(item)) return false;
        build_res_list(g_pcs.display_mode);
        i = res_index(g_pcs.width, g_pcs.height);
        if (i < 0) {   /* not a listed size: go to the nearest one by area */
            long area = (long)g_pcs.width * g_pcs.height;
            for (i = 0; i < s_nres - 1 && (long)s_res[i].w * s_res[i].h < area; i++) {}
            if (dir < 0 && i > 0 && (long)s_res[i].w * s_res[i].h >= area) i--;
        } else {
            i = (i + s_nres + dir) % s_nres;
        }
        g_pcs.width = s_res[i].w;
        g_pcs.height = s_res[i].h;
        break;
    }
    case PCS_ITEM_VSYNC:
        g_pcs.vsync = !g_pcs.vsync;
        render_set_vsync(g_pcs.vsync);
        break;
    case PCS_ITEM_FPS: {
        int i;
        for (i = 0; i < FPS_NVALS && s_fps_vals[i] != g_pcs.fps; i++) {}
        if (i == FPS_NVALS) i = 0;
        g_pcs.fps = s_fps_vals[(i + FPS_NVALS + dir) % FPS_NVALS];
        break;
    }
    case PCS_ITEM_MSAA: {
        int i;
        for (i = 0; i < 4 && s_msaa_vals[i] != g_pcs.msaa; i++) {}
        g_pcs.msaa = s_msaa_vals[(i + 4 + dir) % 4];
        break;
    }
    case PCS_ITEM_RENDERER:
        if (!pcs_item_enabled(item)) return false;
        g_pcs.renderer = (g_pcs.renderer + PCS_RENDERER_COUNT + dir) % PCS_RENDERER_COUNT;
        break;
    case PCS_ITEM_ASPECT:
        g_pcs.aspect = !g_pcs.aspect;
        pcs_on_window_resized(); /* re-map the output box now */
        break;
    case PCS_ITEM_VOLUME:
        g_pcs.volume = clampi(g_pcs.volume + dir * 10, 0, 100);
        pcs_apply_volume();
        break;
    case PCS_ITEM_BUTTONS:   /* applies live: button_prompts.c reads g_pcs */
        g_pcs.button_style = (g_pcs.button_style + PCB_SETTING_MENU_COUNT + dir) % PCB_SETTING_MENU_COUNT;
        break;
    case PCS_ITEM_LANGUAGE:  /* applies on restart (the game sets its locale once at boot) */
        g_pcs.language = (g_pcs.language + PCS_LANG_COUNT + dir) % PCS_LANG_COUNT;
        break;
    default:
        return false;
    }
    return memcmp(&before, &g_pcs, sizeof before) != 0;
}

void pcs_item_confirm(int item) {
    switch (item) {
    case PCS_ITEM_DISPLAY:
        if (g_pcs.display_mode != s_edit_saved.display_mode) {
            /* a fullscreen mode size may not fit as a window */
            if (g_pcs.display_mode == PCS_WINDOWED) {
                build_res_list(PCS_WINDOWED);
                if (res_index(g_pcs.width, g_pcs.height) < 0) {
                    int i, best = -1;
                    for (i = 0; i < s_nres; i++)
                        if (s_res[i].w <= g_pcs.width && s_res[i].h <= g_pcs.height) best = i;
                    if (best < 0) best = s_nres - 1;
                    g_pcs.width = s_res[best].w;
                    g_pcs.height = s_res[best].h;
                }
            }
            pcs_apply_video();
        }
        s_file.display_mode = g_pcs.display_mode;
        s_file.width = g_pcs.width;
        s_file.height = g_pcs.height;
        s_overrides &= ~(PCS_OVR_MODE | PCS_OVR_SIZE);
        break;
    case PCS_ITEM_RESOLUTION:
        if (g_pcs.width != s_edit_saved.width || g_pcs.height != s_edit_saved.height)
            pcs_apply_video();
        s_file.width = g_pcs.width;
        s_file.height = g_pcs.height;
        s_overrides &= ~PCS_OVR_SIZE;
        break;
    case PCS_ITEM_VSYNC:
        s_file.vsync = g_pcs.vsync;
        s_overrides &= ~PCS_OVR_VSYNC;
        break;
    case PCS_ITEM_FPS:
        s_file.fps = g_pcs.fps;
        s_overrides &= ~PCS_OVR_FPS;
        break;
    case PCS_ITEM_MSAA:
        s_file.msaa = g_pcs.msaa;
        s_overrides &= ~PCS_OVR_MSAA;
        break;
    case PCS_ITEM_RENDERER:
        s_file.renderer = g_pcs.renderer;
        s_overrides &= ~PCS_OVR_RENDERER;
        break;
    case PCS_ITEM_ASPECT:
        s_file.aspect = g_pcs.aspect;
        break;
    case PCS_ITEM_VOLUME:
        s_file.volume = g_pcs.volume;
        break;
    case PCS_ITEM_BUTTONS:
        s_file.button_style = g_pcs.button_style;
        break;
    case PCS_ITEM_LANGUAGE:
        s_file.language = g_pcs.language;
        s_overrides &= ~PCS_OVR_LANGUAGE;
        break;
    default:
        return;
    }
    if (memcmp(&g_pcs, &s_edit_saved, sizeof g_pcs) != 0)
        printf("[settings] %s -> %s\n", s_item_names[item][0], pcs_item_value(item));
    pcs_save();
}

void pcs_item_cancel(int item) {
    int vs = g_pcs.vsync, vol = g_pcs.volume, asp = g_pcs.aspect;
    (void)item;
    g_pcs = s_edit_saved;
    if (vs != g_pcs.vsync) render_set_vsync(g_pcs.vsync);
    if (vol != g_pcs.volume) pcs_apply_volume();
    if (asp != g_pcs.aspect) pcs_on_window_resized();
}

/* ------------------------------------------------------------ sound mixer */
static int* mix_field(PcSettings* s, int ch) {
    switch (ch) {
    case PCS_MIX_MASTER: return &s->volume;
    case PCS_MIX_MUSIC:  return &s->vol_music;
    case PCS_MIX_SFX:    return &s->vol_sfx;
    case PCS_MIX_VOICE:  return &s->vol_voice;
    case PCS_MIX_TIMER:  return &s->vol_timer;
    }
    return NULL;
}

int pcs_mix_get(int ch) {
    int* f = mix_field(&g_pcs, ch);
    return f ? *f : 0;
}

bool pcs_mix_set(int ch, int value) {
    int* f = mix_field(&g_pcs, ch);
    if (!f) return false;
    value = clampi(value, 0, 100);
    if (value == *f) return false;
    *f = value;
    pcs_apply_volume();
    return true;
}

void pcs_mix_save(void) {
    int ch, changed = 0;
    if (!s_loaded) pcs_load();
    for (ch = 0; ch < PCS_MIX_COUNT; ch++) {
        int* f = mix_field(&s_file, ch);
        if (*f != pcs_mix_get(ch)) {
            *f = pcs_mix_get(ch);
            changed = 1;
        }
    }
    if (!changed) return;
    printf("[settings] mixer: master %d music %d sfx %d voice %d timer %d\n", s_file.volume,
           s_file.vol_music, s_file.vol_sfx, s_file.vol_voice, s_file.vol_timer);
    pcs_save();
}

static Tr s_mix_names[PCS_MIX_COUNT] = {
    { "Master", "Gesamt", "Général", "General", "Generale", "ぜんたい" },
    { "Music", "Musik", "Musique", "Música", "Musica", "おんがく" },
    { "Sound Effects", "Soundeffekte", "Effets sonores", "Efectos", "Effetti sonori", "こうかおん" },
    { "Voices", "Stimmen", "Voix", "Voces", "Voci", "ボイス" },
    { "Timer Warning", "Zeitwarnung", "Alerte chrono", "Cuenta atrás", "Avviso tempo", "タイマーのおと" },
};
static Tr s_mix_descs[PCS_MIX_COUNT] = {
    { "Volume of all sound.",
      "Lautstärke aller Töne.",
      "Volume de tous les sons.",
      "Volumen de todo el sonido.",
      "Volume di tutti i suoni.",
      "すべての おとの おんりょう。" },
    { "Volume of the music.",
      "Lautstärke der Musik.",
      "Volume de la musique.",
      "Volumen de la música.",
      "Volume della musica.",
      "おんがくの おんりょう。" },
    { "Volume of the sound effects.",
      "Lautstärke der Soundeffekte.",
      "Volume des effets sonores.",
      "Volumen de los efectos de sonido.",
      "Volume degli effetti sonori.",
      "こうかおんの おんりょう。" },
    { "Announcer and monkey voices.",
      "Stimmen des Sprechers und der Affen.",
      "Voix de l'annonceur et des singes.",
      "Voces del locutor y de los monos.",
      "Voci dell'annunciatore e delle scimmie.",
      "アナウンスと サルの こえ。" },
    { "Timer ticks and countdown beeps; 0% is off.",
      "Ticken und Countdown-Töne der Zeit; 0% = aus.",
      "Tic-tac et bips du chrono ; 0% = coupé.",
      "Tictac y pitidos del tiempo; 0% = apagado.",
      "Ticchettio e bip del tempo; 0% = spento.",
      "タイマーの カウントの おと。0%で きえます。" },
};
const char* pcs_mix_name(int ch) {
    return (ch >= 0 && ch < PCS_MIX_COUNT) ? pc_text_pick(s_mix_names[ch]) : "";
}
const char* pcs_mix_desc(int ch) {
    return (ch >= 0 && ch < PCS_MIX_COUNT) ? pc_text_pick(s_mix_descs[ch]) : "";
}

/* --------------------------------------------------------------- overlay */
static volatile int s_overlay_req;
void pc_settings_overlay_request(void) { s_overlay_req = 1; }
int pc_settings_overlay_take_request(void) {
    int r = s_overlay_req;
    s_overlay_req = 0;
    return r;
}
