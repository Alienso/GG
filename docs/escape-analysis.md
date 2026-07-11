# Design note: Escape analysis (reject-on-escape) + threading readiness

**Status:** v1 **implemented** in `source/semantic/SemanticAnalyzer_Escape.cpp` (summaries computed
at collection time; call-site check in `resolveOverload` / `analyzeMethodCall`; tests in
`tests/escape_tests.cpp`). Scope below reflects what shipped; §6 (threading) and the interprocedural
propagation in §4 remain future work.

**v1 as shipped:** intraprocedural, optimistic on calls. A reference parameter escapes when the body
**returns** it or **stores it into a reference field** (`this.f = q` / implicit `f = q`; value-object
field stores are deep copies, not escapes). The call-site check fires only on an `Object` argument
coerced to a `Reference` parameter. Not yet done: following a param forwarded into another call
(interprocedural fixpoint), treating `a[i] = q` as an escape, and the threading extensions. Also
note `thisEscapes` is currently unreachable — `this` is Object-typed and Object→Reference is already
blocked in return/store positions — but the machinery is in place for a future where it isn't.

## 1. Problem

GG has two object kinds:

- **Value objects** — stack/inline structs, no refcount header, destroyed at scope exit.
- **Heap references** (`Class&`) — intrusively refcounted (`gg_alloc` header at `body−8`,
  `gg_retain`/`gg_release`), shared ownership.

The type system already blocks the *binding-level* ways a reference to a stack value object could
outlive it — `canImplicitlyCast` reports `None` for `Object → Reference`, so all of these are
already rejected:

```gg
Point p;                 // value object (stack)
Point& r = p;            // ERROR — can't bind a value object to an owning reference
fn f() -> Point& { Point p; return p; }   // ERROR — can't return it as a reference
```

