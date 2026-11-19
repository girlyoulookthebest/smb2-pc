/* DVD shim: GameCube DVD API over the pc_port ISO archive.
 * Entry numbers ARE FST indices, which map 1:1 to the ISOArchive entries[]
 * array, so DVDFastOpen(entryNum) is a direct index.  All reads are
 * synchronous; async callbacks fire immediately. */
#include "shim.h"
#include "file.h"   /* decomp DiscFileInfo layout (DVDFileInfo, 0x3C) */
#include "bitmap.h" /* FONT_DEFINITIONS */
#include "button_prompts.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int s_current_dir = 0;  /* FST index of the DVD current directory */

static struct ISOArchive *arc(void)
{
    return platform_get_iso_archive();
}

s32 DVDConvertPathToEntrynum(char *path)
{
    if (!arc() || !path)
        return -1;
    return iso_path_to_index_from(arc(), s_current_dir, path);
}

s32 DVDChangeDir(char *path)
{
    s32 e = DVDConvertPathToEntrynum(path);
    if (e < 0 || !iso_entry_is_dir(arc(), e)) {
        fprintf(stderr, "DVDChangeDir: no such dir: %s\n", path);
        return 0;
    }
    s_current_dir = e;
    return 1;
}

s32 DVDFastOpen(s32 entryNum, struct DiscFileInfo *fi)
{
    if (!arc() || entryNum < 0 || entryNum >= iso_entry_count(arc()) ||
        iso_entry_is_dir(arc(), entryNum))
        return 0;
    memset(fi, 0, sizeof(*fi));
    *(s32 *)fi->filler0 = entryNum;   /* stash entrynum in the command block */
    fi->length = iso_entry_size(arc(), entryNum);
    return 1;
}

s32 DVDOpen(char *path, struct DiscFileInfo *fi)
{
    s32 e = DVDConvertPathToEntrynum(path);
    if (e < 0) {
        fprintf(stderr, "DVDOpen: not found: %s\n", path);
        return 0;
    }
    return DVDFastOpen(e, fi);
}

s32 DVDClose(struct DiscFileInfo *fi)
{
    (void)fi;
    return 1;
}

/* ---- directory iteration (recplay.c scans /replay/ with these) ---- */
/* Layouts match the game's local typedefs in recplay.c. */
struct DVDDirShim      { u32 entryNum; u32 location; u32 next; };
struct DVDDirEntryShim { u32 entryNum; s32 isDir; char *name; };

s32 DVDOpenDir(char *path, struct DVDDirShim *dir)
{
    s32 e = DVDConvertPathToEntrynum(path);
    if (e < 0 || !iso_entry_is_dir(arc(), e)) {
        fprintf(stderr, "DVDOpenDir: no such dir: %s\n", path ? path : "(null)");
        return 0;
    }
    dir->entryNum = (u32)e;
    dir->location = (u32)e + 1;
    dir->next = (u32)iso_entry_next(arc(), e);
    return 1;
}

s32 DVDReadDir(struct DVDDirShim *dir, struct DVDDirEntryShim *entry)
{
    s32 loc = (s32)dir->location;
    if (loc <= (s32)dir->entryNum || loc >= (s32)dir->next)
        return 0;
    entry->entryNum = (u32)loc;
    entry->isDir = iso_entry_is_dir(arc(), loc);
    entry->name = (char *)iso_entry_name(arc(), loc);
    dir->location = (u32)iso_entry_next(arc(), loc);  /* skip subdir subtree */
    return 1;
}

s32 fn_80018FF4(struct DVDDirShim *dir)  /* DVDCloseDir */
{
    (void)dir;
    return 1;
}

/* The game's sound code (smgr.c/sound.c) sets its "read in flight" state
 * *after* DVDReadAsyncPrio returns, so completing those reads synchronously
 * would have their callbacks' state overwritten (music never starts / never
 * refills).  For those callbacks the data is read immediately but the
 * completion callback is queued and run at the next audio frame service. */
