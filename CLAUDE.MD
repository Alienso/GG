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
| `"ptr<void>"`       | Alias for opaque `ptr` (`TypeKind::Ptr`)  |

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
- **Not gated**: array/`ptr<T>` element writes (`a[i] = v`), `static` fields (stay mutable),
  enum fields (always immutable — parser rejects `mut` on them, as it does on methods).
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

### Default parameter values (C++-style)
A parameter may declare `= expr` (`ParamDecl::defaultValue`, a `unique_ptr<Expr>` — `ParamDecl` is
thus move-only). Omitted **trailing** arguments are filled from the callee's stored default exprs,
evaluated per-call **at the call site**.
- **Contiguous-trailing rule** (parser, `parseParamList(bool allowDefaults)`): once a param has a
  default, every later param must too — else "must have a default value because an earlier
  parameter has one". `extern` rejects defaults (`allowDefaults=false`). Applies to free functions,
  methods, **and** constructors (all route through `parseParamList`).
- **Semantic** (`analyzeParamDefaults`, called from `analyzeFunctionDecl` + the `analyzeClassDecl`
  method loop): each default is analyzed in the **enclosing scope** with `currentClassName` cleared
  and **before params are declared**, so a default may not reference the function's own params /
  `this` / fields; `checkCast` against the param type ("default value of parameter 'x'").
- **Overload resolution with arity ranges** (`resolveOverload`): `numDefaults` (trailing-default
  count) on `ClassInfo::Method` / `FunctionOverload` / `OverloadCand`; a candidate is viable when
  `args.size()` ∈ `[total - numDefaults, total]`. Arity error reads "expects R to N argument(s)".
- **Codegen** (`funcDefaults_`: emitted-callee-name → `vector<const Expr*>`, keyed like
  `funcParamTypes`): `buildArgString(..., defaultsFor(name))` genExprs the stored defaults for
  positions the caller omitted. Threaded through every call site (free/method/static/ctor/new/sret/
  enum-ctor). Works for object-value (sret) returns and object defaults.

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

### SemanticAnalyzer — 3 passes
1. `collectClasses`  — build `classRegistry` (fields, methods, destructor).
2. `collectFunctions` — hoist top-level function signatures into global scope.
3. `analyzeStmt` over every declaration — full type checking.

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
  safe for pure-borrow callees (operators, readers); it is an unchecked footgun only if the
  callee retains/stores/+1-returns the parameter (no lifetime tracking prevents that).
- **Refcount runtime** (emitted verbatim into IR when `new` is used):
  - `gg_alloc(size)` — malloc with 8-byte header, initialises count to 1.
  - `gg_retain(ptr)` — null-safe refcount increment.
  - `gg_release(ptr, dtor)` — null-safe decrement; calls `dtor` + `free` at zero.
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
generic-class arrow methods ride the verbatim class-body capture + re-parse. Using a value-return
call as a nested sub-expression argument is deferred.

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

### Static members (class-level)
Roadmap: (1) **static fields** ✅ done, (2) **static methods** ✅ done, (3) **static local
variables** (C-style) ✅ done, (4) polish (const statics, access enforcement, file-scope
linkage) — **intentionally deferred** (no-op per project decision: no `const` keyword,
private access stays a warning, static fields keep external linkage).
- **Static fields** — `static T name;` or `static T name = const;` inside a class.
  Shared class-level storage, **not** part of the struct layout. Accessed via the
  scope-resolution operator `ClassName::field` **or** through an instance `obj.field`
  (both resolve to the same global). Static fields are mutable (unlike enum fields).
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
**700 test cases / 1386 assertions** (as of the stdlib `String` class + CRT-layout test update —
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
