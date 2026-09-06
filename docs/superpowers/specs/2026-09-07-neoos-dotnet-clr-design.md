# neoos-dotnet-clr — Milestone 1 (Hello World) Design

## Goal

NeoCLR is a minimal, custom C# runtime for NeoOS: parse a real,
Roslyn-compiled CIL assembly and execute it directly on NeoOS, without
porting CoreCLR or Mono. This document scopes ONLY the first milestone
— the smallest possible vertical slice, end to end:

```csharp
class Program
{
    static void Main()
    {
        Console.WriteLine("Hello from NeoOS!");
    }
}
```

Everything past this (locals/arithmetic, fields/classes, arrays,
exceptions, GC, threads, networking, JIT, AOT) is deliberately out of
scope here and becomes its own later milestone, each with its own
spec/plan cycle — the project is too large for one spec, exactly the
shape `docs/superpowers/specs/`'s other entries decompose large
efforts into.

**The runtime runs ON NeoOS itself** — cross-compiled, statically
linked against `neoos-musl` — not on the host. Only the C# → IL
compilation step (`dotnet build`, i.e. Roslyn) happens on the host;
NeoOS only ever sees the resulting `.dll`.

## Global Constraints

- Language: Rust, using the official `x86_64-unknown-linux-musl`
  target's full `std` (verified working on real NeoOS this session —
  see "Toolchain" below). No custom target JSON, no `-Z build-std`,
  no `no_std`.
- Repo: new `NeoOSOrganization/neoos-dotnet-clr`, matching every other
  component's `neoos-<name>` convention (`neoos-musl`,
  `neoos-openssl`, `neoos-libssh2`, `neoos-curl`).
- PE/CLI metadata parsing is hand-rolled against ECMA-335 directly,
  scoped to exactly what a Roslyn `AnyCPU`/ILOnly assembly contains —
  not a vendored parsing crate, and not an attempt at the full spec.
- The Rust toolchain installation (`rustup`, the musl target, the
  link recipe) must be **persistent** on this host — already true by
  default (`~/.cargo`/`~/.rustup`, not `/tmp`) — matching how the
  `x86_64-elf-gcc` cross-compiler is installed once at
  `~/opt/cross-x86_64-elf` and reused by every C-based port.
- No kernel changes. This milestone is entirely userland; every
  syscall it needs (`open`/`read`/`write`/`close`/`exit`, argv/envp,
  `mmap`/`brk`) already exists and already works.
- Boot-verified only, via QEMU + serial log capture, per project
  convention (no host-runnable unit tests for the on-NeoOS half; the
  host-side `dotnet build` step has no NeoOS dependency to test).

## Toolchain (verified this session, not assumed)

**Host**: `dotnet` SDK (already installed, version 10.0.110) compiles
C# to a real PE/CLI assembly via Roslyn. Verified: a minimal
`Console.WriteLine` console app builds to a 4608-byte, 3-section
PE32/.NET assembly.

**NeoOS target**: Rust's official, stable-channel
`x86_64-unknown-linux-musl` target, relinked against `neoos-musl`
instead of Rust's own bundled musl — the same "it's just an ordinary
Linux+musl program" principle every C port in this org already relies
on, since `neoos-musl` presents a genuine Linux-shaped musl ABI (the
adaptor translates NeoOS's own syscall numbers underneath, invisibly
to anything linked against it). Verified end-to-end this session: a
real `std` Rust program (`println!`, `Vec<i32>`, iterator `.sum()`)
built for this target and relinked this way booted correctly on real
NeoOS via QEMU, printing correct output.

The link recipe, to be committed as `neoos-dotnet-clr/.cargo/config.toml`:

```toml
[target.x86_64-unknown-linux-musl]
linker = "x86_64-elf-gcc"
rustflags = [
    "-C", "link-self-contained=no",
    "-C", "link-arg=-static",
    "-C", "link-arg=-nostdlib",
    "-C", "link-arg=-L<neoos-musl>/build-output/lib",
    "-C", "link-arg=-L<rust-sysroot>/lib/rustlib/x86_64-unknown-linux-musl/lib/self-contained",
    "-C", "link-arg=<neoos-musl>/build-output/lib/crt1.o",
    "-C", "link-arg=-lc",
    "-C", "link-arg=-lgcc",
    "-C", "link-arg=-T<neoos-kernel>/userland/user.ld",
    "-C", "link-arg=-znoexecstack",
]
```