void g_some_dvd_read_async_sound_callback(s32 result, void *fileInfo);
void g_some_dvd_read_async_sound_callback2(s32 result, void *fileInfo);
void fn_1_33DC0(s32 result, void *fileInfo);

#define DVD_MAX_DEFERRED 64
struct DeferredRead {
    void (*cb)(s32, struct DiscFileInfo *);
    struct DiscFileInfo *fi;
    s32 result;
};
static struct DeferredRead s_deferred[DVD_MAX_DEFERRED];
static int s_deferred_count;

static int dvd_should_defer(void (*cb)(s32, struct DiscFileInfo *))
{
    return cb == (void *)g_some_dvd_read_async_sound_callback ||
           cb == (void *)g_some_dvd_read_async_sound_callback2 ||
           cb == (void *)fn_1_33DC0;
}

s32 DVDReadAsyncPrio(struct DiscFileInfo *fi, void *addr, s32 length, s32 offset,
                     void (*callback)(s32, struct DiscFileInfo *), s32 prio)
{
    s32 entry = *(s32 *)fi->filler0;
    int ok;
    (void)prio;
    ok = iso_file_read_at(arc(), entry, (unsigned)offset, (unsigned)length, addr);
    if (ok < 0) {
        fprintf(stderr, "DVDReadAsyncPrio: read failed (entry %d ofs 0x%x len 0x%x)\n",
                entry, offset, length);
        if (callback)
            callback(-1, fi);
        return 0;
    }
    if (callback && dvd_should_defer(callback)) {
        if (s_deferred_count < DVD_MAX_DEFERRED) {
            s_deferred[s_deferred_count].cb = callback;
            s_deferred[s_deferred_count].fi = fi;
            s_deferred[s_deferred_count].result = length;
            s_deferred_count++;
            if (callback == (void *)g_some_dvd_read_async_sound_callback2 ||
                callback == (void *)fn_1_33DC0)
                printf("[snd] header read %s\n", iso_entry_name(arc(), entry));
            return 1;
        }
        fprintf(stderr, "DVDReadAsyncPrio: deferred queue full, completing inline\n");
    }
    if (callback)
        callback(length, fi);
    return 1;
}

/* Run the deferred sound read callbacks (called once per frame by the audio
 * shim, on the game thread).  Callbacks queued by these callbacks run too. */
void dvd_run_deferred_callbacks(void)
{
    int i, guard = 0;

    while (s_deferred_count > 0 && guard++ < 64) {
        struct DeferredRead d = s_deferred[0];
        for (i = 1; i < s_deferred_count; i++)
            s_deferred[i - 1] = s_deferred[i];
        s_deferred_count--;
        d.cb(d.result, d.fi);
    }
}

s32 DVDGetCommandBlockStatus(const void *block)
{
    (void)block;
    return 0; /* DVD_STATE_END: everything completes synchronously */
}

void *DVDGetCurrentDiskID(void)
{
    return gc_mem_ptr(0x80000000u);  /* disc ID bytes placed by gc_mem_init */
}

/* ------------------------------------------------------------------ */
/* Whole-file read used by the game's loaders, including the REL      */
/* loader (relocation.c).  Returns nonzero on success, 0 on failure.  */
/*                                                                    */
/* REL interception: the original reads mkb2.*.rel from disc, OSLink()s */
/* it, and jumps through the module header's prolog pointer.  The RELs */
/* we have decompiled are linked into this exe, so instead of reading  */
/* big-endian REL data we fabricate a native module header whose       */
/* prolog/epilog point at the linked-in entry points.                  */

struct RelModuleHeaderShim {           /* matches decomp relocation.c */
    u8  filler0[0x20];
    u32 bssSize;                       /* +0x20 */
    u8  filler24[0x34 - 0x24];
    void (*prolog)(void);              /* +0x34 */
    void (*epilog)(void);              /* +0x38 */
};

