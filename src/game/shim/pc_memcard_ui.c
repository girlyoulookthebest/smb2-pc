/* In-game Memory Card screen: import a GameCube save (.gci or a whole card
 * image .raw, pc_gci.c) or export the game's saves as .gci, from the PC
 * settings (Options > Game Screen "Memory card" row, F1 overlay).  Drawn and
 * driven like the Updates screen (pc_update_ui.c).
 *
 * The game reads the card at boot and keeps its data in memory, so an import
 * is not swapped in under the running game: after it the card reads as
 * removed (pc_card_freeze) and the game quits; the next start loads the
 * imported save.  Import is offered only on the title / menus (not during
 * play) and only while no memory card job runs.
 *
 * Also shows the notice for saves imported from memcard/import/ at startup. */
#include "gc_intrinsics.h"

struct StickPoint;
struct MemcardRequest;
struct TextBox;
#include "sel_ngc.h"

#include "pc_settings.h"
#include "pc_quit.h"
#include "pc_text.h"
#include "pc_card.h"
#include "pc_memcard_ui.h"

void fn_3_140CC(int counter);                    /* sel_ngc_sprite.c */
void C_MTXOrtho(Mtx44 m, float t, float b, float l, float r, float n, float f);
extern s32 g_smth_with_sprite_draw_reqs_widescreen; /* bitmap.c */
float textdraw_get_pixel_width_of_string(char *str);
void textdraw_print_and_fit_to_width(float maxWidth, char *str);
int pc_text_is_lead(unsigned char c);            /* textdraw.c */
extern s32 main_mode;                            /* mode.c */
extern int pc_memcard_io_busy(void);             /* memcard.c (PC-only) */
extern int pc_memcard_job_state(int ch);

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
/* pc_file_dialog.c */
int pc_file_dialog_open_save(const char *title, char *out, size_t outsz, int *bad_path);
int pc_file_dialog_save_gci(const char *title, const char *default_name, char *out, size_t outsz,
                            int *bad_path);
int pc_open_folder(const char *path);

#define MD_ADV 0
#define MD_SEL 1

typedef const char *const Tr[6];

/* ---------------------------------------------------------------- strings */
static Tr s_title = { "Memory Card", "Memory Card", "Carte mémoire", "Tarjeta de memoria", "Memory card",
                      "メモリーカード" };
static Tr s_folder = { "Save folder:", "Speicherordner:", "Dossier des sauvegardes :", "Carpeta de partidas:",
                       "Cartella salvataggi:", "セーブフォルダ:" };
static Tr s_on_card = { "Saves on the card:", "Spielstände auf der Karte:", "Sauvegardes sur la carte :",
                        "Partidas en la tarjeta:", "Salvataggi sulla scheda:", "カードの セーブ:" };
static Tr s_no_save = { "No Super Monkey Ball 2 save yet.", "Noch kein Super Monkey Ball 2-Spielstand.",
                        "Pas encore de sauvegarde Super Monkey Ball 2.", "Aún no hay partida de Super Monkey Ball 2.",
                        "Ancora nessun salvataggio di Super Monkey Ball 2.", "まだ セーブが ありません。" };
static Tr s_blocks = { "%u blocks", "%u Blöcke", "%u blocs", "%u bloques", "%u blocchi", "%uブロック" };
enum { ROW_IMPORT, ROW_EXPORT, ROW_FOLDER, ROW_BACK, NROWS };
static Tr s_rows[NROWS] = {
    { "Import a save...", "Spielstand importieren...", "Importer une sauvegarde...", "Importar partida...",
      "Importa salvataggio...", "セーブを よみこむ..." },
    { "Export saves...", "Spielstände exportieren...", "Exporter les sauvegardes...", "Exportar partidas...",
      "Esporta salvataggi...", "セーブを かきだす..." },
    { "Open Save Folder", "Speicherordner öffnen", "Ouvrir le dossier", "Abrir carpeta", "Apri cartella",
      "フォルダを ひらく" },
    { "Back", "Zurück", "Retour", "Volver", "Indietro", "もどる" },
};
static Tr s_descs[NROWS] = {
    { "From Dolphin or a GameCube memory card: a .gci file, or a whole card image (.raw).",
      "Aus Dolphin oder von einer GameCube-Karte: eine .gci-Datei oder ein Kartenabbild (.raw).",
      "De Dolphin ou d'une carte GameCube : un fichier .gci ou une image de carte (.raw).",
      "De Dolphin o de una tarjeta de GameCube: un archivo .gci o una imagen de tarjeta (.raw).",
      "Da Dolphin o da una scheda GameCube: un file .gci o un'immagine della scheda (.raw).",
      "Dolphinや メモリーカードの .gciファイル、または カードの イメージ(.raw)。" },
    { "Write your saves as .gci files, for Dolphin or a real memory card (GCMM).",
      "Spielstände als .gci-Dateien speichern, für Dolphin oder eine echte Karte (GCMM).",
      "Enregistre vos sauvegardes en .gci, pour Dolphin ou une vraie carte (GCMM).",
      "Guarda tus partidas como .gci, para Dolphin o una tarjeta real (GCMM).",
      "Salva i salvataggi come file .gci, per Dolphin o una scheda vera (GCMM).",
      "セーブを .gciファイルに かきだします（Dolphin、ほんものの カード: GCMM）。" },
    { "Saves put in its import folder are imported at the next start.",
      "Spielstände im Ordner import werden beim nächsten Start importiert.",
      "Les sauvegardes du dossier import sont importées au prochain démarrage.",
      "Las partidas de la carpeta import se importan al siguiente inicio.",
      "I salvataggi nella cartella import vengono importati al prossimo avvio.",
      "importフォルダの セーブは つぎの きどうで よみこまれます。" },
    { "Back to the previous screen.", "Zurück zum vorigen Bildschirm.", "Revenir à l'écran précédent.",
      "Volver a la pantalla anterior.", "Torna alla schermata precedente.", "まえの がめんに もどります。" },
};
static Tr s_hint = { "Up/Down: select   A: choose   B: back", "Hoch/Runter: wählen   A: OK   B: zurück",
                     "Haut/Bas : choisir   A : valider   B : retour", "Arriba/Abajo: elegir   A: aceptar   B: volver",
                     "Su/Giù: scegli   A: conferma   B: indietro", "うえ/した:えらぶ   A:けってい   B:もどる" };
