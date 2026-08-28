# GG Compiler — Persistent Context

## Build commands

Always pass `--parallel` so builds use all logical cores.

```powershell
# Build the unit-test binary
cmake --build cmake-build-debug --target GG_tests --parallel

# Build the compiler binary (required before using compile.ps1)
cmake --build cmake-build-debug --target GG --parallel

# Run all unit tests
.\cmake-build-debug\GG_tests.exe

# Run a specific tag group
.\cmake-build-debug\GG_tests.exe "[typedptr]"

# If CMake cache is stale (new source files added, FetchContent issues, etc.)
cmake -S . -B cmake-build-debug
```

**Linking uses `lld` when available** (auto-detected from the mysys2 clang64 toolchain; see the
`GG_USE_LLD` block in CMakeLists.txt). MinGW's default BFD `ld` links `GG_tests` (~50 objects +
Catch2, static, with debug info) in ~85 s — the dominant cost of an edit-one-test-file rebuild; lld
does it in ~8 s (≈11×), cutting that rebuild loop from ~88 s to ~12 s. It falls back to the default
linker silently if `ld.lld` isn't found; force the default with `-DGG_USE_LLD=OFF`.
**Test-suite runtime is a non-issue** (~5 s for the whole suite; the slowest single case is ~30 ms —
each test's cost is the temp-file round-trip in the `parseString`/`analyzeString` helpers, not
computation). The compiler sources (`${GG_SOURCES}`) build **once** into a `gg_core` STATIC library
that both `GG` and `GG_tests` link — previously they were compiled twice (once per executable), so a
clean / header-change build did double the work. `GG` additionally compiles only `main.cpp` +
`source/GG.cpp`; `GG_tests` only its `tests/*.cpp`. Warning flags are applied to `gg_core` (PRIVATE)
and each executable; sanitizer flags propagate to the executables via `gg_core`'s PUBLIC link options.

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

# Optimization level: -Opt <0|1|2|3|s|z> → clang -O<level> (default 0 = unoptimized)
.\compile.ps1 e2e\typed_pointer_test.gg -Opt 2 -Run

# Trap on integer overflow + out-of-range narrowing (Rust-style, opt-in) → GG --overflow-checks
.\compile.ps1 e2e\overflow_checks_test.gg -OverflowChecks -Run

