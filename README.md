# richpaste

Paste anything into a terminal app, the way you would into a browser text box.
Text, screenshots, copied files. Locally, over ssh, inside tmux, through several hops.

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

3. **Escape-sequence tricks don't survive the trip.**
   OSC 52 can read the clipboard, but most terminals disable reads, it's text only,
   and tmux filters or rewrites it. Kitty's OSC 5522 can carry images, but only in kitty.
   Every layer (terminal, tmux, nested tmux, screen, mosh) is a place it can break.

4. **tmux keeps stale state.**
   A tmux pane started during yesterday's ssh session still has yesterday's
   environment. Anything found through an env var (like `SSH_AUTH_SOCK`) points at a
   socket that died when that session ended.

5. **Paths lie.** Drag a file into a terminal and you get a path. That path exists
   on your laptop, not on the server.

So no amount of clever input handling in the app can fix this. The bytes never
reach the server in the first place.

## The fix

Stop pushing clipboard bytes through the terminal. Use the terminal only for the
keypress, and carry the data over ssh itself, which already connects the two machines.

```
laptop                                         server
richpaste agent  <── ssh RemoteForward ──>  /tmp/richpaste-you-<random>.sock
reads your real clipboard                      ^
                                               app connects when you press Ctrl+V
```

- **Ctrl+V** is the single byte `0x16`. Every terminal and tmux pass it through.
  The app sees it and asks for the clipboard itself. It never waits for the terminal
  to paste.
- **ssh forwards a unix socket** from the server back to the agent on your laptop,
  like ssh-agent forwarding. The data never touches the pty, so tmux, nested tmux
  and terminal quirks can't get in the way.
- **The agent reads the real clipboard** and picks the best format: copied files
  first, then images (png before jpeg and so on), then text. Every item carries its
  MIME type, its bytes and a sha256.
- **The app checks everything** before trusting it: magic, sizes, hashes, UTF-8,
  MIME syntax, file names (no `../`). Bad data is an error, never a crash.

### Why it works in all the awkward cases

| Case | What happens |
|------|--------------|
| plain ssh | forward is live, paste goes through it |
| ssh + tmux | socket is found by name in `/tmp`, not by env var, so tmux doesn't matter |
| old tmux pane after reconnect | each connection gets a fresh socket name; app tries newest first, deletes dead ones |
| stale sockets left on the server | fresh names mean ssh never fails to bind over an old one |
| laptop → A → B | the agent on A has no display, so it relays to its own forward back to the laptop. Hop count stops loops. |
| local, no ssh | app reads the display directly. Wayland first, X11 next. |
| agent crashed | the ssh hook restarts it on the next connection |
| no forward at all | clear error in about a second, telling you to run `richpaste setup` |

### How ssh gets the forward without you typing `-R` every time

`richpaste setup` adds this to the top of `~/.ssh/config`:

```
Match !host github.com,gitlab.com,bitbucket.org,ssh.dev.azure.com exec "/path/to/richpaste ssh-hook"
  Include ~/.richpaste/ssh/forward.conf
```

On every ssh connection, `ssh-hook` makes sure the agent is running and writes a
`RemoteForward` line with a fresh random socket name. ssh then includes that line.
Git hosts are skipped.

## Use it

On the machine you type on:

```sh
richpaste setup
```

On the server, copy the same binary into your `PATH`. Then, in any app:

```sh
richpaste demo                 # an input box: Ctrl+V pastes text, images, files
richpaste paste                # prints text, or saves images/files and prints their paths
richpaste doctor               # shows what paste would use, and why
```

From C++, the whole client is one call:

```cpp
#include "rp/agent.hpp"

auto clip = rp::paste();       // Result<Clip>: items + where they came from
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

## Layout

```
include/rp/core.hpp       Result, Mime, FileName, Text, Item
include/rp/wire.hpp       the socket protocol (one request, one response)
src/wayland.cpp           Wayland data-control client, spoken directly over the socket
src/x11.cpp               X11 CLIPBOARD client with INCR (large data), no Xlib
src/clipboard.cpp         format choice, file URI lists, display discovery
src/agent.cpp             agent, relay, forwarded-socket lookup, ssh hook
src/composer.cpp          input box model: glyphs and attachment chips
src/term.cpp              raw mode, key parser, bracketed paste, renderer
src/sha256.cpp            sha256
```

## Build

```sh
cmake -S . -B build -G Ninja && cmake --build build
./build/rp_tests                       # unit tests
./tests/e2e_ssh.sh                     # real sshd on a random port, tmux, relay
cmake -S . -B build-static -DRICHPASTE_STATIC=ON -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build build-static             # one ~3 MB binary that runs on any Linux
```

The e2e test starts its own throwaway sshd and never touches `~/.ssh` or your clipboard.

## Hard limits

These can't be engineered away:

- **Something has to run on the machine with the clipboard.** No program on a server
  can reach your laptop's clipboard unless something on the laptop hands it over.
  That something is the agent.
- **Ctrl+V has to reach the app.** Windows Terminal and some others bind Ctrl+V to
  their own paste by default. Unbind it there. Terminals that send an empty bracketed
  paste for an image clipboard also trigger a fetch.
- **Trust is the same as ssh-agent forwarding.** While you're connected, any process
  running as you (or root) on that server can ask for your clipboard. Only use the
  forward with servers you'd forward your ssh-agent to. `Match` lets you limit hosts.
- **GNOME on Wayland** has no data-control protocol. There the agent reads through
  Xwayland, which works for most apps.

## Status

Works and is tested: Linux agent (Wayland, X11), Linux remote, ssh, tmux, nested
tmux, stale panes, multi-hop relay, large images, files, Unicode text.

Not done yet: macOS agent (NSPasteboard) and Windows agent (Win32 clipboard).
They only need a `read_clipboard` for that OS. The protocol, the remote side and the
input box stay the same. Mosh can't forward sockets, so use plain ssh for paste.
