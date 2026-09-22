# System tools toolchain: .NET NativeAOT on NeoOS, and the bindings

Date: 2026-09-23. Status: draft for review. Desktop roadmap milestone
M6. Foundation for specs 08 (Task Manager), 09 (Notepad) and 10
(Terminal). Must not start those three until spec 02's components they
use exist (roadmap ordering rule).

## What already works (verified in `neoos-lvgl/csharp/Hello`)

- **Stock RID `linux-musl-x64`**, .NET 10 (`net10.0`; the reference host
  has SDK 10.0.112). No custom runtime pack and no new RID: NeoOS's
  Linux-ABI conformance is what makes the stock musl runtime run.
- `PublishAot`, `StaticExecutable=true`, `PositionIndependentExecutable=
  false`, `InvariantGlobalization`, workstation GC.
- The hosted cross toolchain (`~/opt/cross-x86_64-neoos`,
  `x86_64-neoos-linux-musl-gcc`) as the linker, through a small
  `gcc-wrapper.sh` that drops the `--target=x86_64-linux-musl` flag the
  ILCompiler always appends.
- `DirectPInvoke` + `NativeLibrary` to link P/Invokes at build time into
  static archives (`liblvgl.a`, `libwmclient.a`).
- NeoOS's `userland/user.ld` passed with `-T` (it keeps `.init_array`
  and `.eh_frame` under `--gc-sections` — both were real crashes; see
  the NativeAOT GC.Collect root cause in the project memory).
- Result: `Hello` is a **1.7 MB** static binary.
- Beyond GUI: TCP, threads, GC and ASP.NET Core/Kestrel run on NeoOS
  (project memory, 2026-09-07+), so the runtime itself is proven.

The toolchain work is therefore **packaging what works**, not porting.

## Goals

1. **`NeoOS.Sdk`**: the build settings above as reusable MSBuild files,
   so each tool's `.csproj` is ten lines and none re-derives the flags.