static Tr s_yes = { "Yes", "Ja", "Oui", "Sí", "Sì", "はい" };
static Tr s_no = { "No", "Nein", "Non", "No", "No", "いいえ" };
static Tr s_ok = { "OK", "OK", "OK", "OK", "OK", "OK" };

static Tr s_busy = { "The memory card is in use. Try again in a moment.",
                     "Die Memory Card wird gerade benutzt. Gleich noch einmal versuchen.",
                     "La carte mémoire est utilisée. Réessayez dans un instant.",
                     "La tarjeta de memoria está en uso. Inténtalo de nuevo enseguida.",
                     "La memory card è in uso. Riprova tra poco.",
                     "メモリーカードを つかっています。すこし まってから もういちど。" };
static Tr s_not_now = { "Import a save from the title screen or the menus, not during a game.",
                        "Spielstände im Titelbild oder in den Menüs importieren, nicht während eines Spiels.",
                        "Importez depuis l'écran titre ou les menus, pas pendant une partie.",
                        "Importa desde la pantalla de título o los menús, no durante una partida.",
                        "Importa dalla schermata del titolo o dai menu, non durante una partita.",
                        "セーブの よみこみは タイトルか メニューで おこなってください。" };
static Tr s_dialog_failed = { "The file dialog could not be opened.", "Der Dateidialog ließ sich nicht öffnen.",
                              "La boîte de dialogue n'a pas pu s'ouvrir.", "No se pudo abrir el diálogo de archivos.",
                              "Impossibile aprire la finestra dei file.", "ファイルの ダイアログを ひらけません。" };
static Tr s_bad_path = { "The game can't open paths with characters outside the system's language settings. "
                         "Move or rename the file and try again.",
                         "Das Spiel kann Pfade mit Zeichen außerhalb der Systemsprache nicht öffnen. "
                         "Datei verschieben oder umbenennen und erneut versuchen.",
                         "Le jeu ne peut pas ouvrir ce chemin (caractères hors de la langue du système). "
                         "Déplacez ou renommez le fichier.",
                         "El juego no puede abrir rutas con caracteres ajenos al idioma del sistema. "
                         "Mueve o renombra el archivo.",
                         "Il gioco non può aprire percorsi con caratteri estranei alla lingua del sistema. "
                         "Sposta o rinomina il file.",
                         "この パスは ひらけません。ファイルを うごかすか なまえを かえてください。" };
static Tr s_dlg_import = { "Import a GameCube save (.gci) or memory card image (.raw)",
                           "GameCube-Spielstand (.gci) oder Kartenabbild (.raw) importieren",
                           "Importer une sauvegarde GameCube (.gci) ou une image de carte (.raw)",
                           "Importar partida de GameCube (.gci) o imagen de tarjeta (.raw)",
                           "Importa salvataggio GameCube (.gci) o immagine della scheda (.raw)",
                           "Import a GameCube save (.gci) or memory card image (.raw)" };
static Tr s_dlg_export = { "Export the Super Monkey Ball 2 save as .gci",
                           "Super Monkey Ball 2-Spielstand als .gci exportieren",
                           "Exporter la sauvegarde Super Monkey Ball 2 en .gci",
                           "Exportar la partida de Super Monkey Ball 2 como .gci",
                           "Esporta il salvataggio di Super Monkey Ball 2 come .gci",
                           "Export the Super Monkey Ball 2 save as .gci" };
static Tr s_ask = { "Import from %s?", "Aus %s importieren?", "Importer depuis %s ?", "¿Importar desde %s?",
                    "Importare da %s?", "%s から よみこみますか？" };
static Tr s_f_new = { "%s: new save", "%s: neuer Spielstand", "%s : nouvelle sauvegarde", "%s: partida nueva",
                      "%s: nuovo salvataggio", "%s: あたらしい セーブ" };
static Tr s_f_replace = { "%s: replaces your save (a backup copy is kept)",
                          "%s: ersetzt deinen Spielstand (eine Sicherung bleibt)",
                          "%s : remplace votre sauvegarde (une copie est gardée)",
                          "%s: sustituye tu partida (se guarda una copia)",
                          "%s: sostituisce il tuo salvataggio (resta una copia)",
                          "%s: いまの セーブと いれかえ（コピーは のこります）" };
static Tr s_f_skip = { "%s: another game, skipped", "%s: anderes Spiel, übersprungen",
                       "%s : autre jeu, ignoré", "%s: otro juego, omitido", "%s: altro gioco, saltato",
                       "%s: ほかの ゲーム（スキップ）" };
