/* In-game updater UI: the release notes box and the Updates screen, drawn
 * with the game's own pieces (wood box, menu font, menu sounds, the yes/no
 * style of pc_quit.c).  The model and network side is pc_update.c; see
 * docs/updater.md. */
#include "gc_intrinsics.h"

struct StickPoint;
struct MemcardRequest;
struct TextBox;
#include "sel_ngc.h"

#include "pc_settings.h"
#include "pc_quit.h"
#include "pc_text.h"
#include "pc_update.h"

void fn_3_140CC(int counter);                    /* sel_ngc_sprite.c */
void C_MTXOrtho(Mtx44 m, float t, float b, float l, float r, float n, float f);
extern s32 g_smth_with_sprite_draw_reqs_widescreen; /* bitmap.c */
float textdraw_get_pixel_width_of_string(char *str);
void textdraw_print_and_fit_to_width(float maxWidth, char *str);
int pc_text_is_lead(unsigned char c);            /* textdraw.c */
extern u32 fn_1_C2478(void);                     /* hud.c: yes/no cursor flash */

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

/* ---------------------------------------------------------------- strings */
static Tr s_t_title = { "Update available", "Update verfügbar", "Mise à jour disponible",
                        "Actualización disponible", "Aggiornamento disponibile", "アップデートが あります" };
static Tr s_t_version = { "Version %s  (you have %s)", "Version %s  (installiert: %s)", "Version %s  (vous avez la %s)",
                          "Versión %s  (tienes la %s)", "Versione %s  (hai la %s)", "バージョン %s（いまは %s）" };
static Tr s_t_update = { "Update Now", "Jetzt aktualisieren", "Mettre à jour", "Actualizar ahora", "Aggiorna ora",
                         "いますぐ アップデート" };
static Tr s_t_open = { "Open Download Page", "Download-Seite öffnen", "Page de téléchargement",
                       "Abrir página de descarga", "Apri pagina download", "ダウンロードページ" };
static Tr s_t_later = { "Later", "Später", "Plus tard", "Más tarde", "Più tardi", "あとで" };
static Tr s_t_skip = { "Skip This Version", "Version überspringen", "Ignorer cette version", "Omitir esta versión",
                       "Salta questa versione", "このバージョンは スキップ" };
static Tr s_t_note_inst = {
    "Update now downloads Setup, checks it and installs it. Your saves and settings are kept.",
    "Jetzt aktualisieren lädt Setup, prüft und installiert es. Spielstände und Einstellungen bleiben.",
    "Mettre à jour télécharge l'installation, la vérifie et l'installe. Sauvegardes et réglages sont gardés.",
    "Actualizar descarga el instalador, lo comprueba y lo instala. Tus partidas y ajustes se conservan.",
    "Aggiorna scarica l'installer, lo verifica e lo installa. Salvataggi e impostazioni restano.",
    "セットアップを ダウンロードして かくにんし、インストールします。セーブと せっていは のこります。" };
static Tr s_t_note_port = {
    "Portable version: download the new version from the release page.",
    "Portable Version: die neue Version auf der Release-Seite herunterladen.",
    "Version portable : téléchargez la nouvelle version sur la page de la version.",
    "Versión portable: descarga la versión nueva en la página de la versión.",
    "Versione portatile: scarica la nuova versione dalla pagina della release.",
    "ポータブルばん：リリースページから あたらしい バージョンを ダウンロードしてください。" };
static Tr s_t_no_notes = { "(no release notes)", "(keine Versionshinweise)", "(pas de notes de version)",
                           "(sin notas de la versión)", "(nessuna nota di rilascio)", "（リリースノートは ありません）" };
static Tr s_t_downloading = { "Downloading %s", "Herunterladen: %s", "Téléchargement : %s", "Descargando %s",
                              "Download di %s", "ダウンロードちゅう：%s" };
static Tr s_t_verifying = { "Checking the download (SHA-256)...", "Download wird geprüft (SHA-256)...",
                            "Vérification du téléchargement (SHA-256)...", "Comprobando la descarga (SHA-256)...",
                            "Verifica del download (SHA-256)...", "ダウンロードを かくにんちゅう（SHA-256）..." };
static Tr s_t_launching = { "Starting Setup - please confirm the Windows prompt.",
                            "Setup wird gestartet - bitte die Windows-Abfrage bestätigen.",
                            "Lancement de l'installation - confirmez la demande de Windows.",
                            "Iniciando la instalación: confirma el aviso de Windows.",
                            "Avvio dell'installazione: conferma la richiesta di Windows.",
                            "セットアップを きどうします。Windowsの かくにんに こたえてください。" };
static Tr s_t_launched = { "Setup is running. The game closes now.", "Setup läuft. Das Spiel wird beendet.",
                           "L'installation est lancée. Le jeu se ferme.", "La instalación está en marcha. El juego se cierra.",
                           "L'installazione è avviata. Il gioco si chiude.", "セットアップを きどうしました。ゲームを おわります。" };
static Tr s_t_hint_notes = { "Up/Down: scroll   Left/Right: choose   A: OK   B: later",
                             "Hoch/Runter: blättern   Links/Rechts: wählen   A: OK   B: später",
                             "Haut/Bas : défiler   Gauche/Droite : choisir   A : valider   B : plus tard",
                             "Arriba/Abajo: desplazar   Izq./Der.: elegir   A: aceptar   B: más tarde",
                             "Su/Giù: scorri   Sinistra/Destra: scegli   A: conferma   B: più tardi",
                             "うえ/した:スクロール   ひだり/みぎ:えらぶ   A:けってい   B:あとで" };
