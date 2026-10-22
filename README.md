# richpaste

The clipboard library inside [agentty](https://github.com/1ay1/agentty). It reads
text, screenshots and copied files: locally, over ssh, inside tmux, through several
hops. Native on Linux, macOS and Windows. No libraries, C++23.

There is no richpaste program. Users run it as `agentty paste ...`, and agentty's
Ctrl+V calls `rp::paste()`.

## The problem

You sit at a laptop. The app runs on a server you ssh'd into, maybe inside tmux.
You copy a screenshot and press Ctrl+V. Nothing happens, because:

1. **The terminal only carries typed text.** With an image on the clipboard there is
   no text, so most terminals send nothing.
2. **The clipboard is on the wrong machine.** It lives in the laptop's display
   server. The server can't reach it.
3. **Escape-sequence reads are patchy.** OSC 52 is text only and often off; tmux
   filters it. Kitty's OSC 5522 carries images, but few terminals have it.
4. **Environment variables go stale.** A tmux pane keeps yesterday's `SSH_AUTH_SOCK`.
5. **Paths lie.** A dropped file's path exists on the laptop, not on the server.

## Routes

The terminal is only used for the keypress. richpaste then fetches the clipboard by
the best route there is. Routes that need no setup come first, the ones that carry
images before the ones that only carry text, and each that has nothing hands over
to the next (`rp::plan`, a pure function, picks the order):

| # | Route | Needs | Carries |
|---|-------|-------|---------|
| 1 | **this machine's display** | you're local | text, images, files |
| 2 | **ssh forward** to the agent on your machine | `agentty paste setup` once there (free to check otherwise) | text, images, files |
| 3 | **the terminal** (OSC 5522) | kitty | text, images, files |
| 3 | **the terminal** (OSC 52) | a terminal that allows clipboard reads (not inside tmux) | text |
| 4 | **tmux's buffer** (`refresh-client -l`) | tmux with `set-clipboard on` | text, exact, no size cap |
| 5 | **pasted as text** (`richpaste:1:` line) | a built-in helper on your machine | text, images, files |

Over ssh the server's own display is skipped (it isn't the one you copied into)
unless you ask for it. Inside tmux, tmux says which terminal is outside: kitty is
asked directly for images, other terminals' text comes through tmux's buffer, since
an OSC 52 reply that crosses tmux into our input gets cut or re-encoded. A terminal
that refuses skips the tmux route too (it would refuse tmux the same way). Whatever
route a paste takes, a `richpaste:1:` line in it turns back into the image or file.

Apps that read the terminal through their own input (agentty) drive the same walk
with `rp::start` / `rp::advance` / `rp::skip`: richpaste runs every route it can and
stops when the next one is "ask the terminal", saying which dialect to use.

```
laptop                                              server
agentty paste agent  <── ssh RemoteForward ──>  /tmp/richpaste-you-<random>.sock
reads your clipboard                                ^
                                                    agentty connects on Ctrl+V
```

- **The ssh forward** never touches the pty, so tmux can't get in the way. Each
  connection gets a fresh socket; the newest is tried first and dead ones are
  removed, so old tmux panes keep working after you reconnect.
- **Through the terminal**, tmux gets passthrough-wrapped escapes, or is asked to
  fetch the clipboard itself for OSC 52. Every query is fenced with DA1, so a
  terminal that ignores it costs one round trip, not a timeout.
- **Formats:** copied files first, then images (png before jpeg...), then text. Each
  item carries its MIME type, bytes and sha256. Windows bitmaps become PNG.
- **Everything is checked:** magic, sizes, hashes, UTF-8, MIME syntax, file names
  (no `../`). Bad data is an error, never a crash.

## Platforms

All native: no helper tools, no extra libraries to install.

| | Reads with | Terminal route | Agent listens on |
|---|---|---|---|
| Linux, Wayland | data-control, spoken over the socket (no libwayland) | /dev/tty | unix socket |
| Linux, X11 | core protocol over the socket (no Xlib), INCR for big data | /dev/tty | unix socket |
| macOS | NSPasteboard through JavaScript for Automation (ships with macOS) | /dev/tty | unix socket |
| Windows | Win32: CF_HDROP, PNG, CF_DIBV5/CF_DIB, CF_UNICODETEXT | console in VT mode | 127.0.0.1 TCP |

