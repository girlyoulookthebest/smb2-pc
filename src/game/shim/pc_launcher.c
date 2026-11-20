/* Debug launcher: a stand-in for mkb2.sel_ngc.rel (the NGC mode-select
 * menu), used when SMB2_PC_LAUNCHER=1 (dvd_shim.c).  A plain debug-text menu
 * that starts the same game flows sel_ngc would: Challenge Mode
 * (Beginner/Advanced/Expert), Practice (the decompiled sel_stage picker) and
 * the minigames.  Games return here through g_return_to_sel_ngc() ->
 * SMD_SEL_NGC_REINIT, which reloads this "REL". */
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "course.h"
#include "input.h"
#include "mode.h"
#include "pool.h"
#include "dip.h"
#include "background.h"

#define PAD_REPEAT(mask) \
    ((pad_status_groups[0].unk0[4].button & (mask)) || (analog_inputs[0][4] & (mask)) \
     || (((pad_status_groups[0].unk0[0].button & (mask)) || (analog_inputs[0][0] & (mask))) \
         && (analog_inputs[0][0] & PAD_BUTTON_B)))
#define PAD_PRESSED(mask) (pad_status_groups[0].unk0[2].button & (mask))

extern s16 g_some_stage_id;
extern s16 g_another_stage_id;
extern s32 active_monkey_id;
extern u32 g_some_other_flags;
extern u32 g_font_char_var_3;
/* include/baseball.h (it can't be included here: conflicting prototypes) */
struct PauseMenuState
{
    s32 timer;
    u32 flags;
    s32 selection;
    s32 itemCount;
    s32 menuType;
    s8 padId;
    s8 playerId;
    s16 unk16;
    s8 screenshotState;
};
extern struct PauseMenuState g_related_to_pause_menu_input;
extern u8 lbl_1_bss_16D0;    /* sel.c struct SelSettings8: race settings, byte 0 = course */
extern u8 lbl_1_bss_172C[];  /* sel.c struct SelSettingsH: baseball settings */

void g_set_some_sel_ngc_global_func_ptrs(void (*a)(void), void (*b)(void),
                                         void (*c)(void), void (*d)(void));
void g_set_up_debugtext_buffer(void);
void dest_all_events(void);
void g_something_with_iteratively_freeing_memory(void);
void debug_textdraw_set_pos(int x, int y);
void debug_textdraw_set_color(int c);
void debug_textdraw_put_buf_1(const char *s);
void fade_screen_to_color(u32 flags, u32 color, int len);
void g_purge_rel(void *rel);
void load_additional_rel(char *name, void *rel);
void g_something_with_sound7_and_game_heaps(int config);
extern u8 g_some_sel_ngc_rel_buffer[];  /* sel.c: struct RelInfo of the sel REL slot */
void disable_all_cameras(void);
int pc_party_game_available(int gameType);   /* pc_party_avail.c */

enum { L_CHALLENGE, L_PRACTICE, L_MINI };

struct LauncherEntry {
    int kind;
    const char *name;
    int arg0;   /* difficulty, or minigame sub mode */
    int arg1;   /* minigame gameType */
};

static const struct LauncherEntry s_entries[] = {
    { L_CHALLENGE, "CHALLENGE  BEGINNER", DIFFICULTY_BEGINNER, 0 },
    { L_CHALLENGE, "CHALLENGE  ADVANCED", DIFFICULTY_ADVANCED, 0 },
    { L_CHALLENGE, "CHALLENGE  EXPERT",   DIFFICULTY_EXPERT,   0 },
    { L_PRACTICE,  "PRACTICE (STAGE SELECT)", 0, 0 },
    /* sub mode / gameType pairs from mini_mode.c's MINI MODE table */
    { L_MINI, "MONKEY RACE",     0xB9, 3 },
    { L_MINI, "MONKEY TARGET",   0xBF, 5 },
    { L_MINI, "MONKEY BOWLING",  0xAB, 7 },
    { L_MINI, "MONKEY BOAT",     0xC1, 10 },
    { L_MINI, "MONKEY DOGFIGHT", 0xC7, 13 },
    { L_MINI, "MONKEY BASEBALL", 0xC9, 15 },
    { L_MINI, "MONKEY GOLF",     0xBB, 8 },
    { L_MINI, "MONKEY FIGHT",    0xBD, 4 },
};
#define N_ENTRIES ((int)(sizeof(s_entries) / sizeof(s_entries[0])))

