# richpaste

Paste anything into a terminal app, the way you would into a browser text box.
Text, screenshots, copied files. Locally, over ssh, inside tmux, through several hops.
Linux, macOS and Windows.

One static binary. No libraries. C++23.

## The problem

You sit at a laptop. The app runs on a server you ssh'd into, maybe inside tmux.
You copy a screenshot and press Ctrl+V in the app. Nothing happens.

Why it breaks:

1. **The terminal is the only pipe, and it only carries typed text.**
   When you paste, the terminal reads *its* clipboard and types the text into the pty.
   If the clipboard holds an image, there is no text, so most terminals send nothing.
   The app never even learns that you tried to paste.

2. **The clipboard is on the wrong machine.**
   The app runs on the server. Your clipboard lives on the laptop, inside a display
   server (Wayland, X11, macOS, Windows). The server can't reach it. There is no
   remote clipboard API.

3. **Escape-sequence tricks are patchy.**
   OSC 52 can read the clipboard, but it's text only, many terminals turn reads off,
   and tmux filters it. Kitty's OSC 5522 carries images, but only some terminals have it.

4. **tmux keeps stale state.**
   A pane started during yesterday's ssh session still has yesterday's environment.
   Anything found through an env var points at a socket that died with that session.

5. **Paths lie.** Drag a file into a terminal and you get a path. That path exists
   on your laptop, not on the server.

## The fix

Use the terminal only for the keypress. Fetch the clipboard yourself, by the best
route that exists, and fall back down the list:

| # | Route | What you need | Carries |
|---|-------|---------------|---------|
| 1 | **ssh forward** to the agent on your machine | `richpaste setup` once on your machine | text, images, files |
| 2 | **the terminal itself** (OSC 5522) | kitty-style terminal | text, images, files |
| 3 | **the terminal itself** (OSC 52) | terminal that allows clipboard reads | text |
| 4 | **this machine's display** | you're local | text, images, files |

Inside ssh, 1 → 2/3 → 4. Locally, 4 first. tmux is handled in every route.

```
laptop                                         server
richpaste agent  <── ssh RemoteForward ──>  /tmp/richpaste-you-<random>.sock
reads your real clipboard                      ^
                                               app connects when you press Ctrl+V
kitty / any OSC 5522 terminal  <── escape codes through the pty (and tmux) ──> app
```

- **Ctrl+V** is the single byte `0x16`; every terminal and tmux pass it through.
  **Alt+V** works too, for terminals that keep Ctrl+V for themselves.
- **The ssh forward** never touches the pty, so tmux can't get in the way.
  Each connection gets a fresh socket name; the app tries the newest first and
  deletes dead ones, so old tmux panes keep working after you reconnect.
- **Through the terminal**, inside tmux, escapes are wrapped in tmux passthrough
  (turned on for that one pane only). For OSC 52, tmux is asked to fetch the
  clipboard itself (`refresh-client -l`), which works with default tmux settings.
  Every query is followed by a DA1 fence, so a terminal that ignores it costs
  one round trip, not a timeout.
- **The agent reads the real clipboard** and picks the best format: copied files
  first, then images (png before jpeg and so on), then text. Every item carries its
  MIME type, its bytes and a sha256. Windows bitmaps (Print Screen) become PNG.
- **The app checks everything** before trusting it: magic, sizes, hashes, UTF-8,
  MIME syntax, file names (no `../`). Bad data is an error, never a crash.

## Use it

On the machine you type on (any OS):

```sh
richpaste setup
```

That adds an ssh `Match exec` hook to the top of `~/.ssh/config` and starts the agent.
Every ssh connection (except git hosts) then carries your clipboard. On Windows it
works with the built-in OpenSSH client (`ssh.exe`).

On the server, put the Linux (or macOS) binary in your `PATH`. Then, in any app:

```sh
richpaste demo                 # an input box: Ctrl+V / Alt+V pastes text, images, files
richpaste paste                # prints text, or saves images/files and prints their paths
richpaste doctor               # shows which route paste would use, and why
```

If you skip `setup`, paste still works in kitty (any type) and in terminals that
allow OSC 52 reads (text).

From C++, the whole client is one call:

```cpp
#include "rp/agent.hpp"

rp::PasteOptions opt;
opt.tty_in = opt.tty_out = tty_fd;   // optional: lets it ask the terminal (tty in raw mode)
auto clip = rp::paste(opt);          // Result<Clip>: items + which route they came from
if (!clip) show_error(clip.error().what);
for (auto& item : clip->items)
    std::visit(rp::overloaded{
        [](const rp::Text& t)  { insert(t.str()); },
        [](const rp::Image& i) { attach(i.mime, i.data); },
        [](const rp::File& f)  { attach(f.name, f.mime, f.data); },
    }, item);
```

