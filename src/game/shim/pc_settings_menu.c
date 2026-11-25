/* PC settings UI drawn with the game's own UI pieces (wood option box, cursor
 * bullet, menu font, menu sounds).
 *
 *  1. Options > Game Screen page (sel_ngc).  sel_ngc.c menu_option_screen_tick
 *     calls pc_screen_page_create/tick/destroy here instead of its
 *     fn_3_20618/fn_3_20698/fn_3_20A28 (#ifndef __MWERKS__ hook).  The page
 *     keeps the game's two rows (TV monitor = modeCtrl.wideMode, split screen)
 *     and adds the PC video/audio rows from pc_settings.c.  Same structure and
 *     controls as the original page: up/down pick a row, A to edit, left/right
 *     to change, A to confirm, B to cancel (B on the list leaves the page).
 *
 *  2. F1 overlay (pc_settings_overlay_run), opened from video_finish_frame on
 *     any screen: freezes the game on its last frame and shows the PC rows in
 *     a modal loop.
 *
 *  Both end with a "Quit to Desktop" row (pc_quit.c): A opens the game's
 *  yes/no box (default No); Yes requests the quit, which video_finish_frame
 *  carries out once no memory card job is running.
 *
 * Also provides pc_game_wide_mode() for the renderer's aspect box. */
#include "gc_intrinsics.h"

struct StickPoint;
struct MemcardRequest;
struct TextBox;
#include "sel_ngc.h"

#include "pc_settings.h"
#include "pc_quit.h"
#include "pc_text.h"
#include "pc_update.h"
#include "pc_memcard_ui.h"

/* ---- game symbols not (correctly) declared in the headers ---- */
struct ScreenOption {
    char **name;
    char **desc;
    u8 *value;
    s32 count;
    char ***options;
    void (*callback)(void);
};
extern struct ScreenOption lbl_3_data_F9B0[2];   /* sel_ngc_option.c */
s32 did_any_pad_press_input(u32 input);          /* sel_ngc.c */
void g_b_button_pressed_on_menu(s32 arg0);       /* sel_ngc.c */
void fn_3_140CC(int counter);                    /* sel_ngc_sprite.c */
void C_MTXOrtho(Mtx44 m, float t, float b, float l, float r, float n, float f);
float textdraw_get_pixel_width_of_string(char *str);
#define OS_SOUND_MODE_MONO 0u
extern s32 g_smth_with_sprite_draw_reqs_widescreen; /* bitmap.c */
#define menuWork ((struct SelNgcMenuWork *)&g_currently_visible_menu_screen)

/* platform / renderer */
extern void input_poll_gc_pads(void);
extern void platform_poll_events(void);
extern int  platform_should_close(void);
extern void platform_swap_buffers(void);
extern unsigned long long platform_get_time_ns(void);
extern void platform_wait_until_ns(unsigned long long target_ns);
extern int  pc_frame_capture(void);
extern void pc_frame_restore(float brightness);
extern void exit(int);
#ifdef _WIN32
extern volatile long g_shim_frame_pulse;
#endif

int pc_game_wide_mode(void) { return modeCtrl.wideMode; }
void pc_game_set_wide_mode(int wide) { modeCtrl.wideMode = wide ? 1 : 0; }

/* ======================================================================
 * Rows of the three lists:
 *  LV_PAGE     Options > Game Screen: the game's 2 rows, the PC items except
 *              the master volume (it is on the Sound page), Quit to Desktop
 *  LV_OVERLAY  F1 overlay: all PC items, Quit to Desktop
 *  LV_SOUND    Options > Sound: the game's Sound Setting (mono / stereo) row,
 *              then the mixer sliders (pcs_mix_*)
 * ====================================================================== */
enum { LV_PAGE, LV_OVERLAY, LV_SOUND };
#define GAME_ROWS 2
#define PAGE_ITEMS (PCS_ITEM_COUNT - 1)
#define QUIT_ROW (GAME_ROWS + PAGE_ITEMS)
#define ROW_COUNT (QUIT_ROW + 1)
#define ROW_SPACING 20.5f   /* F1 overlay: 14 rows */
/* the Options page has to fit 15 rows (2 game + 12 PC + Quit) above the
 * game's description box */
#define PAGE_ROW_SPACING 17.5f
#define PAGE_TEXT_SCALE_Y 0.8f
#define OVERLAY_ROWS (PCS_ITEM_COUNT + 1)   /* PC items + Quit to Desktop */
#define SOUND_ROWS (1 + PCS_MIX_COUNT)

/* Options > Sound page strings (sel_ngc_option.c) */
extern char *lbl_3_data_10294[6];      /* "Sound Setting" */
extern char **lbl_3_data_10358[2];     /* "Mono", "Stereo" */
extern char *lbl_3_data_10340[6];      /* "This is the speaker setting." */

/* PC item of a Game Screen page row (>= GAME_ROWS, < QUIT_ROW) */
static int page_item(int r)
{
    int i = r - GAME_ROWS;
    return i >= PCS_ITEM_VOLUME ? i + 1 : i;
}

