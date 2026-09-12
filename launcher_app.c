// Launcher.exe
//
// Silent game launcher. Takes all settings as command-line arguments (these
// get baked into a per-game shortcut by Configure.exe), so this program
// itself never shows a window.
//
// Usage (as embedded in a shortcut's "Target" arguments):
//   --appid 1174180 --process "RDR2.exe" --res 1920x850 --hz 40
//
// Behavior:
//   1. Save the current display mode.
//   2. Switch to the requested resolution/refresh rate.
//   3. Launch the game via Steam's own protocol (steam://rungameid/<appid>),
//      matching how Steam itself launches a game from your library.
//   4. Wait for the named process to appear, then wait for it to exit
//      (tolerating a relaunch under a new PID during Steam/launcher handoff).
//   5. Restore the original display mode.

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <wchar.h>

static const int WAIT_FOR_START_SECONDS = 180;
static const int POLL_INTERVAL_MS = 2000;
static const int RELAUNCH_GRACE_SECONDS = 6;

static void ShowErr(const wchar_t *msg) {
    MessageBoxW(NULL, msg, L"Launcher", MB_OK | MB_ICONERROR);
}

static BOOL GetCurrentMode(DEVMODEW *mode) {
    ZeroMemory(mode, sizeof(DEVMODEW));
    mode->dmSize = sizeof(DEVMODEW);
    return EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, mode);
}

static LONG SetMode(const DEVMODEW *base, DWORD width, DWORD height, DWORD hz) {
    DEVMODEW mode = *base;
    mode.dmPelsWidth = width;
    mode.dmPelsHeight = height;
    mode.dmDisplayFrequency = hz;
    mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_BITSPERPEL;

    LONG result = ChangeDisplaySettingsW(&mode, CDS_TEST);
    if (result != DISP_CHANGE_SUCCESSFUL) {
        return result;
    }
    return ChangeDisplaySettingsW(&mode, CDS_FULLSCREEN);
}

static LONG RestoreMode(const DEVMODEW *original) {
    DEVMODEW mode = *original;
    return ChangeDisplaySettingsW(&mode, CDS_FULLSCREEN);
}

static DWORD FindProcessId(const wchar_t *processName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(PROCESSENTRY32W);
    DWORD pid = 0;

    if (Process32FirstW(snap, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, processName) == 0) {
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return pid;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrevInst, PWSTR cmdLine, int nCmdShow) {
    (void)hInst; (void)hPrevInst; (void)cmdLine; (void)nCmdShow;

    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    wchar_t appId[64] = L"";
    wchar_t processName[MAX_PATH] = L"";
    DWORD resW = 0, resH = 0, hz = 0;

    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--appid") == 0 && i + 1 < argc) {
            wcsncpy(appId, argv[++i], 63);
        } else if (wcscmp(argv[i], L"--process") == 0 && i + 1 < argc) {
            wcsncpy(processName, argv[++i], MAX_PATH - 1);
        } else if (wcscmp(argv[i], L"--res") == 0 && i + 1 < argc) {
            i++;
            swscanf(argv[i], L"%lux%lu", &resW, &resH);
        } else if (wcscmp(argv[i], L"--hz") == 0 && i + 1 < argc) {
            hz = (DWORD)_wtoi(argv[++i]);
        }
    }
    if (argv) LocalFree(argv);

    if (appId[0] == L'\0' || processName[0] == L'\0' || resW == 0 || resH == 0 || hz == 0) {
        ShowErr(L"This launcher was started without proper settings.\n\n"
                L"It's meant to be run from a shortcut created by Configure.exe, "
                L"not double-clicked directly.");
        return 1;
    }

    DEVMODEW original;
    if (!GetCurrentMode(&original)) {
        ShowErr(L"Could not read the current display mode.");
        return 1;
    }

    LONG result = SetMode(&original, resW, resH, hz);
    if (result != DISP_CHANGE_SUCCESSFUL) {
        wchar_t buf[256];
        swprintf(buf, 256,
                 L"Windows rejected the %lux%lu @ %lu Hz mode.\n"
                 L"Result code: %ld\n\n"
                 L"Make sure that custom resolution still exists (e.g. via CRU).",
                 resW, resH, hz, result);
        ShowErr(buf);
        return 1;
    }

    Sleep(1000);

    wchar_t steamUri[128];
    swprintf(steamUri, 128, L"steam://rungameid/%ls", appId);

    HINSTANCE launchResult = ShellExecuteW(NULL, L"open", steamUri, NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)launchResult <= 32) {
        ShowErr(L"Failed to launch Steam. Make sure Steam is installed.");
        RestoreMode(&original);
        return 1;
    }

    DWORD pid = 0;
    int waited = 0;
    while (waited < WAIT_FOR_START_SECONDS * 1000) {
        pid = FindProcessId(processName);
        if (pid != 0) break;
        Sleep(POLL_INTERVAL_MS);
        waited += POLL_INTERVAL_MS;
    }

    if (pid == 0) {
        wchar_t buf[512];
        swprintf(buf, 512,
                 L"%ls did not start within the expected time.\n\n"
                 L"The display mode will be restored. Check that Steam and any "
                 L"game launcher opened normally.",
                 processName);
        ShowErr(buf);
        RestoreMode(&original);
        return 1;
    }

    for (;;) {
        HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (proc != NULL) {
            WaitForSingleObject(proc, INFINITE);
            CloseHandle(proc);
        }

        DWORD newPid = 0;
        int graceWaited = 0;
        while (graceWaited < RELAUNCH_GRACE_SECONDS * 1000) {
            newPid = FindProcessId(processName);
            if (newPid != 0) break;
            Sleep(500);
            graceWaited += 500;
        }

        if (newPid == 0) break;
        pid = newPid;
    }

    RestoreMode(&original);
    return 0;
}
