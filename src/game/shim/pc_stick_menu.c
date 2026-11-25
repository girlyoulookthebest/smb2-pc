/* PC "Controller Sticks" screen (docs/stick.md): per-controller stick gate,
 * dead zone, sensitivity and a Dolphin-style calibration ("rotate the stick
 * slowly around its edge"), with a live view of both sticks:
 *
 *   grey ring   the host stick's full range (1.0), no calibration
 *   white dots  the calibrated edge (or the capture in progress)
 *   red ring    the dead zone
 *   gold        the emulated GameCube gate (octagon / circle)
 *   small dot   the host stick position (raw, as SDL reports it)
 *   big dot     the emulated GC stick (what PADRead would return)
 *   below       raw -> GC pad value -> value the game plays with (60 = full)
 *
 * Opened from the F1 overlay and the Options > Game Screen page
 * ("Controllers" row, PCS_ITEM_CONTROLLERS).  Modal like the F1 overlay:
 * draws over the dimmed, frozen game frame and runs its own frame loop.
 * Drawn with the game's UI pieces (wood box, menu font, menu sounds); the
 * text goes through pc_text_pick (6 game locales). */
#include "gc_intrinsics.h"

struct StickPoint;
struct MemcardRequest;
struct TextBox;
#include "sel_ngc.h"

#include "pc_settings.h"
#include "pc_text.h"
#include "pad_config.h"

void fn_3_140CC(int counter);                    /* sel_ngc_sprite.c */
void C_MTXOrtho(Mtx44 m, float t, float b, float l, float r, float n, float f);
extern s32 g_smth_with_sprite_draw_reqs_widescreen; /* bitmap.c */

extern void input_poll_gc_pads(void);
extern void platform_poll_events(void);
extern int  platform_should_close(void);
extern void platform_swap_buffers(void);
extern unsigned long long platform_get_time_ns(void);
extern void platform_wait_until_ns(unsigned long long target_ns);
extern int  pc_frame_capture(void);
extern void pc_frame_restore(float brightness);
#ifdef _WIN32
extern volatile long g_shim_frame_pulse;
#endif

typedef const char *const Tr[6];

static Tr s_title = { "Controller Sticks", "Controller-Sticks", "Sticks de la manette",
                      "Palancas del mando", "Levette del controller", "スティックせってい" };
static Tr s_main_label = { "Control Stick", "Control Stick", "Stick de contrôle",
                           "Palanca de control", "Control Stick", "コントロールスティック" };
static Tr s_c_label = { "C Stick", "C-Stick", "Stick C", "Palanca C", "Levetta C", "Cスティック" };

enum { ROW_PAD, ROW_GATE, ROW_DEADZONE, ROW_SENS, ROW_CALIBRATE, ROW_RESET, ROW_BACK, ROWS };

