# GG Compiler — Persistent Context

## Build commands

```powershell
# Build the unit-test binary
cmake --build cmake-build-debug --target GG_tests

# Build the compiler binary (required before using compile.ps1)
cmake --build cmake-build-debug --target GG

# Run all unit tests
.\cmake-build-debug\GG_tests.exe

# Run a specific tag group
.\cmake-build-debug\GG_tests.exe "[typedptr]"

# If CMake cache is stale (new source files added, FetchContent issues, etc.)
cmake -S . -B cmake-build-debug
```

## compile.ps1 — compile a .gg file to a native Windows x86-64 executable

```powershell
# Basic compile (outputs to build\<stem>.exe)
.\compile.ps1 e2e\typed_pointer_test.gg

# Compile and print the generated LLVM IR to the console
.\compile.ps1 e2e\typed_pointer_test.gg -ShowIR

# Compile and immediately run, printing the exit code
.\compile.ps1 e2e\typed_pointer_test.gg -Run

# Both flags together
.\compile.ps1 e2e\typed_pointer_test.gg -ShowIR -Run
```

**Pipeline**: GG.exe (`.gg` → LLVM IR `.ll`) → clang (`.ll` → native `.exe`)
- IR files land in `build\<stem>.ll`
- Executables land in `build\<stem>.exe`
- Clang binary: `C:\Program Files\mysys2\clang64\bin\clang.exe`
- The GG binary must be rebuilt (`--target GG`) whenever the compiler source changes.
  compile.ps1 will abort early if `cmake-build-debug\GG.exe` is missing.

**Source directories:**
- `e2e\` — end-to-end test programs (each has a `main`; expected exit code 0)
- `samples\` — standalone example programs (`hello_factoriel.gg`, `extern_crt.gg`, etc.)
- `stdlib\` — standard library sources (`vec.gg`, `string_class.gg`, etc.) — no `main`,
  intended to be imported; will fail to link standalone (expected)

## Key invariants

### Token — NOT assignable (const members)
`Token` has three `const` members (`type`, `lexeme`, `line`), so its copy **and** move
**assignment operators are deleted**. Copy/move **construction** is fine.

**Never do:**
```cpp
Token t = ...;
t = Token{TokenType::IDENTIFIER, "foo", 1};  // assignment to existing — compile error

std::vector<Token> vec(10, someToken);
vec[0] = Token{...};                          // assignment — compile error

std::unordered_map<std::string, Token> m;
m["key"] = Token{...};                        // operator[] + assign — compile error
```

**Do instead:**
```cpp
Token t2{TokenType::IDENTIFIER, "foo", 1};   // fresh construction
vec.push_back(Token{...});                   // construct in-place
m.emplace("key", Token{...});                // emplace avoids assignment
```

### "A type is a single Token" — synthesized tokens
The AST stores types as single `Token` objects. The parser synthesizes special tokens
for compound types that don't map to a single keyword:

| Lexeme              | Meaning                                   |
|---------------------|-------------------------------------------|
| `"Class&"`          | Heap reference (`Reference` kind)         |
| `"ptr<i32>"`        | Typed raw pointer, primitive element      |
| `"ptr<Class>"`      | Typed raw pointer, object element         |
| `"ptr<Class.ref>"`  | Typed raw pointer, reference element      |

Decoded by `decodeSynthesizedType(const Token&)` in `Type.cpp`.
Both the semantic resolver (`resolveTypeToken`) and codegen resolvers
(`resolveParamType`, `resolveReturnType`) call `decodeSynthesizedType` first.

### ClassDeclStmt::methods is std::deque, not std::vector
`MethodDecl` contains `BlockStmt` (has `unique_ptr`) and `Token` (const members),
making it neither copyable nor noexcept-moveable. `std::deque` avoids the
vector-reallocation copy-fallback that would hit the deleted assignment operator.

## Architecture summary

### Compiler pipeline (per source file)
```
.gg source
  → Lexer            (produces Token stream)
  → ImportResolver   (3 passes — see below)
  → SemanticAnalyzer (3 passes — see below)
  → CodeGen          (walks AST, emits IRModule)
  → IRPrinter        (writes .ll text)
  → clang            (assembles + links to native .exe)
