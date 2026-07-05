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
- **Codegen** adds `funcReturnTypes` (needed so operator calls emit the right return IR, esp.
  `cmp`→`i32`), `genTraitMethodCall` (`CodeGen_Classes.cpp`), and counts impl methods into
  `overloadedBases_`/`funcParamTypes`.

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
trait(s) — user traits **and** built-in operator traits (`Add`/`Ord`/…). Enforced at each
**instantiation site**, not by pre-checking the generic body (the type param never reaches
semantics; bodies may still call any method the concrete type has).
- **Parser**: `GenericTemplate::bounds` (parallel to `typeParams`, `bounds[i]` = trait names).
  Both capture functions call `scanTypeParamList(from, typeParams, bounds)` which parses
  `T [: Trait (+ Trait)*] [, …]` (COLON/PLUS are separators). At monomorphization,
  `runMonomorphization` pushes a `GenericBoundCheck{typeName, traitName, context, line}` (in
  `Ast.h`, on `Program::genericBoundChecks`) per bounded param — `typeName` = the arg's head
  token lexeme (base type, `&` stripped), `context` = the instantiation's mangled name.
- **Semantic**: `checkGenericBounds(program)` runs after `collectImpls` (pass 0d). For each
  obligation: unknown trait (not built-in, not in `traitRegistry`) → error; else the type must
  be in `implementedTraits[typeName]` for that trait (both user + built-in impls populate it).
  Primitives / non-implementing classes → "type 'X' does not satisfy bound 'Trait' …".

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
**597 test cases / 1198 assertions** (as of the unified signature order — `fn name(params) ->
RetType [alias]`, with generalized return aliases).

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
