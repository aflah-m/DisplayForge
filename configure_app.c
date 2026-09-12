// Configure.exe
//
// Small GUI for managing game profiles. Each profile stores:
//   - Steam AppID
//   - The game's process exe name (for detecting when it's running/closed)
//   - A resolution + refresh rate, picked from what Windows currently
//     reports as available (so only real, working modes can be chosen)
//
// Saving a profile writes two files into a "Games" folder next to this exe:
//   Games\<name>.ini  - the settings, so this program can re-load them later
//   Games\<name>.lnk  - a real Windows shortcut that runs Launcher.exe with
//                       those settings baked in as command-line arguments.
//                       This is the file the user actually double-clicks to
//                       play -- it opens no window and needs no lookup.

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <commdlg.h>
#include <stdio.h>
#include <wchar.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comdlg32.lib")

// Control IDs
#define ID_EDIT_APPID       1001
#define ID_EDIT_GAMEPATH    1002
#define ID_BTN_BROWSE       1003
#define ID_COMBO_RES        1004
#define ID_COMBO_HZ         1005
#define ID_EDIT_PROFILENAME 1006
#define ID_BTN_SAVE         1007
#define ID_LIST_PROFILES    1008
#define ID_BTN_LOAD         1009
#define ID_BTN_DELETE       1010
#define ID_BTN_NEW          1011

static wchar_t g_exeDir[MAX_PATH];
static wchar_t g_gamesDir[MAX_PATH];
static wchar_t g_launcherPath[MAX_PATH];

static HWND g_hEditAppId, g_hEditGamePath, g_hComboRes, g_hComboHz;
static HWND g_hEditProfileName, g_hListProfiles;

static void ShowErr(HWND parent, const wchar_t *msg) {
    MessageBoxW(parent, msg, L"Configure", MB_OK | MB_ICONERROR);
}

static void ShowInfo(HWND parent, const wchar_t *msg) {
    MessageBoxW(parent, msg, L"Configure", MB_OK | MB_ICONINFORMATION);
}

// ---------------------------------------------------------------------
// Resolution / refresh rate enumeration
// ---------------------------------------------------------------------

static void PopulateResolutionCombo(HWND combo) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);

    DEVMODEW dm;
    for (int i = 0; ; i++) {
        ZeroMemory(&dm, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsW(NULL, i, &dm)) break;

        if (dm.dmBitsPerPel != 32) continue; // skip low-color duplicates

        wchar_t buf[64];
        swprintf(buf, 64, L"%lux%lu", dm.dmPelsWidth, dm.dmPelsHeight);

        if (SendMessageW(combo, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)buf) == CB_ERR) {
            SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)buf);
        }
    }
}

static void PopulateRefreshCombo(HWND resCombo, HWND hzCombo) {
    SendMessageW(hzCombo, CB_RESETCONTENT, 0, 0);

    wchar_t resBuf[64];
    int sel = (int)SendMessageW(resCombo, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) return;
    SendMessageW(resCombo, CB_GETLBTEXT, sel, (LPARAM)resBuf);

    DWORD selW = 0, selH = 0;
    swscanf(resBuf, L"%lux%lu", &selW, &selH);

    DEVMODEW dm;
    for (int i = 0; ; i++) {
        ZeroMemory(&dm, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsW(NULL, i, &dm)) break;

        if (dm.dmBitsPerPel != 32) continue;
        if (dm.dmPelsWidth != selW || dm.dmPelsHeight != selH) continue;

        wchar_t buf[16];
        swprintf(buf, 16, L"%lu", dm.dmDisplayFrequency);

        if (SendMessageW(hzCombo, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)buf) == CB_ERR) {
            SendMessageW(hzCombo, CB_ADDSTRING, 0, (LPARAM)buf);
        }
    }

    if (SendMessageW(hzCombo, CB_GETCOUNT, 0, 0) > 0) {
        SendMessageW(hzCombo, CB_SETCURSEL, 0, 0);
    }
}

// Make sure a specific value exists in a combo (used when loading a saved
// profile whose exact mode might not currently be listed, e.g. a CRU mode).
// Adds it if missing, then selects it.
static void SelectOrAddCombo(HWND combo, const wchar_t *text) {
    int idx = (int)SendMessageW(combo, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)text);
    if (idx == CB_ERR) {
        idx = (int)SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)text);
    }
    SendMessageW(combo, CB_SETCURSEL, idx, 0);
}

// ---------------------------------------------------------------------
// Profile list (Games\*.ini)
// ---------------------------------------------------------------------

