// Hello Neighbor Tweaks - installer.
// Copyright (c) 2026 forgotten893. Released under the MIT License (see LICENSE).
//
// One self-contained exe: XINPUT1_3.dll, the readme, the license and the source code are embedded as resources.
// It finds Hello Neighbor (Steam libraries, Epic, GOG, uninstall entries, common folders), copies the files next to
// HelloNeighbor-Win64-Shipping.exe and can uninstall again. It never writes the settings file: the DLL creates
// HelloNeighborTweaks.ini itself on first run, so an existing one is always kept.
// It runs with normal user rights only: it never asks for administrator rights and refuses to run elevated. A game
// folder the user cannot write to (e.g. under Program Files) gets a pointer to the portable zip instead.
//
// Command line (for scripts and testing; exit code 0 = OK):
//   setup.exe /detect                          print every install found
//   setup.exe /install   "<game folder>" [/quiet]
//   setup.exe /uninstall "<game folder>" [/removesettings] [/quiet]
// <game folder> may be the game's root folder, its Win64 folder or the exe itself.
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <wchar.h>
#include <stdio.h>
#include <string.h>
#include "resource.h"
#include "..\src\version.h"
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comctl32.lib")

#define EXE_NAME   L"HelloNeighbor-Win64-Shipping.exe"
#define WIN64_REL  L"\\HelloNeighbor\\Binaries\\Win64"
#define DLL_NAME   L"XINPUT1_3.dll"
#define BACKUP_EXT L".tweaks-backup"
#define CFG_NAME   L"HelloNeighborTweaks.ini"
#define LOG_NAME   L"HelloNeighborTweaks.log"
#define README_OUT L"HelloNeighborTweaks_README.txt"
#define LICENSE_OUT L"HelloNeighborTweaks_LICENSE.txt"
#define SOURCE_OUT L"HelloNeighborTweaks_Source.zip"
#define TITLE      L"Hello Neighbor Tweaks " HNT_VERSION_W L" Setup"

#define ZIP_NAME   L"HelloNeighborTweaks-" HNT_VERSION_W L"-Portable.zip"

enum { RC_OK = 0, RC_FAIL = 1, RC_RUNNING = 2, RC_NOTGAME = 3, RC_DENIED = 5, RC_ELEVATED = 6 };

static int g_quiet;      // no message boxes at all