**The one remaining hole** (documented in the memory model as the "value-object → reference borrow
footgun") is *argument position*: passing a value object where a `Class&` parameter is expected is
allowed as a **borrow** (no copy, no refcount change). That is safe for a pure-borrow callee
(operators, readers) but corrupts memory if the callee **retains / stores / returns** the
parameter, because a stack object has no refcount header to retain and no lifetime beyond the frame.

```gg
fn keep(Point& q) { this.saved = q; }    // stores q → the borrow escapes
fn main() -> i32 {
    Point p;
    keep(p);            // TODAY: silent corruption (q retained into a field, p is stack)
    return 0;
}
```

## 2. Design: escape analysis with a hard error (no heap promotion)

Compute, per function, whether each **reference parameter escapes**. At a call site where an
argument is a **value object being borrowed as `Class&`**, require the callee's parameter to be
**non-escaping** — otherwise a **compile error**.

Deliberately **no heap promotion.** If the user wants the object to escape, they make it own its
storage explicitly:

```gg
Point p;
keep(p);                 // ERROR: parameter 'q' of 'keep' escapes;
                         //   copy it (`Point c = p; keep(c)` won't help — still a borrow),
                         //   or allocate on the heap: `Point& hp = new Point(...); keep(hp);`
```

The error message names the escaping parameter and points to the two fixes: **own it on the heap
(`new`)**, or restructure so the callee only borrows.

This keeps the ergonomic, zero-copy borrows that are safe today (operator operands, readers) and
rejects **only** the escaping ones — which is the whole reason to do the analysis instead of a
blanket ban on value→`&` borrows.

## 3. What counts as "escape" for a reference parameter `p`

`p` escapes if its value flows into any construct that can outlive the call or alias it beyond the
callee's frame. These correspond 1:1 to the sites where codegen already emits a retain/persist:

- **Returned** — `return p;` (or returning something aliasing `p`); the `+1` return convention
  retains.
- **Stored** into a field / global / static / array element (field store retains).
- **Retained** explicitly / bound to a longer-lived reference that is itself stored or returned.
- **Passed onward** as an argument to another call at a position that (per that callee's summary)
  escapes — this is the recursive case.

Does **not** escape:

- Read-only use (`p.field`, `p.method()` on a non-storing method), arithmetic, comparisons.
- Bound to a **local** reference that never itself escapes (the local dies with the frame).
- Passed to a callee at a **non-escaping** parameter.

## 4. Algorithm

A new semantic pass, run **after monomorphization + overload resolution** (so the call graph is
concrete and `resolvedCallee` gives exact targets).

1. **Local pass, per function.** Walk the body; for each reference parameter compute a local fact:
   - `Escapes` if it hits a return/store/retain site directly;
   - a set of *constraints* `escapes(p) ⊇ escapes(callee, j)` for each place `p` is forwarded to
     argument position `j` of a known callee.
2. **Fixpoint over the call graph.** Initialise every parameter `NoEscape`; worklist-propagate:
   a parameter becomes `Escapes` when any of its constraints fires. Iterate to stability
   (handles recursion / mutual recursion).
3. **Call-site check.** For every argument that is a value object coerced to `Class&`
   (already detected by `canPassArgument`), error if the resolved callee's parameter `Escapes`.

Conservative fallbacks (treat the parameter as escaping):

- **Indirect calls** — lambda / `Call`-trait dispatch (callee not statically known).
- Any call whose target can't be resolved to a concrete function.

Exempt:

- **`extern`** functions take `ptr`, not `Class&`, so a value-object→`Class&` borrow can't reach
  them — no summary needed.

Cost/shape: one escape bit per reference parameter + a worklist fixpoint. No lifetimes, no regions,
no per-variable liveness ranges. Comparable in effort to the const/mut or generic-bounds passes
(a few hundred lines + tests) — far cheaper than a borrow checker or generational references.

## 5. What it solves / does not solve

| Issue | Result |
|---|---|
| Value-object → `Class&` borrow footgun (callee retains a stack object) | **Solved** — compile error, while keeping safe borrows. |
| Heap-object use-after-free | Already handled — a `Class&` co-owns via refcount; can't dangle while held. |
| `Array<T>`: contiguous values **and** long-lived references to elements | **Not solved.** `get(i) -> T&` returns an interior pointer → escape → rejected. You are steered to `Array<Class&>` (store references) or copy elements out. Escape analysis *forbids* the unsafe form; it does not *enable* the desired one. |

Delivering "usable references into inline value storage" needs a different tool — **generational
references** (runtime-checked, faults instead of corrupts) or scoped/borrow-checked references.
Escape analysis makes the **owning** side sound; those would make a **non-owning** side sound. They
are complementary and can be added later independently.

## 6. Threading readiness

If threading is ever added, escape analysis is the mechanism that keeps it both **safe** and
**cheap** — but it gains a new escape flavour and one call distinction.

### 6.1 A third escape category: thread-escape

Classic taxonomy: **NoEscape** (method-local) ⊂ **ArgEscape** (into a synchronous callee, same
thread) ⊂ **GlobalEscape** (heap / static / **another thread**). "Escapes to another thread" is the
strongest form: a stack value object handed to another thread is both a use-after-free (the
spawning frame returns and destroys it) *and* a data race.

### 6.2 What the analysis adds

- **Thread-spawn is an escape site.** A reference argument to `spawn(fn, args…)` (or a channel
  send / future / async task) thread-escapes → treat as escaping, exactly like a store/return.
- **Async calls break the "borrow bounded by the call" assumption.** The safe-borrow rule relies on
  *synchronous* execution (callee finishes inside the frame). A thread body runs concurrently and
  outlives the spawn statement, so **every** reference argument to an async call escapes, even one
  the callee only reads. → classify calls **sync vs async**; async ⇒ all reference args escape.
- **Reference-capturing closures that run elsewhere escape.** A lambda passed to a thread carries
  its retained reference captures into the other thread (captured *primitives* are copies and are
  fine).

Mechanically this is a small delta: a few new escape sources + a sync/async classification. The
per-parameter-bit + fixpoint machinery is unchanged. Optionally track a distinct **thread-escape
bit** (not just method-escape) to drive the two uses below.

### 6.3 Payoff — it rescues the non-atomic refcount

The refcount is non-atomic (see the memory-model note in CLAUDE.md), which is unsound the moment
objects are shared across threads. Escape analysis lets you avoid making *everything* atomic:

- **Thread-local objects** (the vast majority) → keep the cheap non-atomic `add`/`sub`.
- **Thread-escaping objects only** → emit atomic `atomicrmw` retain/release + an `acquire` fence on
  the count-reaches-0 path before dtor/free.

This is biased/deferred reference counting: you pay for atomics exactly where sharing happens.

### 6.4 Caveat — it identifies sharing, it doesn't make it safe

Escape analysis tells you *which* objects cross the thread boundary; it does not prevent **data
races** on their mutable contents (atomic counts protect the count, not the fields). A discipline is
still required at the boundary.

### 6.5 Recommended pairing: move / ownership-transfer threads

The cleanest model, and the one that reuses this exact machinery (Rust `Send` + `move` closures,
Pony/Erlang message passing):

- `spawn` **consumes (moves)** its arguments — ownership transfers to the new thread; the sender may
  not touch them afterwards.
- Escape analysis enforces **no use-after-move** in the sender.
- Then an object is owned by exactly one thread at a time → **no sharing → no races → the non-atomic
  refcount stays sound.**

This dovetails with the single-ownership / `owning` discussion: "move into a thread" is just
ownership transfer across the thread boundary. The alternative (shared state + locks/atomics) is
more flexible but needs the atomic-refcount work above *and* a full memory model.

## 7. Open decisions

1. **Does a value→`Class&` borrow to a non-escaping parameter stay allowed** (keep today's
   ergonomics), or do we ban all such borrows for simplicity? (Recommendation: keep — that's the
   point of the analysis.)
2. **Diagnostic quality**: report the escape *path* (`p → stored in Foo.bar`) or just the offending
   parameter? (Start with the parameter + fix hint; add the path later.)
3. **Threading model** (only if/when threads land): move/`Send` (recommended) vs shared+locks.
4. Whether to also compute a **thread-escape bit** now (cheap) to future-proof the summary, even
   before threads exist.

## 8. Pipeline placement

New pass in `SemanticAnalyzer`, after `collectImpls` / `checkGenericBounds` and after
monomorphization (call graph concrete). Consumes `resolvedCallee`; produces a
`functionName → per-parameter escape bit` map; the call-site check runs during / after
`analyzeCallArgs` where the `Object → Reference` borrow is already recognised.