static void RefreshProfileList(void) {
    SendMessageW(g_hListProfiles, LB_RESETCONTENT, 0, 0);

    wchar_t pattern[MAX_PATH];
    swprintf(pattern, MAX_PATH, L"%ls\\*.ini", g_gamesDir);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        wchar_t name[MAX_PATH];
        wcsncpy(name, fd.cFileName, MAX_PATH - 1);
        wchar_t *dot = wcsrchr(name, L'.');
        if (dot) *dot = L'\0';
        SendMessageW(g_hListProfiles, LB_ADDSTRING, 0, (LPARAM)name);
    } while (FindNextFileW(h, &fd));

    FindClose(h);
}

static BOOL CreateShortcut(const wchar_t *lnkPath, const wchar_t *targetPath,
                           const wchar_t *args, const wchar_t *iconPath, const wchar_t *workDir,
                           HRESULT *outErr, int *outStage) {
    IShellLinkW *psl = NULL;
    IPersistFile *ppf = NULL;
    BOOL ok = FALSE;
    HRESULT hr;

    *outErr = S_OK;
    *outStage = 0;

    hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IShellLinkW, (void **)&psl);
    if (FAILED(hr)) {
        *outErr = hr;
        *outStage = 1;
        return FALSE;
    }

    IShellLinkW_SetPath(psl, targetPath);
    IShellLinkW_SetArguments(psl, args);
    IShellLinkW_SetWorkingDirectory(psl, workDir);
    if (iconPath && iconPath[0]) {
        IShellLinkW_SetIconLocation(psl, iconPath, 0);
    }

    hr = IShellLinkW_QueryInterface(psl, &IID_IPersistFile, (void **)&ppf);
    if (FAILED(hr)) {
        *outErr = hr;
        *outStage = 2;
        IShellLinkW_Release(psl);
        return FALSE;
    }

    hr = IPersistFile_Save(ppf, lnkPath, TRUE);
    if (FAILED(hr)) {
        *outErr = hr;
        *outStage = 3;
    } else {
        ok = TRUE;
    }

    IPersistFile_Release(ppf);
    IShellLinkW_Release(psl);

    return ok;
}

static void OnSave(HWND hwnd) {
    wchar_t appId[64], gamePath[MAX_PATH], profileName[128];
    wchar_t resBuf[64], hzBuf[16];

    GetWindowTextW(g_hEditAppId, appId, 64);
    GetWindowTextW(g_hEditGamePath, gamePath, MAX_PATH);
    GetWindowTextW(g_hEditProfileName, profileName, 128);

    int resSel = (int)SendMessageW(g_hComboRes, CB_GETCURSEL, 0, 0);
    int hzSel = (int)SendMessageW(g_hComboHz, CB_GETCURSEL, 0, 0);

    if (appId[0] == L'\0') {
        ShowErr(hwnd, L"Enter the Steam AppID for this game.");
        return;
    }
    if (gamePath[0] == L'\0') {
        ShowErr(hwnd, L"Browse to the game's exe (used to detect when it's running).");
        return;
    }
    if (resSel == CB_ERR || hzSel == CB_ERR) {
        ShowErr(hwnd, L"Pick a resolution and refresh rate.");
        return;
    }
    if (profileName[0] == L'\0') {
        ShowErr(hwnd, L"Enter a profile name (used as the shortcut's filename).");
        return;
    }

    SendMessageW(g_hComboRes, CB_GETLBTEXT, resSel, (LPARAM)resBuf);
    SendMessageW(g_hComboHz, CB_GETLBTEXT, hzSel, (LPARAM)hzBuf);

    // Process exe name = filename portion of gamePath
    wchar_t processName[MAX_PATH];
    const wchar_t *slash = wcsrchr(gamePath, L'\\');
    wcsncpy(processName, slash ? slash + 1 : gamePath, MAX_PATH - 1);

    CreateDirectoryW(g_gamesDir, NULL);

    wchar_t iniPath[MAX_PATH], lnkPath[MAX_PATH];
    swprintf(iniPath, MAX_PATH, L"%ls\\%ls.ini", g_gamesDir, profileName);
    swprintf(lnkPath, MAX_PATH, L"%ls\\%ls.lnk", g_gamesDir, profileName);

    WritePrivateProfileStringW(L"Profile", L"AppID", appId, iniPath);
    WritePrivateProfileStringW(L"Profile", L"GamePath", gamePath, iniPath);
    WritePrivateProfileStringW(L"Profile", L"ProcessName", processName, iniPath);
    WritePrivateProfileStringW(L"Profile", L"Resolution", resBuf, iniPath);
    WritePrivateProfileStringW(L"Profile", L"RefreshHz", hzBuf, iniPath);

    wchar_t args[512];
    swprintf(args, 512, L"--appid %ls --process \"%ls\" --res %ls --hz %ls",
             appId, processName, resBuf, hzBuf);

    HRESULT shortcutErr;
    int shortcutStage;
    if (!CreateShortcut(lnkPath, g_launcherPath, args, gamePath, g_exeDir, &shortcutErr, &shortcutStage)) {
        wchar_t buf[512];
        const wchar_t *stageName =
            shortcutStage == 1 ? L"CoCreateInstance (ShellLink)" :
            shortcutStage == 2 ? L"QueryInterface (IPersistFile)" :
            shortcutStage == 3 ? L"IPersistFile::Save" : L"unknown";
        swprintf(buf, 512,
                 L"Settings were saved, but creating the shortcut failed.\n\n"
                 L"Stage: %ls\nHRESULT: 0x%08lX\n\n"
                 L"Target: %ls\nShortcut path: %ls",
                 stageName, (unsigned long)shortcutErr, g_launcherPath, lnkPath);
        ShowErr(hwnd, buf);
    } else {
        ShowInfo(hwnd, L"Profile saved. A shortcut was created in the Games folder -- "
                       L"double-click that shortcut to play.");
    }

    RefreshProfileList();
}