static Tr s_row_names[ROWS] = {
    { "Controller", "Controller", "Manette", "Mando", "Controller", "コントローラー" },
    { "Stick Gate", "Stick-Form", "Forme du stick", "Forma de la palanca", "Forma della levetta", "スティックのかたち" },
    { "Dead Zone", "Totzone", "Zone morte", "Zona muerta", "Zona morta", "あそび" },
    { "Sensitivity", "Empfindlichkeit", "Sensibilité", "Sensibilidad", "Sensibilità", "かんど" },
    { "Calibrate", "Kalibrieren", "Calibrer", "Calibrar", "Calibra", "キャリブレーション" },
    { "Reset to Defaults", "Standardwerte", "Valeurs par défaut", "Restablecer", "Ripristina predefiniti", "しょきせっていに もどす" },
    { "Back", "Zurück", "Retour", "Volver", "Indietro", "もどる" },
};
static Tr s_row_descs[ROWS] = {
    { "The controller to set up (Left/Right). Settings are kept per controller.",
      "Der einzustellende Controller (Links/Rechts). Jeder Controller behält seine Einstellungen.",
      "La manette à régler (Gauche/Droite). Chaque manette garde ses réglages.",
      "El mando que se ajusta (Izq./Der.). Cada mando guarda sus ajustes.",
      "Il controller da regolare (Sinistra/Destra). Ogni controller ha le sue impostazioni.",
      "せっていする コントローラー（ひだり/みぎ）。コントローラーごとに きろくします。" },
    { "GameCube octagon: full tilt reaches the GameCube stick's notches, like Dolphin.",
      "GameCube-Achteck: Vollausschlag wie bis an die Kerben des GameCube-Sticks (wie Dolphin).",
      "Octogone GameCube : à fond, le stick atteint les crans du stick GameCube (comme Dolphin).",
      "Octógono GameCube: a fondo llega a las muescas de la palanca de GameCube (como Dolphin).",
      "Ottagono GameCube: al massimo raggiunge le tacche della levetta GameCube (come Dolphin).",
      "ゲームキューブの はっかくけいの ガイドと おなじ うごきに なります。" },
    { "Ignore small movements around the centre (stick drift).",
      "Kleine Bewegungen um die Mitte ignorieren (Stick-Drift).",
      "Ignorer les petits mouvements autour du centre (dérive du stick).",
      "Ignorar pequeños movimientos en el centro (deriva de la palanca).",
      "Ignora i piccoli movimenti al centro (deriva della levetta).",
      "まんなか ふきんの ちいさな うごきを むしします。" },
    { "100% matches a real GameCube controller. Higher reaches full tilt sooner.",
      "100 % entspricht einem echten GameCube-Controller. Mehr = früher volle Neigung.",
      "100 % correspond à une vraie manette GameCube. Plus = inclinaison max plus tôt.",
      "100 % equivale a un mando de GameCube real. Más = inclinación máxima antes.",
      "100% corrisponde a un vero controller GameCube. Di più = inclinazione massima prima.",
      "100%が ほんものの ゲームキューブと おなじです。" },
    { "Measure the stick edges: press A, rotate both sticks slowly along their edges, press A.",
      "Stick-Ränder messen: A drücken, beide Sticks langsam am Rand drehen, A drücken.",
      "Mesurer les bords : A, tournez lentement les sticks contre leur bord, puis A.",
      "Medir los bordes: pulsa A, gira despacio las palancas por el borde y pulsa A.",
      "Misura i bordi: premi A, ruota lentamente le levette lungo il bordo, premi A.",
      "Aを おして スティックを ふちに そって ゆっくり まわし、もういちど Aを おします。" },
    { "Octagon gate, 5% dead zone, 100% sensitivity, no calibration.",
      "Achteck, 5 % Totzone, 100 % Empfindlichkeit, keine Kalibrierung.",
      "Octogone, zone morte 5 %, sensibilité 100 %, sans calibrage.",
      "Octógono, zona muerta 5 %, sensibilidad 100 %, sin calibrar.",
      "Ottagono, zona morta 5%, sensibilità 100%, nessuna calibrazione.",
      "はっかくけい、あそび5%、かんど100%、キャリブレーションなし。" },
    { "Back to the previous screen.", "Zurück zum vorigen Bildschirm.", "Revenir à l'écran précédent.",
      "Volver a la pantalla anterior.", "Torna alla schermata precedente.", "まえの がめんに もどります。" },
};
static Tr s_gate_octagon = { "GameCube octagon", "GameCube-Achteck", "Octogone GameCube",
                             "Octógono GameCube", "Ottagono GameCube", "はっかくけい" };
static Tr s_gate_circle = { "Raw Circle", "Kreis (roh)", "Cercle brut", "Círculo", "Cerchio", "まる" };
static Tr s_no_pad = { "No controller connected", "Kein Controller angeschlossen", "Aucune manette branchée",
                       "No hay ningún mando", "Nessun controller collegato", "コントローラーが ありません" };
static Tr s_kb_note = { "Keyboard: WASD gives full GameCube stick tilt.",
                        "Tastatur: WASD = voller GameCube-Stick-Ausschlag.",
                        "Clavier : ZQSD/WASD = stick GameCube à fond.",
                        "Teclado: WASD = palanca GameCube a fondo.",
                        "Tastiera: WASD = levetta GameCube al massimo.",
                        "キーボード：WASDで ゲームキューブの スティックを いっぱいに たおします。" };