static Tr s_t_hint_choose = { "Left/Right: choose   A: OK   B: close", "Links/Rechts: wählen   A: OK   B: schließen",
                              "Gauche/Droite : choisir   A : valider   B : fermer", "Izq./Der.: elegir   A: aceptar   B: cerrar",
                              "Sinistra/Destra: scegli   A: conferma   B: chiudi", "ひだり/みぎ:えらぶ   A:けってい   B:とじる" };
static Tr s_t_hint_dl = { "B: cancel", "B: Abbrechen", "B : annuler", "B: cancelar", "B: annulla", "B:キャンセル" };

/* errors, by PCU_ERR_* */
static Tr s_t_err[] = {
    { "", "", "", "", "", "" },
    { "Could not reach the update server (offline?).", "Update-Server nicht erreichbar (offline?).",
      "Serveur de mise à jour injoignable (hors ligne ?).", "No se pudo contactar con el servidor (¿sin conexión?).",
      "Server degli aggiornamenti non raggiungibile (offline?).", "サーバーに つながりません（オフライン？）。" },
    { "The update server sent an unexpected reply.", "Unerwartete Antwort vom Update-Server.",
      "Réponse inattendue du serveur de mise à jour.", "Respuesta inesperada del servidor.",
      "Risposta inattesa dal server.", "サーバーから よそうがいの へんじが ありました。" },
    { "The update server sent an unexpected reply.", "Unerwartete Antwort vom Update-Server.",
      "Réponse inattendue du serveur de mise à jour.", "Respuesta inesperada del servidor.",
      "Risposta inattesa dal server.", "サーバーから よそうがいの へんじが ありました。" },
    { "No release has been published yet.", "Es gibt noch keine Version.", "Aucune version publiée.",
      "Aún no hay ninguna versión.", "Nessuna versione pubblicata.", "まだ リリースが ありません。" },
    { "This release has no installer - please use the download page.",
      "Diese Version hat kein Setup - bitte die Download-Seite nutzen.",
      "Cette version n'a pas d'installateur - utilisez la page de téléchargement.",
      "Esta versión no tiene instalador: usa la página de descarga.",
      "Questa versione non ha un installer: usa la pagina di download.",
      "セットアップが ありません。ダウンロードページを つかってください。" },
    { "No checksum (SHA256SUMS.txt) for the installer - not installing.",
      "Keine Prüfsumme (SHA256SUMS.txt) für das Setup - keine Installation.",
      "Pas de somme de contrôle (SHA256SUMS.txt) - installation refusée.",
      "Sin suma de comprobación (SHA256SUMS.txt): no se instala.",
      "Nessun checksum (SHA256SUMS.txt): installazione rifiutata.",
      "チェックサム（SHA256SUMS.txt）が ないので インストールしません。" },
    { "The download is damaged (SHA-256 mismatch) - deleted, not installing.",
      "Download beschädigt (SHA-256 falsch) - gelöscht, keine Installation.",
      "Téléchargement corrompu (SHA-256 différent) - supprimé, pas d'installation.",
      "Descarga dañada (SHA-256 distinto): borrada, no se instala.",
      "Download danneggiato (SHA-256 diverso): eliminato, non installato.",
      "ダウンロードが こわれています（SHA-256）。インストールしません。" },
    { "The download failed. Please try again later.", "Download fehlgeschlagen. Bitte später erneut versuchen.",
      "Échec du téléchargement. Réessayez plus tard.", "La descarga falló. Inténtalo más tarde.",
      "Download non riuscito. Riprova più tardi.", "ダウンロードに しっぱいしました。あとで ためしてください。" },
    { "Could not save the download (disk full?).", "Download konnte nicht gespeichert werden (Datenträger voll?).",
      "Impossible d'enregistrer le téléchargement (disque plein ?).", "No se pudo guardar la descarga (¿disco lleno?).",
      "Impossibile salvare il download (disco pieno?).", "ダウンロードを ほぞんできません（ディスクが いっぱい？）。" },
    { "Download cancelled.", "Download abgebrochen.", "Téléchargement annulé.", "Descarga cancelada.",
      "Download annullato.", "ダウンロードを ちゅうししました。" },
    { "Setup could not be started.", "Setup konnte nicht gestartet werden.", "Impossible de lancer l'installation.",
      "No se pudo iniciar la instalación.", "Impossibile avviare l'installazione.", "セットアップを きどうできません。" },
    { "Setup needs administrator rights (the prompt was declined).",
      "Setup braucht Administratorrechte (Abfrage abgelehnt).",
      "L'installation nécessite les droits d'administrateur (refusé).",
      "La instalación necesita permisos de administrador (rechazado).",
      "L'installazione richiede i diritti di amministratore (rifiutato).",
      "セットアップには かんりしゃけんげんが ひつようです。" },
    { "Update checks are not set up in this build.", "Updates sind in diesem Build nicht eingerichtet.",
      "Les mises à jour ne sont pas configurées dans cette version.",
      "Las actualizaciones no están configuradas en esta versión.",
      "Gli aggiornamenti non sono configurati in questa build.", "このビルドでは アップデートを つかえません。" },
};
#define N_ERR ((int)(sizeof s_t_err / sizeof s_t_err[0]))

