/* Main entry point for the SMB2 PC port */
#include "platform.h"
#include "input.h"
#include "audio.h"
#include "render.h"
#include "assets.h"
#include "pc_settings.h"
#include "gamedata.h"
#include "install_mode.h"
#include "pc_update.h"
#include "pc_card.h"
#include <SDL2/SDL_hints.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Game state */
typedef enum {
    GAME_STATE_INIT,
    GAME_STATE_RESOLUTION_PICKER,
    GAME_STATE_MAIN_MENU,
    GAME_STATE_GAMEPLAY,
    GAME_STATE_EXIT,
} GameState;

static GameState g_game_state = GAME_STATE_INIT;
static bool g_running = true;

/* Resolution picker defaults */
static int g_window_width = 1280;
static int g_window_height = 720;
static bool g_fullscreen = false;
static int g_display_arg = -1;   /* --display: 0 windowed, 1 borderless, 2 fullscreen */
static bool g_vsync = true;
static int g_msaa = 4;
static const char* g_fps_arg = NULL;  /* --fps 60|120|...|uncapped|display */
static const char* g_iso_arg = NULL;
static const char* g_gamedata_arg = NULL;    /* --gamedata <dir> */
static const char* g_extract_iso_arg = NULL; /* --extract-iso <iso> (headless) */
static const char* g_verify_arg = NULL;      /* --verify-gamedata <dir> (headless) */
static int g_progress_arg = 0;               /* --progress */
static const char* g_renderer_arg = NULL; /* --renderer gl|d3d12 */
static const char* g_language_arg = NULL; /* --language auto|en|de|fr|es|it|ja */
const char* g_save_dir_arg = NULL;    /* --save-dir: settings.dat location */
static int g_cli_overrides = 0;       /* PCS_OVR_* given on the command line */
/* Test hooks (read by input_sdl2.c / platform_sdl2.c); env vars don't cross
 * the WSL->Windows interop boundary here, so these come in as arguments. */
const char* g_input_script_arg = NULL;
long g_dump_interval_arg = 0;
long g_exit_frame_arg = 0;
long g_trace_frame_arg = 0;
const char* g_audio_wav_arg = NULL;   /* --audio-wav: dump mixed output */
int g_audio_null_arg = 0;             /* --audio-null: no audio device */
const char* g_button_style_arg = NULL; /* --button-style: auto|gc|ps|ps4|ps5|xbox|switch */

/* Forward declarations from decompiled game (weak stubs for standalone build) */
#if !defined(SMB2_LINK_GAME)
__attribute__((weak)) void game_init(void) { printf("[STUB] game_init\n"); }
__attribute__((weak)) void game_main_loop(void) { }
__attribute__((weak)) void game_shutdown(void) { printf("[STUB] game_shutdown\n"); }
__attribute__((weak)) void game_set_resolution(int width, int height) {
    printf("[STUB] game_set_resolution(%d, %d)\n", width, height);
}
__attribute__((weak)) void game_set_fullscreen(bool fullscreen) {
    printf("[STUB] game_set_fullscreen(%s)\n", fullscreen ? "true" : "false");
}
#else
/* Real game: the decompiled main_loop.rel entry point (init + frame loop).
 * GC SDK services come from the shim layer in src/game/shim/. */
extern void _prolog(void);
extern int gc_mem_init(void);
extern void pc_check_address_space(void);
#endif

void show_resolution_picker(void) {
    int width = g_window_width;
    int height = g_window_height;
    bool fullscreen = g_fullscreen;

    if (platform_show_resolution_picker(&width, &height, &fullscreen)) {
        g_window_width = width;
        g_window_height = height;
        g_fullscreen = fullscreen;

        /* Apply to platform */
        if (g_platform.vtable && g_platform.vtable->set_fullscreen) {
            g_platform.vtable->set_fullscreen(&g_platform, fullscreen);
        }
        /* the window size is not applied (unused picker) */
    }
}