static Tr s_cal_title = { "Rotate both sticks slowly along their edges", "Beide Sticks langsam am Rand entlang drehen",
                          "Tournez lentement les sticks contre leur bord", "Gira despacio las palancas por el borde",
                          "Ruota lentamente le levette lungo il bordo", "スティックを ふちに そって ゆっくり まわしてください" };
static Tr s_cal_hint = { "A: done   B: cancel", "A: fertig   B: Abbrechen", "A : terminé   B : annuler",
                         "A: listo   B: cancelar", "A: fatto   B: annulla", "A:おわり   B:キャンセル" };
static Tr s_hint = { "Up/Down: select   Left/Right: change   A: choose   B: back",
                     "Hoch/Runter: wählen   Links/Rechts: ändern   A: OK   B: zurück",
                     "Haut/Bas : choisir   Gauche/Droite : changer   A : valider   B : retour",
                     "Arriba/Abajo: elegir   Izq./Der.: cambiar   A: aceptar   B: volver",
                     "Su/Giù: scegli   Sinistra/Destra: cambia   A: conferma   B: indietro",
                     "うえ/した:えらぶ   ひだり/みぎ:かえる   A:けってい   B:もどる" };
static Tr s_calibrated = { "calibrated", "kalibriert", "calibré", "calibrado", "calibrato", "キャリブレーションずみ" };
static Tr s_not_calibrated = { "not calibrated", "nicht kalibriert", "non calibré", "sin calibrar", "non calibrato",
                               "キャリブレーションなし" };

/* ---------------------------------------------------------------- state */
static struct {
    int port;          /* 0-3 */
    int cursor;
    int capturing;     /* calibration capture running */
    GcsCal cap[2];
    float prev[2][2];
    int dirty[4];
    int timer;
} s_st;

static volatile int s_request;
void pc_stick_screen_request(void) { s_request = 1; }
int pc_stick_screen_take_request(void) { int r = s_request; s_request = 0; return r; }

static int first_port(void)
{
    int i;
    for (i = 0; i < 4; i++)
        if (input_port_connected(i)) return i;
    return 0;
}

/* ---------------------------------------------------------------- drawing */
#define PANEL_Y 150.0f
#define PANEL_SCALE 0.6f         /* px per GC unit; host 1.0 = 127 units */
static const float s_panel_x[2] = { 190.0f, 450.0f };

static void text(float x, float y, float sx, float sy, u32 color, int align, const char *s)
{
    textdraw_reset();
    textdraw_set_font(3);
    textdraw_set_flags(0xA0000000U);
    textdraw_set_alignment(align);
    textdraw_set_mul_color(color);
    textdraw_set_scale(sx, sy);
    textdraw_set_pos(x, y);
    textdraw_print((char *)s);
}

/* a dot centred on (x, y): the font's middle dot glyph */
static void dot(float x, float y, float size, u32 color)
{
    text(x, y - 12.0f * size, size, size, color, 3, pc_text_game("・"));
}

static void panel_point(int stick, float ux, float uy, float *x, float *y)
{
    *x = s_panel_x[stick] + ux * PANEL_SCALE;
    *y = PANEL_Y - uy * PANEL_SCALE;
}