Writing text: Win32 `CF_UNICODETEXT` on Windows (`rp::write_clipboard_text`).
agentty uses pbcopy / wl-copy / xclip elsewhere.

On Windows the agent is on loopback TCP, which every local user can reach. It only
answers your own Windows user (checked through the connection's owning process), or a
client holding the random token in `%USERPROFILE%\.agentty\paste\agent.tcp`.

## API

```cpp
#include "rp/agent.hpp"

rp::PasteOptions opt;            // set opt.terminal to also ask the terminal
auto clip = rp::paste(opt);      // Result<Clip>: items + which route they came from
if (!clip) show_error(clip.error().what);
for (auto& item : clip->items)
    std::visit(rp::overloaded{
        [](const rp::Text& t)  { insert(t.str()); },
        [](const rp::Image& i) { attach(i.mime, i.data); },
        [](const rp::File& f)  { attach(f.name, f.mime, f.data); },
    }, item);
```

`Item` is `std::variant<Text, Image, File>`. Every type is built only by a parser, so
a `FileName` can't hold `../x`, a `Text` is always valid UTF-8 and an `Image` always
has an `image/*` type. Errors are `std::expected` values, never exceptions.

Also: `rp::armor` (the text form), `rp::store` (save items by content hash),
`rp::remote_forward` (the `-R` pair for callers that run ssh themselves), and
`rp::cli::run` (the whole `agentty paste` command line). `rp::set_program` changes
how the command line names and re-runs itself; the defaults are agentty's.

State lives in `$AGENTTY_HOME/paste`, else `~/.agentty/paste`.

## Layout

```
include/rp/core.hpp       Result, Mime, FileName, Text, Item, base64
include/rp/wire.hpp       the agent protocol (one request, one response)
src/wayland.cpp           Wayland data-control client
src/x11.cpp               X11 CLIPBOARD client
src/clip_mac.cpp          macOS pasteboard
src/clip_win.cpp          Windows clipboard (read + text write)
src/image.cpp             DIB decoder, PNG encoder, deflate (for Windows bitmaps)
src/inband.cpp            OSC 5522 / OSC 52 / tmux clipboard through the terminal
src/clipboard.cpp         format choice, file URI lists, display discovery
src/agent.cpp             agent, relay, forwarded-socket lookup, ssh hook, paste()
src/store.cpp             saving pasted images and files
src/term.cpp              the terminal in raw mode (POSIX tty, Windows console)
src/cli.cpp               the `agentty paste` command line
src/io_posix.cpp          sockets, files, processes (Linux, macOS)
src/io_win.cpp            the same on Windows
```

## Build and test

agentty builds it with `add_subdirectory`. On its own, for its tests:

```sh
cmake -S . -B build -G Ninja && cmake --build build
./build/rp_tests                          # unit: protocol, parsers, base64, PNG, 5522, store
./tests/e2e_ssh.sh                        # real sshd: ssh, tmux, stale panes, relay, restart
DISPLAY=:N ./tests/inband_kitty.sh        # real kitty: OSC 5522/52, tmux, ssh
./tests/armor.sh                          # images as text: wrapping, corruption, tmux paste
DISPLAY=:N ./tests/browser_page.sh        # the web page in real firefox
python3 tests/png_check.py build/png_tool /tmp   # our PNG against an independent decoder
```

`build/rp_driver` stands in for `agentty paste` in the shell tests. The ssh tests start
their own sshd on a random port and never touch `~/.ssh` or your clipboard. The
terminal tests need an X server (Xvfb works).

## Hard limits

- **Something on your machine has to hand the clipboard over:** the agent, a terminal
  that answers clipboard reads, or a text helper (`agentty paste howto`). Without one,
  paste says so in about a second instead of hanging.
- **Ctrl+V has to reach the app.** Windows Terminal and VS Code keep Ctrl+V for their
  own paste; use Alt+V there. An empty bracketed paste also triggers a fetch.
- **Terminal reads ask permission.** Kitty asks before a program reads the clipboard,
  unless allowed (`clipboard_control ... read-clipboard`).
- **Trust is the same as ssh-agent forwarding.** While you're connected, any process
  running as you (or root) on that server can ask for your clipboard. `Match` limits hosts.
- **GNOME on Wayland** has no data-control protocol; the agent reads through Xwayland.
- **mosh** can't forward sockets. Paste falls back to the terminal routes.