static Tr s_f_replay = { "%s: replay, not converted", "%s: Wiederholung, nicht umgewandelt",
                         "%s : replay, non converti", "%s: repetición, no convertida",
                         "%s: replay, non convertito", "%s: リプレイ（へんかん しません）" };
static Tr s_f_fail = { "%s: not imported", "%s: nicht importiert", "%s : non importé", "%s: no importado",
                       "%s: non importato", "%s: よみこめません" };
static Tr s_f_done = { "%s: imported", "%s: importiert", "%s : importé", "%s: importado", "%s: importato",
                       "%s: よみこみました" };
static Tr s_f_done_rep = { "%s: imported (the old save was backed up)",
                           "%s: importiert (der alte Spielstand wurde gesichert)",
                           "%s : importé (l'ancienne sauvegarde est copiée)",
                           "%s: importado (se copió la partida anterior)",
                           "%s: importato (il vecchio salvataggio è copiato)",
                           "%s: よみこみました（まえの セーブは コピー）" };
static Tr s_f_exported = { "%s: exported", "%s: exportiert", "%s : exporté", "%s: exportado", "%s: esportato",
                           "%s: かきだしました" };
static Tr s_restart_ask = { "The game closes after the import. Start it again to play with the imported save.",
                            "Das Spiel wird nach dem Import beendet. Danach neu starten.",
                            "Le jeu se ferme après l'import. Relancez-le pour jouer avec la sauvegarde.",
                            "El juego se cierra tras importar. Vuelve a iniciarlo para usar la partida.",
                            "Il gioco si chiude dopo l'importazione. Riavvialo per usare il salvataggio.",
                            "よみこんだあと ゲームを おわります。もういちど きどうしてください。" };
static Tr s_imported = { "Save imported.", "Spielstand importiert.", "Sauvegarde importée.", "Partida importada.",
                         "Salvataggio importato.", "セーブを よみこみました。" };
static Tr s_quit_now = { "The game will now close. Start it again to play with the imported save.",
                         "Das Spiel wird jetzt beendet. Neu starten, um mit dem Spielstand zu spielen.",
                         "Le jeu va se fermer. Relancez-le pour jouer avec la sauvegarde importée.",
                         "El juego se cerrará. Vuelve a iniciarlo para jugar con la partida importada.",
                         "Il gioco ora si chiude. Riavvialo per giocare col salvataggio importato.",
                         "ゲームを おわります。もういちど きどうしてください。" };
static Tr s_import_failed = { "Not imported.", "Nicht importiert.", "Rien n'a été importé.", "No se importó.",
                              "Non importato.", "よみこめませんでした。" };
static Tr s_exported = { "Exported to:", "Exportiert nach:", "Exporté dans :", "Exportado a:", "Esportato in:",
                         "かきだしました:" };
static Tr s_export_failed = { "Not exported.", "Nicht exportiert.", "Rien n'a été exporté.", "No se exportó.",
                              "Non esportato.", "かきだせませんでした。" };
static Tr s_notice_title = { "Saves from the import folder", "Spielstände aus dem Ordner import",
                             "Sauvegardes du dossier import", "Partidas de la carpeta import",
                             "Salvataggi dalla cartella import", "importフォルダの セーブ" };
static Tr s_notice_moved = { "The files were moved to import\\done (failed ones to import\\failed).",
                             "Die Dateien liegen jetzt in import\\done (fehlgeschlagene in import\\failed).",
                             "Les fichiers sont dans import\\done (les échecs dans import\\failed).",
                             "Los archivos están en import\\done (los fallidos en import\\failed).",
                             "I file sono in import\\done (quelli non riusciti in import\\failed).",
                             "ファイルは import\\done（しっぱいは import\\failed）に うつしました。" };