static const char *quit_desc(void)
{
    static const char *const desc[6] = {
        "Close the game and return to the desktop.",
        "Spiel beenden und zum Desktop zurückkehren.",
        "Quitter le jeu et revenir au bureau.",
        "Cerrar el juego y volver al escritorio.",
        "Chiudi il gioco e torna al desktop.",
        "ゲームを おわって デスクトップに もどります。",
    };
    return pc_text_pick(desc);
}

static const char *row_name(int kind, int r)
{
    switch (kind) {
    case LV_PAGE:
        if (r < GAME_ROWS) return lbl_3_data_F9B0[r].name[locale];
        if (r == QUIT_ROW) return pc_quit_label();
        return pcs_item_name(page_item(r));
    case LV_OVERLAY:
        if (r == PCS_ITEM_COUNT) return pc_quit_label();
        return pcs_item_name(r);
    default:
        if (r == 0) return lbl_3_data_10294[locale];
        return pcs_mix_name(r - 1);
    }
}

static const char *row_value(int kind, int r)
{
    static char buf[16];
    switch (kind) {
    case LV_PAGE:
        if (r < GAME_ROWS) {
            struct ScreenOption *opt = &lbl_3_data_F9B0[r];
            return opt->options[*opt->value][locale];
        }
        if (r == QUIT_ROW) return "";
        return pcs_item_value(page_item(r));
    case LV_OVERLAY:
        if (r == PCS_ITEM_COUNT) return "";
        return pcs_item_value(r);
    default:
        if (r == 0) return lbl_3_data_10358[OSGetSoundMode() == OS_SOUND_MODE_MONO ? 0 : 1][locale];
        snprintf(buf, sizeof buf, "%d%%", pcs_mix_get(r - 1));
        return buf;
    }
}

static const char *row_desc(int kind, int r)
{
    switch (kind) {
    case LV_PAGE:
        if (r < GAME_ROWS) return lbl_3_data_F9B0[r].desc[locale];
        if (r == QUIT_ROW) return quit_desc();
        return pcs_item_desc(page_item(r));
    case LV_OVERLAY:
        if (r == PCS_ITEM_COUNT) return quit_desc();
        return pcs_item_desc(r);
    default:
        if (r == 0) return lbl_3_data_10340[locale];
        return pcs_mix_desc(r - 1);
    }
}

static int row_enabled(int kind, int r)
{
    switch (kind) {
    case LV_PAGE:
        if (r < GAME_ROWS || r == QUIT_ROW) return 1;
        return pcs_item_enabled(page_item(r));
    case LV_OVERLAY:
        if (r == PCS_ITEM_COUNT) return 1;
        return pcs_item_enabled(r);
    default:
        return 1;
    }
}

/* 0..100 for a slider row, else -1 */
static int row_slider(int kind, int r)
{
    return (kind == LV_SOUND && r >= 1) ? pcs_mix_get(r - 1) : -1;
}

/* ======================================================================
 * Shared list drawing (game's option-page style, fn_3_201C8)
 * ====================================================================== */
struct ListView {
    int kind;            /* LV_* */
    int first, count;    /* rows to show */
    int cursor, editing, timer;
    float x, y;          /* first row's name position */
    float valueX;        /* centre of the value column */
    int boxBmp;          /* ui box bitmap */
    float boxCX, boxCY, boxW, boxH;
    int bullet;          /* draw the 0x1700 cursor bullet (else a ">" glyph) */
    const char *title;   /* optional, above the list */
    const char *hint;    /* optional, below the description */
    const char *version; /* optional, small, top right (F1 overlay) */
    float descY, hintY;
    float spacing;       /* row pitch */
    float textScaleY;    /* row text height scale */
};

static u32 glow_color(int timer)
{
    u32 g = (u32)(64.0 * fabs(math_sin((s16)(timer * 0x180))));
    return g | (g << 8) | (g << 16);
}

/* Sound page slider: ten of the font's own blocks (SJIS 0x81A1), lit up to
 * the value in the selected-row yellow, the rest dim, then the value. */
#define SLIDER_BLOCKS 10
#define SLIDER_SCALE_X 0.55f
static void draw_slider(const struct ListView *v, float y, int pct, int sel)
{
    static char block[3] = { (char)0x81, (char)0xA1, 0 };
    float bw, x0;
    int i, lit = (pct * SLIDER_BLOCKS + 50) / 100;

    textdraw_reset();
    textdraw_set_font(3);
    textdraw_set_flags(0xA0000000U);
    textdraw_set_scale(SLIDER_SCALE_X, 0.6f * v->textScaleY);
    bw = textdraw_get_pixel_width_of_string(block) + 1.0f;
    x0 = v->valueX - 0.5f * bw * SLIDER_BLOCKS;
    for (i = 0; i < SLIDER_BLOCKS; i++) {
        if (i < lit) {
            textdraw_set_mul_color(sel ? 0xFFFF40U : 0xC0C040U);
            textdraw_set_add_color(sel ? glow_color(v->timer) : 0U);
        } else {
            textdraw_set_mul_color(0x504830U);
            textdraw_set_add_color(0U);
        }
        textdraw_set_pos(x0 + bw * (float)i, y + 4.0f * v->textScaleY);
        textdraw_print(block);
    }
    textdraw_reset();
    textdraw_set_font(3);
    textdraw_set_flags(0xA0000000U);
    textdraw_set_scale(0.8f, v->textScaleY);
    textdraw_set_alignment(6);
    textdraw_set_pos(v->valueX + 0.5f * bw * SLIDER_BLOCKS + 72.0f, y);
    textdraw_printf("%d%%", pct);
}

