/* Mixer categories of the game's sounds (docs/audio.md, "Mixer").
 *
 * The Options > Sound page sets a volume per category; the DSP worker
 * (audio_miniaudio.c) scales each voice / stream by its category.  The
 * category is decided here from the game's own sound table and passed to the
 * synth right before the voice starts:
 *
 *  - sound.c g_smth_calls_sndFXStartParaInfo calls pc_snd_fx_cue(index of the
 *    cue in lbl_1_rodata_148C) before its sndFXStartParaInfo (PC-only hook);
 *    voices a SoundMacro spawns inherit their parent's category.
 *  - smgr.c fn_1_393B0 calls pc_snd_stream_file(stream, file) before
 *    sndStreamActivate: snd/stream/ = music, snd/stream_se/ = sound effect
 *    (ambiences, story cutscene sound tracks).
 *  - Anything else (baseball's own sndFXStartEx calls, ...) is a sound effect.
 *
 * lbl_1_rodata_148C (0x5B8 entries, include/sound_tables.inc) lists the cues
 * group by group (GRPse01, GRPnar, GRPboy, ..., a 0x2000-flagged marker row
 * starts each group) with the sound designers' names, which say what each
 * one is:
 *
 *  Timer:  SND_SED_NAR_N4_ES (index 166, group nar, fx 0xA4, cue 6): the
 *          stage timer's tick.  game_mode.c smd_game_play_tick plays it every
 *          second (at -5 above 10 s left, plus twice more per second in the
 *          last 10 / 5 s), bowling's 5-4-3-2-1 countdown plays the same cue.
 *  Voice:  SND_SED_VO*            the monkeys (boy/girl/baby groups)
 *          SND_SED_NAR_*          the announcer (incl. the 0-9 count and
 *                                 HURRY), except the few effects filed in
 *                                 the nar group (falls, hammer, rain, ...)
 *          SFXgoli_*              GonGon's voice set (group goli)
 *          GRPs1a_set             story cutscene voices (aiai, baby, meemee,
 *                                 gongon, vm..)
 *          SFXna_* / SFXna2_*     party game announcer clips
 *          flags & 0x70 == 0x40   the "announcer" flag the party game
 *                                 voice lines carry (0x4040 / 0x4840); 0x4050
 *                                 and 0x4060 are stereo jingles, not voices
 *          <name>_1, <name>_boat  alternates of a voice line (bowling's
 *                                 second takes, boat's copies)
 *  SFX:    everything else.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gc_intrinsics.h"
#include "sound.h"
#include "audio_dsp.h"

#define SOUND_TABLE_COUNT 0x5B8

void pc_snd_set_next_category(int cat);           /* musyx_shim.c */
void pc_snd_stream_category(u32 id, int cat);     /* musyx_shim.c */

static signed char s_cat[SOUND_TABLE_COUNT];
static int s_ready;

/* name without the MusyX export suffix "_basic_macro" / "_bm" (it comes
 * before the "_1" of a second take: "SFXspare_basic_macro_1") */
static void base_name(const char *in, char *out, size_t n)
{
    static const char *const sfx[] = { "_basic_macro", "_bm" };
    size_t k;
    snprintf(out, n, "%s", in ? in : "");
    for (k = 0; k < 2; k++) {
        char *p = strstr(out, sfx[k]);
        if (p != NULL) {
            memmove(p, p + strlen(sfx[k]), strlen(p + strlen(sfx[k])) + 1);
            break;
        }
    }
}