// ------------------------------------------------------------------ small helpers
static int FileExists(const wchar_t *p) { DWORD a = GetFileAttributesW(p); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
static int DirExists(const wchar_t *p)  { DWORD a = GetFileAttributesW(p); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
static void StripSlash(wchar_t *p) { size_t n = wcslen(p); while (n > 3 && (p[n - 1] == L'\\' || p[n - 1] == L'/')) p[--n] = 0; }
static void UpOne(wchar_t *p) { wchar_t *s = wcsrchr(p, L'\\'); if (s && s > p + 2) *s = 0; }
static void Join(wchar_t *out, const wchar_t *a, const wchar_t *b) { swprintf_s(out, MAX_PATH, L"%s\\%s", a, b); }

static void Say(UINT icon, const wchar_t *fmt, ...)
{
    if (g_quiet) return;
    wchar_t t[2048]; va_list ap; va_start(ap, fmt); vswprintf_s(t, 2048, fmt, ap); va_end(ap);
    MessageBoxW(GetActiveWindow(), t, TITLE, icon | MB_OK);
}

// Console output for /detect (this is a GUI exe: use the caller's pipe, or attach to its console)
static void Out(const wchar_t *s)
{
    static HANDLE h; static int init;
    if (!init) {
        init = 1; h = GetStdHandle(STD_OUTPUT_HANDLE);
        if (!h || h == INVALID_HANDLE_VALUE) {
            if (AttachConsole(ATTACH_PARENT_PROCESS)) h = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        }
    }
    if (!h || h == INVALID_HANDLE_VALUE) return;
    char u[4096]; int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, u, sizeof u, NULL, NULL);
    DWORD w; if (n > 1) WriteFile(h, u, (DWORD)(n - 1), &w, NULL);
}

// ------------------------------------------------------------------ game folder
// Accepts the root, the Win64 folder or the exe; returns the root (the folder that holds "HelloNeighbor\Binaries").
static int ResolveGameRoot(const wchar_t *in, wchar_t *root)
{
    wchar_t p[MAX_PATH], t[MAX_PATH];
    if (!in || !in[0]) return 0;
    if (!GetFullPathNameW(in, MAX_PATH, p, NULL)) return 0;
    StripSlash(p);
    size_t n = wcslen(p);
    if (n > 4 && !_wcsicmp(p + n - 4, L".exe")) UpOne(p);          // ...\Win64\HelloNeighbor-Win64-Shipping.exe
    Join(t, p, EXE_NAME);
    if (FileExists(t)) { UpOne(p); UpOne(p); UpOne(p); }            // ...\HelloNeighbor\Binaries\Win64
    swprintf_s(t, MAX_PATH, L"%s" WIN64_REL L"\\" EXE_NAME, p);
    if (!FileExists(t)) return 0;
    wcscpy_s(root, MAX_PATH, p);
    return 1;
}
static void Win64Dir(const wchar_t *root, wchar_t *out) { swprintf_s(out, MAX_PATH, L"%s" WIN64_REL, root); }

// ------------------------------------------------------------------ detection
typedef struct { wchar_t root[MAX_PATH]; const wchar_t *src; } Found;
static Found g_found[32]; static int g_nFound;

static void AddCandidate(const wchar_t *path, const wchar_t *src)
{
    wchar_t r[MAX_PATH];
    if (g_nFound >= 32 || !ResolveGameRoot(path, r)) return;
    for (int i = 0; i < g_nFound; i++) if (!_wcsicmp(g_found[i].root, r)) return;
    wcscpy_s(g_found[g_nFound].root, MAX_PATH, r); g_found[g_nFound].src = src; g_nFound++;
}

static int RegStr(HKEY root, const wchar_t *key, const wchar_t *val, REGSAM view, wchar_t *out, DWORD cch)
{
    HKEY k; DWORD type, cb = cch * sizeof(wchar_t);
    if (RegOpenKeyExW(root, key, 0, KEY_READ | view, &k) != ERROR_SUCCESS) return 0;
    LONG r = RegQueryValueExW(k, val, NULL, &type, (BYTE *)out, &cb);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return 0;
    out[cch - 1] = 0;
    for (wchar_t *c = out; *c; c++) if (*c == L'/') *c = L'\\';
    return out[0] != 0;
}

// Reads a small text file (UTF-8) as UTF-16.
static wchar_t *ReadText(const wchar_t *path)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD n = GetFileSize(h, NULL), got = 0;
    if (n == INVALID_FILE_SIZE || n > 8u << 20) { CloseHandle(h); return NULL; }
    char *a = (char *)HeapAlloc(GetProcessHeap(), 0, n + 1);
    if (!a) { CloseHandle(h); return NULL; }
    ReadFile(h, a, n, &got, NULL); CloseHandle(h); a[got] = 0;
    int wn = MultiByteToWideChar(CP_UTF8, 0, a, (int)got + 1, NULL, 0);
    wchar_t *w = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, (wn + 1) * sizeof(wchar_t));
    if (w) MultiByteToWideChar(CP_UTF8, 0, a, (int)got + 1, w, wn);
    HeapFree(GetProcessHeap(), 0, a);
    return w;
}

// Every quoted string in a Steam .vdf that looks like a folder is a library ("path" in the new format, the value
// itself in the old one). Backslashes are escaped as \\ in the file.
static void ScanSteamLibraries(const wchar_t *steamRoot)
{
    wchar_t f[MAX_PATH], lib[MAX_PATH], c[MAX_PATH];
    swprintf_s(c, MAX_PATH, L"%s\\steamapps\\common\\Hello Neighbor", steamRoot); AddCandidate(c, L"Steam");
    const wchar_t *files[2] = { L"steamapps\\libraryfolders.vdf", L"config\\libraryfolders.vdf" };
    for (int k = 0; k < 2; k++) {
        Join(f, steamRoot, files[k]);
        wchar_t *t = ReadText(f); if (!t) continue;
        for (wchar_t *p = t; (p = wcschr(p, L'"')) != NULL; ) {
            wchar_t *e = p + 1; int n = 0;
            while (*e && *e != L'"' && n < MAX_PATH - 1) {
                if (e[0] == L'\\' && e[1] == L'\\') { lib[n++] = L'\\'; e += 2; } else lib[n++] = *e++;
            }
            lib[n] = 0;
            if (*e != L'"') break;
            p = e + 1;
            if (n > 2 && lib[1] == L':' && lib[2] == L'\\') {
                swprintf_s(c, MAX_PATH, L"%s\\steamapps\\common\\Hello Neighbor", lib); AddCandidate(c, L"Steam");
            }
        }
        HeapFree(GetProcessHeap(), 0, t);
    }
}