static void draw_list(const struct ListView *v)
{
    int i;
    float bw, bh;

    GXSetZMode_cached(1U, 7, 1U);
    set_ui_widescreen_scale_mtx(0x140);

    init_ui_element_sprite_with_defaults();
    set_ui_element_sprite_depth(0.11f);
    set_ui_element_sprite_pos(v->boxCX, v->boxCY);
    bw = (float)g_get_bmp_width(v->boxBmp);
    bh = (float)g_get_bmp_height(v->boxBmp);
    if (bw > 0.0f && bh > 0.0f)
        set_ui_element_sprite_scale(v->boxW / bw, v->boxH / bh);
    draw_ui_box(v->boxBmp);

    if (v->title != NULL) {
        textdraw_reset();
        textdraw_set_font(3);
        textdraw_set_flags(0xA0000000U);
        textdraw_set_alignment(3);
        textdraw_set_mul_color(0xFFFF40U);
        textdraw_set_scale(1.0f, 1.1f);
        textdraw_set_pos(320.0f, v->y - 40.0f);
        textdraw_print((char *)v->title);
    }
    if (v->version != NULL) {
        textdraw_reset();
        textdraw_set_font(3);
        textdraw_set_flags(0xA0000000U);
        textdraw_set_alignment(3);
        textdraw_set_mul_color(0xA0A0A0U);
        textdraw_set_scale(0.55f, 0.6f);
        textdraw_set_pos(v->boxCX + v->boxW * 0.5f - 50.0f, v->y - 34.0f);
        textdraw_print((char *)v->version);
    }

    /* the Quit to Desktop yes/no box replaces the rows while it is open.
     * Drawn before the hint text: as the overlay's very last draw, its final
     * glyph gets the wrong texture. */
    pc_quit_confirm_draw(320.0f, 240.0f, 0.05f, 0.04f);
    set_ui_widescreen_scale_mtx(0x140);
    for (i = 0; i < (pc_quit_confirm_active() ? 0 : v->count); i++) {
        int r = v->first + i;
        float y = v->y + v->spacing * (float)i;
        int sel = (r == v->cursor);
        int en = row_enabled(v->kind, r);
        int pct = row_slider(v->kind, r);

        /* underline (scaled "_") */
        textdraw_reset();
        textdraw_set_font(3);
        textdraw_set_flags(0x80000000U);
        textdraw_set_alignment(3);
        if (sel) {
            textdraw_set_mul_color(0xFFFF40U);
            textdraw_set_add_color(glow_color(v->timer));
        } else {
            textdraw_set_mul_color(0xC0C040U);
            textdraw_set_add_color(0U);
        }
        textdraw_set_scale(15.0f, 1.0f);
        textdraw_set_pos(320.0f, y);
        textdraw_print("_");

        if (sel) {
            if (v->bullet && g_get_bmp_height(0x1700) > 0) {
                init_ui_element_sprite_with_defaults();
                set_ui_element_sprite_pos(v->x - 26.0f, 12.0f + y);
                set_ui_element_sprite_scale(26.0 / g_get_bmp_width(0x1700),
                                            26.0 / g_get_bmp_height(0x1700));
                draw_ui_element(0x1700);
            } else {
                textdraw_reset();
                textdraw_set_font(3);
                textdraw_set_flags(0xA0000000U);
                textdraw_set_mul_color(0xFFFF40U);
                textdraw_set_add_color(glow_color(v->timer));
                textdraw_set_scale(0.8f, 1.0f);
                textdraw_set_pos(v->x - 30.0f, y);
                textdraw_print(">");
            }
        }

        textdraw_reset();
        textdraw_set_font(3);
        textdraw_set_scale(0.8f, v->textScaleY);
        textdraw_set_flags(0xA0000000U);
        if (!en)
            textdraw_set_mul_color(0x808080U);
        textdraw_set_pos(v->x, y);
        textdraw_print((char *)row_name(v->kind, r));
        if (pct >= 0) {
            draw_slider(v, y, pct, sel);
            continue;
        }
        textdraw_set_alignment(3);
        textdraw_set_pos(v->valueX, y);
        if (v->editing && sel)
            textdraw_printf("<%s>", row_value(v->kind, r));
        else
            textdraw_print((char *)row_value(v->kind, r));
    }

    if (!pc_quit_confirm_active()) {
        textdraw_reset();
        fn_3_140CC(v->timer);
        textdraw_set_pos(320.0f, v->descY);
        textdraw_print_and_fit_to_width(390.0f, (char *)row_desc(v->kind, v->cursor));
    }

    if (v->hint != NULL) {
        textdraw_reset();
        textdraw_set_font(3);
        textdraw_set_flags(0xA0000000U);
        textdraw_set_alignment(3);
        textdraw_set_mul_color(0xC0C0C0U);
        textdraw_set_scale(0.6f, 0.7f);
        textdraw_set_pos(320.0f, v->hintY);
        textdraw_print((char *)v->hint);
    }

    reset_ui_widescreen_scale_mtx();
    GXSetZMode_cached(1U, 3, 1U);
}