/* Updates screen */
static Tr s_u_title = { "Updates", "Updates", "Mises à jour", "Actualizaciones", "Aggiornamenti", "アップデート" };
static Tr s_u_this = { "This version: %s (%s)", "Diese Version: %s (%s)", "Cette version : %s (%s)",
                       "Esta versión: %s (%s)", "Questa versione: %s (%s)", "このバージョン：%s（%s）" };
static Tr s_u_installed = { "installed", "installiert", "installée", "instalada", "installata", "インストールばん" };
static Tr s_u_portable = { "portable", "portabel", "portable", "portable", "portatile", "ポータブルばん" };
enum { UROW_AUTO, UROW_NOW, UROW_BACK, UROWS };
static Tr s_u_rows[UROWS] = {
    { "Check Automatically", "Automatisch suchen", "Vérifier automatiquement", "Buscar automáticamente",
      "Controlla automaticamente", "じどうで さがす" },
    { "Check Now", "Jetzt suchen", "Vérifier maintenant", "Buscar ahora", "Controlla ora", "いますぐ さがす" },
    { "Back", "Zurück", "Retour", "Volver", "Indietro", "もどる" },
};
static Tr s_u_descs[UROWS] = {
    { "At start, once a day at most. Only the version number is sent.",
      "Beim Start, höchstens täglich. Gesendet wird nur die Versionsnummer.",
      "Au démarrage, une fois par jour au plus. Seule la version est envoyée.",
      "Al iniciar, una vez al día como mucho. Solo se envía la versión.",
      "All'avvio, al massimo una volta al giorno. Si invia solo la versione.",
      "きどうじに 1にち 1かいまで。おくるのは バージョンだけ。" },
    { "Look for a new version now.", "Jetzt nach einer neuen Version suchen.",
      "Rechercher une nouvelle version maintenant.", "Buscar una versión nueva ahora.",
      "Cerca subito una nuova versione.", "いますぐ あたらしい バージョンを さがします。" },
    { "Back to the previous screen.", "Zurück zum vorigen Bildschirm.", "Revenir à l'écran précédent.",
      "Volver a la pantalla anterior.", "Torna alla schermata precedente.", "まえの がめんに もどります。" },
};
static Tr s_u_checking = { "Checking for updates...", "Suche nach Updates...", "Recherche de mises à jour...",
                           "Buscando actualizaciones...", "Ricerca aggiornamenti...", "アップデートを さがしています..." };
static Tr s_u_latest = { "You have the latest version.", "Du hast die neueste Version.", "Vous avez la dernière version.",
                         "Tienes la última versión.", "Hai l'ultima versione.", "さいしんの バージョンです。" };
static Tr s_u_avail = { "Version %s is available.", "Version %s ist verfügbar.", "La version %s est disponible.",
                        "La versión %s está disponible.", "La versione %s è disponibile.", "バージョン %s が あります。" };
static Tr s_u_hint = { "Up/Down: select   Left/Right: change   A: choose   B: back",
                       "Hoch/Runter: wählen   Links/Rechts: ändern   A: OK   B: zurück",
                       "Haut/Bas : choisir   Gauche/Droite : changer   A : valider   B : retour",
                       "Arriba/Abajo: elegir   Izq./Der.: cambiar   A: aceptar   B: volver",
                       "Su/Giù: scegli   Sinistra/Destra: cambia   A: conferma   B: indietro",
                       "うえ/した:えらぶ   ひだり/みぎ:かえる   A:けってい   B:もどる" };
static Tr s_on = { "On", "Ein", "Oui", "Sí", "Sì", "オン" };
static Tr s_off = { "Off", "Aus", "Non", "No", "No", "オフ" };

static const char *err_text(int e)
{
    return pc_text_pick(s_t_err[(e >= 0 && e < N_ERR) ? e : PCU_ERR_DOWNLOAD]);
}

/* ---------------------------------------------------------------- requests */
static volatile int s_req;   /* 1 notes box, 2 Updates screen */
void pc_update_notes_request(void) { if (!s_req) s_req = 1; }
void pc_update_screen_request(void) { s_req = 2; }
int pc_update_ui_take_request(void) { int r = s_req; s_req = 0; return r; }

/* ---------------------------------------------------------------- drawing helpers */
static u32 glow(int timer)
{
    u32 g = (u32)(64.0 * fabs(math_sin((s16)(timer * 0x180))));
    return g | (g << 8) | (g << 16);
}

static void font(float sx, float sy, u32 color, int align)
{
    textdraw_reset();
    textdraw_set_font(3);
    textdraw_set_flags(0xA0000000U);
    textdraw_set_alignment(align);
    textdraw_set_mul_color(color);
    textdraw_set_scale(sx, sy);
}

static void text(float x, float y, float sx, float sy, u32 color, int align, const char *s)
{
    font(sx, sy, color, align);
    textdraw_set_pos(x, y);
    textdraw_print((char *)s);
}

static void text_fit(float x, float y, float sx, float sy, u32 color, float w, const char *s)
{
    font(sx, sy, color, 3);
    textdraw_set_pos(x, y);
    textdraw_print_and_fit_to_width(w, (char *)s);
}

/* The game's text parser treats "a/", "p/", "c/0x", "/bc../", "/oc../" and
 * "/cd../" as control codes: release notes (paths, URLs) are printed and
 * measured with every '/' as its own piece so none of them can form. */
static float safe_width(const char *s)
{
    char seg[512];
    float w = 0.0f;
    while (*s) {
        size_t n = (*s == '/') ? 1 : strcspn(s, "/");
        if (n >= sizeof seg) n = sizeof seg - 1;
        memcpy(seg, s, n);
        seg[n] = 0;
        w += textdraw_get_pixel_width_of_string(seg);
        s += n;
    }
    return w;
}