// Epic: %ProgramData%\Epic\EpicGamesLauncher\Data\Manifests\*.item (JSON) "InstallLocation": "..."
static void ScanEpic(void)
{
    wchar_t pd[MAX_PATH], dir[MAX_PATH], pat[MAX_PATH], f[MAX_PATH];
    if (!GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH)) return;
    swprintf_s(dir, MAX_PATH, L"%s\\Epic\\EpicGamesLauncher\\Data\\Manifests", pd);
    swprintf_s(pat, MAX_PATH, L"%s\\*.item", dir);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        Join(f, dir, fd.cFileName);
        wchar_t *t = ReadText(f); if (!t) continue;
        wchar_t *p = wcsstr(t, L"\"InstallLocation\"");
        if (p && (p = wcschr(p + 17, L'"')) != NULL) {
            wchar_t loc[MAX_PATH]; int n = 0; p++;
            while (*p && *p != L'"' && n < MAX_PATH - 1) {
                if (p[0] == L'\\' && p[1] == L'\\') { loc[n++] = L'\\'; p += 2; }
                else if (*p == L'/') { loc[n++] = L'\\'; p++; } else loc[n++] = *p++;
            }
            loc[n] = 0; AddCandidate(loc, L"Epic Games");
        }
        HeapFree(GetProcessHeap(), 0, t);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Any uninstall entry or GOG entry whose folder holds the game exe
