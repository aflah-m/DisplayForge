# DisplayForge — Per-Game Display Profiles

Launch any Steam game with its own resolution and refresh rate, automatically
switched on launch and restored when you're done.

Built for cases like ultrawide monitors, TVs limited to specific refresh
rates on certain games, or older titles that only look right at a non-native
resolution — set it once per game, then just double-click a shortcut to play.

## How it works

DisplayForge is two small native Windows programs:

- **Configure.exe** — a GUI where you pick a game's Steam AppID, its game
  `.exe` (used to detect when it's running), and a resolution + refresh rate
  from what your monitor actually supports. Saving a profile creates:
  - `Games\<name>.ini` — the saved settings
  - `Games\<name>.lnk` — a shortcut you actually play from
- **Launcher.exe** — runs silently behind that shortcut. It switches your
  display to the profile's resolution/refresh rate, launches the game via
  Steam, waits for it to close, then restores your original display mode.

You never run Launcher.exe directly — Configure.exe bakes the right
command-line arguments into each shortcut automatically.

## Setup

1. Build `Configure.exe` and `Launcher.exe` from the source (see
   **Building** below), or download them from the latest GitHub Actions run.
2. Put both `.exe` files in the same folder.
3. Run `Configure.exe`.
4. Fill in:
   - **Steam AppID** — found in the game's Steam store URL
     (`store.steampowered.com/app/<AppID>/...`)
   - **Game .exe** — browse to the game's actual executable, used only to
     detect when it's running/closed
   - **Resolution** / **Refresh rate** — pulled live from what Windows
     reports your display currently supports
   - **Profile name** — used as the shortcut's filename
5. Click **Save Profile**. A shortcut appears in the `Games` folder next to
   `Configure.exe` — that's what you double-click to play.

## Building

Requires MSVC (`cl.exe`), e.g. from a free Visual Studio Community install
with the "Desktop development with C++" workload, run from an
**x64 Native Tools Command Prompt**:

```
cl /O2 /DUNICODE /D_UNICODE configure_app.c user32.lib comdlg32.lib ole32.lib gdi32.lib /Fe:Configure.exe
cl /O2 /DUNICODE /D_UNICODE launcher_app.c user32.lib shell32.lib gdi32.lib /Fe:Launcher.exe
```

Or let GitHub Actions build it for you — this repo includes
`.github/workflows/build.yml`, which compiles both `.exe` files on a
Windows runner and uploads them as a downloadable build artifact on every
push, with no local install required.

## Notes

- Both programs are self-contained Win32 C — no runtime dependencies beyond
  standard Windows DLLs.
- If a saved resolution/refresh combination stops being available (e.g. a
  custom mode set via CRU gets removed), Launcher.exe will show an error and
  leave your display mode untouched rather than guessing.
- Your original display mode is always restored when the game closes, even
  if Steam relaunches the game under a new process ID during its own
  handoff.
- The resolution/refresh switch is applied via `CDS_UPDATEREGISTRY`, which
  registers it as the system's actual current mode rather than a throwaway
  runtime override. This is intentional — it's what keeps the display stable
  through exclusive-fullscreen edge cases like Task View / virtual-desktop
  switches and controller-connect events, which would otherwise cause
  Windows to silently fall back to the old resolution mid-game.
  - **Trade-off:** if `Launcher.exe` is ever killed abnormally before it
    reaches the restore step (Task Manager "End task", crash, power loss,
    BSOD) rather than the game closing normally, your display can be left
    on the game's resolution/refresh rate — including across a reboot,
    since it's now the registered default rather than a session-only
    setting. If that happens, just reopen Windows Display Settings and pick
    your normal resolution again; nothing is damaged, it just won't
    self-correct on its own in that specific failure case.
- Recommended to keep the game itself in exclusive fullscreen (not
  borderless/windowed) for best performance/latency — this tool is built
  around and tested against that mode.