extern void mainGamePrologue(void);
extern void mainGameEpilogue(void);
extern void selStagePrologue(void);
extern void selStageEpilogue(void);
extern void exOptionPrologue(void);
extern void exOptionEpilogue(void);
extern void pcLauncherPrologue(void);    /* stand-in for sel_ngc (pc_launcher.c) */
extern void pcLauncherEpilogue(void);
extern void selNgcPrologue(void);
extern void selNgcEpilogue(void);
/* The decompiled sel_ngc menu is the default; SMB2_PC_LAUNCHER=1 selects the
 * debug-text stand-in launcher instead. */
static int use_pc_launcher(void)
{
    const char *e = getenv("SMB2_PC_LAUNCHER");
    return e && e[0] == '1';
}
static void selNgcOrLauncherPrologue(void)
{
    if (use_pc_launcher()) pcLauncherPrologue(); else selNgcPrologue();
}
static void selNgcOrLauncherEpilogue(void)
{
    if (use_pc_launcher()) pcLauncherEpilogue(); else selNgcEpilogue();
}
extern void pcLauncherEpilogue(void);
extern void race2Prologue(void), race2Epilogue(void);
extern void pilot2Prologue(void), pilot2Epilogue(void);
extern void bowlingPrologue(void), bowlingEpilogue(void);
extern void boatPrologue(void), boatEpilogue(void);
extern void dogfightPrologue(void), dogfightEpilogue(void);
extern void baseballPrologue(void), baseballEpilogue(void);
extern void golfPrologue(void), golfEpilogue(void);
extern void fightPrologue(void), fightEpilogue(void);
void pc_test_dip_apply(void);   /* pc_launcher.c: SMB2_PC_DIP test hook */

static const struct { const char *name; void (*pro)(void); void (*epi)(void); }
s_linked_rels[] = {
    { "mkb2.main_game.rel",        mainGamePrologue,   mainGameEpilogue },
    { "mkb2.sel_stage.rel",        selStagePrologue,   selStageEpilogue },
    { "mkb2.exoption.rel",         exOptionPrologue,   exOptionEpilogue },
    { "mkb2.sel_ngc.rel",          selNgcOrLauncherPrologue, selNgcOrLauncherEpilogue },
    { "mkb2.race2.rel",            race2Prologue,      race2Epilogue },
    { "mkb2.pilot2.rel",           pilot2Prologue,     pilot2Epilogue },
    { "mkb2.rel_mini_bowling.rel", bowlingPrologue,    bowlingEpilogue },
    { "mkb2.boat.rel",             boatPrologue,       boatEpilogue },
    { "mkb2.dogfight.rel",         dogfightPrologue,   dogfightEpilogue },
    { "mkb2.baseball.rel",         baseballPrologue,   baseballEpilogue },
    { "mkb2.golf2.rel",            golfPrologue,       golfEpilogue },
    { "mkb2.mini_fight2.rel",      fightPrologue,      fightEpilogue },
};

static int rel_find(const char *name)
{
    unsigned i;

    for (i = 0; name && i < sizeof(s_linked_rels) / sizeof(s_linked_rels[0]); i++)
        if (strcmp(name, s_linked_rels[i].name) == 0)
            return (int)i;
    return -1;
}

/* The one answer to "is this REL's code in the exe": the menus mark the party
 * games whose REL is not (pc_party_avail.c), so a newly linked game shows up
 * as playable without further changes. */
int pc_rel_is_linked(const char *name)
{
    return rel_find(name) >= 0;
}

/* test hook: SMB2_PC_TEST_REL=<name> makes loading that REL fail as if it
 * were not linked (the menus still offer it), to exercise the failed-load
 * paths (mini_mode.c returns to the menu) */
static int rel_test_unlinked(const char *name)
{
    const char *e = getenv("SMB2_PC_TEST_REL");
    return e && e[0] && strcmp(e, name) == 0;
}

