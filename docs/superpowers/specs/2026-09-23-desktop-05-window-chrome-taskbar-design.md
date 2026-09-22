# Window chrome, window states and the taskbar protocol (neoos-wm protocol v2)

Date: 2026-09-23. Status: draft for review. Desktop roadmap milestone
M3. Absorbs sub-projects 3 ("window chrome") and 4 ("taskbar entries
for running windows") of the roadmap begun in
`2026-09-22-wm-taskbar-start-menu-design.md`.

## What the compositor is today (read from `neoos-wm`, 2026-09-23)

- One AF_UNIX connection **is** one surface; `wmclient` keeps a single
  global connection per process (`static struct wm_conn g_conn`).
- At most **8 clients** (`MAX_CLIENTS 8`), fixed array.
- Server-side decoration already exists: an 18 px solid title bar and a
  border around every non-shell, non-fixed-position surface, with the
  title text. No buttons, no dragging, no resizing, no z-order stack —
  windows are painted in slot order; focus is a slot index.
- Keys are forwarded to the focused client as **raw evdev codes**
  (`struct wm_key { keycode, state }` from `/dev/input/event0`).
- `WM_CLOSE` (133) exists in the enum but nothing sends it.
- Surface flags: `SHELL` (bottom, at origin, one only), `GLASS`,
  `FIXED_POS`.

Everything below extends that; nothing is rewritten for its own sake.

## Goals

1. Every normal window has **Close / Maximize / Minimize** buttons in
   its title bar, drawn by the compositor, themed from the UI kit's
   tokens (`/etc/neoos/wm-theme.conf`, spec 02).
2. A real **window state model**: `normal | minimized | maximized |
   fullscreen | hidden`, owned by the compositor.
3. Windows can be **moved** (title-bar drag) and **raised**; a proper
   z-order stack replaces slot order.
4. A **shell channel**: one privileged client (the desktop shell, spec
   04) receives window lifecycle events and may request state changes —
   this is what the taskbar is built on.
5. **Multiple surfaces per connection** and no fixed client ceiling, so a
   single-process shell can own wallpaper + taskbar + menus, and a
   desktop can run more than 8 windows.
6. Opting out of chrome (DOOM fullscreen, games): undecorated surfaces
   and a fullscreen state.
7. A text **clipboard** (Notepad, Terminal copy/paste need one; nothing
   provides it today).
8. **Theme change broadcast** so apps follow light/dark (spec 02).

## Non-goals

- Interactive resize by dragging window edges (v2; `maximized` covers
  the main need). Clients may still request a size.
- Grouping several windows of one app under one taskbar button (v1:
  one button per window; see decision log).
- Virtual desktops, snapping, Alt-Tab switcher UI (the shell may add
  Alt-Tab later on top of the same channel), multi-monitor.
- Client-side decorations.

## Who draws the chrome: the compositor

Decided: **the WM draws the frame, title and the three buttons**
(server-side decorations), using colours and metrics generated from the
UI kit's `tokens.json` into `wm-theme.conf`. Reasons: the compositor
already draws a title bar; window-management gestures (drag, minimize)
must work even when the client is hung; and the WM cannot link LVGL
(it renders through Mesa and must stay small). The UI kit's influence
is its **tokens**, not its code — one token edit restyles both.

Metrics (from tokens): `title-height` 28 px (up from 18), `border` 1 px,
`button-size` 28×28, buttons right-aligned in order Minimize, Maximize
(Restore glyph when maximized), Close. Glyphs are drawn as simple
vector shapes (lines, rectangle outlines, an ×) with 1 px strokes — no
font needed. Close hover uses the `danger` token. Title text keeps the
existing Spleen bitmap font. Focused vs. unfocused windows use
`chrome.title-bg-active` / `-inactive`.

## Protocol v2

`WM_PROTO_VERSION` becomes 2. The hello exchange negotiates: a v1
client still works exactly as today (one implicit surface per
connection); the server replies with its version and the client library
selects the v2 path. Old binaries on an image keep running.

### Surfaces are objects

Every surface message carries a `surface` id (`uint32_t`) chosen by the
server in the create reply. v1 messages without it map to "the
connection's first surface".

```c
struct wm_create_surface2 {          // WM_CREATE_SURFACE2 = 8
    uint32_t width, height, flags;
    int32_t  x, y;
    uint8_t  glass_intensity;
    uint8_t  role;                   // WM_ROLE_* below
    uint16_t _pad;
    uint32_t parent;                 // for POPUP: the owning surface, else 0
};
// reply: WM_SURFACE_CREATED = 134 { uint32_t surface; uint32_t width, height; }
```

Every existing client→server body (`ATTACH_BUFFER`, `DAMAGE`, `COMMIT`,
`SET_TITLE`, `DESTROY_SURFACE`) gains a leading `uint32_t surface` in its
v2 form; every server→client event gains a leading `uint32_t surface`.
Bodies stay under `WM_MAX_BODY` (64) — `wm_set_title`'s text shrinks
from 62 to 58 bytes in v2, which is still one UTF-8 line; longer titles
are truncated on a character boundary.

`wmclient` gets an object API; the old single-surface calls stay as
wrappers over "surface 0":

```c
struct wm_surface *wm_surface_create(struct wm_conn *c, const struct wm_surface_opts *o);
uint32_t *wm_surface_pixels(struct wm_surface *s);
void      wm_surface_damage(struct wm_surface *s, int32_t x, int32_t y, uint32_t w, uint32_t h);
void      wm_surface_commit(struct wm_surface *s);
void      wm_surface_set_title(struct wm_surface *s, const char *utf8);
void      wm_surface_request_state(struct wm_surface *s, enum wm_state st);
void      wm_surface_destroy(struct wm_surface *s);
// wm_conn stops being a static singleton: wm_connect() allocates one.
```

### Roles (z-order layers)

| role | layer (bottom → top) | chrome | who |
|---|---|---|---|
| `WM_ROLE_BACKGROUND` (= v1 `SHELL`) | 0 | none | shell: wallpaper + desktop icons; one per display |
| `WM_ROLE_NORMAL` | 1 | full | applications |
| `WM_ROLE_PANEL` | 2 | none | shell: taskbar; always above normal windows, never focus-stealing on create |
| `WM_ROLE_POPUP` | 3 | none | menus, start menu, tooltips-outside-window; dismissed on outside click (focus loss) |

A NORMAL window in the **fullscreen state** (a state, not a role) is
drawn above the PANEL layer while it has focus — the taskbar is covered,
as on every desktop — and drops back to layer 1 when it loses focus, so
the taskbar reappears.

Within a layer, the compositor keeps a z-ordered list; clicking a
normal window raises it and focuses it. The fixed `clients[8]` array
becomes a dynamically grown array of connections, each owning a list
of surfaces; the stack is a separate intrusive list of surfaces.

Work area: maximized windows fill the screen **minus** every `PANEL`
surface's rect (the taskbar strip) — this finally implements the
"work-area reservation" the taskbar spec listed as a known gap.

### Window states

```c
enum wm_state { WM_STATE_NORMAL = 0, WM_STATE_MINIMIZED = 1,
                WM_STATE_MAXIMIZED = 2, WM_STATE_FULLSCREEN = 3,
                WM_STATE_HIDDEN = 4 };
```

- **normal**: at its own position/size, decorated.
- **minimized**: not drawn, not in the hit-test, not focusable; its
  taskbar button remains. The client keeps its buffer and may keep
  drawing (it just is not composited).
- **maximized**: moved to the work area origin and sent
  `WM_CONFIGURE{work_w, work_h - title}`; the client re-attaches a
  buffer of that size (the existing configure→attach path). Restoring
  sends `WM_CONFIGURE` with the saved normal geometry.
- **fullscreen**: undecorated, full screen (including over panels),
  `WM_CONFIGURE{screen_w, screen_h}`.
- **hidden**: like minimized but with no taskbar button (an app hiding a
  tool window, or the shell's own popups when closed).

Transitions are requested by chrome buttons, by the shell channel, or
by the client itself (`WM_REQUEST_STATE = 9 { surface, state }`). The
compositor applies them and **always** announces the result:
`WM_STATE_CHANGED = 135 { surface, state }` to the owning client (so
it can re-render at the new size) and to the shell.

### Chrome interactions

- Title-bar press + move = drag (the compositor moves the window; no
  client involvement; damage old and new rects).
- Double-click on title = toggle maximize.
- Minimize button → `MINIMIZED`; focus passes to the next window down
  the stack.
- Maximize button → `MAXIMIZED` / restore.
- Close button → `WM_CLOSE` (existing id 133, finally sent) to the
  client. The client decides (Notepad may ask to save). If the client
  has not destroyed the surface or replied within 5 s *of real time* the
  shell's taskbar offers "End task" (a `kill` by pid, which the shell
  knows from `WM_WIN_ADDED`); the compositor itself never kills. The
  5 s is a UI affordance threshold, not a synchronisation wait.

### Undecorated surfaces

`WM_SURFACE_UNDECORATED = 8` (flag): a normal-role window without chrome
(games, splash screens). It is still in the taskbar and still gets
states; it simply has no title bar, so the app must offer its own close
path (DOOM's menu) — documented.

### The shell channel

The first client to send `WM_SHELL_BIND = 10 {}` becomes the shell for
that compositor instance; later binds get `WM_ERROR{EBUSY}`. When the
shell's connection dies the role is free again, which is how a restarted
shell re-binds (spec 04). NeoOS has no uids, so "privileged" means
"first"; recorded as a divergence from X11/Wayland security models, fine
for a single-user OS.

Events to the shell (all carry the surface id of a NORMAL-role window):

| event | id | body |
|---|---|---|
| `WM_WIN_ADDED` | 136 | `surface, pid, state` — sent for every existing window at bind time too (this is how a restarted shell rebuilds its taskbar) |
| `WM_WIN_REMOVED` | 137 | `surface` |
| `WM_WIN_TITLE` | 138 | `surface, text[56]` |
| `WM_WIN_STATE` | 139 | `surface, state` |
| `WM_WIN_FOCUS` | 140 | `surface` (0 = nothing focused) |
| `WM_WIN_ICON` | 141 | `surface` + icon pixels passed as a memfd (SCM_RIGHTS, as buffers are) — 32×32 ARGB; optional, sent when a client sets one |

Requests from the shell:

| request | id | effect |
|---|---|---|
| `WM_WIN_ACTIVATE` | 11 | un-minimize if needed, raise, focus |
| `WM_WIN_SET_STATE` | 12 | any state for any window |
| `WM_WIN_CLOSE` | 13 | send `WM_CLOSE` to the owner (same as the × button) |

The pid comes from `SO_PEERCRED` on the connection. **Assumption to
verify**: NeoOS's AF_UNIX implements `SO_PEERCRED`; if not, it is a
small, Linux-shaped kernel addition (the struct is `{pid, uid, gid}`)
and joins the M0 list.

### Taskbar click semantics (implemented by the shell over the channel)

Decided, matching Windows:

| window is… | click its taskbar button → |
|---|---|
| minimized | `ACTIVATE` (restore to its previous normal/maximized state, raise, focus) |
| visible but not focused | `ACTIVATE` (raise + focus) |
| focused | `SET_STATE(MINIMIZED)` |

Focus rules: activating focuses; minimizing the focused window focuses
the next visible normal window in z-order; nothing focused → keyboard
goes nowhere (today's behaviour).

### Clipboard (text only in v2)

- `WM_CLIP_SET = 14`: the client sends `{ uint32_t len }` with a memfd
  (SCM_RIGHTS) holding UTF-8 text; the compositor keeps the memfd
  (owner-independent, so the text survives the copying app exiting).
- `WM_CLIP_GET = 15` → reply `WM_CLIP_DATA = 142 { uint32_t len }` + a
  dup'd read-only memfd.
- Primary selection (X11 middle-click) is out of scope.

### Theme broadcast

`WM_SET_THEME = 16 { uint8_t theme }` (shell only) → the compositor
re-reads `/etc/neoos/wm-theme.conf` for that theme, redraws chrome, and
broadcasts `WM_THEME_CHANGED = 143 { uint8_t theme }` to every client;
`nui_app` handles it (spec 02).

## Error handling

- Unknown message type or malformed body: log `[wm] client <pid>: bad
  message <type>` and drop the **connection**, as today for bad formats.
- A request for a state the surface's role does not support (minimize a
  popup): ignored, logged once.
- Missing/garbled `wm-theme.conf`: built-in defaults (today's colours),
  logged; never fatal.
- Shell dies: the compositor keeps every window exactly where it was;
  only taskbar-driven actions are unavailable until a shell binds again.

## Testing

- `make wm-chrome` (NeoOS): boots `wm.nex` + two `wmdemo` windows +
  a scripted test client bound as shell, which logs every `WM_WIN_*`
  event. Synthetic clicks (the existing injection path the taskbar test
  uses) press Minimize, the taskbar-equivalent `ACTIVATE`, Maximize,
  double-click title, drag the title bar, and Close; serial markers
  assert the exact event sequence and final geometry; screenshots at
  each step check the chrome renders (compared to goldens).
- A v1 client (today's `wmdemo`, unmodified) runs next to v2 clients —
  the compatibility guarantee is tested, not promised.
- More than 8 windows: a stress test opens 32 surfaces from 4 processes.
- Kill -9 the shell test client while windows exist, bind a new one:
  it receives `WM_WIN_ADDED` for every window.

## Documentation

`docs/stdlib.md`'s "The window system" section gains protocol v2: the
roles, states, shell channel, clipboard, theme — NeoOS-native protocol
with no POSIX analogue, so it must be documented there (CLAUDE.md).
`neoos-wm/README.md` points at it.

## Assumptions to verify

1. `SO_PEERCRED` on NeoOS AF_UNIX (above).
2. The synthetic-input injection path used by `make wm-taskbar` can
   produce drags (press, motion, release) not just clicks.
3. Re-attaching a larger buffer on `WM_CONFIGURE` works for LVGL apps
   (`nui_app` must recreate its `lv_display` buffers; spec 02 owns it).

## Risks

| risk | mitigation |
|---|---|
| Protocol v2 breaks existing clients (Hello, wmdemo, taskbar.nex) | version negotiation + a v1 client in the test |
| Growing `clients[]` introduces lifetime bugs (the fixed array was simple) | slot reuse only after the connection's surfaces are all destroyed; stress test |
| Chrome in the compositor adds per-frame cost | chrome is drawn only into damaged rects, like everything else |

## Open questions

- Should the WM draw a drop shadow (token `elevation`) around windows?
  It looks right with glass but costs fill-rate on softpipe. Draft: off
  in v2, token-controlled.
- Keyboard shortcuts owned by the WM (Win key → start menu, Alt+F4,
  Win+Up maximize)? Draft: the WM forwards a small set of "global"
  combos to the shell over the channel (`WM_SHELL_KEY`) rather than
  interpreting them — the shell owns policy. Specify in M4's plan.

## Decision log

| decision | alternatives | why |
|---|---|---|
| Server-side chrome, themed by generated tokens | UI kit draws chrome client-side | works for hung clients; WM cannot link LVGL; one token source anyway |
| Protocol v2 with negotiation, surfaces as objects | new socket/protocol; keep one-surface-per-connection and open N connections | a single-process shell needs N surfaces; N connections would break `wmclient`'s design and focus attribution |
| Shell channel = "first to bind" | uid check; compositor-hosted shell | NeoOS has no uids; hosting the shell in the WM process would couple LVGL to the compositor and kill it on a shell crash |
| No grouping in v1 | group by pid/app id | grouping needs an app-id concept + a popup list; nothing needs it yet; one button per window is correct, just less tidy |
| Clicking the focused window's button minimizes | always activate | matches Windows, which the brief's desktop is modelled on |
| Clipboard in the compositor | a separate clipboard daemon | the WM already brokers memfds between clients; one fewer process |
