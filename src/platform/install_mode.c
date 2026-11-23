/* Installed vs portable mode: install.ini, path expansion, userdata, logs.
 * See include/install_mode.h. */
#include "install_mode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <direct.h>
#include <io.h>
#define PSEP '\\'
#else
#include <unistd.h>
#define PSEP '/'
#endif

static int s_inited;
static bool s_installed;
static char s_exe_dir[1024];
static char s_gamedata[1024];
static char s_userdata[1024];
static char s_version[128];
static char s_base[1100];
static char s_local_gd[1100];
static bool s_logging;

#define PC_APPDIR      "SMB2"
#define PC_OLD_APPDIR  "SMB2 PC Port"   /* before 1.0.4 */
#define PC_DEFAULT_USERDATA "%APPDATA%\\SMB2\\installed"

static bool is_sep(char c) { return c == '/' || c == '\\'; }

static void add_sep(char* s, size_t n)
{
    size_t l = strlen(s);
    if (l && !is_sep(s[l - 1]) && l + 1 < n) { s[l] = PSEP; s[l + 1] = 0; }
}

static void strip_sep(char* s)
{
    size_t l = strlen(s);
    while (l > 1 && is_sep(s[l - 1]) && s[l - 2] != ':') s[--l] = 0;
}

static void mkdir_p(const char* path)
{
    char buf[1100];
    size_t i, l;
    snprintf(buf, sizeof buf, "%s", path);
    l = strlen(buf);
    i = 1;
    if (l > 2 && is_sep(buf[0]) && is_sep(buf[1])) {   /* UNC: after \\server\share */
        int seps = 0;
        for (i = 2; i < l; i++)
            if (is_sep(buf[i]) && ++seps == 2) break;
        i++;
    }
    for (; i <= l; i++) {
        if (is_sep(buf[i]) || buf[i] == 0) {
            char c = buf[i];
            struct stat st;
            buf[i] = 0;
            if (!(i == 2 && buf[1] == ':') && buf[0] && stat(buf, &st) != 0) {
#ifdef _WIN32
                _mkdir(buf);
#else
                mkdir(buf, 0755);
#endif
            }
            buf[i] = c;
        }
    }
}

const char* pc_exe_dir(void)
{
    if (!s_exe_dir[0]) {
#ifdef _WIN32
        DWORD n = GetModuleFileNameA(NULL, s_exe_dir, sizeof s_exe_dir);
        if (n == 0 || n >= sizeof s_exe_dir) s_exe_dir[0] = 0;
        else {
            char* slash = strrchr(s_exe_dir, '\\');
            if (!slash) slash = strrchr(s_exe_dir, '/');
            if (slash) slash[1] = 0; else s_exe_dir[0] = 0;
        }
#endif
    }
    return s_exe_dir;
}

#ifdef _WIN32
static bool known_folder(const KNOWNFOLDERID* id, char* out, size_t n)
{
    PWSTR w = NULL;
    bool ok = false;
    if (SHGetKnownFolderPath(id, KF_FLAG_CREATE, NULL, &w) == S_OK && w) {
        BOOL lossy = FALSE;
        int r = WideCharToMultiByte(CP_ACP, 0, w, -1, out, (int)n, NULL, &lossy);
        ok = r > 0 && !lossy;
        if (lossy)
            fprintf(stderr, "[install] known folder path not representable in the ANSI code page\n");
    }
    if (w) CoTaskMemFree(w);
    return ok;
}

/* UTF-8 (install.ini) -> ANSI code page (what fopen() takes) */
static void utf8_to_acp(char* s, size_t n)
{
    WCHAR w[1100];
    char tmp[1100];
    BOOL lossy = FALSE;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, 1100)) return;
    if (WideCharToMultiByte(CP_ACP, 0, w, -1, tmp, sizeof tmp, NULL, &lossy) > 0) {
        if (lossy)
            fprintf(stderr, "[install] '%s' is not representable in the ANSI code page\n", s);
        snprintf(s, n, "%s", tmp);
    }
}
#endif