/* the party games this menu can't offer while pc_party_game_available()
 * says their REL isn't linked (not decompiled yet): listed below the menu */
static const struct { const char *name; int gameType; } s_other_games[] = {
    { "FIGHT", 4 }, { "BILLIARDS", 6 }, { "SHOT", 9 }, { "SOCCER", 12 }, { "TENNIS", 16 },
};

static int s_cursor;
static int s_started;

/* ---- SMB2_PC_STAGE and stages no Challenge course plays ------------------
 * The hook can force any stage id into a Challenge, but the game only plays
 * its Challenge/Story/Practice course stages under Challenge's heaps (heap
 * config 0).  The other ids exist on the GC under other heap configs: the
 * story cutscene sets 401-420 (author mode, config 8), the attract-demo sets
 * 397-400 (config 0x12), the party-game courses (e.g. Monkey Boat's 140,
 * config 6) and unused test stages.  Several don't fit config 0 and panic on
 * the GC too when forced into a Challenge (Dolphin: 407 "tex:<bg_au_wat2.tpl>
 * not enough memory", 415 "<bg_au_bub2.gma>", 140 "<st140.gma>"); 186's
 * replay doesn't fit its stage heap.  alloc.c plays such a stage under config
 * 8, everything in one main heap, as author mode does for the cutscene sets. */
static int s_hook_stage;   /* stage the hook chose at the last start, 0 = none */

static int stage_in_courses(int stageId)
{
    const struct CourseCommand *cmd;
    int i, j;

    for (i = 0; i < 12; i++)   /* cm_courses[]: every Challenge course */
        for (cmd = cm_courses[i]; cmd->opcode != CMD_COURSE_END; cmd++)
            if (cmd->opcode == CMD_FLOOR && cmd->type == FLOOR_STAGE_ID
             && cmd->value == stageId)
                return 1;
    for (i = 0; i < 10; i++)   /* Story Mode worlds (Practice plays these) */
        for (j = 0; j < sm_world_info[i].stageCount; j++)
            if (sm_world_info[i].stages[j].stageId == stageId)
                return 1;
    return 0;
}

int pc_stage_hook_needs_shared_heap(void)
{
    return s_hook_stage > 0 && !stage_in_courses(s_hook_stage);
}

static void launcher_set_single_player(void)
{
    modeCtrl.playerCount = 1;
    modeCtrl.unk30 = 1;
    modeCtrl.currPlayer = 0;
    modeCtrl.submodeTimer = 0;
    g_poolInfo.playerPool.statusList[0] = 2;
    g_poolInfo.playerPool.statusList[1] = 0;
    g_poolInfo.playerPool.statusList[2] = 0;
    g_poolInfo.playerPool.statusList[3] = 0;
    /* the attract demo leaves several cameras enabled.  Like the real
     * sel_ngc, keep only player 0's (game init enables those of the active
     * players), or game_main_draw_func draws the scene once per stale
     * camera. */
    disable_all_cameras();
    active_monkey_id = 0;
}

void pc_test_dip_apply(void);

