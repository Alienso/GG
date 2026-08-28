# Design note: Concurrency (`Shared<T>` + `Thread`)

**Status:** Phase 1 **IMPLEMENTED** (`Shared<T>` + `Thread` + `Sendable`/`Shareable` + the boundary
check). This note captures the model, the reasoning, and the phased plan. The authoritative
implementation references are the "`Shared<T>`" and "Threads" invariants in `CLAUDE.md` and the
user-facing §15 of `LANGUAGE.md`; where this note's *sketches* differ from what shipped, the shipped
surface wins. **As-built vs. the §5 sketch below:** `Thread` is spawned through a **static factory
`Thread.create(closure) -> Thread`** that starts the thread immediately (not a `Thread t(closure)` +
separate `start()`), and the compiler's "function-pointer part" is two intrinsics called *inside* that
factory — `__gg_heap_closure(closure)` (heap-copy) and `__gg_trampoline(closure)` (emit the C-ABI
trampoline) — plus lambda-inference for a single-`Call`-bounded generic **static method** so
`Thread.create(() -> {…})` infers the closure class. It is the shared-memory counterpart to the
threading-readiness discussion in `docs/escape-analysis.md` §6 (see §10 below for how the two
reconcile).

**One-line model:** every thread owns a private object graph via the cheap non-atomic `Class&`
(`Rc`); the *only* things that cross a thread boundary are values that are provably safe to share —
carried by a distinct, atomically-refcounted **`Shared<T>`** (`Arc`) type. Sharing is **declared by
the type**, never inferred, which turns a whole-program aliasing question into a local, structural
one.

---

## 1. The problem — what actually breaks under threads

GG today (see the memory-model invariant in CLAUDE.md):

- **value objects** — stack/inline, no refcount header;
- **owning heap references** `Class&` — intrusive **non-atomic** refcount (`gg_alloc` header,
  `gg_retain`/`gg_release`), single-threaded by design;
- **non-owning borrows** `T*` — a plain `ptr`, no refcount;
- **no GC** — reclamation is `free` via the CRT, so misuse is C-level UB.

Split the concurrency hazards by whether they threaten **memory safety** (fatal) or only
**value correctness** (tolerable):

| Hazard | Category |
|---|---|
| Racing **non-atomic refcount** updates → count corrupted → premature free / leak | **fatal** |
| **Rebind double-free** — two threads rebind a shared reference slot; both read the same *old* target and both `release` it (UB **even with atomic counts** — see below) | **fatal** |
| **Reader-vs-rebind UAF** — a reader loads a shared reference field, a writer rebinds+frees it, the reader dereferences freed memory | **fatal** |
| **Dangling borrow** — a `T*` outlives the object another thread freed | **fatal** |
| Racing reads/writes of **scalar/POD fields** → torn/garbage value | **tolerable** (memory-safe) |

The guiding line (set during design): *undefined value reads/writes are acceptable; dangling
references and double-frees are not.* Everything below exists to make the **fatal** rows impossible
by construction while leaving the **tolerable** row alone.

### Why atomic counts alone are not enough

Making `gg_retain`/`gg_release` atomic fixes the **first** row (releasing your own handle is now
correct → object freed exactly once). It does **not** fix the rebind double-free:

```
slot = M_old  (M_old.count == 1, atomic)
T1: retain(A)                 T2: retain(B)
T1: old = read(slot) → M_old  T2: old = read(slot) → M_old   ← both read the same old
T1: store(slot, A)            T2: store(slot, B)
T1: release(M_old) → 1→0 free T2: release(M_old) → 0→-1 → UAF/double-free
```

Every count op was atomic and correct; the bug is that *both threads decided to release M_old*. An
atomic counter cannot prevent a duplicate release. That belongs to the **exclusion** axis (a lock or
atomic-exchange), not the lifetime axis — see §2.

---

## 2. Design overview — two axes, explicit sharing

**Two independent axes** (the `Arc<Mutex<T>>` split):

- **Lifetime axis → `Shared<T>`** — atomic reference counting. Solves the object-death class
  (fatal rows 1 and 4) *completely*. This is the whole of Phase 1.
- **Exclusion axis → `Mutex<T>` / `RwLock<T>`** (Phase 2) — mutual exclusion on *contents*. Solves
  the mutation races (fatal rows 2 and 3), composed as `Shared<Mutex<T>>` / `Shared<RwLock<T>>`,
  and **only needed for mutable shared data**. Immutable shared data needs `Shared<T>` alone.

