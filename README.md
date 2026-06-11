# Corbin's build of st

The [suckless terminal](https://st.suckless.org/), forked from LukeSmithxyz/st and tracking upstream 0.9.3. The usual patch set, plus a round of bugfixes and performance work that the stock patches needed (see below).

## Features

Using dmenu:

- follow URLs on screen with `alt-l`
- copy URLs with `alt-y`
- copy the output of the last command with `alt-o`

Bindings:

- scrollback with `alt-↑/↓`, `alt-pageup/down`, or shift while scrolling the mouse; vim-style with `alt-k/j` (faster with `alt-u/d`)
- scrollback **reflows on resize**: text rewraps to the new width like kitty/foot, with nothing clipped or lost (based on the extended [scrollback-reflow-standalone](https://st.suckless.org/patches/scrollback-reflow-standalone/) patch, replacing the older scrollback patch stack)
- selections persist while scrolling through history (extended variant; a resize still resets the selection, since reflow invalidates its coordinates)
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

- scrollback was rebuilt on the standalone reflow patch (ring buffer + view offset); the merge keeps this fork's fix where only real user input snaps the view to the bottom, so a terminal reply can't yank you out of scrollback
- fixed an out-of-bounds read in the reflow patch itself: growing the window when the "skip reflow" optimization fired left history lines allocated at the old, narrower width while everything reads them at the new width (caught with ASan + a fuzz harness; the ring now tracks its minimum allocation width and rebuilds when growing past it)
- fixed the reflow patch's "junk prompt fragments" artifact (its page documents it as expected behavior when dragging the window narrower than the prompt): the cursor is now tracked through the rewrap to its exact logical position (the same strategy foot/kitty use, plus kitty's pin-to-last-content-line heuristic from kitty#170), the trailing-space trim no longer eats cells at/behind the cursor, and the patch's wrap-flag hack above the cursor is gone. Verified by replaying PTY traces of real zsh and bash redrawing through column-by-column drag-resizes (`tests/`): no duplicated or joined prompt fragments remain. The one accepted artifact — shared by kitty/foot/alacritty, unfixable without OSC 133 shell integration — is that while the window is narrower than the prompt, the shell's own stale-positioned erase can eat a line directly above the prompt
- fixed the reflow patch dropping content entirely when the newest history row carried a wrap flag (the unwrap loop ended without flushing the pending logical line)
- a headless regression harness (`test_st.c`, builds against a stubbed X layer) drives all of the above under ASan/UBSan, including column-by-column drags with simulated zsh/readline prompt redraws (modeled on their actual SIGWINCH emit sequences) and a random-resize fuzz; `tests/` holds PTY traces of the real shells plus the tracer (`pty_trace.py`) — replay with `./test_st tests/trace_*.txt` after building the harness
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
