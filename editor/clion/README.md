# Using GG with CLion

Two things make GG comfortable in CLion: **syntax highlighting** for `.gg` files and a
**debugger** on the compiled executable (GG emits DWARF; CLion drives GDB/LLDB).

## 1. Syntax highlighting

Register the TextMate bundle once:

*Settings → Editor → TextMate Bundles → +* → select `editor/gg` → Apply.

See [`editor/gg/README.md`](../gg/README.md) for details. It's committed to the repo, so every
teammate registers the same folder instead of hand-building a File Type.

## 2. Debugging

Compile with `--debug`/`-g` (DWARF), then debug the `.exe` with GDB. There are three ways, from
least to most GUI.

### a) One command in a terminal (or CLion's built-in terminal)

```powershell
.\debug.ps1 e2e\class_test.gg              # build with symbols, open gdb
.\debug.ps1 e2e\class_test.gg -Break 180   # break at class_test.gg:180, then run
```

`debug.ps1` compiles via `compile.ps1 -DebugInfo` and launches GDB (defaults to
`mysys2\mingw64\bin\gdb.exe`; override with `-Gdb <path>`). Useful GDB commands once in:
`break class_test.gg:LINE`, `run`, `next`/`step`, `print p`, `print p.x`, `bt`, `continue`.

### b) A committed run configuration

The repo ships `.run/Debug GG (class_test).run.xml` — a Shell Script configuration that runs
`debug.ps1` and opens GDB in CLion's run console. It appears automatically in the run-config
dropdown. Duplicate it and change the `.gg` path in *SCRIPT_OPTIONS* for other programs.

> Requires the bundled **Shell Script** plugin. If CLion reports *"Unknown run configuration
> type ShConfigurationType"*, enable it under *Settings → Plugins → Installed → search "Shell
> Script"* (then restart), or just use option (a) / (c) instead — neither needs a plugin.

### c) The full visual debugger (breakpoint gutter + variables pane)

For clickable breakpoints and a Variables panel, set up a **Custom Build Application** once:

1. **Toolchain** — *Settings → Build, Execution, Deployment → Toolchains*. Any toolchain with a
   GDB works. To use the msys2 GDB, set the Debugger field to
   `C:\Program Files\mysys2\mingw64\bin\gdb.exe` (CLion's bundled GDB also works).
2. **External Tool** — *Settings → Tools → External Tools → +*:
   - Program: `powershell`
   - Arguments: `-NoProfile -ExecutionPolicy Bypass -File compile.ps1 e2e\class_test.gg -DebugInfo`
   - Working directory: `$ProjectFileDir$`
3. **Custom Build Target** — *Settings → Build, Execution, Deployment → Custom Build Targets → +*:
   pick a toolchain, set **Build** to the External Tool from step 2.
4. **Run/Debug config** — *Edit Configurations → + → Custom Build Application*:
   - Target: the custom build target
   - Executable: `build\class_test.exe`

Now press **Debug**. You get the full Debug tool window: the step over / into / out / run-to-cursor
toolbar, the Frames and Variables panes, Watches, and Evaluate Expression.

**Breakpoints.** Click the gutter next to a line in the open `.gg` file to toggle a line breakpoint.
If CLion won't place a gutter breakpoint (it may refuse on a file it doesn't treat as C/C++ source),
use either of these — both drive the same GUI:
- In the Debug console (the `(gdb)` prompt / *Console* tab of the debug session):
  `break class_test.gg:180`, then `continue`.
- Start with a breakpoint on `main` (which CLion always resolves by symbol), then **Run to Cursor**
  (Alt+F9) to any `.gg` line.

To make the gutter reliably clickable, associate `*.gg` with **C/C++** under *Settings → Editor →
File Types* instead of the GG TextMate bundle — that trades GG-specific highlighting for C-family
line breakpoints (a filename pattern can belong to only one file type). Stepping, Variables, and
Watches work either way.

### What to expect

- Line breakpoints, stepping, and backtraces map to `.gg` lines (GG emits an absolute source
  path, so GDB finds the file automatically).
- Frames show the mangled linkage names — `main`, `Point_squaredLen`, `Counter_increment`.
- Value objects display as structs with named fields at correct offsets. References, enums, and
  `ptr` show as raw pointer addresses (they're emitted as opaque `void*`).
- The debugger treats the language as C, so the expression evaluator uses C syntax (`p.x`, `*r`).

### Troubleshooting

- **`ld.lld: ... Permission denied` writing the `.exe`** — the executable is locked by a running
  instance or an active/paused debug session, so the rebuild can't overwrite it. Stop the session
  (red ■), make sure no `class_test.exe` lingers in Task Manager, and rebuild. `compile.ps1` now
  pre-clears the file and reports this clearly instead of the raw linker error.
- **No symbols / breakpoints don't bind** — the build step must pass **`-DebugInfo`**, not `-Debug`
  (PowerShell reserves `-Debug` as a common parameter on this advanced script, so it's silently
  ignored). Check the External Tool arguments.