static int rel_fill_fake_header(const char *name, void *buffer, u32 size)
{
    struct RelModuleHeaderShim *h = (struct RelModuleHeaderShim *)buffer;
    int i;

    pc_test_dip_apply();
    i = rel_find(name);
    if (i < 0 || rel_test_unlinked(name)) {
        fprintf(stderr, "[REL] %s is not linked into this build - load fails\n", name);
        return 0;
    }
    if (size < sizeof(*h))
        return 0;
    memset(buffer, 0, sizeof(*h));
    h->bssSize = 32;    /* REL bss objects are linked statically already */
    h->prolog  = s_linked_rels[i].pro;
    h->epilog  = s_linked_rels[i].epi;
    fprintf(stderr, "[REL] dispatching %s to linked-in module\n", name);
    return 1;
}

s32 read_entire_file_using_dvdread_prio_async(struct DiscFileInfo *fi,
                                              void *dest, u32 size, u32 offset)
{
    s32 entry;
    const char *name;

    if (!arc() || !fi || !dest)
        return 0;
    entry = *(s32 *)fi->filler0;
    name = iso_entry_name(arc(), entry);

    if (name && strstr(name, ".rel") && offset == 0)
        return rel_fill_fake_header(name, dest, size);

    if (iso_file_read_at(arc(), entry, offset, size, dest) != 0) {
        fprintf(stderr, "read_entire_file: read failed (entry %d '%s')\n",
                entry, name ? name : "?");
        return 0;
    }
    return 1;
}

/* ---- ARAM font (DOL g_load_aram_font 0x8000A7FC / aram_font_to_mram_font
 * 0x8000AA50).  Retail reads font/fnt_<name>.bin into ARAM 0x7C0000 at boot
 * and ARQ-copies <pageSize>-byte glyph pages into MRAM on demand.  The DOL
 * font table (0x80081D80, 0x91 entries) names a file for font id 3 only
 * (asc_24x24, pageSize 0x240); on PC the file just stays in MRAM and pages
 * are memcpy'd out of it on demand, loading the file on first use. */
void aram_font_to_mram_font(u8 fontId, int page, void *dest)
{
    static u8 *buf;
    static u32 len;
    const u32 pageSize = 0x240;
    u32 off;

    if (fontId != 3) {
        static int warned;
        if (!warned++)
            fprintf(stderr, "aram_font_to_mram_font: font %u has no ARAM pages\n",
                    fontId);
        return;
    }
    if (!buf) {
        s32 e = arc() ? iso_path_to_index_from(arc(), 0, "font/fnt_asc_24x24.bin") : -1;
        u32 sz;
        if (e < 0) {
            fprintf(stderr, "aram_font_to_mram_font: fnt_asc_24x24.bin not on disc\n");
            return;
        }
        sz = iso_entry_size(arc(), e);
        buf = malloc(sz);
        if (!buf || iso_file_read_at(arc(), e, 0, sz, buf) != 0) {
            fprintf(stderr, "aram_font_to_mram_font: font file read failed\n");
            free(buf);
            buf = 0;
            return;
        }
        len = sz;
        fprintf(stderr, "[font] fnt_asc_24x24.bin loaded: 0x%x bytes (%u pages)\n",
                sz, sz / pageSize);
    }
    off = (u32)page * pageSize;
    if (off + pageSize <= len)
        memcpy(dest, buf + off, pageSize);
    /* PlayStation/Xbox prompt icons in place of the GC pad-button glyphs */
    {
        const u16 *codes = FONT_DEFINITIONS[3].string;
        int i;
        for (i = 0; codes[i] != 0 && i < page; i++) {}
        if (i == page && codes[i] != 0)
            pc_button_prompts_glyph(codes[i], dest);
    }
}

/* synchronous DVDReadPrio: returns bytes read or -1 */
s32 DVDReadPrio(struct DiscFileInfo *fi, void *addr, s32 length, s32 offset, s32 prio)
{
    s32 entry = *(s32 *)fi->filler0;
    (void)prio;
    if (iso_file_read_at(arc(), entry, (unsigned)offset, (unsigned)length, addr) < 0)
        return -1;
    return length;
}