static bool lookup_var(const char* name, char* out, size_t n)
{
#ifdef _WIN32
    if (!_stricmp(name, "APPDATA") && known_folder(&FOLDERID_RoamingAppData, out, n)) return true;
    if (!_stricmp(name, "LOCALAPPDATA") && known_folder(&FOLDERID_LocalAppData, out, n)) return true;
    if (!_stricmp(name, "PROGRAMDATA") && known_folder(&FOLDERID_ProgramData, out, n)) return true;
#endif
    {
        const char* v = getenv(name);
        if (v && *v) { snprintf(out, n, "%s", v); return true; }
    }
    return false;
}

bool pc_expand_path(const char* in, char* out, size_t n)
{
    size_t o = 0;
    bool ok = true;
    const char* p = in;
    if (!n) return false;
    while (*p && o + 1 < n) {
        if (*p == '%') {
            const char* end = strchr(p + 1, '%');
            if (end && end > p + 1 && end - p - 1 < 64) {
                char name[64], val[1024];
                memcpy(name, p + 1, (size_t)(end - p - 1));
                name[end - p - 1] = 0;
                if (lookup_var(name, val, sizeof val)) {
                    size_t l = strlen(val);
                    if (o + l >= n) { ok = false; break; }
                    memcpy(out + o, val, l);
                    o += l;
                } else {
                    fprintf(stderr, "[install] unknown variable %%%s%% in path '%s'\n", name, in);
                    ok = false;
                }
                p = end + 1;
                continue;
            }
        }
        out[o++] = *p++;
    }
    out[o] = 0;
    if (*p) ok = false;
    return ok;
}

static bool is_absolute(const char* p)
{
    return (p[0] && p[1] == ':' && is_sep(p[2])) || (is_sep(p[0]) && is_sep(p[1])) || p[0] == '/';
}

/* value -> expanded absolute path (relative = relative to the exe dir) */
static bool resolve_value(const char* v, char* out, size_t n)
{
    char tmp[1100];
    if (!pc_expand_path(v, tmp, sizeof tmp) || !tmp[0]) return false;
    if (is_absolute(tmp)) snprintf(out, n, "%s", tmp);
    else snprintf(out, n, "%s%s", pc_exe_dir(), tmp);
    strip_sep(out);
    return true;
}

/* ---- migration from the pre-1.0.4 names ("SMB2 PC Port") ----
 * Setup 1.0.4 renamed the app to "SMB2": userdata moved from
 * %APPDATA%\SMB2 PC Port to %APPDATA%\SMB2\installed and the recovery copy
 * from %LOCALAPPDATA%\SMB2 PC Port\gamedata to %LOCALAPPDATA%\SMB2\gamedata.
 * Done on the first installed-mode start (not with --save-dir, never in
 * portable mode).  Only when install.ini's userdata= is the default value:
 * a custom/test userdata never pulls in the real %APPDATA% folders.  Tests
 * name the old folders with SMB2_PC_TEST_OLD_USERDATA /
 * SMB2_PC_TEST_OLD_LOCAL_GAMEDATA instead (then the real ones are ignored). */
static char s_migrate_log[2048];

static void mig_log(const char* fmt, ...)
{
    char line[1400];
    size_t l = strlen(s_migrate_log);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fputs(line, stdout);
    fflush(stdout);
    if (l + strlen(line) < sizeof s_migrate_log) memcpy(s_migrate_log + l, line, strlen(line) + 1);
}