/* PCG_E_* */
static Tr s_err[] = {
    { "", "", "", "", "", "" },
    { "The file can't be read.", "Die Datei lässt sich nicht lesen.", "Le fichier est illisible.",
      "No se puede leer el archivo.", "Impossibile leggere il file.", "ファイルを よめません。" },
    { "This is not a .gci save or a GameCube memory card image.",
      "Das ist weder ein .gci-Spielstand noch ein GameCube-Kartenabbild.",
      "Ce n'est ni une sauvegarde .gci ni une image de carte GameCube.",
      "No es una partida .gci ni una imagen de tarjeta de GameCube.",
      "Non è un salvataggio .gci né un'immagine di scheda GameCube.",
      ".gciファイルでも カードの イメージでも ありません。" },
    { "The .gci file is damaged (its size doesn't match its header).",
      "Die .gci-Datei ist beschädigt (Größe passt nicht zum Kopf).",
      "Le fichier .gci est abîmé (sa taille ne correspond pas à son en-tête).",
      "El archivo .gci está dañado (su tamaño no coincide con la cabecera).",
      "Il file .gci è danneggiato (la dimensione non corrisponde all'intestazione).",
      ".gciファイルが こわれています。" },
    { "This save is for another game, not Super Monkey Ball 2.",
      "Dieser Spielstand gehört zu einem anderen Spiel.",
      "Cette sauvegarde est celle d'un autre jeu.", "Esta partida es de otro juego.",
      "Questo salvataggio è di un altro gioco.", "ほかの ゲームの セーブです。" },
    { "This save is from the European or Japanese Super Monkey Ball 2; this port runs the USA version.",
      "Dieser Spielstand stammt aus der europäischen oder japanischen Version; der Port nutzt die US-Version.",
      "Cette sauvegarde vient de la version européenne ou japonaise ; ce portage utilise la version USA.",
      "Esta partida es de la versión europea o japonesa; este port usa la versión de EE. UU.",
      "Questo salvataggio è della versione europea o giapponese; il port usa la versione USA.",
      "ヨーロッパばんか にほんばんの セーブです。この ポートは アメリカばん です。" },
    { "The memory card image is damaged (header).", "Das Kartenabbild ist beschädigt (Kopf).",
      "L'image de carte est abîmée (en-tête).", "La imagen de tarjeta está dañada (cabecera).",
      "L'immagine della scheda è danneggiata (intestazione).", "カードの イメージが こわれています。" },
    { "The memory card image is damaged (file directory).", "Das Kartenabbild ist beschädigt (Verzeichnis).",
      "L'image de carte est abîmée (répertoire).", "La imagen de tarjeta está dañada (directorio).",
      "L'immagine della scheda è danneggiata (directory).", "カードの イメージが こわれています。" },
    { "The memory card image is damaged (block table).", "Das Kartenabbild ist beschädigt (Blocktabelle).",
      "L'image de carte est abîmée (table des blocs).", "La imagen de tarjeta está dañada (tabla de bloques).",
      "L'immagine della scheda è danneggiata (tabella dei blocchi).", "カードの イメージが こわれています。" },
    { "This memory card has no Super Monkey Ball 2 (USA) save.",
      "Auf dieser Karte ist kein Super Monkey Ball 2-Spielstand (USA).",
      "Cette carte n'a pas de sauvegarde Super Monkey Ball 2 (USA).",
      "Esta tarjeta no tiene partida de Super Monkey Ball 2 (EE. UU.).",
      "Questa scheda non ha salvataggi di Super Monkey Ball 2 (USA).",
      "この カードには Super Monkey Ball 2の セーブが ありません。" },
    { "The memory card is full.", "Die Memory Card ist voll.", "La carte mémoire est pleine.",
      "La tarjeta de memoria está llena.", "La memory card è piena.", "メモリーカードが いっぱいです。" },
    { "The save could not be written.", "Der Spielstand ließ sich nicht schreiben.",
      "La sauvegarde n'a pas pu être écrite.", "No se pudo escribir la partida.",
      "Impossibile scrivere il salvataggio.", "セーブを かけませんでした。" },
    { "There is no Super Monkey Ball 2 save to export yet.", "Es gibt noch keinen Spielstand zum Exportieren.",
      "Il n'y a pas encore de sauvegarde à exporter.", "Aún no hay partida que exportar.",
      "Non c'è ancora un salvataggio da esportare.", "かきだす セーブが まだ ありません。" },
    { "The .gci file could not be written.", "Die .gci-Datei ließ sich nicht schreiben.",
      "Le fichier .gci n'a pas pu être écrit.", "No se pudo escribir el archivo .gci.",
      "Impossibile scrivere il file .gci.", ".gciファイルを かけませんでした。" },
    { "Only the game data save can be converted, not replays.",
      "Nur der Spielstand lässt sich umwandeln, keine Wiederholungen.",
      "Seule la sauvegarde du jeu peut être convertie, pas les replays.",
      "Solo se puede convertir la partida, no las repeticiones.",
      "Si può convertire solo il salvataggio, non i replay.",
      "へんかん できるのは ゲームデータだけです（リプレイは できません）。" },
    { "The save is damaged (its checksum doesn't match).",
      "Der Spielstand ist beschädigt (Prüfsumme falsch).",
      "La sauvegarde est abîmée (somme de contrôle fausse).",
      "La partida está dañada (suma de comprobación incorrecta).",
      "Il salvataggio è danneggiato (checksum errato).",
      "セーブが こわれています。" },
};
#define N_ERR ((int)(sizeof s_err / sizeof s_err[0]))

extern int locale;   /* the game's language (main.c sets it at boot) */

/* the UTF-8 source string of the current language (for system dialogs) */
static const char *utf8_pick(Tr t)
{
    return t[(locale >= 0 && locale < 6) ? locale : 0];
}

static const char *err_text(int e)
{
    return pc_text_pick(s_err[(e > 0 && e < N_ERR) ? e : PCG_E_WRITE]);
}

/* ---------------------------------------------------------------- requests */
static volatile int s_req;   /* 1 screen, 2 notice */
static int s_notice_shown;
void pc_memcard_screen_request(void) { s_req = 1; }
void pc_memcard_notice_request(void) { if (!s_req) s_req = 2; }
int pc_memcard_notice_pending(void) { return !s_notice_shown && pc_gci_startup_report() != NULL; }

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

/* '/' starts the game's text control codes: printed and measured one by one */
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

/* left-aligned text may contain '/' (printed piecewise); centred text is
 * printed whole, as piecewise prints would each be centred */
static void text(float x, float y, float sx, float sy, u32 color, int align, const char *s)
{
    font(sx, sy, color, align);
    textdraw_set_pos(x, y);
    if (align == 0)
        safe_print(s);
    else
        textdraw_print((char *)s);
}