static void launcher_start(const struct LauncherEntry *e)
{
    launcher_set_single_player();
    s_hook_stage = 0;
    switch (e->kind) {
    case L_CHALLENGE:
        modeCtrl.gameType = 0;
        modeCtrl.difficulty = e->arg0;
        modeCtrl.courseFlags |= 1;
        modeCtrl.courseFlags &= ~(COURSE_FLAG_EXTRA | COURSE_FLAG_MASTER |
                                  COURSE_FLAG_MASTER_EXTRA);
        in_practice_mode = 0;
        g_some_stage_id = (s16)g_update_cm_course(modeCtrl.difficulty, 1,
                                                  modeCtrl.courseFlags);
        /* test hook: SMB2_PC_STAGE=<id> plays that stage id as Challenge
         * floor 1 (e.g. 9 = HOPPERS, Story World 2 stage 3); a list
         * "<id>,<id>,..." plays the next one at each Challenge start, and
         * "floors" the next floor of the chosen difficulty (long-session
         * tests: exit to the launcher and start again) */
        if (getenv("SMB2_PC_STAGE") && strcmp(getenv("SMB2_PC_STAGE"), "floors") == 0) {
            static int s_floor;
            int id = g_update_cm_course(modeCtrl.difficulty, ++s_floor, modeCtrl.courseFlags);
            if (id <= 0) {
                s_floor = 1;
                id = g_update_cm_course(modeCtrl.difficulty, 1, modeCtrl.courseFlags);
            }
            g_some_stage_id = (s16)id;
        } else if (getenv("SMB2_PC_STAGE")) {
            static int s_stage_n;
            const char *q = getenv("SMB2_PC_STAGE"), *c;
            int n = 1, k;
            for (c = q; *c; c++)
                n += *c == ',';
            for (k = s_stage_n++ % n; k > 0; k--) {
                while (*q && *q != ',') q++;
                if (*q) q++;
            }
            if (atoi(q) > 0)
                g_some_stage_id = (s16)(s_hook_stage = atoi(q));
        }
        g_another_stage_id = mode_info.currStageId = g_some_stage_id;
        /* test hook: SMB2_PC_WIDE=1 plays it in the game's 16:9 TV mode */
        if (getenv("SMB2_PC_WIDE") && atoi(getenv("SMB2_PC_WIDE")) > 0) {
            extern void set_render_mode(int);
            extern void camera_update_aspect_ratios(void);
            modeCtrl.wideMode = 1;
            set_render_mode(1);
            camera_update_aspect_ratios();
        }
        main_mode_request = 2;     /* MD_GAME */
        sub_mode_request = 0x3C;   /* SMD_GAME_FIRST_INIT */
        break;
    case L_PRACTICE:
        /* enter the decompiled sel_stage picker the way the original does:
         * SMD_SEL_STAGE_INIT runs smd_sel_ngc_init (sel.c), which switches to
         * heap config 0x11 (bg heap 0x480020, room for every background the
         * picker previews; this menu's config 0xB is too small for bg_lav2)
         * and loads mkb2.sel_stage.rel into the sel_ngc REL slot.
         * smd_sel_ngc_init can't be requested from main mode 1 (its
         * set_sub_mode_destination would panic), so do its body: unload this
         * "REL" first, as g_sel_ngc_sub_mode_destination does, so the slot
         * holds sel_stage and is purged with its epilog when a stage starts. */
        g_purge_rel(g_some_sel_ngc_rel_buffer);
        dest_all_events();
        g_something_with_iteratively_freeing_memory();
        g_something_with_sound7_and_game_heaps(0x11);
        load_additional_rel("mkb2.sel_stage.rel", g_some_sel_ngc_rel_buffer);
        break;
    case L_MINI:
        modeCtrl.gameType = e->arg1;
        main_mode_request = 4;     /* MD_MINI */
        sub_mode_request = e->arg0;
        /* Baseball reads its settings from sel.c's lbl_1_bss_172C, which
         * sel_ngc's baseball menu fills in.  g_set_lots_of_initial_state
         * leaves mode 0 (2P VS), where the pitcher is driven by the absent
         * second pad and never throws.  This launcher is single player, so
         * pick mode 1 (1P vs COM). */
        if (e->arg1 == 15)
            lbl_1_bss_172C[0] = 1;
        /* test hook: SMB2_PC_RACE=<0-5> picks the Monkey Race course
         * (race2_5.c course table order: 4 = Submarine Street) */
        if (e->arg1 == 3 && getenv("SMB2_PC_RACE"))
            lbl_1_bss_16D0 = (u8)atoi(getenv("SMB2_PC_RACE"));
        /* the flags sel_ngc's menu_start_golf sets */
        if (e->arg1 == 8) {
            modeCtrl.courseFlags |= 4;
            g_related_to_pause_menu_input.flags &= ~2u;
        }
        break;
    }
    s_started = 1;
    dip_switches &= ~DIP_DISP;
    pc_test_dip_apply();
}

/* test hook: SMB2_PC_DIP=<hex> ORs debug DIP switches in (e.g. 20000000 =
 * DIP_TEST_TEAM: Monkey Golf's L+A stroke count and L+RIGHT next hole, for
 * scripted full rounds).  Applied at launcher starts and at every REL load
 * (dvd_shim.c), so it also works through the real menus. */