2. **Bindings** (C#, generated where possible, over the C ABIs):
   `NeoOS.UIKit` (spec 02 `nui.h` + the LVGL subset apps touch),
   `NeoOS.Wm` (protocol v2 `wmclient`, spec 05), `NeoOS.Sqlite`
   (`sqlite3.h` subset + `nsql`, spec 01), `NeoOS.Vt` (terminal parser,
   spec 10), `NeoOS.Native` (procfs readers, `kill`, `spawn`).
3. A repo layout, build pipeline and artifact layout os-builder can drive.
4. A startup-time and binary-size budget, measured, not guessed.

## Non-goals

- A NeoOS runtime pack, a custom RID, or a CoreCLR port (JIT). AOT only.
- `Microsoft.Data.Sqlite` / `SQLitePCLRaw`: they load the native library
  at runtime by name, which `DirectPInvoke` static linking replaces; a
  thin own binding is smaller and has no provider machinery.
- A managed UI framework (Avalonia, MAUI, LVGLSharp's WinForms layer).
  The tools use the UI kit's components through thin wrappers.
- Shared libraries (`.so`) for the tools in v1 (roadmap convention).

## Repositories

- **`neoos-ui-kit/bindings/csharp/`** — `NeoOS.UIKit` lives *with the C
  API it binds* so a C API change and its binding change land in one
  commit and one version (spec 02 versioning).
- **`NeoOSOrganization/neoos-systools`** — the SDK files, the other
  bindings, and the three tools:

```
neoos-systools/
  README.md, ARCHITECTURE.md
  Directory.Build.props          # imports sdk/NeoOS.Sdk.props for every project
  sdk/NeoOS.Sdk.props            # RID, AOT, static, GC, globalization, linker wrapper, user.ld
  sdk/NeoOS.Sdk.targets          # DirectPInvoke/NativeLibrary items from *_DIR properties
  sdk/gcc-wrapper.sh             # moved here from neoos-lvgl/csharp/Hello
  bindings/NeoOS.Wm/             # generated + handwritten
  bindings/NeoOS.Sqlite/
  bindings/NeoOS.Vt/
  bindings/NeoOS.Native/
  apps/TaskManager/  apps/Notepad/  apps/Terminal/
  tests/                         # host xUnit tests of pure logic (see Testing)
  publish.sh                     # builds every app into out/<app>/<app>
```

`neoos-lvgl/csharp/Hello` stays as the minimal example, switched to
import `NeoOS.Sdk.props` from a path so it keeps proving the SDK.

## `NeoOS.Sdk.props` (sketch)

```xml
<Project>
  <PropertyGroup>
    <TargetFramework>net10.0</TargetFramework>
    <RuntimeIdentifier>linux-musl-x64</RuntimeIdentifier>
    <PublishAot>true</PublishAot>
    <StaticExecutable>true</StaticExecutable>
    <PositionIndependentExecutable>false</PositionIndependentExecutable>
    <InvariantGlobalization>true</InvariantGlobalization>
    <ServerGarbageCollection>false</ServerGarbageCollection>
    <AllowUnsafeBlocks>true</AllowUnsafeBlocks>
    <IlcOptimizationPreference>Size</IlcOptimizationPreference>
    <StackTraceSupport>true</StackTraceSupport>   <!-- crash logs need it; measure its size -->
    <NeoosToolchain Condition="'$(NeoosToolchain)'==''">$(HOME)/opt/cross-x86_64-neoos</NeoosToolchain>
    <CppCompilerAndLinker>$(MSBuildThisFileDirectory)gcc-wrapper.sh</CppCompilerAndLinker>
    <ObjCopyName>x86_64-neoos-linux-musl-objcopy</ObjCopyName>
  </PropertyGroup>
  <ItemGroup>
    <ExtraLinkerArg Include="-T,$(NeoosUserLd)" />
    <ExtraLinkerArg Include="--no-relax" />
  </ItemGroup>
</Project>
```

`NeoOS.Sdk.targets` turns `UIKIT_DIR`, `LVGL_DIR`, `WM_DIR`,
`SQLITE_DIR`, `VT_DIR` (the same names os-builder passes to every port)
into `DirectPInvoke`/`NativeLibrary` items, only for the libraries a
project references.

## Bindings

### Generation

- **ClangSharp's `ClangSharpPInvokeGenerator`** (a `dotnet tool`, host
  only — the technique the LVGLSharp projects use, per the survey)
  generates raw `[DllImport("nui")]`-style declarations from `nui.h`,
  `wmclient.h`, `sqlite3.h` (a curated function list, not all 280
  functions), and `vt.h`. Output is **committed**, like the UI kit's
  generated token header, so a build does not need the generator (and
  the restricted-bandwidth host does not need to fetch it per build).
  If the tool is too large to fetch, the fallback is hand-written
  declarations — the surface is small (≈150 functions total), and the
  Hello app already hand-writes its bindings.
- `DllImport` library names (`"nui"`, `"lvgl"`, `"wmclient"`,
  `"sqlite3"`, `"vt"`) match the `DirectPInvoke` items, which is what
  binds them statically.
- Function pointers for callbacks are `delegate* unmanaged` with
  `[UnmanagedCallersOnly]` handlers (as Hello's flush callback) — no
  delegate marshalling, AOT-safe, no reverse-P/Invoke thunks at runtime.

### Hand-written layer (`NeoOS.UIKit`)

A thin, idiomatic wrapper; it adds no behaviour the C API lacks:

```csharp
using var app = NuiApp.Create("taskmgr");          // nui_app_create
var win = app.CreateWindow(640, 480, "Task Manager");
var list = Nui.ListVirtual(win.Root, 0, RenderRow);  // nui_list_virtual
var end = Nui.Button(win.Root, "End task", ButtonVariant.Danger);
end.Clicked += OnEndTask;                          // LV_EVENT_CLICKED via one static trampoline
return app.Run();                                  // nui_app_run
```

- Objects are `readonly struct` handles over `lv_obj_t*` (no finalizers;
  LVGL owns object lifetime, deleting a parent deletes children — the
  wrapper documents that rather than fighting it).
- Events: one `[UnmanagedCallersOnly]` trampoline registered with
  `lv_obj_add_event_cb`, user data = a `GCHandle` to the C# handler,
  freed on `LV_EVENT_DELETE`. This is the only place managed/unmanaged
  lifetime meets, so it is written once and tested.
- Threading: LVGL is single-threaded. All UI calls happen on the thread
  that called `NuiApp.Run`. Background work (Task Manager's `/proc`
  scan) posts results back with `app.Post(Action)` (a pipe the loop
  watches via `nui_app_watch_fd`) — never touches LVGL from a pool thread.

### `NeoOS.Native`

What the tools need from the OS that neither the BCL nor musl covers
well on NeoOS:

- `Proc.List()`: reads `/proc/<pid>/stat`, `status`, `cmdline`,
  `/proc/stat`, `/proc/meminfo`, `/proc/uptime` (K2) — plain file reads
  with parsing, so it works through the BCL's `File` API; no P/Invoke.
- `Process.Kill(pid, signal)`: `kill(2)` via P/Invoke to libc (musl,
  linked statically anyway).
- `Spawn(path, argv)`: NeoOS `spawnv` via `lib/`, matching the shell's
  launch rules (spec 06). `System.Diagnostics.Process.Start` is *not*
  used: it `fork`s, which on NeoOS copies the whole AOT process address
  space — **assumption to verify** whether NeoOS `fork` is copy-on-write;
  if it is, `Process.Start` is acceptable and `Spawn` becomes optional.

## Build pipeline and artifacts

```
host (build machine)                          image
dotnet publish (per app, via publish.sh)  ->  out/TaskManager/taskmgr
  + static libs from UIKIT/LVGL/WM/SQLITE/VT      -> /usr/local/bin/taskmgr.nex (nexify.sh)
                                           ->  manifests/*.app -> /usr/share/neoos/apps/
```

- `publish.sh` builds all apps; os-builder calls it with the `*_DIR`
  variables (roadmap os-builder section) after every C dependency is
  built.
- NeoOS binaries are wrapped with `tools/nexify.sh` like every other
  program (`hrtest`, `dyntest` precedent).
- Each app ships its start-menu manifest (spec 06) in `apps/<App>/<app>.app`.
- CI (when a CI exists): `dotnet build` + host tests on every push;
  `publish.sh` needs the cross toolchain, so it runs where that is
  installed (today: the reference host).

## Startup and size budget

| metric | budget | basis |
|---|---|---|
| binary size per tool | ≤ 8 MB | Hello (LVGL + wmclient + minimal C#) is 1.7 MB; tools add SQLite (~1.3 MB), the kit, and more managed code |
| cold start to first frame (KVM) | ≤ 300 ms | static, no JIT, no dynamic loading; ~dominated by LVGL init + first render |
| cold start to first frame (TCG) | ≤ 2 s | TCG is ~10× slower |
| idle CPU | 0 wakeups/s when nothing changes | the `nui_app` loop blocks in `poll`; Task Manager's refresh is the only periodic work, and only while visible |

Measured in M6 with the C# gallery port (below) and recorded in each
tool's README; a regression beyond budget is a bug.

## Testing

- **Host**: xUnit tests for pure managed logic — procfs parsing (fixture
  files), Notepad's text buffer and undo stack, terminal key encoding.
  They run with the ordinary host SDK, no cross toolchain.
- **Target**: M6's exit criterion is a **C# port of `nui-gallery`**
  that renders identically to the C one (screenshot diff), which
  exercises bindings, events, callbacks, multi-page UI and theme
  switching in one go. Each tool then has its own headless QEMU target
  (specs 08–10).

## Error handling and logging

- Every tool logs `[taskmgr]`/`[notepad]`/`[terminal]` lines to stderr.
- Unhandled exceptions: a top-level handler logs the exception type,
  message and stack trace (`StackTraceSupport=true`) before exiting 1 —
  the serial log is the only crash report NeoOS has.
- P/Invoke errors: C functions return error codes; wrappers throw typed
  exceptions (`NuiException`, `SqliteException`) carrying the code and
  the C-side message.

## Assumptions to verify

1. NeoOS `fork` is copy-on-write (decides `Process.Start` vs `Spawn`).
2. `ClangSharpPInvokeGenerator` is small enough to install on the
   restricted plan; otherwise hand-written bindings.
3. `StackTraceSupport` cost in binary size (measure; drop if it blows
   the budget and log `ToString()` of exceptions instead).
4. The `-mcmodel=large` question: if `neoos-lvgl`'s archives stay
   large-model, AOT-generated code linking against them must be too —
   Hello proves it links today; keep checking when LVGL's flags change.

## Risks

| risk | mitigation |
|---|---|
| .NET SDK updates change ILCompiler link flags (the wrapper exists because of one) | pin the SDK with `global.json`; the wrapper's comment names the flag |
| GC pauses in UI | workstation non-concurrent GC is fine at this heap size; tools allocate little per frame |
| Binding drift from C headers | generated + committed; `make check-bindings` regenerates and diffs |

## Decision log

| decision | alternatives | why |
|---|---|---|
| Stock `linux-musl-x64`, static AOT | custom RID/runtime pack; CoreCLR JIT port | already works (Hello, Kestrel); ABI conformance is the point of NeoOS |
| `NeoOS.Sdk` props/targets in `neoos-systools` | copy flags per project | the wrapper and linker script are load-bearing; one place |
| `NeoOS.UIKit` bindings live in `neoos-ui-kit` | in `neoos-systools` | versioned with the C API they bind |
| ClangSharp-generated, committed | hand-written only; runtime generation | less drift, no generator at build time; hand-written fallback |
| Own thin SQLite binding | Microsoft.Data.Sqlite | runtime native loading doesn't fit static `DirectPInvoke`; smaller |
| `[UnmanagedCallersOnly]` + one event trampoline | delegates | AOT-safe, no marshalling stubs, one lifetime rule |
