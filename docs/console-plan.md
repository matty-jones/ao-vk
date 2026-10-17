# An in-game console for the renderer's settings

Status (2026-10-17): phases 1-3 done on branch `console` (proxy/ddraw/rvk_console.cpp, rvk_input.cpp), tested outside
the game with randy_harness (below); not yet seen in game. Phase 4 deferred. The user wants a console to turn features on and off quickly: the tilde key
opens and closes it, Up / Down step through the history, names complete as you type, and a value set there takes
effect in the game at once.

## Today

- Every renderer option is one row of `g_settings` (proxy/ddraw/rvk_settings.cpp, ~266 rows): name (`RVK_*`, at most
  15 characters), label, section, type (bool / int / float / choice), range, step, default, parent (the feature's on /
  off switch). `rvk_settings::Set(name, v)` clamps, applies it to the device at once and saves randy-vk.ini; that is
  the whole hot-reload path, the console only has to call it. (`ApplyTemporary` / `Restore` give an unsaved value.)
- Actions that aren't settings are Ctrl+Shift hotkeys in rvk_ddraw.cpp's present path: hide the interface, free camera,
  frame dump, profiling sweep, reload the LUTs, lighting debug view, pick (I / J).
- Keeping the game's input away is solved already, by the free camera (rvk_freecam.cpp): DirectInput devices give an
  idle state, and the game's window is subclassed and gets no key or mouse messages.
- Nothing draws text yet. The interface layer (rvk/interface.cpp) shows how to put an image over the finished frame:
  a premultiplied image blended (ONE, INV_SRC_ALPHA) over the 8-bit main target, after the game's interface.
- AOReloaded's Renderer tab reads and writes the same table through the exported `RvkSettings_*` interface.

## Where it lives: randy-vk

randy-vk owns the frame (it can draw the console over everything, in any zone and on the login screen) and owns the
settings it changes. AOReloaded would have to build it from the game's GUI XML, which has no free text drawing; a
text field there can't do inline completion well. AOReloaded's own settings come in later through a small provider
interface (phase 4), so one console covers both.

## What it does

