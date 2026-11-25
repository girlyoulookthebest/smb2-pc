/* PC port video/audio settings: persistent model (settings.dat), live apply,
 * and a small "menu item" API shared by the in-game settings page (the
 * game's Options > Game Screen page, see src/game/shim/pc_settings_menu.c)
 * and the F1 settings overlay. */
#ifndef PC_SETTINGS_H
#define PC_SETTINGS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { PCS_WINDOWED = 0, PCS_FULLSCREEN = 1, PCS_BORDERLESS = 2 };
enum { PCS_ASPECT_AUTO = 0, PCS_ASPECT_STRETCH = 1 };
enum { PCS_RENDERER_GL = 0, PCS_RENDERER_D3D12 = 1, PCS_RENDERER_COUNT };
/* Game language (docs/language.md).  Auto follows the Windows UI language. */
enum { PCS_LANG_AUTO = 0, PCS_LANG_EN, PCS_LANG_DE, PCS_LANG_FR, PCS_LANG_ES,
       PCS_LANG_IT, PCS_LANG_JA, PCS_LANG_COUNT };
enum { PCS_FPS_UNCAPPED = 0, PCS_FPS_DISPLAY = -1 };

typedef struct {
    int width, height;   /* window size (windowed) / mode size (fullscreen) */
    int display_mode;    /* PCS_WINDOWED / PCS_FULLSCREEN / PCS_BORDERLESS */
    int vsync;           /* 0/1 */
    int msaa;            /* 0, 2, 4, 8 (applies on restart) */
    int aspect;          /* PCS_ASPECT_AUTO: 4:3 or 16:9 box from the game's
                          * TV-monitor setting; PCS_ASPECT_STRETCH: fill */
    int volume;          /* master volume, 0..100 (Options > Sound > Master) */
    int vol_music;       /* mixer (Options > Sound page, docs/audio.md "Mixer"), */
    int vol_sfx;         /*   0..100 each */
    int vol_voice;
    int vol_timer;       /* stage timer ticks / countdown beeps; 0 = off */
    int button_style;    /* button prompts: 0 auto, 1 GameCube, 2 PlayStation,
                          * 3 Xbox, 4 Switch (PCB_SETTING_*, button_prompts.h) */
    int renderer;        /* PCS_RENDERER_* (applies on restart) */
    int language;        /* PCS_LANG_* (applies on restart) */
    int fps;             /* rendered frames per second (docs/hfr.md): 60 =
                          * one per game tick (no interpolation), N > 60,
                          * PCS_FPS_UNCAPPED, PCS_FPS_DISPLAY (refresh rate) */
    int update_check;    /* automatic update check at start, at most daily (docs/updater.md) */
    long long update_last;   /* time() of the last completed update check, 0 = never */
    char update_skip[32];    /* release the user chose "Skip this version" for */
    char iso_path[1024];
} PcSettings;

/* Effective settings (saved file + command-line overrides). */
extern PcSettings g_pcs;

/* Command-line overrides: values that apply to this run only and are not
 * written back unless the user changes that setting in game. */
enum {
    PCS_OVR_SIZE = 1, PCS_OVR_MODE = 2, PCS_OVR_VSYNC = 4, PCS_OVR_MSAA = 8,
    PCS_OVR_RENDERER = 16, PCS_OVR_LANGUAGE = 32, PCS_OVR_FPS = 64,
};

/* Load settings.dat (or first-run defaults that fit the desktop). Call
 * before platform_init.  Safe to call more than once. */
void pcs_load(void);
void pcs_save(void);
void pcs_cli_override(int which, const PcSettings* v);
void pcs_set_iso_path(const char* path); /* + save */

/* After window + renderer creation: enter exclusive fullscreen if chosen,
 * size the renderer to the drawable, set the master volume. */