static void safe_print(const char *s)
{
    char seg[512];
    while (*s) {
        size_t n = (*s == '/') ? 1 : strcspn(s, "/");
        if (n >= sizeof seg) n = sizeof seg - 1;
        memcpy(seg, s, n);
        seg[n] = 0;
        textdraw_print(seg);
        s += n;
    }
}

static void begin_2d(void)
{
    Mtx44 m;
    g_some_sprite_reset_gx_state_func();
    mtxa_from_identity();
    GXLoadPosMtxImm(mtxa->mtxA, 0);
    C_MTXOrtho(m, 0.0f, 480.0f, 0.0f, 640.0f, 0.0f, 20000.0f);
    g_something_with_GXSetProjectionv(m, 1);
    g_smth_with_sprite_draw_reqs_widescreen = 1;
    GXSetZMode_cached(1U, 7, 1U);
    set_ui_widescreen_scale_mtx(0x140);
}

static void end_2d(void)
{
    reset_ui_widescreen_scale_mtx();
    GXSetZMode_cached(1U, 3, 1U);
}

static void wood_box(float cx, float cy, float w, float h)
{
    float bw, bh;
    init_ui_element_sprite_with_defaults();
    set_ui_element_sprite_depth(0.11f);
    set_ui_element_sprite_pos(cx, cy);
    bw = (float)g_get_bmp_width(5);
    bh = (float)g_get_bmp_height(5);
    if (bw > 0.0f && bh > 0.0f)
        set_ui_element_sprite_scale(w / bw, h / bh);
    draw_ui_box(5);
}

/* horizontal bar from x (left) w px wide: the font's block glyph stretched */
static void bar(float x, float y, float w, float h, u32 color)
{
    const char *blk = pc_text_game("■");
    float gw;
    if (w <= 0.5f) return;
    font(1.0f, 1.0f, color, 0);
    gw = textdraw_get_pixel_width_of_string((char *)blk);
    if (gw <= 0.0f) gw = 24.0f;
    textdraw_set_scale(w / gw, h / 24.0f);
    textdraw_set_pos(x, y);
    textdraw_print((char *)blk);
}

/* ---------------------------------------------------------------- input */
static u16 pad_held(void)
{
    u16 b = 0;
    int i;
    for (i = 0; i < 4; i++) {
        PADStatus *s = &pad_status_groups[i].unk0[0];
        if (s->err != 0 && i != 0)
            continue;
        b |= s->button;
        if (s->stickX < -40) b |= PAD_BUTTON_LEFT;
        if (s->stickX > 40) b |= PAD_BUTTON_RIGHT;
        if (s->stickY > 40) b |= PAD_BUTTON_UP;
        if (s->stickY < -40) b |= PAD_BUTTON_DOWN;
    }
    return b;
}

typedef struct { u16 prev; int rep; unsigned long long next_ns; } Loop;

static void loop_begin(Loop *l)
{
    l->prev = pad_held();
    l->rep = 0;
    l->next_ns = platform_get_time_ns();
}

/* present the drawn frame, poll, return newly pressed buttons (with repeat
 * for the directions); *close set when the window is closed */
static u16 loop_frame(Loop *l, int *close)
{
    u16 held, press;
    platform_swap_buffers();
#ifdef _WIN32
    g_shim_frame_pulse++;
#endif
    input_poll_gc_pads();
    platform_poll_events();
    if (platform_should_close())
        *close = 1;
    held = pad_held();
    press = held & ~l->prev;
    if (held & (PAD_BUTTON_UP | PAD_BUTTON_DOWN | PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT)) {
        if (++l->rep > 20 && (l->rep % 4) == 0)
            press |= held & (PAD_BUTTON_UP | PAD_BUTTON_DOWN | PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT);
    } else {
        l->rep = 0;
    }
    l->prev = held;
    l->next_ns += 16666667ULL;
    {
        unsigned long long now = platform_get_time_ns();
        if (now < l->next_ns)
            platform_wait_until_ns(l->next_ns);
        else if (now > l->next_ns + 100000000ULL)
            l->next_ns = now;
    }
    return press;
}

/* the button that closed a modal screen must not reach the game screen */
static void swallow_presses(void)
{
    int i;
    for (i = 0; i < 4; i++) {
        pad_status_groups[i].unk0[1].button = pad_status_groups[i].unk0[0].button;
        pad_status_groups[i].unk0[2].button = 0;
        pad_status_groups[i].unk0[3].button = 0;
        pad_status_groups[i].unk0[4].button = 0;
    }
}

/* ======================================================================
 * 1. Release notes box
 * ====================================================================== */
#define NOTES_X 56.0f
#define NOTES_Y 104.0f
#define NOTES_W 520.0f
#define NOTES_PITCH 17.0f
#define NOTES_ROWS 13
#define NOTES_SX 0.6f
#define NOTES_SY 0.68f
#define MAX_LINES 800

enum { ACT_UPDATE, ACT_OPEN, ACT_LATER, ACT_SKIP };

static struct {
    char *lines[MAX_LINES];
    u8 heading[MAX_LINES];
    int nlines, first, wrapped;
    int acts[3], nacts, sel;
    int phase;          /* 0 notes, 1 download running, 2 error, 3 launched */
    int err, timer, done_timer;
} s_nb;