**Open / close.** The key left of 1 (scan code 0x29: `~` / `` ` `` on US layouts, `½` / `^` / `ö` elsewhere - the
position, not the character, so it works on every layout). Esc also closes it. While open, the game gets no keyboard
or mouse input (the free camera's blocking, shared); the key press that opened it is eaten so it never reaches the
chat. The key is set in randy-vk.ini `[Console] Key=` (a VK code or a scan code) for anyone who needs it in the game.

**Look.** A strip across the top of the screen, about a third of its height: dark translucent background, the
scrollback (command echo + replies), the input line with a caret at the bottom, the completion list under it. It
slides in over ~0.1 s. Scale follows the window height (one size up at 1440p and above). It is drawn after the game's
interface, so Ctrl+Shift+H hiding the interface doesn't hide it, and frame dumps / screenshots leave it out.

**Commands.**

| Typed | Does |
| --- | --- |
| `bloom` | shows the value, range, default and label: `RVK_Bloom = 0.6 (0 .. 2, default 0.6) Bloom strength` |
| `bloom 1.2` | sets it (clamped, applied at once, saved like the settings window) |
| `pbr on` / `off` / `toggle` | bools; `1` / `0` too |
| `aniso 8` | choices: only the listed values (completion offers them) |
| `bloom reset`, `reset pbr` | back to the default |
| `find shadow` | lists every setting whose name or label has the word, with values |
| `list lighting` | a section's settings, with values; `list` alone: the sections |
| `changed` | every setting that differs from its default |
| `try bloom 2` | temporary (`ApplyTemporary`): not saved, gone at the next start or with `untry` |
| `preset ultra` | the presets (rvk_settings presets), as in the Renderer tab |
| `hideui`, `freecam`, `dump`, `profile`, `luts`, `lightdebug` | the hotkey actions |
| `help [command]`, `clear` | |

Names are matched without the `RVK_` prefix and ignoring case (`RVK_Bloom`, `rvk_bloom`, `bloom` are one). A setting
whose parent is off says so when set: `RVK_PbrSpec = 1 (has no effect while RVK_Pbr is off)`.

**Completion as you type.** After each key: the best match shown as grey ghost text after the caret, and up to 8
candidates listed under the line with their current values. Tab (or Right at the line's end) accepts the ghost; Tab
again steps through the list. Ranking: prefix of the name > prefix of a word inside it (`spec` -> `PbrSpec`) > label
words > substring. After a name and a space it completes the values: `on off toggle reset` for bools, the choices for
choice settings, the range as a hint for numbers.

**History.** Up / Down step through earlier commands (the typed text kept as the newest entry, like a shell); the
same command twice in a row is stored once. Kept across sessions in `randy-vk-console.txt` next to the ini (last 200
lines). PgUp / PgDn scroll the scrollback; Ctrl+A / Ctrl+E / Home / End, Ctrl+Left / Right by word, Ctrl+Backspace
deletes a word, Ctrl+V pastes.

## How

1. **proxy/ddraw/rvk_console.cpp** (new), ticked from the present path beside the hotkeys (the same thread they run
   on, so `Set` is called as the settings window and hotkeys call it):
   - Input: the open key watched with `GetAsyncKeyState` (as the hotkeys); while open, typing comes from WM_CHAR and
     WM_KEYDOWN in the subclassed window procedure (moved out of rvk_freecam.cpp into a shared `rvk_input.cpp`: one
     subclass, one DirectInput patch, two users), queued for the next present.
   - The command parser and the completion work on the settings table through new `rvk_settings` calls: `Count`,
     `Info(i)` (what `RvkSettings_Get` gives), `Find(name)` by the console's loose rules, `Default`, `Presets`.
   - The hotkey actions moved into named functions in rvk_ddraw.cpp so a key and a command run the same code.
2. **Text drawing, on the CPU.** A monospace system font (Consolas, DejaVu Sans Mono, Liberation Mono, ...: the
   first GDI has), each character 32-255 drawn once with GDI into a coverage atlas at a size from the window height;
   the console drawn from it into a premultiplied image (strip + the candidates under it), redrawn only when
   something changed (typing, a reply, the caret's blink at 2 Hz, the slide).
3. **rvk: `Device::SetConsoleImage(pixels, w, h, y)`** (the threaded device copies it to the render thread), uploaded
   to a texture of its own and blended after `InterfaceEnd`'s composite with the same pipeline, its viewport the
   strip at the top. Present-time, so it shows whether or not the frame redrew the interface. GPU cost: one small
   blended quad.
4. **Settings window in step.** The Renderer tab reads values when it is shown; check that it reflects a console change
   without reopening (if not, a change counter in `RvkSettings_*` it can poll).

## Phases

1. Console shown and hidden, input blocked, text typed and echoed, `set` / show by exact name. Check the toggle key
   on the user's layout under Wine, and that chat never receives the `~`.
2. Completion (ghost + list, values), loose names, `find` / `list` / `changed` / `reset`, parent warnings.
3. History (with the file), scrollback, editing keys, `try`, presets, the hotkey actions, `help`.
4. AOReloaded through the console: randy31.dll exports `RvkConsole_Register(prefix, provider)` - AOReloaded registers
   `aor.` with its AOR_* settings (its table already lives in settings.cpp and goes through the game's DValues, so
   setting one takes effect as from its own tab). Possibly later, a `game.` provider for the game's own DValues
   (dvalue_dump.cpp already walks the registry), read-only at first.

## Testing without the game

`RANDYVK_CONSOLE_TEST=<frame>:<keys>` types into the console from that frame on (`{open}`, `{tab}`, `{enter}`, `{up}`,
`{bs}` ..., `{shot}` saves the console's image as logs/console-test-N.bmp); `RANDYVK_CONSOLE_SHOT=1` puts the console
into screenshots (`RANDYVK_DUMP_AT_FRAME`). With the harness and a test client folder (symlinks, its own randy31.dll
and randy-vk.ini - the console saves settings):

    AO_CLIENT=~/.wine-prk/drive_c/linux/client-ctest RANDYVK_DDRAW=rvk RANDYVK_CONSOLE_SHOT=1 RANDYVK_DUMP_AT_FRAME=14 \
    RANDYVK_CONSOLE_TEST='3:{open}help{enter}{shot}blo{shot}{tab}{shot}{enter}bloom 1.5{enter}{shot}' \
    tools/randy-harness.sh build/ctest --frames 20

## Verification

- rvk_demo: open the console over the demo scene, type, set a bool and a float, see the image change; the console is
  missing from the frame dump.
- In game: open / close with the key; chat and movement get nothing while it's open; `pbr off` / `on`, `bloom 2`,
  `aniso 4` change the picture at once and survive a restart (`try` ones don't); Tab and Up / Down; Renderer tab shows
  the new values; no frame-time change while closed.

## Decided (user, 2026-10-17)

- The open key is the key left of 1 by position (scan code 0x29), on every layout.
- A value set in the console is saved, as from the settings window; `try` gives an unsaved one.
- Phase 4 (AOReloaded's settings, game options) deferred.