/* ======================================================================
 * 1. Options > Game Screen page
 * ====================================================================== */
static struct {
    s32 state;      /* 0 choosing a row, 1 editing */
    s32 cursor;
    s32 savedValue; /* game rows: value restored on B */
    s32 timer;
} s_page;

static void page_draw(struct Sprite *sprite)
{
    struct ListView v;

    memset(&v, 0, sizeof(v));
    v.kind = LV_PAGE;
    v.first = 0;
    v.count = ROW_COUNT;
    v.cursor = s_page.cursor;
    v.editing = (s_page.state == 1);
    v.timer = s_page.timer;
    v.x = sprite->x;
    v.y = sprite->y;
    v.valueX = sprite->x + 250.0f;
    v.spacing = PAGE_ROW_SPACING;
    v.textScaleY = PAGE_TEXT_SCALE_Y;
    v.boxBmp = 0x1701;
    v.boxCX = 320.0f;
    v.boxW = 400.0f;
    v.boxH = v.spacing * (ROW_COUNT - 1) + 24.0f + 24.0f;
    v.boxCY = sprite->y + (v.spacing * (ROW_COUNT - 1) + 24.0f) * 0.5f;
    v.bullet = 1;
    v.descY = 400.0f;
    draw_list(&v);
}

void pc_screen_page_create(void)
{
    struct Sprite *sprite;

    memset(&s_page, 0, sizeof(s_page));
    pc_quit_confirm_close();
    sprite = create_sprite();
    if (sprite != NULL) {
        sprite->x = 170.0f;
        sprite->y = 84.0f;   /* 15 rows at 17.5 px end above the description box */
        sprite->tag = 0x5D;
        sprite->drawFunc = page_draw;
        strcpy(sprite->text, "screen");
    }
}

void pc_screen_page_destroy(void)
{
    pc_quit_confirm_close();
    destroy_sprite_with_unique_id(0x5D);
}