void pcs_apply_startup(void);
void pcs_apply_video(void);   /* window mode / size + renderer resize */
void pcs_apply_volume(void);
int  pcs_active_msaa(void);   /* samples the GL context was created with */
int  pcs_active_renderer(void);        /* PCS_RENDERER_* in use this run */
int  pcs_parse_fps(const char* v);     /* "120", "uncapped", "display" -> fps value */
/* effective render rate for the pacer: 60 (off), N, or 0 = uncapped */
int  pcs_effective_fps(void);
void pcs_first_run_aspect(int automated);   /* first start: 16:9 TV mode on wide desktops */
void pcs_set_active_fps(int fps);      /* value the run started with */
void pcs_set_active_renderer(int r);
/* Game locale (0 en, 1 de, 2 fr, 3 es, 4 it, 5 ja) for a PCS_LANG_* value;
 * PCS_LANG_AUTO resolves from the Windows UI language. */
int  pcs_language_locale(int language);
/* Locale this run started with (the game's `locale`, set once at boot like
 * the console's main() does). */
int  pcs_active_locale(void);
void pcs_set_active_locale(int l);
/* Parse a --language / settings.dat value; -1 if unknown. */
int  pcs_parse_language(const char* v);

/* Aspect box for the renderer: 0 = stretch to the window, else w/h. */
float pc_settings_output_aspect(void);
/* The game's modeCtrl.wideMode (0 4:3, 1 16:9, 2 letterbox); provided by
 * the game shim (weak default 0 for standalone builds). */
int pc_game_wide_mode(void);
/* Window size changed (SDL event): resize the renderer to the drawable. */
void pcs_on_window_resized(void);
void pcs_toggle_fullscreen(void); /* Alt+Enter */

/* ---- menu items (shared by both settings screens) ---- */
enum {
    PCS_ITEM_DISPLAY, PCS_ITEM_RESOLUTION, PCS_ITEM_VSYNC, PCS_ITEM_FPS, PCS_ITEM_MSAA,
    PCS_ITEM_RENDERER, PCS_ITEM_ASPECT, PCS_ITEM_VOLUME, PCS_ITEM_BUTTONS,
    PCS_ITEM_LANGUAGE,
    PCS_ITEM_CONTROLLERS,   /* opens the Controller Sticks screen (pc_stick_menu.c) */
    PCS_ITEM_UPDATES,       /* opens the Updates screen (pc_update_ui.c) */
    PCS_ITEM_MEMCARD,       /* opens the Memory Card screen (pc_memcard_ui.c) */
    PCS_ITEM_COUNT
};
const char* pcs_item_name(int item);
const char* pcs_item_desc(int item);
const char* pcs_item_value(int item);   /* static buffer */
bool pcs_item_enabled(int item);        /* e.g. resolution in borderless */
void pcs_item_begin_edit(int item);     /* remember value for cancel */
bool pcs_item_step(int item, int dir);  /* -1/+1; true if changed */
void pcs_item_confirm(int item);        /* apply deferred + save */
void pcs_item_cancel(int item);         /* restore + re-apply */

/* ---- sound mixer (the game's Options > Sound page, pc_settings_menu.c) ---- */
enum { PCS_MIX_MASTER, PCS_MIX_MUSIC, PCS_MIX_SFX, PCS_MIX_VOICE, PCS_MIX_TIMER, PCS_MIX_COUNT };
#define PCS_MIX_STEP 10   /* the old Volume item's step too */
int  pcs_mix_get(int ch);                 /* 0..100 */
/* set + apply live (not saved; pcs_mix_save writes settings.dat) */
bool pcs_mix_set(int ch, int value);
void pcs_mix_save(void);                  /* if anything changed since the last save */
const char* pcs_mix_name(int ch);
const char* pcs_mix_desc(int ch);

/* Controller Sticks screen (src/game/shim/pc_stick_menu.c): run it modally
 * (`captured`: the frame behind is already captured, as in the F1 overlay),
 * or request it from a game screen (opened at the end of the frame). */
void pc_stick_screen_run(int captured);
void pc_stick_screen_request(void);
int  pc_stick_screen_take_request(void);

/* Updater settings (docs/updater.md): written to settings.dat at once. */
void pcs_set_update_check(int on);
void pcs_set_update_last(long long t);
void pcs_set_update_skip(const char* version);

/* F1 overlay request flag (set by the F1 key / "F1" input-script token). */
void pc_settings_overlay_request(void);
int  pc_settings_overlay_take_request(void);

#ifdef __cplusplus
}
#endif
#endif /* PC_SETTINGS_H */