static void draw_panel(int stick, const GcsPadConfig *cfg, const PadStickView *v)
{
    int i;
    float x, y;
    char buf[96];
    const GcsCal *cal = s_st.capturing ? &s_st.cap[stick] : (cfg ? &cfg->cal[stick] : NULL);

    /* host full range (1.0) */
    for (i = 0; i < 64; i++) {
        double a = i * 6.283185307179586 / 64;
        panel_point(stick, (float)(127 * cos(a)), (float)(127 * sin(a)), &x, &y);
        dot(x, y, 0.35f, 0x606060U);
    }
    /* calibrated edge / capture in progress */
    if (cal && (cal->valid || s_st.capturing)) {
        for (i = 0; i < GCS_CAL_SAMPLES; i++) {
            double a = i * 6.283185307179586 / GCS_CAL_SAMPLES;
            float r = cal->r[i];
            if (r <= 0.0f) continue;
            panel_point(stick, (float)(127 * r * cos(a)), (float)(127 * r * sin(a)), &x, &y);
            dot(x, y, 0.5f, r >= 0.5f ? 0xFFFFFFU : 0x8080FFU);
        }
    }
    /* dead zone */
    if (cfg && cfg->deadzone > 0) {
        for (i = 0; i < 24; i++) {
            double a = i * 6.283185307179586 / 24;
            double r = 127 * cfg->deadzone / 100.0 * gcs_cal_radius(&cfg->cal[stick], a);
            panel_point(stick, (float)(r * cos(a)), (float)(r * sin(a)), &x, &y);
            dot(x, y, 0.3f, 0xFF4040U);
        }
    }
    /* emulated GameCube gate */
    for (i = 0; i < 96; i++) {
        double a = i * 6.283185307179586 / 96;
        double r = 127 * gcs_gate_radius(stick, cfg ? cfg->gate : GCS_GATE_OCTAGON, a);
        panel_point(stick, (float)(r * cos(a)), (float)(r * sin(a)), &x, &y);
        dot(x, y, 0.4f, 0xFFC020U);
    }
    text(s_panel_x[stick], PANEL_Y - 127 * PANEL_SCALE - 22.0f, 0.7f, 0.8f, 0xFFFF40U, 3,
         pc_text_pick(stick == GCS_MAIN ? s_main_label : s_c_label));

    if (v) {
        /* raw host position (small), emulated GC stick (big) */
        panel_point(stick, 127 * v->host[stick][0], 127 * v->host[stick][1], &x, &y);
        dot(x, y, 0.7f, 0xFFFFFFU);
        panel_point(stick, v->gc[stick][0], v->gc[stick][1], &x, &y);
        dot(x, y, 1.2f, 0x40FF40U);
        /* host value -> emulated GC pad -> what the game plays with */
        snprintf(buf, sizeof buf, "%+.2f %+.2f", v->host[stick][0], v->host[stick][1]);
        text(s_panel_x[stick], PANEL_Y + 127 * PANEL_SCALE + 2.0f, 0.5f, 0.55f, 0xE0E0E0U, 3, buf);
        snprintf(buf, sizeof buf, "GC %+d %+d    %+d %+d", v->gc[stick][0], v->gc[stick][1],
                 v->game[stick][0], v->game[stick][1]);
        text(s_panel_x[stick], PANEL_Y + 127 * PANEL_SCALE + 18.0f, 0.5f, 0.55f, 0x80FF80U, 3, buf);
        if (s_st.capturing) {
            snprintf(buf, sizeof buf, "%d%%", gcs_cal_coverage(&s_st.cap[stick]) * 100 / GCS_CAL_SAMPLES);
            text(s_panel_x[stick], PANEL_Y + 127 * PANEL_SCALE + 36.0f, 0.7f, 0.8f,
                 gcs_cal_done(&s_st.cap[stick]) ? 0x40FF40U : 0xFFFF40U, 3, buf);
        } else if (cfg) {
            text(s_panel_x[stick], PANEL_Y + 127 * PANEL_SCALE + 36.0f, 0.5f, 0.55f, 0xA0A0A0U, 3,
                 pc_text_pick(cfg->cal[stick].valid ? s_calibrated : s_not_calibrated));
        }
    }
}

static const char *row_value(int r, const GcsPadConfig *cfg)
{
    static char buf[128];
    buf[0] = 0;
    switch (r) {
    case ROW_PAD:
        if (!input_port_connected(s_st.port))
            return pc_text_pick(s_no_pad);
        {
            char name[64];
            pc_text_game_buf(input_port_name(s_st.port), name, sizeof name);
            snprintf(buf, sizeof buf, "%d: %.24s", s_st.port + 1, name);
        }
        return buf;
    case ROW_GATE:
        return cfg ? pc_text_pick(cfg->gate == GCS_GATE_CIRCLE ? s_gate_circle : s_gate_octagon) : "-";
    case ROW_DEADZONE:
        if (!cfg) return "-";
        snprintf(buf, sizeof buf, "%d%%", cfg->deadzone);
        return buf;
    case ROW_SENS:
        if (!cfg) return "-";
        snprintf(buf, sizeof buf, "%d%%", cfg->sensitivity);
        return buf;
    }
    return "";
}