void pc_screen_page_tick(void)
{
    u32 sound = 0;
    s32 sel;
    int idle = (menuWork->unkC == 0 && menuWork->unk18 == 0);

    if (s_page.state == 2) {
        /* Quit to Desktop yes/no box */
        u32 press = 0;
        if (did_any_pad_press_input(0) != 0) press |= PAD_BUTTON_LEFT;
        if (did_any_pad_press_input(1) != 0) press |= PAD_BUTTON_RIGHT;
        if (idle && did_any_pad_press_input(6) != 0) press |= PAD_BUTTON_A;
        if (idle && did_any_pad_press_input(7) != 0) press |= PAD_BUTTON_B;
        switch (pc_quit_confirm_tick(press)) {
        case PCQ_MOVED: sound |= 2; break;
        case PCQ_NO: sound |= 0x10; s_page.state = 0; break;
        case PCQ_YES: sound |= 8; s_page.state = 3; break;
        }
    } else if (s_page.state == 3) {
        /* quitting: ignore input until pc_quit_poll exits */
    } else if (s_page.state == 0) {
        sel = s_page.cursor;
        if (did_any_pad_press_input(2) != 0) {
            if (--sel < 0) sel = ROW_COUNT - 1;
        } else if (did_any_pad_press_input(3) != 0) {
            if (++sel >= ROW_COUNT) sel = 0;
        }
        if (sel != s_page.cursor) {
            sound |= 2;
            s_page.cursor = sel;
        }
        if (did_any_pad_press_input(6) != 0 && idle) {
            if (s_page.cursor == QUIT_ROW) {
                sound |= 8;
                pc_quit_confirm_open();
                s_page.state = 2;
            } else if (s_page.cursor >= GAME_ROWS && page_item(s_page.cursor) == PCS_ITEM_CONTROLLERS) {
                /* Controller Sticks screen, modal at the end of this frame */
                sound |= 8;
                pc_stick_screen_request();
            } else if (s_page.cursor >= GAME_ROWS && page_item(s_page.cursor) == PCS_ITEM_UPDATES) {
                /* Updates screen (pc_update_ui.c), modal at the end of this frame */
                sound |= 8;
                pc_update_screen_request();
            } else if (s_page.cursor >= GAME_ROWS && page_item(s_page.cursor) == PCS_ITEM_MEMCARD) {
                /* Memory Card screen (pc_memcard_ui.c), modal at the end of this frame */
                sound |= 8;
                pc_memcard_screen_request();
            } else if (row_enabled(LV_PAGE, s_page.cursor)) {
                sound |= 8;
                if (s_page.cursor < GAME_ROWS)
                    s_page.savedValue = *lbl_3_data_F9B0[s_page.cursor].value;
                else
                    pcs_item_begin_edit(page_item(s_page.cursor));
                s_page.state = 1;
            } else {
                sound |= 0x10;
            }
        } else if (did_any_pad_press_input(7) != 0 && idle) {
            g_b_button_pressed_on_menu(1);
        }
    } else if (s_page.cursor < GAME_ROWS) {
        struct ScreenOption *opt = &lbl_3_data_F9B0[s_page.cursor];
        sel = *opt->value;
        if (did_any_pad_press_input(0) != 0) {
            if (--sel < 0) sel = opt->count - 1;
        } else if (did_any_pad_press_input(1) != 0) {
            if (++sel >= opt->count) sel = 0;
        }
        if (sel != *opt->value) {
            sound |= 2;
            *opt->value = sel;
            if (opt->callback != NULL) opt->callback();
        }
        if (did_any_pad_press_input(6) != 0 && idle) {
            sound |= 8;
            s_page.state = 0;
        } else if (did_any_pad_press_input(7) != 0 && idle) {
            sound |= 0x10;
            *opt->value = s_page.savedValue;
            if (opt->callback != NULL) opt->callback();
            s_page.state = 0;
        }
    } else {
        int item = page_item(s_page.cursor);
        if (did_any_pad_press_input(0) != 0) {
            if (pcs_item_step(item, -1)) sound |= 2;
        } else if (did_any_pad_press_input(1) != 0) {
            if (pcs_item_step(item, 1)) sound |= 2;
        }
        if (did_any_pad_press_input(6) != 0 && idle) {
            sound |= 8;
            pcs_item_confirm(item);
            s_page.state = 0;
        } else if (did_any_pad_press_input(7) != 0 && idle) {
            sound |= 0x10;
            pcs_item_cancel(item);
            s_page.state = 0;
        }
    }

    if (sound & 2)
        call_SoundReqID_arg_0(0x6C);
    else if (sound & 8)
        call_SoundReqID_arg_0(0x65);
    else if (sound & 0x10)
        call_SoundReqID_arg_0(0x66);
    s_page.timer += 1;
}

/* ======================================================================
 * 3. Options > Sound page (sel_ngc.c fn_3_1119C calls these instead of
 *    fn_3_23DE4 / fn_3_23EA8 / fn_3_242B4, #ifndef __MWERKS__ hook)
 *
 * The original page has one row, Sound Setting (Mono / Stereo: A to edit,
 * left/right to change, A to keep, B to restore), raises the menu music to
 * full volume while it is open and plays a random monkey voice from a
 * random side every second as a speaker test.  The PC page keeps all of
 * that and adds the mixer sliders below the row (pcs_mix_*, applied live,
 * saved to settings.dat): up/down pick a row, left/right move a slider at
 * once, each step plays a preview of that channel (the cursor sound for
 * Master / Music / Sound effects, a monkey voice, the timer tick).
 * ====================================================================== */
#define SOUND_ROW_SPACING 32.0f   /* the game's option pages (fn_3_201C8) */
static struct {
    s32 state;      /* 0 choosing a row, 1 editing Mono / Stereo */
    s32 cursor;
    s32 savedMode;  /* Mono / Stereo restored on B */
    s32 timer;
    s32 lastVoice;  /* speaker test: last cue */
    s32 dirty;      /* mixer changed since the last save */
    s32 idle;       /* frames since the last mixer change */
    s32 previewWait;/* frames until the next voice / timer preview */
} s_snd;

int rand(void);

static void sound_page_draw(struct Sprite *sprite)
{
    struct ListView v;

    memset(&v, 0, sizeof(v));
    v.kind = LV_SOUND;
    v.first = 0;
    v.count = SOUND_ROWS;
    v.cursor = s_snd.cursor;
    v.editing = (s_snd.state == 1);
    v.timer = s_snd.timer;
    v.x = sprite->x;
    v.y = sprite->y;
    v.valueX = sprite->x + 242.0f;
    v.spacing = SOUND_ROW_SPACING;
    v.textScaleY = 1.0f;
    v.boxBmp = 0x1701;
    v.boxCX = 320.0f;
    v.boxW = 470.0f;
    v.boxH = v.spacing * (SOUND_ROWS - 1) + 24.0f + 40.0f;
    v.boxCY = sprite->y + (v.spacing * (SOUND_ROWS - 1) + 24.0f) * 0.5f;
    v.bullet = 1;
    v.descY = 400.0f;
    draw_list(&v);
}