static void OnLoad(HWND hwnd) {
    int sel = (int)SendMessageW(g_hListProfiles, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR) {
        ShowErr(hwnd, L"Select a profile from the list first.");
        return;
    }

    wchar_t name[128];
    SendMessageW(g_hListProfiles, LB_GETTEXT, sel, (LPARAM)name);

    wchar_t iniPath[MAX_PATH];
    swprintf(iniPath, MAX_PATH, L"%ls\\%ls.ini", g_gamesDir, name);

    wchar_t buf[512];

    GetPrivateProfileStringW(L"Profile", L"AppID", L"", buf, 512, iniPath);
    SetWindowTextW(g_hEditAppId, buf);

    GetPrivateProfileStringW(L"Profile", L"GamePath", L"", buf, 512, iniPath);
    SetWindowTextW(g_hEditGamePath, buf);

    SetWindowTextW(g_hEditProfileName, name);

    GetPrivateProfileStringW(L"Profile", L"Resolution", L"", buf, 512, iniPath);
    SelectOrAddCombo(g_hComboRes, buf);
    PopulateRefreshCombo(g_hComboRes, g_hComboHz);

    GetPrivateProfileStringW(L"Profile", L"RefreshHz", L"", buf, 512, iniPath);
    SelectOrAddCombo(g_hComboHz, buf);
}

static void OnDelete(HWND hwnd) {
    int sel = (int)SendMessageW(g_hListProfiles, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR) {
        ShowErr(hwnd, L"Select a profile from the list first.");
        return;
    }

    wchar_t name[128];
    SendMessageW(g_hListProfiles, LB_GETTEXT, sel, (LPARAM)name);

    wchar_t msg[256];
    swprintf(msg, 256, L"Delete the profile \"%ls\"? This removes its shortcut too.", name);
    if (MessageBoxW(hwnd, msg, L"Confirm delete", MB_YESNO | MB_ICONQUESTION) != IDYES) {
        return;
    }

    wchar_t iniPath[MAX_PATH], lnkPath[MAX_PATH];
    swprintf(iniPath, MAX_PATH, L"%ls\\%ls.ini", g_gamesDir, name);
    swprintf(lnkPath, MAX_PATH, L"%ls\\%ls.lnk", g_gamesDir, name);

    DeleteFileW(iniPath);
    DeleteFileW(lnkPath);

    RefreshProfileList();
}

static void OnNew(HWND hwnd) {
    (void)hwnd;
    SetWindowTextW(g_hEditAppId, L"");
    SetWindowTextW(g_hEditGamePath, L"");
    SetWindowTextW(g_hEditProfileName, L"");
    SendMessageW(g_hComboRes, CB_SETCURSEL, (WPARAM)-1, 0);
    SendMessageW(g_hComboHz, CB_RESETCONTENT, 0, 0);
}

static void OnBrowse(HWND hwnd) {
    wchar_t path[MAX_PATH] = L"";

    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Programs (*.exe)\0*.exe\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle = L"Select the game's executable";

    if (GetOpenFileNameW(&ofn)) {
        SetWindowTextW(g_hEditGamePath, path);

        // Suggest a profile name from the exe's filename if the field is empty.
        wchar_t current[128];
        GetWindowTextW(g_hEditProfileName, current, 128);
        if (current[0] == L'\0') {
            const wchar_t *slash = wcsrchr(path, L'\\');
            wchar_t base[128];
            wcsncpy(base, slash ? slash + 1 : path, 127);
            wchar_t *dot = wcsrchr(base, L'.');
            if (dot) *dot = L'\0';
            SetWindowTextW(g_hEditProfileName, base);
        }
    }
}