static u32 glow(int timer)
{
    u32 g = (u32)(64.0 * fabs(math_sin((s16)(timer * 0x180))));
    return g | (g << 8) | (g << 16);
}

static void screen_draw(void)
{
    Mtx44 m;
    GcsPadConfig *cfg = input_port_config(s_st.port);
    PadStickView view;
    int have = input_port_view(s_st.port, &view);
    float bw, bh, y0 = 276.0f, sp = 17.5f;
    int i;

    g_some_sprite_reset_gx_state_func();
    mtxa_from_identity();
    GXLoadPosMtxImm(mtxa->mtxA, 0);
    C_MTXOrtho(m, 0.0f, 480.0f, 0.0f, 640.0f, 0.0f, 20000.0f);
    g_something_with_GXSetProjectionv(m, 1);
    g_smth_with_sprite_draw_reqs_widescreen = 1;
    GXSetZMode_cached(1U, 7, 1U);
    set_ui_widescreen_scale_mtx(0x140);

    init_ui_element_sprite_with_defaults();
    set_ui_element_sprite_depth(0.11f);
    set_ui_element_sprite_pos(320.0f, 240.0f);
    bw = (float)g_get_bmp_width(5);
    bh = (float)g_get_bmp_height(5);
    if (bw > 0.0f && bh > 0.0f)
        set_ui_element_sprite_scale(600.0f / bw, 460.0f / bh);
    draw_ui_box(5);

    text(320.0f, 22.0f, 1.0f, 1.1f, 0xFFFF40U, 3,
         pc_text_pick(s_st.capturing ? s_cal_title : s_title));

    draw_panel(GCS_MAIN, cfg, have ? &view : NULL);
    draw_panel(GCS_C, cfg, have ? &view : NULL);

    if (!s_st.capturing) {
        for (i = 0; i < ROWS; i++) {
            float y = y0 + sp * i;
            int sel = (i == s_st.cursor);
            int en = (i == ROW_PAD || i == ROW_BACK || cfg != NULL);
            if (sel) {
                textdraw_reset();
                textdraw_set_font(3);
                textdraw_set_flags(0xA0000000U);
                textdraw_set_mul_color(0xFFFF40U);
                textdraw_set_add_color(glow(s_st.timer));
                textdraw_set_scale(0.8f, 0.85f);
                textdraw_set_pos(130.0f, y);
                textdraw_print(">");
            }
            text(160.0f, y, 0.75f, 0.85f, !en ? 0x808080U : sel ? 0xFFFF40U : 0xFFFFFFU, 0,
                 pc_text_pick(s_row_names[i]));
            {
                const char *v = row_value(i, cfg);
                if (v[0]) {
                    char buf[160];
                    if (sel && (i == ROW_PAD || i == ROW_GATE || i == ROW_DEADZONE || i == ROW_SENS) && cfg)
                        snprintf(buf, sizeof buf, "<%s>", v);
                    else
                        snprintf(buf, sizeof buf, "%s", v);
                    text(440.0f, y, 0.75f, 0.85f, en ? 0xFFFFFFU : 0x808080U, 3, buf);
                }
            }
        }
        textdraw_reset();
        fn_3_140CC(s_st.timer);
        textdraw_set_scale(0.8f, 0.8f);   /* below the last row, above the hint line */
        textdraw_set_pos(320.0f, y0 + sp * ROWS + 20.0f);
        textdraw_print_and_fit_to_width(540.0f, (char *)(cfg || s_st.cursor != ROW_PAD
                                                         ? pc_text_pick(s_row_descs[s_st.cursor])
                                                         : pc_text_pick(s_kb_note)));
        text(320.0f, 444.0f, 0.6f, 0.7f, 0xC0C0C0U, 3, pc_text_pick(s_hint));
    } else {
        textdraw_reset();
        fn_3_140CC(s_st.timer);
        textdraw_set_pos(320.0f, 340.0f);
        textdraw_print_and_fit_to_width(540.0f, (char *)pc_text_pick(s_row_descs[ROW_CALIBRATE]));
        text(320.0f, 444.0f, 0.7f, 0.8f, 0xC0C0C0U, 3, pc_text_pick(s_cal_hint));
    }

    reset_ui_widescreen_scale_mtx();
    GXSetZMode_cached(1U, 3, 1U);
}