/* centred, shrunk horizontally to fit w */
static void text_fit(float x, float y, float sx, float sy, u32 color, float w, const char *s)
{
    float tw;
    font(sx, sy, color, 3);
    tw = safe_width(s);
    if (tw > w && tw > 0.0f)
        textdraw_set_scale(sx * w / tw, sy);
    textdraw_set_pos(x, y);
    textdraw_print((char *)s);   /* centred: callers pass text without '/' */
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

static void wood_box(float cx, float cy, float w, float h, float depth)
{
    float bw, bh;
    init_ui_element_sprite_with_defaults();
    set_ui_element_sprite_depth(depth);
    set_ui_element_sprite_pos(cx, cy);
    bw = (float)g_get_bmp_width(5);
    bh = (float)g_get_bmp_height(5);
    if (bw > 0.0f && bh > 0.0f)
        set_ui_element_sprite_scale(w / bw, h / bh);
    draw_ui_box(5);
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

/* ---------------------------------------------------------------- message box */
#define BOX_LINES 14
#define BOX_W 500.0f
#define BOX_TEXT_W 460.0f
#define BOX_SX 0.62f
#define BOX_SY 0.7f
#define BOX_PITCH 19.0f

static struct {
    int open;
    int confirm;        /* Yes/No, else OK */
    int sel;            /* confirm: 0 Yes, 1 No */
    int nlines;
    char lines[BOX_LINES][160];
    u32 color[BOX_LINES];
} s_box;

static void box_clear(void)
{
    memset(&s_box, 0, sizeof s_box);
}

/* ASCII-only copy of a path for drawing (the font has no other code page) */
static void path_for_display(const char *p, char *out, size_t outsz)
{
    size_t i;
    for (i = 0; p[i] && i + 1 < outsz; i++) {
        unsigned char c = (unsigned char)p[i];
        out[i] = (c < 0x20 || c >= 0x7F) ? '?' : (c == '/' ? '\\' : (char)c);
    }
    out[i] = 0;
}

/* word-wrapped into the box (game-encoded text) */
static void box_add(const char *s, u32 color)
{
    font(BOX_SX, BOX_SY, 0xFFFFFFU, 0);
    while (*s && s_box.nlines < BOX_LINES) {
        char tmp[160];
        const char *p = s, *last_space = NULL;
        size_t len = 0;
        while (*p) {
            size_t cl = (pc_text_is_lead((unsigned char)*p) && p[1]) ? 2 : 1;
            if (len + cl >= sizeof tmp) break;
            memcpy(tmp + len, p, cl);
            tmp[len + cl] = 0;
            if (safe_width(tmp) > BOX_TEXT_W && len > 0)
                break;
            if (*p == ' ') last_space = p;
            len += cl;
            p += cl;
        }
        if (*p && last_space && last_space > s)
            p = last_space;
        memcpy(s_box.lines[s_box.nlines], s, (size_t)(p - s));
        s_box.lines[s_box.nlines][p - s] = 0;
        s_box.color[s_box.nlines] = color;
        s_box.nlines++;
        s = p;
        while (*s == ' ') s++;
    }
}

static void box_add_fmt(Tr fmt, const char *arg, u32 color)
{
    char f[300], out[400];
    snprintf(f, sizeof f, "%s", pc_text_pick(fmt));   /* game-encoded format */
    snprintf(out, sizeof out, f, arg);
    box_add(out, color);
}

static void box_open(int confirm)
{
    s_box.open = 1;
    s_box.confirm = confirm;
    s_box.sel = 1;   /* default No */
}

static void box_draw(int timer)
{
    float h = 60.0f + BOX_PITCH * (float)s_box.nlines + 30.0f;
    float top = 240.0f - h * 0.5f;
    float y = top + 26.0f;
    int i;

    wood_box(320.0f, 240.0f, BOX_W, h, 0.05f);
    for (i = 0; i < s_box.nlines; i++) {
        font(BOX_SX, BOX_SY, s_box.color[i], 0);
        textdraw_set_pos(320.0f - BOX_TEXT_W * 0.5f, y);
        safe_print(s_box.lines[i]);
        y += BOX_PITCH;
    }
    y += 14.0f;
    if (s_box.confirm) {
        int k;
        for (k = 0; k < 2; k++) {
            float x = k == 0 ? 250.0f : 390.0f;
            int sel = s_box.sel == k;
            font(0.8f, 0.9f, sel ? 0xFFFF40U : 0xFFFFFFU, 3);
            if (sel)
                textdraw_set_add_color(glow(timer));
            textdraw_set_pos(x, y);
            textdraw_print((char *)pc_text_pick(k == 0 ? s_yes : s_no));
        }
    } else {
        font(0.8f, 0.9f, 0xFFFF40U, 3);
        textdraw_set_add_color(glow(timer));
        textdraw_set_pos(320.0f, y);
        textdraw_print((char *)pc_text_pick(s_ok));
    }
}

/* 1 = closed with OK/Yes, 0 = closed with No/B, -1 = still open */
static int box_input(u16 press)
{
    if (s_box.confirm && (press & (PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT))) {
        s_box.sel ^= 1;
        call_SoundReqID_arg_0(0x6C);
    }
    if (press & PAD_BUTTON_A) {
        int yes = !s_box.confirm || s_box.sel == 0;
        call_SoundReqID_arg_0(yes ? 0x65 : 0x66);
        s_box.open = 0;
        return yes;
    }
    if (press & (PAD_BUTTON_B | 0x1000)) {
        call_SoundReqID_arg_0(0x66);
        s_box.open = 0;
        return s_box.confirm ? 0 : 1;
    }
    return -1;
}

/* ---------------------------------------------------------------- screen */
#define MAX_SHOWN 4
static PcCardFile s_files[PC_CARD_MAX_FILES];
static int s_nfiles;

static void refresh_files(void)
{
    PcCardFile all[PC_CARD_MAX_FILES];
    int n = pc_card_list(all, PC_CARD_MAX_FILES), i;
    s_nfiles = 0;
    for (i = 0; i < n; i++)
        if (!memcmp(all[i].gameName, "GM2E", 4) && !memcmp(all[i].company, "8P", 2))
            s_files[s_nfiles++] = all[i];
}

/* seconds since 2000-01-01 -> "2026-11-14 13:05" */
static void card_time_str(u32 t, char *out, size_t outsz)
{
    u32 days = t / 86400, rem = t % 86400;
    int y = 2000, m = 0;
    static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    for (;;) {
        int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
        u32 yd = leap ? 366 : 365;
        if (days < yd) break;
        days -= yd;
        y++;
    }
    for (m = 0; m < 12; m++) {
        int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
        u32 md = (u32)mdays[m] + (m == 1 && leap ? 1 : 0);
        if (days < md) break;
        days -= md;
    }
    snprintf(out, outsz, "%04d-%02d-%02d %02u:%02u", y, m + 1, (int)days + 1, rem / 3600, (rem / 60) % 60);
}

static void screen_draw(int cursor, int timer, const char *status, u32 status_color)
{
    char buf[300], path[300];
    int i;
    float y;

    begin_2d();
    wood_box(320.0f, 240.0f, 540.0f, 420.0f, 0.11f);
    text(320.0f, 40.0f, 1.0f, 1.1f, 0xFFFF40U, 3, pc_text_pick(s_title));

    text(70.0f, 80.0f, 0.6f, 0.7f, 0xC0C0C0U, 0, pc_text_pick(s_folder));
    path_for_display(pc_card_base_dir(), path, sizeof path);
    text_fit(320.0f, 98.0f, 0.55f, 0.65f, 0xFFFFFFU, 490.0f, path);

    text(70.0f, 124.0f, 0.6f, 0.7f, 0xC0C0C0U, 0, pc_text_pick(s_on_card));
    y = 142.0f;
    if (s_nfiles == 0)
        text(90.0f, y, 0.6f, 0.7f, 0xFFFFFFU, 0, pc_text_pick(s_no_save));
    for (i = 0; i < s_nfiles && i < MAX_SHOWN; i++, y += 18.0f) {
        char when[32], fmt[64], blocks[64];
        snprintf(fmt, sizeof fmt, "%s", pc_text_pick(s_blocks));
        snprintf(blocks, sizeof blocks, fmt, (unsigned)(s_files[i].length / PC_CARD_BLOCK));
        card_time_str(s_files[i].time, when, sizeof when);
        text(90.0f, y, 0.6f, 0.7f, 0xFFFFFFU, 0, s_files[i].fileName);
        text(330.0f, y, 0.6f, 0.7f, 0xFFFFFFU, 0, blocks);
        text(420.0f, y, 0.6f, 0.7f, 0xA0A0A0U, 0, when);
    }

    for (i = 0; i < NROWS; i++) {
        float ry = 222.0f + 28.0f * i;
        int sel = (i == cursor);
        if (sel && !s_box.open) {
            font(0.8f, 0.9f, 0xFFFF40U, 0);
            textdraw_set_add_color(glow(timer));
            textdraw_set_pos(130.0f, ry);
            textdraw_print(">");
        }
        text(160.0f, ry, 0.8f, 0.9f, sel ? 0xFFFF40U : 0xFFFFFFU, 0, pc_text_pick(s_rows[i]));
    }

    if (status && status[0])
        text_fit(320.0f, 346.0f, 0.65f, 0.72f, status_color, 490.0f, status);
    text_fit(320.0f, 380.0f, 0.65f, 0.72f, 0xFFFF80U, 490.0f, pc_text_pick(s_descs[cursor]));
    snprintf(buf, sizeof buf, "%s", pc_text_pick(s_hint));
    text(320.0f, 420.0f, 0.6f, 0.7f, 0xC0C0C0U, 3, buf);
    if (s_box.open)
        box_draw(timer);
    end_2d();
}

static int import_allowed(void)
{
    return main_mode == MD_ADV || main_mode == MD_SEL;
}

static const char *base_name(const char *p)
{
    const char *a = strrchr(p, '\\'), *b = strrchr(p, '/');
    if (b > a) a = b;
    return a ? a + 1 : p;
}

/* the import being confirmed */
static PcGciSet *s_set;
static char s_pick[1200];

enum { PEND_NONE, PEND_IMPORT_CONFIRM, PEND_QUIT_AFTER };

static int start_import(void)
{
    PcGciReport *rep;
    int r, bad = 0, i;
    char disp[300];

    box_clear();
    if (pc_memcard_io_busy()) {
        box_add(pc_text_pick(s_busy), 0xFFFFFFU);
        box_open(0);
        return PEND_NONE;
    }
    if (!import_allowed()) {
        box_add(pc_text_pick(s_not_now), 0xFFFFFFU);
        box_open(0);
        return PEND_NONE;
    }
    r = pc_file_dialog_open_save(utf8_pick(s_dlg_import), s_pick, sizeof s_pick, &bad);
    if (r == 0)
        return PEND_NONE;
    if (r < 0) {
        box_add(pc_text_pick(bad ? s_bad_path : s_dialog_failed), 0xFF7060U);
        box_open(0);
        return PEND_NONE;
    }
    rep = (PcGciReport *)calloc(1, sizeof *rep);
    if (!s_set)
        s_set = (PcGciSet *)calloc(1, sizeof *s_set);
    if (!rep || !s_set) {
        free(rep);
        return PEND_NONE;
    }
    printf("[card-ui] import %s\n", s_pick);
    r = pc_gci_load(s_pick, s_set, rep);
    fputs(rep->text, stdout);
    path_for_display(base_name(s_pick), disp, sizeof disp);
    if (r != 0) {
        printf("[card-ui] can't import: %s\n", pc_gci_err_text(rep->err));
        box_add(pc_text_pick(s_import_failed), 0xFF7060U);
        box_add(err_text(rep->err), 0xFFFFFFU);
        box_open(0);
        free(rep);
        fflush(stdout);
        return PEND_NONE;
    }
    box_add_fmt(s_ask, disp, 0xFFFF40U);
    for (i = 0; i < s_set->count; i++)
        box_add_fmt(s_set->exists[i] ? s_f_replace : s_f_new, s_set->files[i].fileName,
                    s_set->exists[i] ? 0xFFC060U : 0xFFFFFFU);
    for (i = 0; i < rep->nentries; i++) {
        PcGciEntry *e = &rep->entries[i];
        char nm[80];
        if (e->status == PCG_F_SKIPPED && e->err == PCG_E_NOT_CONVERTIBLE) {
            box_add_fmt(s_f_replay, e->name, 0xA0A0A0U);
        } else if (e->status == PCG_F_SKIPPED) {
            snprintf(nm, sizeof nm, "%.4s%.2s %s", (char *)e->gameName, (char *)e->company, e->name);
            path_for_display(nm, nm, sizeof nm);
            box_add_fmt(s_f_skip, nm, 0xA0A0A0U);
        } else if (e->status == PCG_F_FAILED) {
            box_add_fmt(s_f_fail, e->name, 0xFF7060U);
            box_add(err_text(e->err), 0xFF7060U);
        }
    }
    box_add(pc_text_pick(s_restart_ask), 0xC0C0C0U);
    box_open(1);
    free(rep);
    fflush(stdout);
    return PEND_IMPORT_CONFIRM;
}

/* returns PEND_QUIT_AFTER when something was imported */
static int finish_import(void)
{
    PcGciReport *rep = (PcGciReport *)calloc(1, sizeof *rep);
    int n, i;

    box_clear();
    if (!rep || !s_set)
        return PEND_NONE;
    if (pc_memcard_io_busy()) {   /* a job started meanwhile (should not happen: the game is paused) */
        pc_gci_free(s_set);
        box_add(pc_text_pick(s_busy), 0xFFFFFFU);
        box_open(0);
        free(rep);
        return PEND_NONE;
    }
    n = pc_gci_store(s_set, rep);
    pc_gci_free(s_set);
    fputs(rep->text, stdout);
    printf("[card-ui] imported %d file(s), %d failed\n", n, rep->failed);
    fflush(stdout);
    if (n > 0) {
        pc_card_freeze();
        box_add(pc_text_pick(s_imported), 0x80FF80U);
    } else
        box_add(pc_text_pick(s_import_failed), 0xFF7060U);
    for (i = 0; i < rep->nentries; i++) {
        PcGciEntry *e = &rep->entries[i];
        if (e->status == PCG_F_IMPORTED) box_add_fmt(s_f_done, e->name, 0xFFFFFFU);
        else if (e->status == PCG_F_REPLACED) box_add_fmt(s_f_done_rep, e->name, 0xFFFFFFU);
        else if (e->status == PCG_F_FAILED) {
            box_add_fmt(s_f_fail, e->name, 0xFF7060U);
            box_add(err_text(e->err), 0xFF7060U);
        }
    }
    if (n > 0)
        box_add(pc_text_pick(s_quit_now), 0xFFFF40U);
    box_open(0);
    free(rep);
    return n > 0 ? PEND_QUIT_AFTER : PEND_NONE;
}

static void do_export(void)
{
    PcGciReport *rep;
    char def[80], dir[1200], disp[300];
    int r, bad = 0;
    char *slash;

    box_clear();
    if (pc_memcard_io_busy()) {
        box_add(pc_text_pick(s_busy), 0xFFFFFFU);
        box_open(0);
        return;
    }
    refresh_files();
    if (s_nfiles == 0) {
        box_add(err_text(PCG_E_NO_FILES_ON_CARD), 0xFFFFFFU);
        box_open(0);
        return;
    }
    pc_gci_default_name(&s_files[0], def, sizeof def);
    r = pc_file_dialog_save_gci(utf8_pick(s_dlg_export), def, s_pick, sizeof s_pick, &bad);
    if (r == 0)
        return;
    if (r < 0) {
        box_add(pc_text_pick(bad ? s_bad_path : s_dialog_failed), 0xFF7060U);
        box_open(0);
        return;
    }
    snprintf(dir, sizeof dir, "%s", s_pick);
    slash = strrchr(dir, '\\');
    if (!slash || (strrchr(dir, '/') && strrchr(dir, '/') > slash)) slash = strrchr(dir, '/');
    if (slash) slash[1] = 0; else dir[0] = 0;
    rep = (PcGciReport *)calloc(1, sizeof *rep);
    if (!rep)
        return;
    printf("[card-ui] export to %s\n", s_pick);
    r = pc_gci_export(dir, s_pick, rep);
    fputs(rep->text, stdout);
    fflush(stdout);
    if (r > 0 && !rep->failed) {
        int i;
        box_add(pc_text_pick(s_exported), 0x80FF80U);
        path_for_display(dir[0] ? dir : s_pick, disp, sizeof disp);
        box_add(disp, 0xFFFFFFU);
        for (i = 0; i < rep->nentries; i++)
            if (rep->entries[i].status == PCG_F_EXPORTED) {
                /* the first one under the picked name, the others next to it */
                char nm[80];
                int j;
                if (i == 0)
                    path_for_display(base_name(s_pick), nm, sizeof nm);
                else
                    for (j = 0; j < s_nfiles; j++)
                        if (!strcmp(s_files[j].fileName, rep->entries[i].name))
                            pc_gci_default_name(&s_files[j], nm, sizeof nm);
                box_add_fmt(s_f_exported, nm, 0xFFFFFFU);
            }
    } else {
        box_add(pc_text_pick(s_export_failed), 0xFF7060U);
        box_add(err_text(rep->err), 0xFFFFFFU);
    }
    box_open(0);
    free(rep);
}

void pc_memcard_screen_run(int captured)
{
    Loop l;
    int cursor = 0, timer = 0, pend = PEND_NONE;

    if (!captured && !pc_frame_capture())
        return;
    refresh_files();
    box_clear();
    printf("[card-ui] Memory Card screen open (%d save file(s); main mode %d, card job %s)\n", s_nfiles,
           (int)main_mode, pc_memcard_io_busy() ? "running" : "idle");
    fflush(stdout);
    loop_begin(&l);
    for (;;) {
        int close = 0;
        u16 press;

        pc_frame_restore(0.35f);
        screen_draw(cursor, timer, NULL, 0);
        press = loop_frame(&l, &close);

        if (s_box.open) {
            int r = box_input(press);
            if (r >= 0) {
                if (pend == PEND_IMPORT_CONFIRM) {
                    pend = r == 1 ? finish_import() : PEND_NONE;
                    if (r != 1) {
                        pc_gci_free(s_set);
                        printf("[card-ui] import cancelled\n");
                    }
                    refresh_files();
                } else if (pend == PEND_QUIT_AFTER) {
                    pc_quit_request("save imported");
                    close = 1;
                    pend = PEND_NONE;
                }
                loop_begin(&l);
            }
        } else if (press & PAD_BUTTON_UP) {
            cursor = (cursor + NROWS - 1) % NROWS;
            call_SoundReqID_arg_0(0x6C);
        } else if (press & PAD_BUTTON_DOWN) {
            cursor = (cursor + 1) % NROWS;
            call_SoundReqID_arg_0(0x6C);
        } else if (press & PAD_BUTTON_A) {
            call_SoundReqID_arg_0(0x65);
            switch (cursor) {
            case ROW_IMPORT:
                pend = start_import();
                break;
            case ROW_EXPORT:
                do_export();
                break;
            case ROW_FOLDER:
                pc_open_folder(pc_card_base_dir());
                break;
            default:
                close = 1;
                break;
            }
            fflush(stdout);
            loop_begin(&l);   /* a dialog may have eaten the release */
        } else if (press & (PAD_BUTTON_B | 0x1000)) {
            call_SoundReqID_arg_0(0x66);
            close = 1;
        }
        if (close && pend == PEND_QUIT_AFTER) {
            /* window closed on the result box: still quit */
            pc_quit_request("save imported");
        }
        if (close)
            break;
        timer++;
    }
    swallow_presses();
    if (!captured)
        pc_frame_restore(1.0f);
    printf("[card-ui] Memory Card screen closed\n");
    fflush(stdout);
}

/* ---------------------------------------------------------------- startup notice */
static void notice_run(void)
{
    const PcGciReport *r = pc_gci_startup_report();
    Loop l;
    int timer = 0, i;

    s_notice_shown = 1;
    if (!r || !pc_frame_capture())
        return;
    box_clear();
    box_add(pc_text_pick(s_notice_title), 0xFFFF40U);
    for (i = 0; i < r->nentries; i++) {
        const PcGciEntry *e = &r->entries[i];
        char nm[80];
        switch (e->status) {
        case PCG_F_IMPORTED: box_add_fmt(s_f_done, e->name, 0xFFFFFFU); break;
        case PCG_F_REPLACED: box_add_fmt(s_f_done_rep, e->name, 0xFFFFFFU); break;
        case PCG_F_SKIPPED:
            if (e->err == PCG_E_NOT_CONVERTIBLE) {
                box_add_fmt(s_f_replay, e->name, 0xA0A0A0U);
                break;
            }
            snprintf(nm, sizeof nm, "%.4s%.2s %s", (const char *)e->gameName, (const char *)e->company, e->name);
            path_for_display(nm, nm, sizeof nm);
            box_add_fmt(s_f_skip, nm, 0xA0A0A0U);
            break;
        default:
            path_for_display(e->name, nm, sizeof nm);
            box_add_fmt(s_f_fail, nm, 0xFF7060U);
            box_add(err_text(e->err), 0xFF7060U);
            break;
        }
    }
    box_add(pc_text_pick(s_notice_moved), 0xC0C0C0U);
    box_open(0);
    printf("[card-ui] import folder notice shown\n");
    fflush(stdout);
    loop_begin(&l);
    for (;;) {
        int close = 0;
        u16 press;
        pc_frame_restore(0.45f);
        begin_2d();
        box_draw(timer);
        end_2d();
        press = loop_frame(&l, &close);
        if (box_input(press) >= 0)
            close = 1;
        if (close)
            break;
        timer++;
    }
    box_clear();
    swallow_presses();
    pc_frame_restore(1.0f);
    pc_gci_startup_report_clear();
}

int pc_memcard_ui_run_requested(void)
{
    int r = s_req;
    s_req = 0;
    if (r == 1) {
        pc_memcard_screen_run(0);
        return 1;
    }
    if (r == 2) {
        notice_run();
        return 1;
    }
    return 0;
}