void pc_test_dip_apply(void)
{
    if (getenv("SMB2_PC_DIP"))
        dip_switches |= (u32)strtoul(getenv("SMB2_PC_DIP"), NULL, 16);
}

static void launcher_tick(void)
{
    int i;

    if (!s_started && !(g_some_other_flags & 0xA)) {
        if (PAD_REPEAT(PAD_BUTTON_DOWN) && ++s_cursor >= N_ENTRIES)
            s_cursor = 0;
        if (PAD_REPEAT(PAD_BUTTON_UP) && --s_cursor < 0)
            s_cursor = N_ENTRIES - 1;
        if (PAD_PRESSED(PAD_BUTTON_A) || PAD_PRESSED(0x1000 /* START */)) {
            launcher_start(&s_entries[s_cursor]);
            if (s_entries[s_cursor].kind == L_PRACTICE)
                return;
        }
    }

    debug_textdraw_set_pos(4, 3);
    debug_textdraw_set_color(0xFF);
    debug_textdraw_put_buf_1("SUPER MONKEY BALL 2  -  PC PORT");
    debug_textdraw_set_pos(4, 5);
    debug_textdraw_set_color(0x57);
    debug_textdraw_put_buf_1("(stand-in menu: sel_ngc is not decompiled yet)");
    for (i = 0; i < N_ENTRIES; i++) {
        int y = 8 + i * 2 + (i >= 3) + (i >= 4);

        debug_textdraw_set_pos(6, y);
        if (i == s_cursor) {
            debug_textdraw_set_color(((g_font_char_var_3 >> 3) & 1) ? 0xFF : 0xFC);
            debug_textdraw_put_buf_1("\034 ");
        } else {
            debug_textdraw_set_color(0x57);
            debug_textdraw_put_buf_1("  ");
        }
        debug_textdraw_put_buf_1(s_entries[i].name);
    }
    debug_textdraw_set_pos(6, 8 + N_ENTRIES * 2 + 3);
    debug_textdraw_set_color(0x57);
    debug_textdraw_put_buf_1("UP/DOWN: choose    A: start");
    {
        unsigned k;
        int any = 0;

        debug_textdraw_set_pos(2, 8 + N_ENTRIES * 2 + 5);   /* the last row that shows */
        for (k = 0; k < sizeof(s_other_games) / sizeof(s_other_games[0]); k++) {
            if (pc_party_game_available(s_other_games[k].gameType))
                continue;
            debug_textdraw_put_buf_1(any++ ? " " : "NOT DECOMPILED: ");
            debug_textdraw_put_buf_1(s_other_games[k].name);
        }
    }
}

static void launcher_draw(void)
{
}

void pcLauncherPrologue(void)
{
    g_set_some_sel_ngc_global_func_ptrs(launcher_tick, launcher_draw, NULL, NULL);
    g_set_up_debugtext_buffer();
    s_started = 0;
    /* window.c only renders the debug text layer with DIP_DISP set */
    dip_switches |= DIP_DISP;
    /* main mode 1 clears to the world backdrop colour, which still holds
     * the last stage's (e.g. white after a jungle stage); the real sel_ngc
     * covers it with its own backdrop, the debug-text menu does not */
    world_theme.backdropColor.r = 0;
    world_theme.backdropColor.g = 0;
    world_theme.backdropColor.b = 0;
    if (s_cursor < 0 || s_cursor >= N_ENTRIES)
        s_cursor = 0;
    fade_screen_to_color(0, 0, 30);
    sub_mode_request = 0x20;   /* SMD_SEL_NGC_MAIN runs menu_tick_func */
}

void pcLauncherEpilogue(void)
{
}

/* test hook: SMB2_PC_GOLF_WIND=<angle>,<velocity> replaces Monkey Golf's random
 * wind of each hole (s16 angle units, velocity as golf_wind_velocity), for
 * comparing single shots with Dolphin (rand() state differs between runs) */
extern s32 golf_wind_angle;
extern float golf_wind_velocity;
void pc_golf_test_wind(void)
{
    const char *e = getenv("SMB2_PC_GOLF_WIND");
    if (e) {
        golf_wind_angle = atoi(e);
        e = strchr(e, ',');
        golf_wind_velocity = e ? (float)atof(e + 1) : 0.0f;
    }
}