/* ---------------------------------------------------------------- input */
static u16 pad_held(int with_stick)
{
    u16 b = 0;
    int i;
    for (i = 0; i < 4; i++) {
        PADStatus *s = &pad_status_groups[i].unk0[0];
        if (s->err != 0 && i != 0)
            continue;
        b |= s->button;
        if (!with_stick) continue;
        /* processed values (60 = full tilt) */
        if (s->stickX < -40) b |= PAD_BUTTON_LEFT;
        if (s->stickX > 40) b |= PAD_BUTTON_RIGHT;
        if (s->stickY > 40) b |= PAD_BUTTON_UP;
        if (s->stickY < -40) b |= PAD_BUTTON_DOWN;
    }
    return b;
}

static void save_dirty(void)
{
    int i;
    for (i = 0; i < 4; i++)
        if (s_st.dirty[i]) {
            input_port_config_save(i);
            s_st.dirty[i] = 0;
        }
}

static int step(int r, int dir)
{
    GcsPadConfig *cfg = input_port_config(s_st.port);
    switch (r) {
    case ROW_PAD: {
        int i, p = s_st.port;
        for (i = 0; i < 4; i++) {
            p = (p + dir + 4) % 4;
            if (input_port_connected(p)) break;
        }
        if (p == s_st.port) return 0;
        s_st.port = p;
        return 1;
    }
    case ROW_GATE:
        if (!cfg) return 0;
        cfg->gate = cfg->gate == GCS_GATE_CIRCLE ? GCS_GATE_OCTAGON : GCS_GATE_CIRCLE;
        break;
    case ROW_DEADZONE:
        if (!cfg) return 0;
        if ((dir < 0 && cfg->deadzone <= 0) || (dir > 0 && cfg->deadzone >= PADCFG_DEADZONE_MAX)) return 0;
        cfg->deadzone += dir;
        break;
    case ROW_SENS:
        if (!cfg) return 0;
        if ((dir < 0 && cfg->sensitivity <= PADCFG_SENS_MIN) || (dir > 0 && cfg->sensitivity >= PADCFG_SENS_MAX)) return 0;
        cfg->sensitivity += dir * 5;
        break;
    default:
        return 0;
    }
    s_st.dirty[s_st.port] = 1;
    return 1;
}

static void capture_tick(void)
{
    PadStickView v;
    int k;
    if (!input_port_view(s_st.port, &v)) return;
    for (k = 0; k < 2; k++) {
        gcs_cal_update(&s_st.cap[k], s_st.prev[k][0], s_st.prev[k][1], v.host[k][0], v.host[k][1]);
        s_st.prev[k][0] = v.host[k][0];
        s_st.prev[k][1] = v.host[k][1];
    }
}

/* Runs the screen; `captured`: the frame behind is already captured (from
 * the F1 overlay), else capture the game frame first. */