# gcc-style -o: choose the output executable path (default: build\<stem>.exe)
.\compile.ps1 e2e\typed_pointer_test.gg -o build\bin\app.exe -Run
```

**Pipeline**: GG.exe (`.gg` → LLVM IR `.ll`) → clang (`.ll` → native `.exe`)
- IR files land in `build\<stem>.ll` (always — GG itself has no output-path flag, it only emits IR)
- Executables land in `build\<stem>.exe` by default, or at `-o <path>` when given (parent
  directories are created as needed). `-o` is the driver script's flag — it controls the path
  clang links to, since GG never invokes the linker itself.
- Clang binary: `C:\Program Files\mysys2\clang64\bin\clang.exe`
- The GG binary must be rebuilt (`--target GG`) whenever the compiler source changes.
  compile.ps1 will abort early if `cmake-build-debug\GG.exe` is missing.
- `compile.sh` (Linux) supports the same flag as `-o <path>` / `--output=<path>`.

**Source directories:**
- `e2e\` — end-to-end test programs (each has a `main`; expected exit code 0)
  - `e2e\stdlib\` — e2e programs whose subject is a specific stdlib class (`Array`, `String`,
    `List`, `Pair`, `IO.print`, …), as opposed to a language/compiler feature
- `samples\` — standalone example programs (`hello_factoriel.gg`, `extern_crt.gg`, etc.)
- `stdlib\` — standard library sources (`vec.gg`, `string_class.gg`, etc.) — no `main`,
  intended to be imported; will fail to link standalone (expected)
- `tests\stdlib\` — unit tests (`.cpp`) that compile/analyze actual `stdlib\*.gg` files directly
  (via `codegenFile`/`ImportResolver`), as opposed to `tests\*.cpp`'s synthetic inline-string
  programs. Currently just `stdlib_tests.cpp` (CRT/IO/math bindings); its `#include` of
  `helpers.h` is `"../helpers.h"` since it lives one directory below `tests\`. Registered in
  `CMakeLists.txt` as `tests/stdlib/stdlib_tests.cpp`.

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

| Lexeme              | Meaning                                        | Source syntax   |
|---------------------|------------------------------------------------|-----------------|
| `"Class&"`          | Owning heap reference (`Reference`, no borrow) | `Class&`        |
| `"ref:Class"`       | Non-owning class borrow (`Reference`, borrow)  | `Class*`        |
| `"ref:i32"`         | Non-owning primitive borrow (lvalue ref)       | `i32*`          |
| `"ptr<i32>"`        | Typed raw pointer, primitive element           | `ptr<i32>`      |
| `"ptr<Class>"`      | Typed raw pointer, object element              | `ptr<Class>`    |
| `"ptr<Class.ref>"`  | Typed raw pointer, reference element           | `ptr<Class&>`   |
| `"ptr<void>"`       | Alias for opaque `ptr` (`TypeKind::Ptr`)       | `ptr` / `ptr<void>` |
| `"Class&?"`         | Nullable owning reference (`isNullable`)       | `Class&?`       |
| `"ref:Class?"`      | Nullable class borrow                          | `Class*?`       |
| `"Color?"`          | Nullable enum                                  | `Color?`        |
| `"i32?"`            | Nullable primitive → tag+payload `{ i1, iN }`  | `i32?`          |

Decoded by `decodeSynthesizedType(const Token&)` in `Type.cpp`.
Both the semantic resolver (`resolveTypeToken`) and codegen resolvers
(`resolveParamType`, `resolveReturnType`) call `decodeSynthesizedType` first.

### ClassDeclStmt::methods is std::deque, not std::vector
`MethodDecl` contains `BlockStmt` (has `unique_ptr`) and `Token` (const members),
making it neither copyable nor noexcept-moveable. `std::deque` avoids the
vector-reallocation copy-fallback that would hit the deleted assignment operator.

### Const-by-default / `mut` keyword (Rust-like)
Bindings and instance fields are **immutable by default**; `mut` on the declaration opts
into mutability. Enforced entirely at compile time (semantic analyzer) — **codegen is
unchanged**. `mut` token mirrors `static`. Flags: `VarDeclExpr::isMut`, `ParamDecl::isMut`,
`FieldDecl::isMut`, `CastExpr::isMut`; `Symbol::isMutable`/`Symbol::paramMut`;
`ClassInfo::Field::isMut`; `ClassInfo::Method::paramMut`.
- **Locals**: const = *single assignment* (deferred init OK). `analyzeAssign` permits a write
  only when `isMutable` **or** not-yet-`isInitialized` (the one defining assignment);
  `analyzeCompoundAssign` and `analyzeUnary`/`analyzePostfix` (`++`/`--`) require `isMutable`.
- **Object (class-value) locals — definite-init now covers them too, with a deliberate no-magic
  rule**: a bare `ClassName name;` (no initializer) used to be **unconditionally** marked
  `isInitialized = true` in `analyzeVarDecl` the moment its struct was zero-initialized —
  regardless of whether the class even had a constructor. That let a method call run on an object
  whose constructor never executed (found via a real crash: a generic container's zero-arg ctor,
  e.g. `Array<T>`'s, sets up `cap`/`data`; skipping it left `cap == 0` forever, and every element
  write went into an effectively zero-sized `malloc` — silent heap corruption that only became
  visible once enough writes landed). Decided per-class in `analyzeVarDecl` from whether the class
  has a constructor at all — **not** from its arity:
  - **No constructor at all** → unaffected: `isInitialized = true` — zero bytes genuinely **is**
    the class's complete, valid state; there is no ctor logic being skipped.
  - **Any constructor exists at all** (including a zero-arg-callable one) → `isInitialized =
    false`, always — a bare declaration **never** implicitly calls a constructor. The class is
    treated exactly like an uninitialized primitive: any use before an explicit
    assignment/construction is the ordinary "used before it has been assigned a value" error, and
    the **single defining assignment** (on whichever branch reaches it — the existing
    branch-merge/definite-assignment machinery, already proven for primitives, applies unchanged)
    is where construction actually happens. An explicit `ClassName name();` / `ClassName name{}`
    (an empty-but-explicit call) is an ordinary initializer — parsed as a real constructor call —
    and is **unaffected** by this rule; it constructs immediately, same as any other initializer.
  - **Earlier design, tried and reverted**: a first version special-cased "a zero-arg-callable
    constructor exists" to auto-invoke it on a bare declaration (so `Array<T> a;` silently ran
    `Array()`), to avoid disrupting the huge amount of existing code relying on that pattern
    (confirmed empirically: removing the Object exemption outright broke ~27 pre-existing tests).
    This was deliberately reverted in favor of the simpler, uniform rule above: whether a bare
    declaration is safe to use immediately must never depend on the invisible detail of whether the
    class happens to also have a zero-arg overload — that's spooky action at a distance. The
    migration cost (updating every bare `ClassName x;` for a ctor-bearing class to an explicit
    `ClassName x();`) is paid once, mechanically, across the stdlib/tests/e2e programs.
  - This is also what *enables* picking a different constructor overload per branch with **no**
    default-construction overhead: `mut Widget w; if (c) { w = Widget(1); } else { w =
    Widget('a'); }` constructs directly into `w` on whichever branch runs, never wastes work
    constructing-then-discarding a default, and needs no temp object at all.
  - **Codegen fast path for the defining assignment** (`CodeGen::emitObjectDirectInit`, extracted
    from `genVarDecl`'s existing initializer-lowering so both share it): `analyzeAssign` captures
    `wasUninitialized = !sym->isInitialized` *before* the const-single-assignment check runs, and —
    for an `Object`-kind LHS where `wasUninitialized` holds — records the node in
    `SemanticResult::directConstructAssigns`. `genAssign` checks that set and, if present, tries
    `emitObjectDirectInit` (handles a bare constructor-call RHS or an sret call, exactly like a
    var-decl initializer) **before** falling back to the ordinary `genExpr` + `@Class_clone` copy
    lowering — so the defining assignment constructs straight into the destination with no
    temporary, no clone, no wasted allocation. A **non-defining** reassignment of an already-live
    `mut` binding is untouched (still goes through clone, correctly releasing/replacing the old
    value). **Loop-safety guard**: `directConstructAssigns` is only recorded when `loopDepth == 0`
    at the assignment — the semantic pass visits an AST node exactly once, so `wasUninitialized`
    reflects a single static snapshot; outside any loop that snapshot and "executes at most once
    ever" coincide exactly, but INSIDE a loop the same instruction can run many times at runtime,
    and skipping the destroy-old-value step on iterations 2+ would leak. Verified: a resource-owning
    class reassigned 5× inside a `while` loop (no zero-arg ctor) shows every `malloc` paired with a
    `free` (falls back to the always-correct clone path inside the loop; only the loop-external case
    gets the fast path) — this is a missed optimization in that case, never a correctness gap.
  - **`rejectUncloneablePtrOwner` gating in `analyzeAssign` also had to become two-part**, not
    zero/one like `analyzeVarDecl`'s: assigning into an Object LHS risks aliasing a raw-ptr-owning
    class's buffer (no `Clone` impl) via the generated memberwise clone in **two independent** ways
    — either the destination already holds a live value that a shallow clone would leak (fires
    whenever `!wasUninitialized`, regardless of the RHS shape), or the RHS is a genuine copy source
    (an existing live object/reference place, fires **even on the defining assignment** — aliasing
    two independently-destroyed owners doesn't care whether the destination was previously empty).
    The check fires on `!wasUninitialized || copySource`; only when *neither* applies (the defining
    assignment **and** a fresh constructor-call RHS) is it genuinely safe, which is exactly the
    shape the direct-construct optimization targets. (An earlier, incorrect draft gated solely on
    `copySource` — mirroring `analyzeVarDecl`'s gating, which is valid there because a var-decl's
    destination is *always* fresh by construction — and wrongly let a raw-ptr-owning class's
    **reassignment** via a fresh constructor call, e.g. `mut Buf b(1); b = Buf(2);` with no `Clone`
    impl, silently skip the leak-detection error; caught by `tests/clone_tests.cpp`'s pre-existing
    regression test for exactly that shape.)
  - Tests: `tests/const_tests.cpp` (`[mut][const]`, covering the ctor-less/has-any-ctor split, the
    explicit-empty-call-is-immediately-usable case, the per-branch-constructor codegen shape, the
    reassignment-still-clones case, and the two-part `rejectUncloneablePtrOwner` gating),
    `e2e/definite_init_object_test.gg`.
- **Parameters**: const by default; `mut` allowed on primitives **and references**
  (`mut Point&` = mutable borrow). A reference param still may **not** be rebound
  (the isParameter+Reference block in `analyzeAssign` fires first); `mut` only unlocks field
  writes. `paramIsMutable()` just returns `param.isMut`.
- **Instance fields — two gates** (both required for `obj.field = v`): (1) *field-level* —
  field is `mut`, or a const field set via `this.field =` in the ctor (`inConstructor` flag);
  (2) *binding-level / transitive* — the receiver must be a mutable place (`exprIsMutablePlace`:
  `mut` binding, `new`/call result, `this` **iff `currentThisMutable`**, or a `mut`-field chain
  on a mutable root). So a const object/ref binding forbids field writes even to `mut` fields.
- **Methods — Rust `&mut self`** (trailing `mut`: `void f() mut`). `MethodDecl::isMut`,
  `ClassInfo::Method::isMut`. `currentThisMutable = md.isMut || md.isConstructor ||
  md.isDestructor` (save/restored per method in `analyzeClassDecl`) drives
  `exprIsMutablePlace(ThisExpr)`. A non-`mut` method writing `this.field` → error ("read-only
  method"); calling a `mut` method requires a mutable receiver (gated in `analyzeMethodCall`).
  Parser: trailing `mut` after `)`; rejected on ctor/dtor(implicit)/static/enum methods.
- **const→mut cast warning** (`warnConstToMut`): coercing a read-only reference into a `mut`
  binding — at `mut T&` init (`analyzeVarDecl`), `mut T&` reassignment (`analyzeAssign`), or a
  `mut T&` argument (`analyzeCallArgs`, via `paramMut`) — warns unless the source is an explicit
  cast. References only (value copies are independent). `expr as mut T` (parser: `parseCast`
  accepts `mut` + `consumeType`) marks `CastExpr::isMut`, producing a mutable place / silencing.
- **Not gated**: array/`ptr<T>` element writes (`a[i] = v`); enum fields (always immutable — parser
  rejects `mut` on them, as it does on methods). `static` fields **are** now gated — const-by-default
  like every binding, `mut static` to opt in (see the static-members invariant).
- Parser: `mut`/`static` accepted in either order on locals (`parseExpression`) and members
  (`parseMemberList` gained an `isEnum` param); params via the `parseParam()` helper.

### Implicit `this` — members without `this.`
A bare name in a method resolves to a local/param/free-function **first** (they shadow), and
only falls back to a member of the enclosing class — so members have the **lowest** priority.
Because fields/methods aren't in the symbol table, this falls out of doing the member lookup
only when `symbolTable.lookup` misses. Semantic helpers `currentInstanceField` /
`currentStaticFieldType` / `currentClassMethod` (all keyed on `currentClassName`, instance
fields gated by `!currentMethodIsStatic`); fallbacks in `analyzeIdentifier` (read),
`analyzeAssign`/`analyzeCompoundAssign` (write, with the same field-mut + `currentThisMutable`
gating), `incDecTargetOk` (`++`/`--` for both locals and fields), and `analyzeCall` (bare
`foo()` → this-method or static call; instance-method-in-static-context and mut-method-in-
read-only-method are errors). Codegen mirrors it: `genImplicitFieldLoad`/`genImplicitFieldPtr`
+ `resolveAssignTarget` (used by genAssign/genCompoundAssign/genPostfix/genUnary) route a
bare name to a static global or a GEP on `this`; `genCall` falls back to `@Class_method`
(mangled name present in `funcParamTypes`) with a `ptr this` prefix for instance methods.
Purely additive — existing `this.`-prefixed code is unaffected.

### Unified signature — `fn NAME(params) [mut] [-> RetType [alias]]`
One signature shape for every function/method (free, instance/static, trait/impl, generic).
`fn` leads (before `static`/`private`). The **return type follows `->`**; **omitting the arrow
means void**. `extern` uses the same order: `extern name(params) [-> RetType];`. Constructors
(`ClassName(...)`) / destructors (`~ClassName()`) take no `fn` and no return type. Trailing `mut`
(a `&mut self` method) sits after `)` and before the optional `->`.
- **Lexer**: `fn` → `TokenType::FN`; `->` is `ARROW`.
- **Parser**: `parseFnDeclaration` = `name parseParamList() parseReturnSuffix() body`.
  `parseReturnSuffix` returns the return type token (a synthesized `void` token when there is no
  arrow) and, if an IDENTIFIER follows the type, the return alias. `parseMemberList` (methods),
  `parseTraitMethod`, and `parseExternFuncDecl` all use `parseParamList`/`parseReturnSuffix`.
  `tryCaptureFunctionTemplate`/`prescanTemplateNames` are name-first (`fn name<...>(...)`); the
  captured tokens keep the `fn` and the `-> RetType alias` so the monomorphized re-parse is a
  normal decl. A non-`fn` member with `(` errors ("methods must be declared with 'fn'").
- `fn`/the arrow order are purely syntactic; see the return-alias invariant for alias semantics.

### Return aliases (generalized "return slot")
`-> RetType alias` names the result binding. Required for object-value returns, optional for
primitives/references; when present, only `return;`/`return <alias>;` are allowed (`hasReturnSlot`
/`returnSlotName` on `FunctionDeclStmt`/`MethodDecl` carry it).
- **Object value** → caller-allocated **sret slot** (unchanged): hidden `ptr` first param, `void`
  LLVM return, zero-init in place. Detected in codegen by `isObjectReturnType(retTypeToken)`;
  emitted names go in `slotReturningFns_` / drive `currentFnHasReturnSlot_`.
- **Primitive** → an ordinary zero-initialized returned local (`setupReturnAliasLocal`,
  `currentReturnAliasLocal_`); `genReturn`/fall-through do `emitReturnAlias` (load + `ret`).
- **Reference** → a null-initialized returned local that **must be definitely assigned before
  return**. Semantic reuses `Symbol::isInitialized`: `setupReturnSlot` declares a reference alias
  `isInitialized=false`; `analyzeReturn` checks it on bare `return;`, and
  `checkReturnAliasAssignedAtExit` checks the fall-through path (branch merges via the existing
  snapshot machinery). Codegen returns it with the +1 retain convention.
- Void + alias → error; object return with no alias → "requires a return alias".

### Default parameter values + named arguments
A parameter may declare `= expr` (`ParamDecl::defaultValue`, a `unique_ptr<Expr>` — `ParamDecl` is
thus move-only). A call may pass arguments **positionally**, **by name** (`f(x: 1, y: 2)`), or
**mixed** (positional prefix, then named). Defaults are filled per-call **at the call site**.
- **Defaults may sit on ANY parameter** (the old contiguous-trailing rule was removed once named
  args landed). `parseParamList(bool allowDefaults)` only still rejects defaults on `extern`. A
  purely **positional** call can only omit a *trailing* run of defaults (left-to-right binding); a
  **named** call can skip a defaulted middle param. `countTrailingDefaults` = the trailing run (for
  the friendly arity message + the legacy `numDefaults`); per-slot presence is `paramHasDefault`.
- **Parser**: named args parsed by `parseCallArgs(names, close, allowNames)` — the shared arg-list
  helper used by every user call site (free/method/static/ctor/`new`). `IDENTIFIER ':'` (unambiguous
  — no expression begins that way) marks a name; a positional arg after a named one errors. Each
  arg-carrying node (`CallExpr`/`MethodCallExpr`/`NewExpr`) gained a parallel `std::vector<Token>
  argNames` (empty ⇒ all positional). Braces (`Point{...}`, `new Point{...}`) stay **positional**.
- **Semantic** (`analyzeParamDefaults`): each default is analyzed in the **enclosing scope** with
  `currentClassName` cleared and **before params are declared** (so it can't reference the
  function's own params / `this` / fields); `checkCast` against the param type.
- **Overload resolution** (`resolveOverload`, now taking `argNames` + a `nodeKey`): a per-candidate
  slot map binds the positional prefix to slots `0..k-1`, then each named arg to its slot **by name
  in that candidate** — so **names participate in selection** (an unknown/duplicate/positionally-
  taken name, or an unfilled slot with no default, makes the candidate non-viable). Viable
  candidates rank by the existing conversion cost over their mapped slots; ties break on the
  contextual return type. `ClassInfo::Method` / `FunctionOverload` / `OverloadCand` gained
  `paramNames` + `paramHasDefault`. Errors: "unknown parameter name 'x'", "…given more than once",
  "no argument for required parameter 'x'".
- **Semantic→codegen handoff**: for a **named** call, the winner's per-slot permutation (slot →
  written-arg index, `-1` = default) is recorded in `SemanticResult::callArgOrder` keyed by the call
  node. A purely positional call records nothing (codegen keeps its identity-order path unchanged).
- **Codegen** (`funcDefaults_`: emitted-callee-name → **full-length** `vector<const Expr*>`, nullptr
  where no default): `buildArgString(args, params, defaults, order)` — when `order` is present it
  evaluates written args in **source order** (side effects), then assembles the call in **parameter
  order**, filling `-1` slots from `defaults[slot]`. Threaded through every call site (free/method/
  static/ctor/`new`/sret) via `orderFor(node)`. Positional calls take the unchanged path.
- **v1 limitation**: named args are **rejected on the callable-object sugar** `obj(args)` (it lowers
  through `genTraitMethodCall`, which doesn't reorder) — call `obj.call(args)` explicitly, which is a
  normal method call and does reorder. Braces stay positional. Everything else (free/method/static/
  ctor/`new`/generic/sret) supports named args and reorders correctly.

### Tuple types & literals — anonymous value objects `(T1, T2, …)`
Anonymous tuples are **synthesized value-object classes**, not a new type kind — so the whole feature
is a thin front-end layer and codegen/semantics treat a tuple as an ordinary class.
- **Type syntax** `(T1, T2, …)` (arity ≥ 2; `()` reserved for a future empty-collection pattern,
  `(x)` stays grouping): `consumeTypeCore` (`Parser.cpp`) branches on `LEFT_PAREN` →
  `consumeTupleType`, which parses the element types, mangles a canonical class name
  `Tuple$<e0>$<e1>…` (each element lexeme is a clean identifier), records a synthesis request in
  `gen_->tupleRequests`, and returns a single synthesized `IDENTIFIER` token — same collapse-to-one-
  token convention as `Class&`/`ptr<T>`/`Vec$i32`. It flows into the shared `&`/`*` suffix logic, so
  a tuple can be borrowed (`(i32,i32)*`) / owned-referenced (`(i32,i32)&`). `isTypeName`'s paren case
  returns `typeSpanAt(current) > 0`; `typeSpanAt` recursively validates a paren type span (≥ 2
  elements, closing `)`), so a grouped expr `(a+b)` / tuple literal `(a,b)` is not mistaken for a type.
- **v1 element restriction**: elements must be **value/primitive types** (no `&`/`*`/`?`/`ptr`
  elements → clean parse error) so the mangled name is a clean identifier. Value classes (incl.
  nested tuples, `str`) are fine.
- **Literal syntax** `(e1, e2, …)`: `parsePrimary`'s grouping branch, on a trailing `COMMA`, emits a
  **`BraceInitExpr`** (the `{...}` node) — **no dedicated tuple-literal node**. `analyzeBraceInit`
  resolves the class from the **expected tuple type** at the use site, so a tuple literal needs a
  typed context (var annotation / param / return / another tuple slot); `var t = (1,2)` with no
  annotation is the same "cannot infer" error as untyped `{...}`.
- **Class synthesis** (`Parser::synthesizeTupleClasses`, run in `runMonomorphization` **before**
  `expandReflection`): for each recorded shape, hand-build a `ClassDeclStmt` `class Tuple$… { E0 _0;
  … Tuple$…(P0 a0, …) { _0 = a0; … } }` (tokens built directly — the lexer rejects `$` in idents —
  and re-parsed via `parseDeclaration`, like a monomorphized generic/lambda class), deduped by name.
  **Object-typed elements take a reference ctor param** (`ElemType&`) that clones into the value
  field (mirrors an embedded value-object field like `class Named { String name; }`).
- **Free reuse**: `._0`/`._1` are ordinary member accesses (`_0` lexes as an identifier); nested
  `p._0._1` chains GEPs; struct layout / clone / dtor / structeq / `needsDtor` all key off the
  synthesized class — so `(String, i32)` deep-copies through `@String_clone` automatically.
- **v1 limits** (clean errors): tuple **params must be a borrow/reference** (a tuple is an object;
  by-value object params are rejected — write `(i32,i32)*`); no unit/1-tuples; tuple-type diagnostics
  render the mangled `Tuple$…` name (pretty-printing deferred). Tests: `tests/tuple_tests.cpp`
  (`[tuple]`), `e2e/tuple_test.gg`.

### `match` — destructuring pattern matching (statement + expression)
A **new front-end construct** distinct from `switch` (which stays equality-label only — mixing the
two analyses is incoherent, the C#/Java lesson). `match <scrutinee> { pattern -> body … }` (no parens
around the scrutinee — a full expr parsed up to `{`). Reuses the `switch` arm-lowering skeleton
(`genSwitchArms` → `genMatchArms`), swapping equality-labels for **pattern test-trees + bindings**.
- **AST** (`Ast.h`): a recursive `Pattern` variant (`WildcardPat` `_`, `BindingPat` `x`, `LiteralPat`
  (compared via `==`), `TuplePat` `(p0,p1)`, `StructPat` `Class{ field: sub, field }`); sub-patterns
  held as `unique_ptr<Pattern>` (breaks the completeness cycle). `MatchArm` (pattern + arrow body,
  mirrors `SwitchArm`), `MatchStmt`/`MatchExpr` (mirror `SwitchStmt`/`SwitchExpr`); `yield` reused for
  expr block arms. Free `patternIsIrrefutable(Pattern)` in `Ast.h` (shared with control-flow analysis).
- **Parser** (`parseMatchStmt`/`parseMatchExpr`/`parseMatchArmBlock`/`parsePattern`, `Parser_Stmt.cpp`;
  primary dispatch in `Parser_Expr.cpp`): `match` is pattern-first, so a bare identifier is
  unambiguously a **binding** (`x`), `_` a wildcard, `Class{…}` a struct pattern, `Enum::V`/`E.V` a
  literal (scoped/member access), else a literal (number/string/char/bool/null, via `parseUnary`).
  Bindings recorded in `scopes_` with the `var` sentinel.
- **Semantic** (`checkPattern`/`analyzeMatchStmt`/`analyzeMatchExpr`/`analyzeMatchArm`,
  `SemanticAnalyzer_Exprs.cpp`): each arm gets a fresh scope; `checkPattern` type-checks the pattern
  against the scrutinee/sub-value type and **declares bindings** (immutable, initialized) — tuple
  requires a `Tuple$…` Object/Reference with matching arity, struct requires the named class + fields,
  literal reuses `classifyEquality` (records the codegen decision keyed by the literal node).
  **Exhaustiveness** (expression form): needs an irrefutable arm (`patternIsIrrefutable`) else error.
  **Reachability** (both): an arm after an irrefutable one is unreachable → error. `alwaysReturns`
  gained a `MatchStmt` arm (exhaustive + all-arm-blocks-return).
- **Codegen** (`genMatchStmt`/`genMatchExpr`/`genMatchArms`/`emitPatternTest`/`materializeScrutinee`,
  `CodeGen_Stmts.cpp`): the scrutinee is materialized **once** into a *place* — a ptr to its storage
  (an object is already its address; any other value is spilled to an alloca) — so bindings alias the
  place, field GEPs recurse into it, and literal tests load from it. `emitPatternTest` returns an i1
  (or the literal `"true"` for an irrefutable pattern): wildcard/binding → `"true"` (+ register the
  binding in `allocaMap`/`varTypeMap`), literal → `emitEquality` (reused), tuple/struct → AND of
  sub-tests over `resolveFieldGEP`'d fields (a Reference place is `load`ed first to reach the object).
  `genMatchArms` snapshots/restores `allocaMap` per arm so bindings don't leak; the expr form reuses
  the `switchExprStack_` result slot + `yield`.
- **v1 limits / deferred**: enum-payload patterns (`Some(x)` — needs tagged unions; GG enums are
  Java-singletons), `(x:xs)` cons / collection patterns (the `()`=empty-collection direction),
  or-patterns, `if` guards, ranges, and `var (a,b) = …` destructuring-let. Tests:
  `tests/match_tests.cpp` (`[match]`), `e2e/match_test.gg`.

### Variadic parameters — compile-time type packs `<...Ts>(Ts... args)`
A heterogeneous variadic — `fn f<...Ts>(fixed…, Ts... args)` — is a **compile-time type pack**, all in
the **parser/monomorphizer** (semantics/codegen see ordinary functions over ordinary tuples). No
runtime heterogeneous list / RTTI. Built on tuples (packs materialize as tuples) + generic
monomorphization.
- **Lexer**: `...` → `ELLIPSIS` (peek both trailing dots; a lone `.` stays `DOT`).
- **Pack type param** `<...Ts>`: `scanTypeParamList` marks it; `GenericTemplate::isPack` (parallel to
  `typeParams`) threaded through the capture sites. v1: **one pack, last type param**. Pack **value
  param** `Ts... args` (`ParamDecl::isVariadic`, `parseParam`; must be the last param).
- **Call-site collection** (`deduceVariadicInstantiation`, `Parser_Expr.cpp` free-fn path): split the
  call's fixed prefix from the trailing pack; **deduce** each pack element's type at parse time
  (`deduceArgTypeToken` — literals default-typed `1`→i32/`1.0`→f64/`"x"`→str/bool/char, in-scope
  identifiers, ctor/`new`, `this`, and a **field access** `obj.field` — resolves `obj`'s class (either
  a known class name directly, for the static `Class::field` form — `Class::field` and `obj.field`
  parse to the identical `MemberAccessExpr` node — or recursively via `deduceArgTypeToken`, for every
  other receiver shape), then looks up `field`'s declared type in `GenericRegistry::classFieldTypes`
  [class/enum name → field name → type token, populated for EVERY class as its member list is parsed,
  regardless of which class is "current" — see `Parser::parseMemberList`]. The looked-up field type is
  decayed through `genericArgBaseName` (strip a trailing `?`/`&` or a `ref:` borrow prefix) exactly
  like an identifier's declared type already is, before it can reach the tuple mangling — a
  reference/borrow/nullable-typed field must never contribute a raw sigil-bearing lexeme to
  `requestTupleType` (`&`/`:`/`?` are not valid unquoted LLVM identifier characters). So
  `printf("{}", v.x)` works for any field (plain, reference, or static) of any already-parsed class,
  chaining through `a.b.c` as long as each step is a plain field]; build the pack tuple
  (`requestTupleType`, allows internal unit/1-tuples),
  mangle `f$Tuple$…`, and rewrite the args to `[fixed…, one tuple literal]`. A trailing **spread**
  `xs...` (parsed by `parseCallArgs`) passes an existing pack tuple through directly **when the
  target `f` is itself pack-bearing** (re-tuples/re-instantiates, as above). When the target is
  **NOT** pack-bearing — an ordinary function, an ordinary non-pack generic function, a constructor,
  a method, or `new` — the spread instead **unwraps**: `Parser::unwrapSpreadArgs`
  (`Parser_Generics.cpp`, alongside `deduceVariadicInstantiation`) splices `xs._0, xs._1, …,
  xs._{N-1}` (`MemberAccessExpr` nodes, `N` read directly off `gen_->tupleRequests[tupleName]` — no
  mangled-string splitting) into the argument list in place of the spread, so ordinary overload
  resolution then does all arity/type checking exactly as if they'd been hand-written. Since packs
  are v1-scoped to **free functions only** (a class or method can never be pack-bearing — see below),
  only the ONE shared bare `IDENTIFIER ( args )` call site (used for both free-function calls and
  constructor calls) needs the "is the target pack-bearing" discriminator
  (`gen_->templates.find(name)` + `isPack.back()`, **not** `gen_->funcNames`, which only tracks
  generic names and would wrongly skip an ordinary non-pack generic function's spread); every other
  call form (method, `new`, var-decl constructor sugar `Point p(args...)`/`p{args...}`, explicit-`<T>`
  generic calls) unconditionally unwraps any spread, since none of those targets can ever be
  pack-bearing. **v1 limits on unwrap** (clean errors): at most one spread per call (position need
  not be trailing, unlike the pack-forward case); the spread target must be a bare in-scope
  identifier — the only two shapes a pack variable ever has (the pack parameter itself, or a
  cons-match tail-pack local) are always bare identifiers by construction, and there is no
  `Expr`-clone utility in the codebase to safely duplicate anything more complex (e.g. `data[i]...`)
  without risking re-evaluating a side-effecting expression N times; a named spread argument
  (`f(x: xs...)`) is rejected. Spreading into an **inferred** (no-`<…>`) generic function
  (`identity(args...)`) is a clean "cannot infer" error — the unwrapped `args._k` is a tuple-field
  access, and generic inference (like `deduceArgTypeToken`) doesn't read a type through a
  `MemberAccessExpr`; the explicit `identity<i32>(args...)` works (a constructor / plain function /
  method target needs no inference, so those are unaffected). The brace-form primary expression `ClassName{ args }` (a separate
  hand-rolled arg-parsing loop, no named-arg support either) doesn't support spread in v1 — the paren
  form covers the need. This closes the second of two gaps toward a real `Array<T>::emplace_back`
  (the first — direct-construct `ptr<T>` buffer elements — is documented separately above); combined,
  `data[i] = Point(args...)` now forwards an arbitrary compile-time pack straight into the element's
  own constructor with zero temporary object and zero clone.
- **Monomorphization** (`runMonomorphization`): the pack param `Ts... args` substitutes to
  `Tuple$…* args` (a tuple borrow). For a variadic template, the self-reference rename applies **only
  to the decl name** (`tokens[1]`), NOT recursive *calls* — so `f(…, xs...)` re-deduces the SHORTER
  pack each level (else infinite self-recursion over a fixed pack).
- **Consumption = compile-time cons-`match`** (`expandPackMatch`, token-level, run per instantiation
  where the pack arity is known): `match args { () -> A; (x:xs) -> B }` → the arm chosen by arity —
  arity 0 → `A`; else `B` with head `x` rewritten to `args._0` and tail `xs` materialized as a
  tail-tuple local (`Tuple$…(args._1, …)`, unit for arity 1). The recursive `f(…, xs...)` unrolls to
  the `()` base case; terminates because the pack strictly shrinks. Distinct from the runtime `match`
  (arms bind different types per arity — must be comptime).
- **v1 limits** (clean errors): heterogeneous packs only (no homogeneous `i32...`→Array, no C-extern
  `printf(ptr,…)`); pack element types must be **parser-deducible** (a nested-**call** arg, e.g.
  `f(g()).y`, → "cannot infer", same limitation as generic inference incl. forward-reference — a
  field access on an already-parsed class, e.g. `v.x`, IS deducible, see above); explicit `<…>` on a
  variadic is rejected (the pack is inferred); pack elements are value/primitive (tuple-element
  rules); pack trait bounds deferred (overloaded helpers like `weight(x)` per element suffice). A
  field access on a **synthesized tuple** (`args._0`, from a spread-unwrap) is NOT deducible even
  though it's the same `MemberAccessExpr` shape — the tuple class isn't synthesized/parsed until
  `runMonomorphization` (well after the call site's own parse), so `classFieldTypes` has no entry for
  it yet; this is what keeps `identity(args...)` (no explicit `<i32>`) a clean "cannot infer" error
  (the explicit `identity<i32>(args...)` is unaffected — it needs no inference). Tests:
  `tests/variadic_tests.cpp` (`[variadic]`), `e2e/variadic_test.gg`,
  `e2e/variadic_spread_unwrap_test.gg`.

### Variadic methods — `fn m<...Ts>(Ts... args)` on a class
A variadic **method** = the generic-method machinery + the variadic-pack machinery, composed. The
pack is collected at the call site into a tuple and consumed by compile-time cons-`match` / spread,
identically to a free variadic function; the concrete `@Owner_m$Tuple$…` is injected into the class
by the generic-method transplant path. The **flagship use is `Array<T>::emplaceBack`** — a zero-copy
emplace (`data[count] = T(args...)` direct-constructs the element straight into the buffer slot, no
temp, no clone). Works on **generic classes** (`Array<T>`) via the same `pendingMethodInsts_` deferral
fixpoint as ordinary generic methods.
- **Capture** (`captureMethodTemplate`, `Parser_Generics.cpp`): a pack type param (`<...Ts>`) is now
  **allowed** on a method (the old "variadic methods are not supported yet" rejection is gone); the
  pack must be the **last type param** (else a clean "must be the last type parameter" error), and
  `GenericTemplate::isPack` is stored on the method template exactly as for a free function.
- **Prescan / registry** (`GenericRegistry`): a class-body `fn NAME<...>(` registers into the parallel
  **variadic** sets `variadicMethodNames` (bare) / `variadicMethodKeys` (`Owner::NAME`) instead of the
  plain generic-method sets, plus `variadicMethodFixedCount[Owner::NAME]` = the count of **fixed**
  params before the pack (via `fixedParamCount`). The fixed count is recorded in the **prescan**, not
  at the call site, because a generic-class variadic method's concrete template (`Array$Point::
  emplaceBack`) doesn't exist yet when the initial-parse call site runs. The prescan **skips leading
  `static`/`private` modifiers** between `fn` and the name (so `fn static m<...>` / `fn private m<...>`
  register — this also fixed a latent cross-file gap for `static`/`private` *generic* methods, which
  share this prescan). `captureMethodTemplate` **backstops** the variadic sets too (as it already does
  the generic sets), so a same-file variadic method whose class is declared *after* a call site still
  resolves without depending on the prescan.
- **Call site — inferred, no `<…>`** (`parsePostfix`, `Parser_Expr.cpp`, a branch **before** the
  spread-unwrap): gated on `member ∈ variadicMethodNames`. Resolves the receiver class (an identifier
  → static form / `deduceArgTypeToken` for this/field/local; `$`-suffix stripped for the base-class
  key), looks up `variadicMethodFixedCount[baseClass::member]`, requires positional args, then calls
  `deduceVariadicMethodInstantiation(recvClass, member, fixedCount, args, spreads)`. That reuses the
  shared `deducePackTargs` core (extracted from the free-function `deduceVariadicInstantiation` — it
  validates spread position, deduces each pack element's type, `requestTupleType`s the pack, and
  rewrites the args to `[fixed…, one tuple literal]`), then `mangleInstantiation` +
  `recordMethodInstantiation`. The mangled method token flows through an ordinary `MethodCallExpr`.
- **Monomorphization** (`instantiateMethod`): when the method template `isPack.back()`, the pack type
  param substitutes to a `Tuple$…* args` borrow (capturing the pack value-param name), and after the
  ordinary type-param splice runs `expandPackMatch(out, packValueName, elems)` — the same token-level
  cons-`match` expansion as a free variadic function (arity-selected arm, head `x` → `args._0`, tail
  `xs` materialized as a tail-tuple local). A recursive `this.m(base + x, xs...)` re-instantiates the
  shorter pack, unrolling to the `()` base case.
- **v1 limits**: the pack is **always inferred** (explicit `<…>` on a variadic method is not a thing);
  everything else inherits the free-variadic v1 limits (heterogeneous packs, parser-deducible pack
  args) and the generic-method v1 limits (parser-visible receivers only; impl/trait-block methods
  rejected). Tests: `tests/variadic_tests.cpp` (`[variadic][genericmethod]`),
  `e2e/variadic_method_test.gg`; the stdlib `Array<T>::emplaceBack` (`stdlib/Array.gg`) is the
  real-world consumer.

### Primitive cast-call syntax — `PrimitiveType(x)` ≡ `x as PrimitiveType`
A primitive type keyword immediately followed by `(` is a cast-call: `i32(x)` means `x as i32`.
This exists specifically to make the generic "construct `T` from its packed args" idiom —
`T(args...)`, the exact shape `Array<T>::emplaceBack`/`emplace` use (see above) and the general
bare-constructor-call convention this parser already applies everywhere — work when `T`
monomorphizes to a **primitive** rather than a class. A class instantiates via an ordinary
constructor call; a primitive has no constructor, so casting its single (packed) argument to it is
the only meaning that makes sense. Before this, `Array<i32>::emplaceBack(42)` (or any generic
method using the `T(args...)` pattern with a primitive element type) was a hard **parser** error —
`i32(args...)` after substitution left a bare `i32` keyword token followed by `(`, and
`parsePrimary`'s catch-all "a bare type keyword leaked into expression position" diagnostic fires
unconditionally on any type keyword in expression-starting position, with no exception for what
follows. Confirmed nobody had ever exercised `emplaceBack` with a primitive element type before —
every existing test/e2e file used a class `T` (`Vec<Point>`, never `Vec<i32>`).
- **Parser** (`parsePrimary`, `Parser_Expr.cpp`): a new branch checked via `isTypeKeyword(peek().type)
  && peekNext().type == LEFT_PAREN`, inserted immediately before the catch-all error. Parses the
  argument list exactly like any other call (`parseCallArgs`, so a spread `args...` unwraps via
  `unwrapSpreadArgs` exactly as it already does for an ordinary bare constructor call — a primitive
  cast is never itself pack-bearing), requires **exactly one** argument (a primitive can't be
  "constructed" from zero or several values — a clear error otherwise), and produces an ordinary
  `CastExpr{ operand, targetType }` — the same node the `as` operator builds. **Purely additive**:
  a bare type keyword in expression position was *unconditionally* a hard parse error before this,
  and neither of that error's own two documented causes (`f(i32)` passing a type as an argument;
  `Name<i32>(...)` misparsed as a comparison chain when `Name` isn't a declared generic) ever
  produces a type keyword *immediately* followed by `(` — in both those cases the keyword is
  followed by `)` or `>` respectively — so this new branch's trigger condition never overlaps with
  either, and nothing that used to compile is affected.
- Tests: `tests/generic_method_tests.cpp`/`tests/variadic_tests.cpp` exercise `Array<i32>`-shaped
  variadic-method calls; verified via a minimal non-stdlib repro (`class Vec<T> { fn
  emplaceBack<...Ts>(Ts... args) mut { v = T(args...); } }` instantiated at `T = i32`) and the full
  `Array<i32>` stdlib path (`push`/`emplace`/`pop`/`delete`/`contains`, see below).

### Switch — statement and expression (Java arrow form)
Java-style `switch`, arrow form only (`case L1, L2 -> body;` / `default -> body;`, **no**
fall-through, no `break`). Two AST nodes: `SwitchStmt` (Stmt) and `SwitchExpr` (Expr), sharing a
`SwitchArm` (`std::deque<SwitchArm>` — `SwitchArm` has a `Token` and `unique_ptr`s, move-only). An
arm body is either `-> expr ;` (`valueExpr`) or `-> { ... }` (`block`). `yield expr;` (`YieldStmt`)
produces a block arm's value in the expression form.
- **Parser**: `switch` in statement position → `parseSwitchStmt`; in expression position (primary)
  → `parseSwitchExpr`; shared `parseSwitchArmBlock`. `yield` → `parseYieldStmt`. At most one
  `default` (parse error otherwise). New keywords `SWITCH`/`CASE`/`DEFAULT`/`YIELD`.
- **Equality reuse (the key design)**: case labels compare to the scrutinee via the **same
  `==` machinery**, factored into `SemanticAnalyzer::classifyEquality(left,right,nodeKey,at,what)`
  (records `resolvedCallee`/`addressIdentityCmp_`/`structuralValueCmp_` keyed by the **label node**)
  and `CodeGen::emitEquality(nodeKey,lval,lt,rval,rt,op)` (emits structeq / `icmp eq/ne ptr` /
  `@Class_eq` / primitive `icmp`/`fcmp`). `analyzeBinary`/`genBinary` `==`/`!=` now delegate to
  these too. So: `Eq` impl → its `eq`; else same-class refs → address identity; else value object →
  structural; enums → identity; primitives → numeric compare.
- **Exhaustiveness**: a switch **expression** must be exhaustive — an explicit `default`, **or** an
  enum scrutinee whose arms cover every variant (`covered == EnumInfo::variantSet`; variant name
  read from each label's `MemberAccessExpr::field`). A switch **statement** never requires `default`.
- **Duplicate labels**: `checkDuplicateLabels` (both forms) errors on repeated compile-time-
  identifiable labels — int/char/bool/string literals, negated int literals, enum variants, and
  bare identifiers (canonical key via `labelKey`). Non-constant labels (arbitrary/value-object
  expressions) are skipped (can't be compared at compile time).
- **Arm typing (expr)**: result type = `expectedType_` (set by `i32 y = switch…`, `return switch…`,
  etc.) else inferred from the first arm; each arm value/`yield` `checkCast`-ed to it. Value-object
  results are rejected ("use a reference") — deferred. `yield` is valid only inside a switch-
  expression block arm (`switchExprResultStack_` / codegen `switchExprStack_`), checked against the
  result type; a block arm must `yield`/`return` on every path (`blockAlwaysYields`, mirrors
  `alwaysReturns`, which is itself extended so an exhaustive all-returning switch counts).
- **Codegen**: scrutinee evaluated **once**; a linear test-chain (`genSwitchArms`) ORs each arm's
  label comparisons and `emitCondBr`s to the arm body or the next test; `default` is the final
  fallthrough. Expression form allocates a result slot, each arm stores into it, loaded after the
  `sw.merge` block. `break`/`continue` inside an arm bind to an enclosing loop (switch pushes nothing
  on the loop stacks). Not an LLVM `switch` instruction (labels may be arbitrary `eq` calls).

### Ternary conditional — `cond ? a : b`
A C-style ternary, implemented as a **parser-only desugar** to a switch **expression** — so it adds
**no** AST node and **no** semantic/codegen. `parseTernary` (`Parser_Expr.cpp`) sits between
`parseAssignment` and `parseElvis` in the precedence ladder (`parseAssignment` calls it; it calls
`parseElvis` for the condition), right-associative (`a ? b : c ? d : e` = `a ? b : (c ? d : e)`).
- **Desugar**: `cond ? a : b` → `SwitchExpr{ scrutinee: cond, arms: [ case true -> a; default -> b ] }`
  (a synthesized `true` `LiteralExpr` label + a `default` arm). The condition is an Elvis-level expr;
  the middle is a full expression (`parseAssignment`); the else recurses (`parseTernary`).
- **Reuses switch-expression semantics/codegen wholesale**: bool scrutinee compared to `true` via
  `classifyEquality`, both branches unified to `expectedType_`, exhaustive via `default`, and the
  result-slot lowering in `genSwitchExpr`. So a **non-bool condition** errors ("incompatible types in
  switch case: T and bool" — the message leaks the desugar, a known cosmetic wart), and **value-object
  branches** are rejected ("use a reference") exactly as in a switch expression. `?` lexes as a lone
  `QUESTION` here (distinct from `?:`/`?.`/the `T?` type suffix), so there's no grammar conflict.
- Tests: `tests/ternary_tests.cpp` (`[ternary]`).

### `for (DECL : ITERABLE)` — Java-style range loop + the `Iterator` trait
A range `for` loop over anything iterable, implemented as a **parser-only desugar** (like ternary /
lambdas / tuples) — **no** new AST / semantic / codegen. `for (DECL : COLL) BODY` lowers (in
`parseForInStmt`, `Parser_Stmt.cpp`) to:
```
{ mut var __forit_N = (COLL).iter(); while (__forit_N.hasNext()) { DECL = __forit_N.next(); BODY } }
```
The programmer never writes `.iter()` — the desugar inserts it. `next()` returns a `T*` borrow, so
`DECL` binds it by the ordinary rules: a primitive `i32 x` **loads** the value, a borrow `Point* p`
binds the pointer directly.
- **Range detection** (`parseForStmt`): after `for (`, a scan for a `:` at paren-depth 1 that is **not**
  a ternary colon (tracked by matching `?`) and precedes any `;` → the range form. A `::` (`COLON_COLON`)
  is a distinct token, so `Class::x` in the header is never mistaken for it; a C-style `for` with a
  ternary in its condition stays C-style (the `?`-tracking skips that colon).
- **Loop-variable form** (enforced syntactically in the parser, since element-kind isn't known there):
  the loop variable must be a **primitive keyword** (`i32 x` — bare value, allowed) or a **borrow**
  (`T*` — any element type). A bare **object/enum** loop variable (`Point p`, `Color c`) is a clean
  error ("iterate 'Point' by borrow: … 'Point* p'") — objects/enums are iterated by borrow, only
  primitives by value. `mut T*` gives a mutable borrow (write-through to the element).
- **The `Iterator` built-in trait** (`SemanticAnalyzer.cpp`): `Iterator` is a **built-in** trait (added
  to `isBuiltinTrait` + the `builtinMethod` map like `Index`/`Clone`), recognized by convention —
  conformance requires the two conventional methods `fn hasNext() -> bool` and `fn next() -> T*`. Being
  built-in means the trait carries **no written signature**, so `impl<T> Iterator for ArrayIter<T>`
  supplies the concrete `next() -> T*` (the element type `T` bound by the **impl's** own `<T>`, exactly
  as `impl<T> Index for Array<T>`'s `get() -> T*` does) — **no parameterized traits / associated types
  needed**. (An abstract algorithm *bounded* on `Iterator` that consumes the element — `fn sum<I:
  Iterator>` — would still need associated types to name the element type; that's the only deferred
  case, and `for-in` sidesteps it because the loop declares the element type explicitly.)
- **The `Iterable` built-in trait** (the collection side): `Iterable` is a **built-in** trait too
  (`isBuiltinTrait` + `builtinMethod` map `{"Iterable","iter"}`), the symmetric partner of `Iterator` —
  a collection declares `impl<T> Iterable for Array<T> { fn iter() -> <cursor> … }` to formally mark
  itself iterable. Conformance is by convention (the `iter` method must exist — no written signature,
  so the impl supplies the cursor return type; no associated types needed, same as `Iterator`/`Index`).
  `impl Iterable` with no `iter()` is a clean "must define method 'iter'" error.
- **Iterability of the for-in itself is still by convention** (unchanged): `for (x : coll)` lowers to
  `coll.iter()` and does **not** gate on `Iterable` conformance — a collection needs an `iter()`
  returning a cursor with `hasNext`/`next`, whether or not it also `impl Iterable`s. A missing `iter()`
  is the ordinary "no method 'iter'" error (a leaky-desugar wart, same class as ternary's). `Iterable`
  is the formal marker + `@implements(T, Iterable)` query; wiring the for-in gate onto it is a future
  polish. A fresh `iter()` per loop → independent cursors, so loops nest and re-run (verified:
  `e2e/foreach_test.gg` nests two cursors).
- **Source binding for temp-producing iterables** (`parseForInStmt`): when the iterable is a **call /
  `new`** (a temporary), the desugar first binds it to `mut var __forsrc_N = ITERABLE;` and iterates
  `__forsrc_N`. This keeps the collection alive for the whole loop — **without it a reference-returning
  call iterable is a use-after-free**: the call's `+1` temp is released (running the collection's dtor,
  freeing the buffer) at the `.iter()` full-expression boundary, *before* the loop body runs (confirmed
  in IR: `gg_release(…, @Array$i32_dtor)` emitted between the `iter()` call and `while.cond`). A plain
  **place** (identifier / member / `this` / index) is iterated directly — no `__forsrc`, no copy (so
  the common `for (x : arr)` is unchanged). Verified: `e2e/foreach_test.gg` iterates a `make() ->
  IntVec&` whose `IntVec` has a `~IntVec` that frees the buffer, and gets the right sum.
- **A value-returning (sret) call iterable now works** (`for (x : f())` where `f` returns an object
  **by value**): the `__forsrc_N` binding is a `mut var`, and inferring a `var` from an sret call was a
  **pre-existing bug** (`var x = <sret call>` silently failed to declare `x`) — now fixed (see the
  "`var` inference from an sret method call" invariant / `resolveTypeToken`'s `declaredClassNames_`
  fallback). Reference-returning calls (`-> T&`), `new`, locals, fields, and index places all work too.
- **stdlib** (`stdlib/Array.gg`): `class ArrayIter<T>` holds a **non-owning borrow of the collection**
  (`Array<T>*`) plus an index — NOT a snapshot of the buffer pointer — and reads the length/elements
  **live** (`fn hasNext() { pos < arr.size() }`, `fn next() { arr.get(pos as i32) }`). `impl<T> Iterable
  for Array<T>` builds the cursor **in place in the return slot** (`fn iter() -> ArrayIter<T> it { it.arr
  = this; it.pos = 0; return it; }`) — a stack value object via sret, **no heap allocation per loop**,
  **no refcount** (`it.arr = this` is the Silent Object→`Class*` coercion, no cast), and **no cursor
  destructor** (a borrow owns nothing). Because reads go through the live Array, a `push`/`grow()` that
  reallocates the buffer **during** iteration is harmless — `next()` always uses the current buffer (a
  snapshot cursor would dangle on the freed old buffer). The borrow field requires `--unsafe-ptr` (see
  the borrow-fields note under the `T*` invariant); it is safe here because a for-loop cursor never
  outlives its collection. Verified: `e2e/foreach_realloc_test.gg` grows the Array mid-iteration and
  does not crash.
- **Copy-guard refinement (related fix)**: the raw-ptr-owner copy guard (`memberwiseCopyAliasesRawPtr`,
  `SemanticAnalyzer_Exprs.cpp`) now fires only when the class has a **user destructor** — a raw-ptr
  *view* with no dtor (an iterator holding a non-owning buffer pointer) never frees the buffer, so its
  shallow copy is safe. This lets a **stack-value** iterator (`fn iter() -> IntIter it { it =
  IntIter(…); … }`, no `new`) return/copy without a spurious "cannot copy … owns a raw pointer" error
  (a generated dtor from ref fields never frees a raw ptr, so keying on the *user* dtor is correct;
  `[clone]` still green). Invalidation (a mutate-during-iteration `modCount` fail-fast trap) is
  deferred. Tests: `tests/foreach_tests.cpp` (`[foreach]`), `e2e/foreach_test.gg`.

### Lambdas & callable objects (`Call` trait)
Any class implementing the **`Call` trait** is callable: `obj(args)` desugars to `obj.call(args)`.
Lambdas `(params) -> [Ret] { body }` are **parser-only sugar** — they lower to a hoisted value-object
class implementing `Call`, so semantics/codegen gain **no new AST nodes**, only the `Call` desugar.
- **Signature-carrying bound** `Call(P…) -> R` canonicalizes (parser) to a mangled **user trait**
  `Call$P…$ret$R` (`fn call(P…) -> R`), auto-emitted once per signature (dedup in
  `GenericRegistry::emittedCallTraits`). Both a bound and an `impl Call for X` (signature derived
  from its `call` method) reduce to the same name, so conformance/bound-checking/body-checking reuse
  the **existing user-trait pipeline** — a signature mismatch is "does not satisfy bound
  `Call(i32)->i32`" (demangled by `prettyBound`). `mangleType`+`typeFromToken`/`decodeSynthesizedType`
  compute the canonical name identically on both sides. `readCallSigType`/`scanCallBound`/
  `canonicalCallTrait` in `Parser.cpp`; recognized in bound (`scanTypeParamList`) and impl positions.
- **`obj(args)` desugar** (`analyzeCall`): a variable whose class implements a `Call$…` trait →
  resolve its `call` overload, record `SemanticResult::callableCalls[node]=class`; a bounded type
  param `F: Call$…` resolves via `resolveBoundMethod` (extended to also **validate argument types**,
  not just arity). Codegen `genCall` checks `callableCalls_` → receiver (callee var's alloca / loaded
  ptr) + `genTraitMethodCall(…, "call", …)`.
- **Untyped/inferred lambda params** (`finishLambda` + `expectedLambdaSig_`): a lambda argument to a
  single-`Call`-bounded-param generic may omit parameter types and the return type — they're filled
  from that bound's stored signature (`gen_->callTraitSigs`, keyed by canonical name, threaded into
  arg parsing). Forms: `x -> {…}` (bare single param), `(x) -> {…}`, `(a, b) -> {…}`; explicit
  `(i32 x) -> i32 {…}` still works. A bare `IDENTIFIER ->` is a lambda **except** inside a switch
  case label (`parsingCaseLabel_` guards it, since `case x -> body` uses `->` as the arm separator).
  An untyped lambda with no inferable signature is a clear error.
- **Lambda desugar** (`parseLambda`/`finishLambda`): generate `class __lambda_N { <capture fields>; ctor }` +
  `impl Call for __lambda_N { fn call(params)->ret { body } }`, append to `Program` (like
  monomorphization); replace the literal with `__lambda_N(capturedVars…)` (stack value object).
  Omitted return type = `void`. Captured names become **same-named fields**, so the body reuses
  implicit-`this` unchanged.
- **By-value capture** (`scopes_` name→type-token stack seeded from params via `pendingScopeSeed_`;
  `classFieldScope_` for instance fields): enclosing locals/params/instance-fields used in the body
  are copied into fields (primitives copied, references retained → auto clone/dtor). Captured names
  used **bare** (not `this.field`). **Non-escaping** value objects; **nested lambdas rejected**;
  a lambda type is unspellable so it's usable **only as a literal argument** to a generic with
  **exactly one `Call`-bounded type parameter** (inference in the call-parsing path via
  `gen_->lambdaClassNames`, shared for monomorphization re-parse). Object params must be references,
  so consumers take `F& f` (the lambda/callable value borrows in).
- **Also fixed generally**: a constructor-call **rvalue** `Class(args)` (e.g. as an argument) now
  materializes a temp value object and yields its address (`genCall`), so it borrows correctly as a
  `Class&` — previously emitted a bogus `@Class(...)` struct value.

### Overloading (functions, constructors, methods)
Functions, constructors, and methods (instance & static) may be **overloaded** by parameter
signature **and/or return type**. Enums are excluded (single ctor, no member overloading).
- **Storage as overload sets**: `ClassInfo::methods` is `unordered_map<string, vector<Method>>`;
  free functions live in `SemanticAnalyzer::functionRegistry` (`vector<FunctionOverload>` per
  name) with a single Function marker kept in the symbol table for shadowing / "not a value".
  Exact-signature duplicate = redefinition error; `extern`/`main` can't be overloaded.
- **Resolution** (`resolveOverload`, `SemanticAnalyzer_Exprs.cpp`): analyze args once, keep
  candidates with matching arity + all args implicitly convertible, rank by conversion cost
  (exact 0 / `Silent` 1 / `Warn` 2 via `canImplicitlyCast`), pick unique min; ties broken by
  the **contextual expected type** on the return type (`expectedType_`, threaded via
  `analyzeWithExpected` at var-init / assign / return / member-assign / cast-target). No
  context + tie ⇒ "ambiguous" (fix with `expr as T`). Zero viable ⇒ "no matching overload"
  (single-candidate arity mismatch keeps the friendlier "expects N argument(s)").
- **Mangling** (`mangleOverload`/`mangleType` in `Type.cpp`): `base$p1$p2$ret$R`, e.g.
  `foo$i32$f64$ret$void`, ctor `Point_Point$i32$ret$void`. Applied **only when a name is
  actually overloaded** (decl count > 1) so non-overloaded symbols keep their plain names
  (`@foo`, `@Point_dist`) and existing IR-name assertions stay valid. `main`/`extern` never
  mangled. **Every mangled component must be a valid unquoted LLVM identifier** (`[A-Za-z$._0-9]`) —
  `$` separates components, `.` marks a variant: an **owning** heap reference `Class&` → `Class.ref`;
  a **non-owning borrow** `Class*` (and a bare-object param, which is a borrow) → `Class.brw`; a
  **primitive borrow** `i32*` → `i32.brw` (the element type is encoded because a primitive borrow's
  className is empty); array → `.arr`; **nullable → `.opt`**. The borrow (`.brw`) vs owning (`.ref`)
  distinction is load-bearing: both lower to `ptr` in IR, so if they shared a suffix, overloading one
  against the other (`f(Obj o)` vs `f(Obj& o)`, or two distinct primitive borrows `f(i32*)`/`f(f64*)`)
  would emit two definitions with the same symbol — a raw clang "invalid redefinition". Conversely
  `f(Obj o)` and `f(Obj* o)` ARE the same lowered type (both `Obj.brw`) and are correctly reported as a
  clean duplicate-signature error. A nullable type mangles as `<underlying>.opt` (`i32?` → `i32.opt`),
  **not** `typeName`'s `i32?` — a literal `?` is not a legal symbol char and made clang reject the
  emitted `.ll` (e.g. an overloaded `parseI32 -> i32?`).
- **Semantic→codegen handoff**: `SemanticResult::resolvedCallee` (`const void*` call/new node
  address → chosen mangled name; empty ⇒ plain). Codegen: `overloadedBases_` + `freeFnBases_`
  computed in `generate()`; `genFunction`/`genMethod` emit `overloadEmittedName(...)`;
  `funcParamTypes` keyed by the emitted name; call sites (`genCall`/`genMethodCall`/
  `genStaticCall`/`genNew` + stack-ctor + implicit-`this`) resolve via `calleeName(node, base)`.

### Traits & operator overloading
Rust-style traits with **static dispatch only** — no vtables, no trait objects, no generic
bounds. Reuses the overloading `resolvedCallee` node→symbol handoff for operator desugaring.
- **Tokens/AST**: `trait`/`impl`/`Self` (`TRAIT`/`IMPL`/`SELF`). `MethodDecl` gained
  `bool hasBody` (trait signature-only methods set it false). New stmt nodes
  `TraitDeclStmt{ name; deque<MethodDecl> methods }` and
  `ImplDeclStmt{ traitName; typeName; deque<MethodDecl> methods }`. `Self` is a type keyword,
  accepted by `isTypeName()`/`consumeType()` (synth tokens `"Self"` / `"Self&"`).
- **`impl` methods ARE class methods**: `collectImpls` registers each into the target class's
  `ClassInfo::methods` overload set (with `Self`→type) and codegen emits them via
  `genMethod(impl.typeName, md)` as `@Type_method`. So overloading, `mut`, implicit-`this`,
  mangling, and `funcParamTypes`/`funcReturnTypes` all apply unchanged. Impl target must be a
  **class** (not primitive/enum). `Self` substitution: `resolveTypeToken` (semantic, via
  `currentSelfType_`) and `resolveParamType`/`resolveReturnType` (codegen, via
  `currentClassName_`).
- **Registries** (semantic): `traitRegistry` (name→`TraitDeclStmt*`), `implementedTraits`
  (type→set of trait names). `collectTraits`/`collectImpls` run after `collectClasses`.
  Conformance: builtin trait ⇒ the conventional method must exist; user trait ⇒ every required
  signature present (after `Self` subst). **Default (bodied) trait methods are rejected** —
  `collectTraits` errors on `md.hasBody` ("not yet supported"). v1 = required-methods-only.
- **Built-in operator traits** (no prelude; recognised by name via `operatorTraitFor(TokenType)`
  + `isBuiltinTrait`): Add/Sub/Mul/Div/Rem → `add/sub/mul/div/rem`; `Eq` → `bool eq(Self&)`;
  `Ord` → `i32 cmp(Self&)`; `Neg` → `neg()`; `Index` → `get(i)` / `set(i,v)`.
- **Desugaring** (semantic records `resolvedCallee[node]` when the method is overloaded;
  codegen emits via `genTraitMethodCall`): `analyzeBinary`/`genBinary` (arith + `==`/`!=`
  (eq then `xor i1 …, true`) + `<`/`<=`/`>`/`>=` (cmp `i32` then `icmp slt/sle/sgt/sge …, 0`)),
  `analyzeUnary`/`genUnary` (`Neg`), `analyzeIndex`/`genIndex` (`get`),
  `analyzeIndexAssign`/`genIndexAssign` (`set`). Overload active **only if** the operand's
  class is in `implementedTraits` for that trait; else the built-in numeric rules stand.
  `Eq`/`Ord` operators type as `bool`; others as the method's return type.
- **Default `==`/`!=` for classes without an `Eq` impl** (an `Eq` impl **overrides** either;
  semantic records the node + codegen has no `Eq` gate of its own, so both must agree via the
  handoff sets on `SemanticResult`, threaded to codegen):
  - **Two references** of the same class → **address identity** (`icmp eq/ne ptr`, like enums).
    Recorded in `addressIdentityCmp`; `genBinary` skips the desugar + reuses the enum-identity path.
  - **A value object** (or a value/reference mix of the same class) → **memberwise structural
    equality** (a value has no meaningful address). Recorded in `structuralValueCmp`; codegen
    emits a generated `@Class_structeq(ptr, ptr) -> i1` (drained worklist like clones): primitives
    by value (`icmp eq`/`fcmp oeq`), reference/enum/ptr fields by address (`icmp eq ptr`), and an
    embedded value-object field via its own `@Field_eq` if the field's class implements `Eq`
    (from `eqImplementors`, guarded by the emitted name existing) else recursively via
    `@Field_structeq`; all AND-accumulated. Terminates because value-embedding cycles are rejected.
  - Other operators (Add/Ord/…) on a class without the impl still **error**.
- **Codegen** adds `funcReturnTypes` (needed so operator calls emit the right return IR, esp.
  `cmp`→`i32`), `genTraitMethodCall` (`CodeGen_Classes.cpp`), and counts impl methods into
  `overloadedBases_`/`funcParamTypes`.
- **Operators may return an object by value** (`fn add(Self& rhs) -> Vec2 out`): this rides
  the return-alias sret machinery. `genTraitMethodCall` checks `slotReturningFns_` for an
  object return and calls via the **sret convention** (hidden slot `ptr` first, then receiver,
  then args; `void` LLVM return), materializing a temp slot and returning its address — **no
  heap allocation**, no by-value struct return. (Prior to this, an object-value operator
  emitted `%t = call %V @V_add(...)` and fed the struct where a `ptr` was expected → malformed
  IR.) Combined with the value-object borrow, `Vec2 c = a + b;` on stack operands is fully
  stack-resident (operand borrows + sret slot; a trailing `Vec2_clone` copies the slot into
  `c`, still no heap). Operator **operands** may be value objects (borrowed as `Self&` — see
  the memory-model borrow note).

### Debug info (DWARF) — `--debug` / `-g`
LLVM debug metadata so a compiled `.gg` is debuggable with gdb/lldb (clang→MinGW emits DWARF).
**Off by default; with `--debug` off the emitted IR is byte-identical to before** (the only hook in
the hot path is `emit()` appending `currentDbgLoc_`, which is empty unless debug is on). Flag:
`CompilerOptions::debugInfo` (+ `sourceFile`, the DWARF `DIFile`); parsed in `main.cpp` (`--debug`/
`-g`); `compile.ps1 -DebugInfo` passes `--debug` to GG **and** `-g` to clang (named `-DebugInfo`,
not `-Debug`, which PowerShell reserves as a common parameter on advanced scripts). All emission lives in
`source/codegen/CodeGen_Debug.cpp` (gated by `CodeGen::debug_`).
- **Storage / printing**: `IRFunction::dbg` (the ` !dbg !N` suffix on the `define`), `IRModule::debugMeta`
  (`!N = !DI…` nodes) and `IRModule::namedMeta` (`!llvm.dbg.cu` / `!llvm.module.flags`). `IRPrinter`
  prints `signature + dbg + " {"` and appends the metadata after the runtime block.
- **Tier 1 (line-level)**: `dbgInit()` (once, in `generate()`) emits the `!DIFile` + `distinct
  !DICompileUnit` + the `"Debug Info Version"` module flag. `dbgBeginFunction`/`dbgEndFunction`
  (in `genFunction`/`genMethod`) create a `distinct !DISubprogram` (attached via `IRFunction::dbg`)
  with a `!DISubroutineType`; `dbgStmtLine` (called at the top of `genStmt`) sets `currentDbgLoc_`
  to a `!DILocation` from the statement's leading `Token.line`, which `emit()` appends to every
  instruction. **Only user functions are decorated** — runtime helpers (`gg_*`), clones, dtors,
  structeq, enum/static-init are left undecorated (`currentSubprogram_`/`currentDbgLoc_` cleared at
  `dbgEndFunction`), which the LLVM verifier requires (a subprogram-less function may carry no `!dbg`).
- **Tier 2 (variables & types)**: `dbgTypeOf(Type)` (cached) maps GG types to DWARF — `!DIBasicType`
  for primitives, `!DICompositeType` with per-member byte offsets for value objects (offsets from
  `dbgSizeAlign`, matching LLVM's natural-alignment struct layout), and an opaque
  `DW_TAG_pointer_type` (`baseType: null`) for references/enums/`ptr`. Object composites reserve
  their id **before** recursing into fields (cycle-safe). `dbgDeclare` emits a
  `#dbg_declare(ptr <alloca>, !<DILocalVariable>, !DIExpression(), !<loc>)` **record** (LLVM-22 form,
  not the old `@llvm.dbg.declare` intrinsic) for each parameter (`arg: N`) and local — hooked in the
  param-spill loop and each `genVarDecl` branch (scalars/refs/enums/`ptr`/objects; arrays and static
  locals are skipped). `this` is not declared (it's the `%self` value, not an alloca). A `str` value
  is a precise `DW_TAG_structure_type` (`{ ptr data; u64 len }`, 128 bits) — see the `str` invariant.
- **Multi-file attribution (function granularity)**: each user function/method is attributed to its
  **declaring source file** — a per-file `!DIFile` (cached in `dbgFileIds_`, keyed by source path;
  `dbgFileFor(path)` creates/reuses it, `dbgInit` seeds the main file so it isn't duplicated). The
  declaring path comes from the `sourceFile` field on `FunctionDeclStmt`/`ClassDeclStmt`/
  `ImplDeclStmt`/`EnumDeclStmt` (stamped by the parser; the same field the semantic error-attribution
  uses), threaded into `dbgBeginFunction(..., sourceFile)` (and `genMethod(..., sourceFile)`). The
  `!DISubprogram` `scope`/`file` and its `!DILocalVariable`s use `currentDbgFileId_` (the function's
  file), not the main file — so a debugger stepping into an imported function (e.g. `Integer::parseInt`
  from `number.gg`) lands in the right file, not the main source. The `!DICompileUnit` still points at
  the main file (one CU, many files — the standard layout). `dbgEndFunction` resets `currentDbgFileId_`
  to the main file. Type-composite members (`str`/object) still reference the main `!DIFile`
  (cosmetic; not a line-attribution concern).
- **v1 limitation (residual)**: attribution is **function-granular**, not statement-granular — every
  statement of a function maps to that function's file (correct, since a function can't span files).
  Generic instantiations attribute to whatever file the monomorphizer re-parse ran under (usually
  correct; the mangled instantiation carries the template's tokens). `Token` still carries only a
  line, not a per-token source identity — fine for the function-granular model.

## Architecture summary

### Compiler pipeline (per source file)
```
.gg source
  → Lexer            (produces Token stream)
  → ImportResolver   (3 passes — see below)
  → SemanticAnalyzer (3 passes — see below)
  → CodeGen          (walks AST, emits IRModule)
  → IRPrinter        (writes .ll text; appends DWARF metadata when --debug is on)
  → clang            (assembles + links to native .exe)
```

### Parser file organization
The `Parser` class is one contract (`source/parser/Parser.h`) whose implementation is split by
responsibility across several `.cpp`s (mirroring the `CodeGen_*` / `SemanticAnalyzer_*` split), so no
single file is oversized. All are pure relocation behind the shared header — a method's physical file
doesn't affect behavior (members resolve via `Parser.h`):
- `Parser.cpp` — driver (`parse`), the token cursor (`peek`/`advance`/`match`/`consume`), `error`/
  `synchronize`, `parseDeclaration`/`parseStatement`/`parseBlock`, and type parsing
  (`isTypeName`/`consumeType`/`consumeTypeCore`).
- `Parser_Modules.cpp` — `scanModuleDirectives`/`scanModuleMembers`/`qualifyTokens`.
- `Parser_Generics.cpp` — template capture (`tryCapture*`), `Call`-bound scanning, type-arg parsing,
  mangling, deduction, and `runMonomorphization`.
- `Parser_Reflect.cpp` — `expandReflection*` + `parseInlineForStmt`/`parseReflectExpr`.
- `Parser_Decl.cpp` — `parseFnDeclaration`/class/enum/trait/impl/`parseMemberList` + params/args.
- `Parser_Stmt.cpp` — `if`/`while`/`for`/`switch`/`return`/`yield`/`break`/`continue`/expr-stmt.
- `Parser_Expr.cpp` — the precedence ladder (`parseExpression` … `parsePrimary`) + lambdas.
- `Parser_internal.h` — two `inline` helpers (`isTypeKeyword`, `argMangle`) shared by the type and
  generics files. (`callSigTokenType`/`reflectBaseClassName` are file-local `static` in Generics/
  Reflect respectively.)

### ImportResolver — 3 passes
1. **Prescan templates**: walk all files recursively, register every generic template
   name into a shared `GenericRegistry` (so use sites in other files are recognised).
2. **Parse all files** (monomorphization deferred): each file parsed with
   `runMonomorphization=false`; instantiation requests accumulate in the shared registry.
3. **Monomorphize once**: a single `monoParser` expands all pending instantiations,
   appending concrete mangled declarations to the unified `Program`.

Each of the three passes (`prescanTemplates`, `collectClassNames`, `processFile`) resolves an
`import "..."` path through the **one** shared `resolveImportPath` helper, so they always agree on
which file an import names.

**Import dedup is case-insensitive on Windows** (`dedupKey`, `ImportResolver.cpp`). Every pass keys its
visited/processed set on `dedupKey(canonical)` — `weakly_canonical(...).string()`, **lowercased on
`_WIN32`** — instead of the raw canonical string. `weakly_canonical` normalizes `.`/`..`/separators but
**preserves case**, so on a case-insensitive filesystem two spellings of one file (`import
"std/String.gg"` in one file, `import "std/string.gg"` in another) would otherwise produce two distinct
keys → the file is parsed twice → its classes are redefined ("class 'String' is already defined"). The
lowercased key folds the spellings into one import. Guarded by `#ifdef _WIN32` so a genuinely
case-sensitive filesystem keeps the two as distinct files. The real path (original case) is still used
to read/lex the file; only the dedup *key* is lowercased.

**Pass 2 (`processFile`) processes a file's DEPENDENCIES before parsing its OWN tokens** (fixed
bug — it previously parsed its own tokens first, then recursed into dependencies). This ordering is
load-bearing, not cosmetic: a generic-function call written WITHOUT explicit `<...>`
(`deduceVariadicInstantiation`/`inferGenericTypeArgs`, `Parser_Generics.cpp`) looks up the template
directly in the shared `GenericRegistry` **at parse time** — unlike an explicit `name<Targs>(...)`
call, which only needs the template to exist later, once `runMonomorphization` runs (well after
every file is parsed) — so if the importing file's own tokens were parsed before its dependency's,
an inferred call to a template declared in that dependency found nothing and wrongly reported
"cannot infer type argument(s)", even though the call was valid and would have resolved correctly
once the dependency's own parse ran moments later. This never surfaced before because every prior
generic template happened to be declared in the SAME file as its non-explicit call sites (captured
earlier in that file's own top-to-bottom parse); it surfaces once a consumer file calls an inferred
generic-pack function declared in a *separate* imported file (found via `stdlib/io/IO2.gg`'s
`printf<...Ts>`, called without explicit type args from `samples/std_lib.gg`). Tests:
`tests/import_tests.cpp` (`[import][genoverload]`).

### Module / import resolution — search roots + the `std/` anchor
`import "path"` is resolved by `ImportResolver::resolveImportPath(importerDir, rawPath)` against a
`ModuleSearchConfig { stdlibDir, searchRoots }` (in `ImportResolver.h`), in this order (first
existing wins):
1. **Reserved `std/` prefix** → `<stdlibDir>/<rest>`. `std/` always names the compiler's standard
   library regardless of the importer's location on disk — so `import "std/String.gg"` works from
   anywhere (no `../stdlib` walk). Resolved under `stdlibDir` **even when the target is missing**, so
   a bad stdlib import reports against the stdlib dir. Disabled if `stdlibDir` is empty.
2. **File-relative** — `importerDir/rawPath` (the original, backward-compatible behavior; all
   pre-existing relative imports resolve exactly as before, and take precedence over search roots).
3. **`searchRoots`**, in order — `<root>/rawPath`.
4. Fallback: the file-relative candidate (so the existing "cannot find imported file" error names the
   most likely path; the error also lists the `std/` target + roots that were tried).
- **Dotted imports SELF-LOAD their module directory** (Go/Java-style — the module IS its directory):
  a `import a.b.C;` (or bare-module `import a.b;`) is no longer only a name binding — the resolver
  now also **loads every `*.gg` file in the directory that module maps to**, so a consuming file needs
  just the one dotted line (no companion quoted `import "…"`). Resolution
  (`ImportResolver::resolveModuleDir`) maps the dotted module path to a directory using the SAME roots
  as `resolveImportPath` (`std` → `stdlibDir`; else importer-relative then each search root), trying
  the whole path (bare module, `import std.crt;`) then all-but-last (module + trailing symbol,
  `import std.utility.Pair;`) — longest first. The last segment is the symbol, everything before it the
  module path (unambiguous — GG top-level decls are flat). A dotted import that maps to **no**
  directory contributes no file and stays a pure name binding (backward-compatible: a module living in
  a single quoted-import file, e.g. `e2e/mods/geo.gg`, still works — its `import geo.Point;` binds and
  the `import "mods/geo.gg";` loads). All four graph-walking passes (`scanModules`,
  `prescanTemplates`, `collectClassNames`, `processFile`) share one `dependencyPaths(tokens,
  importerDir)` helper that returns both quoted-file and dotted-directory dependencies, so they always
  agree on the file set. Loading a directory pulls in the WHOLE module (all its files), so importing
  one symbol makes its siblings available too (by FQN or their own import) — importing `std.String`
  compiles all of `module std` (Array/Number/String); unused generics cost nothing, unused non-generic
  decls are dead IR. **Verification** (`verifyModuleDir`, once per directory): every `*.gg` in a
  loaded module directory must declare that module — a file whose declared `module` mismatches its
  directory is reported to stderr (a file's location must match its module). Quoted `import "path"`
  is unchanged and remains the escape hatch for module-less / ad-hoc files.
- **`stdlibDir` auto-detection** (`main.cpp`, `findStdlibDir(argv0)`): honors `$GG_STDLIB` first,
  else walks **up from the GG executable's directory** for a `stdlib/` folder (confirmed by its
  `crt/` subdir). `searchRoots` come from `$GG_PATH` (`;`-separated, `findSearchRoots`). Both are
  computed in `main`, passed via the `GG(paths, options, ModuleSearchConfig)` ctor →
  `resolver.resolve(file, config)`. The default `resolve(file)` (no config) is file-relative only,
  so the resolver **unit tests are unaffected** (they use temp files + relative imports).
- The stdlib and its consumers now use **dotted self-loading imports exclusively** — there are no
  quoted `import "…"` lines anywhere in `stdlib/`, `e2e/`, or `samples/` for stdlib deps. Each stdlib
  file pulls its siblings/dependencies with dotted imports (`import std.crt;`, `import std.String;`,
  `import std.io.print;`), and each consumer writes a single `import std.<Symbol>;` per bare stdlib
  name it uses — the file is loaded and the name bound by that one line (see the dotted-self-load
  bullet above). **v1 scope**: symbols are still flattened into one flat-global `Program`.
  Tests: `tests/import_tests.cpp` (`[import]`), `e2e/module_std_test.gg`.
- **A missing/unresolvable import used to fail SILENTLY in three of the four passes that walk the
  import graph** (`scanModules`, `prescanTemplates`, `collectClassNames` — only `processFile` printed
  anything). `collectClassNames` runs *before* parsing, to seed the parser with every reachable class
  name so `ClassName ident(args)`/`ClassName ident{args}` parses as a declaration; when an import
  broke deep in that walk, the class silently never got registered — with **zero diagnostic** — and
  the *real* symptom surfaced far later as a baffling, unrelated parser error at the actual use site
  (`"expected ';' after expression"` on `Vec2 v1{1, 5.0};`, because `Vec2` wasn't recognized as a type
  and `Vec2 v1` doesn't parse as an expression). Meanwhile `processFile`'s own correct "cannot find
  imported file" diagnostic never even ran, because the root file's parse threw before `processFile`
  got to recurse into the broken import. Fixed with a shared `reportMissingImport(path)` helper
  (deduped via `reportedMissing_`, since multiple passes independently walk the same import graph)
  wired into all four passes, so a broken import now reports immediately and clearly instead of
  masquerading as an unrelated parse error three files away.

### Module namespacing — `module` + Java-style `import a.B` (axis B)
Per-module namespaces so two files may each declare `class Foo` without colliding (the deferred
axis-B counterpart to the `std/` resolver above). A file declares `module geo;` (or a **dotted**
`module std.utility;` — see below); its top-level names are qualified with that prefix (`geo.Foo`,
`geo.Foo_method`, `geo.Vec$i32`, `std.utility.Pair`). **Java model**: `import geo.Point;` lets you
write bare `Point`; two imported modules that both export `Point` make the bare name ambiguous,
resolved by the fully-qualified `geo.Point` at the use site.
- **Key design — qualify the name STRING in the parser, as a whole-token rewrite** (mirrors generic
  monomorphization, which turns `Vec<i32>` into the bare className `Vec$i32`). Because `mangleType`
  returns `t.className` verbatim and codegen builds every `%`/`@` symbol from the className string,
  baking the module prefix into the name early means **semantic and codegen need no changes** — the
  qualified name flows through as an ordinary className. The rewrite (`Parser::qualifyTokens`) runs
  in `parse()` after reading the file's directives, before the parse loop:
  1. **Fold FQNs**: greedy LONGEST-PREFIX match — collect the maximal dotted identifier chain, then
     try module-name candidates (from `moduleNames`, which may itself be dotted) from longest to
     shortest, leaving ≥1 trailing segment as "Name"; the first (longest) hit folds into one
     `"mod.Name"` token. This is what makes `std.io.print` fold as module `std.io` + name `print`
     rather than module `std` + a dangling `.io.print` chain, when both `std` and `std.io` happen to
     be registered modules. So a fully-qualified reference becomes a single identifier — never
     N tokens — and `consumeTypeCore`/`isTypeName`/`parsePrimary`/decl builders are **untouched**
     (they see one already-qualified token). This is also why ctor/dtor "just work": the class name
     **and** the ctor/dtor names (which equal it) are qualified together, so `name == className`
     still holds.
  2. **Qualify bare names — context-aware** (so a field/local named like a module free function
     isn't mistaken for it): the module tables are split into **types** (class/enum/trait/annotation)
     and **funcs**, because they qualify differently. A **TYPE** name is qualified in **any** position
     (decl, ctor/dtor, param/field type, return type, cast, `new`, generic base, …). A **FUNCTION**
     name is qualified **only in call/generic position** (next token `(` or `<`) — so `size;` /
     `size = …` (a field or local named `size`) is left bare while `size(` / `size<…>` (a call) is
     qualified. Target resolved via `bindings[X]` (an `import a.B;`, kind taken from `B`'s home
     module) else the current module's own type/func sets. **Never qualified**: a name after `.`/`::`
     (member field / `Type::member` / enum variant access), a **method or enum-member decl name** (a
     bare name right after `fn` at brace depth > 0), an already-qualified name, `main`, and `extern`.
     Ambiguous imports (`X` from two modules) are left bare → a hard compile error at use.
  3. **A top-level `fn`'s own declared name is NEVER looked up via `bindings`** (any depth — this is
     distinct from, and in addition to, the depth > 0 method/enum-member exemption above): it always
     resolves to `currentModule.empty() ? name : currentModule + "." + name`, bypassing the
     bindings-then-current-module-fallback logic entirely for that one token. A declaration doesn't
     *refer* to anything — it introduces a new symbol belonging to its own module by construction —
     so without this, an unrelated `import mod.f;` for some OTHER module's same-named function would
     silently hijack THIS file's own `fn f(...)` declaration to that other qualified name instead of
     its own module. (This was a pre-existing bug in the single-segment v1, not something dotted
     names introduced — it just never had a reason to manifest until a file both declared and
     imported the same bare name for the first time; see `stdlib/math/vector/Vector2.gg`'s
     `fn print(vec2&)`, which imports `std.io.print` under the same bare name `print` for its
     internal calls and relies on this exemption to keep its own declaration as
     `std.math.vector.print` rather than being renamed to `std.io.print`.) Ordinary call sites are
     unaffected — they still resolve via bindings-first, else current-module fallback, unchanged.
- **Separator = `.`** (`geo.Foo`) — a legal unquoted LLVM identifier char, matching the FQN spelling.
- **Dotted (multi-segment) module names and import paths**: `module std.utility;` and
  `import std.utility.Pair;` both read a greedy `IDENT ('.' IDENT)*` chain (in `scanModuleDirectives`
  and, for the real parse, `Parser::parse()`'s module-line skip — the two must stay in lockstep, or a
  dotted `module` line parses differently between the prescan and the real parse and `currentModule_`
  desyncs). An import path's LAST segment is always the imported symbol; everything before it
  (dot-joined) is always the module path — unambiguous at any depth, since GG top-level decls are
  always flat, never nested. A single-segment name is just the degenerate case of the same grammar
  (zero extra dot-segments), so existing single-segment code is byte-identical.
- **Per-module symbol table** — `GenericRegistry::moduleMembers` (module → simple top-level decl
  names) + `moduleNames`, shared across files. `ImportResolver` fills them in a new **pass 0**
  (`scanModules`, before all parsing) so a file can qualify references to a sibling file's
  same-module symbols and fold FQNs written before the defining file is parsed;
  `prescanTemplates` + `collectClassNames` qualify each file's tokens (`qualifyFileTokens`) so the
  names they register match what the parser produces. Static helpers `Parser::scanModuleDirectives`
  / `scanModuleMembers` / `qualifyTokens` are reused by both.
- **Backward-compatible**: a file with no `module` is in the global module `""`; qualification is
  **skipped entirely** when no module is in play, so module-less programs' tokens — and thus IR —
  are byte-identical to before. `main` and `extern` are never qualified (entry / C-ABI symbols).
- **Duplicate diagnostic** (`collectClasses`): a duplicate type within one module now errors
  ("class/type … already defined"), closing the old silent first-wins gap; cross-module same names
  are distinct keys → no error.
- **Caveat — overload sets are scoped per exact module string, never merged across a dotted
  family**: `std.io.print(...)` and `std.math.vector.print(...)` are two entirely distinct qualified
  functions, even though one textually nests under the other; a bare `print` in a file resolves to
  at most ONE of them (import binding, else the current module's own membership) — never a
  cross-module overload union. A file that needs to call a same-named function from a *different*
  module, while also declaring its own same-named function, imports the foreign one under the bare
  name (safe — see the declaration exemption above) and calls it bare; a file that needs to call
  BOTH of two *different* modules' same-named functions bare would create an ambiguous import-binding
  collision (`scanModuleDirectives`'s existing ambiguity check) and must fall back to the FQN for at
  least one of them.
- **Java-style wildcard import — `import a.b.*;`**: binds every top-level member (types AND free
  functions) that module `a.b` declares, exactly as if each had its own `import a.b.Name;` line —
  reusing the same per-name ambiguity check (`scanModuleDirectives`'s `bind` helper) a single-symbol
  import already goes through, so a wildcard-vs-wildcard or wildcard-vs-explicit collision on the same
  bare name is left unqualified → a clean "not a known type"/ambiguous error at use, same as today.
  **Non-recursive, like Java**: `a.b.*` binds only `a.b`'s own members — a submodule `a.b.c` is a
  distinct `moduleTypes`/`moduleFuncs` registry key, never merged into `a.b`'s entry, so it is never
  pulled in by a wildcard on `a.b`. Implementation is entirely in `scanModuleDirectives`
  (`Parser_Modules.cpp`): the existing dotted-segment reader is unchanged; after it stops, a trailing
  `DOT STAR` (instead of assuming the last segment is always a symbol) treats **every** collected
  segment as the module path and looks it up in `moduleTypes`/`moduleFuncs` (now passed into
  `scanModuleDirectives` as two extra const-ref params — default-empty so any caller that doesn't care
  about wildcards still compiles) to bind each member name in turn. **No lexer/AST change** — `*`
  already lexes as `STAR` (used elsewhere for multiply/pointer sigils); the wildcard is recognized
  purely by position (right after `IMPORT <dotted path> .`), and — like every dotted import — never
  produces an `ImportStmt` AST node (`Parser::parse`'s "skip a dotted import to `;`" loop already
  tolerates any trailing tokens, including `*`, with no change). `dependencyPaths`/`resolveModuleDir`
  (file/directory loading) need **no change either** — they already stop their own dotted-chain read
  at the same `DOT STAR` boundary and resolve the pre-`*` segments to the module's directory exactly
  as a bare `import a.b;` would.
- **v1 scope / limits** (all → a clean error, never a miscompile): no import aliasing, no
  module-private visibility (namespacing only). `import a.B;` does **not** load a
  file — the defining file must still be `import "…"`-loaded. An ambiguous bare name errors with a
  generic message (use the FQN). A local named identically to a module name (or a module-name prefix)
  would shadow the FQN fold (module wins; don't do it).
  Residual name-collision edges (all → a clean error, never a miscompile, all niche): a **field or
  enum-variant named identically to a module TYPE** (types qualify in any position, but the field's
  `.`-access does not → "no field/variant"); a **local named identically to a top-level function**
  used in a `<` comparison (`count < 9` parses `<` as generic-call position → "cannot use function as
  value"). Rename one. Tests: `tests/module_tests.cpp` (`[module]`), `e2e/module_test.gg`,
  `e2e/module_dotted_test.gg` (+ `e2e/mods/*.gg`).
- **The entire stdlib is namespaced under `std`**: `std` (top-level `Array.gg`/`Number.gg`/
  `String.gg`), `std.crt` (`crt/*.gg` — a no-op today since `extern` is never qualified, kept for
  consistency), `std.io` (`io/IO.gg`), `std.math` (`math/Math.gg`, `math/Vector.gg`),
  `std.math.vector` (`math/vector/Vector2.gg`, `Vector3.gg`), `std.utility` (`utility/List.gg`,
  `Pair.gg`). Every consuming `.gg` file must `import std.<Symbol>;` (or the appropriate
  sub-namespace) for each bare stdlib name it uses, in addition to the existing `import "std/…";`
  file-load line.

### SemanticAnalyzer — 3 passes
1. `collectClasses`  — build `classRegistry` (fields, methods, destructor).
2. `collectFunctions` — hoist top-level function signatures into global scope.
3. `analyzeStmt` over every declaration — full type checking.

### Escape analysis (v1, `SemanticAnalyzer_Escape.cpp`)
Rejects the "value-object → reference borrow" footgun: passing a **stack value object** where a
`Class&` parameter is expected is a compile error **iff** the callee lets that reference escape.
- **Summaries** computed at collection time (no fixpoint): `computeParamEscapes` scans each
  function/method body and fills `paramEscapes` (per param) + `thisEscapes` on `ClassInfo::Method` /
  `FunctionOverload`. A reference "escapes" when it is **returned** or **stored into a reference
  field** (`this.f = q` or implicit `f = q`, where `f`'s type is `Reference` — a value-object field
  store is a deep copy, *not* an escape; a **local shadowing a reference field** is a rebind, not a
  store — declared-local names are excluded). Local rebinds are tracked as alias edges; escape is
  reachability from the param to a return/ref-store site. **Return-slot clone exception**: an
  assignment into a **non-reference** return alias (`fn dup(T& v) -> T out { out = v; return out; }`,
  object/primitive/enum slot) is a **clone** into the caller's storage, not an alias — so it records
  **no** alias edge (`computeParamEscapes` takes the slot name via `objectSlotName`, computed by the
  `nonRefSlotName` helper), and a borrowed param filled into it does not escape. **Gated on
  `returnType.kind != Reference`, NOT `== Object`** — critical because the escape summary is computed
  in `buildClassInfo`, where a method returning its **own** class resolves to a non-`Object`
  placeholder (the class isn't in `classRegistry` yet), so an `== Object` gate silently missed every
  method self-return; a **reference/borrow** alias (`-> T&`/`-> T*`) always decodes to `Reference`
  from token syntax, and there `out = v` is a rebind that returns the borrow — so a stack-value
  argument is still rejected. The ref-field-store escape path (`this.f = v`, a `MemberAssignExpr`) is
  untouched, so a genuine store alongside a clone is still caught.
- **Check** (in `resolveOverload`'s arg loop + `analyzeMethodCall`): gated on the argument being an
  `Object` coerced to a `Reference` param whose `paramEscapes[k]` is set — so reference→reference
  passing, value-field copies, and pure-borrow readers/operators are never touched (zero false
  positives across the suite). `OverloadCand` carries a `paramEscapes` pointer alongside `paramMut`.
- **v1 scope**: intraprocedural and *optimistic on calls* — a param forwarded into another call is
  not followed into that callee, and `a[i] = q` isn't treated as an escape (element type unknown).
  Catches the direct footguns (return / ref-field store). `thisEscapes` is currently unreachable
  (`this` is Object-typed and Object→Reference is already blocked in return/store), kept for the
  future. Interprocedural propagation is deferred — see `docs/escape-analysis.md`.

### Generics — monomorphization entirely in the Parser
- `class Foo<T>` / `fn ReturnType bar<T>(...)` captured as raw token slices in `GenericRegistry::templates`.
  Generic functions use the **same declaration syntax as regular functions**, just with `<T>` after the name
  (both begin with `fn` — see the `fn` invariant below).
- Use sites (`Foo<i32>`) queue a `GenericInstantiation` on the worklist.
- **A bare generic constructor call** `ClassName<T>(args)` (value construction, **no `new`**) is a use
  site too — `parsePrimary` has a dedicated branch (gated on `gen_->classNames.count(name) &&
  check(LESS)`, mirroring the `new Class<...>(...)` path) that mangles + `recordInstantiation`s and
  emits an ordinary constructor `CallExpr` with the mangled name. Without it a bare `Wrap<i32>(x)`
  mis-parsed as the comparison `Wrap < i32 > (x)` ("type used as a value") — a bare class name is never
  a value, so `ClassName <` is unambiguously a generic construction. `new`, the var-decl ctor sugar
  (`Wrap<i32> w(x)`), and generic-method calls were unaffected; only the bare-expression form was missing.
- `monomorphize()` substitutes type args, re-parses each instantiation into a concrete
  mangled declaration. **Semantic and CodeGen never see type parameters.**
- **Mangling** (`$` separator; `$` and `.` are valid unquoted LLVM identifier chars):
  - `Vec<i32>`       → `Vec$i32`
  - `Pair<i32,f64>`  → `Pair$i32$f64`
  - `Vec<Box<i32>>`  → `Vec$Box$i32`
  - `Vec<Point&>`    → `Vec$Point.ref`
- **Bare generic template names are rejected as an unknown type** (`resolveTypeToken`,
  `SemanticAnalyzer.cpp`): the parser's `isTypeName()`/`typeSpanAt` accept a bare, unparameterized
  generic template name (`Array`, not `Array<i32>`) as a type token in several positions — a fixed-
  size-array declaration (`Array[5] arr;`), an ordinary var-decl (`Array a;`), a parameter type
  (`fn f(Array a)`) — because those checks only confirm the name is *some* known class/template, not
  that it was ever actually instantiated (only `consumeTypeCore`'s `Name<args>` branch, gated on a
  following `<`, produces a real mangled instantiation). A bare template name that reaches
  `resolveTypeToken` matches none of its resolution branches (it's not in `classRegistry`/
  `declaredClassNames_` — only *mangled* instantiations like `Array$i32` are ever registered there)
  and used to fall through silently to `typeFromToken`'s `TypeKind::Error` fallback with **no
  diagnostic at all** — every caller assumes an `Error` result already had `error()` called on it, so
  it just silently propagated. Codegen's own `Error → "i32"` IR fallback (`irTypeName`, meant for
  "an error was already reported elsewhere") then rendered the bogus type as a plain `i32`: `Array[5]
  arr;` silently became `[5 x i32]` — a 5-element `i32` array of zeros, not five `Array` objects, no
  error, no crash, just silently wrong. Fixed by making `resolveTypeToken`'s final IDENTIFIER
  fallback call `error(typeToken, "unknown type '<name>'")` before returning `Error` — since `error()`
  throws (single-shot), every caller across the codebase (params, fields, return types, var decls,
  casts) gets this diagnostic for free with no other changes needed. There is currently **no valid
  syntax** for "a fixed-size array of N instances of a generic-instantiated class" at all — the
  fixed-array grammar only accepts a *bare* type token immediately followed by `[`, and
  `Array<i32>[5]` doesn't parse either (the general var-decl branch requires an `IDENTIFIER`
  immediately after the type span, not `[`) — so this is a real, currently-unsupported gap, not just
  a diagnostic-quality one. `samples/std_lib.gg`'s stale `Array[5] arrayArray;` (previously marked
  `//TODO how does this get initialized?`) was removed rather than fixed, since it never did anything
  real and there is no working syntax to replace it with yet. Tests: `tests/semantic_tests.cpp`
  (`[semantic][generics]`).
- **Generic functions may return a reference** (`T& maxOf<T>(...)`): `tryCaptureFunctionTemplate`
  / `prescanTemplateNames` accept an optional `&` between the return type and the name.
- **Generic functions may use an arrow-form return slot** (`dup<T>(...) -> Point p`, even
  `zero<T>() -> T out`): the capture leads with the name directly before `<`, strips the
  `<…>`, and keeps the `-> RetType slot` so the re-parse takes the arrow branch. See the
  "Return slots" invariant. Generic-class arrow methods work via the verbatim body capture.
- **Generic impls** — `impl<T> Trait for Class<T> { … }` is captured by `tryCaptureImplTemplate`
  (fires only on `impl <`; a plain `impl Trait for Type` still goes through `parseImplDecl`) as a
  `GenericImplTemplate` in `GenericRegistry::implTemplates`: its `<T…>` header type params, the
  target class name, the positional map `targetParamAtPos` (target arg j → impl type-param name),
  and the captured `impl Trait for Class<…> { … }` tokens **with the `<T…>` header stripped**. In
  `runMonomorphization`, right after a **class** instantiation is expanded, every generic impl whose
  `targetClass` matches is instantiated with the **same** type args (impl param ← class arg,
  positionally), substituted, and re-parsed as an ordinary concrete impl — its `Class<args>` target
  mangles to the same `Class$args` (via `consumeType`) and attaches through the normal
  `collectImpls` path, so `arr[i]` etc. dispatch. Deduped by `implInstantiated` ("`<implIdx>@<mangledClass>`").
  Target args must each be a bare impl type-param (no nested/concrete args) — else a parse error.
  A bare `T` inside a **non**-generic `impl Trait for Foo<T>` (missing the `impl<T>` header) is an
  "unknown type" error that points at the `impl<T>` fix (`throwTypeExpected` + `inImplBlock_`).
  - **Specialization — concrete overrides blanket** ("most specific wins"): a concrete
    `impl Trait for Class<i32>` and the blanket `impl<T> Trait for Class<T>` may coexist; the
    concrete one is used for `Class<i32>`, the blanket for every other instantiation. Implemented by
    a **skip** in `runMonomorphization`: before the worklist loop it collects every explicit concrete
    impl's `Trait@MangledClass` key (`ImplDeclStmt::traitName` + the `consumeType`-mangled
    `ImplDeclStmt::typeName`) from `program.declarations`, then skips instantiating a blanket impl
    (its `GenericImplTemplate::traitName`) for any `Class$args` a concrete impl already covers —
    otherwise both would emit `Class$args::method` and collide. **Order-independent** (the set is
    built up-front from all user-written impls) and **per-trait** (the key includes the trait, so a
    concrete `impl A for Box<i32>` doesn't suppress a blanket `impl B for Box<T>`). GG expresses only
    **two tiers** (fully-concrete vs. fully-blanket — partial specialization like `impl<T> for
    Box<Vec<T>>` isn't a thing), so there is no ambiguity lattice; whole-impl replacement only (an
    impl must satisfy the whole trait). Works for multi-param generics (`Pair$i32$f64` keys) and
    operator traits (`Add`, via the normal desugar). **`Call` special-case**: a concrete `impl Call`
    canonicalizes its trait to `Call$…$ret$…` while the blanket stores the raw `Call`, so the
    key-collect step **also** registers a raw-`Call@class` alias for any `Call$…` concrete impl, so
    the concrete still suppresses the blanket. Two identical *concrete* impls remain a real duplicate
    error (specialization only relaxes blanket-vs-concrete). Tests: `tests/trait_tests.cpp`
    (`[specialization]`), `e2e/impl_specialization_test.gg`.

### Generic type-argument deduction — `f(x)` ⇒ `f<T>(x)` (parser-side)
A generic **free function** called **without** explicit `<…>` infers its type parameters from the
argument types, so `valid(u)` behaves like `valid<User>(u)`. **Purely additive** — a no-`<…>` call to
a template was already an error (only mangled instantiations are ever defined), so there is no prior
meaning to break. Inference runs **in the parser** (where monomorphization lives), reusing the
`scopes_` name→type-token stack the parser **already maintains for every in-scope local/param/field**
(populated by `recordLocal` on every var decl + the param seed; previously only read for lambda
capture). `inferGenericTypeArgs` (`Parser.cpp`), called from the `name(args)` primary-parse branch
(after the existing single-`Call`-bound lambda-inference block, which still returns first):
- **Matching**: scans the template's raw parameter tokens (`GenericTemplate::tokens`; params are not
  pre-parsed) for parameters whose type is a **top-level** type parameter — head token is a type-param
  name followed only by sigils `& * ?` then the param name (so `T`/`T&`/`T*`/`T?` match; `Array<T>&`
  does **not**). Builds paramPos → type-param-name.
- **Reading an arg's type** (`genericArgBaseName` strips a trailing `?`, a `ref:` borrow prefix, and a
  trailing `&`): an **in-scope identifier** (local/param via `scopes_`, or an instance field via
  `classFieldScope_`) → its recorded type's base name; a `Class(...)` **constructor CallExpr** (callee
  in `classNames`/`gen_->classNames`) → the class; a **`NewExpr`** → its class.
- **All-or-nothing**: every type parameter must be deduced; otherwise a clear parse error ("cannot
  infer type argument(s) for generic function 'f'; specify them explicitly, e.g. f<Type>(...)").
  On success, mangles + `recordInstantiation` + emits the mangled `CallExpr` exactly like the explicit
  `f<…>(…)` path, so it reaches the **same** monomorphized instantiation.
- **v1 limits** (all → the clear error, never a miscompile): only **free functions** (not methods);
  only **positional** calls (a named-arg call isn't inference-matched); type params that appear only
  **nested** (`Array<T>`), only in the **return type/body** (`zero<T>() -> T out`), or are supplied a
  **literal**/arbitrary-expression argument are not inferred. A **`var`-typed local** argument is not
  inferable either — `recordLocal` stores the `var` *sentinel* (the real type is deduced later, in
  semantics, after monomorphization), and `baseFrom` maps that sentinel to "" so the call gets the
  clean error rather than a bogus `f$var` instantiation (same root cause as var-in-lambda-capture).
  A repeated type param (`f<T>(T&, T&)`) infers from the **first** occurrence; a mismatched later
  argument is caught by normal overload resolution. Explicit `<…>` always works.
- Tests: `tests/generic_infer_tests.cpp` (`[geninfer]`), `e2e/generic_infer_test.gg`;
  `e2e/annotation_test.gg` uses `valid(u)`/`sumLive(u)` (inferred) alongside `renameKeyBytes<User>()`
  (explicit — `T` only in the body).

### Generic FREE-FUNCTION overload sets — `fn name<T>`/`fn name<...Ts>` declared more than once
A bare name may have **multiple** free-function templates — an overload set, exactly like ordinary
(non-generic) function overloading, but resolved a parser-token-level candidate selection instead of
the semantic layer's `resolveOverload` (no `Type`s exist yet at this point in the pipeline).
`GenericRegistry::templates` is `unordered_map<string, vector<GenericTemplate>>` (one **vector** per
name — was a single `GenericTemplate`, silently overwritten on a second declaration before this
feature); `tryCaptureFunctionTemplate` `push_back`s. **Class** templates (`tryCaptureClassTemplate`)
still wholesale-replace — no overload-set behavior there, out of scope. `GenericTemplate` carries a
`fixedCount` (pack template: # params before the pack; non-pack: total arity), cached at capture time.
- **Selection** (`Parser::selectTemplateCandidate`, `Parser_Generics.cpp`), a 4-stage pipeline over a
  homogeneous (all-pack or all-non-pack) candidate subset:
  1. **Arity/pack-shape gate** (`arityCompatible`): non-pack → exact `argCount == fixedCount` (never a
     spread); pack → `trailingSpread ? argCount == fixedCount+1 : argCount >= fixedCount` (a trailing
     `xs...` spread can only mean "forward an existing pack tuple unchanged", so it needs the exact
     `fixedCount+1` shape — see `deducePackTargs`'s pre-existing `spreadPack` condition). An explicit
     `name<Targs>(...)` call additionally requires `typeParams.size() == explicitTypeArgCount` and
     rejects any pack candidate outright (a pack's type args are always inferred).
  2. **Fixed-parameter type-compatibility filter** (`classifyTemplateParamAt` + the existing
     `deduceArgTypeToken`): for each candidate's FIXED (non-generic) parameter position, reject the
     candidate on a clear mismatch between the call argument's syntactically-deduced type and the
     parameter's declared concrete type — lenient (never rejects) when the position is the template's
     own type parameter being inferred, or when either side can't be determined at parse time. This is
     what resolves the case where two *different*-`fixedCount` pack candidates both satisfy stage 1's
     `>=` check purely by having enough arguments (e.g. `print(str*, Ts...)` vs
     `print(str*, i32, Ts...)` called with a non-`i32` second argument — arity alone can't tell them
     apart, but the second candidate's `i32 cursor` position doesn't match the argument's type).
  3. **Non-empty-pack preference**: among stage-2 survivors, discard any candidate whose PACK would
     be reduced to **zero elements** if another survivor's pack would not be. Rationale: when a call's
     literal argument type happens to ALSO match a candidate's extra fixed parameter (e.g. an i32
     substitution value fits both "pack element" and a `i32 cursor` fixed param), stage 2 can't
     distinguish them by type — and picking the larger-fixedCount candidate purely by the old stage-4
     tie-break would silently swallow an argument that was meant for the pack, reducing that pack to
     nothing (defeating the point of it being variadic at all). Only fires on a genuine disagreement
     among survivors; if every survivor's pack is empty (or none are), falls through unchanged. A
     no-op for non-pack candidates, where `args.size() == fixedCount` always holds by stage 1, so the
     "empty pack" concept never applies to them. Found via a real bug: `stdlib/io/IO2.gg`'s public
     `printf<...Ts>(str* fmt, Ts... args)` (fixedCount=1) vs its `// TODO private`
     `printf<...Ts>(str* fmt, i32 cursor, Ts... args)` (fixedCount=2) — calling `printf(fmt, 5)` from
     outside the file was silently hijacked by the private cursor overload (binding `5` as `cursor`
     and leaving its pack empty) purely because the old tie-break preferred the larger `fixedCount`,
     even though the public overload's pack would have consumed `5` correctly.
  4. **Tie-break**: among stage-3 survivors, the **largest `fixedCount`** wins (most specific, mirrors
     C++ preferring a non-template overload). A genuine remaining tie (equal `fixedCount`) is a clean
     parser error ("ambiguous call to generic function …), never a silent pick.
  A name with 0 or 1 candidates never reaches this pipeline — every call site fast-paths that case
  identically to the pre-overload-set code (byte-identical mangled output, zero behavior change).
  **Residual v1 limitation**: stage 3 only catches the case where a candidate's pack is reduced ALL
  THE WAY to zero. When 2+ substitution values are passed and the FIRST one's type happens to also
  match the extra fixed parameter (e.g. `printf(fmt, 1, 2, 3)`, all `i32`), both candidates' packs
  end up non-empty (`[1,2,3]` vs `[2,3]`) and stage 3 can't distinguish them — the old fixedCount
  tie-break still applies, still misrouting one argument into the fixed parameter. Fixing that
  general case would need a real static-vs-runtime disambiguation signal this call shape doesn't
  provide (both interpretations type-check); the practical workaround is avoiding same-typed fixed
  parameters adjacent to a pack in an overload set, or renaming one overload.
- **`GenericInstantiation::candidateIndex`** records which candidate a call site selected, so
  `runMonomorphization`'s later re-lookup (`gen_->templates[inst.templateName][inst.candidateIndex]`)
  doesn't need to re-run selection.
- **Mangling** (`mangleInstantiation`) appends `.ov<candidateIndex>` **only when
  `gen_->templates[base].size() > 1`** — the overwhelming common single-candidate case mangles exactly
  as before. Without this, two overloads instantiated with identical type args (e.g. both taking
  `T=i32`) would collide on the same mangled symbol and one body would be silently dropped via
  `gen_->instantiated`'s dedup.
- **Read sites updated** (all previously assumed one template per name):
  `Parser_Expr.cpp`'s explicit `name<Targs>(args)` call, the single-`Call`-bounded-type-param lambda
  conveniences (lambda-signature exposure + lambda-literal inference — applied only when EXACTLY ONE
  candidate has that shape; 0 or 2+ just skip the optimization, no error), the spread-target
  pack-ness check (defers to the pack path if ANY candidate is pack-bearing), `inferGenericTypeArgs`,
  `deduceVariadicInstantiation`, and `runMonomorphization`'s bounded-body-check loop (nested over each
  name's candidate list).
- **Explicit `name<Targs>(args)` and mixed pack/non-pack sets**: a pack candidate is never eligible
  for an explicit call (its type args are always inferred), so the candidate pool for this call form
  is always the NON-pack subset — computed up front (`nonPackCands`). The "does not take explicit
  type arguments; the pack is inferred" error fires only when that subset is **empty** (every
  candidate under the name is pack-bearing) — a name that mixes pack and non-pack candidates still
  lets an explicit call target its non-pack member(s). (Shipped-then-fixed bug: an earlier version
  rejected the whole set the moment ANY candidate was pack-bearing, wrongly erroring on a valid call
  like `foo<Box>(a)` when `foo` also had an unrelated `fn foo<...Ts>(Ts... args)` overload.)
- **Named arguments in the 2+-survivors explicit-call branch are rejected** (a clean parse error, not
  silently mis-resolved): the fixed-parameter type filter indexes `genArgs` **positionally** (it reads
  the declared type at each fixed parameter *position*), so an out-of-order named argument could
  misalign against the wrong position and select the wrong overload. Only fires when disambiguation
  actually needs the argument shapes — a uniquely-matching candidate (by type-parameter count alone)
  never reaches this branch, so named args to a non-ambiguous generic call are unaffected.
- **Deferring to an ordinary overload on an undeterminable fixed-parameter argument**
  (`selectTemplateCandidate`'s `hasOrdinaryFallback` param, threaded from
  `gen_->ordinaryFuncNames.count(fnName)` at all three call sites): stage 2's leniency on an
  undeterminable argument type (e.g. `obj.field` — a `MemberAccessExpr`, which `deduceArgTypeToken`
  has no case for) is **conditional**, not unconditional. When the bare name ALSO has a plain
  (non-generic) declaration to fall back on, an undeterminable fixed-parameter argument now
  **rejects** the candidate instead of leniently passing — so the call defers to the ordinary
  overload rather than risking a silent, wrong match. Found via a real program:
  `stdlib/io/IO2.gg`'s two `print<...Ts>` overloads share the bare name `print` with `stdlib/io/
  IO.gg`'s many ordinary `print(i32)`/`print(str)`/… overloads; a reflective `printObj<T>`
  (`inline for (f in @fields(T)) { print(@field(obj, f.name)); }`) substitutes `@field(obj, f.name)`
  to a member access whose type (e.g. `i32`) couldn't be determined at parse time — previously this
  was leniently accepted into the `str*`-first-param pack template, producing a "no matching
  overload" semantic error once the real (i32) type reached the fixed `str*` parameter. **Scope
  limit**: this only protects call sites that reach `selectTemplateCandidate` — i.e. a bare name
  with **2+** candidates of the relevant kind (pack or non-pack). A name with exactly **one**
  candidate takes the pre-existing byte-identical fast path (`packCands.size()==1` /
  `nonPack.size()==1`) that never calls `selectTemplateCandidate` at all, so it does NOT get this
  protection — an undeterminable-argument call to a lone pack template still greedily claims it
  over a same-named ordinary overload, exactly as before this fix (a deeper, pre-existing
  characteristic of the variadic-pack feature, predating overload sets — extending the fast path
  to defer too would need a semantic-conversion-aware fixed-parameter comparison, not just the
  current exact-string-match `classifyTemplateParamAt` check, to avoid regressing implicit-widening
  cases like an `i32` literal passed to a declared `f64` fixed parameter).
- **v1 limitation**: a name with BOTH a pack and a non-pack candidate, called WITHOUT explicit `<…>`,
  resolves via "the pack path (`deduceVariadicInstantiation`) is tried first and wins if its arity
  gate is satisfiable at all" — not a unified cross-kind selector (pinned down by a dedicated test,
  not left as untested prose). Generic **classes** and generic **methods** (`methodTemplates`, a
  separate registry keyed `Owner::method`) are unaffected — no overload-set support there.
- Tests: `tests/generic_overload_tests.cpp` (`[genoverload]`), `e2e/generic_overload_test.gg`.

### Generic methods — `fn m<T>(...)` on a class (own type parameters)
A method with its **own** type parameters, distinct from any class type params. Like free-function
generics, it's a **parser/monomorphizer** feature (semantics/codegen only ever see the concrete
`@Class_m$i32`). **The central difficulty**: a call `obj.m<i32>(...)` depends on the receiver's
*type*, which the parser (where monomorphization lives) doesn't know — unlike `f<i32>()` (callee names
the fn) or `Vec<i32>` (names the class). Resolved by reusing the parser's `scopes_`/`classFieldScope_`
type-token tracking: **v1 supports a generic-method call only where the receiver's class is
parser-visible** — a bare in-scope local/param/field, `this`, or the static `Class::m<T>(...)` form.
- **Capture** (`captureMethodTemplate`, `Parser_Generics.cpp`): a `fn NAME<…>(` inside a class body is
  diverted in `parseMemberList` (before the param list) and stored as a `GenericTemplate` keyed
  `Owner::NAME` in `gen_->methodTemplates` (the `Owner` is `parseMemberList`'s class-name param — the
  **mangled** name during a generic class's re-parse, so `Array$i32::wrap` registers automatically);
  the captured tokens are a synthetic ordinary method (`<…>` stripped, `fn`/`static`/`private`
  synthesized from bools). No `MethodDecl` is emitted for the template itself. `genericMethodNames`
  (bare) + `genericMethodKeys` (`Owner::m`) gate the call site; populated by a now **brace-aware**
  `prescanTemplateNames` (a `fn NAME<` at depth 0 → `funcNames`; in a class/enum body → a method key;
  in an impl/trait/other brace → neither).
- **Call site** (`parsePostfix`, `Parser_Expr.cpp`): gated on `member ∈ genericMethodNames && check(<)`.
  Resolves the receiver class via `deduceArgTypeToken` (which gained a `ThisExpr` case →
  `currentClassName_`, a new parser field set around `parseMemberList`) or the leading class name for
  the static form; strips a `$…` mangling suffix for the key check. Disambiguates `obj.m < x` from
  `obj.m<T>(...)` by whether the `<…>` closes and is followed by `(` (a `looksLikeGenericCall`
  lookahead): if so an unresolved/absent method is a clean error, else it falls through to less-than.
  On a hit: `parseTypeArgList` → mangle `m$i32` → `recordMethodInstantiation` (dedup key
  `Owner::m$i32`) → emit a `MethodCallExpr` whose `method` token is the mangled name.
- **Monomorphization** (`instantiateMethod`, run from `runMonomorphization`): substitute type-param
  tokens + rename the method-name token to the mangled name (reusing the existing splice), then a
  **shell re-parse** — wrap the concrete method tokens in `class <Owner> { … }`, `parseDeclaration()`,
  and **transplant** the single `MethodDecl` into the real owner's `ClassDeclStmt::methods` deque
  (found by name). Naming the shell the real owner makes a `this.m<U>()` recursion inside the body
  resolve correctly. From there `collectClasses`/`genMethod` pick it up with **zero** new
  semantic/codegen code. Bound obligations (`<T: Trait>`) reuse the existing `GenericBoundCheck` push.
- **Generic classes** (the ordering fix): a method on `Array<T>` can only instantiate after `Array$i32`
  is re-parsed (which registers `Array$i32::wrap`). A method instantiation whose owner class / template
  isn't ready is **deferred** to `pendingMethodInsts_`; after the main worklist drains,
  `runMonomorphization` retries deferred methods to a **fixpoint** (a method body may enqueue more
  class/method work), stopping when nothing's pending or no pass makes progress (→ a clear error).
- **Variadic methods** (`fn m<...Ts>(Ts... args)`) build on this — see the "Variadic methods" invariant
  below; the flagship use is `Array<T>::emplaceBack`.
- **Type-argument DEDUCTION for a call with no explicit `<…>`** (`recv.m(args)` in place of
  `recv.m<T>(args)`) — the method counterpart of the free-function inference described under "Generic
  type-argument deduction" below, sharing its exact param-list-scan mechanism.
  `Parser::inferTypeArgsFromParamList` (`Parser_Generics.cpp`) was extracted out of
  `inferGenericTypeArgs`'s body (scan the raw parameter list for a bare-type-parameter position, then
  read that type off the matching positional argument via an in-scope identifier / `Class(...)`
  constructor call / `new Class(...)` — literals and other expression shapes stay un-inferable) so it
  could be reused, unchanged, by both call sites — `inferGenericTypeArgs` (after its free-function
  overload-candidate selection) and the new `Parser::inferGenericMethodTypeArgs` (methods have no
  candidate-selection step: one template per `Owner::method` key). The call site (`parsePostfix`,
  right where the ordinary/variadic method-call branch already parses `args`) gates on
  `member ∈ genericMethodNames`, resolves the receiver's class exactly as the explicit-`<…>` path does,
  requires a **positional** call with **no spread** (a named/reordered arg would break the
  positional-index assumption the scan relies on), and looks up `methodTemplates[recvClass + "::" +
  method]` — using the receiver's **full** (possibly mangled, e.g. `Array$i32`) class name, NOT the
  `$`-stripped base, since that is exactly the key `captureMethodTemplate` registers under. On success:
  mangle + `recordMethodInstantiation` + emit the mangled `MethodCallExpr`, identically to the explicit
  path — so an inferred call and an equivalent explicit-`<T>` call dedupe to the same instantiation.
  **On failure this is silent, never an error** — falls through to the ordinary method-call path below
  it, so a same-named ordinary overload, or a call shape inference can't handle, resolves/errors exactly
  as it did before this feature existed (a literal argument, a named argument, an argument whose type
  isn't parser-visible).
  - **Real v1 boundary — only works when the owner is a plain (non-generic) class.** `methodTemplates`
    for a method ON a generic class (e.g. `Array<T>::pushAll`) is populated by `captureMethodTemplate`
    only once that owner class has ITSELF been monomorphized (a generic class's body is captured as raw,
    unparsed tokens — `parseMemberList`/`captureMethodTemplate` only run on it during the
    post-monomorphization re-parse under its **mangled** name, e.g. `Array$i32`). Monomorphization runs
    strictly after the ENTIRE program's initial parse completes (all files), so a call site in the same
    initial parse pass — which is every call site, in practice — can never see that owner's
    `methodTemplates` entry yet; the lookup misses and inference silently doesn't fire. The EXPLICIT
    `<T>` form sidesteps this because it never needs to consult `methodTemplates` at the call site at
    all — `looksLikeGenericCall`'s lookahead is purely syntactic, and `recordMethodInstantiation` queues
    the instantiation "blind" (mangled from the explicit args, no template lookup needed) for
    `instantiateMethod` to resolve later, deferring via `pendingMethodInsts_` until the owner class
    exists. Making inference work here too would need an analogous deferral — capturing the
    already-parsed argument `Expr`s and re-running `inferTypeArgsFromParamList` once the owner's
    template becomes available, then patching the (already-constructed) `MethodCallExpr`'s method
    token — which is a materially bigger change than reusing the existing eager-token mechanism, and is
    NOT implemented. So `arr.pushAll(other)` on a generic `Array<T>` still requires
    `arr.pushAll<OtherType>(other)` today; only a method on a non-generic class (`Box::identity`,
    `Factory::make`) benefits from inference.
- **v1 limits** (clean errors, never a miscompile): impl/trait-block generic methods rejected;
  `@annotation` on a generic method rejected; non-parser-visible receivers (call results, `a.b.m<T>()`
  chains, and a **`var`-typed** local — `var` records a sentinel type at parse time, same limitation as
  generic type-argument inference over `var`) → the "cannot determine receiver" error. A bare
  `obj.m(args)` with no `<…>` that inference can't resolve (an owner-is-a-generic-class call, a literal/
  named/un-inferable argument, or a name semantics can't see as generic at all) reports the terse
  "no method 'm'" error — unchanged from before inference existed.
  Tests: `tests/generic_method_tests.cpp` (`[genericmethod]`, inference cases tagged `[geninfer]`),
  `e2e/generic_method_test.gg`, `e2e/generic_method_infer_test.gg`.

### Type-parameter sigil substitution — `T*`/`T&` when `T` is itself a reference (`spliceTypeArg`)
A type parameter's captured argument is a **token span**, not one synthesized token — `Point&`
captures as `[IDENTIFIER("Point"), AMPERSAND]` (`parseOneTypeArg`, `Parser_Generics.cpp`), so that
`argMangle` can render `.ref`. Every substitution site splices that whole span wherever `T` appears
in a template's raw tokens. This broke when the template ALSO writes its own trailing sigil right
after `T` (`-> T*`, `-> T&`) and `T` is bound to an already-sigiled argument (`T = Point&`): splicing
produced two sigil tokens back to back (`Point & *`), which `consumeType` cannot decode at all — not
even a clean semantic error, a raw parser failure (`"expected '{' before method body"`, since
`consumeType` consumes exactly one trailing sigil and leaves the second dangling before the body's
`{`). This is distinct from — and cannot be fixed by — C++-style reference collapsing: GG's `&`
(owning, refcounted) and `*` (borrow, no refcount) are not interchangeable, so a merge-and-pick-a-
winner rule risks silently handing back the wrong ownership kind. Instead: **the template site's own
sigil is authoritative and always wins** — `spliceTypeArg` (a free helper at the top of
`Parser_Generics.cpp`) drops the argument's trailing sigil, not the template's, whenever both are
present: `nextIdx` (the token right after where `T` was in the template's raw stream) is a sigil AND
`argToks.back()` is a sigil ⇒ splice `argToks` minus its last token; otherwise splice the whole span
unchanged (the overwhelmingly common case — `T` bound to a plain value/primitive, or `T` used with no
following sigil at all — is completely unaffected). This is a **strip, not a merge**: the author of
`-> T*` always gets a borrow back, never a silently-widened owning reference; the resulting concrete
type is then validated by the **existing, unchanged** cast rules once the substituted declaration is
re-parsed and analyzed (owning `Class&` field → `T*` return is the existing Silent "owning → borrow"
coercion; a value-object field into `T&` is still the existing "cannot bind a value as an owning
reference" error) — nothing new is invented, the doubled-sigil case just now reaches those checks
instead of dying in the parser. Applied identically at all three token-splice sites that share this
pattern: class/free-function templates (the main `runMonomorphization` worklist), generic methods
(`instantiateMethod`), and generic impls (`impl<T> Trait for Class<T>`) — the pack-substitution branch
(`Ts... args` → tuple `*` borrow) is untouched, since it always emits its own fixed `*` regardless of
the pack's element types and was never part of this bug. Zero runtime cost (pure parse-time token
manipulation); cannot regress prior behavior (the doubled-sigil case was a hard parse error in every
prior version, so nothing that used to compile can be affected). Tests: `tests/generic_method_tests.cpp`
(`[genericmethod]`, the "sigil" cases — accept: class-template/generic-method/generic-impl getters
returning `T*`/`T&` with `T` bound to a reference, plus a same-name non-sigiled control case; reject:
`T&` still correctly errors when `T` is a primitive or a plain value, unchanged from before).

### Generic trait bounds (static dispatch)
`<T: Trait>` / `<T: A + B>` constrain a type parameter to types implementing the named
trait(s) — user traits **and** built-in operator traits (`Add`/`Ord`/…). Enforced **both** at each
instantiation site (the concrete arg must implement the trait) **and** by checking the generic
**body** against the bounds at the definition (bounded params only).
- **Parser**: `GenericTemplate::bounds` (parallel to `typeParams`, `bounds[i]` = trait names).
  Both capture functions call `scanTypeParamList(from, typeParams, bounds)` which parses
  `T [: Trait (+ Trait)*] [, …]` (COLON/PLUS are separators). At monomorphization,
  `runMonomorphization` pushes a `GenericBoundCheck{typeName, traitName, context, line}` (in
  `Ast.h`, on `Program::genericBoundChecks`) per bounded param — `typeName` = the arg's head
  token lexeme (base type, `&` stripped), `context` = the instantiation's mangled name.
- **Semantic (instantiation site)**: `checkGenericBounds(program)` runs after `collectImpls` (pass
  0d). For each obligation: unknown trait (not built-in, not in `traitRegistry`) → error; else the
  type must be in `implementedTraits[typeName]` for that trait (both user + built-in impls populate
  it). Primitives / non-implementing classes → "type 'X' does not satisfy bound 'Trait' …".
- **Body checking (definition site)** — `checkGenericBodies` (pass 3, Rust-`where`-like, bounded-
  only). After monomorphization the parser re-parses each **bounded** template's original body
  (type-param names registered as types, own scratch registry so it can't perturb expansion) into
  `Program::genericTemplates` (a `GenericTemplateDecl{decl, typeParams, bounds}`). The pass binds
  each param name → bounds in `currentTypeParamBounds_`, then reuses `analyzeFunctionDecl` /
  `analyzeClassDecl` (the class is temporarily registered) so the body is analyzed with a new
  `TypeKind::TypeParam`. On a type-param value: **method calls** resolve against the bound traits
  (`resolveBoundMethod`: user-trait methods + built-in conventional `add`/`cmp`/`eq`/… with `Self`
  → the param); **operators** require the matching operator-trait bound; **field access** is an
  error (traits declare no fields). A type parameter and a reference to it (`T` ↔ `T&`) are
  **interchangeable** in `canImplicitlyCast` (gated on a `TypeParam` operand) — the value-vs-
  reference split isn't knowable abstractly (a bound method may return `Self` or `Self&`). A bounded param used beyond its bounds → error *at the
  definition* — even when the concrete instantiation type happens to provide it. **Unbounded
  params stay permissive** (empty bounds ⇒ suppressed, duck-typed at instantiation as before), so
  `addT<T>(T a,T b){return a+b;}` still compiles. Errors are single-shot (`error()` throws), so a
  failing monomorphized instantiation (pass 2) can pre-empt the pass-3 message for the same misuse;
  the body-check's unique value is catching use-beyond-bounds that instantiation would accept.

### Trait-name parameter sugar — `fn f(Trait a)` / `fn f(Trait& a)`
A bare (or `&`/`*`-sigiled) trait name used as a whole parameter TYPE is sugar for a bounded generic
(Java `interface`-parameter / Rust `impl Trait`-in-argument-position style): `fn square(Iterable x)`
means `fn square<T: Iterable>(T x)`. Implemented as a **pure token-level rewrite**
(`Parser::desugarTraitParams`, `Parser_Generics.cpp`) that runs BEFORE `tryCaptureFunctionTemplate`
ever sees the declaration, so the desugared function flows through the **entire existing** bounded-
generic pipeline (instantiation, `GenericBoundCheck` obligations, `checkGenericBodies`) with **zero**
semantic/codegen changes — same design as tuples/ternary/lambdas ("parser-only sugar").
- **Rewrite**: `fn [private] NAME (` at brace depth 0 (free function; no explicit `<...>` of its own)
  has its parameter list scanned for an IDENTIFIER that (a) is a known trait name, (b) sits in type
  position (preceded by `(`/`,`/`mut`), (c) is followed by an optional `&`/`*` sigil then the param's
  name IDENTIFIER. Each hit is rewritten to a **fresh synthesized type-param name** (`__T0`, `__T1`,
  …, unique per file) and a matching `<__T0: Trait0, __T1: Trait1, …>` is spliced in right after the
  function name — e.g. `fn same(Eq& a, Eq& b) -> bool { return a == b; }` becomes `fn same<__T0: Eq,
  __T1: Eq>(__T0& a, __T1& b) -> bool { return a == b; }`. An explicit sigil is preserved verbatim
  (copied through unchanged); a bare param keeps no sigil.
- **Why the sigil matters (not just cosmetic)**: a bare/no-sigil trait param is sugar for a **borrow**
  (`T*`), following the pre-existing "bare value-object parameter is sugar for a non-owning class
  borrow" rule. But GG forbids passing a borrow into a method expecting an **owning** reference
  (`Self&`) — and every built-in operator trait's conventional method (`eq(Self&)`, `add(Self&)`, …)
  and the common user-trait-method pattern both take `Self&`. So a bare-trait param can only call
  trait methods that take **no** Self-typed argument (e.g. a zero-arg `size()`); calling `a.eq(b)` or
  `a == b` between two bare-trait params is a clean "no matching 'eq' method" error. Writing `Eq& a`
  (an explicit `&`) desugars to an **owning-reference** type param instead — which *does* satisfy
  `Self&` — so operator-trait/Self&-taking usage requires the `&` sigil. This is discoverable, not
  silent: the error names the concrete instantiation type, and the fix (`&`) mirrors ordinary class
  param syntax exactly (`fn f(Obj o)` = borrow, `fn f(Obj& o)` = owning reference).
- **Each occurrence is an INDEPENDENT type parameter** (Rust `impl Trait` semantics): `fn f(Eq& a, Eq&
  b)` desugars to **two** unrelated bounded params (`__T0`, `__T1`), so `a` and `b` may bind to
  different concrete types at a call site — but that also means `a == b` between them is a compile
  error unless the caller happens to pass the same concrete type (checked at the definition by
  `checkGenericBodies`, same as any other bounded-generic body). A user who wants "both params must
  be the exact same type" writes the explicit form `fn f<T: Eq>(T& a, T& b)` — the sugar deliberately
  does not infer that constraint, exactly as Rust's `impl Trait` doesn't either.
- **Trait-name recognition**: built-in operator/marker traits (`Add`/`Sub`/`Mul`/`Div`/`Rem`/`Eq`/
  `Ord`/`Neg`/`Index`/`Clone`/`Iterator`/`Iterable`) are seeded unconditionally in the `Parser`
  constructor (`builtinTraitNames()`, `Parser.cpp` — a small literal kept in sync with
  `SemanticAnalyzer::isBuiltinTrait`'s set by hand, since the parser layer has no dependency on
  `SemanticAnalyzer`). User-declared traits are recognised **regardless of which file declares
  them**, mirroring the existing `classNames` cross-file prescan: `Parser::traitNames` (a **separate**
  set from `classNames`, so a bare trait name is never mistaken for an ordinary type anywhere else —
  var decls, return types, `new`, …) is seeded from `ImportResolver::collectTraitNames` (a
  `collectClassNames` clone: scans a file's own `trait NAME` tokens + recurses into every dependency)
  at `Parser` construction in `processFile`, PLUS a same-file `Parser::prescanTraitNames` call inside
  `parse()` itself (mirrors `prescanTemplateNames`'s own "idempotent if pre-seeded by the caller"
  pattern) so a bare single-`Parser` caller (unit tests, single-file compilation with no
  `ImportResolver`) still sees same-file trait declarations. `ImportResolver::resolve`'s pass-1
  `seedParser` is ALSO pre-seeded with the full-graph trait-name set (computed once up front, same as
  `collectClassNames` is for pass 3) so its own `desugarTraitParams` call — needed so a sugared
  function is recognised as generic in `gen_->funcNames`/not wrongly landing in
  `ordinaryFuncNames` — sees every trait regardless of file visitation order, including import
  cycles.
- **Call sites need no new code**: an inferred call (`square(x)`, no explicit `<...>`) rides the
  pre-existing "Generic type-argument deduction" feature (`inferGenericTypeArgs`) unchanged — the
  desugared function is an ordinary generic free function by the time any call-site code runs. An
  explicit `square<Array$i32>(x)` call also works unchanged.
- **v1 limits** (clean pre-existing errors, never a miscompile): **free functions only** — a method
  (inside a class body; brace depth > 0) is untouched by the rewrite, so a bare trait name there hits
  the ordinary "not a known type" parameter-type error. A declaration with its **own** explicit
  `<...>` is left completely untouched (no mixing sugar with hand-written generics on the same
  function) — a bare trait name used as a param type there also hits the ordinary type error (once
  the template is actually instantiated; an uninstantiated generic's body is never parsed at all, so
  a *dead* mis-typed template silently compiles, same as any other unused generic). No return-type or
  local-variable trait sugar (`-> Iterable` / `Iterable x = ...`) — that would be a **trait object**
  (erased type, runtime vtable dispatch), a fundamentally different feature requiring a new dispatch
  mechanism GG doesn't have; deliberately out of scope. Tests: `tests/trait_param_sugar_tests.cpp`
  (`[traitparam]`), `e2e/trait_param_sugar_test.gg`.

### Memory model
- **Value objects** (`Point p`): stack-allocated struct; copied on assignment.
- **`new` → value-object binding warns** (`analyzeVarDecl`): `Point p = new Point(...)` binds a heap
  reference (`new` → `Point&`) to a *value* variable, which silently deep-copies the fresh heap object
  into the stack value and frees the heap allocation immediately — almost always a mistake. A warning
  nudges toward `Point& p = new Point(...)` (bind the object) or `var p = new Point(...)` (infers the
  reference). Gated on `declaredType.kind == Object` + the initializer being a `NewExpr`, so `Point&`,
  `var`, and a plain value-to-value copy (`Point d = c;`) stay silent. (Copy still runs the generated
  memberwise `@Class_clone`, **not** the constructor — a copy duplicates state, it doesn't reconstruct;
  the `Clone` trait is the customization hook.)
- **Value-object fields (embedding)** — a class field may be another class *value*
  (`class Line { Point start; Point end; }`), lowered to a nested struct member
  (`%Line = type { %Point, %Point }`); its "value" is its address (a GEP), like a local value
  object. Copy semantics: clone **deep-copies** value fields (recursive `@Field_clone`) and
  **retains** reference fields; a dtor **recursively destroys** embedded value fields (calling
  `@Field_dtor` on the field address) + releases reference fields, reverse declaration order.
  `needsDtor` is **transitive** (a class embedding a dtor-needing value field needs one too;
  fixpoint in `generate()`); the clone worklist drains (recursive clone may enqueue more).
  **Value-embedding cycles are rejected** (`class A{B b} class B{A a}` = infinite size —
  `checkValueFieldCycles`, semantic pass 0a); use a reference to break the cycle. Enum fields
  may not be value objects (enums are pre-main singletons). Field types may forward-reference a
  class declared later (name pre-pass in `collectClasses` / codegen `generate()`).
- **Heap references** (`Point& r = new Point(...)`): intrusive-refcounted heap pointer.
  `new` produces a `+1` reference. Assignment rebinds (retains new target, releases old).
- **A bare value-object parameter is sugar for a non-owning class borrow** (`fn f(Obj o)` ≡
  `fn f(Obj* o)`). GG objects are address-backed — there is no by-value struct-param ABI — so a
  by-value object param can't be a *copy*; it lowers to a `ptr` borrow. Both layers map it: semantic
  (`analyzeFunctionDecl`/method/ctor param loops in `SemanticAnalyzer_Stmts.cpp`) turns an
  `Object` param type into `makeBorrowType(className)`, and codegen (`resolveParamType`) resolves a
  bare-object param token to the same borrow. Reads/method-calls go through the borrow; `field = o`
  into a value field **clones** (the source is not consumed); returning it is escape-checked like any
  borrow. Owning-ref params (`Obj&`) and primitive params are unchanged. **This is what lets one
  by-value generic constructor `Pair(T t, V v)` serve every type argument** — `T` a primitive
  (copied), a value-object (borrow + clone), or an owning reference `Foo&` (the field co-owns it) —
  since a reference is itself a `ptr` value and substitutes into `T t` with no sigil stacking. (Was
  previously a hard "object parameter must be passed by reference" error.) Primitive parameters
  always pass by value.
- **Value-object → reference borrow (argument position only)** — passing a *value* object
  where a `Class&` is expected is allowed and lowers to a **borrow**: the callee receives the
  object's address (no copy, no refcount change). A value object's SSA value is already a
  `ptr` to its body (`genIdentifier` returns the alloca), so codegen is a no-op — `emitCast`
  Object→Reference returns the value unchanged. Enabled by `canPassArgument`
  (`Type.cpp`) — used in the arg-matching paths (`resolveOverload` viability, its final
  `checkArgCast`, and `pickOverloadByArgs` for operators). **Binding contexts keep using
  `canImplicitlyCast` (still `None` for Object→Reference)** so `Class& r = valueObj`, ref
  reassignment, and ref-`return` stay rejected — a stack object has no refcount header at
  body-8, so retaining/releasing/+1-returning one would corrupt memory. The borrow is thus
  safe for pure-borrow callees (operators, readers). **Escape analysis (v1) now rejects the unsafe
  case**: see the escape-analysis invariant below. A borrow to a callee that returns/stores it into
  a reference field is a compile error; only truly-escaping borrows are rejected.
- **Refcount runtime** (emitted verbatim into IR when `new` is used, in `CodeGen_Emit.cpp`):
  - `gg_alloc(size)` — malloc with 8-byte header, initialises count to 1.
  - `gg_retain(ptr)` — null-safe refcount increment.
  - `gg_release(ptr, dtor)` — null-safe decrement; calls `dtor` + `free` at zero.
  - **NON-ATOMIC / single-threaded (by design, for now).** The count is a plain
    load-add-store (an `Rc`, not an `Arc`) — correct and cheap because GG has no threading
    (no thread-spawn, async, or shared-memory model). If concurrency is ever added, these three
    helpers must become atomic: a *relaxed* `atomicrmw add` for retain, and an `atomicrmw sub`
    with `release` ordering + an `acquire` fence on the count-reaches-0 path before dtor/free
    (the standard `shared_ptr`/`Arc` pattern). That is a ~10-line change here, but atomic counts
    alone are NOT enough for thread safety — reference **rebind** (`r = e` = retain-new /
    release-old / store) is three non-atomic steps, and shared mutable object state is
    unsynchronised, so a real threading story needs a memory model on top (deferred). **The full
    concurrency design is written up in `docs/concurrency.md`** — a distinct atomically-refcounted
    `Shared<T>` type (this atomic variant lives *inside* it; plain `Class&` stays non-atomic),
    `Sendable`/`Shareable` auto-derived markers, and a stdlib `Thread` class over a minimal spawn
    intrinsic. Design only, not implemented.
- **+1 return convention**: functions/methods returning `Class&` return a retained (`+1`)
  reference. Callee retains borrowed references before returning. Unbound temporaries
  (e.g. `make(99);`) are released at the full-expression boundary.
- **Ownership transfer via `claimTemp` (`CodeGen_Emit.cpp`)**: every `new`/ref-returning-call
  result is pushed to `pendingTemps_` (released at the full-expression boundary unless claimed).
  A consumer that takes ownership (a binding, a `return`, a field/`this` store) calls
  `claimTemp(ptr)` to remove it from the pending list. `claimTemp` **searches** the list — it is
  NOT necessarily the most recent temp, because a nested `new`/ref-call in the constructor args
  pushes further temps after it (`new Box(new Node())` ⇒ pending `[Box, Node]`, Node at back).
  The inner temp stays pending and is released (its `+1` was consumed by the outer object's field
  retain). *(Historical bug: `claimTemp` only checked `back()`, so the outer object was released
  as a temp and the caller received freed memory — double-free. Fixed; guarded by
  `tests/refcount_tests.cpp` + `e2e/refcount_leak_test.gg`.)*
- **Reference return alias transfers, never retains** (`emitReturnAlias`): a `-> Class& r`
  alias always owns exactly one `+1` (every `r = <expr>` either claims a `+1` producer or
  retains a borrow, in `genAssign`). On return that `+1` is handed to the caller directly — no
  extra `gg_retain`, and the alias is NOT in a dtor scope (not released in-function). *(Historical
  bug: it retained on return but was never released → returned a `+2` object → one-object leak
  per call. Fixed.)*

### `T*` — non-owning borrow (class **and** primitive)
`T*` (postfix sigil, mirroring the owning `T&`) is a **non-owning borrow** — a plain `ptr` with
**no refcount traffic** (never retained/released, never in a dtor scope, no `+1` on return).
`mut T*` reuses `mut` to make the borrow mutable. Owning `Class&` is unchanged; `T*` is the everyday
borrow for zero-copy element/parameter access. `T` may be a **class** (`Point*`) or a **primitive**
(`i32*`, an lvalue reference like C++'s `int&`) — but not `ptr`/`void`. **There is no `ref` keyword**
— the two user-facing reference forms are `&` (owning) and `*` (borrow); `ptr`/`ptr<T>` stays an
internal `--unsafe-ptr` escape hatch. *(The internal representation still uses the synthesized
`"ref:T"` token spelling — only the surface syntax is `*`.)*
- **Type representation**: reuses `TypeKind::Reference` + a `bool borrow` flag on `Type`. A **class
  borrow** sets `className`; a **primitive borrow** leaves `className` empty and stores the element
  in `elementKind` (`makePrimitiveBorrow` / `isPrimitiveBorrow` / `borrowElementType` /
  `decayPrimitiveBorrow` in `Type.h`). Class-borrow member access / method dispatch / passing reuse
  the Reference path unchanged; only the refcount decision sites branch on `borrow`.
- **Surface syntax → synthesized token**: `T*` → lexeme `"ref:T"` (unchanged internal spelling, so
  Type.cpp / semantic / codegen are untouched). The parser reads a **postfix `*`** in `consumeType`
  (mirroring the `&` branch) and `typeSpanAt` (consumes a trailing `AMPERSAND` **or** `STAR`); the
  `*` is accepted after a class name, `Self`, or a primitive keyword (`ptr`/`void` → parse error
  "… cannot be borrowed"). `decodeSynthesizedType` maps a primitive inner (via `typeKindFromName`)
  to a primitive borrow, else a class borrow. `typeName` renders a borrow as `i32*` / `Point*` and an
  owning reference as `Point&`. **Disambiguation from multiply** is identical to `&`: a postfix `*`
  is only read as a borrow when the head token is a known class/primitive type in a type position
  (declaration detection via `typeSpanAt`); `a * b` on a non-type head stays multiplication.
- **Primitive borrow = lvalue reference** (like C++ `int&`): reads **auto-deref** (load through the
  pointer) and writes **store through** it — it does not rebind.
  - *Read*: `analyzeIdentifier` returns `decayPrimitiveBorrow(sym->type)`; `analyzeBinary`/
    `analyzeUnary` decay operands first (so an `i32*` isn't misrouted into class operator
    overloading). `canImplicitlyCast` allows `P* → P` (deref) and `P → P*` (borrow a matching
    primitive lvalue). Codegen: `genIdentifier` loads the referent ptr then the value; `emitCast`
    `P* → P` emits a load.
  - *Write*: `analyzeAssign` has a primitive-borrow branch — `x = v` writes through (requires
    `mut T*`; a shared `T*` errors). Codegen `genAssign` loads the referent ptr and stores.
  - *Create / return*: `genBorrowSource` produces the referent pointer for a borrow source — the
    stored ptr for a borrow identifier, the address for a plain lvalue (`resolveAssignTarget` /
    `genElementAddress` / `resolveFieldGEP` for a `MemberAccessExpr`), or the value itself for a
    borrow-returning call. Used by `genVarDecl` (primitive-borrow init) and `genReturn`
    (primitive-borrow return). `analyzeVarDecl` requires the source be an addressable lvalue
    (`isLvalueExpr`) — a temporary (`a+b`) is rejected. **The `MemberAccessExpr` case was missing
    for a long time** — `genBorrowSource` only ever handled `IdentifierExpr`/`IndexExpr`, so an
    explicit `obj.field` (or `obj!!.field`) as a primitive-borrow source silently fell through to
    "not addressable" and returned `""`, emitting a malformed `store ptr , ptr %x.addr` (a missing
    operand) that only clang's parser caught. An *implicit*-`this` field (`return n;` inside a
    method, no `this.` prefix) was unaffected — that resolves via the `IdentifierExpr` branch's
    `resolveAssignTarget` fallback, which already covers implicit fields. Fixed by adding a
    `MemberAccessExpr` branch (excluding `?.`, which flows through a value slot, not a place) that
    resolves a static field to its global or an instance field via `resolveFieldGEP`, mirroring
    `genMemberAccess`'s own field-address logic. Found while wiring up a linked-list stdlib
    iterator (`ListIter<T>::next()`, `stdlib/utility/List.gg`), whose `T* val = pos!!.value;` was
    the first stdlib code to exercise a primitive borrow sourced from an explicit member access.
- **Coercions INTO a class borrow** (`canImplicitlyCast`, `Silent`): owning `Class&`→`Class*`, value
  `Object`→`Class*`, `Class*`→`Class*`. The reverse — a borrow → an owning `Class&` — is **forbidden**.
- **Borrow fields — allowed only under `--unsafe-ptr`, class borrows only** (`buildClassInfo`): a
  `Class*` borrow field is rejected by default (a borrow could outlive its source, which escape
  analysis can't prove across a stored field), but **permitted when `--unsafe-ptr` is on** — lifetimes
  become the programmer's responsibility, the same contract as raw `ptr<T>`. A **primitive** borrow
  field (`i32*`) is **always rejected** (no struct/clone lowering — it would be mistaken for the scalar
  value and wrongly retained). A borrow field lowers to a plain `ptr` and is **never
  retained/released** — all three refcount sites branch on `.borrow`:
  - **assignment** (`genMemberAssign`, `CodeGen_Classes.cpp`): a dedicated borrow branch stores the
    pointer with **no retain of the new / release of the old** and does **not** `claimTemp` a `+1`
    producer (a borrow takes no ownership). *(Missing this branch silently retained the target — the
    stdlib iterator's `it.arr = this` leaked the whole collection every loop; caught only by a
    live-object counter, since a leak doesn't change the exit code.)*
  - **clone** (`genCloneFunction`): shallow-copies it (falls through to the primitive/ptr branch, not
    the reference retain/release).
  - **dtor** (`genDtor`): skips it. And a borrow field does **not** set `hasRefField`/`needsDtor`
    (only owning `Class&` fields do).
  The canonical use is a **non-owning cursor holding a live handle to its collection** — e.g. the stdlib
  `ArrayIter<T>` holds `Array<T>*` and reads `arr.size()`/`arr.get(pos)` live, so a `grow()`/realloc
  during iteration is harmless (no snapshot to dangle). `it.arr = this` needs no cast — `this`
  (Object) → `Class*` is already a Silent coercion (unlike Object→`Class&`/`ptr<Class>`, both rejected).
  Tests: `tests/ref_tests.cpp` (`[ref]`: gated class-borrow accept/reject, primitive-borrow reject,
  no-retain assignment), `e2e/foreach_realloc_test.gg` (realloc-safety), `e2e/foreach_borrow_leak_test.gg`
  (no leak — a `~Vec` live counter returns to 0 after iterating).
- **A borrow-returning call is NOT a refcount temp**: every borrow-returning call site pushes its
  result to `pendingTemps_` only when `!resolvedType.borrow` (a borrow owns nothing, so releasing
  it would corrupt the real owner's count).
- **Escape analysis still applies**: passing a *stack value object* to a `T*` parameter that
  escapes (returned / ref-field-stored) is rejected — only heap references may escape.
- **Generics**: `T*` survives monomorphization (T→concrete substitutes on raw tokens before
  `consumeType`). `Array<i32>`'s `fn get(i) -> T*` instantiates to `i32*`.
- **Store through a returned reference** (`v.at(i) = x`): a new `RefStoreExpr{target, op, value}`
  AST node. The parser (`parseAssignment`) builds it when the LHS of `=` is not a name/index/member
  (those keep their dedicated nodes) — e.g. a call. `analyzeRefStore` requires the target to be a
  reference; a **primitive borrow** target stores the value into the referent (`checkCast` against
  the element), a class-reference target is rejected ("not yet supported"), a non-reference is
  rejected ("not assignable"). `genRefStore` evaluates the target to the referent pointer and
  `emitStore`s. So `v.at(i) = x` works for `ref <primitive>` returns; whole-object store-through is
  still deferred.
- **Primitive-borrow v1 limitations** (rejected with a clear message, never miscompiled): compound
  assignment (`+=` …) and `++`/`--` **through** a primitive borrow — write `r = r + 1` instead (they'd
  need referent write-through in the compound/postfix codegen); binding or passing a **temporary** to
  a `ref <primitive>` — a borrow needs an addressable lvalue. Passing a primitive lvalue *argument* to
  a `ref <primitive>` param takes its address in `buildArgString` (via `genBorrowSource`).
- Tests: `tests/ref_tests.cpp` (`[ref]`), `e2e/ref_test.gg`.

### `Shared<T>` — atomic, born-shared ownership handle (concurrency Phase 1)
The **lifetime axis** of the concurrency design (`docs/concurrency.md`): an owning heap reference like
`Class&`, but **atomically** refcounted and **born-shared** — the cross-thread ownership handle. This
is the `Rc` (`Class&`, non-atomic, thread-local) vs `Arc` (`Shared<T>`, atomic) split. The `Thread`
class + the `Sendable` thread boundary are now implemented too (see the "Threads" invariant below);
this is the fully-working handle type + its safety gate.
- **Representation**: a `bool shared` flag on `TypeKind::Reference` (mirrors the `borrow` flag) —
  owning + atomic. Reuses ALL the reference machinery (member access, method dispatch, **auto-deref**
  via the reference-receiver path) for free; new logic only at refcount / construction / cast / gate
  sites. `shared` and `borrow` are mutually exclusive. `makeSharedType`/`isShared` in `Type.h`.
  Synthesized token `"shared:Class"` (mirrors `"ref:Class"`); `typeName` → `Shared<Class>`;
  `mangleType` → `Class.shared` (a distinct symbol, never colliding with `Class&`'s `.ref`);
  `synthTypeToken`/`decodeSynthesizedType` round-trip it.
- **Parser**: a builtin generic like `ptr<T>`, recognised by the **reserved name `Shared` + `<`**
  (never a class name) — in `isTypeName`, `consumeTypeCore` (→ `"shared:Elem"`), and the `typeSpanAt`
  var-decl lookahead. Construction `Shared<Class>(args)` is a `parsePrimary` branch producing a
  `NewExpr{shared=true}` (reuses the heap-alloc + ctor lowering; build-in-place = born-shared, no raw
  alias). `parseOneTypeArg` rejects `Shared<…>` as a generic type argument with a clear Phase-1 error.
- **Semantic**: `resolveTypeToken` decodes it (+ validates `T` is a class — no `Shared<primitive>`).
  `analyzeNew` returns `makeSharedType` for a shared `NewExpr`. **Born-shared cast rules** (the
  `from.shared||to.shared` guard in `canImplicitlyCast`, plus `canPassArgument`): `Shared→Shared`
  (same class) is a retaining copy (Silent); `Shared→Class*` borrow and `Shared→Object` value-clone
  are allowed (safe view / independent copy, like `Class&→Class*`/`→Object`); `Shared→` **owning
  `Class&`** is FORBIDDEN (would create a non-atomic co-owner of an atomic object); anything **→
  `Shared`** is forbidden (born-shared — can't promote an existing alias).
- **Shareable gate** (`isShareableClass`, memoized structural fixpoint like `needsDtor`): a
  `Shared<T>` may only wrap a **Shareable** class, enforced at the construction site (`analyzeNew`).
  A class is Shareable iff every instance field is: a POD scalar (primitive/bool/char/str/enum, **any
  `mut`** — races to garbage, memory-safe); a **non-`mut`** owning-reference/`Shared` field whose
  pointee is Shareable (recurse); a **non-`mut`** value-object field of a Shareable class; or a `mut`
  value-object field of a transitively-**POD** class (`isPODClass`). A **`mut` reference-like field**
  (the #2 rebind double-free hazard) or ANY raw `ptr`/borrow field makes it non-Shareable →
  `analyzeNew` errors naming the offending field (`shareableReason`). Caches cleared per `analyze()`.
- **Codegen**: atomic runtime `gg_retain_shared` (relaxed `atomicrmw add`) / `gg_release_shared`
  (`release`-ordered `atomicrmw sub` + `acquire` fence before dtor/free — the `Arc` pattern), in
  `CodeGen_Emit.cpp`, emitted only when `sharedUsed_` (set at every shared lowering). The atomic-vs-
  non-atomic choice is centralized in `retainFn(bool)/releaseFn(bool)` helpers, threaded through a
  `shared` bit on `TempRelease`/`DtorEntry` and applied at **every** refcount site: construction/
  temp-release (`genNew`/`flushTempReleases`), var-init + copy + scope-exit (`genVarDecl`/
  `emitDtorsForScope`), rebind (`genAssign`), `+1` return (`genReturn`), and **fields** — clone
  (`genCloneFunction`), dtor (`genDtor`), store/rebind (`genMemberAssign`), default-init
  (`genFieldInitializer`). A missed atomic would be silent heap corruption under threads, hence the
  centralization. `hasRefField`/`needsDtor` already count `Shared` fields (they decode to
  `Reference{shared}`).
- **Phase-1 limits** (clean errors, never miscompiled): no `Shared<Shared<T>>`, no `Shared<T>` as a
  generic argument (`Array<Shared<T>>` — needs symbol-safe element mangling), no `Shared` tuple
  elements; a **mutable** shared reference field needs `Mutex`/`RwLock` (Phase 2, not built).
  `Shared<T>?` (nullable handle) IS allowed.
- Tests: `tests/shared_tests.cpp` (`[shared]`, 31 cases — type plumbing, born-shared + borrow/clone
  cast rules, Shareable gate, construction/rebind/return/field atomic IR), `e2e/shared_test.gg`
  (construct/auto-deref/copy/rebind/return, balanced refcount via a live counter),
  `e2e/shared_field_test.gg`, `e2e/shared_field_edge_test.gg` (const/nullable/user-dtor/nested/
  embedded field compositions). See `docs/concurrency.md`.

### Threads — stdlib `Thread` class + the compiler's "function-pointer part" (concurrency Phase 1)
The **execution axis** of the concurrency design (`docs/concurrency.md`). `Thread` is an **ordinary
stdlib class** (`stdlib/thread/Thread.gg`, module `std.thread`); the compiler provides only the parts
GG source can't express itself — heap-copying/trampolining a closure and the OS-thread runtime. User
surface: `Thread t = Thread.create(() -> { … }); … t.join();`. The closure is a `Call() -> void`
callable (a lambda); its captures cross into the spawned thread and so must be **Sendable**.
- **stdlib `Thread`** (non-generic — no `Thread<F>` needed): one `private mut ptr handle` field, a
  static factory `fn static create<F: Call() -> void>(F closure) -> Thread t`, and `fn join()`.
  `create` builds the thread state directly on the return slot (`t.handle = gg_thread_create(ent,
  clo)`), no nested ctor. Declares `extern gg_thread_create/gg_thread_join` (DEFINED by the compiler,
  like `gg_stdout`). Being under `stdlibDir`, its raw `ptr` field/params are exempt from the
  `--unsafe-ptr` gate.
- **Two compiler intrinsics** (recognised by name in `analyzeCall`/`genCall`; called INSIDE the
  monomorphized `create$__lambda_N` where the closure's concrete class `F` is known):
  - `__gg_heap_closure(closure) -> ptr` (`genCall`): `gg_alloc(sizeof %F)` + `store zeroinitializer`
    + `@F_clone(heap, src)` — a heap copy that **retains** (atomically, for `Shared` captures) the
    closure's captured fields, so it outlives the spawning frame. Returns the heap ptr.
  - `__gg_trampoline(closure) -> ptr`: `emitThreadTrampoline(F)` then returns `@__thread_entry$F` as
    an opaque ptr. Both take exactly one arg and return `ptr` (semantic: `Type{Ptr}`).
- **Per-closure trampoline** `@__thread_entry$F` (`emitThreadTrampoline`, deduped via
  `threadTrampolines_`): a C-ABI entry that `call @F_call(ptr %arg)` then `gg_release(%arg,
  @F_dtor)` (drops the closure's captures — the thread solely owns the heap closure) and returns.
  Return type is **platform-specific**: Windows `i32` (CreateThread's `DWORD(*)(LPVOID)`), Linux
  `ptr` (pthread). The closure dtor releases captured `Shared<T>` atomically, so exactly-once
  destruction holds even as main and the spawned thread race the count.
- **Runtime** `@gg_thread_create(ptr fn, ptr arg) -> ptr` / `@gg_thread_join(ptr handle)` /
  `@gg_thread_dispose(ptr handle)` (`emitThreadRuntime`, gated on `threadsUsed_`; emitted after
  `emitStdioHelpers`): Windows → `CreateThread` + `WaitForSingleObject(h, -1)` + `CloseHandle`
  (needs **`-lkernel32`**, added to `compile.ps1`); Linux → `pthread_create`/`pthread_join` +
  a **no-op** dispose. Removes the stdlib `extern gg_thread_create/join/dispose` **declares** (LLVM
  forbids declare + define), mirroring `emitStdioHelpers`' `gg_stdout` handling. (The block label is
  `entry:`; the fn param is `%fn`, not `%entry` — a label and an SSA value share one namespace in
  LLVM, so `%entry` would collide.)
- **Handle release — `~Thread` + `gg_thread_dispose`** (fixes the OS-handle leak): `Thread` has a
  destructor `~Thread() { gg_thread_dispose(this.handle); }`, so the handle is released at scope
  exit. On **Windows** `gg_thread_dispose` = `CloseHandle` (required — `WaitForSingleObject`/`join`
  does NOT close the handle; closing a still-running thread's handle just drops our reference, so it
  is correct whether or not the thread was joined first → **no leak** in either the joined or
  never-joined case). On **POSIX** `pthread_join` already frees the thread, so dispose is a **no-op**
  and the common create+join path leaks nothing; a POSIX thread that is **never joined** leaks until
  process exit (Phase-1 limit — a real `detach()` is Phase-2, since detach-after-join is UB and
  distinguishing the two needs per-`Thread` join-state we don't track yet). **Side effect (correct
  and intended): `Thread` is now non-copyable** — a raw-`ptr`-owning field + a user destructor trips
  the raw-pointer copy guard (`rejectUncloneablePtrOwner`), so `Thread a = b;` is a clean compile
  error (a thread handle must never be duplicated — double-`CloseHandle`). The `Thread t =
  Thread.create(…)` surface is unaffected (sret direct-construct + return-slot transfer, not a copy);
  `join()` stays non-`mut`, so `Thread t` (const binding) still works.
- **Lambda inference for a single-`Call`-bounded generic STATIC method** (`Thread.create(() -> {})`):
  `parsePostfix`'s method-call path gained the free-function lambda-inference machinery — resolve the
  receiver class, look up `methodTemplates[Class::method]`, and if it has exactly one `Call`-bounded
  type param and a lambda-literal argument, infer that param as the lambda's generated class and emit
  the mangled `MethodCallExpr` (`create$__lambda_N`). Works via `.` or `::`; the signature-exposure
  half (for typed-param lambdas) is also wired. A no-arg `() -> {}` needs no exposure (already parses
  as `Call() -> void`).
- **Sendable boundary check** (`isSendableType`/`isSendableClass`, memoized `sendableCache_`, cleared
  per `analyze()`): enforced in `analyzeCall`'s `__gg_heap_closure` special-case — every field of the
  concrete closure class (`__lambda_N`, its captures) must be Sendable, else a clear error naming the
  field + suggesting `Shared<…>`. Sendable = copied-or-atomic: primitive/bool/char/str/enum (copied),
  `Shared<T>` (atomic), or a value object all of whose fields are Sendable. NOT Sendable: a non-atomic
  owning `Class&`, a borrow `Class*`, a raw `ptr`/`ptr<T>`, or a value object embedding any of those
  (cloning would retain a non-atomic ref shared across threads). An abstract type-param receiver (the
  un-instantiated template body) has no class in the registry and is skipped — the concrete
  instantiation is what's checked. **v1 wart**: the error attributes to the stdlib
  `__gg_heap_closure` line, not the user's capture site (the message names the offending field, so
  it's actionable).
- **Static confinement — `checkThreadClosures`** (`SemanticAnalyzer_Thread.cpp`, a post-analysis pass,
  the ambient-globals counterpart to the capture check): a `static` field touched by name inside a
  thread body is **not** a capture (not a field of the `__lambda_N` class), so the boundary check
  above can't see it — this pass closes that hole. It walks each recorded thread-closure class
  (`threadClosureClasses_`, filled at `__gg_heap_closure`) starting from its `call` method,
  **transitively** into every function/method it invokes (a body-pointer-deduped worklist), and
  gates every static it reaches with **`isSharedSafeField`** — the Shared<T>-**field** rule, NOT
  `isSendableType`. This distinction is load-bearing: `isSendableType` governs by-value **captures**
  (copied in), but a static is **shared in place**, so the correct rule forbids a **`mut`
  reference-like** slot — including an atomic **`mut static Shared<T>`** — because it can be REBOUND
  from two threads (a torn store / double-free; atomicity guards the count, not the slot). Allowed:
  `mut static i32` (POD scalar), a **non-`mut`** `static Shared<T>`, a non-`mut` `static Class&` to a
  Shareable class. Rejected: `mut static Class&`, `mut static Shared<T>`, any raw-ptr/borrow static.
  (`isSharedSafeField(Type,isMut)` is the single source of truth, shared with `isShareableClass`,
  which was refactored to call it — the Shareable-gate and the static gate can never diverge.)
  Runs as pass 4 (after `checkGenericBodies`), so the `typeMap` is complete — method receivers
  resolve precisely (via `typeMap` / a bare class name for the `Class::m` static form); free-function
  calls scan **all** overloads of the name (sound over-approximation — never misses a reachable
  static, may over-report). Bare-identifier static reads are gated only when the name is not a
  declared local/param (shadowing), avoiding that false positive. Errors point at the **actual**
  offending static reference (even deep in a transitively-called function), a better location than
  the stdlib-attributed capture error. **Residual v1 gap**: destructors invoked implicitly by a
  refcount *release* on the thread (e.g. a captured `Shared<T>`'s `T` dtor when the thread drops the
  last ref) are not in the call AST, so a dtor touching a non-Sendable static isn't traced — exotic,
  documented, not a common footgun.
- **v1 limits / deferred**: no `detach()` (a never-joined POSIX thread leaks until process exit — see
  the handle-release bullet; Windows is leak-free either way); no thread return values, no
  `Mutex`/`RwLock` (a `mut` `Shared` field is still Phase-2), no thread-local storage. (Static
  confinement is now enforced — see the bullet above; a plain `mut static` scalar remains touchable,
  which is memory-safe by design.)
- Tests: `tests/thread_tests.cpp` (`[thread]`, 22 cases — intrinsic + trampoline + runtime + dispose
  codegen, static-method lambda inference via `.`/`::`, a **hand-written `impl Call` callable**
  (non-lambda, via ordinary generic inference into the intrinsics), Sendable accept for primitive/
  `str`/enum/`Shared`/value-object-of-Sendable-fields/value-object-embedding-`Shared` (the
  `isSendableClass` recursion, both polarities), Sendable reject for owning-ref/borrow/embedding-
  owning-ref captures, the error naming the offending capture (not a Sendable sibling), **static
  confinement** (`[thread][statics]`: Sendable `mut static i32` / `static Shared<T>` allowed;
  non-Sendable `mut static Class&` rejected directly and transitively via a called free-function
  **and** a called method; a **`mut static Shared<T>` rebind** rejected (the rebind-race edge — gated
  by the shareable-field rule, not isSendableType); a non-Sendable static reached **two calls deep
  through recursion** rejected (worklist depth + cycle-termination); implicit-bare and instance-
  receiver access to a Sendable static allowed; a local shadowing a static's name not falsely gated),
  and intrinsic arity), `e2e/thread_test.gg` (a thread mutating a captured `Shared<T>`, joined, with a live-counter
  proving exactly-once destruction — 30/30 runs deterministic). See `docs/concurrency.md`.

### Nullable types — `T?` (Kotlin-style)
`T?` marks a **nullable** type, with two representations by underlying kind:
- **Reference-like** (`Class&?`, `Class*?`, `Color?`) → a plain `ptr` (null = machine null), so `T`
  and `T?` are **bit-identical** and the null-safe `gg_retain`/`gg_release` need no change.
- **Primitive** (`i32?`, `bool?`, …) → an inline tag+payload value `{ i1 present, iN value }` (a
  stack value type, no heap). `irTypeName` emits the struct; `emitCast` wraps (`iN` → insertvalue
  `{true, v}`) / a `null` → `zeroinitializer`; `!!`/`?:`/`== null` extract the tag (field 0) and
  payload (field 1); a smart-cast narrows via `genIdentifier` payload-extract (no runtime check).
**Value objects (`Point?`) are rejected** (no null representation); `ptr?`/`void?` rejected.
- **`?.` safe-call** (`x?.field` / `x?.m(args)`): semantics narrow the receiver and make the result
  nullable (`wrapSafe` in `analyzeMemberAccess`/`analyzeMethodCall`; a value-object result is
  rejected — return a reference/primitive). Codegen (`genMemberAccess`/`genMethodCall` safe branch)
  evaluates the receiver once, `icmp ne ptr recv, null`, and flows through a result slot: the null
  path leaves the empty default (`null` / `zeroinitializer`), the present path does the field
  load / instance call and wraps. `?.` never yields a value object, so there's no sret path; an
  owning-reference result is pushed to `pendingTemps_` (null-safe release covers the null path).
- **Tokens/AST**: lexer `null` (`NULL_LITERAL`), `?.`/`?:` (`QUESTION_DOT`/`QUESTION_COLON`); `!!`
  is parsed as **two `BANG` tokens** (no dedicated token); `?` (`QUESTION`) as a postfix type
  suffix. New expr nodes `NullLiteralExpr`,
  `UnwrapExpr` (`x!!`), `ElvisExpr` (`a ?: b`); a `bool safe` on `MemberAccessExpr`/`MethodCallExpr`
  for `?.`. Elvis sits just below assignment (right-assoc); `!!` is postfix.
- **Type**: reuses the (pre-existing) `Type::isNullable` flag + `TypeKind::Null` (the type of
  `null`). `makeNullable`/`stripNullable`. `canImplicitlyCast`: `null → T?` and `T → T?` are Silent
  (implicit wrap); `T? → T` is **None** (must unwrap). `typeName` renders `T?`. `consumeType` reads
  a trailing `?` → a synthesized `"...?"` token; `decodeSynthesizedType` and the semantic/codegen
  resolvers strip it. `resolveTypeToken` rejects nullable value objects + `??` + (Phase-1) primitives.
- **The `&`/`*` sigil must come BEFORE `?`, never after** (`Class&?`/`i32*?`, not `Class?&`/`i32?*`):
  the type-suffix grammar is fixed as `BaseType [&|*] [?]`, never the reverse — there is only one
  nullability axis for a reference (a valid, non-null handle always points at a fully-formed object;
  there is no separate "valid handle to an absent object" state), so the reversed order could never
  carry different semantics even if it parsed. `consumeType()` (`Parser.cpp`) throws a precise error
  ("'&' must come before '?', not after; write 'C&?' instead of 'C?&'") when a `&`/`*` immediately
  follows a `?`. This requires `typeSpanAt` (`Parser_Generics.cpp`, the type-detection look-ahead used
  by `parseExpression`'s var-decl branch) to also recognize the reversed `? &`/`? *` order as *part of*
  the span — otherwise the declaration-detection branch never fires at all (a `?` with no sigil after
  it looks like a complete, valid nullable type to the look-ahead), and the dangling `&`/`*` gets
  misparsed as something unrelated (`C?& a = null;` used to fall through to `parseTernary` — `C` as a
  scrutinee, `?` starting a ternary, then `& a` failing inside the ternary's "then" branch — surfacing
  a confusing "expected expression" instead of the real cause). Fixed at both sites so the real
  diagnostic is always reached, for both class and primitive-borrow forms (`i32?*`).
- **Operators**: `!!` (`analyzeUnwrap`/`genUnwrap`) null-checks and `abort()`s on null (no
  exceptions), else passes the ptr through. `?:` (`analyzeElvis`/`genElvis`) selects the non-null
  operand via a result slot. `== null` / `!= null` route through `classifyEquality`'s new null case
  → the address-identity `icmp eq/ne ptr …, null` path.
- **A nullable operand is rejected by every NON-equality operator** (`analyzeBinary` after the
  equality case, and `analyzeUnary` up front): arithmetic / bitwise / shift / relational / unary
  `!`/`-` all error on an `isNullable` operand ("narrow it with a null check … or unwrap with `!!`").
  This is load-bearing for nullable **primitives** — the operand's `Type::kind` is the *underlying*
  kind (an `i32?` has `kind == I32`), so the `isNumeric(kind)` checks would otherwise wave it through
  and codegen would silently `extractvalue` the payload even when null. The `isNullable` flag is the
  only thing distinguishing `i32?` from `i32` at an operator, so it must be checked explicitly. After
  a narrowing (`if (x != null)`, a guard clause, or `x!!`) `analyzeIdentifier` returns the
  `stripNullable` type, so the operand is non-nullable there and the operators apply normally.
- **Smart-casts** (`Symbol::isNarrowedNonNull`, snapshotted like `isInitialized`): `if (x != null)`
  narrows the then-branch, `if (x == null)` the else; **guard clauses** (`if (x == null) { return; }`
  → non-null after) reuse `alwaysReturns`; branch joins merge (non-null iff on every reaching path).
  `analyzeIdentifier` returns `stripNullable` for a narrowed binding (recorded on the use-node —
  free for references, same `ptr`). **`mut` invalidation**: `analyzeAssign` refreshes the bit from
  the RHS (non-null value ⇒ still narrowed; `null`/nullable ⇒ dropped). Loops intersect pre-loop and
  post-body narrowing (`mergeLoopNarrowing`) so a body can neither leak nor unsoundly keep a cast.
  **Bare local/param bindings only** (not field chains); `&&`-narrowing not done.
- **Codegen**: nullable references need **no representation change** (still `ptr`); the smart-cast
  is representation-transparent for references. A **nullable→nullable cast across payload widths**
  (`i64? → i32?`) unpacks the `{ i1, iN }` (extract tag + payload), converts the payload through the
  scalar rules, and re-wraps under the same tag — a plain `trunc`/`ext` on the whole struct is invalid
  LLVM and was a real emitted-IR bug (only clang's verifier caught it; GG doesn't self-verify its IR
  before handing off to clang). Tests: `tests/nullable_tests.cpp` `[nullable]`, `e2e/nullable_test.gg`.

### Brace construction — `Type{args}` (C++-style)
Braces are an **alternate delimiter for a positional constructor call** — `Point{1, 2}` ≡
`Point(1, 2)`, same overload resolution. No aggregate/field-fill, no named fields. Two forms:
- **Typed** `Point{args}` (and `Point p{args}`, `new Point{args}`): the parser emits an ordinary
  constructor `CallExpr`/`NewExpr` — **zero new semantic/codegen**. Handled in the var-decl path
  (`(`/`{` both accepted), `parsePrimary` (a class-name identifier followed by `{`), and the `new`
  parser.
- **Untyped** `{args}` — the type is **deduced from the expected type** at the use site (a
  constructor argument, a var initializer, or a return): the type-elided inner form, e.g. `{0,0}`
  in `Line l{ {0,0}, {1,1} }`. A new `BraceInitExpr{args, brace}` AST node (parsed by `parsePrimary`
  on a leading `{`). `analyzeBraceInit` reads `expectedType_` → the class, resolves its constructor
  overload (like a `Class(args)` call), records the class in `SemanticResult::braceInitClass`
  (+ `resolvedCallee` for the overload). `resolveOverload` injects the per-arg expected type for a
  brace-init argument, but only when the **enclosing call** is unambiguous (a single candidate) — an
  untyped `{...}` argument to an *overloaded* function/method can't know which param type to deduce
  from, so it requires an explicit `Type{...}`. (The deduced class's *own* constructor may be freely
  overloaded — that's resolved normally once the class is known.)
  `genBraceInit` materializes a temp value object + runs the ctor + returns its address (same as the
  constructor-rvalue path in `genCall`), so it borrows into a `Class&` param / initializes a value.
- **Enums** are not brace-constructable (`Color{...}` rejected, like `Color(...)`). Untyped `{...}`
  with no class context (a primitive/enum expected type, or none) → clear error.
- **Default-construct with no constructor**: `C c{}` / `C c()` on a class with no ctor zero-initializes
  (like `C c;`) — the var-decl object-init path guards the ctor call on `funcParamTypes` containing it
  (previously it emitted a call to a non-existent `@C_C`). Fixed with the brace work.
- Tests: `tests/brace_init_tests.cpp` (`[brace]`), `e2e/brace_init_test.gg`.

### Numeric-literal type adoption (untyped constants)
A bare numeric literal has **no fixed type of its own** — it adopts the contextual expected type
(Go/Zig "untyped constant" model), falling back to the default only when there's no numeric context.
Implemented entirely at the literal-typing site: **no new type-kind, no codegen restructuring**.
- **Default**: integer literal → `i32`, decimal literal → `f64` (unchanged when there's no context).
- **Adoption** (`analyzeLiteral`, `SemanticAnalyzer_Exprs.cpp`): when `expectedType_` (stripped of
  nullability) is a numeric type, the literal takes it — `i64 y = 5;` types `5` as i64, `f32 f = 1.0;`
  types `1.0` as f32, `u32 n = 5;` / `u8 c = 5;` adopt cleanly (no widening/narrowing/sign warning,
  no more `u8 = 5` *error*). An integer literal in a **float** slot adopts the float type.
- **Range check** (`integerLiteralFits` in `Type.cpp`): an integer literal's magnitude is checked
  against the adopted type — out-of-range **warns** (still adopts, truncating): `i8 b = 300;`. Signed
  types allow magnitude up to `2^(bits-1)` so a negated boundary (`i8 = -128`) is accepted (a
  negative literal is a `UnaryExpr(-)` over the literal, and `analyzeUnary` doesn't clear
  `expectedType_`, so the inner literal adopts). With **no** numeric context the default is i32 and an
  i32-overflowing literal warns (`var z = 9000000000;`) — this also fixes the old **silent** i32
  overflow of `i64 big = 9000000000;` (now typed i64 directly).
- **Direct bindings + a non-literal sibling**: `analyzeBinary` snapshots and **clears** `expectedType_`
  around its operands (as `resolveOverload` does for call args), so a literal buried in a subexpression
  keeps its default type — **except** a bare numeric-literal operand adopts the type of a *non-literal*
  numeric sibling (`isNumericLiteralOperand`, incl. a negated literal). So `bigI64 == 6000000000` and
  `bigI64 + 6000000000` type the literal as i64 (no wrap/overflow warning), while the both-literals
  `1 / 2` (no non-literal sibling) stays **integer** division (→ 0.0), and a *decimal* literal never
  adopts an integer sibling (`analyzeLiteral` keeps it f64), so `1.0 / count` stays float division.
  Function-argument literals still keep their default and reach the parameter via silent widening.
- **Compound assignment** (`analyzeCompoundAssign`): the RHS is analysed with the target's type as the
  expected type (`analyzeWithExpected(rhs, lhsType)`), so a bare literal adopts it — `u64 x; x *= 10;`
  types `10` as u64 (no spurious i32→u64 narrowing warning). Literal-only, like the other binding
  contexts: a **non-literal** RHS (an `i32` variable into a `u8 +=`) still warns on a genuine lossy
  conversion via `checkCast`.
- **Codegen** (`genLiteral`): emits the literal at its resolved semantic type. Integer literals are
  **masked to the target width** (`unsigned long long` parse, `& ((1<<bits)-1)`) so the constant is
  always a valid `iN` even when out of range — `i8 = 300` → `store i8 44`, not the malformed
  `store i8 300` (a >u64 literal that can't even be parsed emits `0`; both cases are already warned).
  An integer lexeme in a float slot becomes a decimal (`5` → `5.0`); an **f32** literal is emitted as
  an **LLVM hex-float** (`store float 0x3FE0000000000000`) computed from the value rounded to float —
  necessary because f32 was previously only ever reached via `fptrunc` from an f64 constant, and LLVM
  rejects a non-exactly-representable decimal for `float`.
- Tests: `tests/cast_tests.cpp` (`[numlit]`).

### Hex / octal literals + digit separators (`0x`/`0X`, `0o`/`0O`, `_`)
Integer literals may be written in hex (`0x1F`/`0X1F`) or octal (`0o17`/`0O17`), and any numeric
literal (integer or decimal) may use `_` as a digit separator (`1_000_000`, `1_234.5`) for
readability. **Deliberately NOT supported**: legacy C-style "a leading zero means octal" (`017`) —
that's a classic ambiguity footgun (and would collide with a plain `0` literal); an explicit `0o`
prefix is unambiguous. No hex-float syntax (`0x1.8p3`) — hex/octal literals are integer-only.
- **Lexer** (`Lexer.cpp`, the digit-scan branch): after a leading `0`, a following `x`/`X` switches to
  scanning hex digits (`isxdigit`), `o`/`O` to octal digits (`0`-`7`); otherwise the existing decimal
  scan applies. `_` is accepted anywhere in the digit run (integer part, decimal part, and inside a
  hex/octal run) and is stripped later by `stripDigitSeparators` — the lexer does no validation on
  placement (`1_.5`, `_1`-after-prefix, etc. all lex as one `NUMBER` token; a downstream numeric parse
  is the only validator). A bare prefix with no digits (`0x;`, `0o;`) is a **lex error** ("no digits
  after '0x'"/"'0o'"), not a silent malformed token.
- **Shared parsing helper** (`Type.h`/`Type.cpp`, since every literal-magnitude call site needed the
  same fix): `stripDigitSeparators(lexeme)` removes `_`; `isPrefixedIntegerLiteral(lexeme)` detects a
  `0x`/`0X`/`0o`/`0O` prefix (used to guard the pre-existing '.'/'e'/'E' "is this decimal?" sniff — a
  hex digit run can itself contain `e`/`E`, e.g. `0xFE`/`0xE0`, which would otherwise be misdetected as
  a float); `parseIntegerLiteral(lexeme, magnitude)` strips separators, dispatches on the prefix, and
  parses the remaining digits at base 16/8/10 via `std::stoull` (false on malformed/> u64, mirroring
  the try/catch pattern every call site used before). This **replaced 5 independent ad-hoc
  `std::stoull`/`std::stoll` call sites** (magnitude check in `analyzeLiteral`, IR emission in
  `genLiteral`, the switch-label dedup key in `labelKey`, the array-size parse in `parseVarDecl`'s
  array-decl branch, and the constant-index-bounds check) — consolidating them removed the risk of
  fixing hex/octal support in some call sites but not others.
- **No new AST/type-system changes** — still a `LiteralExpr`/`NUMBER` token; only which *base* the
  digits are read in changes. The numeric-literal-**adoption** logic (which type a bare literal takes)
  is untouched — `0xFF` in an `i8` slot warns "does not fit" exactly like `255` would.
- **Codegen — a prefixed integer literal in a FLOAT slot must be converted to decimal first**
  (`genLiteral`): `f64 d = 0xFF;` would otherwise append `.0` to the raw lexeme → `store double
  0xFF.0` (malformed IR — a real bug caught only by clang's parser, since GG doesn't self-verify its
  IR; the f32 path fed the raw `0xFF` to `strtof`, which is platform-dependent). Both float paths now
  parse a prefixed literal to its magnitude and emit the decimal (`255.0` / the hex-float encoding of
  `255`). A plain decimal literal just has its `_` stripped (`_` is invalid in an LLVM float constant,
  and `strtof` stops at the first `_`). The integer path emits the parsed *value*, so the source base
  is irrelevant there.
- **Lexer requires at least one REAL digit** after a prefix (`sawDigit` flag, not just "consumed
  something"): `0x`, `0o`, and separator-only `0x_`/`0o_` are all clean lex errors ("no digits after
  '0x'") rather than slipping through as a `NUMBER` token that later parses to 0 with a misleading
  "does not fit" warning.
- Tests: `tests/cast_tests.cpp` (`[numlit]`, lexer/codegen/semantic cases incl. the `0xFE`/`0xE0`
  hex-digit-vs-float-exponent guard, the hex/octal-in-float-slot regression, `0x_`/`0o_` lex errors, a
  negated hex literal, an out-of-bounds hex array index, a hex/decimal switch-label duplicate-detection
  cross-check, and the array-size parser), `e2e/hex_octal_literal_test.gg`.

### Mixed-arithmetic common type — signed wins (`commonArithmeticType`, `Type.cpp`)
The unified type for a **symmetric** binary numeric op (both the semantic result type and the codegen
operand type) is: **float dominates** (`f64` > `f32` > any int); both-same-signedness ints → **wider
wins**; **mixed signed/unsigned → a SIGNED int of the wider width** (GG departs from C's "unsigned
wins"). So `-6 / 3u == -2` (`sdiv`, not `udiv`), `-1 < 1u` is true (`slt`, not `ult`) — the unsigned
operand is reinterpreted as signed. The division/compare *instruction* (`sdiv`/`udiv`/`fdiv`,
`slt`/`ult`/…) is chosen from this common type in `arithInstr`/`cmpInstr`. Integer `/` truncates
toward zero. Both operands are `emitCast` to the common type before the op.
- **Shifts are the exception** (`<<`/`>>` in `analyzeBinary`): a shift is *asymmetric*, so its result
  type and its `ashr`-vs-`lshr` choice follow the **left operand only** (not `commonArithmeticType`).
  So `u32 >> anyCount` is always **logical** (`lshr`) and `i32 >> anyCount` always **arithmetic**
  (`ashr`), regardless of the count's signedness; the count is `emitCast` to the left type (LLVM
  requires equal shift-operand types). (Symmetric `& | ^` still use the common type.)
- Tests: `tests/cast_tests.cpp` `[arith]`.

### Logical `&&`/`||` genuinely short-circuit (fixed correctness bug)
`genBinary`'s `AND`/`OR` branch is labeled "short-circuit logical ops" in a comment but used to
**eagerly evaluate both operands** and combine them with a plain `and i1`/`or i1` — no branching at
all. This was a real, silent correctness bug, not just a missed optimization: it meant every
`guard && use-guarded-value` idiom in GG (the standard way to make an unsafe operation conditional —
null checks, bounds guards, etc.) was unsound. E.g. `i < len && arr[i] == x` would still evaluate
`arr[i]` (and hit *its own* runtime bounds check) even when `i < len` was false — the guard changed
the final boolean result but never prevented the right-hand side from running. Caught via a stdlib
`printf`-style scanner (`stdlib/io/IO.gg`'s `nextPlaceholder`) that used exactly this pattern
(`i + 1 < len && s[i + 1] == '{'`) and crashed with a bounds-check trap instead of safely returning
false at the end of the string.
- **Fix** (`CodeGen_Exprs.cpp::genBinary`): real branching, mirroring `genElvis`'s result-slot
  pattern. Evaluate the left operand; branch (`and`: false → short-circuit, true → evaluate right;
  `or`: true → short-circuit, false → evaluate right) via `emitCondBr` to a `*.short`/`*.rhs` block;
  each path stores its result (the left value on the short-circuit path, the right value otherwise)
  into an `i1` alloca; a `*.merge` block loads and returns it. The right operand's code — including
  any bounds check / null deref it contains — is only ever emitted on the path that actually runs it.
- No AST/semantic change — `analyzeBinary`'s typing of `&&`/`||` was already correct (always `bool`);
  only the codegen shape changed. Byte-identical results when both operands are side-effect-free;
  behaviorally different (correctly so) whenever the right operand is unsafe to evaluate under a
  false/true guard.
- Tests: `tests/codegen_tests.cpp` (`[codegen]`, asserts the branch/label shape and the ABSENCE of a
  bare `and i1`/`or i1`), `e2e/short_circuit_test.gg` (behavioral: an out-of-bounds guard that would
  abort() if the right side were still evaluated, for both `&&` and `||`, plus a truth-table sanity
  check that the boolean VALUE is unchanged by the new codegen shape).

### Integer overflow checks — `--overflow-checks` (Rust-style, opt-in)
Off by default; when enabled, integer overflow and out-of-range narrowing **trap at runtime**
(`abort()`). `CompilerOptions::overflowChecks` (parsed from `--overflow-checks` in `main.cpp`;
`compile.ps1 -OverflowChecks`) → `CodeGen::overflowChecks_`. **With it off the emitted IR is
byte-identical to before** — every check is gated on the flag.
- **Arithmetic** (`genBinary` + `genCompoundAssign` + `genUnary` + `genPostfix`): integer `+`/`-`/`*`
  (and `+=`/`-=`/`*=`, the `++`/`--` prefix **and** postfix forms, and signed unary `-` — which catches
  `-INT_MIN`) route through `emitCheckedArith`, which calls `llvm.{s,u}{add,sub,mul}.with.overflow.iN`
  (s* for signed, u* for unsigned; the intrinsic is `declare`d once, deduped in `module.declares`),
  extracts the `{result, i1 overflow}` pair, and `emitOverflowTrap`s on the overflow bit. **Not
  checked**: division/remainder, shifts/bitwise, all float arithmetic, and unsigned unary `-` (a
  degenerate op) — all kept on the plain-instruction path.
- **Conversions** (`emitCast`, integer→integer branch): an **implicit** narrowing (`toBits < fromBits`)
  is verified by `trunc`→extend-back (`sext`/`zext` per the target's signedness)→`icmp ne` against
  the original; a same-width signedness flip checks the sign bit (`icmp slt …, 0`). Out of range →
  `emitOverflowTrap`. An **explicit `as` cast never traps** — `genCast` calls `emitCast(..., checked=false)`
  (intentional truncation, matching Rust/C); `emitCast`'s new trailing `bool checked = true` param
  defaults on so all *implicit* conversion sites are checked, and only `genCast` opts out.
- **Trap** = `emitOverflowTrap(badCond, what)` (`CodeGen_Emit.cpp`): mirrors `emitBoundsCheck` —
  cond-branch to a block that prints a diagnostic then `abort()`s + `unreachable`, continue in a fresh
  block. `what` is `"integer overflow"` (arithmetic) or `"value out of range in narrowing conversion"`
  (checked `emitCast`).
- **Runtime-panic messages** (`emitPanicMessage`, shared by the overflow, bounds/`str`-index, and `!!`
  null-unwrap traps): before `abort()`, the trap block writes `GG runtime error: <what> at <file:line>`
  to **stderr** via `fputs` + the portable `@gg_stderr()` accessor (see the cross-platform runtime note).
  The message is a compile-time-constant private `[N x i8]`
  (no runtime formatting). The line comes from `currentStmtLine_` — tracked **unconditionally** by
  `dbgStmtLine` (called at the top of `genStmt`), independent of `--debug`; the **filename** only
  appears when `--debug` is on (that's the only time `dbgSourceFile_` is set), else the message reads
  `at line N`. The bounds/`str` and `!!` traps always carry a message (they're not flag-gated); the
  overflow message only exists when `--overflow-checks` emits the trap, so **default IR is unchanged**.
- **Cost / rationale**: the overflow bit is free from the ALU; the added cost is a well-predicted
  not-taken branch (+ lost auto-vectorization at higher `-O`). At the default `-O0` the overhead is
  small. Compile-time-known-safe conversions (our untyped-literal adoption) never emit a check.
- **clang `-O` passthrough**: `compile.ps1 -Opt <0|1|2|3|s|z>` adds `-O<level>` to the clang step
  (default `0` — GG previously always compiled unoptimized). Independent of overflow checks.
- Tests: `tests/cast_tests.cpp` (`[overflow]`), `e2e/overflow_checks_test.gg` (run with `-OverflowChecks`).

### Target platform — configurable triple + cross-platform runtime
GG can target **Windows** and **Linux** (both via LLVM). Two things are platform-specific and both are
now parameterized by the target:
- **Target triple**: `CompilerOptions::targetTriple` (defaults to the **host** via `#ifdef _WIN32` —
  `x86_64-w64-windows-gnu` on Windows, `x86_64-pc-linux-gnu` elsewhere; override with `--target=<triple>`).
  Threaded to `IRModule::targetTriple`, emitted by `IRPrinter`. `CodeGen::targetWindows_` = the triple
  contains `"windows"`.
- **stdout/stderr access** (the only libc divergence): centralized behind compiler-emitted helpers
  `@gg_stdout()` / `@gg_stderr()` (`emitStdioHelpers`, called at the end of `generate()`). Windows/UCRT
  bodies call `__acrt_iob_func(1|2)`; glibc/musl bodies `load` the external `@stdout`/`@stderr` FILE*
  globals. Emitted only when referenced — the stdlib declares them `extern gg_stdout/gg_stderr` (in
  `crt/cio.gg`) and `emitStdioHelpers` **replaces that `declare` with the `define`** (LLVM forbids
  both), while the panic path (`emitPanicMessage`) forces `@gg_stderr` via `panicUsesStderr_` even with
  no stdlib import. So `stdlib/IO.gg` (`print` → `fputs(s.data, gg_stdout())`) and `cio.gg` are
  **platform-agnostic** — the one switch lives in `emitStdioHelpers`. Everything else (malloc/free/
  abort/puts/putchar/fputs/memcpy/…, the refcount runtime, all check intrinsics) is standard C/LLVM.
  (The `_WIN32` guard in `ImportResolver` is host-side — case-insensitive import dedup — unrelated to
  the target.) Tests: `tests/cast_tests.cpp` (`[target]`). **Only the Windows path is runtime-tested
  here** (no Linux toolchain in-repo); the Linux IR is verified structurally.
- **Driver scripts**: `compile.ps1` (Windows, drives the mysys2 clang64) and `compile.sh` (Linux,
  `clang` from `PATH` or `$CLANG`). Both run the same pipeline (GG `--unsafe-ptr` → `build/<stem>.ll`
  → clang → `build/<stem>[.exe]`) and take the same options (`--show-ir`/`-ShowIR`, `--run`/`-Run`,
  `--debug`/`-DebugInfo`, `-O<level>`/`-Opt`, `--overflow-checks`/`-OverflowChecks`, plus `--target=`).
  **`-o <path>` (gcc-style, `compile.sh` also accepts `--output=<path>`)** overrides the final
  executable's path/name (default `build/<stem>[.exe]`), creating parent directories as needed —
  this lives in the driver script, not GG itself: GG only ever emits `build/<stem>.ll`/`.ast` (no
  output-path flag of its own), and it's clang's `-o` (the actual linker invocation) that the driver
  forwards the chosen path to.

### Local type inference — `var` (const by default)
`var name = expr;` deduces a local's type from its initializer. **Const by default** (single-
assignment, exactly like a typed local); `mut var` **or** `var mut` opts into mutability. An
initializer is **required** — there is nothing to infer from otherwise. `var` is **locals-only**
(not params, fields, or return types — those stay explicit). The whole feature reuses the existing
declaration machinery: the deduced type flows back as an ordinary type token, so semantics and
codegen gain **no new type-kind and no new branch** beyond the inference site.
- **Lexer**: `var` was already a keyword (`TokenType::VAR`), previously unused. No lexer change.
- **Parser** (`parseExpression`): after the leading `static`/`mut` modifier loop, `match(VAR)` →
  read `name`, require `=`, parse the initializer, and emit a `VarDeclExpr` whose `typeName` is the
  **`var` sentinel token** (`{ VAR, "var" }`). `var mut` is accepted alongside `mut var`.
- **Semantic** (`analyzeVarDecl`, `var` branch): analyze the initializer with **no expected type**
  (`analyzeExpr`, not `analyzeWithExpected`); the inferred type **is** its type. Integer literals →
  `i32`, decimals → `f64` (the literal-default rule). Rejects: no initializer, `void`, bare `null`
  (needs an explicit nullable annotation), a lambda literal (unspellable — pass it to a `Call`-bound
  generic), and an inferred raw `ptr`/`ptr<T>` unless `--unsafe-ptr`. (A string literal infers `str`,
  which is safe/ungated — see the `str` invariant; only a genuine `ptr` initializer is gated.)
  `static var` still enforces the scalar-primitive + constant-initializer static-local rules against
  the inferred type. The synthesized type token is recorded in `SemanticResult::inferredVarType`
  (node → `Token`, via `synthTypeToken(Type,line)` in `Type.cpp`) — the inverse of the type-token
  resolvers, round-tripping through `typeFromToken`/`decodeSynthesizedType` to the same Type.
- **Codegen** (`genVarDecl`/`genStaticLocal`): `varDeclTypeToken(v)` swaps the `var` sentinel for the
  recorded synthesized token at the top, so **every existing branch** (primitive / object / owning
  `Class&` / borrow / enum / nullable / `ptr<T>`) resolves the deduced type and emits **byte-
  identical IR** to the explicit form (`var r = new Point()` co-owns + releases at scope exit exactly
  like `Point& r = …`). `Token` has const members ⇒ not assignable ⇒ the side table is populated with
  `emplace`, never `map[k]=`.
- **Inferring from a value-returning (sret) method call** (`var e = v.empty()` where `fn empty() ->
  Vec e` returns its own class by value) works — but relied on a fix to `resolveTypeToken`: a bare
  class-name **return type** is resolved inside `buildClassInfo` *before* the owning class is in
  `classRegistry`, so it must also consult the `declaredClassNames_`/`declaredEnumNames_` name pre-pass
  sets, else the method's stored `returnType` was `Error` and the `var` binding silently wasn't
  declared. The explicit-typed path (`Vec e = v.empty()`) never hit this (it uses the declared type);
  only `var` inference reads the method's return type directly.
- **v1 limitation**: capturing a `var`-typed local inside a lambda is unsupported (the parser records
  the capture's type at parse time, before inference runs). Rare; a clear failure, not a miscompile.
- Tests: `tests/var_infer_tests.cpp` (`[var]`), `e2e/var_infer_test.gg`, `e2e/var_infer_sret_test.gg`.

### Compile-time reflection — `@`-sigil builtins + `inline for` (Phases 1 & 2)
Zig-style compile-time reflection with **zero runtime cost** — no runtime type tags, no vtables, no
CTFE VM. `sizeof(T)` was the pre-existing member of this family; the rest are `@`-prefixed builtins
(the `@` sigil keeps them out of the identifier namespace and marks them as compiler intrinsics).
- **Surface (fields)**: `inline for (f in @fields(T)) { … }` (compile-time unroll; `f.name` is the
  field-name string, `@field(v, f.name)` is the field as an lvalue — read or assignment target);
  scalar queries `@fieldCount(T)`→u64, `@hasField(T,"x")`→bool, `@offsetOf(T,"x")`→u64 (byte offset).
- **Surface (enums, Phase 2)**: `inline for (v in @variants(E)) { … }` — one body copy per variant;
  bare `v` lowers to the variant singleton `E::VARIANT` (a real value — `v.method()`/`v.field` work,
  `==` compares identity), `v.name` to the variant-name string; `@variantCount(E)`→u64.
- **Surface (type-level)**: `@typeName(T)`→string; layout `@alignOf(T)`→u64; predicates
  `@isInteger/@isFloat/@isClass/@isEnum/@isPrimitive(T)`→bool and `@implements(T, Trait)`→bool
  (user **and** built-in operator traits); `@compileError("msg")` aborts compilation.
- **AST (transient)**: `InlineForStmt{keyword,loopVar,targetType,bodyTokens,overVariants}` (body
  captured as a raw **token slice**, not parsed; `overVariants` picks fields vs enum variants) and
  `ReflectExpr{at,kind,typeArgs,valueArgs}` (one node, `ReflectKind` enum). Lexer: `@`→`AT`, plus
  `inline`/`in` keywords.
- **Key design — expansion in a post-monomorphization PARSER pass** (`Parser::expandReflection`, run
  at the end of `runMonomorphization`, so it sees every class/enum incl. monomorphized ones).
  Generics are monomorphized *before* semantics, so the parser has type names but not field/variant
  lists — hence reflection can't expand *during* monomorphization like generics do, and a
  "double-iterate the body in semantic+codegen" scheme would break GG's one-AST-node→one-resolution
  invariant (the node-address-keyed side tables). Instead: `inline for` bodies are captured as
  tokens; the pass builds a `ReflectRegistry` (`className → ordered field names`, `enumName → ordered
  variant names`) from the parsed `ClassDeclStmt`/`EnumDeclStmt`s, then **re-parses the body once per
  member** (`substituteInlineForBody`: `v.name`→a `STRING` literal; for fields `@field(obj,v.name)`→
  `(obj).<field>`, for variants bare `v`→`Enum::variant`; nested `inline for` handled by leaving
  other-binding references verbatim + a fixpoint recurse). Everything lowers to **ordinary AST**
  (member accesses, `Enum::V`, literals) → **semantic and codegen need almost no reflection logic**,
  only: scalar-query arms (`analyzeReflect`/`genReflect`) that fold each `@`-query to a constant like
  `sizeof`, plus trivial guard arms for the transient nodes. `@compileError` errors in `analyzeReflect`.
- **`runMonomorphization` drains reflection-triggered generic work too** (fixed bug): `expandReflection`
  re-parses a substituted `inline for` body through the ordinary statement/call-parsing path — so a
  call inside a reflected body that targets a GENERIC function (not just an ordinary one) can enqueue
  a brand-new instantiation onto `gen_->worklist`. Since `expandReflection` used to run strictly
  *after* the main worklist-drain loop, with nothing draining it afterward, that queued instantiation
  was orphaned: the call site referenced a mangled name that was never actually monomorphized/emitted,
  and semantic analysis reported "undeclared function". Found via a reflective `printObj<T>` (`inline
  for (f in @fields(T)) { print(f.name); … }`) whose body calls a variadic-pack `print<...Ts>` for the
  first time. Fixed by wrapping drain-worklist → `synthesizeTupleClasses` → `expandReflection` in an
  outer fixpoint loop that repeats whenever a reflection pass leaves `gen_->worklist` non-empty.
  `synthesizeTupleClasses` needed a dedup guard (`tupleClassesSynthesized_`) to stay idempotent across
  repeated calls (it previously unconditionally re-emitted every tuple class on every call — harmless
  when called once, a duplicate-class error if called again). The bounded-generic-body-check loop
  (`checkGenericBodies` surfacing) was moved to run once, after the whole fixpoint, so it also covers
  templates whose first instantiation only happened via a reflection-triggered call. Tests:
  `tests/reflect_tests.cpp` (`[reflect][genoverload]`), regression case using two `take<...Ts>`
  overloads called from inside an `inline for` (mirrors IO2.gg's two-`print<...Ts>` shape).
- **Scalar folds (codegen)**: `genReflect` computes each query's constant — `@typeName` a `str` view
  (`{ptr,i64}`) over a private NUL-terminated constant; counts/offsets/alignment from
  `cgClasses_`/`enumRegistry_`/`dbgSizeAlign` (the same
  natural-alignment layout used by DWARF); predicates from the base type name vs `cgClasses_`/
  `cgEnumNames_`/`typeKindFromName`; `@implements` from `implementedTraits_` (surfaced onto
  `SemanticResult` — trait info is otherwise private to semantics). No `comptime if` yet, so a
  predicate is just an `i1` constant usable at runtime (e.g. as an `if` condition).
- **Rulings**: `break`/`continue` directly in an `inline for` (brace-depth 1) is an error; `@field`
  as an lvalue reuses `MemberAssignExpr` (so `mut`-field gating applies for free); an `inline for`
  over an abstract `TypeParam` in `checkGenericBodies` is skipped (validated at each concrete
  instantiation instead); `@field`/`@fields`/`@variants` outside their valid positions are parse
  errors; `@offsetOf` on a missing field and `@variantCount`/`@fieldCount` on the wrong type kind are
  semantic errors. **Enum names are type names to the monomorphizer** — `ImportResolver::collectClassNames`
  collects `enum` names too, so `@variants(T)` works after `T` is substituted with a concrete enum.
- **Phase 3 (deferred)**: `f.type`/indexed `@fieldName`/`@fieldType`, `comptime if` (per-type
  specialization), method reflection (`@methods`/`@hasMethod`).
- Tests: `tests/reflect_tests.cpp` (`[reflect]`), `e2e/reflect_test.gg`.

### Annotations — compile-time metadata (`annotation` + `@Name`)
User-defined compile-time metadata on declarations, read only by reflection. **Zero runtime cost, no
runtime representation** — an annotation emits **no IR** (no struct/ctor); it's collected into a
registry and folded away, riding the same reflection-expansion pipeline.
- **Surface**: `annotation Name { <const fields> }` declares a metadata type (fields limited to
  `str`/primitive/`bool`/`char`/enum — the comptime-constant set; **no methods**). `@Name(args)`
  prefixes a **class / field / method / enum-variant** (args must be compile-time constants). Read
  inside `inline for`: `f.has(Ann)`→bool, `f.get(Ann).field`→the const value; type-level
  `@hasAnnotation(T, Ann)`→bool (type or any member carries it).
- **`@`-disambiguation (the load-bearing rule)**: a leading `@` in **declaration/member position**
  is an annotation **iff the name is Capitalized** (`@Skip`); a lowercase `@name` (`@compileError`,
  `@typeName`) is a reflection builtin and falls through to the statement/expression path. Position
  alone is insufficient because statement-level builtins (`@compileError`) also start a statement —
  case is the tiebreaker. (`parseDeclaration` gates on `std::isupper`; member/variant positions have
  no builtins so any `@` there is an annotation.)
- **AST**: `AnnotationDeclStmt{name, deque<FieldDecl> fields}` (Stmt variant, no-codegen like a
  trait); `AnnotationApp{name, args (Exprs), argTokens}` on `FieldDecl`/`MethodDecl`/`ClassDeclStmt`/
  `EnumVariant` (as `std::deque<AnnotationApp>` — `AnnotationApp` holds a const-member `Token`, so its
  move isn't noexcept → `vector` would copy-on-grow and hit `Token`'s deleted copy-assign; deque
  doesn't relocate). `ReflectKind::HasAnnotation`.
- **Reading — token-splice, no Expr→token reconstruction**: `parseAnnotationPrefixes` captures each
  arg's **raw source tokens** (`AnnotationApp::argTokens`) at parse time. `expandReflection` builds
  the annotation tables into `ReflectRegistry` (annotation types, per-member names, `annotationFields`
  order, per-member arg tokens, per-field-type default placeholder); `substituteInlineForBody` folds
  `v.has(Ann)`→`true`/`false` and `v.get(Ann).field`→the spliced arg tokens.
- **The comptime-if gap (important)**: `inline for` unrolls the body per member and folds `f.get`
  **unconditionally** — an enclosing `if (f.has(Ann))` does NOT prune the dead branch (`comptime if`
  is deferred). So `f.get(Ann).field` on a member that **lacks** `Ann` folds to a **type-appropriate
  default placeholder** (`0`/`""`/`false`) instead of erroring — only ever reached in a false-guarded
  dead branch. Trade-off: an *unguarded* `f.get` on an absent annotation silently defaults rather than
  erroring (documented v1 limitation).
- **Semantic** (`collectAnnotations`, pass 0b'): registers `annotation` types (`annotationRegistry`),
  records per-type annotation names (`typeAnnotations`, surfaced on `SemanticResult` for codegen's
  `@hasAnnotation` fold like `implementedTraits`), and validates each application — **unknown
  annotation**, **duplicate on one target**, **arg-count mismatch**, and **non-constant arg**
  (`isConstantExpr`). `analyzeAnnotationDecl` validates field types are comptime-representable.
- **v1 limits**: no `@Target` restrictions, no annotation inheritance / annotations-on-annotations /
  repeatable, enum-variant args (`Enum::V`) not spliceable, and `f.get` needs `f.has` guarding.
- Tests: `tests/annotation_tests.cpp` (`[annotation]`), `e2e/annotation_test.gg`.

### ptr<T> — typed raw pointer
- `ptr` and `ptr<T>` (e.g. `ptr<i32>`, `ptr<Point&>`) are low-level raw pointers;
  lower to LLVM `ptr`. Intended for stdlib/CRT container internals. **No bounds check.**
- `ptr<void>` is an alias for `ptr` (same `TypeKind::Ptr`); decoded in `decodeSynthesizedType`.
- `buf[i]` on a `ptr<T>`: one-index GEP — `getelementptr <elemIR>, ptr buf, i64 i`.
- `arr[i]` on a fixed-size array: two-index GEP into the alloca + runtime bounds check.
- `[]` is generalized: works on any expression, so `this.data[i]` works when `data`
  is a `ptr<T>` field.
- **Gated behind `--unsafe-ptr`**: the semantic analyser rejects any variable declaration,
  function return type, or parameter of type `ptr`/`ptr<T>` unless `CompilerOptions::allowRawPtr`
  is true. `extern` declarations are always exempt. The gate fires only at these DECLARATION sites
  (var/param/return/field types, plus `destroy`/`addressOf`/`var`-inference) — a bare expression of
  type `Ptr` (e.g. `str.data`) flowing through as a call argument is never gated, since it declares
  nothing.
- **Stdlib internals are exempt automatically, independent of the `--unsafe-ptr` flag**
  (`SemanticAnalyzer::rawPtrAllowedHere` = `allowRawPtr_ || pathUnder(filename, stdlibDir_)`):
  `CompilerOptions::stdlibDir` (mirrors `ModuleSearchConfig::stdlibDir`, wired in `main.cpp`) names
  the compiler's own stdlib root; a declaration whose current `filename` (the per-declaration save/
  restore already done for error attribution in `analyzeFunctionDecl`/`analyzeClassDecl`/
  `analyzeImplDecl`/`analyzeEnumDecl`) resolves under that root may use raw pointers with no flag —
  so `Array<T>`'s `private mut ptr<T> data;` field, `String`'s local `ptr<u8>` scans, and CRT-binding
  files all compile without `--unsafe-ptr`, while a plain user file declaring `ptr<i32>` still needs
  it. Path comparison (`pathUnder`, `SemanticAnalyzer.cpp`) normalizes via `std::filesystem` and
  lowercases on Windows (mirroring `ImportResolver`'s `dedupKey`), and does nothing when
  `stdlibDir_` is empty (e.g. every unit test, which never sets it and instead defaults
  `allowRawPtr = true`) — so this is purely additive for real compiler invocations.
  **Load-bearing fix for generics**: a monomorphized class (`Array$i32`) must attribute back to
  wherever `class Array<T>` was actually written, not to whichever user file triggered `Array<i32>`
  — `runMonomorphization` already swapped `filename` to `GenericTemplate::sourceFile` during a
  template's re-parse for the free-function `private`-warning feature, but `tryCaptureClassTemplate`
  and `captureMethodTemplate` never populated `sourceFile` on the `GenericTemplate`s they build (a
  latent gap — the existing DWARF-attribution note already called this "usually correct" and
  deferred it). Both now stamp `sourceFile = filename` at capture time, so this exemption (and DWARF
  file attribution) is accurate for monomorphized classes/methods too, not just free functions.
- `compile.ps1` still always passes `--unsafe-ptr` (unchanged) — the stdlib exemption above only
  matters when invoking `GG.exe` directly without that flag. Test helpers default to
  `allowRawPtr = true` and never set `stdlibDir`, so the exemption is inert in the test suite.

### `addressOf(local)` — raw address of a local's storage slot (FFI/C-binding primitive)
A `sizeof`/`destroy`-style intrinsic (`AddressOfExpr{keyword, place}`, same shape as `DestroyExpr`)
that returns the raw address of a **local variable's or parameter's own storage slot** as a typed
`ptr<T>`. Motivation: binding to a C API without pointers means no way to get an out-parameter
address — `T*` (primitive borrow) only produces a GG *reference*, not a raw `ptr` an `extern` can
take directly. Gated behind `--unsafe-ptr` like every other raw-pointer construct.
- **v1 scope — locals only**: the operand must be a **bare identifier** naming an in-scope local or
  parameter (`Symbol::Kind::Variable`) — no fields, array/`ptr<T>` elements, or `this` (unlike
  `isLvalueExpr`'s broader "addressable" set used elsewhere for `T*` borrows). Codegen doesn't yet
  extend the address-of-a-place logic to those shapes; a later pass could.
- **The element type mirrors what the storage slot actually holds** (`analyzeAddressOf`,
  `SemanticAnalyzer_Exprs.cpp`, right after `analyzeDestroy`) — every GG local is alloca-backed, and
  `addressOf` never dereferences (unlike `genBorrowSource`, which follows an existing borrow through
  to its referent):
  - primitive local (`i32 x`) → `ptr<i32>` — address of the scalar's own storage.
  - value-object local (`Point p`) → `ptr<Point>` — address of the struct's own storage (an object's
    alloca already holds its bytes directly, same as any other local).
  - **owning-reference local** (`Point& r`) → `ptr<Point&>` (the pre-existing `"ptr<Class.ref>"`
    synthesized form) — the address of the **slot holding the pointer value**, i.e. one level of
    indirection *above* the object itself (a pointer-to-pointer). This is deliberate, not a
    primitive/object at wraparound — it's what a C API that writes a pointer back to you (an
    allocator out-param) needs; dereferencing the slot once (`slot[0]`) yields the reference back.
  - **`ptr`/`ptr<T>`-typed local** → `ptr<ptr>` — same "one level of indirection above the stored
    value" rule as the reference case, for a C API whose out-param is itself a pointer (e.g.
    `strtoll`'s `char **endptr`). The inner element always collapses to a generic `ptr` regardless
    of the original `ptr<T>`'s `T` — `Type::elementKind` is a single flat field, not a nested
    `Type`, so `ptr<ptr<Point>>` can't be represented distinctly from `ptr<ptr<i32>>` — but this
    loses nothing at the IR level (every pointer is opaque `ptr` in LLVM) and matches the existing
    `ptr<T>`/`ptr` free-interconversion rule; an explicit cast re-specializes the inner type at the
    read site if needed (`slot[0] as ptr<Foo>`).
  - Rejected (v1, clean errors): borrow-typed locals (`Point*`/`i32*` — a borrow's slot already
    holds just a `ptr`, and doubling it needs no distinct spelling GG has), nullable locals, and
    enums (no synthesized-type support for a `ptr<Enum>` element yet).
- **Codegen** (`genAddressOf`, right after `genDestroy`) is a two-line reuse of the existing
  `resolveAssignTarget(name, ptr, type)` helper — it already returns a variable's alloca (or an
  implicit-`this` field GEP, unreached here since semantics restricts to locals) for ANY declared
  type, primitive or not, which is exactly the raw address wanted; no new codegen machinery.
- **Incidental fixes made alongside** (both real, pre-existing gaps `addressOf` exposed, not new
  restrictions):
  1. `emitEquality` (`CodeGen_Exprs.cpp`) had no case for comparing two `ptr`/`ptr<T>` values —
     `==`/`!=` fell through to the generic numeric-widening path (`commonArithmeticType`), which
     doesn't recognize `Ptr`/`TypedPtr` and emitted a malformed `icmp ne i32` on pointer operands
     (caught only by clang's IR verifier, since GG doesn't self-verify). Fixed by extending the
     existing reference/enum address-identity branch to also cover `Ptr`/`TypedPtr` on either side —
     `a == b` on two raw pointers is now a plain `icmp eq/ne ptr`, which is exactly what "did I get
     the same address back" (the most natural thing to do with `addressOf`'s result) needs.
  2. Explicit `as` casting (`analyzeCast`) has its own hand-rolled rule set — separate from
     `canImplicitlyCast` — and never gained a `ptr ↔ ptr<T>` case even though the *implicit* path
     (`canImplicitlyCast`, var-decl/assignment/call-arg binding) has freely allowed it since `ptr<T>`
     shipped. So `someTypedPtr as ptr` is a clean "cannot cast" error today, while
     `ptr p = someTypedPtr;` works. Left as-is (not fixed) — it's an existing, narrow, easily-worked-
     around gap (use a binding instead of `as`), and out of scope for this feature; noted here as a
     candidate follow-up.
- Tests: `tests/address_of_tests.cpp` (`[addressof]`), `e2e/address_of_test.gg`.

### The `Clone` trait — a user deep-copy hook (Array<T> Phase 2)
A built-in trait `Clone { fn clone(Self& src) mut; }` (a copy constructor): `this` = destination,
`src` = source; the method deep-copies `src` into `this`. It unblocks storing **buffer-owning value
objects** (`String`, a nested `Array`) by value in an `Array<T>` (Phase 1 rejected them — memberwise
clone is shallow over a raw `ptr` → double-free).
- **Built-in `obj.clone()` method** — a zero-arg call on any class receiver (value or reference) that
  returns a **fresh value-object copy**. Semantic (`analyzeMethodCall`) intercepts `clone()` with no
  args (only when the class has no user-declared zero-arg `clone` — a `Clone`-trait `clone(Self&)`
  takes one arg, so no collision), returns `Object{class}`, and records the node in
  `SemanticResult::builtinCloneCalls`. Codegen materializes a value-object temp (or, for a var
  initializer, writes in place via `emitSlotCall`) and emits `@Class_clone(dest, src)` — the **same
  helper** as every other clone site, so it transparently uses the user's `impl Clone` when present
  else the generated memberwise clone. This is the explicit, user-callable form of the copy that
  value-object assignment does implicitly; `?.clone()` falls through to normal resolution.
- **Key mechanism — transparent replacement, zero call-site changes**: a `clone` impl method mangles
  to `@Class_clone` — the *exact* symbol the generated memberwise clone uses and every clone site
  (value copy, sret slot, `Array` element write) already calls. So a `Clone` impl **replaces** the
  generated clone, and recursion composes automatically (memberwise clone already calls `@Field_clone`
  for object fields → the field's own impl when it has one). `Array<Array<Point>>` clones correctly:
  `@Array$Array$Point_clone` → `@Array$Point_clone` → `@Point_clone`.
- **C1 recognition** (`SemanticAnalyzer.cpp`): `Clone` added to `isBuiltinTrait` builtins + the
  `collectImpls` `builtinMethod` map (`{"Clone","clone"}`). Redeclaring `trait Clone` is an error
  ("built-in").
- **C2 suppress the generated clone** (`CodeGen.cpp` clone-drain loop): skip `genCloneFunction(cn)`
  when `implementedTraits_[cn]` contains `Clone` — the impl's method already emits `@Class_clone` with
  the identical `(dest, src)` ABI. `clonesNeeded_.insert` still fires; it resolves to the user's def.
- **C3 contract (stdlib follows, not a compiler step)**: `clone` frees/releases `this`'s old state
  first — a no-op on a zeroed `this` (`free(null)`/`gg_release(null,…)`), so one body serves both
  copy-**construct** (zeroed dest: Phase-1 element write, copy-init) and copy-**assign** (live dest).
  Bonus: a general value copy of a `Clone` type now deep-copies (`String a = b;` → independent buffers),
  fixing the pre-existing shallow-`ptr` footgun.
- **C4 guard relaxation** (`analyzeIndexAssign`): the owning-`ptr` element guard (Phase 1) is relaxed to
  reject **unless** the element class impls `Clone` (`implementedTraits[...].count("Clone")`).
- **C5 stdlib impls**: `impl Clone for String` (fields made `private mut`; frees old buffer, `malloc`s
  fresh, `memcpy`s `src`) and `impl<T> Clone for Array<T>` (a **generic impl** — monomorphizes with the
  class; destroys old elements + frees old buffer, then `malloc`s and element-writes each `src` element,
  routing through `T`'s clone = recursive).
- **C6 copy guard for plain value copies** (`rejectUncloneablePtrOwner` + `memberwiseCopyAliasesRawPtr`,
  `SemanticAnalyzer_Exprs.cpp`): a value object whose **generated memberwise clone would shallow-copy a
  raw pointer** cannot be copied by value — the shallow copy aliases the buffer (leak of the old + double
  free of the new, previously a **silent heap-corruption crash**). Now a clean compile error suggesting
  `impl Clone` or a reference binding. Fires at the two whole-object copy sites: **assignment**
  (`analyzeAssign`, Object LHS — always clones, guarded unconditionally) and **copy-initialization**
  (`analyzeVarDecl`, Object — only for a genuine copy SOURCE: an identifier / field / element / `this` /
  `new` result; a direct construct `Point p = Point(...)` or a return-slot call builds in place and is
  NOT flagged). The predicate is **Clone-aware and recursive**: it returns false (safe) at any class that
  impls `Clone` (copying routes through its deep impl), so a class embedding a `Clone` field (`Wrap {
  Bytes b; }` where `Bytes` impls Clone but `Wrap` doesn't) is correctly allowed — its memberwise clone
  recurses through `@Bytes_clone`. (Distinct from the element guard's `classOwnsRawPtr`, which does not
  stop at Clone fields; only the copy guard uses the Clone-aware variant.) **Refinement (for-in
  iterators)**: the raw-ptr-field danger is gated on the class having a **user destructor** — the
  double-free requires a dtor that frees the buffer, and a generated dtor (from reference fields) never
  frees a raw ptr, so a raw-ptr *view* with no destructor (e.g. an iterator holding a non-owning buffer
  pointer) is safe to shallow-copy and is **not** flagged. So a stack-value iterator returns/copies
  freely while `String`/`Array` (raw ptr **+** a user/`Clone` path) stay protected.
- Tests: `tests/clone_tests.cpp` (`[clone]`), `e2e/array_clone_test.gg` (`Array<String>` +
  `Array<Array<Point>>`, independent-buffer + recursion checks, exit 0).

### Owned value-object elements in a `ptr<T>` buffer + `destroy(place)` (Array<T> Phase 1)
A `ptr<T>` buffer can hold **value objects by value** — deep-cloned in, destroyed exactly once out —
which is what makes stdlib `Array<Point>` work (primitives-only before). No `comptime if`: codegen
branches on the concrete element type after monomorphization; primitives are unchanged.
- **Object element WRITE** (`genIndexAssign`, `CodeGen_Exprs.cpp`): for `elementType.kind == Object`,
  a **bare constructor-call RHS** (`data[i] = Point(2,3)`, guarded on `cgClasses_.count(callee)` —
  the same test `genCall`'s own bare-ctor-rvalue branch uses, to exclude the `Call`-trait `obj(args)`
  sugar) **constructs directly into the slot** — no temp, no clone — the same "result location"
  treatment a local `Point p(2,3);` already gets in `genVarDecl`. Order matters: the constructor's
  **arguments are evaluated first** (`buildArgString`, while the slot still holds its old bytes — so
  `data[i] = Point(data[i].x, data[i].y + 1)` reads the old fields, not zeroed garbage), **then** the
  slot is zero-initialized, **then** the ctor is called (or nothing at all, for a ctor-less class —
  the zero-init alone is the default-constructed value). Any other RHS shape falls back to the
  original path: `store %C zeroinitializer, ptr <slot>` then `@C_clone(<slot>, <src>)` (register
  `clonesNeeded_`) — the zero-init makes clone's "release dest's old ref" a null no-op on the **raw**
  slot, turning copy-assignment into a safe copy-**construct**. Primitive elements keep the scalar
  store. (Full self-aliasing through an address/receiver argument — `data[i] = data[i]` or
  `data[i] = data[i].scaled(2)` — is a known, pre-existing limitation of the clone fallback, shared
  with `genMemberAssign`'s field-clone branch; not fixed by the direct-construct fast path.)
- **Object element READ** (`genIndex`): for an object element, return the `genElementAddress` GEP
  (objects carry their address), **not** an `emitLoad` — so it borrows/clones correctly and
  `Index::get(i) -> T*` works.
- **`destroy(place)`** — a new keyword/intrinsic (`DESTROY`, `DestroyExpr`, mirroring `SizeofExpr` but
  taking a parsed expression), **gated on `--unsafe-ptr`** (`analyzeDestroy`). Codegen (`genDestroy`)
  evaluates the operand then emits `@C_dtor(addr)` **iff** `cgClasses_[C].needsDtor`, else nothing —
  so `destroy(data[i])` on a primitive/no-dtor element is a no-op. Container teardown (`~Array`,
  `clear`, `pop`, `set`) loops `destroy(...)` since explicit dtor calls are otherwise forbidden.
  **Double-free guard**: `destroy` on a bare **local object variable** is rejected (it is already
  auto-destroyed at scope exit) — `destroy` is for `ptr<T>` elements / fields the compiler does not
  auto-destroy. (A value-object *field* destroy is still caveat-emptor under `--unsafe-ptr`.)
- **Temp-materialization**: a primitive rvalue may bind to a primitive-borrow (`T*`) **argument** — a
  hidden slot holds the value and its address is passed (`lowerArgOperand`; `buildArgString`/`emitOne`
  and `genTraitMethodCall`). So `a.push(5)` / `a[i] = 999` work. A `mut T*` still rejects a temporary
  (write-through would be lost).
- **Owning-`ptr` guard** (`analyzeIndexAssign` → `classOwnsRawPtr`): a value object that transitively
  owns a raw `ptr`/`ptr<T>` field (String, nested `Array`) is **rejected** as a `ptr<T>` element unless
  it impls `Clone` — memberwise clone is shallow and would double-free. (See the `Clone` trait above.)
- stdlib `Array.gg`'s current method set — `pushBack(T* value)` / `emplaceBack<...Ts>(Ts... args)`
  (append), `push(i32 pos, T* value)` / `emplace<...Ts>(i32 pos, Ts... args)` (insert at `pos`,
  shifting `[pos, count)` right), `popBack() -> T out` / `pop(i32 pos) -> T out` (remove-and-return,
  shifting the tail left for the positional form), `delete(i32 pos)` (remove-and-destroy, no
  return), `contains(T* value) -> bool`, `pushAll<U: Iterable>(U collection)` (iterate `collection`
  via `for (T* elem : collection) { pushBack(elem); }` — see the `for`-in invariant; requires an
  explicit `arr.pushAll<OtherType>(other)` type argument today, since argument-driven inference for
  a generic method whose OWNER is itself a generic class can't see the owner's `methodTemplates`
  entry until the owner itself is monomorphized, which happens after the call site's own parse —
  see the generic-method-inference invariant above), plus `set(i32 i, T* value)` (via `impl Index`).
  `set(T&)` (`impl Index for Array<T>`), `push`/`emplace`/`pop`/`delete` all use the
  **clone-forward-then-destroy** shift idiom (`data[i] = data[i∓1]; destroy(data[i∓1]);`, one pair
  per shifted slot) rather than a raw `memmove` — 2× the memory traffic of a true move, but correct
  for arbitrary (including resource-owning) `T` with zero new codegen, reusing exactly the
  clone/destroy primitives `popBack`/`set` already established; a raw-pointer-level move would be
  more efficient but was never built since GG has no move semantics separate from clone. `clear`/
  `~Array` destroy loops. `Array<Point>` (a plain value class) and `Array<i32>` (primitive `T`, only
  reachable once the primitive-cast-call parser fix above landed — `Array<i32>::emplaceBack`
  literally never compiled before that) both work; `Array<String>`/`Array<Array<Point>>` work via
  the Phase-2 `Clone` impls. **`e2e/array_object_test.gg` and `e2e/array_clone_test.gg` are stale**
  against this current method set (written against an older `push(value)`/`pop()`-with-no-position
  API and no longer compile) — a known, pre-existing gap, not touched here.
  Tests: `tests/array_object_tests.cpp` (`[array-object]`).

### `str` — compile-time string-literal type (Phase 1)
`str` is the type of a **string literal** — an immutable view over static (rodata) bytes, lowered to
a `{ ptr, i64 }` value (`TypeKind::Str`; IR `{ ptr, i64 }`, modeled on the nullable-primitive inline
value type). It is a **safe** type (no `--unsafe-ptr` gate), trivially copyable, with no ownership /
refcount. `String` (the heap class) is unchanged and distinct.
- **Representation**: field 0 `data` = a **NUL-terminated** pointer into static bytes (so `.data` is a
  valid C-string); field 1 `len` = the **byte** length (excludes the NUL; codepoint counting stays a
  `String` concern). For a literal both are compile-time-known.
- **Accessors** (`analyzeMemberAccess`/`genMemberAccess`): `s.data` → `ptr` (`extractvalue …, 0`),
  `s.len` → `u64` (`extractvalue …, 1`). Any other member is an error.
- **Decay** `str → ptr` is `Silent` (`canImplicitlyCast`), emitted as `extractvalue …, 0` in
  `emitCast`; it is the mechanism that lets a literal flow to a `ptr`/`extern` parameter (`puts("hi")`)
  and threads through `buildArgString`'s per-arg cast. The **reverse `ptr → str` is forbidden** (a raw
  pointer has no known length).
- **Lexer/parser**: `str` keyword → `TokenType::STR` (in `isTypeName`/`isTypeKeyword`). `str*` (a
  **primitive borrow** — an lvalue reference to a `str` value, exactly like `i32*`/`f64*`) is allowed:
  `str` is an ordinary `{ptr,i64}` value with the same addressable-storage story as any other
  primitive, so there was never a codegen or safety reason to reject it — the original "str is a view,
  not a borrowable lvalue" rejection was a leftover scope restriction from before this generic-code
  use case (`Pair<T,V>`'s `Pair(T* t, V* v)` ctor instantiated at `T=str`) was exercised. Reads
  auto-deref, `mut str*` supports write-through, and a temporary/literal argument temp-materializes
  into a hidden slot — all via the ordinary primitive-borrow machinery (see the `T*` invariant), no
  `str`-specific codegen. **Type plumbing**: `typeFromToken`/`typeName`/`typeKindFromName`/
  `primitiveTokenType` handle `Str`; `mangleType` falls through to `"str"`; `synthTypeToken(Str)` →
  the `STR` keyword token (so `var s = "x"` round-trips). `analyzeLiteral(STRING)` returns `Str`
  (was `Ptr`), so **string literals are now safe** and `var s = "lit"` is ungated.
- **`String(str)`**: the stdlib constructor takes a `str` and uses `literal.len` (no `strlen` scan) +
  `literal.data`. All CRT bindings stay `ptr` and receive `String`'s `ptr` buffer, so `crt/*.gg` is
  unaffected.
- **Debug info**: `dbgSizeAlign(Str)` = `{16, 8}`; `dbgTypeOf` emits a precise
  `DW_TAG_structure_type` `str` (128 bits) with two members — `data` (a `ptr`, offset 0) and `len`
  (`u64`, offset 64) — reusing the cached `ptr`/`u64` type nodes (neither refers back to `str`, so no
  cycle). So a debugger shows `{data=…, len=…}`, not an opaque address. (Debug is off by default.)
- **Reflection strings are `str`** (Phase 2): `@typeName(T)` returns a `str` (was a bare `ptr`) — so
  `@typeName(T).len` and `.data` work; `f.name`/`v.name` were already `str` (they lower to string
  literals). `analyzeReflect(TypeName)` → `Str`; `genReflect(TypeName)` materializes the `{ptr,i64}`
  view over its private NUL-terminated constant.
- **Indexing** `s[i]`: the i-th **UTF-8 byte** as a `char` (u8 zero-extended to i32 — a code point).
  **Bounds-checked** — unlike a raw pointer, a `str` carries its length (field 1), so `genIndex`
  extracts `.len` and traps (`abort()`) on an out-of-range index, exactly like a fixed-size array
  (the shared `emitBoundsCheckValue(idx, lenOperand)` helper — an `icmp ult i64` that also catches
  negatives, since they wrap to huge unsigned; `emitBoundsCheck` for arrays now delegates to it with
  a constant size). Built in as a `TypeKind::Str` case in `analyzeIndex` (returns `Char`; the
  index-must-be-integer check already precedes it) and `genIndex` (extract data ptr + len from
  `{ptr,i64}`, bounds-check, `getelementptr i8`, `load i8`, `zext to i32`). Built *in the compiler*
  because `str` is a primitive and cannot carry a user `Index` impl. `s[i] = …` (write-through) is
  **not** supported — `str` is an immutable view over static bytes. Enables the digit-parsing idiom
  `(s[i] as i32) - 48` and byte iteration `while (i < (s.len as i32)) { … s[i] … }`.
- **Deferred, rejected with a clean semantic error (never miscompiled)**: `==`/`!=` on `str`
  (`classifyEquality` guards it before the numeric fall-through), relational ops (numeric check),
  `str + str` (numeric check), and `str?` (`resolveTypeToken` rejects it alongside `ptr?`/`void?`).
  Also deferred: `str` slicing/concat and `s[i] =` write-through. A `str` value-object **field** works
  (POD `{ptr,i64}`, memberwise-copied). Tests: `tests/str_tests.cpp` (`[str]`), `e2e/str_test.gg`.

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

### Object return slot (sret) — call-site lowering
The object-value case of the return alias (see the "Return aliases" invariant). An object alias
is a caller-allocated slot: hidden `ptr` first param, `void` LLVM return, zero-init in place; the
callee never runs the slot's dtor (ownership → caller). Call sites: `genVarDecl` object-init calls
`emitSlotCall(init, ptrName)` (writes straight into the variable's alloca — no clone/ctor); other
positions (`genCall`/`genMethodCall`) materialize a temp via `materializeSlotTemp` + `emitSretCall`.
Works on generics — the alias/slot type may be a type parameter (`fn zero<T>() -> T out`);
generic-class arrow methods ride the verbatim class-body capture + re-parse. **A value-return call
used as a nested sub-expression argument works** (`takeRef(make(1,2))`, `combine(make(1,1),
make(2,2))`, a discarded temp, a method call on the result): `genCall`/`genMethodCall` materialize a
`slottmp.addr` via `materializeSlotTemp` + `emitSretCall` and yield its address, so it borrows into a
`Class&` param like any other value object; the temp is dtor-scoped and released at the full-expression
boundary. (Only the *variable-init* position uses the zero-copy `emitSlotCall` in-place path.)

### Enums (Java-style)
- Declared with `enum Name { VARIANT, ... }` or with a body:
  `enum Planet { EARTH(5.976, 6.378); f64 mass; f64 radius; Planet(f64 m, f64 r){...} f64 getMass(){...} }`.
- Each variant is a global singleton `%Enum` struct; an enum variable is a `ptr`
  to a singleton (`TypeKind::Enum` lowers to LLVM `ptr`).
- Fields are immutable — assignable only via `this.field =` inside the constructor.
  All declared fields must be initialised in the constructor; variant arg counts
  must match the constructor arity.
- Identity equality only: `==`/`!=` lower to `icmp eq/ne ptr`. No other operators.
- Enums cannot be constructed directly (`Enum()`), `new`-ed, or given a destructor.
- Variant mangling: `@Enum$VARIANT` (a global). Methods/ctor mangle like classes:
  `@Enum_method`, ctor `@Enum_Enum(ptr %self, …)`.
- Pre-main init: variant singletons with a constructor are populated by
  `@gg_enum_init()`, registered in `@llvm.global_ctors`. Fieldless enums skip this
  and get a single `i8` padding byte (`%Color = type { i8 }`) so distinct variants
  have distinct addresses.
- Enums reuse `classRegistry`/`ClassInfo` and `cgClasses_`/`CGClassInfo` for
  fields/methods/ctor, plus a parallel `enumRegistry`/`EnumInfo` (semantic) and
  `cgEnumNames_` set (codegen). `EnumDeclStmt::variants`/`methods` are `std::deque`.

### `private` on free functions (file-local, warning-level)
A free function may be declared `fn private name(...)` — public by default. `private` makes it
**file-local**: calling it from a **different source file** emits a **warning** (never an error),
mirroring private fields/methods. Same-file calls are silent.
- **Parser** (`parseFnDeclaration`): an optional `private` after `fn` (before the name) sets
  `FunctionDeclStmt::isPublic=false`; `FunctionDeclStmt::sourceFile` is stamped from the parser's
  canonical `filename`.
- **Semantic**: `FunctionOverload` carries `isPublic` + `sourceFile` (filled in `collectFunctions`).
  `currentFile_` is set to the declaring file at the top of `analyzeFunctionDecl` (save/restored) and
  is **empty inside class/enum/impl bodies**. The free-function call site in `analyzeCall` warns iff
  `!chosen.isPublic && both files known (non-empty) && they differ` — so same-file calls and
  unknown-origin contexts (method bodies) never warn (zero false positives; conservative v1).
- **Codegen**: unaffected — access control is purely a semantic warning (no name mangling / linkage
  change; `private` functions keep their plain `@name`). Tests: `[private]` in `tests/semantic_tests.cpp`.
- **Generic free functions** (`fn private name<...>`/`fn private name<...Ts>`) are now supported too
  — `tryCaptureFunctionTemplate` was name-first (assumed the name sat immediately after `fn`), so
  `private` there used to be a hard "expected '(' after function name" parse error rather than a
  clean rejection or a working feature; `prescanTemplateNames` already tolerated it (it registers
  the name into `funcNames` correctly either way), which masked the gap until an actual call site
  was exercised. Fixed with **zero new semantic-layer code** — reuses the exact mechanism above:
  - `tryCaptureFunctionTemplate` now skips an optional `PRIVATE` token between `fn` and the name,
    and — the key trick — **re-emits** a synthesized `private` token into the captured tokens (not
    just a stored flag) when `!isPublic`, mirroring `captureMethodTemplate`'s existing pattern for
    generic methods (line ~610, though that's for **class-scoped** method privacy — a different,
    orthogonal visibility axis, not file-scoped). Since the monomorphized instantiation is re-parsed
    through the ordinary `parseDeclaration()` → `parseFnDeclaration()` path, the concrete
    `printf$Tuple$i32` ends up an ordinary `FunctionDeclStmt` with `isPublic=false` — indistinguishable
    to semantics from a hand-written private function, so the existing cross-file warning check above
    fires with no modification.
  - `GenericTemplate` gained `isPublic` (recorded at capture time, mostly for documentation/future use
    — the re-emitted token is what actually drives behavior) and `sourceFile` (the template's OWN
    declaring file, captured via `this->filename` during that file's own per-file parse pass). The
    latter is load-bearing: `runMonomorphization`'s single shared `Parser::filename` is otherwise one
    value for the **entire** monomorphization pass (the entry/root file — see the ImportResolver
    "single monoParser" note), so every instantiation's `FunctionDeclStmt::sourceFile` would attribute
    to the root file regardless of which file the template came from. That happens to equal the
    calling file in the common case (calling from the program's entry file), which would silently
    suppress the very warning this feature exists to produce. Fixed by temporarily swapping
    `filename = tmpl.sourceFile` around the instantiation's re-parse (restored immediately after) —
    this also improves DWARF file attribution accuracy for generic instantiations as a side effect
    (previously documented as "usually correct" / an approximation in the debug-info invariant).
  - **Incidental fix required**: the free-function monomorphization loop's self-reference rename
    hardcoded the declaration name at token index 1 (`fn NAME` → idx 1) to distinguish a variadic
    template's *declaration* occurrence (renamed) from its *recursive call* occurrences (left bare, so
    each level re-deduces the shrinking pack). Injecting `private` pushes the name to index 2, so this
    is now computed per-template (`declNameIdx`) rather than hardcoded — a no-op for every
    non-private/non-generic-class template, where it still resolves to 1.
  - Tests: `tests/semantic_tests.cpp` (`[private][genoverload]`) — the same four-scenario shape as the
    non-generic private-function tests (same-file silent, cross-file warns, public-generic cross-file
    silent, used-internally-then-imported warns only at the cross-file site), each with a pack
    template instead of a plain function.

### Field default initializers + constructor field-initialization enforcement
Two coupled features: (1) instance fields may carry a default initializer at their declaration
(`= expr` or `{args}`), and (2) every field a constructor doesn't cover via a default must be
definitely assigned somewhere in that constructor's own body, or it's a compile error. Together
these close the same class of bug the "Object locals" definite-init work fixed for locals — a field
silently left at its zero-initialized (possibly invariant-violating) state — but for class FIELDS.
v1 is self-contained-initializer-only (no `this`/params/other fields in an initializer expression);
see "v1 limits" below for the full boundary.

- **Field default initializers — syntax** (`Parser_Decl.cpp`'s field branch): `[modifiers] Type name
  [= expr | {args}];`. Previously `= expr` only parsed for **static** fields; now any INSTANCE field
  may carry one too. `{args}` is new for fields entirely — mirrors the local var-decl `Type
  name{args}` brace-construction sugar exactly: gated on `classNames.count(memberType.lexeme) > 0`
  (so it fires only for a class-typed field; `consumeType` already synthesized/mangled `memberType`
  and registered it in `classNames`, identically to how a local's type token does for a generic
  instantiation), parses a positional arg list, and lowers to an ordinary `CallExpr{ memberType, args
  }` stored in the existing `FieldDecl::initializer` (previously statics-only). **Enums are
  excluded** from both new forms (they have their own, separate, already-correct model — fields set
  via variant constructor args, immutable thereafter); only the pre-existing static-field `=` form
  is preserved for an enum, unchanged.
- **Semantic** (`analyzeClassDecl`, `SemanticAnalyzer_Stmts.cpp`): each non-static field initializer
  is analyzed with the field's declared type as the expected type (literal adoption, same as a
  static field's), but with `currentClassName` **cleared** for the duration — the exact guard
  `analyzeParamDefaults` already uses for the identical reason: a bare name must not resolve via the
  implicit-instance-field fallback, and `this` must be a clean "used outside of a class method"
  error rather than silently resolving to this class. `ClassInfo::Field` gained `hasInitializer`
  (set in `buildClassInfo` from `fd.initializer != nullptr`). **The class's own static fields are NOT
  blocked**: `Config::DEFAULT` (explicit, qualified) resolves via the type name — independent of
  `currentClassName` — so it works from a field initializer; a **bare** `DEFAULT` still fails
  ("undeclared identifier"), matching `analyzeParamDefaults`' own identical pre-existing asymmetry
  (same guard, same gap) rather than introducing a new inconsistency.
- **Codegen — zero-copy, not clone** (`CodeGen::genFieldInitializer`, `CodeGen_Classes.cpp`, right
  after `genMemberAssign`): a per-class `classFieldInits_` map (className → ordered
  `(fieldName, const Expr*)`, populated in `generate()`'s existing per-class fields prepass) is
  walked in `genMethod` for every constructor, emitting each initializer **before** the constructor's
  own body runs (C++ member-init-list order). Unlike `genMemberAssign`'s Object-field branch (always
  construct-a-temp-then-clone — correct there since an ordinary `this.field = ...` write may be
  overwriting an already-live value), a field's own default runs on freshly zero-initialized storage,
  so it gets the **direct-construct fast path** first (`emitObjectDirectInit` — the same one a
  local's defining assignment uses: a bare constructor-call RHS, which is what `{args}` always
  produces and what `= Ctor(args)` commonly does, constructs straight into the field's GEP address,
  no temp, no clone), falling back to clone only for a non-bare-constructor-call RHS shape. This is
  what makes the flagship case (`Array<List<V>> values{8};` in `HashMap<K,V>`) truly zero-copy — see
  the stdlib note below.
- **A class with field initializers but NO user-written constructor at all is synthesized one**
  (`Parser::parseClassDecl`, right after `parseMemberList` returns): if no method in the parsed set
  has `isConstructor` and at least one non-static field has an initializer, an empty `ClassName() { }`
  `MethodDecl` is pushed into the class's method list — literally as if the author had written it
  themselves (same token/field shape as a hand-parsed constructor). This is not a rare edge case —
  it's the flagship shape (`class HashMap<K,V> { Array<List<V>> values{8}; }`, zero explicit methods
  at all): without a real `MethodDecl` there is nothing for the codegen prologue-injection to attach
  to, and nothing for the deferred-init rule (below) to see either. A class **with** a user ctor is
  unaffected — the user's own ctor already hosts the injected prologue.
  - **Interaction with the "Object locals" no-magic rule**: this synthesized ctor is a REAL
    constructor in `ClassInfo::methods`, so `objectHasAnyCtor` (in `analyzeVarDecl`) sees it exactly
    like a hand-written one — a bare `HashMap<K,V> m;` local is deferred-init (errors), matching the
    established "any constructor at all ⇒ never implicit" rule; only an explicit `HashMap<K,V> m();`
    (an ordinary initializer) constructs it.
- **Constructor field-initialization enforcement** (the actual "must-initialize" check): every
  instance field must be definitely assigned by the time a constructor returns, on every reachable
  path, unless it's exempt (below). Reuses the ENTIRE existing branch-merge machinery for locals —
  the mechanism is a `SymbolTable::ctorFieldInit_` map (field name → assigned) that rides along
  inside the SAME `InitSnapshot` a local's `isInitialized` already uses, under a reserved key prefix
  (`"#field#"`, not a legal identifier prefix) that `captureInitState`/`restoreInitState` fold in and
  extract transparently. Because `analyzeIf`/`analyzeWhile`/`analyzeFor`'s merge loops iterate the
  whole flat snapshot map generically (they don't enumerate known variable names), if/else
  both-arms-assign, one-arm-only, and loop-body-only-assigns-don't-count all fall out **with zero
  changes to any control-flow analyzer** — the same reason the earlier definite-init work for locals
  composed for free.
  - **Seeded** at constructor entry (`analyzeClassDecl`'s per-method loop, right after `enterScope()`)
    from each field's exemption status (below); **marked** true at the two field-write hooks
    (`analyzeMemberAssign`'s `this.field = value` tail, and `analyzeAssign`'s implicit-`this`
    fallback branch) — both call `symbolTable.setFieldInitialized(name)` right before returning,
    gated (for the explicit path) on the write target actually being `this` and `inConstructor`.
  - **Checked at TWO points**, mirroring `checkReturnAliasAssignedAtExit`'s existing two-point shape
    for the identical reason (a function can exit either by falling off the end or via an early
    `return`, and each must be checked where it actually happens): a bare `return;` inside a
    constructor calls the new `checkCtorFieldsInitialized` at that exact point (catches a field
    skipped by an early exit — `if (skip) { return; }` before a field is set); and once after the
    whole body if control can fall off the end (`!alwaysReturns(md.body)`, skipped when the body
    always returns — every path already went through the live per-return check). Error: `field 'x' is
    not initialized in constructor 'C'; assign it, or give it a default value at its declaration`.
  - **Exemptions** (a field starts "already initialized," so the constructor need not touch it) —
    computed once per constructor from `ClassInfo::Field`/`Type`, in order:
    1. **Has a default initializer** (`hasInitializer`, i.e. Phase 1 above) — the obvious case.
    2. **Is NULLABLE** (`Type::isNullable`) — a nullable field's zero-initialized state (`null` / an
       empty tag) is already a meaningful, deliberate value (the field is "absent"), exactly
       analogous to a ctor-less class's zero bytes already being complete. Without this, every
       existing nullable-field class in the test suite (`N&? next;`, `i32? tag;`, left unset in a
       trivial ctor — a completely ordinary, correct pattern) would have needed busywork
       `= null`-equivalent assignments for no safety benefit.
    3. **An embedded value-object field whose class has NO constructor at all** — zero bytes
       genuinely IS that class's complete valid state (mirrors the identical ctor-less-class
       exemption already used for locals in `analyzeVarDecl`'s `objectHasAnyCtor` check, applied
       here to a FIELD's class instead of the field's OWN class).
    A field whose type is Object with a REAL constructor, and is neither defaulted nor nullable,
    is NOT exempt — this is the case the whole feature targets (a `HashMap`-shaped field silently
    skipping `Array<T>`'s own cap/data setup, the exact "worked by luck via `realloc(NULL,…)`" latent
    bug this session found and fixed in the real `stdlib/utility/HashMap.gg`, below).
  - **v1 limits** (clean errors or accepted-as-is, never a miscompile): **intraprocedural only** — a
    field set by a helper method the constructor calls (`this.init();`) is not seen; the constructor
    must assign it directly. **Read-before-init is not checked** — only "assigned by the end"; a
    `field += 1;` compound-assign does not itself count as the defining write (conservative — it
    reads before writing, so it doesn't satisfy "assign it a value" the way a plain `=` does), so a
    ctor relying solely on compound assignment for a field still errors, prompting an explicit `=`
    or a default initializer instead. **Switch/match arms are not merged** for field (or, pre-
    existing, local) definite-assignment at all — only if/while/for participate; a field assigned on
    every switch/match arm is conservatively still "not initialized" after — a known, pre-existing
    gap for locals that this feature inherits rather than introduces or fixes.
- **Real-world fix, found via this feature**: `stdlib/utility/HashMap.gg`'s own constructor —
  `HashMap() { values.reserve(8); }` — called a method on `values` (`Array<List<V>>`) **before**
  `Array`'s own constructor had ever run, silently "working" only because `reserve`'s `realloc(data,
  …)` happens to tolerate a null/zero `data`/`cap` (a latent bug identified earlier in this same
  session, then still present). Phase 2 now catches this at COMPILE TIME (`field 'values' is not
  initialized`) instead of relying on that implementation-defined `realloc(NULL, …)` behavior. Fixed
  to `Array<List<V>> values{};` (a Phase-1 default initializer, zero-copy per the direct-construct
  path above) with an empty constructor body — `Array()`'s own ctor already sets `cap = 8` by
  default, so the explicit `reserve(8)` became unnecessary too.
- Tests: `tests/const_tests.cpp`-adjacent field-initializer/enforcement cases (parse/semantic/codegen
  for both initializer forms, the synthesized-ctor path, the three exemptions, if/else branch-merge,
  early-return, and the direct-construct-vs-clone codegen shape). Migration: ~30 pre-existing unit
  tests across 12 files had an empty or field-incomplete constructor written purely to exercise
  unrelated functionality (escape analysis, generic impls, ref-field layout, …) — each fixed by
  adding the missing assignment/default, not by weakening the check.

### Static members (class-level)
Roadmap: (1) **static fields** ✅ done, (2) **static methods** ✅ done, (3) **static local
variables** (C-style) ✅ done, (4) polish — **const statics** ✅ done (see below); access
enforcement + file-scope linkage still **intentionally deferred** (private access stays a warning;
static fields keep external linkage, a no-op until separate compilation exists).
- **Static fields** — `static T name;` or `static T name = const;` inside a class.
  Shared class-level storage, **not** part of the struct layout. Accessed via the
  scope-resolution operator `ClassName::field` **or** through an instance `obj.field`
  (both resolve to the same global).
- **const-by-default static fields** — a static field is **immutable unless `mut static`**, like
  every other binding (`static i32 MAX = 100;` is a named constant; `mut static i32 count = 0;` is a
  reassignable counter). Enforced purely in semantics via `ClassInfo::StaticField::isMut` (populated
  from `FieldDecl::isMut`; the parser already accepts `mut`/`static` in either order on fields). A
  write to a non-`mut` static is an error at every write form — `Class::field =` (`analyzeMemberAssign`
  type-name branch), `obj.field =` (instance branch), and implicit-`this` `=` / `+=` / `++`/`--`
  (`analyzeAssign` / `analyzeCompoundAssign` / `incDecTargetOk`, via the new `currentStaticField`
  helper). Reads and the declaration initializer are always allowed. **Codegen is unchanged** — const
  is compile-time only; the emitted `@Class$field = global …` is byte-identical (a const static is
  *not* an LLVM `constant`, matching how instance const fields are handled). `Class::field +=` /
  `Class::field++` (compound/increment through `::`) don't parse — a pre-existing parser limit, so
  there's no ungated write path. Static fields still keep **external** linkage (deferred).
- **Static methods** — `static T method(...)` inside a class. Class-level: **no
  implicit `this`** (using `this` in a static body is an error). Called as
  `ClassName::method(args)` **or** through an instance `obj.method(args)` (both lower
  to the same receiver-less call). May read/write static fields. Mangled
  `@ClassName_method(...)` **without** the leading `ptr %self`. Calling an instance
  method via `::` is an error ("not static"); calling a static via an instance is fine.
- `static` keyword (lexer) + `::` (`COLON_COLON`) token. `private static` / `static`
  in either order is accepted in a member list.
- Lowering: one global per static field, `@ClassName$field = global <ir> zeroinitializer`.
  Initializers run before main in `@gg_static_init()`. Parser stores the initializer
  on `FieldDecl::initializer` (a `unique_ptr<Expr>`); `FieldDecl` also has `isStatic`.
- **Static-field initializers adopt the field's declared type** (`analyzeClassDecl`, via
  `analyzeWithExpected(*fd.initializer, fieldType)` — mirrors the local-var path): a bare numeric
  literal takes the field type, so `static i64 X = 9223372036854775807;` types the literal **i64** and
  stores the full 64-bit value — previously it defaulted to `i32`, warned "overflows i32", and
  **truncated the constant** (a real miscompile for wide constants, not just a spurious warning). A
  literal that genuinely doesn't fit the declared type still warns accurately ("does not fit in 'i32'").
- `ClassName::field` / `ClassName::method(...)` parse to the same
  `MemberAccessExpr`/`MemberAssignExpr`/`MethodCallExpr` nodes as `obj.field` / `obj.m()`
  (the left identifier is resolved as a type name in semantics/codegen).
- Static fields live in `ClassInfo::staticFields` (semantic) and
  `CGClassInfo::staticFields` (codegen); they are excluded from the instance field
  list / struct layout and the field index numbering. Enums may **not** declare statics.
- Static methods carry `isStatic` on `MethodDecl` and `ClassInfo::Method`; codegen
  records their names in `CGClassInfo::staticMethods` so `genMethodCall` dispatches to
  `genStaticCall` (no receiver). `SemanticAnalyzer::currentMethodIsStatic` gates `this`.
- **Static locals** — `static T name;` / `static T name = const;` inside a function or
  method body. A single persistent global shared across all calls; initialised once
  before main. Phase 3 supports **scalar primitive** types only (numeric/bool/char);
  the initializer must be a compile-time constant (`SemanticAnalyzer::isConstantExpr`:
  literals and unary/binary/cast of constants). `VarDeclExpr` carries `isStatic`; the
  parser accepts a leading `static` in `parseExpression`. Lowering: one
  `@<prefix>$<name> = internal global <ir> zeroinitializer` (prefix = function name or
  `Class_method`; collisions get a `.N` suffix). The name maps in `allocaMap` directly
  to the global, so reads/writes load/store through it (no alloca). Non-zero
  initializers are queued in `CodeGen::staticLocalInits_` and flushed in
  `@gg_static_init` alongside static-field initializers.
- **`@llvm.global_ctors` is now unified**: both `@gg_enum_init` and `@gg_static_init`
  register through a single `globalCtors_` list emitted by `emitGlobalCtors()` as one
  `[N x { i32, ptr, ptr }]` array (LLVM permits only one such global).

### AST container note
`ClassDeclStmt::fields` and `EnumDeclStmt::fields` are `std::deque<FieldDecl>` (not
`vector`): `FieldDecl` now owns a `unique_ptr<Expr> initializer` and contains `Token`s
(const members), making it move-only and not move-assignable — same rationale as the
`methods` deque.

## Test baseline
**1513 test cases / 2822 assertions** (as of implementing **threads** — the stdlib `Thread` class +
the compiler's "function-pointer part" (concurrency Phase 1, execution axis) — see the "Threads"
invariant above and `docs/concurrency.md`. `Thread` is ordinary GG (`stdlib/thread/Thread.gg`, module
`std.thread`); the compiler adds only two intrinsics (`__gg_heap_closure` = heap-copy a closure via
its clone; `__gg_trampoline` = emit the per-closure C-ABI `@__thread_entry$F` that calls `@F_call`
then releases the closure), the platform-neutral `@gg_thread_create`/`@gg_thread_join` runtime
(Windows CreateThread/WaitForSingleObject — needs `-lkernel32`, added to `compile.ps1`; Linux
pthread), lambda-literal inference for a single-`Call`-bounded generic STATIC method (so
`Thread.create(() -> {})` infers the closure class the same way a generic free function does), and a
**Sendable** boundary check (`isSendableType`/`isSendableClass`) enforced on the closure's captures at
`__gg_heap_closure` — a captured non-atomic owning `Class&`/borrow/`ptr`, or a value object embedding
one, is rejected naming the field + suggesting `Shared<…>`; primitives/`str`/enum/`Shared<T>`/all-
Sendable value objects pass. A separate post-pass (`checkThreadClosures`) transitively confines
STATICS a thread touches to shareable ones too (the ambient-globals hole the capture check can't see;
gated by the Shared-field rule, so an atomic-but-`mut` `static Shared<T>` rebind is still rejected).
User surface: `Thread t = Thread.create(() -> { … }); t.join();`. 25
`tests/thread_tests.cpp` cases (`[thread]`) + `e2e/thread_test.gg` (a thread mutating a captured
`Shared<T>`, joined, live-counter proving exactly-once destruction, 30/30 runs deterministic). The
full end-to-end chain (lambda inference → heap-copy → trampoline → CreateThread → join) was proven
from `build/` before the stdlib surface was wrapped on top. Prior baseline was
**1488 test cases / 2773 assertions** (as of implementing **`Shared<T>` — the atomic, born-shared
ownership handle** (concurrency Phase 1) — see the "`Shared<T>`" invariant above and
`docs/concurrency.md`. This is the compiler-builtin cross-thread lifetime handle: a `bool shared`
flag on `TypeKind::Reference` (mirrors `borrow`), atomic `gg_retain_shared`/`gg_release_shared`
runtime, build-in-place construction `Shared<Class>(args)`, born-shared cast rules (retain-copy;
borrow/value-clone out OK; owning-`Class&` out and anything-in forbidden), the **Shareable gate**
(`isShareableClass` — a `Shared<T>` may only wrap a class with no mutable reference-like field and no
raw ptr/borrow field, enforced at construction), and atomic refcounting wired at every site (var-init,
copy, rebind, `+1` return, scope-exit, temps, and fields: clone/dtor/store/init). 37 new
`tests/shared_tests.cpp` cases (`[shared]`) + `e2e/shared_test.gg`/`shared_field_test.gg`/
`shared_field_edge_test.gg` (behavioral, balanced-refcount via a live-object counter). Built
incrementally across slices, each edge-audited: the type plumbing (tuple-element rejection, nested
`Shared<Shared<T>>` + generic-arg rejection), the codegen (a real bug fixed: `Shared→Class*` borrow /
`Shared→Object` clone were wrongly rejected by an over-strict born-shared guard — only `Shared→` owning
`Class&` is actually unsafe), and the field wiring (const/nullable/user-dtor/nested/embedded
compositions + implicit-`this` ctor store atomicity). **`Thread`, the `Sendable` marker, and the
thread-boundary/statics confinement are NOT yet implemented** (task 17) — this is the handle type and
its Shareable safety gate, single-threaded-correct and atomic-ready. Full `GG_tests` + all 78 e2e
files frontend-clean, zero regressions. Prior baseline was
**1451 test cases / 2696 assertions** (as of adding trait-name parameter sugar — `fn f(Trait a)` /
`fn f(Trait& a)` as sugar for a bounded generic — see the "Trait-name parameter sugar" invariant
above. 16 `tests/trait_param_sugar_tests.cpp` cases (`[traitparam]`): the desugar records a real
`GenericBoundCheck` obligation (for both a user trait and a built-in operator trait), a satisfied
bound is accepted whether called via inference or explicit `<...>`, the `&` sigil is what lets an
operator-trait method taking `Self&` (e.g. `Eq::eq`) actually be called, a bare (borrow) param works
for a Self-argument-free trait method, two same-trait params bind independently to different
concrete types, a trait declared in a separate imported file is recognised, an unsatisfied bound is a
clean semantic error, and the two v1-scope boundaries (a class method; mixing with an explicit
`<...>` on the same function) hit the ordinary pre-existing "not a known type" parser error — surfaced
that this specific error is a PARSE-time `CompileError`, which `parseString`'s catch block swallows
into an empty (trivially error-free) `Program`, so those two cases assert via `StderrCapture` rather
than `SemanticResult::hadError` (`analyzeString` can't see a parse-time failure at all). A follow-up
edge-case sweep (prompted by "double check the tests for missed edge cases") added 4 more cases,
each verified by hand against a scratch `.gg` program first: `mut Trait&` (the trait recognised when
preceded by `mut`, both `mut` and `&` preserved), the `Trait*` explicit-borrow sigil, a `fn private`
free function using the sugar (the desugar skips the optional `private` between `fn` and the name),
and the false-positive guard — a parameter *named* like a trait (`fn f(i32 Eq)`, `Eq` preceded by the
`i32` type keyword, not `(`/`,`/`mut`) is NOT rewritten and stays an ordinary non-generic function.
Also added `e2e/trait_param_sugar_test.gg`. Full `GG_tests` suite plus a fresh compile-only sweep of
every file under `e2e/` (75 files, via `GG.exe --unsafe-ptr` directly) confirmed no regressions — the
same 7 pre-existing, unrelated failures already documented in the baseline below reappeared verbatim
(same files, same messages), none newly failing and none mentioning "not a known type" or "trait".
Separately, **`LANGUAGE.md` (the user-facing reference) was brought up to date** — it had last been
touched at commit `24aa3f3` and was missing/contradicting 10 commits' worth of features: added the
trait-param-sugar subsection (§13), `addressOf()` (§10), and corrected §2/§8 (bare `ClassName p;` no
longer auto-invokes a constructor; field default initializers + constructor field-init enforcement),
§6 (bare value-object params are sugar for a borrow), §11 (generic-method argument inference), §8
(private generic functions), §12 (dotted module names, `import a.b.*` wildcards, dotted self-loading
imports), and §15's stale "not supported" list (tuples/`match`/modules/`str`/variadics all shipped).
Prior baseline was
**1435 test cases / 2678 assertions** (as of making `resolveTypeToken` report "unknown type" for a
bare, unparameterized generic class template name instead of silently degrading to `Error → i32` —
see the new bullet under the "Generics" invariant above. 4 new `tests/semantic_tests.cpp` cases
(`[semantic][generics]`): the array-declaration, plain-var-decl, and parameter-type shapes of the bug
now error, plus a control case confirming a properly instantiated `Vec<i32>` is unaffected. Full
`GG_tests` suite + a manual sweep of every file under `e2e/`/`samples/` confirmed no regressions —
7 pre-existing, unrelated e2e failures were found in the sweep (definite-init/field-initialization
errors, an arg-count mismatch, a use-before-assignment, and a `List.gg` reference-type restriction),
none mentioning "unknown type," confirmed pre-existing and out of scope. `samples/std_lib.gg`'s own
stale `Array[5] arrayArray;` line (previously compiling silently wrong, now caught) was removed —
see the "Generics" invariant. Prior baseline was
**1431 test cases / 2670 assertions** (as of a follow-up edge-case sweep over the field-init feature
below, prompted by "double check the tests for missed edge cases." Hand-verified then permanently
pinned down 8 more `tests/const_tests.cpp` cases (`[fieldinit]`): per-constructor-overload isolation
(a class with two constructors, only one of which correctly initializes every field, errors on
exactly that one — and the correct sibling type-checks clean in isolation); a synthesized
constructor coexisting with a user-written destructor; a generic class's field default staying
independent per instantiation (two instantiations, no cross-contamination); a field default that
references the class's own type parameter; and the class's own static field from a default
initializer — confirmed **asymmetric but intentional**: `Config::DEFAULT` (explicit) works (static
access resolves via the type name, independent of the `currentClassName`-clearing guard), while bare
`DEFAULT` fails with "undeclared identifier" — verified this is not a new gap but an exact match for
`analyzeParamDefaults`' own pre-existing identical limitation (same guard, same asymmetry), so the
field-initializer feature inherits established behavior rather than introducing a new inconsistency.
All were already correct before being pinned down — no bugs found, only missing coverage. Prior
baseline was **1423 test cases / 2658 assertions** (as of adding field default initializers + constructor
field-initialization enforcement — see the "Field default initializers + constructor
field-initialization enforcement" invariant above. 16 new `tests/const_tests.cpp` cases
(`[fieldinit]`) cover both initializer forms, the self-contained-only restriction (field/`this`
references rejected), the enum exclusion, the synthesized-empty-ctor path (and its interaction with
the bare-declaration no-magic rule), the missing-field error, all three exemptions (defaulted,
nullable, ctor-less field class — including the negative case: a field class with a MIXED zero-arg-
and-parameterized ctor set is NOT exempt), if/else branch-merge, the early-`return;` catch, and the
direct-construct-not-clone codegen shape plus prologue-before-body ordering. Migration: ~30
pre-existing tests across 12 files (fixed by adding the missing field assignment/default, not by
weakening the check — see the invariant above) plus one real stdlib bug fixed
(`stdlib/utility/HashMap.gg`'s constructor was calling a method on a field before that field's own
constructor had ever run — see the invariant's "Real-world fix" bullet). Prior baseline was
**1407 test cases / 2628 assertions** (as of removing the implicit zero-arg-constructor auto-invoke
on a bare `ClassName name;` declaration — see the "Object (class-value) locals" invariant above.
A follow-up edge-case sweep (prompted by "double check the tests for missed edge cases") added 3
more `tests/const_tests.cpp` cases pinning down shapes no existing test covered: a MIXED ctor set
(a zero-arg overload coexisting with parameterized ones — every existing test used a pure zero-arg
or pure parameterized set) is still deferred; passing a deferred-init object as a call ARGUMENT
before assignment errors (every existing test read via a field/method, not an argument); and the
direct-construct fast path works when a branch picks the zero-arg overload specifically out of a
mixed set. All three were hand-verified as already correct before being pinned down — no new bug,
just missing regression coverage. (Also found, en route, a genuinely separate pre-existing bug,
untouched here at the time: a fixed-size array of a bare non-parameterized generic class, e.g.
`Array[5] arr;` with no `<T>` — since fixed by teaching `resolveTypeToken` to error on any
unresolvable identifier; see the "Generics" invariant's "Bare generic template names are rejected as
an unknown type" bullet below. `samples/std_lib.gg`'s own `Array[5] arrayArray;` line, marked
`//TODO how does this get initialized?`, was removed as part of that fix — it never did anything real
in the first place.)
The prior hybrid auto-invoked a class's zero-arg-callable constructor on a bare declaration to avoid
disrupting existing code; this was deliberately reverted in favor of a simpler, uniform rule: ANY
constructor on the class (zero-arg or not) makes a bare declaration deferred-init, full stop — a
bare declaration never implicitly calls anything. Whether it's safe to use a bare declaration
immediately must never depend on the invisible detail of the class's constructor shape. An explicit
`ClassName name();`/`ClassName name{}` is unaffected (an ordinary initializer) and remains the way to
construct immediately.

This is a real behavioral reversion with a known, bounded migration cost — every bare
`ClassName x;` for a class that has ANY constructor (not just non-zero-arg ones, as before) must
become an explicit `ClassName x();`. Fixed across: 6 unit test files with 20 individual call-site
updates (`escape_tests.cpp`, `generic_method_tests.cpp`, `clone_tests.cpp`, `refcount_tests.cpp`,
`array_object_tests.cpp`, `ref_tests.cpp`, `variadic_tests.cpp`, `implicit_this_tests.cpp`,
`var_infer_tests.cpp`, `class_tests.cpp`, `named_args_tests.cpp`, `const_tests.cpp` — the latter also
had 3 tests specifically validating the now-removed auto-invoke behavior, repurposed to assert the
new no-magic rule instead), plus `samples/std_lib.gg` and `e2e/definite_init_object_test.gg`/
`e2e/array_position_ops_test.gg`. A broad re-compile sweep of every file under `e2e/` (not just the
ones touched by earlier sessions) turned up two more affected sites that had never been re-verified
against the *earlier* ctor-aware-hybrid work in this same session — `e2e/class_test.gg`'s `Point
zero;`/`Counter blank;` (both classes have only parameterized constructors, so these were already
broken under the hybrid, just never caught) — fixed to explicit zero-value constructor calls.

Also fixed along the way, found via the same e2e sweep: `Array<T>::contains(T* value)` compared
`data[i] == value` — for a class `T` implementing `Eq` with an owning-reference-typed `eq(Self&)`
parameter (e.g. `String`), passing the borrow parameter `value` as the right-hand operand hit the
deliberately-forbidden borrow→owning-reference coercion ("no matching 'eq' method"). Flipping the
comparison to `value == data[i]` fixes it: the right operand (`data[i]`, a genuine object *value*)
binds to the owning `Self&` parameter via the separately-allowed "value object → reference in
argument position" rule. This was a latent bug in `contains` since it was added, invisible until a
class with a real `Eq` impl (`String`) was used as `T` — surfaced by `e2e/array_clone_test.gg`, one
of two pre-existing stale e2e tests (the other: `e2e/array_object_test.gg`) that predated the
`push`/`pop` → `pushBack`/`popBack` + positional `push(pos,value)`/`pop(pos)` split and had never
been updated; both fixed to the current API. A third, previously-undiscovered stale test in the same
family, `e2e/array_test.gg` (single-arg `push`/no-arg `pop`), was also fixed the same way.

Prior baseline was **1403 test cases / 2618 assertions** (as of implementing `Array<T>`'s remaining unfinished methods
— `push`/`emplace`/`pop`/`delete`/`contains` — which surfaced two real, unrelated compiler bugs
along the way, both now fixed:
1. **`Array<T>::emplaceBack`/`emplace` never compiled for a primitive `T`** (e.g. `Array<i32>`) —
   the flagship `T(args...)` construct pattern becomes `i32(args...)` after monomorphization, and
   the parser had no notion of calling a bare primitive-type keyword at all; it was an unconditional
   hard error. Fixed by adding `PrimitiveType(x)` as cast-call syntax (`≡ x as PrimitiveType`),
   spread-aware — see the new "Primitive cast-call syntax" invariant above. Purely additive; no
   prior valid program could have relied on that exact "keyword immediately followed by `(`" shape
   erroring, since it was the only shape reachable there.
2. **A far more consequential, PRE-EXISTING definite-initialization gap**, found while chasing a
   heap-corruption crash in the new `push`/`emplace`/`pop`/`delete` methods against `Array<T>` for a
   resource-owning `T`: a bare `ClassName name;` (no initializer) was **unconditionally** marked
   definitely-initialized in `analyzeVarDecl` the instant its struct was zero-initialized —
   regardless of whether the class had a constructor, so `Array`'s own ctor (which sets up
   `cap`/`data`) silently never ran, and `cap` stayed `0` forever (`grow()` doubles `0` to `0`).
   Fixed with a ctor-aware hybrid: auto-invoke a zero-arg-callable constructor at a bare declaration
   (zero disruption — confirmed by first trying the "always require deferred init" version, which
   broke 27 pre-existing tests, all of which rely on exactly this "just works" pattern for a
   trivial zero-arg ctor); genuinely defer (require an explicit assignment before use, exactly like
   an uninitialized primitive) only when a class has constructor(s) but none is zero-arg-callable.
   The deferred case's single defining assignment additionally constructs directly into the
   destination with no temporary/clone (a real efficiency win for a "pick a different constructor
   per branch" pattern), gated safely against running inside a loop more than once. See the new
   "Object (class-value) locals" sub-bullet under the const-by-default invariant above for the full
   mechanism, the two-part `rejectUncloneablePtrOwner` gating this also required, and the specific
   pre-existing test (`Clone - assigning a raw-ptr class without Clone is a clean error`) whose
   regression caught an incorrect first draft of that gating.
Net: +10 test cases in `tests/const_tests.cpp` (`[mut][const]`, the definite-init hybrid across all
three ctor shapes + the direct-construct codegen shape + the two-part Clone gating), plus
`e2e/definite_init_object_test.gg` and `e2e/array_position_ops_test.gg` (the new `Array<T>`
`push`/`emplace`/`pop`/`delete`/`contains` methods against both a primitive and a resource-owning
element type — previously unreachable at all, since bug #2 corrupted memory before any assertion
could even run). Prior baseline was
**1393 test cases / 2604 assertions** (as of adding argument-driven type-argument DEDUCTION for
generic-method calls — `recv.m(args)` in place of `recv.m<T>(args)` — see the "Generic methods"
invariant's new sub-bullet above. Extracted the free-function inference's param-list-scan into a
shared `Parser::inferTypeArgsFromParamList` and added `Parser::inferGenericMethodTypeArgs` +
a new call-site branch in `parsePostfix` (`Parser_Expr.cpp`) right alongside the existing
variadic-method-call check. Caught one real bug against my own first draft, via hand-verification
with a scratch `.gg` program before writing the permanent tests: the call site's methodTemplates
lookup used the `$`-stripped BASE class name (`"Holder"`, mirroring the cheap `genericMethodKeys`
check used for the unrelated explicit-call-site heuristic) instead of the receiver's FULL,
possibly-mangled name (`"Holder$i32"`) — the actual key `captureMethodTemplate` registers under —
so inference always missed for a method on a generic class, silently falling through to the
"no method" error instead of ever succeeding. Fixing the lookup key surfaced a second, structural
finding (not a bug — a real v1 boundary, now pinned down by a dedicated test rather than left as
prose): inference for a method on a GENERIC owner class can never fire at all in practice, because
that owner's `methodTemplates` entry doesn't exist until the owner itself is monomorphized, which
happens strictly after the call site's own parse. So `Array<T>::pushAll`-shaped calls still need
explicit `<T>`; only methods on plain (non-generic) classes gained the new inference. 6 new
`tests/generic_method_tests.cpp` cases (`[genericmethod][geninfer]`: instance + static inferred
calls, dedup against an equivalent explicit call, an un-inferable-literal fallthrough, a named-arg
fallthrough, and the generic-owner-class limitation) + 1 new `e2e/generic_method_infer_test.gg`.
Full-suite regression run confirmed zero change to any pre-existing test. Prior baseline was
**1387 test cases / 2592 assertions** (as of hardening the `MemberAccessExpr` case in
`Parser::deduceArgTypeToken` added just below — a self-review pass looking for edge cases the
original fix missed found two real gaps, both now fixed):
1. **A reference/borrow/nullable-typed FIELD reached the pack-tuple mangling verbatim.** The
   original fix returned the field's declared type token exactly as stored in `classFieldTypes` —
   correct for a plain primitive field (`f32 x;`, the reported case) but wrong for a field declared
   `Class&`/`Class*`/`T?`: the token's lexeme carries the sigil (`"Inner&"`, `"ref:Inner"`, `"i32?"`),
   and `requestTupleType` mangles a pack element's type by appending its lexeme verbatim (`"$" +
   e.lexeme`) — `&`/`:`/`?` are not valid unquoted LLVM identifier characters, so e.g. `take(h.item,
   1)` where `item` is `Inner&` would have silently emitted an unparsable tuple class name
   (`%Tuple$Inner&$i32`), caught only by clang's parser (GG doesn't self-verify its IR). Fixed by
   routing the field's type through the SAME `genericArgBaseName` decay an identifier's declared type
   already gets (strip a trailing `?`/`&` or a `ref:` borrow prefix) before falling through to the
   shared `base`/`primTokenFor` return path — a reference-typed field and a value-typed field of the
   same class now both decay to one plain class element, exactly like a reference-typed local already
   did. Verified via generated IR: `take(h.item, 1)` now correctly emits `%Tuple$Inner$i32 = type {
   %Inner, i32 }` (not `Tuple$Inner&$i32`).
2. **`Class::field` (a static field via `::`) was not deducible at all.** `Class::field` parses to
   the identical `MemberAccessExpr` node as `obj.field` (see the "Static members" invariant) — but the
   object is an `IdentifierExpr` naming a CLASS, not a variable, so the existing recursive
   `deduceArgTypeToken(ma->object)` call (which only searches `scopes_`/`classFieldScope_` for a
   local/param/field binding) always returned `nullopt` for it, making `take(Holder::counter, 2)`
   silently un-inferable (fell through to the "cannot infer" error). Fixed by special-casing an
   object that is itself a known class name (`classNames`/`gen_->classNames`, the same check already
   used for the analogous `Class::method<T>()`/`Class::method(...)` receiver resolution a few lines
   above in `Parser_Expr.cpp`) before falling back to the recursive-variable-binding case.
3. Chaining through a reference-typed intermediate field (`h.item.v`, where `item` is `Inner&`) was
   exercised as a byproduct of fix #1 (the decayed `"Inner"` base name is what makes the recursive
   `classFieldTypes` lookup for `.v` succeed at all) and confirmed working, not a separate bug.

2 new `tests/variadic_tests.cpp` cases (`[variadic][codegen]`): a reference-typed field passed
directly into a pack argument (asserts the correctly-decayed `Tuple$Inner$i32`, not a broken
`Tuple$Inner&$i32`), and a static field accessed via `Class::field` as a pack argument. Net: +2 test
cases, +4 assertions (2592 vs. 2588). Prior baseline was
**1385 test cases / 2588 assertions** (as of teaching `Parser::deduceArgTypeToken` to resolve a
**field access** (`v.x`) as a pack/generic-argument type — found dogfooding `printf`: calling
`printf("There are {} vectors...", 2, v1.x, v1.y)` (`v1.x`/`v1.y` being plain `f32` fields of a
`vec2` value) failed with "cannot infer the type of a variadic argument to 'std.io.printf'", even
though the field's type is perfectly knowable at parse time — `deduceArgTypeToken` simply had no
case for `MemberAccessExpr` at all (only literals, in-scope identifiers, `Class(...)`/`new Class(...)`,
and `this`). Fixed by adding a `GenericRegistry::classFieldTypes` map (class/enum name → field name →
declared type token), populated for **every** class/enum as `Parser::parseMemberList` parses its
member list — regardless of which class is "current" (unlike the pre-existing `classFieldScope_`,
which only ever holds the ONE class actively being parsed, for lambda capture, and is cleared on
every new class). `deduceArgTypeToken` gained a `MemberAccessExpr` case: recursively deduce the
object's class (reusing the existing identifier/`this`/ctor/`new` cases), then look up the field's
type in `classFieldTypes` — so `a.b.c` chains work as long as each step is a plain field of an
already-parsed class. Relies on the same "a file's dependencies are parsed before its own tokens"
ordering already documented for cross-file generic inference (`std.math.vector.vec2` is parsed
before `samples/std_lib.gg`'s own call site, so its fields are already registered). Deliberately does
**not** help `args._0` on a synthesized pack tuple (`identity(args...)`, the pre-existing "cannot
infer" case for a spread into an inferred generic) — the tuple class isn't synthesized/parsed until
`runMonomorphization`, well after the call site's own parse, so it has no `classFieldTypes` entry yet
at deduction time; that limitation is unchanged (see the "Variadic parameters" invariant above). 1
new `tests/variadic_tests.cpp` case (`[variadic][codegen]`): a field of a value-object local passed
straight into a pack argument, asserting the correct `Tuple$i32$i32` instantiation. Net: +1 test
case, +2 assertions (2588 vs. 2586). Prior baseline was
**1384 test cases / 2586 assertions** (as of fixing `&&`/`||` to genuinely short-circuit — see the
"Logical `&&`/`||` genuinely short-circuit" invariant above. `genBinary` was eagerly evaluating both
operands and combining them with a plain `and i1`/`or i1` despite being commented "short-circuit",
making every `guard && use-guarded-value` idiom unsound (the guarded side still ran, and could still
trap, even when the guard was false). Caught via a stdlib `printf` format-string scanner
(`stdlib/io/IO.gg`) that used exactly that idiom for its own bounds guard. Fixed with real branching
in `CodeGen_Exprs.cpp::genBinary` (mirrors `genElvis`'s result-slot pattern) — no AST/semantic
change. 2 existing `tests/codegen_tests.cpp` cases were updated (they asserted the OLD eager `and
i1`/`or i1` shape, which no longer appears) to assert the new branch/label shape instead, and to
assert a full AND/OR truth table stays correct. 1 new `e2e/short_circuit_test.gg` (behavioral: an
out-of-bounds array guard that would abort() if the right operand were still evaluated, for both
`&&` and `||`). Net: 0 new test cases (2 rewritten in place), +8 assertions (2586 vs. 2578). Prior
baseline was
**1384 test cases / 2578 assertions** (as of adding Java-style wildcard imports, `import a.b.*;` —
binds every top-level member (type or free function) that module `a.b` declares, non-recursively
(a submodule `a.b.c` is a distinct registry key, never pulled in). Implemented entirely in
`scanModuleDirectives` (`Parser_Modules.cpp`): the existing dotted-segment reader is unchanged, but
a trailing `DOT STAR` now treats every collected segment as the module path and expands to one
binding per name found in `moduleTypes`/`moduleFuncs` for that module, reusing the same per-name
ambiguity-merge logic (`bind` helper) a single-symbol `import a.B;` already used — so a wildcard
colliding with another import on the same bare name is left unqualified and errors at use exactly
like today's explicit-import collisions. `scanModuleDirectives` gained two new (default-empty)
const-ref params (`types`, `funcs`) so it can look up a module's members; all four callers
(`Parser::parse`, and `ImportResolver`'s `verifyModuleDir`/`qualifyFileTokens`/`scanModules`) were
updated to pass `moduleTypes`/`moduleFuncs`. No lexer/AST change (`*` already lexes as `STAR`); no
change needed in `dependencyPaths`/`resolveModuleDir` (directory loading) since they already stop
their own dotted-chain read at the same `DOT STAR` boundary and resolve the pre-`*` segments to the
module's directory exactly as a bare `import a.b;` would. See the "Module namespacing" invariant
above. `samples/std_lib.gg` was updated to use `import std.io.*;` in place of 4 separate
single-symbol imports, as a real-world smoke test. 3 new `tests/module_tests.cpp` cases
(`[module]`): a wildcard binding every module member (type + free function), a non-recursion guard
(a submodule's member is NOT pulled in), and a wildcard-vs-explicit-import ambiguity case.

Runtime leak coverage lives in `e2e/refcount_leak_test.gg`: a live-object counter
(ctor `inc` / dtor `dec`) proves every heap allocation across ~30 scenarios is released
exactly once (balanced counter ⇒ no leak; a double-free crashes). Complemented by the IR-level
`tests/refcount_tests.cpp`.

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

**`LANGUAGE.md` must be updated in the SAME change whenever a user-facing language feature
changes.** CLAUDE.md is the internal/implementation reference; `LANGUAGE.md` is the user-facing
language reference (syntax, semantics, and the "What GG does NOT support" list). Any change to
**observable language behavior** — new syntax, a new type/operator/trait, changed defaults (e.g.
"bare `ClassName p;` no longer auto-invokes a constructor"), a feature moving from unsupported to
supported, or a new v1 limitation — must be reflected in `LANGUAGE.md` too, not just here.
- When you promote a feature from "not yet supported" to working, **remove it from `LANGUAGE.md`'s
  §15 ("What GG does NOT support")** in the same commit — a stale §15 that contradicts the body of
  the same document is the most common way `LANGUAGE.md` rots (it happened across ~10 commits once).
- Purely internal changes (refactors, codegen/IR shape, pass reordering, new C++ source files, test
  scaffolding) touch **only CLAUDE.md** — `LANGUAGE.md` describes the language, not the compiler.
- `LANGUAGE.md` is git-tracked; CLAUDE.md is not. So a "did I forget to update the docs?" check is
  `git log -1 -- LANGUAGE.md` vs. the language-feature commits since — if they've diverged, sync it.