**Sharing is declared, not inferred.** Because of the boundary rules in §4, the *only* way an object
reaches two threads is through a `Shared<T>` handle. So "is this shared?" ⟺ "is its type
`Shared<T>`?" — a local, syntactic fact. GG never needs whole-program points-to analysis (which its
intraprocedural machinery couldn't do anyway).

**Two refcount disciplines coexist**, as distinct types with distinct codegen:

| Type | Refcount | Reachability |
|---|---|---|
| `Class&` | **non-atomic** `Rc` | thread-local (cheap, unchanged) |
| `Shared<T>` | **atomic** `Arc` | may cross threads |

**Born-shared rule (load-bearing for soundness).** A single intrusive count cannot be safely
touched by both atomic and non-atomic code, so an object commits to one discipline **at creation**
and is never reachable the other way. A `Shared<T>` object is built *inside* the `Shared` (§3) and
never has a raw `Class&` alias. This is what lets atomicity live *entirely inside* `Shared<T>`
instead of being a global change to all refcounting.

---

## 3. `Shared<T>` (compiler-builtin)

**Builtin generic**, like `ptr<T>` — cannot be stdlib, because it needs atomic-op codegen and the
born-shared/`Shareable` checks.

- **Representation:** a pointer to a heap block `{ atomic i64 count; T data }`.
- **Copy** = atomic retain; **scope exit** = atomic release; at 0 → `@T_dtor(data)` + `free`.
- **Construction — build-in-place (decided):** `Shared<Player>(1, 2)` forwards the constructor
  arguments and builds the `Player` *directly inside* the `Shared` block. No raw `Player` alias ever
  exists → born-shared holds by construction. Reuses the variadic-`emplaceBack` machinery.
  - Rationale vs alternatives: `Shared(new Player(...))` would create a momentary raw temp (sound
    only via `claimTemp` proving it unaliased); `new Shared<Player>(...)` reads oddly (Shared is
    itself a handle). Build-in-place is airtight and needs no proof.
- **Access — auto-deref for value-returning reads:** `shared.method()` and `shared.field` work
  directly (the transient borrow can't escape the expression). No closure/`.with` needed for the
  common case.
- **No raw alias leaks out:** a method that returns a *borrow* into the object (`-> Inventory*`)
  yields a borrow tied to a held handle; escape analysis confines it (Phase-2 `.with`/guard APIs, or
  the existing "can't store/return a borrow" rule). Value-returning access is unrestricted.
- **`T` may be** a class or value object (anything `Shareable`, §4). **Not** a borrow/`ptr`; no
  `Shared<Shared<T>>`.

---

## 4. `Sendable` / `Shareable` — auto-derived markers (decided names)

Both are **compiler-derived, never hand-written** (a hand-written `impl` would be an
`unsafe impl Send` soundness hole). The *strategies* (`Shared`, later `Mutex`/`RwLock`, `Channel`,
`Immutable`) are an open set of types; *conformance* is compiler-verified structurally, as a
fixpoint over fields (same shape as `needsDtor`).

**`Sendable(T)`** — safe to hand to / touch from another thread:

| T | Sendable? |
|---|---|
| primitive, `bool`, `char`, `str`, enum | ✅ (immutable / value) |
| value object | ✅ iff every field is Sendable |
| tuple | ✅ iff every element is Sendable |
| `Shared<T>` | ✅ iff `Shareable(T)` |
| `Class&`, `T*`, `ptr`/`ptr<T>` | ❌ (non-atomic `Rc` / raw / borrow — cannot cross) |

**`Shareable(T)`** — safe to *share by reference* (gates `Shared<T>` legality). Phase 1 (immutable):
a `Shared<T>` may have no **mutable reference-like** field. Structural rule per field:

- `mut` scalar / transitively-POD value object → ✅ (races to garbage; memory-safe — the tolerable
  row);
- non-`mut` reference field (`Class&`/`Shared<U>`) → ✅ iff the pointee type is `Shareable` (recurse
  — it is reachable from both threads);
- **`mut` reference-like field** (`mut Class&`, `mut Shared<U>`, `mut ptr`, `mut` value object
  containing a reference) → ❌ in Phase 1 → `Shared<T>` rejected, pointing you to make it immutable
  (or, in Phase 2, wrap it in `Mutex`/`RwLock`).

The recursion through non-`mut` reference fields is what catches a *deep* mutable slot (`Shared<A>`
where `A` holds a `B&` and `B` has a `mut Node& target`): `Shareable(A)` needs `Shareable(B)` needs
`target` synchronized → rejected. Terminates like `needsDtor` (value-embedding cycles already
rejected; reference cycles memoize).

**Boundary / confinement.** The thread body (§5) may only **capture** `Sendable` values, and may
only **touch `Sendable` statics** — a raw `mut static Class&` is not thread-accessible (compile
error). This is a lexical check over the closure's free variables + static references (a closure's
free variables *are* its captures), closing the ambient-statics hole. (Static *initialization* is
unaffected — statics init before `main`, single-threaded.)

