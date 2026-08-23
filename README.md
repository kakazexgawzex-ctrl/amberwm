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

## Configuration

`~/.config/amberwm/amberwm.cfg`, hot-reloaded on save: gaps, blur / corner
radius / shadow rules, animations (`animations`,
`center-focused-column`, magic-lamp close, workspace slide), key binds and
per-app window rules.

## Highlights

- Scrollable horizontal strip of columns (niri-style), optional
  dead-center focus (`center-focused-column=yes`)
- Tiles glide into their new slot on every rearrange (150 ms ease-out)
- Region screenshots on Print: screen freezes under a dim layer, drag to
  select, drag inside to move the rectangle, `P` stamps the pointer,
  `Ctrl+C` copies, `Space` saves, `Esc` cancels

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