static void ScanRegistryTree(HKEY root, const wchar_t *base, const wchar_t *valName, const wchar_t *src)
{
    const REGSAM views[2] = { KEY_WOW64_64KEY, KEY_WOW64_32KEY };
    for (int v = 0; v < 2; v++) {
        HKEY k; if (RegOpenKeyExW(root, base, 0, KEY_READ | views[v], &k) != ERROR_SUCCESS) continue;
        wchar_t sub[256], key[512], loc[MAX_PATH];
        for (DWORD i = 0; ; i++) {
            DWORD cch = 256; if (RegEnumKeyExW(k, i, sub, &cch, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
            swprintf_s(key, 512, L"%s\\%s", base, sub);
            if (RegStr(root, key, valName, views[v], loc, MAX_PATH)) AddCandidate(loc, src);
        }
        RegCloseKey(k);
    }
}

static void DetectAll(void)
{
    g_nFound = 0;
    wchar_t s[MAX_PATH];
    if (RegStr(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", 0, s, MAX_PATH)) ScanSteamLibraries(s);
    if (RegStr(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath", KEY_WOW64_32KEY, s, MAX_PATH)) ScanSteamLibraries(s);
    if (RegStr(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath", KEY_WOW64_64KEY, s, MAX_PATH)) ScanSteamLibraries(s);
    const REGSAM views[2] = { KEY_WOW64_64KEY, KEY_WOW64_32KEY };
    for (int v = 0; v < 2; v++)   // Steam's own uninstall entry for Hello Neighbor (app 521890)
        if (RegStr(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Steam App 521890", L"InstallLocation", views[v], s, MAX_PATH))
            AddCandidate(s, L"Steam");
    ScanEpic();
    ScanRegistryTree(HKEY_LOCAL_MACHINE, L"SOFTWARE\\GOG.com\\Games", L"path", L"GOG");
    ScanRegistryTree(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", L"InstallLocation", L"installed programs");
    ScanRegistryTree(HKEY_CURRENT_USER,  L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", L"InstallLocation", L"installed programs");
    // common folders on every fixed drive
    DWORD drives = GetLogicalDrives();
    const wchar_t *rel[] = { L"Program Files (x86)\\Steam\\steamapps\\common\\Hello Neighbor", L"Program Files\\Steam\\steamapps\\common\\Hello Neighbor",
                             L"Steam\\steamapps\\common\\Hello Neighbor", L"SteamLibrary\\steamapps\\common\\Hello Neighbor",
                             L"Games\\Steam\\steamapps\\common\\Hello Neighbor", L"Games\\SteamLibrary\\steamapps\\common\\Hello Neighbor",
                             L"Program Files\\Epic Games\\HelloNeighbor", L"Epic Games\\HelloNeighbor", L"GOG Games\\Hello Neighbor",
                             L"Games\\Hello Neighbor" };
    for (int d = 2; d < 26; d++) {
        if (!(drives & (1u << d))) continue;
        wchar_t r[8]; swprintf_s(r, 8, L"%c:\\", L'A' + d);
        if (GetDriveTypeW(r) != DRIVE_FIXED) continue;
        for (int i = 0; i < (int)(sizeof rel / sizeof rel[0]); i++) { swprintf_s(s, MAX_PATH, L"%c:\\%s", L'A' + d, rel[i]); AddCandidate(s, L"common folder"); }
    }
}

// ------------------------------------------------------------------ state of a folder
// Our DLL contains the UTF-16 string "HNTweaksOverlay" (all builds) and, from 1.0.0 on, "; Hello Neighbor Tweaks <ver>".
typedef struct { int present, ours; char ver[32]; } DllInfo;
static DllInfo InspectDll(const wchar_t *path)
{
    DllInfo r = {0};
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return r;
    r.present = 1;
    DWORD n = GetFileSize(h, NULL), got = 0;
    unsigned char *b = (n != INVALID_FILE_SIZE && n < (64u << 20)) ? (unsigned char *)HeapAlloc(GetProcessHeap(), 0, n + 1) : NULL;
    if (b) {
        ReadFile(h, b, n, &got, NULL); b[got] = 0;
        static const wchar_t mark[] = L"HNTweaksOverlay";
        const char *tag = "; Hello Neighbor Tweaks ";
        size_t ml = sizeof mark - sizeof(wchar_t), tl = strlen(tag);
        for (DWORD i = 0; i + ml <= got && !r.ours; i++) if (b[i] == 'H' && !memcmp(b + i, mark, ml)) r.ours = 1;
        for (DWORD i = 0; i + tl < got; i++) if (b[i] == ';' && !memcmp(b + i, tag, tl)) {
            int k = 0; const unsigned char *v = b + i + tl;
            while (k < 31 && ((v[k] >= '0' && v[k] <= '9') || v[k] == '.')) { r.ver[k] = (char)v[k]; k++; }
            r.ver[k] = 0; break;
        }
        HeapFree(GetProcessHeap(), 0, b);
    }
    CloseHandle(h);
    return r;
}

static int GameRunning(void)
{
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = { sizeof pe }; int found = 0;
    if (Process32FirstW(s, &pe)) do {
        if (!_wcsicmp(pe.szExeFile, EXE_NAME) || !_wcsicmp(pe.szExeFile, L"HelloNeighbor.exe")) found = 1;
    } while (!found && Process32NextW(s, &pe));
    CloseHandle(s);
    return found;
}

static int IsElevated(void)
{
    HANDLE t; TOKEN_ELEVATION e; DWORD n; int r = 0;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) {
        if (GetTokenInformation(t, TokenElevation, &e, sizeof e, &n)) r = e.TokenIsElevated != 0;
        CloseHandle(t);
    }
    return r;
}

// ------------------------------------------------------------------ payload
static const void *Res(int id, DWORD *n)
{
    HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));   // RT_RCDATA
    HGLOBAL g = r ? LoadResource(NULL, r) : NULL;
    *n = r ? SizeofResource(NULL, r) : 0;
    return g ? LockResource(g) : NULL;
}
// Writes to "<path>.new" first, then swaps it in, so a failure never leaves a half-written file.
static DWORD WriteRes(int id, const wchar_t *path)
{
    DWORD n; const void *p = Res(id, &n);
    if (!p) return ERROR_RESOURCE_DATA_NOT_FOUND;
    wchar_t tmp[MAX_PATH]; swprintf_s(tmp, MAX_PATH, L"%s.new", path);
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD w = 0; BOOL ok = WriteFile(h, p, n, &w, NULL) && w == n;
    DWORD err = ok ? 0 : GetLastError();
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmp); return err ? err : ERROR_WRITE_FAULT; }
    if (!MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { err = GetLastError(); DeleteFileW(tmp); return err; }
    return 0;
}

// Can the current user (normal rights) create files in this folder?
static int CanWrite(const wchar_t *dir)
{
    wchar_t t[MAX_PATH]; swprintf_s(t, MAX_PATH, L"%s\\hnt_write_test_%lu.tmp", dir, GetCurrentProcessId());
    HANDLE h = CreateFileW(t, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    CloseHandle(h);
    return 1;
}
#define NEED_ADMIN_TEXT \
    L"This game folder can only be changed with administrator rights (it is probably under Program Files).\n" \
    L"The installer only runs with normal user rights, so please use the portable zip instead:\n" \
    L"download " ZIP_NAME L" and copy XINPUT1_3.dll from it into\n"

// ------------------------------------------------------------------ install / uninstall
static int DoInstall(const wchar_t *in)
{
    wchar_t root[MAX_PATH], w64[MAX_PATH], dll[MAX_PATH], bak[MAX_PATH], f[MAX_PATH], sys[MAX_PATH];
    if (!ResolveGameRoot(in, root)) {
        Say(MB_ICONERROR, L"This is not a Hello Neighbor folder:\n%s\n\nPick the folder that contains HelloNeighbor\\Binaries\\Win64\\" EXE_NAME L".", in);
        return RC_NOTGAME;
    }
    if (GameRunning()) { Say(MB_ICONWARNING, L"Hello Neighbor is running. Close the game, then click Install again."); return RC_RUNNING; }
    Win64Dir(root, w64); Join(dll, w64, DLL_NAME);
    swprintf_s(bak, MAX_PATH, L"%s" BACKUP_EXT, dll);
    if (!CanWrite(w64)) { Say(MB_ICONWARNING, NEED_ADMIN_TEXT L"%s", w64); return RC_DENIED; }

    // another XINPUT1_3.dll (another mod / wrapper): keep it as a backup, restored by Uninstall
    DllInfo cur = InspectDll(dll);
    if (cur.present && !cur.ours && !FileExists(bak)) {
        if (!MoveFileExW(dll, bak, 0)) {
            DWORD e = GetLastError();
            if (e == ERROR_ACCESS_DENIED) { Say(MB_ICONWARNING, NEED_ADMIN_TEXT L"%s", w64); return RC_DENIED; }
            Say(MB_ICONERROR, L"Could not back up the existing %s (error %lu).", DLL_NAME, e); return RC_FAIL;
        }
    }
    DWORD e = WriteRes(IDR_DLL, dll);
    if (e == ERROR_ACCESS_DENIED) { Say(MB_ICONWARNING, NEED_ADMIN_TEXT L"%s", w64); return RC_DENIED; }
    if (e) {
        Say(MB_ICONERROR, L"Could not write %s (error %lu).\n%s", dll, e,
            e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION || e == ERROR_USER_MAPPED_FILE ? L"The file is in use - close the game and try again." : L"");
        return RC_FAIL;
    }
    Join(f, w64, README_OUT);  WriteRes(IDR_README, f);
    Join(f, w64, LICENSE_OUT); WriteRes(IDR_LICENSE, f);
    Join(f, w64, SOURCE_OUT);  WriteRes(IDR_SOURCE, f);

    {
        GetSystemDirectoryW(sys, MAX_PATH); Join(f, sys, L"XInput1_3.dll");
        Join(bak, w64, CFG_NAME);
        Say(MB_ICONINFORMATION,
            L"Hello Neighbor Tweaks %s is installed in:\n%s\n\n"
            L"Start the game and press F1 for the settings menu.\n"
            L"%s%s",
            HNT_VERSION_W, w64,
            FileExists(bak) ? L"Your existing settings (" CFG_NAME L") were kept.\n" : L"The settings file is created on the first start.\n",
            FileExists(f) ? L"" : L"\nWarning: " L"XInput1_3.dll is missing from Windows. Install the DirectX End-User Runtime (June 2010), or the game will not start.");
    }
    return RC_OK;
}

// Puts back the Engine.ini / Input.ini lines the DLL changed (recorded in the settings file's [State] section).
static void RevertGameConfig(const wchar_t *cfg)
{
    wchar_t la[MAX_PATH], dir[MAX_PATH], eng[MAX_PATH], inp[MAX_PATH], was[64];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH)) return;
    swprintf_s(dir, MAX_PATH, L"%s\\HelloNeighbor\\Saved\\Config\\WindowsNoEditor", la);
    Join(eng, dir, L"Engine.ini"); Join(inp, dir, L"Input.ini");
    struct { const wchar_t *mark, *file, *sec, *key; } m[2] = {
        { L"WroteAnisotropy",     eng, L"SystemSettings",               L"r.MaxAnisotropy" },
        { L"WroteMouseSmoothing", inp, L"/Script/Engine.InputSettings", L"bEnableMouseSmoothing" } };
    for (int i = 0; i < 2; i++) {
        GetPrivateProfileStringW(L"State", m[i].mark, L"", was, 64, cfg);
        if (!was[0]) continue;
        if (FileExists(m[i].file)) WritePrivateProfileStringW(m[i].sec, m[i].key, wcscmp(was, L"-") ? was : NULL, m[i].file);
        WritePrivateProfileStringW(L"State", m[i].mark, NULL, cfg);
    }
}

static int DoUninstall(const wchar_t *in, int removeSettings)
{
    wchar_t root[MAX_PATH], w64[MAX_PATH], dll[MAX_PATH], bak[MAX_PATH], f[MAX_PATH];
    if (!ResolveGameRoot(in, root)) { Say(MB_ICONERROR, L"This is not a Hello Neighbor folder:\n%s", in); return RC_NOTGAME; }
    if (GameRunning()) { Say(MB_ICONWARNING, L"Hello Neighbor is running. Close the game, then click Uninstall again."); return RC_RUNNING; }
    Win64Dir(root, w64); Join(dll, w64, DLL_NAME);
    swprintf_s(bak, MAX_PATH, L"%s" BACKUP_EXT, dll);
    DllInfo cur = InspectDll(dll);
    if (cur.present && !cur.ours) {
        Say(MB_ICONWARNING, L"The %s in this folder is not Hello Neighbor Tweaks, so it was left alone.", DLL_NAME);
        return RC_FAIL;
    }
    if (!CanWrite(w64)) {
        Say(MB_ICONWARNING, L"This game folder can only be changed with administrator rights, and the installer only runs with "
                            L"normal user rights. To uninstall, delete %s (and HelloNeighborTweaks.ini / .log) from:\n%s", DLL_NAME, w64);
        return RC_DENIED;
    }
    if (cur.present && !DeleteFileW(dll)) {
        DWORD e = GetLastError();
        Say(MB_ICONERROR, L"Could not remove %s (error %lu).%s", dll, e,
            e == ERROR_SHARING_VIOLATION || e == ERROR_ACCESS_DENIED ? L"\nClose the game and try again." : L"");
        return RC_FAIL;
    }
    if (FileExists(bak)) MoveFileExW(bak, dll, 0);   // the DLL that was there before us
    Join(f, w64, README_OUT);  DeleteFileW(f);
    Join(f, w64, LICENSE_OUT); DeleteFileW(f);
    Join(f, w64, SOURCE_OUT);  DeleteFileW(f);
    Join(f, w64, CFG_NAME);    RevertGameConfig(f);
    if (removeSettings) { DeleteFileW(f); Join(f, w64, LOG_NAME); DeleteFileW(f); }
    else { Join(f, w64, LOG_NAME); DeleteFileW(f); }
    Say(MB_ICONINFORMATION, L"Hello Neighbor Tweaks was removed.%s",
            removeSettings ? L"" : L"\nYour settings file (" CFG_NAME L") was kept for a later reinstall.");
    return RC_OK;
}

// ------------------------------------------------------------------ dialog
static HWND g_dlg;

static void GetPathText(wchar_t *out) { GetDlgItemTextW(g_dlg, IDC_PATH, out, MAX_PATH); StripSlash(out); }

static void UpdateStatus(void)
{
    wchar_t in[MAX_PATH], root[MAX_PATH], w64[MAX_PATH], f[MAX_PATH], t[1600];
    GetPathText(in);
    int ok = ResolveGameRoot(in, root);
    EnableWindow(GetDlgItem(g_dlg, IDC_INSTALL), ok);
    if (!ok) {
        EnableWindow(GetDlgItem(g_dlg, IDC_UNINSTALL), FALSE);
        SetDlgItemTextW(g_dlg, IDC_STATUS, in[0]
            ? L"This folder is not a Hello Neighbor install.\nPick the game's folder (the one that contains HelloNeighbor\\Binaries\\Win64\\" EXE_NAME L")."
            : L"Hello Neighbor was not found automatically. Click Browse... and pick the game's folder.\n(Steam: right-click the game > Manage > Browse local files.)");
        return;
    }
    const wchar_t *src = NULL;
    for (int i = 0; i < g_nFound; i++) if (!_wcsicmp(g_found[i].root, root)) src = g_found[i].src;
    Win64Dir(root, w64); Join(f, w64, DLL_NAME);
    DllInfo d = InspectDll(f);
    wchar_t ver[40] = L""; if (d.ver[0]) MultiByteToWideChar(CP_UTF8, 0, d.ver, -1, ver, 40);
    Join(f, w64, CFG_NAME); int haveCfg = FileExists(f);
    int len = swprintf_s(t, 1600, L"Hello Neighbor found%s%s%s.\n\n", src ? L" (" : L"", src ? src : L"", src ? L")" : L"");
    if (!d.present)      len += swprintf_s(t + len, 1600 - len, L"The mod is not installed yet. Install copies " DLL_NAME L" and its readme into the game's Win64 folder.");
    else if (!d.ours)    len += swprintf_s(t + len, 1600 - len, L"Another " DLL_NAME L" is in the game folder (probably another mod). Install keeps it as " DLL_NAME BACKUP_EXT L" and Uninstall puts it back.");
    else if (ver[0])     len += swprintf_s(t + len, 1600 - len, L"Hello Neighbor Tweaks %s is installed. Install replaces it with %s.", ver, HNT_VERSION_W);
    else                 len += swprintf_s(t + len, 1600 - len, L"An earlier build of Hello Neighbor Tweaks is installed. Install updates it to %s.", HNT_VERSION_W);
    static wchar_t lastDir[MAX_PATH]; static int lastWritable;   // checked once per folder, not on every timer tick
    if (_wcsicmp(lastDir, w64)) { wcscpy_s(lastDir, MAX_PATH, w64); lastWritable = CanWrite(w64); }
    if (!lastWritable) {   // never elevate: point to the zip instead
        swprintf_s(t, 1600, L"Hello Neighbor found%s%s%s, but this folder can only be changed with administrator rights "
                            L"(it is probably under Program Files).\n\nThe installer only runs with normal user rights. "
                            L"Please download the portable zip (" ZIP_NAME L") instead and copy XINPUT1_3.dll from it into:\n%s",
                   src ? L" (" : L"", src ? src : L"", src ? L")" : L"", w64);
        SetDlgItemTextW(g_dlg, IDC_STATUS, t);
        EnableWindow(GetDlgItem(g_dlg, IDC_INSTALL), FALSE);
        EnableWindow(GetDlgItem(g_dlg, IDC_UNINSTALL), FALSE);
        return;
    }
    if (haveCfg) len += swprintf_s(t + len, 1600 - len, L"\nYour settings (" CFG_NAME L") are kept.");
    if (GameRunning()) len += swprintf_s(t + len, 1600 - len, L"\n\nThe game is running - close it before installing.");
    SetDlgItemTextW(g_dlg, IDC_STATUS, t);
    SetDlgItemTextW(g_dlg, IDC_INSTALL, d.present && d.ours ? L"Update" : L"Install");
    EnableWindow(GetDlgItem(g_dlg, IDC_UNINSTALL), d.present && d.ours);
}

static void Browse(void)
{
    BROWSEINFOW bi = {0}; wchar_t p[MAX_PATH];
    bi.hwndOwner = g_dlg; bi.lpszTitle = L"Pick the Hello Neighbor folder (the one that contains \"HelloNeighbor\" and \"Engine\").";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&bi);
    if (!id) return;
    if (SHGetPathFromIDListW(id, p)) { wchar_t r[MAX_PATH]; SetDlgItemTextW(g_dlg, IDC_PATH, ResolveGameRoot(p, r) ? r : p); }
    CoTaskMemFree(id);
    UpdateStatus();
}

static void ShowReadme(void)
{
    wchar_t tmp[MAX_PATH], f[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp); swprintf_s(f, MAX_PATH, L"%sHelloNeighborTweaks_README.txt", tmp);
    if (!WriteRes(IDR_README, f)) ShellExecuteW(g_dlg, L"open", f, NULL, NULL, SW_SHOWNORMAL);
}

static INT_PTR CALLBACK DlgProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    (void)l;
    switch (m) {
    case WM_INITDIALOG: {
        g_dlg = h;
        SetWindowTextW(h, TITLE);
        HICON ic = LoadIconW(NULL, IDI_APPLICATION);
        SendMessageW(h, WM_SETICON, ICON_BIG, (LPARAM)ic); SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)ic);
        DetectAll();
        HWND cb = GetDlgItem(h, IDC_PATH);
        for (int i = 0; i < g_nFound; i++) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)g_found[i].root);
        if (g_nFound) SendMessageW(cb, CB_SETCURSEL, 0, 0);
        UpdateStatus();
        SetTimer(h, 1, 1500, NULL);   // notices the game being closed / started
        return TRUE;
    }
    case WM_TIMER: UpdateStatus(); return TRUE;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDC_PATH:
            if (HIWORD(w) == CBN_EDITCHANGE) UpdateStatus();
            if (HIWORD(w) == CBN_SELCHANGE) PostMessageW(h, WM_TIMER, 1, 0);   // the edit text updates after this message
            return TRUE;
        case IDC_BROWSE: Browse(); return TRUE;
        case IDC_README: ShowReadme(); return TRUE;
        case IDC_INSTALL: {
            wchar_t p[MAX_PATH]; GetPathText(p);
            DoInstall(p);
            UpdateStatus();
            return TRUE;
        }
        case IDC_UNINSTALL: {
            wchar_t p[MAX_PATH]; GetPathText(p);
            int r = MessageBoxW(h, L"Remove Hello Neighbor Tweaks from this game folder?\n\n"
                                   L"Yes = remove the mod and keep your settings file\n"
                                   L"No = remove the mod and its settings file too\n"
                                   L"Cancel = do nothing", TITLE, MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r != IDCANCEL) DoUninstall(p, r == IDNO);
            UpdateStatus();
            return TRUE;
        }
        case IDCANCEL: EndDialog(h, 0); return TRUE;
        }
        break;
    }
    return FALSE;
}