static void notes_free(void)
{
    int i;
    for (i = 0; i < s_nb.nlines; i++) free(s_nb.lines[i]);
    s_nb.nlines = 0;
}

static void add_line(const char *s, size_t n, int heading)
{
    char *p;
    if (s_nb.nlines >= MAX_LINES) return;
    p = (char *)malloc(n + 1);
    if (!p) return;
    memcpy(p, s, n);
    p[n] = 0;
    s_nb.heading[s_nb.nlines] = (u8)heading;
    s_nb.lines[s_nb.nlines++] = p;
}

/* word-wrap the plain notes (game encoding) to NOTES_W with the menu font */
static void notes_wrap(void)
{
    const PcuRelease *r = pc_update_release();
    char *plain, *s;
    static char conv[8192];
    notes_free();
    plain = pc_update_plain_notes(r && r->body ? r->body : "");
    font(NOTES_SX, NOTES_SY, 0xFFFFFFU, 0);
    s = plain;
    while (*s && s_nb.nlines < MAX_LINES) {
        char *e = s + strcspn(s, "\n");
        char save = *e;
        int heading = 0;
        char *p, *line, *last_space;
        size_t ind;
        int first;
        *e = 0;
        if (*s == '\x01') { heading = 1; s++; }
        pc_text_game_buf(s, conv, sizeof conv);
        *e = save;
        if (!conv[0]) add_line("", 0, 0);
        /* greedy: extend the line char by char, break at the last space,
         * else (no space: Japanese, long URLs) before the char that overflows */
        /* hanging indent for list items: continuation lines line up with
         * the text after "- " */
        {
            size_t k = strspn(conv, " ");
            if (conv[k] == '-' && conv[k + 1] == ' ') k += 2;
            ind = k < 16 ? k : 16;
        }
        line = conv;
        first = 1;
        while (*line) {
            char tmp[1024];
            size_t len = 0, pre = first ? 0 : ind;
            last_space = NULL;
            memset(tmp, ' ', pre);
            tmp[pre] = 0;
            p = line;
            while (*p) {
                size_t cl = (pc_text_is_lead((unsigned char)*p) && p[1]) ? 2 : 1;
                if (pre + len + cl >= sizeof tmp) break;
                memcpy(tmp + pre + len, p, cl);
                tmp[pre + len + cl] = 0;
                if (safe_width(tmp) > NOTES_W && len > 0)
                    break;
                if (*p == ' ') last_space = p;
                len += cl;
                p += cl;
            }
            if (*p && last_space && last_space > line)
                p = last_space;
            memcpy(tmp + pre, line, (size_t)(p - line));
            add_line(tmp, pre + (size_t)(p - line), heading);
            line = p;
            while (*line == ' ') line++;
            first = 0;
            if (s_nb.nlines >= MAX_LINES) break;
        }
        s = *e ? e + 1 : e;
    }
    free(plain);
    if (s_nb.nlines == 0) {
        const char *t = pc_text_pick(s_t_no_notes);
        add_line(t, strlen(t), 0);
    }
    s_nb.wrapped = 1;
}

static void set_actions(int phase)
{
    s_nb.nacts = 0;
    if (phase == 0) {
        s_nb.acts[s_nb.nacts++] = pc_update_installed() ? ACT_UPDATE : ACT_OPEN;
        s_nb.acts[s_nb.nacts++] = ACT_LATER;
        s_nb.acts[s_nb.nacts++] = ACT_SKIP;
    } else if (phase == 2) {
        s_nb.acts[s_nb.nacts++] = ACT_OPEN;
        s_nb.acts[s_nb.nacts++] = ACT_LATER;
    }
    s_nb.sel = 0;
}

static const char *act_label(int a)
{
    switch (a) {
    case ACT_UPDATE: return pc_text_pick(s_t_update);
    case ACT_OPEN:   return pc_text_pick(s_t_open);
    case ACT_LATER:  return pc_text_pick(s_t_later);
    default:         return pc_text_pick(s_t_skip);
    }
}

