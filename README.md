# amberwm

A lightweight scrollable-column Wayland compositor built on **wlroots 0.20**
and **scenefx**, with KDE-style open/close/wobble animations, an interactive
region screenshot tool, and a tiny IPC helper (`ambermsg`).

## Build

Needs the development packages for: wlroots-0.20, scenefx-0.5, wayland,
libxkbcommon, pixman, libdrm.

```
make
```

Run `amberwm` from a TTY or through the provided session desktop entry.

## Running as a session (systemd)

`contrib/` ships a user unit plus a session wrapper:

- `contrib/amberwm-user.service` — install to
  `~/.config/systemd/user/`, then `systemctl --user daemon-reload`.
  Crashes restart after 3 s; logs land in
  `journalctl --user -u amberwm -f`. Logout (SIGTERM, exit 143) is
  treated as clean.
- `contrib/session-exec.sh` — resolves the active logind session into
  `XDG_SESSION_ID` so libseat binds the right seat, and execs the
  compositor. Point the ly/SDDM desktop entry at
  `systemctl --user --wait start amberwm`.

## IPC: ambermsg

`ambermsg` is the compositor's remote control over its Unix socket
(`$AMBERWM_IPC_SOCKET` or auto-discovered):

```
status clients version          # inspect
workspace N / focus next|ID     # navigate
close [ID]                      # kill windows by id (see `clients`)
enable|disable FEATURE          # animations blur shadows center-focused-column ws-slide
reload quit watch call ARGS...  # lifecycle, event stream, raw protocol
```

Unknown verbs get roasted; a handful of joke features (`enable opsec`,
`larp`, `hacker`, ...) answer locally and never touch the compositor.

## Configuration

`~/.config/amberwm/amberwm.cfg`, hot-reloaded on save: gaps, blur / corner
radius / shadow rules, animations (`animations`,
`center-focused-column`, magic-lamp close, workspace slide), key binds and
per-app window rules.


## Highlights

- Scrollable horizontal strip of columns (niri-style), optional
  dead-center focus (`center-focused-column=yes`); clicking any window
  scrolls the camera onto it
- Tiles glide into their new slot on every rearrange (150 ms ease-out)
- Region screenshots on Print: screen freezes under a dim layer, drag to
  select, drag inside to move the rectangle, `P` stamps the pointer,
  `Ctrl+C` copies, `Space` saves, `Esc` cancels; Shift+Print captures
  the whole screen
- Clipboard done properly: selection serves PNG + text, and the
  data-control + primary-selection protocols let noctalia's clipboard
  history, wl-copy/wl-paste and cliphist read and set it
- 9 workspaces with urgency pills over `ext-workspace-v1`; window lists
  via both `zwlr_foreign_toplevel_management` and
  `ext_foreign_toplevel_list_v1` so bars/switchers always see everything;
  taskbar clicks focus — and follow across workspaces
- Keyboard niceties: config key names are physical keys (case-folded),
  auto-repeat never re-fires one-shot binds, Ctrl+Alt+Fx reaches the TTY,
  closing the focused window hands focus to its neighbor
- Crash-resilient sessions: systemd unit restarts on failure and keeps
  logs in journald

## Credits & inspirations

amberwm is an independent implementation — no source code is copied from
other compositors — but several concepts and behaviors are deliberately
inspired by great projects:

- **[niri](https://github.com/niri-wm/niri)** (GPL-3.0) — the scrollable
  column layout idea and interaction model, center-focused column
  scrolling, and the region-screenshot UX (frozen frame behind a dim
  layer, persistent selection, move / copy / save keys).
- **[mango](https://github.com/mangowm/mango)** (GPL-3.0) — the
  tile-reflow animation approach (windows glide to their new slot on any
  rearrange) and stable numeric client ids exposed over IPC.
- **[wlroots](https://gitlab.freedesktop.org/wlroots/wlroots)** (MIT) and
  **[scenefx](https://github.com/wlrfx/scenefx)** (MIT) — the foundations
  everything here runs on.
- **[Noctalia](https://github.com/noctalia-dev/noctalia)** — highly
  recommended bar/shell to pair with amberwm; the default `SUPER+TAB`
  window-switcher binding talks to it via `noctalia msg window-switcher`.