int main(int argc, char* argv[]) {
    /* Per-monitor DPI awareness must be declared before anything initialises
     * SDL video (pc_settings sizes the default window early).  Without it,
     * Windows display scaling (e.g. 150% on a 3840x2160 screen) presents a
     * scaled 2560x1440 desktop and stretches our output back up. */
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
#ifdef SMB2_LINK_GAME
    extern void shim_install_crash_handler(void);
    extern void shim_start_watchdog(void);
    shim_install_crash_handler();
#endif
    /* Headless installer modes: no window, audio or game, only the
     * PROGRESS/RESULT protocol on stdout (docs/installer.md). */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--extract-iso") == 0 || strcmp(argv[i], "--verify-gamedata") == 0) {
            const char* iso = NULL; const char* gd = NULL; const char* ver = NULL;
            bool progress = false, bad = false;
            for (int j = 1; j < argc; j++) {
                if (strcmp(argv[j], "--extract-iso") == 0 && j + 1 < argc) iso = argv[++j];
                else if (strcmp(argv[j], "--verify-gamedata") == 0 && j + 1 < argc) ver = argv[++j];
                else if (strcmp(argv[j], "--gamedata") == 0 && j + 1 < argc) gd = argv[++j];
                else if (strcmp(argv[j], "--progress") == 0) progress = true;
                else if (strcmp(argv[j], "--extract-iso") == 0 || strcmp(argv[j], "--verify-gamedata") == 0 ||
                         strcmp(argv[j], "--gamedata") == 0) bad = true;
            }
            if (bad || (iso && ver) || (iso && !gd) || (!iso && !ver)) {
                printf("RESULT ERROR %d usage: --extract-iso <iso> --gamedata <dir> [--progress]"
                       " | --verify-gamedata <dir> [--progress]\n", GD_ERR_OTHER);
                fflush(stdout);
                return GD_ERR_OTHER;
            }
            return iso ? gamedata_cli_extract(iso, gd, progress) : gamedata_cli_verify(ver, progress);
        }
    }
    printf("DEBUG: main() started\n");
    fflush(stdout);
    printf("========================================\n");
    printf("  SMB2 - unofficial PC port\n");
    printf("========================================\n\n");
    fflush(stdout);

    /* Parse command line arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            g_window_width = atoi(argv[++i]);
            g_cli_overrides |= PCS_OVR_SIZE;
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            g_window_height = atoi(argv[++i]);
            g_cli_overrides |= PCS_OVR_SIZE;
        } else if (strcmp(argv[i], "--fullscreen") == 0) {
            g_fullscreen = true;
            g_cli_overrides |= PCS_OVR_MODE;
        } else if (strcmp(argv[i], "--windowed") == 0) {
            g_fullscreen = false;
            g_cli_overrides |= PCS_OVR_MODE;
        } else if (strcmp(argv[i], "--display") == 0 && i + 1 < argc) {
            /* windowed | borderless | fullscreen (exclusive), e.g. for
             * frame pacing measurements in automated runs */
            const char* m = argv[++i];
            g_display_arg = !strcmp(m, "fullscreen") ? 2 : !strcmp(m, "borderless") ? 1 : 0;
            g_fullscreen = g_display_arg != 0;
            g_cli_overrides |= PCS_OVR_MODE;
        } else if (strcmp(argv[i], "--no-vsync") == 0) {
            g_vsync = false;
            g_cli_overrides |= PCS_OVR_VSYNC;
        } else if (strcmp(argv[i], "--msaa") == 0 && i + 1 < argc) {
            g_msaa = atoi(argv[++i]);
            g_cli_overrides |= PCS_OVR_MSAA;
        } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            g_fps_arg = argv[++i];
            g_cli_overrides |= PCS_OVR_FPS;
        } else if (strcmp(argv[i], "--dump-from") == 0 && i + 1 < argc) {
            extern long g_dump_from_arg;
            g_dump_from_arg = atol(argv[++i]);
        } else if (strcmp(argv[i], "--dump-ranges") == 0 && i + 1 < argc) {
            extern void platform_set_dump_ranges(const char* spec);
            platform_set_dump_ranges(argv[++i]);
        } else if (strcmp(argv[i], "--renderer") == 0 && i + 1 < argc) {
            g_renderer_arg = argv[++i];
        } else if (strcmp(argv[i], "--language") == 0 && i + 1 < argc) {
            g_language_arg = argv[++i];
        } else if (strcmp(argv[i], "--save-dir") == 0 && i + 1 < argc) {
            g_save_dir_arg = argv[++i];
        } else if (strcmp(argv[i], "--iso") == 0 && i + 1 < argc) {
            g_iso_arg = argv[++i];
        } else if (strcmp(argv[i], "--gamedata") == 0 && i + 1 < argc) {
            g_gamedata_arg = argv[++i];
        } else if (strcmp(argv[i], "--input-script") == 0 && i + 1 < argc) {
            g_input_script_arg = argv[++i];
        } else if (strcmp(argv[i], "--dump-interval") == 0 && i + 1 < argc) {
            g_dump_interval_arg = atol(argv[++i]);
        } else if (strcmp(argv[i], "--exit-frame") == 0 && i + 1 < argc) {
            g_exit_frame_arg = atol(argv[++i]);
        } else if (strcmp(argv[i], "--trace-frame") == 0 && i + 1 < argc) {
            g_trace_frame_arg = atol(argv[++i]);
        } else if (strcmp(argv[i], "--audio-wav") == 0 && i + 1 < argc) {
            g_audio_wav_arg = argv[++i];
        } else if (strcmp(argv[i], "--button-style") == 0 && i + 1 < argc) {
            g_button_style_arg = argv[++i];
        } else if (strcmp(argv[i], "--audio-null") == 0) {
            g_audio_null_arg = 1;
        } else if (strcmp(argv[i], "--perf") == 0) {
            extern int g_perf_on;
            g_perf_on = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [options]\n", argv[0]);
            printf("Options:\n");
            printf("  (video options override settings.dat for this run; change them\n");
            printf("   in game: Options > Game Screen, or F1 anywhere)\n");
            printf("  --width <w>       Window width (default: 1280 or fit desktop)\n");
            printf("  --height <h>      Window height (default: 720)\n");
            printf("  --fullscreen      Start in (borderless) fullscreen\n");
            printf("  --windowed        Start in windowed mode\n");
            printf("  --display <m>     windowed, borderless or fullscreen (exclusive)\n");
            printf("  --no-vsync        Disable VSync\n");
            printf("  --msaa <n>        MSAA samples (default: 4)\n");
            printf("  --fps <n>         Rendered frames per second: 60 (default, no\n"
                   "                    interpolation), 120, 144, 240, ..., uncapped or\n"
                   "                    display (refresh rate); the game still runs at 60\n");
            printf("  --renderer <r>    Renderer: gl or d3d12 (default: settings.dat, else gl)\n");
            printf("  --language <l>    Game language: auto, en, de, fr, es, it or ja\n"
                   "                    (this run only; default: settings.dat, else auto)\n");
            printf("  --save-dir <dir>  Directory for settings.dat and saves\n");
            printf("  --iso <file>      Game disc image (GM2E8P .iso/.gcm)\n");
            printf("  --gamedata <dir>  Extracted game data folder (instead of an ISO)\n");
            printf("  --extract-iso <file> --gamedata <dir> [--progress]\n"
                   "                    Extract + verify the disc into <dir> (headless)\n");
            printf("  --verify-gamedata <dir> [--progress]\n"
                   "                    Check <dir> against its manifest.txt (headless)\n");
            printf("  --import-save <file>\n"
                   "                    Import a GameCube save: a .gci file (Dolphin Memory\n"
                   "                    Card Manager > Export GCI, GCMM) or a whole memory\n"
                   "                    card image (.raw); then exit\n");
            printf("  --export-save <folder>\n"
                   "                    Write the game's saves as .gci files; then exit\n");
            printf("  --audio-wav <f>   Write the mixed audio output to a WAV file\n");
            printf("  --audio-null      Run audio without an output device\n");
            printf("  --button-style <s> Button prompts: auto, gc, ps, ps4, ps5, xbox or switch\n"
                   "                    (this run only)\n");
            printf("  --perf            Print frame timing / GL counters every 300 frames\n");
            printf("  --help, -h        Show this help\n");
            return 0;
        }
    }

    /* install.ini next to the exe (installed mode); absent = portable */
    pc_install_init(g_save_dir_arg);