`Item` is `std::variant<Text, Image, File>`. Every type is built only by a parser,
so a `FileName` can't hold `../x`, a `Text` is always valid UTF-8 and an `Image`
always has an `image/*` type. Errors are `std::expected` values, never exceptions.

## Platforms

| | Reads the clipboard with | Agent listens on |
|---|---|---|
| Linux, Wayland | data-control protocol, spoken over the socket (no libwayland) | unix socket |
| Linux, X11 | core protocol over the socket (no Xlib), INCR for big data | unix socket |
| macOS | NSPasteboard through JavaScript for Automation (ships with macOS) | unix socket |
| Windows | Win32 clipboard: CF_HDROP, PNG, CF_DIBV5/CF_DIB, CF_UNICODETEXT | 127.0.0.1 TCP |

On Windows the agent is on loopback TCP, which every local user can reach. It only
answers your own Windows user (checked through the connection's owning process),
or a client holding the random token in `%USERPROFILE%\.richpaste\agent.tcp`.

## Layout

```
include/rp/core.hpp       Result, Mime, FileName, Text, Item, base64
include/rp/wire.hpp       the agent protocol (one request, one response)
src/wayland.cpp           Wayland data-control client
src/x11.cpp               X11 CLIPBOARD client
src/clip_mac.cpp          macOS pasteboard
src/clip_win.cpp          Windows clipboard
src/image.cpp             DIB decoder, PNG encoder, deflate (for Windows bitmaps)
src/inband.cpp            OSC 5522 / OSC 52 / tmux clipboard through the terminal
src/clipboard.cpp         format choice, file URI lists, display discovery
src/agent.cpp             agent, relay, forwarded-socket lookup, ssh hook, paste()
src/io_posix.cpp          sockets, files, processes (Linux, macOS)
src/io_win.cpp            the same on Windows
src/composer.cpp          input box model: glyphs and attachment chips
src/term.cpp              tty raw mode (POSIX + Windows console), keys, renderer
```

## Build

```sh
cmake -S . -B build -G Ninja && cmake --build build
cmake -S . -B build-static -DRICHPASTE_STATIC=ON -DCMAKE_BUILD_TYPE=MinSizeRel   # any Linux
cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw.cmake -DRICHPASTE_STATIC=ON  # Windows
```

macOS: same as Linux with Apple clang (`-DRICHPASTE_STATIC=OFF`).

## Tests

```sh
./build/rp_tests                          # unit: protocol, parsers, base64, PNG, 5522, keys
./tests/e2e_ssh.sh                        # real sshd: ssh, tmux, stale panes, relay, restart
DISPLAY=:N ./tests/inband_kitty.sh        # real kitty: OSC 5522/52, tmux, ssh, the input box
DISPLAY=:N ./tests/wine_clip.sh build-win # Windows build under wine: clipboard formats, agent, ssh
python3 tests/png_check.py build/png_tool /tmp   # our PNG against an independent decoder
```

The ssh tests start their own sshd on a random port and never touch `~/.ssh` or
your clipboard. The terminal tests need an X server (Xvfb works).

## Hard limits

These can't be engineered away:

- **Something on your machine has to hand the clipboard over.** That is the agent,
  or a terminal that answers clipboard reads. A terminal with neither (for example
  wezterm, alacritty, GNOME Terminal) can't give a server your clipboard; there you
  need `richpaste setup`. richpaste says so in about a second instead of hanging.
- **Ctrl+V has to reach the app.** Windows Terminal and VS Code keep Ctrl+V for their
  own paste. Use Alt+V there, or unbind Ctrl+V. Terminals that send an empty
  bracketed paste for an image clipboard also trigger a fetch.
- **Terminal reads ask permission.** Kitty asks before letting a program read the
  clipboard, unless you allow it (`clipboard_control ... read-clipboard`). That's
  kitty protecting you; richpaste can't and shouldn't bypass it.
- **Trust is the same as ssh-agent forwarding.** While you're connected, any process
  running as you (or root) on that server can ask for your clipboard. Only forward to
  servers you'd forward your ssh-agent to. `Match` lets you limit hosts.
- **GNOME on Wayland** has no data-control protocol. The agent reads through Xwayland,
  which works for most apps.
- **mosh** can't forward sockets. Paste then falls back to the terminal routes.

## Status

Tested end to end here: Linux agent on Wayland (live Hyprland) and X11, ssh, tmux,
nested tmux, stale panes, multi-hop relay, kitty OSC 5522 (direct, in tmux, over ssh
with nothing installed on the laptop), OSC 52 (direct and via tmux), the Windows
build under wine (text, bitmaps, PNG, Explorer file copy, agent auth, and a Windows
agent serving a Linux server over a real ssh forward).

macOS compiles (checked against the macOS code paths) but hasn't run on a Mac yet.
Under wine, Windows' own ssh.exe wasn't available, so the Linux ssh client stood in for it.
