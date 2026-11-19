/* PC port: party games whose REL is not decompiled yet.
 *
 * Each party game is its own REL (mini_mode.c loads it); the port links the
 * decompiled ones into the exe (dvd_shim.c s_linked_rels).  The others can't
 * run, so sel_ngc's Party Game Select dims their icons with a "not yet" tag,
 * explains it in the description box and buzzes instead of starting them
 * (decomp sel_ngc.c / sel_ngc_sprite.c, PC-only).  Availability is asked
 * from dvd_shim.c, so a game's mark goes away once its REL is linked. */
#include <stddef.h>
#include "pc_text.h"

int pc_rel_is_linked(const char *name);   /* dvd_shim.c */

/* modeCtrl.gameType of each party game -> its REL (mini_mode.c's table,
 * through the sub mode sel_ngc's menu_start_* functions request) */
static const struct { int gameType; const char *rel; } s_party_rels[] = {
    { 3,    "mkb2.race2.rel" },
    { 4,    "mkb2.mini_fight2.rel" },
    { 5,    "mkb2.pilot2.rel" },
    { 6,    "mkb2.rel_mini_billiards.rel" },
    { 7,    "mkb2.rel_mini_bowling.rel" },
    { 8,    "mkb2.golf2.rel" },
    { 9,    "mkb2.shooting.rel" },
    { 0xA,  "mkb2.boat.rel" },
    { 0xC,  "mkb2.rel_mini_futsal.rel" },
    { 0xD,  "mkb2.dogfight.rel" },
    { 0xF,  "mkb2.baseball.rel" },
    { 0x10, "mkb2.tennis.rel" },
};

int pc_party_game_available(int gameType)
{
    size_t i;

    for (i = 0; i < sizeof(s_party_rels) / sizeof(s_party_rels[0]); i++)
        if (s_party_rels[i].gameType == gameType)
            return pc_rel_is_linked(s_party_rels[i].rel);
    return 1;   /* not a party game */
}

/* ---- strings (UTF-8, one per game locale: en de fr es it ja).  The menus'
 * own term "Play Points" stays English in the European languages, as on the
 * US disc's sel_ngc text. */
static const char *const s_desc[6] = {
    "Not available in the PC version yet.\n"
    "(This game hasn't been decompiled yet.)",
    "In der PC-Version noch nicht spielbar.\n"
    "(Dieses Spiel ist noch nicht dekompiliert.)",
    "Pas encore disponible dans la version PC.\n"
    "(Ce jeu n'a pas encore été décompilé.)",
    "Aún no está disponible en la versión PC.\n"
    "(Este juego aún no se ha descompilado.)",
    "Non ancora disponibile nella versione PC.\n"
    "(Questo gioco non è ancora stato decompilato.)",
    "この ゲームは PCばんでは まだ あそべません。\n"
    "（まだ デコンパイル されていません）",
};
/* the same plus a line on unlocking: sel_ngc prints it with the price (%d) */
static const char *const s_desc_open[6] = {
    "Not available in the PC version yet.\n"
    "(This game hasn't been decompiled yet.)\n"
    "You can still open it: /bcff8000/%d/bcffff00/ Play Points.",
    "In der PC-Version noch nicht spielbar.\n"
    "(Dieses Spiel ist noch nicht dekompiliert.)\n"
    "Freischalten geht trotzdem: /bcff8000/%d/bcffff00/ Play Points.",
    "Pas encore disponible dans la version PC.\n"
    "(Ce jeu n'a pas encore été décompilé.)\n"
    "Déblocable quand même : /bcff8000/%d/bcffff00/ Play Points.",
    "Aún no está disponible en la versión PC.\n"
    "(Este juego aún no se ha descompilado.)\n"
    "Puedes desbloquearlo igual: /bcff8000/%d/bcffff00/ Play Points.",
    "Non ancora disponibile nella versione PC.\n"
    "(Questo gioco non è ancora stato decompilato.)\n"
    "Puoi sbloccarlo comunque: /bcff8000/%d/bcffff00/ Play Points.",
    "この ゲームは PCばんでは まだ あそべません。\n"
    "（まだ デコンパイル されていません）\n"
    "/bcff8000/%d/bcffff00/プレイポイントで ゲットは できます。",
};
static const char *const s_tag[6] = {
    "NOT YET", "NOCH NICHT", "PAS ENCORE", "AÚN NO", "NON ANCORA", "じゅんびちゅう",
};

/* description box text (game encoding); canOpen: still locked and enough
 * Play Points to open it */
const char *pc_party_unavail_desc(int canOpen)
{
    return pc_text_pick(canOpen ? s_desc_open : s_desc);
}

/* the short tag drawn on the dimmed icon (game encoding) */
const char *pc_party_unavail_tag(void)
{
    return pc_text_pick(s_tag);
}