static void notes_draw(void)
{
    const PcuRelease *r = pc_update_release();
    char buf[256], fmt[256];
    int i;

    begin_2d();
    wood_box(320.0f, 240.0f, 600.0f, 456.0f);
    text(320.0f, 24.0f, 1.0f, 1.1f, 0xFFFF40U, 3, pc_text_pick(s_t_title));
    snprintf(fmt, sizeof fmt, "%s", pc_text_pick(s_t_version));
    snprintf(buf, sizeof buf, fmt, r ? r->version : "?", pc_update_current_version());
    text(320.0f, 56.0f, 0.75f, 0.8f, 0xFFFFFFU, 3, buf);
    if (r && r->name[0] && strcmp(r->name, r->tag) != 0 && strcmp(r->name, r->version) != 0) {
        char nm[256];
        pc_text_game_buf(r->name, nm, sizeof nm);
        font(0.65f, 0.7f, 0xFFE080U, 3);
        textdraw_set_pos(320.0f, 78.0f);
        {
            float w = safe_width(nm);
            if (w > 540.0f) textdraw_set_scale(0.65f * 540.0f / w, 0.7f);
        }
        safe_print(nm);
    }

    if (s_nb.phase == 0) {
        if (!s_nb.wrapped) notes_wrap();
        for (i = 0; i < NOTES_ROWS && s_nb.first + i < s_nb.nlines; i++) {
            int k = s_nb.first + i;
            font(NOTES_SX, NOTES_SY, s_nb.heading[k] ? 0xFFFF40U : 0xFFFFFFU, 0);
            textdraw_set_pos(NOTES_X, NOTES_Y + NOTES_PITCH * i);
            safe_print(s_nb.lines[k]);
        }
        /* scroll marks */
        if (s_nb.first > 0)
            text(600.0f, NOTES_Y - 4.0f, 0.6f, 0.6f, 0xFFFF40U, 3, pc_text_game("▲"));
        if (s_nb.first + NOTES_ROWS < s_nb.nlines)
            text(600.0f, NOTES_Y + NOTES_PITCH * (NOTES_ROWS - 1), 0.6f, 0.6f, 0xFFFF40U, 3, pc_text_game("▼"));
        if (s_nb.nlines > NOTES_ROWS) {
            snprintf(buf, sizeof buf, "%d-%d / %d", s_nb.first + 1,
                     s_nb.first + NOTES_ROWS < s_nb.nlines ? s_nb.first + NOTES_ROWS : s_nb.nlines, s_nb.nlines);
            text(560.0f, NOTES_Y + NOTES_PITCH * NOTES_ROWS + 1.0f, 0.5f, 0.55f, 0xA0A0A0U, 3, buf);
        }
        text_fit(320.0f, 344.0f, 0.6f, 0.65f, 0xC0C0C0U, 540.0f,
                 pc_text_pick(pc_update_installed() ? s_t_note_inst : s_t_note_port));
    } else {
        long long done = 0, total = 0;
        int st = pc_update_state();
        pc_update_progress(&done, &total);
        int show_bar = s_nb.phase != 2 || done > 0;   /* failed before any download: no bar */
        snprintf(fmt, sizeof fmt, "%s", pc_text_pick(s_t_downloading));
        snprintf(buf, sizeof buf, fmt, r && r->setup_name[0] ? r->setup_name : "Setup");
        if (show_bar)
            text_fit(320.0f, 140.0f, 0.75f, 0.8f, 0xFFFFFFU, 540.0f, buf);
        /* progress bar: dark track, gold fill */
        if (show_bar)
            bar(90.0f, 186.0f, 460.0f, 22.0f, 0x302010U);
        if (show_bar && total > 0)
            bar(92.0f, 188.0f, 456.0f * (float)((double)(done > total ? total : done) / (double)total), 18.0f,
                s_nb.phase == 2 ? 0xC04040U : 0xFFC020U);
        if (total > 0)
            snprintf(buf, sizeof buf, "%.1f / %.1f MB  (%d%%)", done / 1048576.0, total / 1048576.0,
                     (int)(done * 100 / total));
        else
            snprintf(buf, sizeof buf, "%.1f MB", done / 1048576.0);
        if (show_bar)
            text(320.0f, 222.0f, 0.65f, 0.7f, 0xE0E0E0U, 3, buf);
        if (s_nb.phase == 2)
            text_fit(320.0f, 270.0f, 0.7f, 0.75f, 0xFF7060U, 540.0f, err_text(s_nb.err));
        else if (s_nb.phase == 3)
            text_fit(320.0f, 270.0f, 0.7f, 0.75f, 0x80FF80U, 540.0f, pc_text_pick(s_t_launched));
        else if (st == PCU_VERIFYING)
            text_fit(320.0f, 270.0f, 0.7f, 0.75f, 0xFFFF40U, 540.0f, pc_text_pick(s_t_verifying));
        else if (st == PCU_LAUNCHING)
            text_fit(320.0f, 270.0f, 0.7f, 0.75f, 0xFFFF40U, 540.0f, pc_text_pick(s_t_launching));
    }

    /* choices, game yes/no style: flashing bracket cursor on the selected one */
    for (i = 0; i < s_nb.nacts; i++) {
        float x = s_nb.nacts == 3 ? 140.0f + 180.0f * i : 210.0f + 220.0f * i;
        int sel = (i == s_nb.sel);
        font(0.75f, 0.85f, sel ? 0xFFFF40U : 0xA08060U, 3);
        if (sel) textdraw_set_add_color(fn_1_C2478());
        textdraw_set_pos(x, 384.0f);
        textdraw_print_and_fit_to_width(160.0f, (char *)act_label(s_nb.acts[i]));
        if (sel) {
            float w = textdraw_get_pixel_width_of_string((char *)act_label(s_nb.acts[i]));
            if (w > 160.0f) w = 160.0f;
            text(x - w * 0.5f - 12.0f, 384.0f, 0.75f, 0.85f, 0xFFFF40U, 3, "\x81m");
            text(x + w * 0.5f + 12.0f, 384.0f, 0.75f, 0.85f, 0xFFFF40U, 3, "\x81n");
        }
    }
    if (s_nb.phase != 3 && !(s_nb.phase == 1 && pc_update_state() != PCU_DOWNLOADING))
        text(320.0f, 438.0f, 0.6f, 0.7f, 0xC0C0C0U, 3,
             pc_text_pick(s_nb.phase == 1 ? s_t_hint_dl : s_nb.phase == 2 ? s_t_hint_choose : s_t_hint_notes));
    end_2d();
}

/* Runs the box for the release pc_update_release() (state PCU_AVAILABLE).
 * `captured`: the frame behind is already captured (nested screens). */