void pc_sound_page_create(void)
{
    struct Sprite *sprite;

    memset(&s_snd, 0, sizeof(s_snd));
    s_snd.lastVoice = -1;
    g_change_music_volume(are_all_party_games_unlocked() != 0 ? 0x5A : 5, 0x1EU, 0x64);
    sprite = create_sprite();
    if (sprite != NULL) {
        sprite->x = 150.0f;
        sprite->y = 210.0f - 12.0f - SOUND_ROW_SPACING * (SOUND_ROWS - 1) * 0.5f;
        sprite->tag = 0x5C;
        sprite->drawFunc = sound_page_draw;
        strcpy(sprite->text, "sound");
    }
}

void pc_sound_page_destroy(void)
{
    pcs_mix_save();
    g_change_music_volume(are_all_party_games_unlocked() != 0 ? 0x5A : 5, 0x1EU, 0x3C);
    destroy_sprite_with_unique_id(0x5C);
}

/* a monkey voice (the speaker test's first cue) or the timer tick, centred */
static void sound_preview(int ch)
{
    if (ch == PCS_MIX_VOICE) {
        g_smth_with_sound = 0;
        g_player_id_for_sound = 0;
        call_SoundReqID_arg_0(0x12D);
    } else if (ch == PCS_MIX_TIMER) {
        g_player_id_for_sound = 0;
        call_SoundReqID_arg_0(6);
    }
}

void pc_sound_page_tick(void)
{
    u32 sound = 0;
    s32 sel;
    int idle = (menuWork->unkC == 0 && menuWork->unk18 == 0);

    if (s_snd.previewWait > 0)
        s_snd.previewWait--;
    if (s_snd.state == 0) {
        sel = s_snd.cursor;
        if (did_any_pad_press_input(2) != 0) {
            if (--sel < 0) sel = SOUND_ROWS - 1;
        } else if (did_any_pad_press_input(3) != 0) {
            if (++sel >= SOUND_ROWS) sel = 0;
        }
        if (sel != s_snd.cursor) {
            sound |= 2;
            s_snd.cursor = sel;
        } else if (s_snd.cursor >= 1) {
            int ch = s_snd.cursor - 1, v = pcs_mix_get(ch), dir = 0;
            if (did_any_pad_press_input(0) != 0) dir = -1;
            else if (did_any_pad_press_input(1) != 0) dir = 1;
            if (dir != 0) {
                /* next multiple of the step in that direction */
                v = dir > 0 ? (v / PCS_MIX_STEP + 1) * PCS_MIX_STEP
                            : ((v + PCS_MIX_STEP - 1) / PCS_MIX_STEP - 1) * PCS_MIX_STEP;
                if (pcs_mix_set(ch, v)) {
                    s_snd.dirty = 1;
                    s_snd.idle = 0;
                    if (ch == PCS_MIX_VOICE || ch == PCS_MIX_TIMER) {
                        if (s_snd.previewWait == 0) {
                            sound_preview(ch);
                            s_snd.previewWait = 12;
                        }
                    } else {
                        sound |= 2;
                    }
                } else {
                    sound |= 0x10;   /* at the end of the slider */
                }
            }
        }
        if (did_any_pad_press_input(6) != 0 && idle) {
            if (s_snd.cursor == 0) {
                sound |= 8;
                s_snd.savedMode = OSGetSoundMode();
                s_snd.state = 1;
            } else if (s_snd.cursor - 1 == PCS_MIX_VOICE || s_snd.cursor - 1 == PCS_MIX_TIMER) {
                sound_preview(s_snd.cursor - 1);
            } else {
                sound |= 8;
            }
        } else if (did_any_pad_press_input(7) != 0 && idle) {
            g_b_button_pressed_on_menu(1);
        }
    } else {
        /* Sound Setting, as fn_3_23EA8 */
        int mode = OSGetSoundMode() == OS_SOUND_MODE_MONO ? 0 : 1, m = mode;
        if (did_any_pad_press_input(0) != 0 || did_any_pad_press_input(1) != 0)
            m ^= 1;
        if (m != mode) {
            sound |= 2;
            fn_1_367E4(m);
        }
        if (did_any_pad_press_input(6) != 0 && idle) {
            sound |= 8;
            s_snd.state = 0;
        } else if (did_any_pad_press_input(7) != 0 && idle) {
            sound |= 0x10;
            fn_1_367E4(s_snd.savedMode);
            s_snd.state = 0;
        }
    }

    /* the original page's speaker test: a random monkey voice every second,
     * panned around the listener (fn_3_23EA8) */
    if ((g_font_char_var_3 % 60) == 0) {
        static const s32 voices[4] = { 0x12D, 0x130, 0x12F, 0x12E };
        s8 pan = (s8)(63.0f * math_sin((s16)(g_font_char_var_3 << 9)));
        s8 span = (s8)(63.0f * math_sin((s16)((g_font_char_var_3 << 9) + 0x8000)));
        s32 k = (s32)(rand() & 0x7FFF) >> 0xD;
        s32 cue = voices[k];
        if (cue == s_snd.lastVoice)
            cue = voices[k & 4]; /* sic: always index 0 */
        g_smth_with_sound = 0;
        g_player_id_for_sound = 0;
        call_SoundReqID_arg_0(((span & 0x7F) << 0x19) | (cue | 0x10000 | 0xF800 | ((pan & 0x7F) << 0x12)));
        s_snd.lastVoice = cue;
    }

    if (sound & 2)
        call_SoundReqID_arg_0(0x6C);
    else if (sound & 8)
        call_SoundReqID_arg_0(0x65);
    else if (sound & 0x10)
        call_SoundReqID_arg_0(0x66);

    /* save once the sliders rest for a moment (and when the page closes) */
    if (s_snd.dirty && ++s_snd.idle >= 90) {
        pcs_mix_save();
        s_snd.dirty = 0;
    }
    s_snd.timer += 1;
}