#ifdef _WIN32
static bool dir_exists(const char* p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool file_exists(const char* p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* settings.dat or memcard\ in the folder = it holds the user's data */
static bool has_user_data(const char* dir)
{
    char p[1200];
    snprintf(p, sizeof p, "%s\\settings.dat", dir);
    if (file_exists(p)) return true;
    snprintf(p, sizeof p, "%s\\memcard", dir);
    return dir_exists(p);
}

/* Copy src into dst (merging; files already in dst are kept), deleting each
 * source file once it is copied and each source folder once it is empty.
 * Returns the number of files that could not be moved. */
static int move_tree_by_copy(const char* src, const char* dst)
{
    char pat[1200];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int left = 0;
    CreateDirectoryA(dst, NULL);
    snprintf(pat, sizeof pat, "%s\\*", src);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        char s[1200], d[1200];
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        snprintf(s, sizeof s, "%s\\%s", src, fd.cFileName);
        snprintf(d, sizeof d, "%s\\%s", dst, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { left++; continue; }  /* never follow links */
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            left += move_tree_by_copy(s, d);
        } else if (CopyFileA(s, d, TRUE)) {   /* TRUE: never overwrite */
            DeleteFileA(s);
        } else {
            left++;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    RemoveDirectoryA(src);   /* only if empty */
    return left;
}

/* Move folder old_dir to new_dir (rename; else copy + delete).  new_dir may
 * exist only without user data (e.g. an empty folder). */
static void move_dir(const char* what, const char* old_dir, const char* new_dir)
{
    char parent[1100];
    char* sl;
    int left;
    snprintf(parent, sizeof parent, "%s", new_dir);
    sl = strrchr(parent, '\\');
    if (sl) { *sl = 0; mkdir_p(parent); }
    RemoveDirectoryA(new_dir);   /* an empty leftover would block the rename */
    if (!dir_exists(new_dir) && MoveFileExA(old_dir, new_dir, 0)) {
        mig_log("[install] migrated %s: moved %s -> %s\n", what, old_dir, new_dir);
        return;
    }
    left = move_tree_by_copy(old_dir, new_dir);
    if (left)
        mig_log("[install] migrated %s by copying %s -> %s; %d file(s) could not be moved and stay in the old folder\n",
                what, old_dir, new_dir, left);
    else
        mig_log("[install] migrated %s: copied %s -> %s (old folder removed)\n", what, old_dir, new_dir);
}

static bool is_our_gamedata(const char* dir)
{
    char p[1200], line[64] = "";
    FILE* f;
    snprintf(p, sizeof p, "%s\\manifest.txt", dir);
    f = fopen(p, "rb");
    if (!f) return false;
    if (!fgets(line, sizeof line, f)) line[0] = 0;
    fclose(f);
    line[strcspn(line, "\r\n")] = 0;
    return !strcmp(line, "SMB2PC-GAMEDATA 1");
}
#endif

static void migrate_old_names(const char* ud_raw)
{
#ifdef _WIN32
    char old_ud[1100] = "", old_lgd[1100] = "", old_lparent[1100] = "", base[1024];
    const char* t_ud = getenv("SMB2_PC_TEST_OLD_USERDATA");
    const char* t_lgd = getenv("SMB2_PC_TEST_OLD_LOCAL_GAMEDATA");
    const char* new_lgd;
    if ((t_ud && *t_ud) || (t_lgd && *t_lgd)) {          /* tests */
        if (t_ud && *t_ud) snprintf(old_ud, sizeof old_ud, "%s", t_ud);
        if (t_lgd && *t_lgd) snprintf(old_lgd, sizeof old_lgd, "%s", t_lgd);
    } else if (!_stricmp(ud_raw, PC_DEFAULT_USERDATA)) {
        if (lookup_var("APPDATA", base, sizeof base))
            snprintf(old_ud, sizeof old_ud, "%s\\%s", base, PC_OLD_APPDIR);
        if (!getenv("SMB2_PC_LOCAL_GAMEDATA") && lookup_var("LOCALAPPDATA", base, sizeof base)) {
            snprintf(old_lparent, sizeof old_lparent, "%s\\%s", base, PC_OLD_APPDIR);
            snprintf(old_lgd, sizeof old_lgd, "%s\\gamedata", old_lparent);
        }
    } else {
        return;   /* custom userdata= : leave other folders alone */
    }
    strip_sep(old_ud);
    strip_sep(old_lgd);
    /* saves, settings, shader cache, logs */
    if (old_ud[0] && _stricmp(old_ud, s_userdata) != 0 && dir_exists(old_ud)) {
        if (has_user_data(s_userdata))
            mig_log("[install] not migrating %s: %s already has saves/settings\n", old_ud, s_userdata);
        else
            move_dir("saves and settings", old_ud, s_userdata);
    }
    /* recovery copy of the game data */
    new_lgd = pc_local_gamedata_dir();
    if (old_lgd[0] && new_lgd && _stricmp(old_lgd, new_lgd) != 0 && dir_exists(old_lgd)) {
        if (!is_our_gamedata(old_lgd))
            mig_log("[install] not migrating %s: no game data manifest\n", old_lgd);
        else if (dir_exists(new_lgd) && is_our_gamedata(new_lgd))
            mig_log("[install] not migrating %s: %s already exists\n", old_lgd, new_lgd);
        else
            move_dir("the game data recovery copy", old_lgd, new_lgd);
        if (old_lparent[0]) RemoveDirectoryA(old_lparent);   /* only if empty */
    }
#else
    (void)ud_raw;
#endif
}

void pc_install_init(const char* save_dir_arg)
{
    char path[1100], line[1200];
    FILE* f;
    int lineno = 0;
    char mode[32] = "", gd[1100] = "", ud[1100] = "";

    if (s_inited) return;
    s_inited = 1;
    snprintf(path, sizeof path, "%sinstall.ini", pc_exe_dir());
    f = pc_exe_dir()[0] ? fopen(path, "rb") : NULL;
    if (!f) return;                        /* portable: nothing changes */
    while (fgets(line, sizeof line, f)) {
        char *k = line, *v, *eq;
        lineno++;
        if (lineno == 1 && (unsigned char)k[0] == 0xEF && (unsigned char)k[1] == 0xBB &&
            (unsigned char)k[2] == 0xBF)
            k += 3;                        /* UTF-8 BOM */
        k[strcspn(k, "\r\n")] = 0;
        while (*k == ' ' || *k == '\t') k++;
        if (!*k || *k == '#' || *k == ';' || *k == '[') continue;
        eq = strchr(k, '=');
        if (!eq) continue;
        *eq = 0;
        v = eq + 1;
        { char* e = eq; while (e > k && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0; }
        while (*v == ' ' || *v == '\t') v++;
        { size_t l = strlen(v); while (l && (v[l - 1] == ' ' || v[l - 1] == '\t')) v[--l] = 0; }
        if (v[0] == '"' && strlen(v) >= 2 && v[strlen(v) - 1] == '"') { v[strlen(v) - 1] = 0; v++; }
#ifdef _WIN32
        utf8_to_acp(v, 1100);
#endif
        if (!strcmp(k, "mode")) snprintf(mode, sizeof mode, "%s", v);
        else if (!strcmp(k, "gamedata")) snprintf(gd, sizeof gd, "%s", v);
        else if (!strcmp(k, "userdata")) snprintf(ud, sizeof ud, "%s", v);
        else if (!strcmp(k, "version")) snprintf(s_version, sizeof s_version, "%s", v);
    }
    fclose(f);
    if (strcmp(mode, "installed") != 0) {
        printf("[install] %s has mode=%s - running portable\n", path, mode[0] ? mode : "(none)");
        return;
    }
    s_installed = true;
    if (gd[0] && !resolve_value(gd, s_gamedata, sizeof s_gamedata)) {
        fprintf(stderr, "[install] cannot expand gamedata=%s\n", gd);
        s_gamedata[0] = 0;
    }
    if (!ud[0]) snprintf(ud, sizeof ud, "%s", PC_DEFAULT_USERDATA);
    if (!resolve_value(ud, s_userdata, sizeof s_userdata)) {
        fprintf(stderr, "[install] cannot expand userdata=%s\n", ud);
        s_userdata[0] = 0;
    } else {
        if (!(save_dir_arg && save_dir_arg[0])) { /* --save-dir: leave userdata alone */
            migrate_old_names(ud);
            mkdir_p(s_userdata);
        }
        add_sep(s_userdata, sizeof s_userdata);
    }
    if (save_dir_arg && save_dir_arg[0]) {
        snprintf(s_base, sizeof s_base, "%s", save_dir_arg);
        add_sep(s_base, sizeof s_base);
    } else if (s_userdata[0]) {
        snprintf(s_base, sizeof s_base, "%s", s_userdata);
    }
    printf("[install] installed mode (version %s): gamedata=%s userdata=%s%s\n",
           s_version[0] ? s_version : "?", s_gamedata[0] ? s_gamedata : "(none)",
           s_userdata[0] ? s_userdata : "(none)",
           save_dir_arg && save_dir_arg[0] ? " (--save-dir overrides userdata)" : "");
    fflush(stdout);
}

bool pc_installed(void) { return s_installed; }
const char* pc_install_gamedata(void) { return s_installed && s_gamedata[0] ? s_gamedata : NULL; }
const char* pc_install_userdata(void) { return s_installed && s_userdata[0] ? s_userdata : NULL; }
const char* pc_install_version(void) { return s_version; }
const char* pc_userdata_base(void) { return s_installed && s_base[0] ? s_base : NULL; }

const char* pc_local_gamedata_dir(void)
{
    if (!s_local_gd[0]) {
        char base[1024];
        const char* ovr = getenv("SMB2_PC_LOCAL_GAMEDATA");   /* tests / advanced */
        if (ovr && *ovr) {
            snprintf(s_local_gd, sizeof s_local_gd, "%s", ovr);
            strip_sep(s_local_gd);
        } else if (lookup_var("LOCALAPPDATA", base, sizeof base))
            snprintf(s_local_gd, sizeof s_local_gd, "%s%c%s%cgamedata", base, PSEP, PC_APPDIR, PSEP);
    }
    return s_local_gd[0] ? s_local_gd : NULL;
}

bool pc_stdin_interactive(void)
{
#ifdef _WIN32
    return GetFileType(GetStdHandle(STD_INPUT_HANDLE)) == FILE_TYPE_CHAR;
#else
    return isatty(0);
#endif
}

void pc_install_setup_logs(void)
{
#ifdef _WIN32
    char dir[1200], log[1300], old[1300];
    /* installed: <userdata>\logs; portable: <exe dir>\logs, next to the
     * portable memcard\ folder (a crash otherwise leaves no trace once
     * the console closes).  If the folder isn't writable, the console
     * keeps the output. */
    const char* base = s_installed ? pc_userdata_base() : pc_exe_dir();
    if (!base || !base[0]) return;
    /* only when stdout is the console: redirected output (tests, the
     * installer, a user's "> file") is left alone */
    if (GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) != FILE_TYPE_CHAR) return;
    snprintf(dir, sizeof dir, "%slogs", base);
    mkdir_p(dir);
    snprintf(log, sizeof log, "%s%csmb2_pc_port.log", dir, PSEP);
    snprintf(old, sizeof old, "%s%csmb2_pc_port.old.log", dir, PSEP);
    remove(old);
    rename(log, old);
    fflush(stdout);
    fflush(stderr);
    if (!freopen(log, "w", stdout)) {
        freopen("CONOUT$", "w", stdout);
        return;
    }
    _dup2(_fileno(stdout), _fileno(stderr));
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    s_logging = true;
    {
        FILE* con = fopen("CONOUT$", "w");
        if (con) {
            fprintf(con, "SMB2 - log: %s\n", log);
            fclose(con);
        }
    }
    if (s_migrate_log[0]) {   /* what migrate_old_names() did, for the log file */
        fputs(s_migrate_log, stdout);
        fflush(stdout);
    }
#endif
}

void pc_user_msg(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
#ifdef _WIN32
    if (s_logging) {
        FILE* con = fopen("CONOUT$", "w");
        if (con) {
            va_start(ap, fmt);
            vfprintf(con, fmt, ap);
            va_end(ap);
            fclose(con);
        }
    }
#endif
}