#ifdef SMB2_LINK_GAME
    {   /* save import/export from the command line: before the log
         * redirect, so the result shows in the console */
        int rc = pc_gci_cli(argc, argv);
        if (rc >= 0)
            return rc;
    }
#endif
    pc_install_setup_logs();
#ifdef SMB2_LINK_GAME
    pc_gci_startup();   /* memcard\import\ drop-in saves, before the game reads the card */
#endif

    /* Saved settings (or first-run defaults), then command-line overrides */
    pcs_load();
    /* updater (docs/updater.md): version to the log, automatic check on a
     * background thread (at most daily; not in automated runs) */
    pc_update_startup(g_input_script_arg || g_dump_interval_arg || g_exit_frame_arg ||
                      getenv("SMB2_INPUT_SCRIPT"));
    /* Automated runs (input scripts, frame dumps, --exit-frame) stay
     * windowed unless asked otherwise, so they never take over the screen. */
    if (!(g_cli_overrides & PCS_OVR_MODE) &&
        (g_input_script_arg || g_dump_interval_arg || g_exit_frame_arg || getenv("SMB2_INPUT_SCRIPT"))) {
        g_fullscreen = false;
        g_cli_overrides |= PCS_OVR_MODE;
    }
    {
        PcSettings ovr = g_pcs;
        ovr.width = g_window_width;
        ovr.height = g_window_height;
        if (g_cli_overrides & PCS_OVR_MODE)
            ovr.display_mode = g_display_arg == 2 ? PCS_FULLSCREEN : g_fullscreen ? PCS_BORDERLESS : PCS_WINDOWED;
        ovr.vsync = g_vsync;
        ovr.msaa = g_msaa;
        if (g_fps_arg) ovr.fps = pcs_parse_fps(g_fps_arg);
        if (g_cli_overrides & PCS_OVR_SIZE) {   /* --width alone keeps the saved height */
            int w_given = 0, h_given = 0;
            for (int i = 1; i < argc; i++) {
                if (!strcmp(argv[i], "--width")) w_given = 1;
                if (!strcmp(argv[i], "--height")) h_given = 1;
            }
            if (!w_given) ovr.width = g_pcs.width;
            if (!h_given) ovr.height = g_pcs.height;
        }
        {   /* renderer: --renderer > SMB2_RENDERER > settings.dat > gl */
            const char* r = g_renderer_arg ? g_renderer_arg : getenv("SMB2_RENDERER");
            if (r && r[0]) {
                ovr.renderer = (!strcmp(r, "d3d12") || !strcmp(r, "dx12") || !strcmp(r, "D3D12"))
                             ? PCS_RENDERER_D3D12 : PCS_RENDERER_GL;
                g_cli_overrides |= PCS_OVR_RENDERER;
            }
        }
        if (g_language_arg) {
            int l = pcs_parse_language(g_language_arg);
            if (l < 0)
                fprintf(stderr, "unknown --language '%s' (auto, en, de, fr, es, it, ja)\n", g_language_arg);
            else {
                ovr.language = l;
                g_cli_overrides |= PCS_OVR_LANGUAGE;
            }
        }
        pcs_cli_override(g_cli_overrides, &ovr);
        g_window_width = g_pcs.width;
        g_window_height = g_pcs.height;
        g_fullscreen = g_pcs.display_mode == PCS_BORDERLESS;
        g_vsync = g_pcs.vsync != 0;
        g_msaa = g_pcs.msaa;
    }

    /* Platform configuration */
    PlatformConfig config = {
        .window_width = g_window_width,
        .window_height = g_window_height,
        .fullscreen = g_fullscreen,
        .vsync = g_vsync,
        .msaa_samples = g_msaa,
        .window_title = "SMB2",
        .opengl = g_pcs.renderer != PCS_RENDERER_D3D12,
    };

    /* Locate the game data before the window exists: --gamedata, --iso,
     * install.ini gamedata, saved settings.dat ISO path, or the file dialog
     * (platform_resolve_game_data).  A (fullscreen) game window would cover
     * the dialog. */
    if (!platform_resolve_game_data(g_gamedata_arg, g_iso_arg)) {
        pc_user_msg("No game data - exiting.\n");
        return 1;
    }

    /* Initialize platform */
    printf("DEBUG: Calling platform_init...\n");
    fflush(stdout);
    if (!platform_init(&config)) {
        fprintf(stderr, "Failed to initialize platform\n");
        return 1;
    }
    printf("DEBUG: platform_init returned\n");
    fflush(stdout);

    /* Video settings are in game (Options > Game Screen, F1);
     * show_resolution_picker() is unused. */
    (void)show_resolution_picker;

    platform_apply_window_icon();