/* ======================================================================
 * 2. F1 overlay (modal, any screen)
 * ====================================================================== */
static u16 pad_held(void)
{
    u16 b = 0;
    int i;

    for (i = 0; i < 4; i++) {
        PADStatus *s = &pad_status_groups[i].unk0[0];
        if (s->err != 0 && i != 0)
            continue;
        b |= s->button;
        if (s->stickX < -50) b |= PAD_BUTTON_LEFT;
        if (s->stickX > 50) b |= PAD_BUTTON_RIGHT;
        if (s->stickY > 50) b |= PAD_BUTTON_UP;
        if (s->stickY < -50) b |= PAD_BUTTON_DOWN;
    }
    return b;
}

static void overlay_draw(int cursor, int editing, int timer)
{
    struct ListView v;
    Mtx44 m;

    g_some_sprite_reset_gx_state_func();
    mtxa_from_identity();
    GXLoadPosMtxImm(mtxa->mtxA, 0);
    C_MTXOrtho(m, 0.0f, 480.0f, 0.0f, 640.0f, 0.0f, 20000.0f);
    g_something_with_GXSetProjectionv(m, 1);
    g_smth_with_sprite_draw_reqs_widescreen = 1;

    memset(&v, 0, sizeof(v));
    v.kind = LV_OVERLAY;
    v.first = 0;
    v.count = OVERLAY_ROWS;
    v.cursor = cursor;
    v.spacing = ROW_SPACING;
    v.textScaleY = 0.95f;
    v.editing = editing;
    v.timer = timer;
    v.x = 170.0f;
    v.y = 100.0f;
    v.valueX = 420.0f;
    v.boxBmp = 5;
    v.boxCX = 320.0f;
    v.boxW = 420.0f;
    v.descY = v.y + ROW_SPACING * OVERLAY_ROWS + 16.0f;
    v.hintY = v.descY + 36.0f;
    v.boxH = v.hintY + 34.0f - (v.y - 50.0f);   /* title (y-40) .. hint */
    v.boxCY = (v.y - 50.0f) + v.boxH * 0.5f;
    v.bullet = 0;
    {
        static const char *const title[6] = {
            "PC Settings", "PC-Einstellungen", "Réglages PC", "Ajustes de PC",
            "Impostazioni PC", "PCせってい" };
        static const char *const hint_confirm[6] = {
            "Left/Right: select   A: confirm   B: cancel",
            "Links/Rechts: wählen   A: OK   B: Abbrechen",
            "Gauche/Droite : choisir   A : valider   B : annuler",
            "Izq./Der.: elegir   A: aceptar   B: cancelar",
            "Sinistra/Destra: scegli   A: conferma   B: annulla",
            "ひだり/みぎ:えらぶ   A:けってい   B:キャンセル" };
        static const char *const hint_edit[6] = {
            "Left/Right: change   A: apply   B: cancel",
            "Links/Rechts: ändern   A: übernehmen   B: Abbrechen",
            "Gauche/Droite : changer   A : appliquer   B : annuler",
            "Izq./Der.: cambiar   A: aplicar   B: cancelar",
            "Sinistra/Destra: cambia   A: applica   B: annulla",
            "ひだり/みぎ:かえる   A:けってい   B:キャンセル" };
        static const char *const hint_list[6] = {
            "Up/Down: select   A: edit   B / F1: close",
            "Hoch/Runter: wählen   A: ändern   B / F1: schließen",
            "Haut/Bas : choisir   A : modifier   B / F1 : fermer",
            "Arriba/Abajo: elegir   A: editar   B / F1: cerrar",
            "Su/Giù: scegli   A: modifica   B / F1: chiudi",
            "うえ/した:えらぶ   A:へんこう   B / F1:とじる" };
        v.title = pc_text_pick(title);
        {
            static char ver[48];
            snprintf(ver, sizeof ver, "v%s", pc_update_current_version());
            v.version = ver;
        }
        v.hint = pc_text_pick(pc_quit_confirm_active() ? hint_confirm
                              : editing ? hint_edit : hint_list);
    }
    draw_list(&v);
}