(`<neoos-musl>`, `<rust-sysroot>`, `<neoos-kernel>` are placeholders
for the plan to resolve to real absolute or relative paths — e.g. via
a build script reading environment variables, matching how
`build.sh` scripts in the C-based ports take `MUSL_DIR`/etc. as
overridable variables rather than hardcoding paths.)

`-C link-self-contained=no` stops Rust from linking its own bundled
musl; `libunwind.a` (still needed by `std` internals regardless of
panic strategy) is Rust's own bundled copy, found under the sysroot's
`self-contained/` directory — not something `neoos-musl` needs to
provide.

Full `std` availability means the object model, BCL, and interpreter
are written in ordinary safe Rust with `String`/`Vec`/`HashMap`/`Box`
available from the start — no `alloc`-only constraints shaping
Milestone 1's design. (`std::thread` may also work now, given the
`clone(2)` milestone this session — worth verifying in a later
milestone when threading actually matters; irrelevant to this one,
which is single-threaded.)

## Architecture

### PE/CLI parsing

Roslyn's ILOnly/AnyCPU output has no native code sections and no
relocations to handle. The parse path: DOS header → PE header →
optional header's CLI data directory → `IMAGE_COR20_HEADER` (the CLI
header) → metadata root → `#~` stream (compressed metadata tables) +
`#Strings`/`#US`/`#GUID`/`#Blob` heaps. Milestone 1 needs exactly the
tables a `Console.WriteLine` program touches: `Module`, `TypeRef`,
`TypeDef`, `MethodDef`, `MemberRef`, `Assembly`, `AssemblyRef` — not
the full 40+ ECMA-335 table set.

### Finding `Main`

The CLI header's `EntryPointToken` field is a `MethodDef` token —
already a direct index into the MethodDef table. No name-based lookup,
no ambiguity, no scanning for an `[EntryPoint]`-like convention.

### Method representation

A method body starts with either a tiny header (1 byte: code size ≤
63, no locals, no exceptions) or a fat header (12 bytes: flags,
max-stack, code size, a local-variable-signature token, optional
exception clauses), followed by raw IL bytes. Milestone 1's `Method`
type:

```rust
struct Method {
    max_stack: u16,
    il: Vec<u8>,
    locals_sig: Option<Token>,   // unused until Milestone 2's locals
}
```

### IL interpreter

A stack-based VM over `Vec<Value>`:

```rust
enum Value {
    I4(i32),
    Object(Rc<NeoObject>),   // only NeoString populates this in Milestone 1
}
```