---

## 5. `Thread` (stdlib class over a minimal spawn intrinsic)

**Decision: `Thread` is a stdlib class**, not a compiler/language construct — matching the
C++/Rust/Java camp (1:1 OS threads; the thread *object* is library, the *safety* is compiler). The
only reason it is not *pure* stdlib is that GG has no first-class function pointers, and OS
thread-create needs a C-ABI entry pointer — so the actual spawn is one small compiler intrinsic
(exactly as `std::thread` sits on pthreads).

### API (decided: reuse `Call() -> void`; separate `start()`)

```gg
Thread t(() -> {                       // ctor takes F: Call() -> void + Sendable
    manager.player.doStuff();          // captures manager.player (a Shared<>) by value → retained
});
t.start();                             // spawn the OS thread, hand it the closure, begin
t.join();                              // block until completion
```

- **State machine:** created → started → (joined | detached). Double `start()` / `join()`-after-
  detach → runtime error (not statically catchable).
- **Runs once, owns its captures.** The closure is a "once" callable; captures (`Shared` handles)
  are retained into it at construction.
- **Ownership timeline:** the `Thread` object owns the closure between construction and `start()`;
  at `start()` ownership transfers to the running thread, which destroys the closure on completion
  (releasing each `Shared` capture). Construct-without-`start()` → closure destroyed with the
  `Thread`, nothing ran. Drop-without-`join()` → detached; the thread still cleans up its own
  captures.
- **Escaping Sendable closure — the one significant lambda extension.** GG lambdas are currently
  *non-escaping* stack value objects; a thread body must outlive `spawn`, so it is heap-allocated,
  its captures `Sendable`-checked, and released when the thread ends. Same generated `Call`-class as
  today, just heap + Sendable + owned-by-thread.
- Works via the existing generic-ctor + `Call`-bound + lambda-inference path
  (`Thread(F)` where `F: Call() -> void + Sendable`).

### Lowering

- Per closure type `F`, codegen emits a C-ABI trampoline `@__thread_entry$F(arg) { (F*)arg.call();
  release(arg); }` and a recognized intrinsic inside `Thread::start()` lowers to
  `gg_thread_create(closurePtr, @__thread_entry$F)`. (The intrinsic is needed only because stdlib
  code cannot name/address a function — no function pointers.)
- `gg_thread_create` / `gg_thread_join` are **compiler-emitted runtime helpers** hiding the
  pthread-vs-Win32 difference — the same host-split pattern as `gg_stdout` / `gg_stderr`
  (`emitStdioHelpers`). `Thread` `extern`-declares them.
- **Runtime helpers IMPLEMENTED** (`CodeGen::emitThreadRuntime`, `CodeGen_Emit.cpp`): `@gg_thread_create`
  /`@gg_thread_join` with the platform split (Windows `CreateThread`/`WaitForSingleObject`; else
  `pthread_create`/`pthread_join`, `pthread_t` round-tripped through the ptr handle slot), gated on
  `threadsUsed_` (nothing emitted until the Thread lowering sets it — the same gating as the refcount
  runtime). **Still TODO**: the `Thread` builtin (parser reserved name + var-decl; semantic type +
  special-cased construction + `start`/`join`; codegen: heap-allocate the escaping closure, emit the
  per-closure C-ABI trampoline `@__thread_entry$Class` — Windows `i32(ptr)`/Linux `ptr(ptr)` — that
  calls `@Class_call` then `gg_release`s the closure, and lower `start`→`gg_thread_create(trampoline,
  closure)` / `join`→`gg_thread_join(handle)`; a 3-ptr Thread value `{closure, trampoline, handle}`),
  the `Sendable` capture check, and `-lkernel32` in `compile.ps1`.