#ifdef SMB2_LINK_GAME
    /* after the (possibly interactive / extracting) data step: the watchdog
     * expects a frame every 8 s */
    shim_start_watchdog();
#endif


    /* Initialize subsystems */
    printf("DEBUG: Initializing renderer...\n");
    fflush(stdout);
    {
        int use_d3d12 = g_pcs.renderer == PCS_RENDERER_D3D12;
        bool ok = render_init(use_d3d12 ? "d3d12" : "gl", g_window_width, g_window_height,
                              g_vsync, g_msaa);
        if (!ok && use_d3d12) {
            fprintf(stderr, "D3D12 renderer unavailable - falling back to OpenGL\n");
            use_d3d12 = 0;
            ok = platform_recreate_window(true) &&
                 render_init("gl", g_window_width, g_window_height, g_vsync, g_msaa);
        }
        if (!ok) {
            fprintf(stderr, "Failed to initialize renderer\n");
            platform_shutdown();
            return 1;
        }
        pcs_set_active_renderer(use_d3d12 ? PCS_RENDERER_D3D12 : PCS_RENDERER_GL);
    }
    printf("DEBUG: Renderer initialized\n");
    fflush(stdout);
    pcs_apply_startup();   /* exclusive fullscreen, drawable size */
    pcs_first_run_aspect(g_input_script_arg || g_dump_interval_arg || g_exit_frame_arg ||
                         getenv("SMB2_INPUT_SCRIPT"));

    printf("DEBUG: Initializing audio...\n");
    fflush(stdout);
    if (!audio_init()) {
        fprintf(stderr, "Failed to initialize audio\n");
    }
    printf("DEBUG: Audio initialized\n");
    fflush(stdout);
    pcs_apply_volume();

    printf("DEBUG: Initializing input...\n");
    fflush(stdout);
    input_init();
    printf("DEBUG: Input initialized\n");
    fflush(stdout);

    printf("DEBUG: Initializing assets...\n");
    fflush(stdout);
    if (!assets_init("assets")) {
        fprintf(stderr, "Failed to initialize asset manager\n");
    }
    printf("DEBUG: Assets initialized\n");
    fflush(stdout);