// ---------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        int y = 15;

        #define LABEL(txt, yy) CreateWindowW(L"STATIC", txt, WS_CHILD | WS_VISIBLE, \
            15, yy, 150, 20, hwnd, NULL, NULL, NULL)
        #define SETFONT(h) SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE)

        SETFONT(LABEL(L"Steam AppID:", y));
        g_hEditAppId = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER,
            170, y - 2, 200, 24, hwnd, (HMENU)ID_EDIT_APPID, NULL, NULL);
        SETFONT(g_hEditAppId);
        y += 34;

        SETFONT(LABEL(L"Game .exe (for tracking):", y));
        g_hEditGamePath = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER,
            170, y - 2, 250, 24, hwnd, (HMENU)ID_EDIT_GAMEPATH, NULL, NULL);
        SETFONT(g_hEditGamePath);
        HWND browse = CreateWindowW(L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE,
            425, y - 2, 80, 24, hwnd, (HMENU)ID_BTN_BROWSE, NULL, NULL);
        SETFONT(browse);
        y += 34;

        SETFONT(LABEL(L"Resolution:", y));
        g_hComboRes = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            170, y - 2, 150, 200, hwnd, (HMENU)ID_COMBO_RES, NULL, NULL);
        SETFONT(g_hComboRes);
        y += 34;

        SETFONT(LABEL(L"Refresh rate (Hz):", y));
        g_hComboHz = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            170, y - 2, 150, 200, hwnd, (HMENU)ID_COMBO_HZ, NULL, NULL);
        SETFONT(g_hComboHz);
        y += 34;

        SETFONT(LABEL(L"Profile name:", y));
        g_hEditProfileName = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER,
            170, y - 2, 200, 24, hwnd, (HMENU)ID_EDIT_PROFILENAME, NULL, NULL);
        SETFONT(g_hEditProfileName);
        y += 40;

        HWND saveBtn = CreateWindowW(L"BUTTON", L"Save Profile", WS_CHILD | WS_VISIBLE,
            15, y, 120, 30, hwnd, (HMENU)ID_BTN_SAVE, NULL, NULL);
        SETFONT(saveBtn);
        HWND newBtn = CreateWindowW(L"BUTTON", L"New / Clear", WS_CHILD | WS_VISIBLE,
            145, y, 120, 30, hwnd, (HMENU)ID_BTN_NEW, NULL, NULL);
        SETFONT(newBtn);
        y += 45;

        SETFONT(LABEL(L"Saved profiles:", y));
        y += 24;
        g_hListProfiles = CreateWindowW(L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | LBS_NOTIFY,
            15, y, 200, 100, hwnd, (HMENU)ID_LIST_PROFILES, NULL, NULL);
        SETFONT(g_hListProfiles);

        HWND loadBtn = CreateWindowW(L"BUTTON", L"Load", WS_CHILD | WS_VISIBLE,
            225, y, 90, 30, hwnd, (HMENU)ID_BTN_LOAD, NULL, NULL);
        SETFONT(loadBtn);
        HWND delBtn = CreateWindowW(L"BUTTON", L"Delete", WS_CHILD | WS_VISIBLE,
            225, y + 35, 90, 30, hwnd, (HMENU)ID_BTN_DELETE, NULL, NULL);
        SETFONT(delBtn);

        PopulateResolutionCombo(g_hComboRes);
        RefreshProfileList();
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wp);
        int code = HIWORD(wp);

        if (id == ID_COMBO_RES && code == CBN_SELCHANGE) {
            PopulateRefreshCombo(g_hComboRes, g_hComboHz);
        } else if (id == ID_BTN_BROWSE && code == BN_CLICKED) {
            OnBrowse(hwnd);
        } else if (id == ID_BTN_SAVE && code == BN_CLICKED) {
            OnSave(hwnd);
        } else if (id == ID_BTN_LOAD && code == BN_CLICKED) {
            OnLoad(hwnd);
        } else if (id == ID_BTN_DELETE && code == BN_CLICKED) {
            OnDelete(hwnd);
        } else if (id == ID_BTN_NEW && code == BN_CLICKED) {
            OnNew(hwnd);
        }
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrevInst, PWSTR cmdLine, int nCmdShow) {
    (void)hPrevInst; (void)cmdLine;

    CoInitialize(NULL);

    GetModuleFileNameW(NULL, g_exeDir, MAX_PATH);
    wchar_t *slash = wcsrchr(g_exeDir, L'\\');
    if (slash) *slash = L'\0';

    swprintf(g_gamesDir, MAX_PATH, L"%ls\\Games", g_exeDir);
    swprintf(g_launcherPath, MAX_PATH, L"%ls\\Launcher.exe", g_exeDir);

    const wchar_t *cls = L"ConfigureWindowClass";
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = cls;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowW(cls, L"Game Launcher - Configure",
        WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, 550, 420,
        NULL, NULL, hInst, NULL);

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return 0;
}