- **Runtime validated (2026-08)**: hand-written IR probes confirm the OS primitives work with the
  clang64/mingw toolchain, at `-O0`, built into `build/` like every GG exe:
  - **Windows**: a plain `declare ptr @CreateThread(ptr, i64, ptr, ptr, i32, ptr)` + a **direct**
    call (start routine `i32 (ptr)` — WINAPI == default CC on x64), joined with
    `declare i32 @WaitForSingleObject(ptr, i32)` and `INFINITE` = `i32 -1`. Needs `-lkernel32` at
    link (add to `compile.ps1`'s clang args; harmless/always-present). **No `__imp_` indirection
    needed** (the direct call resolves correctly).
  - **Linux**: `pthread_create`/`pthread_join` + `-lpthread`.
  - C11 `<threads.h>` (`thrd_create`) is **absent** from this mingw CRT — do not use it.
  - Red-herring note for whoever debugs this next: an exe run from a deeply-nested *scratch/temp*
    path may fault with `0xC0000005` for reasons unrelated to its code; build/run threading tests
    from `build/` (as GG's pipeline does).

---

## 6. Compiler vs stdlib split

| Piece | Where |
|---|---|
| `Shared<T>` type, atomic retain/release, born-shared/build-in-place, auto-deref | **compiler (builtin)** |
| `Sendable` / `Shareable` derivation + boundary/confinement check | **compiler (semantic pass)** |
| Escaping-Sendable closure (thread-body lambda) | **compiler (lambda-lowering extension)** |
| `__thread_entry$F` trampoline + the `start()` spawn intrinsic | **compiler (codegen)** |
| `gg_thread_create` / `gg_thread_join` platform helpers | **compiler-emitted runtime** (like `gg_stdout`) |
| `Thread` class — ctor, `start()`, `join()`, state machine, handle storage | **stdlib** |
| atomic `gg_retain`/`gg_release` variant | **compiler-emitted runtime** |

---

## 7. Memory ordering

- Atomic retain: relaxed `atomicrmw add`.
- Atomic release: `atomicrmw sub` with **release** ordering; on the count-reaches-0 path, an
  **acquire** fence before `dtor` + `free` (the standard `Arc` pattern).
- Born-shared guarantees no non-atomic path ever touches a `Shared` object's count, so the atomic
  and non-atomic disciplines never mix on one counter.

---

## 8. What it solves / does not solve

| Issue | Phase 1 (`Shared<T>`) |
|---|---|
| Object freed while another thread holds it / non-atomic release race | **Solved** (atomic count → freed exactly once) |
| Dangling **owning** reference across threads | **Solved** (a `Shared` co-owns; can't drop to 0 while held) |
| Passing a raw `Class&`/`T*` into a thread | **Rejected** (`!Sendable`) |
| Rebind double-free / reader-vs-rebind UAF on **mutable** shared state | **Phase 2** — needs `Mutex`/`RwLock`; Phase 1 forbids mutable reference fields in a `Shared<T>` |
| Torn/garbage reads of `mut` **scalar** fields | **Tolerated** (memory-safe, by design) |
| Logical races (lost updates, deadlock, starvation) | **Not a memory-safety concern**; programmer's responsibility even with locks |

---

## 9. Phasing

- **Phase 1 (this note's core):** `Shared<T>` (atomic refcount, immutable sharing) + `Sendable` /
  `Shareable` + boundary check + `Thread` (`start`/`join`) + the escaping-closure extension +
  runtime helpers. A complete, shippable, useful subset (immutable shared data: config, loaded
  assets, read-only entities) with **no locks**.
- **Phase 2:** `Mutex<T>` / `RwLock<T>` as separate composable types; `read`/`write`/`lock` scoped
  (closure) access APIs backed by real OS locks; `.with` + guard-scoped-borrow confinement for
  borrow extraction; mutable shared state.
- **Deferred / out of scope (may never be needed):** move semantics (share-nothing transfer without
  refcounting), channels / message passing, structured concurrency (scoped threads), first-class
  function pointers (would make `Thread` *pure* stdlib + enable general FFI callbacks), green
  threads / `async` (would require a scheduler runtime — against GG's thin, no-GC character).

Suggested first implementation slice: **`Shared<T>` atomic type without threads yet** — provable in
isolation (IR shows atomic retain/release, build-in-place construction, `Shareable` gating) before
`Thread` and the escaping closure are wired up.

---

## 10. Relationship to `docs/escape-analysis.md` §6

That note sketched a *different* threading route: infer a **thread-escape** bit via (interprocedural)
escape analysis, keep most objects on the cheap non-atomic count, and make **only** thread-escaping
objects atomic — pairing it with **move/`Send`** (share-nothing) as the recommended model.

This design deliberately takes the **explicit, type-driven** route instead:

- Atomicity is carried by a distinct **`Shared<T>` type** (born-shared), not inferred per object. GG's
  escape analysis is **intraprocedural**, so a sound whole-program thread-escape inference isn't
  available; declaring sharing in the type sidesteps it entirely.
- **Move semantics is deferred**, not required — `Shared<T>` covers the shared-memory case now; the
  share-nothing/move path from escape-analysis §6.5 remains a possible future addition.
- Escape analysis is still used, for the pieces it's good at: the value-object→`Class&` borrow
  footgun (already shipped) and **borrow-extraction confinement** out of a `Shared`/lock guard
  (Phase 2 `.with`).

The two notes are consistent: escape-analysis §6 is the "make the existing count selectively atomic
via inference + move" vision; this note is the "explicit `Shared<T>` type" realization chosen for
Phase 1 because it needs no global analysis and reuses existing machinery.

---

## 11. Decisions log & open items

**Decided:**
- `Shared<T>` construction: **build-in-place** `Shared<Player>(1, 2)`.
- `Thread` start: **separate `start()`** (construct → start → join).
- Marker names: **`Sendable` / `Shareable`**.
- Thread body type: **reuse `Call() -> void`** (lambda sugar via the existing desugar).
- `Thread`: **stdlib class** over a minimal compiler spawn intrinsic + `gg_thread_create`/`join`
  helpers; `Shared<T>`: **compiler builtin**.
- **No first-class function pointers** (so `Thread` is thin-stdlib-over-intrinsic, not pure stdlib).
- **`Shared<T>` representation:** a `bool shared` flag on `TypeKind::Reference` (mirrors the existing
  `borrow` flag), owning + atomic-refcounted; synthesized token `"shared:Class"` (mirrors `"ref:Class"`).
  Reuses the reference machinery (member access / dispatch / auto-deref); new logic only at
  refcount / construction / cast / marker sites. `shared` and `borrow` are mutually exclusive in Phase 1.

**Defaults assumed (revisit if wrong):**
- `main` returning **terminates the process**; detached (unjoined) threads are killed (no implicit
  join-all).
- `Shared<T>` wraps a class or value object; not a borrow/`ptr`; no `Shared<Shared<T>>`.
- The `Thread` handle itself is **not `Sendable`** in v1 (join it from the thread that started it).

**Phase-1 type-plumbing limitations (cleanly rejected, never miscompiled):**
- `Shared<T>` wraps a **class** (value object / class); `Shared<primitive>` / `Shared<unknown>` are
  errors.
- **No `Shared<Shared<T>>`** (nested) and **no `Shared<T>` as a generic type argument**
  (`Array<Shared<T>>` — a container of shared handles) yet; the latter needs a symbol-safe mangling
  of the shared element (reconciling the internal `"shared:Class"` decode spelling with the
  `"Class.shared"` mangle spelling). Both are clean parse errors today.
- **No `Shared<T>` tuple elements** (a tuple is a value object; `Shared` is reference-like) —
  rejected alongside `&`/`*`/`?`/`ptr` elements.
- `Shared<T>?` (nullable handle) **is** allowed (shares the plain-pointer representation).

**Open:**
- Do we ever want first-class **function pointers**? (→ pure-stdlib `Thread` + general FFI callbacks.)
- Lifting the generic-argument limitation (containers of shared handles) — the main ergonomic gap.
- Phase-2 `Mutex`/`RwLock` naming and scoped-access API shape.
- Diagnostic quality for `Sendable`/`Shareable` failures (name the offending field + fix hint;
  optionally the reachability path).

---

## 12. Pipeline / implementation placement

- **Parser:** `Shared<T>` synthesized token (reuse the `ptr<T>` path); `Thread`/lambda surface is
  ordinary (the escaping-closure change is in lambda *lowering*, not parsing).
- **Semantic:** `Sendable` / `Shareable` derivation as a fixpoint pass (after `collectClasses`,
  alongside `checkGenericBounds`); the capture/statics confinement check where captures are analyzed;
  `Shared<T>` legality (`Shareable`) at the construction site.
- **Codegen:** atomic retain/release for `Shared`; build-in-place construction (reuse
  `emitObjectDirectInit` / the emplace path); the `@__thread_entry$F` trampoline + `start()` spawn
  intrinsic; `gg_thread_create`/`gg_thread_join` emission (platform split like `emitStdioHelpers`);
  the escaping-closure heap allocation + release-on-completion.
- **Runtime (emitted):** atomic `gg_retain`/`gg_release`; `gg_thread_create`/`gg_thread_join`.