```

### ImportResolver — 3 passes
1. **Prescan templates**: walk all files recursively, register every generic template
   name into a shared `GenericRegistry` (so use sites in other files are recognised).
2. **Parse all files** (monomorphization deferred): each file parsed with
   `runMonomorphization=false`; instantiation requests accumulate in the shared registry.
3. **Monomorphize once**: a single `monoParser` expands all pending instantiations,
   appending concrete mangled declarations to the unified `Program`.

### SemanticAnalyzer — 3 passes
1. `collectClasses`  — build `classRegistry` (fields, methods, destructor).
2. `collectFunctions` — hoist top-level function signatures into global scope.
3. `analyzeStmt` over every declaration — full type checking.

### Generics — monomorphization entirely in the Parser
- `class Foo<T>` / `ReturnType bar<T>(...)` captured as raw token slices in `GenericRegistry::templates`.
  Generic functions use the **same declaration syntax as regular functions**, just with `<T>` after the name.
  There is no `fn` keyword in GG.
- Use sites (`Foo<i32>`) queue a `GenericInstantiation` on the worklist.
- `monomorphize()` substitutes type args, re-parses each instantiation into a concrete
  mangled declaration. **Semantic and CodeGen never see type parameters.**
- **Mangling** (`$` separator; `$` and `.` are valid unquoted LLVM identifier chars):
  - `Vec<i32>`       → `Vec$i32`
  - `Pair<i32,f64>`  → `Pair$i32$f64`
  - `Vec<Box<i32>>`  → `Vec$Box$i32`
  - `Vec<Point&>`    → `Vec$Point.ref`

### Memory model
- **Value objects** (`Point p`): stack-allocated struct; copied on assignment.
- **Heap references** (`Point& r = new Point(...)`): intrusive-refcounted heap pointer.
  `new` produces a `+1` reference. Assignment rebinds (retains new target, releases old).
- **Object parameters must be references** — the semantic analyser rejects bare value-type
  class parameters. Primitive parameters always pass by value.
- **Refcount runtime** (emitted verbatim into IR when `new` is used):
  - `gg_alloc(size)` — malloc with 8-byte header, initialises count to 1.
  - `gg_retain(ptr)` — null-safe refcount increment.
  - `gg_release(ptr, dtor)` — null-safe decrement; calls `dtor` + `free` at zero.
- **+1 return convention**: functions/methods returning `Class&` return a retained (`+1`)
  reference. Callee retains borrowed references before returning. Unbound temporaries
  (e.g. `make(99);`) are released at the full-expression boundary.

### ptr<T> — typed raw pointer
- `ptr<T>` (e.g. `ptr<i32>`, `ptr<Point&>`) is a low-level raw pointer; lowers to
  LLVM `ptr`. Intended for container internals (malloc'd buffers). **No bounds check.**
- `buf[i]` on a `ptr<T>`: one-index GEP — `getelementptr <elemIR>, ptr buf, i64 i`.
- `arr[i]` on a fixed-size array: two-index GEP into the alloca + runtime bounds check.
- `[]` is generalized: works on any expression, so `this.data[i]` works when `data`
  is a `ptr<T>` field.

### Destructors
- Declared as `~ClassName()` (no params, implicitly void). At most one per class.
- Cannot be called explicitly — compiler injects calls at scope exit and before `return`.
- Emitted as `@ClassName_dtor(ptr %self)` in IR.
- Any class with a user destructor **or** reference fields gets a generated `@ClassName_dtor`
  (the generated one releases reference fields in reverse declaration order).

### Clone helpers
- `@ClassName_clone(ptr %dest, ptr %src)`: deep copy — copies primitive fields,
  retains reference fields (releasing `dest`'s old reference targets first).
- Generated on demand when a value-copy initialisation is encountered.

## Test baseline
**362 test cases / 735 assertions** (as of `ptr<T>` + generalized `[]` indexing).

## Keeping CLAUDE.md current

**This file must be updated whenever the compiler is changed.** Specifically:

- **New language feature added** (new syntax, new type kind, new statement/expression node):
  update the architecture summary and any relevant invariants.
- **New test milestone**: update the test baseline count after a feature lands.
- **New source file or directory**: add it to the relevant section.
- **Invariant changes** (e.g. a data structure changes type, a new `const` member is added):
  update the Key Invariants section.
- **Pipeline changes** (new pass, new pass order, new tool in the chain): update
  the Compiler pipeline / ImportResolver / SemanticAnalyzer sections.
- **New synthesized token form**: add a row to the synthesized-token table.

Treat CLAUDE.md as the authoritative onboarding document — it is auto-loaded at the
start of every Claude Code session and replaces the need to re-explain the project.
