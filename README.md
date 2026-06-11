# Corbin's build of st

The [suckless terminal](https://st.suckless.org/), forked from LukeSmithxyz/st and tracking upstream 0.9.3. The usual patch set, plus a round of bugfixes and performance work that the stock patches needed (see below).

## Features

Using dmenu:

- follow URLs on screen with `alt-l`
- copy URLs with `alt-y`
- copy the output of the last command with `alt-o`

Bindings:

- scrollback with `alt-↑/↓`, `alt-pageup/down`, or shift while scrolling the mouse; vim-style with `alt-k/j` (faster with `alt-u/d`)
- zoom/change font size with `alt-shift-↑/↓` or `alt-shift-k/j`; `alt-home` returns to default
- copy with `alt-c`, paste with `alt-v`. `shift-insert` pastes the primary selection, ie same as middle click
- kitty-style `shift-enter` escape sequence, so TUI apps that distinguish it (like some chat clients) get the real thing

Other patches: boxdraw (pixel-perfect box-drawing characters), HarfBuzz ligatures, font2 fallback fonts.

## Theming

- Xresources and pywal compatible for dynamic colors; gruvbox is the default and fallback palette
- transparency/alpha, with a separate unfocused-window alpha, both settable from Xresources and adjustable at runtime with `alt-a`/`alt-s` (needs a compositor running)
- default font is the system "mono", so it follows your fontconfig setup

Priority for colors: wal's sequences beat Xresources, which beat the compiled-in gruvbox.

## Fixes and performance

The popular st patches don't always compose cleanly, and this build has had a "proper" bug hunt over them with Claude Code running Fable 5. Highlights from the commit history:

- scrollback: fixed jumbled history when resizing the window down and back up, fixed a repeating-line bug, and the view no longer snaps to the bottom when the terminal itself answers a query (so a background program can't yank you out of scrollback)
- resizing no longer reallocates every history line unless the width actually grows, which makes resizes much cheaper with a large scrollback
- fixed an off-by-one that excluded the last row and column from attribute updates
- fixed an out-of-bounds read in the HarfBuzz shaping call and a couple of memory leaks
- the `-A` alpha flag and the `minlatency`/`maxlatency` Xresources actually work now (the latter were being written as integers into doubles)
- fixed a cursor restore bug that showed up in lazygit
- builds with `-O2` by default, and keystroke handling no longer mallocs per keypress

## Installation

Dependencies: `libX11`, `libXft`, `fontconfig`, `freetype2`, `harfbuzz`, and `make` to build.

```sh
git clone https://github.com/corbin-zip/st
cd st
sudo make install
```

Run a compositor (eg `picom`) if you want transparency.

## Configuring with Xresources

Persistent configuration is compiled in from `config.h`, but font, colors, alpha, and a few other variables can be overridden from `~/.Xresources` or `~/.Xdefaults` (load with `xrdb`):

```
*.font:  Liberation Mono:pixelsize=12:antialias=true:autohint=true;
*.alpha: 0.9
*.color0: #111
...
```

`alpha` goes from 0 (transparent) to 1 (opaque). There is an example `Xdefaults` file in this repository.