void pc_update_notes_run(int captured)
{
    Loop l;
    const PcuRelease *r = pc_update_release();

    if (r == NULL)
        return;
    if (!captured && !pc_frame_capture())
        return;
    pc_update_prompt_shown();
    memset(&s_nb, 0, sizeof s_nb);
    set_actions(0);
    printf("[update] notes box open: %s -> %s (%s)\n", pc_update_current_version(), r->version,
           pc_update_installed() ? "installed: Update now" : "portable: Open download page");
    fflush(stdout);
    call_SoundReqID_arg_0(0x65);

    loop_begin(&l);
    for (;;) {
        int close = 0, st;
        u16 press;

        pc_frame_restore(0.35f);
        notes_draw();
        press = loop_frame(&l, &close);
        st = pc_update_state();

        if (s_nb.phase == 1) {
            if (st == PCU_DOWNLOAD_FAILED || st == PCU_LAUNCH_FAILED) {
                s_nb.err = pc_update_error();
                s_nb.phase = 2;
                set_actions(2);
                call_SoundReqID_arg_0(0x66);
                printf("[update] update failed: error %d\n", s_nb.err);
                fflush(stdout);
            } else if (st == PCU_LAUNCHED) {
                s_nb.phase = 3;
                s_nb.done_timer = 0;
                call_SoundReqID_arg_0(0x65);
            } else if ((press & PAD_BUTTON_B) && (st == PCU_DOWNLOADING)) {
                pc_update_cancel();
                call_SoundReqID_arg_0(0x66);
            }
        } else if (s_nb.phase == 3) {
            /* show "Setup is running" briefly, then quit (pc_quit waits for
             * memory card jobs and saves settings.dat) */
            if (++s_nb.done_timer >= 60) {
                printf("[update] Setup started - quitting the game\n");
                fflush(stdout);
                pc_quit_request("update");
                close = 1;
            }
        } else {
            if (s_nb.phase == 0) {
                int maxfirst = s_nb.nlines - NOTES_ROWS;
                if (maxfirst < 0) maxfirst = 0;
                if ((press & PAD_BUTTON_UP) && s_nb.first > 0) { s_nb.first--; call_SoundReqID_arg_0(0x6C); }
                if ((press & PAD_BUTTON_DOWN) && s_nb.first < maxfirst) { s_nb.first++; call_SoundReqID_arg_0(0x6C); }
                if (press & (PAD_TRIGGER_R | PAD_TRIGGER_L)) {   /* page */
                    s_nb.first += (press & PAD_TRIGGER_R) ? NOTES_ROWS - 1 : -(NOTES_ROWS - 1);
                    if (s_nb.first > maxfirst) s_nb.first = maxfirst;
                    if (s_nb.first < 0) s_nb.first = 0;
                    call_SoundReqID_arg_0(0x6C);
                }
            }
            if ((press & PAD_BUTTON_LEFT) && s_nb.nacts) {
                s_nb.sel = (s_nb.sel + s_nb.nacts - 1) % s_nb.nacts;
                call_SoundReqID_arg_0(0x6C);
            } else if ((press & PAD_BUTTON_RIGHT) && s_nb.nacts) {
                s_nb.sel = (s_nb.sel + 1) % s_nb.nacts;
                call_SoundReqID_arg_0(0x6C);
            }
            if (press & PAD_BUTTON_A) {
                switch (s_nb.acts[s_nb.sel]) {
                case ACT_UPDATE:
                    printf("[update] choice: Update now\n");
                    pc_update_download_start();
                    s_nb.phase = 1;
                    s_nb.nacts = 0;
                    call_SoundReqID_arg_0(0x65);
                    break;
                case ACT_OPEN:
                    printf("[update] choice: Open download page\n");
                    call_SoundReqID_arg_0(pc_update_open_page() ? 0x65 : 0x66);
                    close = 1;
                    break;
                case ACT_LATER:
                    printf("[update] choice: Later\n");
                    close = 1;
                    break;
                case ACT_SKIP:
                    printf("[update] choice: Skip this version\n");
                    pc_update_skip_release();
                    close = 1;
                    break;
                }
                fflush(stdout);
            } else if (press & (PAD_BUTTON_B | 0x1000)) {
                printf("[update] choice: Later (B)\n");
                fflush(stdout);
                close = 1;
            }
            if (close && s_nb.acts[s_nb.sel] != ACT_OPEN)
                call_SoundReqID_arg_0(0x66);
        }
        if (close)
            break;
        s_nb.timer++;
    }
    notes_free();
    swallow_presses();
    if (!captured)
        pc_frame_restore(1.0f);
    printf("[update] notes box closed\n");
    fflush(stdout);
}

/* ======================================================================
 * 2. Updates screen
 * ====================================================================== */