void pc_stick_screen_run(int captured)
{
    u16 prev, held, press;
    int rep = 0;
    unsigned long long next_ns;

    if (!captured && !pc_frame_capture())
        return;
    memset(&s_st, 0, sizeof s_st);
    s_st.port = first_port();
    printf("[sticks] screen open (port %d)\n", s_st.port + 1);
    fflush(stdout);

    prev = pad_held(1);
    next_ns = platform_get_time_ns();
    for (;;) {
        int close = 0;
        GcsPadConfig *cfg;

        pc_frame_restore(0.35f);
        screen_draw();
        platform_swap_buffers();
#ifdef _WIN32
        g_shim_frame_pulse++;
#endif
        input_poll_gc_pads();
        platform_poll_events();
        if (platform_should_close())
            close = 1;
        if (!input_port_connected(s_st.port)) {
            s_st.capturing = 0;
            s_st.port = first_port();
        }
        cfg = input_port_config(s_st.port);

        held = pad_held(!s_st.capturing);
        press = held & ~prev;
        if (held & (PAD_BUTTON_UP | PAD_BUTTON_DOWN | PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT)) {
            if (++rep > 20 && (rep % 4) == 0)
                press |= held & (PAD_BUTTON_UP | PAD_BUTTON_DOWN | PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT);
        } else {
            rep = 0;
        }
        prev = held;

        if (s_st.capturing) {
            capture_tick();
            if (press & PAD_BUTTON_A) {
                int k;
                for (k = 0; k < 2 && cfg; k++)
                    if (gcs_cal_done(&s_st.cap[k])) {
                        cfg->cal[k] = s_st.cap[k];
                        cfg->cal[k].valid = 1;
                        s_st.dirty[s_st.port] = 1;
                    }
                printf("[sticks] calibration done: control stick %d/%d, C stick %d/%d rays\n",
                       gcs_cal_coverage(&s_st.cap[0]), GCS_CAL_SAMPLES,
                       gcs_cal_coverage(&s_st.cap[1]), GCS_CAL_SAMPLES);
                s_st.capturing = 0;
                save_dirty();
                call_SoundReqID_arg_0(0x65);
            } else if (press & PAD_BUTTON_B) {
                s_st.capturing = 0;
                call_SoundReqID_arg_0(0x66);
            }
        } else {
            if (press & PAD_BUTTON_UP) {
                s_st.cursor = (s_st.cursor + ROWS - 1) % ROWS;
                call_SoundReqID_arg_0(0x6C);
            } else if (press & PAD_BUTTON_DOWN) {
                s_st.cursor = (s_st.cursor + 1) % ROWS;
                call_SoundReqID_arg_0(0x6C);
            } else if (press & (PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT)) {
                if (step(s_st.cursor, (press & PAD_BUTTON_LEFT) ? -1 : 1))
                    call_SoundReqID_arg_0(0x6C);
            }
            if (press & PAD_BUTTON_A) {
                switch (s_st.cursor) {
                case ROW_CALIBRATE:
                    if (cfg) {
                        PadStickView v;
                        int k;
                        gcs_cal_begin(&s_st.cap[0]);
                        gcs_cal_begin(&s_st.cap[1]);
                        input_port_view(s_st.port, &v);
                        for (k = 0; k < 2; k++) {
                            s_st.prev[k][0] = v.host[k][0];
                            s_st.prev[k][1] = v.host[k][1];
                        }
                        s_st.capturing = 1;
                        printf("[sticks] calibrating port %d\n", s_st.port + 1);
                        call_SoundReqID_arg_0(0x65);
                    } else {
                        call_SoundReqID_arg_0(0x66);
                    }
                    break;
                case ROW_RESET:
                    if (cfg) {
                        gcs_config_defaults(cfg);
                        s_st.dirty[s_st.port] = 1;
                        save_dirty();
                        call_SoundReqID_arg_0(0x65);
                    } else {
                        call_SoundReqID_arg_0(0x66);
                    }
                    break;
                case ROW_BACK:
                    close = 1;
                    break;
                case ROW_GATE:
                    if (step(ROW_GATE, 1)) call_SoundReqID_arg_0(0x6C);
                    break;
                default:
                    break;
                }
            } else if (press & (PAD_BUTTON_B | 0x1000 /* START */)) {
                close = 1;
            }
        }
        if (close) {
            call_SoundReqID_arg_0(0x66);
            break;
        }
        s_st.timer++;

        next_ns += 16666667ULL;
        {
            unsigned long long now = platform_get_time_ns();
            if (now < next_ns)
                platform_wait_until_ns(next_ns);
            else if (now > next_ns + 100000000ULL)
                next_ns = now;
        }
    }
    save_dirty();
    /* the button that closed the screen must not reach the game screen
     * behind (it runs next with the pads as this loop's last poll left
     * them): no presses pending */
    {
        int i;
        for (i = 0; i < 4; i++) {
            pad_status_groups[i].unk0[1].button = pad_status_groups[i].unk0[0].button;
            pad_status_groups[i].unk0[2].button = 0;
            pad_status_groups[i].unk0[3].button = 0;
            pad_status_groups[i].unk0[4].button = 0;
        }
    }
    if (!captured)
        pc_frame_restore(1.0f);
    printf("[sticks] screen closed\n");
    fflush(stdout);
}