`ldstr <token>` reads the `#US` heap at that offset (UTF-16LE,
.NET's native string encoding already) and pushes a `NeoString`.
`call <token>` resolves the token through the metadata tables to a
`(TypeName, MethodName)` pair; if it matches the internal-call table,
the native Rust function runs directly — there is no IL body for
`Console::WriteLine` to interpret, since it never had one in the real
BCL either. `ret` ends the frame (trivial with exactly one frame,
`Main`, in this milestone).

### Strings

`NeoString { utf16: Box<[u16]> }` — the `#US` heap's native encoding,
kept as-is inside the runtime. Converted to UTF-8 only at the
native-call boundary (`char::decode_utf16`, from `std`), matching real
.NET's own console-encoding conversion at exactly the same boundary.

### Resolving `System.Console::WriteLine`

A hardcoded `HashMap<(&'static str, &'static str), NativeFn>` for
Milestone 1: `("System.Console", "WriteLine") -> neo_console_write_line`.
No signature-based overload resolution yet — Milestone 1's test
program calls exactly one overload (`WriteLine(string)`), and
overload matching becomes real work only once a program has more than
one candidate.

### Reaching musl and the NeoOS syscall

```
neo_console_write_line(&NeoString)
  -> UTF-16 to UTF-8 (std::char::decode_utf16)
  -> std::io::stdout().write_all(...)   // or the `libc` crate's write() directly; equivalent here
  -> musl's write()                      // neoos-musl, already working
  -> neoos-musl's syscall_arch.h funnel  // already working, unchanged
  -> NeoOS's real SYS_WRITE               // already working, unchanged
```

Nothing new is needed anywhere below `neo_console_write_line` — this
is the exact same path every existing C-based NeoOS program already
uses.

### Minimum NeoOS syscall ABI required

None beyond what already exists: `open`/`read`/`write`/`close`/`exit`,
argv/envp (to receive the assembly path), and `mmap`/`brk` (for
`std`'s allocator). This milestone makes no kernel changes.

## Project structure

```
neoos-dotnet-clr/
  .cargo/config.toml       # the musl-target link recipe above
  Cargo.toml                # workspace root
  crates/
    pe-cli/                # PE header + CLI metadata table/heap parsing.
                            # Produces: Assembly { tables, heaps, entry_point_token }.
                            # No IL execution knowledge.
    il/                    # Method body decode (tiny/fat header) + the
                            # Method type + a Token->(TypeName,MethodName)
                            # resolver over pe-cli's tables.
                            # Depends on: pe-cli's Assembly/table types.
    interp/                # The interpreter loop, Value, NeoObject,
                            # NeoString. Depends on: il's Method/decoded
                            # instruction types.
    bcl/                    # The internal-call HashMap + native fn
                            # implementations (neo_console_write_line).
                            # Depends on: interp's Value/NeoObject only
                            # -- never pe-cli or il directly, so the
                            # internal-call surface can grow without
                            # the BCL crate needing to know how
                            # metadata parsing or IL decoding work.
    neoclr/                 # The bin crate: argv[1] -> read file ->
                            # pe-cli::load -> il::find_entry_point ->
                            # interp::run, wired to bcl's table.
  test-assemblies/
    Hello/
      Hello.csproj           # trimmed: no ImplicitUsings, no Nullable,
                              # minimal TargetFramework -- keeps the
                              # metadata surface small on purpose.
      Program.cs
                              # Hello.dll itself is a build artifact,
                              # not committed -- matching how every
                              # .nex test binary elsewhere in this org
                              # is scratch, never committed.
```

## Testing plan

Boot verification via QEMU + serial log, matching every other
milestone's own convention:

1. `dotnet build` the trimmed `Hello.csproj` on the host, confirm a
   `Hello.dll` is produced.
2. Cross-compile `neoclr` for `x86_64-unknown-linux-musl` per the link
   recipe above, `nexify.sh` it, embed both `neoclr.nex` and
   `Hello.dll` into a NeoOS disk image (`Hello.dll` as a plain data
   file, not a `.nex` — it is never executed as a NeoOS binary itself,
   only read by `neoclr`).
3. Boot with `/etc/inittab` set to `wait /bin/neoclr.nex /path/to/Hello.dll`.
4. Expected serial output: `Hello from NeoOS!` (or whatever exact
   string the trimmed test program prints), no exception/panic/halted
   line.
5. Full `tools/gauntlet.sh 15 3` regression, zero retries — this
   milestone changes no kernel code and no shared musl shim code, so
   a regression here would mean something in the new repo's own build
   process disturbed shared state, which would be surprising and
   worth root-causing rather than dismissing.

## Next milestone (not this one)

Locals, arguments, and arithmetic on IL a `Console.WriteLine`-only
program never touches: `ldarg`/`ldloc`/`stloc`/`ldc.i4`/`add`/`sub`/
`mul`/`div`/`br`/`brtrue`/`brfalse` — enough for something like a
small loop-and-print or recursive Fibonacci program. Still no heap
objects beyond strings, no GC, no fields/classes — those follow later,
matching the ordering in the original project brief.

## Self-review

- **Placeholders**: the `.cargo/config.toml` recipe has bracketed
  path placeholders (`<neoos-musl>` etc.) called out explicitly as
  "for the plan to resolve," not left ambiguous — the plan's own job
  is picking the exact variable-passing mechanism (env vars, a build
  script), matching how every C-based port's `build.sh` already takes
  `MUSL_DIR` etc. as overridable inputs rather than hardcoded paths.
- **Internal consistency**: the crate dependency direction stated in
  "Project structure" (`bcl` depends only on `interp`'s types, never
  on `pe-cli`/`il`) matches the data flow described in "Resolving
  `System.Console::WriteLine`" (resolution happens in `il`/`interp`,
  before ever reaching `bcl`'s table).
- **Scope**: single vertical slice, no kernel changes, one new repo —
  appropriately sized for one implementation plan. Everything larger
  (GC, classes, threads, JIT/AOT) is explicitly deferred, not
  smuggled in.