static void screen_draw(int cursor, int timer)
{
    char buf[256], fmt[256];
    int i, st = pc_update_state();
    float y0 = 170.0f, sp = 30.0f;
    const PcuRelease *r = pc_update_release();

    begin_2d();
    wood_box(320.0f, 240.0f, 520.0f, 400.0f);
    text(320.0f, 50.0f, 1.0f, 1.1f, 0xFFFF40U, 3, pc_text_pick(s_u_title));
    snprintf(fmt, sizeof fmt, "%s", pc_text_pick(s_u_this));
    {
        char kind[64];
        snprintf(kind, sizeof kind, "%s", pc_text_pick(pc_update_installed() ? s_u_installed : s_u_portable));
        snprintf(buf, sizeof buf, fmt, pc_update_current_version(), kind);
    }
    text(320.0f, 96.0f, 0.75f, 0.8f, 0xFFFFFFU, 3, buf);
    if (pc_update_repo()[0]) {
        font(0.55f, 0.6f, 0xA0A0A0U, 3);
        textdraw_set_pos(320.0f, 120.0f);
        safe_print("github.com/");
        safe_print(pc_update_repo());
    }

    for (i = 0; i < UROWS; i++) {
        float y = y0 + sp * i;
        int sel = (i == cursor);
        if (sel) {
            font(0.8f, 0.9f, 0xFFFF40U, 0);
            textdraw_set_add_color(glow(timer));
            textdraw_set_pos(110.0f, y);
            textdraw_print(">");
        }
        text(140.0f, y, 0.8f, 0.9f, sel ? 0xFFFF40U : 0xFFFFFFU, 0, pc_text_pick(s_u_rows[i]));
        if (i == UROW_AUTO) {
            snprintf(buf, sizeof buf, sel ? "<%s>" : "%s", pc_text_pick(g_pcs.update_check ? s_on : s_off));
            text(470.0f, y, 0.8f, 0.9f, 0xFFFFFFU, 3, buf);
        }
    }

    /* status of the last check */
    buf[0] = 0;
    switch (st) {
    case PCU_CHECKING: snprintf(buf, sizeof buf, "%s", pc_text_pick(s_u_checking)); break;
    case PCU_UP_TO_DATE: snprintf(buf, sizeof buf, "%s", pc_text_pick(s_u_latest)); break;
    case PCU_CHECK_FAILED: snprintf(buf, sizeof buf, "%s", err_text(pc_update_error())); break;
    case PCU_IDLE:
        if (!pc_update_enabled())
            snprintf(buf, sizeof buf, "%s", err_text(PCU_ERR_DISABLED));
        break;
    default:
        if (r) {
            snprintf(fmt, sizeof fmt, "%s", pc_text_pick(s_u_avail));
            snprintf(buf, sizeof buf, fmt, r->version);
        }
        break;
    }
    if (buf[0])
        text_fit(320.0f, 278.0f, 0.7f, 0.75f, (st == PCU_CHECK_FAILED || st == PCU_IDLE) ? 0xFF7060U : 0x80FF80U,
                 460.0f, buf);

    textdraw_reset();
    fn_3_140CC(timer);
    textdraw_set_scale(0.75f, 0.75f);
    textdraw_set_pos(320.0f, 330.0f);
    textdraw_print_and_fit_to_width(460.0f, (char *)pc_text_pick(s_u_descs[cursor]));
    text(320.0f, 404.0f, 0.6f, 0.7f, 0xC0C0C0U, 3, pc_text_pick(s_u_hint));
    end_2d();
}

void pc_update_screen_run(int captured)
{
    Loop l;
    int cursor = 0, timer = 0, waiting = 0;

    if (!captured && !pc_frame_capture())
        return;
    printf("[update] Updates screen open (version %s, auto check %s)\n", pc_update_current_version(),
           g_pcs.update_check ? "on" : "off");
    fflush(stdout);
    loop_begin(&l);
    for (;;) {
        int close = 0;
        u16 press;

        pc_frame_restore(0.35f);
        screen_draw(cursor, timer);
        press = loop_frame(&l, &close);
        pc_update_poll();

        if (waiting && pc_update_state() != PCU_CHECKING) {
            waiting = 0;
            if (pc_update_state() == PCU_AVAILABLE) {
                /* the found release: its notes box, over the same frame */
                pc_update_notes_run(1);
                if (pc_quit_requested())
                    close = 1;
                loop_begin(&l);
                continue;
            }
            call_SoundReqID_arg_0(pc_update_state() == PCU_UP_TO_DATE ? 0x65 : 0x66);
        }
        if (press & PAD_BUTTON_UP) {
            cursor = (cursor + UROWS - 1) % UROWS;
            call_SoundReqID_arg_0(0x6C);
        } else if (press & PAD_BUTTON_DOWN) {
            cursor = (cursor + 1) % UROWS;
            call_SoundReqID_arg_0(0x6C);
        } else if ((press & (PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT)) && cursor == UROW_AUTO) {
            pcs_set_update_check(!g_pcs.update_check);
            printf("[update] automatic check %s\n", g_pcs.update_check ? "on" : "off");
            call_SoundReqID_arg_0(0x6C);
        }
        if (press & PAD_BUTTON_A) {
            switch (cursor) {
            case UROW_AUTO:
                pcs_set_update_check(!g_pcs.update_check);
                printf("[update] automatic check %s\n", g_pcs.update_check ? "on" : "off");
                call_SoundReqID_arg_0(0x6C);
                break;
            case UROW_NOW:
                if (pc_update_state() != PCU_CHECKING) {
                    printf("[update] Check now\n");
                    pc_update_check_now();
                    waiting = 1;
                    call_SoundReqID_arg_0(0x65);
                }
                break;
            default:
                close = 1;
                break;
            }
            fflush(stdout);
        } else if (press & (PAD_BUTTON_B | 0x1000)) {
            close = 1;
        }
        if (close) {
            call_SoundReqID_arg_0(0x66);
            break;
        }
        timer++;
    }
    swallow_presses();
    if (!captured)
        pc_frame_restore(1.0f);
    printf("[update] Updates screen closed\n");
    fflush(stdout);
}

/* video_finish_frame: a request from a game screen, run modally */
int pc_update_ui_run_requested(void)
{
    switch (pc_update_ui_take_request()) {
    case 1:
        if (pc_update_state() == PCU_AVAILABLE) {
            pc_update_notes_run(0);
            return 1;
        }
        return 0;
    case 2:
        pc_update_screen_run(0);
        return 1;
    }
    return 0;
}