#ifdef SMB2_LINK_GAME
    /* Map GameCube main RAM at 0x80000000 and hand control to the game.
     * _prolog() contains the game's own init + infinite frame loop; the
     * shim's video_finish_frame() pumps SDL and exits on window close. */
    gc_mem_init();
    pc_check_address_space();
    {   /* The game's language: the DOL's main() sets `locale` once at boot
         * (US disc: always 0, 0x800069F8); the port's main stands in for it
         * and sets the chosen one.  docs/language.md */
        extern int locale;
        int l = pcs_language_locale(g_pcs.language);
        extern void pc_font_ext_init(int locale);
        locale = l;
        pcs_set_active_locale(l);
        printf("[lang] locale %d (%s)\n", l, "en\0de\0fr\0es\0it\0ja" + 3 * l);
        pc_font_ext_init(l);   /* Japanese: the font's kanji pages */
    }
    printf("\nHanding off to SMB2 _prolog()...\n\n");
    fflush(stdout);
    _prolog();

    printf("\nGame loop exited - shutting down.\n");
    assets_shutdown();
    input_shutdown();
    audio_shutdown();
    render_shutdown();
    platform_shutdown();
    return 0;
#else
    /* Initialize game */
    game_init();
    game_set_resolution(g_window_width, g_window_height);
    game_set_fullscreen(g_fullscreen);

    g_game_state = GAME_STATE_MAIN_MENU;
    g_running = true;

    printf("\nStarting main loop...\n");
    printf("Press ESC or close window to exit\n\n");

    uint64_t last_time = platform_get_ticks_ms();
    const double target_frame_time = 1000.0 / 60.0; /* 60 FPS */

    /* Main loop */
    while (g_running && !platform_should_close()) {
        uint64_t current_time = platform_get_ticks_ms();
        double dt = (current_time - last_time) / 1000.0;
        last_time = current_time;

        /* Cap frame time */
        if (dt > 0.1) dt = 0.1;

        /* Process events */
        platform_poll_events();
        process_inputs();
        threshold_analog_inputs();
        g_calc_frames_since_last_input_change();
        merge_inputs();

        /* Update audio */
        audio_update();

        /* Render frame */
        render_begin_frame();

        if (g_game_state == GAME_STATE_MAIN_MENU) {
            /* standalone build: no menu to draw */
        } else if (g_game_state == GAME_STATE_GAMEPLAY) {
            game_main_loop();
        }

        render_end_frame();
        platform_swap_buffers();

        /* Frame rate limiting */
        uint64_t frame_end = platform_get_ticks_ms();
        double frame_time = frame_end - current_time;
        if (frame_time < target_frame_time) {
            platform_sleep_ms((uint32_t)(target_frame_time - frame_time));
        }
    }

    printf("\nShutting down...\n");

    /* Cleanup */
    game_shutdown();
    assets_shutdown();
    input_shutdown();
    audio_shutdown();
    render_shutdown();
    platform_shutdown();

    printf("Goodbye!\n");
    return 0;
#endif /* SMB2_LINK_GAME */
}