void pc_settings_overlay_run(void)
{
    int cursor = 0, editing = 0, timer = 0, rep = 0;
    u16 prev, held, press;
    unsigned long long next_ns;

    int outer_confirm;

    if (!pc_frame_capture())
        return;
    /* a yes/no box the game screen had open (pause menu / Options page)
     * comes back when the overlay closes */
    outer_confirm = pc_quit_confirm_active();
    pc_quit_confirm_close();
    printf("[settings] overlay open\n");
    fflush(stdout);

    prev = pad_held(); /* ignore buttons already down */
    next_ns = platform_get_time_ns();
    for (;;) {
        int close = 0;

        pc_frame_restore(0.45f); /* dimmed game frame behind the box */
        overlay_draw(cursor, editing, timer);
        platform_swap_buffers();
#ifdef _WIN32
        g_shim_frame_pulse++;
#endif
        input_poll_gc_pads();
        platform_poll_events();
        if (pc_settings_overlay_take_request())
            close = 1; /* F1 again */
        if (platform_should_close())
            close = 1; /* window closed: video_finish_frame quits */

        held = pad_held();
        press = held & ~prev;
        /* auto-repeat for the d-pad / stick */
        if (held & (PAD_BUTTON_UP | PAD_BUTTON_DOWN | PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT)) {
            if (++rep > 20 && (rep % 6) == 0)
                press |= held & (PAD_BUTTON_UP | PAD_BUTTON_DOWN | PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT);
        } else {
            rep = 0;
        }
        prev = held;

        if (pc_quit_confirm_active()) {
            switch (pc_quit_confirm_tick(press & (PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT |
                                                  PAD_BUTTON_A | PAD_BUTTON_B | 0x1000))) {
            case PCQ_MOVED: call_SoundReqID_arg_0(0x6C); break;
            case PCQ_NO: call_SoundReqID_arg_0(0x66); break;
            case PCQ_YES:
                /* back to the game, which quits from video_finish_frame */
                call_SoundReqID_arg_0(0x65);
                close = 1;
                break;
            }
            if (close)
                pc_quit_confirm_close();
        } else if (!editing) {
            if (press & PAD_BUTTON_UP) {
                cursor = (cursor + OVERLAY_ROWS - 1) % OVERLAY_ROWS;
                call_SoundReqID_arg_0(0x6C);
            } else if (press & PAD_BUTTON_DOWN) {
                cursor = (cursor + 1) % OVERLAY_ROWS;
                call_SoundReqID_arg_0(0x6C);
            }
            if (press & PAD_BUTTON_A) {
                if (cursor == PCS_ITEM_COUNT) {
                    pc_quit_confirm_open();
                    call_SoundReqID_arg_0(0x65);
                } else if (cursor == PCS_ITEM_CONTROLLERS) {
                    call_SoundReqID_arg_0(0x65);
                    pc_stick_screen_run(1);
                    prev = pad_held();   /* buttons still down from that screen */
                } else if (cursor == PCS_ITEM_UPDATES) {
                    call_SoundReqID_arg_0(0x65);
                    pc_update_screen_run(1);
                    prev = pad_held();
                    if (pc_quit_requested())
                        close = 1;   /* Setup started: back to the game, which quits */
                } else if (cursor == PCS_ITEM_MEMCARD) {
                    call_SoundReqID_arg_0(0x65);
                    pc_memcard_screen_run(1);
                    prev = pad_held();
                    if (pc_quit_requested())
                        close = 1;   /* save imported: back to the game, which quits */
                } else if (pcs_item_enabled(cursor)) {
                    pcs_item_begin_edit(cursor);
                    editing = 1;
                    call_SoundReqID_arg_0(0x65);
                } else {
                    call_SoundReqID_arg_0(0x66);
                }
            } else if (press & (PAD_BUTTON_B | 0x1000 /* START */)) {
                close = 1;
            }
        } else {
            if (press & PAD_BUTTON_LEFT) {
                if (pcs_item_step(cursor, -1)) call_SoundReqID_arg_0(0x6C);
            } else if (press & PAD_BUTTON_RIGHT) {
                if (pcs_item_step(cursor, 1)) call_SoundReqID_arg_0(0x6C);
            }
            if (press & PAD_BUTTON_A) {
                pcs_item_confirm(cursor);
                editing = 0;
                call_SoundReqID_arg_0(0x65);
            } else if (press & PAD_BUTTON_B) {
                pcs_item_cancel(cursor);
                editing = 0;
                call_SoundReqID_arg_0(0x66);
            }
            if (close) {
                pcs_item_cancel(cursor);
                editing = 0;
            }
        }
        if (close)
            break;
        timer++;

        next_ns += 16666667ULL;
        {
            unsigned long long now = platform_get_time_ns();
            if (now < next_ns)
                platform_wait_until_ns(next_ns);
            else if (now > next_ns + 100000000ULL)
                next_ns = now;
        }
    }

    if (outer_confirm && !pc_quit_requested())
        pc_quit_confirm_open();

    /* put the frozen game frame back; the next gx_start_new_frame swap
     * presents it and the game carries on */
    pc_frame_restore(1.0f);
    printf("[settings] overlay closed\n");
    fflush(stdout);
}