// ------------------------------------------------------------------ entry
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)prev; (void)cmd; (void)show;
    int argc = 0; wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const wchar_t *action = NULL, *path = NULL; int removeSettings = 0;
    for (int i = 1; argv && i < argc; i++) {
        if (!_wcsicmp(argv[i], L"/quiet")) g_quiet = 1;
        else if (!_wcsicmp(argv[i], L"/removesettings")) removeSettings = 1;
        else if (argv[i][0] == L'/') action = argv[i];
        else path = argv[i];
    }
    // normal user rights only (see the top of the file)
    if (IsElevated() && !(action && !_wcsicmp(action, L"/detect"))) {
        Say(MB_ICONWARNING, L"Please run this installer normally, without \"Run as administrator\".\n\n"
                            L"It only works with normal user rights. If Windows runs every program as administrator on this PC, "
                            L"use the portable zip (" ZIP_NAME L") instead: copy XINPUT1_3.dll from it next to " EXE_NAME L".");
        return RC_ELEVATED;
    }
    if (action && !_wcsicmp(action, L"/detect")) {
        DetectAll();
        wchar_t t[MAX_PATH + 64];
        for (int i = 0; i < g_nFound; i++) { swprintf_s(t, MAX_PATH + 64, L"%s\t(%s)\r\n", g_found[i].root, g_found[i].src); Out(t); }
        if (!g_nFound) Out(L"Hello Neighbor not found\r\n");
        return g_nFound ? RC_OK : RC_NOTGAME;
    }
    if (action && !_wcsicmp(action, L"/install"))   return DoInstall(path ? path : L"");
    if (action && !_wcsicmp(action, L"/uninstall")) return DoUninstall(path ? path : L"", removeSettings);

    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_MAIN), NULL, DlgProc, 0);
    CoUninitialize();
    return 0;
}