static int starts(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

static int classify_direct(const char *group, const char *name, u16 flags)
{
    static const char *const narSfx[] = {
        "FALL00", "FALL01", "FALL02", "OP_TRN", "RST5", "HAMMER1", "HAMMER2",
        "RAIN1", "RAIN1_OFF", "RAIN2", "RAIN2_OFF",
    };
    size_t i;

    if (!strcmp(name, "SND_SED_NAR_N4_ES"))
        return ADSP_CAT_TIMER;
    if (starts(name, "SND_SED_VO"))
        return ADSP_CAT_VOICE;
    if (starts(name, "SND_SED_NAR_")) {
        for (i = 0; i < sizeof(narSfx) / sizeof(narSfx[0]); i++)
            if (!strcmp(name + 12, narSfx[i]))
                return ADSP_CAT_SFX;
        return ADSP_CAT_VOICE;
    }
    if (starts(name, "SFXgoli_") || !strcmp(group, "GRPs1a_set"))
        return ADSP_CAT_VOICE;
    if (starts(name, "SFXna_") || starts(name, "SFXna2_"))
        return ADSP_CAT_VOICE;
    if ((flags & 0x70) == 0x40)
        return ADSP_CAT_VOICE;
    return -1;   /* undecided */
}

static void build(void)
{
    static char names[SOUND_TABLE_COUNT][64];
    static char groups[SOUND_TABLE_COUNT][24];
    char group[24] = "";
    int i, j, nv = 0, nt = 0;

    for (i = 0; i < SOUND_TABLE_COUNT; i++) {
        const struct SoundEffectDesc *d = &lbl_1_rodata_148C[i];
        const char *nm = d->name ? d->name : "";
        if ((d->flags & 0x2000) && starts(nm, "GRP"))
            snprintf(group, sizeof group, "%s", nm);
        base_name(nm, names[i], sizeof names[i]);
        snprintf(groups[i], sizeof groups[i], "%s", group);
        s_cat[i] = (signed char)classify_direct(group, nm, d->flags);
    }
    /* alternates: "<voice line>_1", "<voice line>_boat" */
    for (i = 0; i < SOUND_TABLE_COUNT; i++) {
        char base[64];
        size_t len;
        int found = -1;
        if (s_cat[i] >= 0)
            continue;
        snprintf(base, sizeof base, "%s", names[i]);
        len = strlen(base);
        if (len > 2 && !strcmp(base + len - 2, "_1"))
            base[len - 2] = 0;
        else if (len > 5 && !strcmp(base + len - 5, "_boat"))
            base[len - 5] = 0;
        else
            continue;
        len = strlen(base);
        for (j = 0; j < SOUND_TABLE_COUNT && found < 0; j++)
            if (j != i && !strcmp(names[j], base) && s_cat[j] >= 0)
                found = s_cat[j];
        if (found < 0)   /* "final_lap_boat" -> "final_lap_01" */
            for (j = 0; j < SOUND_TABLE_COUNT && found < 0; j++)
                if (s_cat[j] == ADSP_CAT_VOICE && !strncmp(names[j], base, len) && names[j][len] == '_')
                    found = ADSP_CAT_VOICE;
        s_cat[i] = (signed char)(found == ADSP_CAT_VOICE ? ADSP_CAT_VOICE : -1);
    }
    for (i = 0; i < SOUND_TABLE_COUNT; i++) {
        if (s_cat[i] < 0)
            s_cat[i] = ADSP_CAT_SFX;
        if (s_cat[i] == ADSP_CAT_VOICE) nv++;
        if (s_cat[i] == ADSP_CAT_TIMER) nt++;
    }
    s_ready = 1;
    printf("[snd] mixer categories: %d voice, %d timer cues of %d\n", nv, nt, SOUND_TABLE_COUNT);
    if (getenv("SMB2_MIXCAT_DUMP")) {
        static const char *const cn[ADSP_CATS] = { "sfx", "music", "voice", "timer" };
        for (i = 0; i < SOUND_TABLE_COUNT; i++)
            if (lbl_1_rodata_148C[i].name && !(lbl_1_rodata_148C[i].flags & 0x2000))
                printf("[mixcat] %4d %-14s fx 0x%04x flags 0x%04x %-5s %s\n", i, groups[i],
                       lbl_1_rodata_148C[i].fxId, lbl_1_rodata_148C[i].flags, cn[(int)s_cat[i]],
                       lbl_1_rodata_148C[i].name);
    }
}

int pc_snd_cue_category(int index)
{
    if (!s_ready)
        build();
    return (index >= 0 && index < SOUND_TABLE_COUNT) ? s_cat[index] : ADSP_CAT_SFX;
}

void pc_snd_fx_cue(int index)
{
    pc_snd_set_next_category(pc_snd_cue_category(index));
}

void pc_snd_stream_file(u32 stid, const char *fileName)
{
    int cat = (fileName && strstr(fileName, "stream_se/")) ? ADSP_CAT_SFX : ADSP_CAT_MUSIC;
    pc_snd_stream_category(stid, cat);
}
