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
  mangled.
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
  locals are skipped). `this` is not declared (it's the `%self` value, not an alloca).
- **v1 limitation**: a single `DIFile` = the main source; lines from imported files map against it
  (multi-file line attribution deferred).

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
- **`stdlibDir` auto-detection** (`main.cpp`, `findStdlibDir(argv0)`): honors `$GG_STDLIB` first,
  else walks **up from the GG executable's directory** for a `stdlib/` folder (confirmed by its
  `crt/` subdir). `searchRoots` come from `$GG_PATH` (`;`-separated, `findSearchRoots`). Both are
  computed in `main`, passed via the `GG(paths, options, ModuleSearchConfig)` ctor →
  `resolver.resolve(file, config)`. The default `resolve(file)` (no config) is file-relative only,
  so the resolver **unit tests are unaffected** (they use temp files + relative imports).
- The stdlib's own internal imports stay file-relative (`import "crt/cmem.gg"` — siblings, robust);
  `e2e/` + `samples/` were migrated from `../stdlib/...` to `std/...`. **v1 scope**: this is axis-A
  (resolution) only — symbols are still flattened into one flat-global `Program` (no per-module
  namespacing; that's the deferred axis B). No manifest / dependency versioning yet.
  Tests: `tests/import_tests.cpp` (`[import]`), `e2e/module_std_test.gg`.

### Module namespacing — `module` + Java-style `import a.B` (axis B)
Per-module namespaces so two files may each declare `class Foo` without colliding (the deferred
axis-B counterpart to the `std/` resolver above). A file declares `module geo;`; its top-level names
are qualified with that prefix (`geo.Foo`, `geo.Foo_method`, `geo.Vec$i32`). **Java model**:
`import geo.Point;` lets you write bare `Point`; two imported modules that both export `Point` make
the bare name ambiguous, resolved by the fully-qualified `geo.Point` at the use site.
- **Key design — qualify the name STRING in the parser, as a whole-token rewrite** (mirrors generic
  monomorphization, which turns `Vec<i32>` into the bare className `Vec$i32`). Because `mangleType`
  returns `t.className` verbatim and codegen builds every `%`/`@` symbol from the className string,
  baking the module prefix into the name early means **semantic and codegen need no changes** — the
  qualified name flows through as an ordinary className. The rewrite (`Parser::qualifyTokens`) runs
  in `parse()` after reading the file's directives, before the parse loop:
  1. **Fold FQNs**: `IDENT(mod) '.' IDENT(Name)` where `mod` ∈ `moduleNames` → one `"mod.Name"`
     token. So a fully-qualified reference becomes a single identifier — never three tokens — and
     `consumeTypeCore`/`isTypeName`/`parsePrimary`/decl builders are **untouched** (they see one
     already-qualified token). This is also why ctor/dtor "just work": the class name **and** the
     ctor/dtor names (which equal it) are qualified together, so `name == className` still holds.
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
- **Separator = `.`** (`geo.Foo`) — a legal unquoted LLVM identifier char, matching the FQN spelling.
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
- **v1 scope / limits** (all → a clean error, never a miscompile): single-segment module names only
  (`module geo;`, not `module a.b;`); no `import a.*;` wildcard, no import aliasing, no module-private
  visibility (namespacing only). `import a.B;` does **not** load a file — the defining file must
  still be `import "…"`-loaded. An ambiguous bare name errors with a generic message (use the FQN).
  A local named identically to a module name would shadow the FQN fold (module wins; don't do it).
  Residual name-collision edges (all → a clean error, never a miscompile, all niche): a **field or
  enum-variant named identically to a module TYPE** (types qualify in any position, but the field's
  `.`-access does not → "no field/variant"); a **local named identically to a top-level function**
  used in a `<` comparison (`count < 9` parses `<` as generic-call position → "cannot use function as
  value"). Rename one. Tests: `tests/module_tests.cpp` (`[module]`), `e2e/module_test.gg`
  (+ `e2e/mods/*.gg`).

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
- `monomorphize()` substitutes type args, re-parses each instantiation into a concrete
  mangled declaration. **Semantic and CodeGen never see type parameters.**
- **Mangling** (`$` separator; `$` and `.` are valid unquoted LLVM identifier chars):
  - `Vec<i32>`       → `Vec$i32`
  - `Pair<i32,f64>`  → `Pair$i32$f64`
  - `Vec<Box<i32>>`  → `Vec$Box$i32`
  - `Vec<Point&>`    → `Vec$Point.ref`
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

### Memory model
- **Value objects** (`Point p`): stack-allocated struct; copied on assignment.
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
- **Object parameters must be references** — the semantic analyser rejects bare value-type
  class parameters. Primitive parameters always pass by value.
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
    unsynchronised, so a real threading story needs a memory model on top (deferred).
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
    `genElementAddress`), or the value itself for a borrow-returning call. Used by `genVarDecl`
    (primitive-borrow init) and `genReturn` (primitive-borrow return). `analyzeVarDecl` requires the
    source be an addressable lvalue (`isLvalueExpr`) — a temporary (`a+b`) is rejected.
- **Coercions INTO a class borrow** (`canImplicitlyCast`, `Silent`): owning `Class&`→`Class*`, value
  `Object`→`Class*`, `Class*`→`Class*`. The reverse — a borrow → an owning `Class&` — is **forbidden**.
- **Not a field**: `buildClassInfo` rejects a borrow field.
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
- **Operators**: `!!` (`analyzeUnwrap`/`genUnwrap`) null-checks and `abort()`s on null (no
  exceptions), else passes the ptr through. `?:` (`analyzeElvis`/`genElvis`) selects the non-null
  operand via a result slot. `== null` / `!= null` route through `classifyEquality`'s new null case
  → the address-identity `icmp eq/ne ptr …, null` path.
- **Smart-casts** (`Symbol::isNarrowedNonNull`, snapshotted like `isInitialized`): `if (x != null)`
  narrows the then-branch, `if (x == null)` the else; **guard clauses** (`if (x == null) { return; }`
  → non-null after) reuse `alwaysReturns`; branch joins merge (non-null iff on every reaching path).
  `analyzeIdentifier` returns `stripNullable` for a narrowed binding (recorded on the use-node —
  free for references, same `ptr`). **`mut` invalidation**: `analyzeAssign` refreshes the bit from
  the RHS (non-null value ⇒ still narrowed; `null`/nullable ⇒ dropped). Loops intersect pre-loop and
  post-body narrowing (`mergeLoopNarrowing`) so a body can neither leak nor unsoundly keep a cast.
  **Bare local/param bindings only** (not field chains); `&&`-narrowing not done.
- **Codegen**: nullable references need **no representation change** (still `ptr`); the smart-cast
  is representation-transparent for references. Tests: `tests/nullable_tests.cpp` `[nullable]`,
  `e2e/nullable_test.gg`.

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
- **Codegen** (`genLiteral`): emits the literal at its resolved semantic type. Integer literals are
  **masked to the target width** (`unsigned long long` parse, `& ((1<<bits)-1)`) so the constant is
  always a valid `iN` even when out of range — `i8 = 300` → `store i8 44`, not the malformed
  `store i8 300` (a >u64 literal that can't even be parsed emits `0`; both cases are already warned).
  An integer lexeme in a float slot becomes a decimal (`5` → `5.0`); an **f32** literal is emitted as
  an **LLVM hex-float** (`store float 0x3FE0000000000000`) computed from the value rounded to float —
  necessary because f32 was previously only ever reached via `fptrunc` from an f64 constant, and LLVM
  rejects a non-exactly-representable decimal for `float`.
- Tests: `tests/cast_tests.cpp` (`[numlit]`).

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
- **Trap** = `emitOverflowTrap(badCond)` (`CodeGen_Emit.cpp`): mirrors `emitBoundsCheck` — cond-branch
  to an `abort()`+`unreachable` block (reusing `ensureAbortDeclared`), continue in a fresh block.
- **Cost / rationale**: the overflow bit is free from the ALU; the added cost is a well-predicted
  not-taken branch (+ lost auto-vectorization at higher `-O`). At the default `-O0` the overhead is
  small. Compile-time-known-safe conversions (our untyped-literal adoption) never emit a check.
- **clang `-O` passthrough**: `compile.ps1 -Opt <0|1|2|3|s|z>` adds `-O<level>` to the clang step
  (default `0` — GG previously always compiled unoptimized). Independent of overflow checks.
- Tests: `tests/cast_tests.cpp` (`[overflow]`), `e2e/overflow_checks_test.gg` (run with `-OverflowChecks`).

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
- **v1 limitation**: capturing a `var`-typed local inside a lambda is unsupported (the parser records
  the capture's type at parse time, before inference runs). Rare; a clear failure, not a miscompile.
- Tests: `tests/var_infer_tests.cpp` (`[var]`), `e2e/var_infer_test.gg`.

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
  is true. `extern` declarations are always exempt.
- `compile.ps1` always passes `--unsafe-ptr`; test helpers default to `allowRawPtr = true`.

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
- **Lexer/parser**: `str` keyword → `TokenType::STR` (in `isTypeName`/`isTypeKeyword`); `str*` (a
  borrow of `str`) is a parse error. **Type plumbing**: `typeFromToken`/`typeName`/`typeKindFromName`/
  `primitiveTokenType` handle `Str`; `mangleType` falls through to `"str"`; `synthTypeToken(Str)` →
  the `STR` keyword token (so `var s = "x"` round-trips). `analyzeLiteral(STRING)` returns `Str`
  (was `Ptr`), so **string literals are now safe** and `var s = "lit"` is ungated.
- **`String(str)`**: the stdlib constructor takes a `str` and uses `literal.len` (no `strlen` scan) +
  `literal.data`. All CRT bindings stay `ptr` and receive `String`'s `ptr` buffer, so `crt/*.gg` is
  unaffected.
- **Debug info**: `dbgSizeAlign(Str)` = `{16, 8}`; `dbgTypeOf` approximates it as an opaque pointer
  (Phase 1 — a precise composite is deferred; debug is off by default).
- **Reflection strings are `str`** (Phase 2): `@typeName(T)` returns a `str` (was a bare `ptr`) — so
  `@typeName(T).len` and `.data` work; `f.name`/`v.name` were already `str` (they lower to string
  literals). `analyzeReflect(TypeName)` → `Str`; `genReflect(TypeName)` materializes the `{ptr,i64}`
  view over its private NUL-terminated constant.
- **Deferred, rejected with a clean semantic error (never miscompiled)**: `==`/`!=` on `str`
  (`classifyEquality` guards it before the numeric fall-through), relational ops (numeric check),
  `str + str` (numeric check), and `str?` (`resolveTypeToken` rejects it alongside `ptr?`/`void?`).
  Also deferred: `str` indexing/slicing/concat. A `str` value-object **field** works (POD `{ptr,i64}`,
  memberwise-copied). Tests: `tests/str_tests.cpp` (`[str]`), `e2e/str_test.gg`.

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
  canonical `filename`. Generic free functions (`fn name<T>`) are name-first in the capture path, so
  `private` on a **generic** free function is not supported in v1 (non-generic only).
- **Semantic**: `FunctionOverload` carries `isPublic` + `sourceFile` (filled in `collectFunctions`).
  `currentFile_` is set to the declaring file at the top of `analyzeFunctionDecl` (save/restored) and
  is **empty inside class/enum/impl bodies**. The free-function call site in `analyzeCall` warns iff
  `!chosen.isPublic && both files known (non-empty) && they differ` — so same-file calls and
  unknown-origin contexts (method bodies) never warn (zero false positives; conservative v1).
- **Codegen**: unaffected — access control is purely a semantic warning (no name mangling / linkage
  change; `private` functions keep their plain `@name`). Tests: `[private]` in `tests/semantic_tests.cpp`.

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
**1081 test cases / 2015 assertions** (as of **const-by-default static fields** — a static field is now
**immutable unless `mut static`**, closing the one gap in GG's const-by-default model (static fields
were previously always mutable). Enforced purely in semantics: `ClassInfo::StaticField` gained
`isMut` (from `FieldDecl::isMut`; the parser already accepted `mut`/`static` in either order on
fields), and every static write form is gated against it — `Class::field =`, `obj.field =`, and
implicit-`this` `=`/`+=`/`++`/`--` (new `currentStaticField` helper). `static i32 MAX = 100;` is a
named constant; `mut static i32 count = 0;` a reassignable counter. **Codegen unchanged** — const is
compile-time only, the emitted global is byte-identical. Migrated the always-mutable statics in the
suite + six e2e files (counters/totals → `mut static`). This is item 4's "const statics"; access
enforcement + file-scope linkage remain deferred. A QA pass confirmed the tricky edges: a const
static is **not** writable in a constructor (the instance-const-field `inConstructor` allowance does
not leak to statics), const gating survives monomorphization (a generic class's static, via instance
access), and `static mut`/`mut static` both work. See the static-members invariant;
`tests/static_tests.cpp` (`[static]`, +12 cases), `e2e/static_field_test.gg`. Prior baseline was
**1069 test cases / 1996 assertions** (as of a **sret nested-argument confirmation + escape-analysis
return-slot-clone fix**, with a **QA follow-up that caught a method-path bug in the first fix** — the
CLAUDE.md note "using a value-return call as a nested sub-expression argument is deferred" was
**stale**: `takeRef(make(1,2))`, `combine(make(),make())`, doubly-nested calls, a discarded temp, and
a method call on an sret result all already work via the `genCall`/`genMethodCall` `materializeSlotTemp`
+ `emitSretCall` value path (only variable-init uses the zero-copy in-place `emitSlotCall`). The
confirmation surfaced a **real, separate** escape-analysis false positive: `fn dup(T& v) -> T out
{ out = v; return out; }` (a borrowed param cloned into an object return slot) wrongly reported the
value-object argument as escaping — the syntactic scan treated `out = v` as an alias edge, and
`return out` then made `v` "escape." Fixed by recording **no** alias edge for an assignment into a
return slot. **The QA pass then found the first fix gated on `returnType.kind == Object`, which
silently missed every *method* self-return** — the escape summary is computed in `buildClassInfo`,
where the method's own class isn't in `classRegistry` yet, so its slot type resolves to a non-`Object`
placeholder (free functions, collected later, worked by accident). Re-gated on `returnType.kind !=
Reference` (via the `nonRefSlotName` helper): object/primitive/enum slots clone (skip the edge),
while reference/borrow slots (`-> T&`/`-> T*`, which decode to `Reference` from token syntax) keep the
edge and still reject a stack-value argument; the ref-field-store escape path is untouched. See the
sret + escape-analysis invariants; `tests/return_slot_tests.cpp` (`[return-slot]`, +7 cases: nested-arg
codegen, free-fn/method/impl-method clone accepted, reference-slot/borrow-slot/ref-field-store still
rejected), `e2e/return_slot_test.gg`. Prior baseline was
**1062 test cases / 1985 assertions** (as of a **QA pass over module namespacing** — the initial
whole-token rewrite was context-free, so a **member declaration** whose name collided with a
module-level symbol was qualified while its `.`-access was not, wrongly rejecting valid code: a class
**field** or **method** named like a module free function (`size`, `area`, `count`) failed with
"no field/method". Fixed by making qualification **context-aware**: the module table is split into
**types** (qualified in any position) and **funcs** (qualified only in call/generic position `(`/`<`),
and a **method / enum-member decl name** (right after `fn` inside a body) is never qualified — so a
field/local named like a free function is left bare and matches its access. Generic free functions
stay namespaced (the `<` case); the residual edges (field/variant named like a TYPE; a local named
like a top-level fn used in a `<` comparison) are niche and error cleanly, never miscompile. See the
"Module namespacing" invariant; `tests/module_tests.cpp` (`[module]`, +3 cases → 12). Prior baseline
was **1059 test cases / 1979 assertions** (as of **module namespacing — `module` + Java-style
`import a.B`** (axis B). A file declares `module geo;` and its top-level names are qualified with that
prefix (`geo.Foo`), so two files may each declare `class Foo` without colliding. `import geo.Point;`
drops the qualifier (bare `Point`); two imported modules both exporting `Point` → the bare name is
ambiguous, disambiguated by the fully-qualified `geo.Point`. Implemented as a **whole-token rewrite
in the parser** (`Parser::qualifyTokens`, mirroring generic monomorphization): fold `mod.Name` into
one token, then prefix bare decl/ctor/dtor/reference names — so semantic + codegen are unchanged (the
qualified name flows through as an ordinary className, emitting `%geo.Point`/`@geo.Point_method`). A
new ImportResolver **pass 0** (`scanModules`) builds the shared `moduleMembers`/`moduleNames` tables
before parsing. `main`/`extern` are never qualified; module-less programs are byte-identical (IR
regression-safe). Adds a duplicate-type-in-module diagnostic (closes the old silent first-wins gap).
v1 limits: single-segment module names, no wildcard/alias import, no module-private visibility; a
symbol `import` doesn't load a file. See the "Module namespacing" invariant; `tests/module_tests.cpp`
(`[module]`, +9 cases), `e2e/module_test.gg`. Prior baseline was
**1050 test cases / 1961 assertions** (as of **module resolution — import search roots + the `std/`
anchor** (axis A). `import "..."` now resolves through one shared `ImportResolver::resolveImportPath`
against a `ModuleSearchConfig`: a reserved **`std/` prefix** maps to the compiler's standard library
(auto-detected by walking up from the GG executable for a `stdlib/` dir, or `$GG_STDLIB`), so
`import "std/String.gg"` works from anywhere instead of a location-dependent `../stdlib` walk;
**file-relative** resolution stays the backward-compatible default (all pre-existing relative imports
unchanged, and it takes precedence over search roots); extra **`$GG_PATH`** roots are tried last.
Config is computed in `main.cpp` and threaded via the `GG(paths, options, ModuleSearchConfig)` ctor;
the default `resolve(file)` is file-relative-only, so resolver unit tests are unaffected. `e2e/` +
`samples/` migrated to `std/...`; stdlib-internal imports stay sibling-relative. Symbols are still
flat-global (per-module namespacing = the deferred axis B; no manifest/versioning yet). See the
"Module / import resolution" invariant; `tests/import_tests.cpp` (`[import]`, +4 cases),
`e2e/module_std_test.gg`. Prior baseline was
**1046 test cases / 1955 assertions** (as of **impl specialization — concrete overrides blanket**
— a concrete `impl Trait for Class<i32>` and the blanket `impl<T> Trait for Class<T>` may now coexist
("most specific wins"): the concrete impl is used for `Class<i32>`, the blanket for every other
instantiation. Previously the two collided as a duplicate `Class$i32::method`. Implemented by a skip
in `runMonomorphization` — before expanding the worklist it collects every explicit concrete impl's
`Trait@MangledClass` key, then skips instantiating the blanket for a class a concrete impl already
covers. Order-independent and per-trait; GG expresses only two tiers (no partial specialization), so
there's no ambiguity. A double-check QA pass added coverage — multi-param generics, operator traits
(`Add`), the two-identical-concrete duplicate guard — and fixed a **`Call`-trait corner**: a concrete
`impl Call` canonicalizes to `Call$…` while the blanket stores raw `Call`, so a raw-`Call@class` alias
key is now also registered so the concrete still suppresses the blanket (previously they collided).
See the generic-impls invariant; `tests/trait_tests.cpp` (`[specialization]`, 8 cases),
`e2e/impl_specialization_test.gg`. Prior baseline was
**1038 test cases / 1937 assertions** (as of **generic type-argument deduction** — a generic free
function called without explicit `<…>` infers its type parameters from the argument types
(`valid(u)` ⇒ `valid<User>(u)`). Runs entirely in the parser, reusing the `scopes_` name→type-token
stack it already maintains for every in-scope local/param/field; v1 matches a **top-level** type-param
parameter (`T`/`T&`/`T*`/`T?`) against a positional argument whose type is syntactically known (an
in-scope identifier, a `Class(...)` constructor call, or `new Class(...)`), all-or-nothing, else a
clear "cannot infer … specify explicitly" error. Purely additive (a no-`<…>` template call was already
an error). A double-check QA pass added edge coverage — repeated type param (`f<T>(T&,T&)`),
bare-instance-field argument, named callable-object → `Call`-bound inference, mismatched-repeated-arg
and named-arg errors — and fixed two bugs in the process: a **`var`-typed local argument** produced a
bogus `f$var` instantiation (now a clean error, via a `var`-sentinel guard in `baseFrom`) and the
scope walk **skipped past a shadowing `var` binding** to an outer same-named local (now stops at the
innermost binding). See the "Generic type-argument deduction" invariant; `tests/generic_infer_tests.cpp`
(`[geninfer]`, 14 cases), `e2e/generic_infer_test.gg`. Prior baseline was
**1024 test cases / 1921 assertions** (as of a **QA edge-case pass over annotations** — added
field-type coverage (`f.get` splicing `char`/`bool` const values), target coverage (`@hasAnnotation`
seeing a **method-carried** and a **class-level** annotation), and more error cases
(non-comptime annotation field type, annotation/class name collision, `@hasAnnotation` with an
undeclared annotation). The pass surfaced and fixed a **pre-existing `char` bug**: `integerLiteralFits`
had no `Char` case, so **any** integer literal adopted into `char` (`char c = 65;`, and the annotation
`f.get` char-field default placeholder) spuriously warned "does not fit in 'char'" — `char` is a
32-bit code point (u32 range), now handled; the char-field placeholder is also a `char` literal, not a
numeric `0`. `tests/annotation_tests.cpp` (`[annotation]`, 22 cases), `tests/cast_tests.cpp`
(`[numlit]` char adoption). Prior baseline was
**1015 test cases / 1906 assertions** (as of **user-defined annotations — compile-time metadata,
Phases 1 & 2** — `annotation Name { <const fields> }` declares a metadata type (no methods; fields
limited to the comptime-constant set: `str`/primitive/`bool`/`char`/enum); `@Name(args)` prefixes a
class / field / method / enum-variant with compile-time-constant args. Read only via reflection:
`f.has(Ann)`→bool and `f.get(Ann).field`→the const value inside `inline for`, and type-level
`@hasAnnotation(T, Ann)`→bool. **Zero runtime cost, no IR** — annotation arg tokens are captured raw
at parse time and spliced by the same post-monomorphization `expandReflection` pass. A leading `@` in
declaration position is disambiguated from a reflection builtin by **case** (Capitalized = annotation).
Semantic validation (`collectAnnotations`): unknown-annotation / duplicate-on-target / arg-count /
non-constant-arg errors. v1 limits: `comptime if` isn't pruned so an unguarded `f.get` on an absent
annotation folds to a type-default placeholder (`f.has`-guard it); no `@Target`, no enum-variant-arg
splicing. See the "Annotations" invariant; `tests/annotation_tests.cpp` (`[annotation]`, 14 cases),
`e2e/annotation_test.gg`. Prior baseline was
**1001 test cases / 1883 assertions** (as of **the `str` compile-time string-literal type, Phases 1 & 2** —
a string literal is typed `str` (an immutable `{ ptr, i64 }` view over static bytes) instead of a
raw `ptr`: `.data` decays to a NUL-terminated `ptr` for FFI, `.len` is the O(1) byte length, `str`
is safe/ungated and implicitly decays to `ptr` (the reverse is forbidden), and `String`'s constructor
now takes `str` (using `.len`, no `strlen`). Modeled on the nullable-primitive inline value type; CRT
bindings stay `ptr` and are untouched. **Phase 2** retrofitted reflection: `@typeName(T)` now returns
a `str` (was a bare `ptr`), so `@typeName(T).len`/`.data` work (`f.name`/`v.name` were already `str`).
Still deferred (all rejected with a clean semantic error, never miscompiled): `str` `==`/relational/
concat and `str?`. See the "`str`" invariant; `tests/str_tests.cpp` (`[str]`, 16 cases) +
`[reflect]` (`@typeName` view), `e2e/str_test.gg` + `e2e/reflect_test.gg`. Prior baseline was
**985 test cases / 1858 assertions** (as of **compile-time reflection, Phase 2** — enum + type-level
queries on top of Phase 1: `inline for (v in @variants(E)) { … }` unrolls once per variant (bare `v`
→ the singleton `E::VARIANT`, usable as a value — `v.method()`/`v.field` work; `v.name` → the
variant-name string); scalar folds `@variantCount(E)`,
`@alignOf(T)`, `@offsetOf(T,"f")`; predicates `@isInteger/@isFloat/@isClass/@isEnum/@isPrimitive(T)`
and `@implements(T, Trait)` (user + built-in operator traits). Same post-monomorphization
`Parser::expandReflection` pipeline as Phase 1 — each query folds to a constant in `genReflect`
(layout via `dbgSizeAlign`, traits via `implementedTraits` surfaced onto `SemanticResult`), no
`comptime if` yet (a predicate is an `i1` constant). `ImportResolver::collectClassNames` now also
collects `enum` names so `@variants(T)` works after a generic type param is substituted with a
concrete enum. Deferred to Phase 3: `f.type`/indexed field type queries, `comptime if`, method
reflection. See the "Compile-time reflection" invariant; `tests/reflect_tests.cpp` (`[reflect]`, 37
cases), `e2e/reflect_test.gg`. Prior baseline was
**966 test cases / 1828 assertions** (as of **compile-time reflection, Phase 1** — `@`-sigil builtins
+ `inline for`: `inline for (f in @fields(T)) { … }` unrolls at compile time (one body copy per
field, `f.name` → the field-name string, `@field(v, f.name)` → ordinary member access), and scalar
queries `@typeName(T)` / `@fieldCount(T)` / `@hasField(T,"x")` fold to constants like `sizeof`;
`@compileError(msg)` aborts compilation. Implemented as a **post-monomorphization parser expansion
pass** (`Parser::expandReflection`) that lowers everything to ordinary AST — semantic/codegen get only
scalar-query arms + trivial guards, no reflection logic. Zero runtime cost; no runtime type info. See
the "Compile-time reflection" invariant; `tests/reflect_tests.cpp` (`[reflect]`, 18 cases),
`e2e/reflect_test.gg`. Prior baseline was
**948 test cases / 1801 assertions** (as of **mixed signed/unsigned arithmetic resolves to the SIGNED
type**, plus a **shift-signedness fix** surfaced by it. GG departs from C's "unsigned wins":
`commonArithmeticType` returns a *signed* int of the wider width for a mixed pair, so `-6 / 3u == -2`
and `-1 < 1u` is true (signed `slt`); the unsigned operand is reinterpreted as signed. Affects every
`commonArithmeticType` consumer (`+ - * / %`, bitwise `& | ^`, comparisons). **Shifts are now
excluded** — `<<`/`>>` follow the **left operand's** type (fixing `ashr`/`lshr` selection: `u32 >>
signedCount` is now `lshr`, `i32 >> unsignedCount` is `ashr` — the latter was already wrong before
this change). `tests/cast_tests.cpp` `[arith]` (8 cases). Prior baseline was
**940 test cases / 1787 assertions** (as of extending `--overflow-checks` to **`++`/`--` (prefix &
postfix) and signed unary `-`** — previously only `+`/`-`/`*` and `+=`/`-=`/`*=` were checked, so
`x++` at a type's max silently wrapped while `x += 1` trapped; now consistent. Also verified (already
correct) that narrowing conversions are checked in **return / call-argument / assignment** positions,
not just var-init. `tests/cast_tests.cpp` `[overflow]` (17 cases). Prior baseline was
**933 test cases / 1777 assertions** (as of a **numeric-literal adoption refinement**: a bare
literal operand of a binary op now adopts the type of a *non-literal* numeric sibling, so
`bigI64 == 6000000000` / `bigI64 + 6000000000` no longer wrap/warn; the both-literals `1 / 2` and
`1.0 / count` cases are preserved. `tests/cast_tests.cpp` `[numlit]`. Prior baseline was
**929 test cases / 1769 assertions** (as of **opt-in integer overflow checks (`--overflow-checks`)
+ clang `-O` passthrough**: Rust-style, off by default. When on, integer `+`/`-`/`*` (and
`+=`/`-=`/`*=`) trap via `llvm.{s,u}{add,sub,mul}.with.overflow.iN`, and an out-of-range *implicit*
narrowing / sign-changing conversion traps after a range check (`trunc`→extend-back→`icmp ne`); an
explicit `as` cast is never checked (intentional truncation, Rust/C semantics), nor is division /
float arithmetic. Trap = `abort()` (reuses the `!!` path). **Off, the emitted IR is byte-identical
to before.** `compile.ps1` gained `-Opt <0|1|2|3|s|z>` (→ clang `-O<level>`, default 0) and
`-OverflowChecks`. See the "Integer overflow checks" invariant; `tests/cast_tests.cpp`
(`[overflow]`, 10 cases), `e2e/overflow_checks_test.gg`. Prior baseline was
**919 test cases / 1750 assertions** (as of **numeric-literal type adoption (untyped constants)**:
a bare numeric literal is untyped — it adopts the contextual expected type when that is numeric,
else defaults to `i32`/`f64`. So `i64 y = 5;`, `f32 f = 1.0;`, `u32 n = 5;`, `u8 c = 5;` no longer
warn/error; `i64 big = 9000000000;` types the literal i64 directly (fixing the old silent i32
overflow); an out-of-range literal (`i8 = 300`) or an i32-default overflow (`var z = 9000000000`)
warns. Adoption is confined to **direct** literal bindings — `analyzeBinary` clears `expectedType_`
for its operands, so `f64 d = 1 / 2;` keeps integer division. f32 literals emit as LLVM hex-floats.
`genLiteral` masks the integer constant to the target width so an out-of-range literal always emits
**valid** `iN` IR (`i8 = 300` → `store i8 44`) instead of the previously malformed `store i8 300`.
Adoption works across every binding context — var-init, assignment RHS, `return`, and the inner type
of a nullable target (`i64? = 5`). See the "Numeric-literal type adoption" invariant;
`tests/cast_tests.cpp` (`[numlit]`, 18 cases). Prior baseline was
**901 test cases / 1708 assertions** (as of **local type inference — the `var` keyword**:
`var x = 5;` deduces a local's type from its initializer, const by default (`mut var`/`var mut` for
mutable); the deduced type flows back through `synthTypeToken` as an ordinary type token so
semantics/codegen gain no new type-kind and emit IR byte-identical to the explicit form; rejects
inference from `void`/`null`/lambda/unspellable, and `static var` keeps the static-local rules. The
`[var]` group (27 cases) covers inference across primitives/value-objects/heap-refs/borrows/enums/
nullable-primitives, `mut`/const-by-default, `for`-init + nested-scope shadowing + self-reference,
`static var` (persist + non-primitive rejection), the raw-ptr gate, and `var` inside a **generic
body** (surviving monomorphization). See the "Local type inference" invariant;
`tests/var_infer_tests.cpp` (`[var]`), `e2e/var_infer_test.gg`.) Prior baseline was
**874 test cases / 1656 assertions** (as of the **comprehensive nullable-type test pass** — the
`[nullable]` group grew to 65 cases (incl. nullable composing with generics and `switch`) covering the full surface (references/primitives/enums, `null`/
`==null`/`!!`/`?:`/`?.`, smart-casts incl. `mut` invalidation, nullable fields/params/returns,
overload interaction, codegen IR). The pass surfaced and fixed **three real bugs**: (1) Elvis with a
nullable RHS (`a ?: b`, both nullable) wrongly checked the RHS against the *non-null* result;
(2) a nullable **enum local** (`Color?`) mis-alloced as `%Color?` instead of `ptr`; (3) nullable
**primitive/enum fields** (`i32? tag`) were unresolved in both the semantic field pass and the codegen
struct layout. `tests/nullable_tests.cpp`.) Prior baseline was
**833 test cases / 1598 assertions** (as of **`?.` safe-call — Phase 2 complete**: `x?.field` /
`x?.m(args)` on a nullable receiver — null-checked once, result flows through a slot (empty on the
null path), wrapped nullable on the present path; a value-object result is rejected. With this the
Kotlin-style null-safety story — nullable references + primitives, `null`/`==null`/`!!`/`?:`,
smart-casts, and `?.` — is fully implemented. `tests/nullable_tests.cpp` `[safecall]`,
`e2e/nullable_test.gg`.) Prior baseline was
**830 test cases / 1596 assertions** (as of **nullable primitives `i32?` — Phase 2**: a nullable
primitive lowers to an inline tag+payload `{ i1, iN }` value (stack, no heap); `null`/`== null`/
`!!`/`?:` and smart-casts all work on it (payload extracted with no runtime check once narrowed);
`ptr?`/`void?` and nullable value objects stay rejected. `?.` safe-call remains parsed-but-deferred.
`tests/nullable_tests.cpp` `[primitive]`, `e2e/nullable_test.gg`.) Prior baseline was
**828 test cases / 1593 assertions** (as of **nullable references `T?` — Kotlin-style, Phase 1**:
`Class&?`/`Class*?`/`Color?` (reference-like only; value objects + primitives rejected, primitives
deferred to Phase 2), `null` literal, `== null`, implicit `T→T?` wrap, `!!` (aborts on null), `?:`
Elvis, and **smart-casts** — `if (x != null)` / guard-clause narrowing with `mut` reassignment
invalidation, all sound via the definite-assignment snapshot machinery. `?.` parsed but deferred.
Nullable references reuse the `ptr` representation (no codegen change). See the "Nullable
references" invariant; `tests/nullable_tests.cpp` `[nullable]`, `e2e/nullable_test.gg`.) Prior
baseline was **809 test cases / 1565 assertions** (as of **named & mixed arguments** — `f(x: 1, y: 2)` / a
positional prefix then named; names participate in overload selection; **defaults may now sit on
any parameter** (contiguous-trailing rule removed) and a named call can skip a defaulted middle
param; args evaluate in source order then reorder into parameter order; a semantic→codegen
permutation handoff (`SemanticResult::callArgOrder`) keeps codegen positional calls unchanged. See
the "Default parameter values + named arguments" invariant. `tests/named_args_tests.cpp` `[named]`,
`e2e/named_args_test.gg`.) Prior baseline was **779 test cases / 1516 assertions** (the **borrow
syntax migrated from the `ref` keyword to a postfix `*`** — `ref T`/`mut ref T` are now `T*`/`mut T*`,
mirroring the owning `T&`; the `ref` keyword is removed. Semantics are identical (same
`TypeKind::Reference` + `borrow`, same internal `"ref:T"` token); only the parser front-end and
user-facing spelling changed. See the "`T*` — non-owning borrow" invariant. All `.gg`/test sources
migrated; `[ref]` group still green.) The count previously reached this figure with `private` on
free functions — `fn private name(...)` is file-local, warning-level: a cross-file call warns, a
same-file call is silent, and method/impl bodies never warn; `tests/semantic_tests.cpp` `[private]`.
Prior baseline was **774 test cases / 1501 assertions** (as of pinning the
const/deferred-init definite-assignment edge cases — a const assigned by BOTH branches of an if/else is thereafter initialized so a later
write is a reassignment error; a one-branch assignment leaves it re-definable; a loop-body
assignment never establishes post-loop initialization; nested if/else assigning on every path
initializes; definite-assignment applies to `mut` too. Behavioural docs unchanged; see
`tests/const_tests.cpp` `[const]` + the existing `[uninit]` cases in `tests/semantic_tests.cpp`.)
Prior baseline was **769 test cases / 1493 assertions** (as of the stdlib `String` revision + growable `Array<T>` —
`String` now has a Java-like interface (`length`/`byteLength`/`isEmpty`/`isBlank`/`charAt`/`byteAt`/
`equals`/`compareTo`/`startsWith`/`endsWith`/`contains`/`indexOf` + `Eq`/`Ord` impls), with the
`ptr` buffer + `size` fields and the `cStr()`/`byteIndexOf()` helpers marked `private` so no `ptr`
appears in the public API (the literal constructor `String(ptr)` is the one necessary exception);
`stdlib/array.gg` is a `std::vector`-like `Array<T>` (heap buffer, geometric growth, `push`/`pop`/
`size`/`capacity`/`reserve`/`clear`, `[]` via the `Index` trait, `get(i) -> ref T`), primitive
elements only in v1 (object elements need per-element clone/dtor or retain/release — deferred, and
`Array<SomeClass>` fails cleanly at the `push(T)` object-value-param check); `e2e/string_test.gg`,
`e2e/array_test.gg`, `tests/stdlib_tests.cpp` `[stdlib]`). Prior baseline was
**767 test cases / 1486 assertions** (as of C++-style brace construction — `Type{args}` is an
alternate delimiter for a positional constructor call; typed `Point{...}`/`new Point{...}` reuse the
constructor CallExpr/NewExpr path, untyped `{...}` is a new `BraceInitExpr` whose class is deduced
from the expected type (constructor arg / var init / return), resolved via `resolveOverload`'s
per-arg expected-type injection (unambiguous enclosing call only); enums non-constructable; a
no-ctor `C c{}`/`C c()` zero-inits instead of calling a missing ctor; `tests/brace_init_tests.cpp`
`[brace]`, `e2e/brace_init_test.gg`). Prior baseline was
**752 test cases / 1455 assertions** (as of primitive-borrow edge-case hardening — passing a
primitive lvalue to a `ref <primitive>` **parameter** now takes its address in `buildArgString`
(`genBorrowSource`); passing a **temporary** to such a parameter is rejected (lvalue check in
`resolveOverload`); `+=`/`++`/`--` **through** a primitive borrow are rejected with a clear message
(write `r = r + 1`) instead of miscompiling; `genBinary` decays primitive-borrow operands so a
`ref i32`-returning call used in arithmetic isn't misrouted. Prior baseline was
**745 test cases / 1445 assertions** (as of store-through-a-returned-reference — `v.at(i) = x`, a
new `RefStoreExpr` node: the parser builds it when the `=` LHS is not a name/index/member,
`analyzeRefStore`/`genRefStore` store a value into a primitive-borrow referent; class-reference and
non-reference targets are rejected. Prior baseline was
**743 test cases / 1442 assertions** (as of `ref T` borrows of **primitives** — `ref i32` is an
lvalue reference like C++'s `int&`: `makePrimitiveBorrow` (Reference+borrow+elementKind), reads
auto-deref / writes store-through / borrow-creation & ref-return take the address via
`genBorrowSource`; a borrow-returning call is no longer a refcount temp; `Array<i32>::get -> ref T`
now works. Prior baseline was
**739 test cases / 1437 assertions** (as of `ref T` non-owning borrows — prefix `ref`/`mut ref`,
`TypeKind::Reference` + `borrow` flag, synthesized `"ref:Class"`, coercions into a borrow only,
no refcount traffic, rejected as a field, escape-checked; a shared `ref` is read-only while
`mut ref` may mutate; method dispatch and `ref`→`ref` passing work; `ref Self` resolves in an
`impl`; `tests/ref_tests.cpp` `[ref]`, `e2e/ref_test.gg`). Prior baseline was
**724 test cases / 1417 assertions** (as of escape analysis v1 — rejects passing a stack value
object to a `Class&` parameter that returns/ref-field-stores it; gated on Object→Reference borrows
so no false positives; `SemanticAnalyzer_Escape.cpp`, `tests/escape_tests.cpp` covers ctor stores via
`new`/stack, local-alias escape, control-flow nesting, per-parameter granularity, and the
local-shadows-field guard). Prior baseline was
**704 / 1392** — generic impls — `impl<T> Trait for Class<T>` captured
as an impl template and monomorphized alongside each `Class<args>` instantiation; `tests/trait_tests.cpp`
`[generic][impl]`, `e2e/generic_impl_test.gg`). Prior baseline was **700 / 1386** — the stdlib `String` class + CRT-layout test update —
`stdlib/string.gg` is now a heap-owned UTF-8 `String` (bytes + byte-length; `byteLength`/`length`
codepoint count/`byteAt`/`charAt` UTF-8 decode to `char`/`cStr`, `Eq` impl for `==`); the old flat
CRT binding files moved to `stdlib/crt/c*.gg` and `tests/stdlib_tests.cpp` follows the new paths;
`e2e/string_test.gg` covers runtime behavior). Prior baseline was **696 / 1375** as of DWARF debug info — `--debug`/`-g`: Tier 1 line-level
`!DICompileUnit`/`!DIFile`/`!DISubprogram`/`!DILocation`, Tier 2 `!DIBasicType`/`!DICompositeType`
with member offsets/`!DILocalVariable` + `#dbg_declare` records; off by default, IR byte-identical
when off; `tests/debug_tests.cpp`). Prior baseline was **687 / 1350** as of lambdas & callable
objects — the `Call` trait with
signature-carrying `Call(P…)->R` bounds canonicalized to user traits, `obj(args)` desugar,
parser-desugared value-object lambdas with by-value capture of locals/params/fields, single-param
inference; Java-style `switch` — statement + expression, arrow
form, `Eq`-trait/default label comparison via factored `classifyEquality`/`emitEquality`, enum
exhaustiveness, duplicate-label detection, `yield`; C++-style default parameter values —
contiguous-trailing rule, call-site fill, arity-range overload resolution; generic-body checking
against trait bounds
(definition-time, bounded-only, new `TypeKind::TypeParam`); default class `==`/`!=` — reference
address identity +
value-object memberwise structural equality; value-object fields / embedding — recursive
deep-copy clone + recursive dtor + transitive `needsDtor` + cycle rejection; the refcount
ownership fixes; value-object → reference borrow; object-value operator returns; unified
signature order `fn name(params) -> RetType [alias]`).

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
