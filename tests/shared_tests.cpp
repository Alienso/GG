#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// Shared<T> — the atomically-refcounted, born-shared cross-thread ownership handle.
//
// Phase 1, slice 1: TYPE PLUMBING only (parse / resolve / cast rules). Construction, atomic
// codegen, and threads come in later slices. See docs/concurrency.md.
//
// Representation: a `bool shared` flag on TypeKind::Reference (mirrors `borrow`); synthesized token
// "shared:Class". `Shared<T>` wraps a class; it does not coerce to/from a plain Class& / value
// Object / borrow (born-shared + no-leak).
// ============================================================

// ------------------------------------------------------------
// Resolution — Shared<Class> is a valid type
// ------------------------------------------------------------

TEST_CASE("Shared - Shared<Class> resolves as a parameter type", "[shared][semantic]") {
    auto result = analyzeString(R"(
        class Player { mut i32 hp; Player(i32 h) { hp = h; } }
        fn takes(Shared<Player> p) -> i32 { return 0; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Shared - Shared<Class> resolves as a field type", "[shared][semantic]") {
    auto result = analyzeString(R"(
        class Player { mut i32 hp; Player(i32 h) { hp = h; } }
        class Holder { mut Shared<Player> p; Holder(Shared<Player> x) { p = x; } }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Shared - Shared<Class> resolves as a return type", "[shared][semantic]") {
    auto result = analyzeString(R"(
        class Player { mut i32 hp; Player(i32 h) { hp = h; } }
        fn makeShared(Shared<Player> p) -> Shared<Player> { return p; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE_FALSE(result.hadError);
}

// ------------------------------------------------------------
// Rejection — Shared<T> wraps a class
// ------------------------------------------------------------

TEST_CASE("Shared - Shared<primitive> is rejected", "[shared][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn takes(Shared<i32> p) -> i32 { return 0; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("wraps a class"));
}

TEST_CASE("Shared - Shared<unknown> is rejected", "[shared][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn takes(Shared<Nonexistent> p) -> i32 { return 0; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(result.hadError);
}

// ------------------------------------------------------------
// Born-shared / no-leak cast rules (via Type-level checks)
// ------------------------------------------------------------

TEST_CASE("Shared - born-shared cast rules", "[shared][type]") {
    Type sharedPlayer = makeSharedType("Player");
    Type ownRef       = makeReferenceType("Player");
    Type valObj       = makeObjectType("Player");
    Type borrow       = makeBorrowType("Player");

    // Shared<Player> -> Shared<Player> is a retaining copy (Silent).
    REQUIRE(canImplicitlyCast(sharedPlayer, sharedPlayer) == CastResult::Silent);

    // The ONLY forbidden direction out of a Shared is an OWNING Class& — that would create a
    // NON-atomic co-owner of an atomic object (the no-leak / atomicity violation).
    REQUIRE(canImplicitlyCast(sharedPlayer, ownRef) == CastResult::None);
    // A non-owning borrow (Player*) and a value copy (deref+clone) ARE allowed — safe views/copies,
    // exactly like Class& -> Class* / Class& -> Object.
    REQUIRE(canImplicitlyCast(sharedPlayer, borrow) == CastResult::Silent);
    REQUIRE(canImplicitlyCast(sharedPlayer, valObj) == CastResult::Silent);
    REQUIRE(canPassArgument(sharedPlayer, borrow)   == CastResult::Silent);

    // No coercion INTO a Shared handle (born-shared: no pre-existing alias may be promoted).
    REQUIRE(canImplicitlyCast(ownRef, sharedPlayer) == CastResult::None);
    REQUIRE(canImplicitlyCast(valObj, sharedPlayer) == CastResult::None);
    // ...not even in argument position (unlike a plain Object -> Class& borrow).
    REQUIRE(canPassArgument(valObj, sharedPlayer) == CastResult::None);
    // A plain value-object -> Class& borrow is still allowed (control: shared didn't break it).
    REQUIRE(canPassArgument(valObj, ownRef) == CastResult::Silent);
}

// ------------------------------------------------------------
// Borrowing through / copying out of a Shared
// ------------------------------------------------------------

TEST_CASE("Shared - a Shared can be passed to a borrow (Class*) reader", "[shared][semantic]") {
    auto result = analyzeString(R"(
        class Player { mut i32 hp; Player(i32 h) { hp = h; } fn get() -> i32 { return hp; } }
        fn reader(Player* p) -> i32 { return p.get(); }   // non-owning borrow, no ownership taken
        fn main() -> i32 {
            Shared<Player> s = Shared<Player>(42);
            return reader(s);                              // Shared -> Player* borrow (safe view)
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Shared - a Shared can be copied out to a value (deref + clone)", "[shared][semantic]") {
    auto result = analyzeString(R"(
        class Player { mut i32 hp; Player(i32 h) { hp = h; } }
        fn main() -> i32 {
            Shared<Player> s = Shared<Player>(5);
            Player v = s;               // Shared -> value Object: independent clone (like Class& -> Object)
            return v.hp;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Shared - passing a Shared to an OWNING Class& is rejected", "[shared][semantic]") {
    // The dangerous direction: an owning Class& is NON-atomically refcounted, so co-owning an
    // atomic object through it would corrupt the count. Must be a clean error.
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Player { mut i32 hp; Player(i32 h) { hp = h; } }
        fn owns(Player& p) -> i32 { return p.hp; }
        fn main() -> i32 {
            Shared<Player> s = Shared<Player>(1);
            return owns(s);
        }
    )");
    REQUIRE(result.hadError);
}

// ------------------------------------------------------------
// Edge cases
// ------------------------------------------------------------

TEST_CASE("Shared - a Shared handle compares against null", "[shared][semantic]") {
    auto result = analyzeString(R"(
        class Player { mut i32 hp; Player(i32 h) { hp = h; } }
        fn main() -> i32 {
            Shared<Player>? s = null;
            if (s == null) { return 0; }
            return 1;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Shared - nullable Shared<Class>? resolves", "[shared][semantic]") {
    // A Shared handle may be nullable (like Class&?) — it shares the plain-pointer representation.
    auto result = analyzeString(R"(
        class Player { mut i32 hp; Player(i32 h) { hp = h; } }
        fn takes(Shared<Player>? p) -> i32 { return 0; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Shared - nested Shared<Shared<Class>> is rejected", "[shared]") {
    // No Shared<Shared<T>> in Phase 1. Rejected at PARSE time (the inner `Shared` isn't a class
    // template, so the type-arg reader can't consume `Shared<Player>`), which parseString swallows
    // into an empty Program — so assert via stderr, not SemanticResult::hadError.
    StderrCapture cap;
    parseString(R"(
        class Player { mut i32 hp; Player(i32 h) { hp = h; } }
        fn takes(Shared<Shared<Player>> p) -> i32 { return 0; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE_FALSE(cap.str().empty());   // some diagnostic was reported (rejected)
}

TEST_CASE("Shared - a Shared element in a tuple type is rejected", "[shared][parser]") {
    // Tuple elements must be value/primitive in v1; a Shared handle is reference-like (and its
    // synthesized lexeme contains ':', not a valid identifier char). Parse-time CompileError →
    // parseString swallows it, so assert via stderr.
    StderrCapture cap;
    parseString(R"(
        class Player { mut i32 hp; Player(i32 h) { hp = h; } }
        fn takes((Shared<Player>, i32)* pair) -> i32 { return 0; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(cap.contains("tuple elements must be value or primitive"));
}

TEST_CASE("Shared - Shared<Class> is recognized as a local var-decl type", "[shared][parser]") {
    // Exercises typeSpanAt: `Shared<Player> p;` inside a body must parse as a var declaration, not be
    // misread as a comparison. (It has no initializer and Shared isn't constructible yet, so the only
    // requirement here is that it parses and analyzes to a *clean* result — no crash / misparse.)
    auto prog = parseString(R"(
        class Player { mut i32 hp; Player(i32 h) { hp = h; } }
        fn f(Shared<Player> src) -> i32 {
            Shared<Player> p = src;   // copy = retain
            return 0;
        }
        fn main() -> i32 { return 0; }
    )");
    // A successful parse yields a non-empty program (parseString returns an empty Program on a
    // CompileError). The declarations include Player, f, and main.
    REQUIRE(prog.declarations.size() >= 3);
}

TEST_CASE("Shared - copy of a Shared binding type-checks (retain)", "[shared][semantic]") {
    auto result = analyzeString(R"(
        class Player { mut i32 hp; Player(i32 h) { hp = h; } }
        fn f(Shared<Player> src) -> i32 {
            Shared<Player> p = src;   // Shared -> Shared, same class: a retaining copy (Silent)
            return 0;
        }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Shared - Shared<T> as a generic type argument is a clean Phase-1 error", "[shared][parser]") {
    // A container of shared handles (`Box<Shared<Player>>` / `Array<Shared<T>>`) is deferred — it
    // needs a symbol-safe mangling of the shared element. Parse-time error (swallowed by parseString
    // into an empty Program), so assert via stderr. The message is explicit, not the cryptic
    // "expected '>'".
    StderrCapture cap;
    parseString(R"(
        class Player { mut i32 hp; Player(i32 h) { hp = h; } }
        class Box<T> { mut T v; Box(T x) { v = x; } }
        fn takes(Box<Shared<Player>>* b) -> i32 { return 0; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(cap.contains("not yet supported as a generic type argument"));
}

TEST_CASE("Shared - nullable Shared type-level mangling", "[shared][type]") {
    Type s = makeNullable(makeSharedType("Player"));
    REQUIRE(typeName(s) == "Shared<Player>?");
    REQUIRE(mangleType(s) == "Player.shared.opt");   // '?' never reaches a symbol name
}

// ------------------------------------------------------------
// Shareable gate — Shared<T> requires T to be safe to share (Phase 1)
// ------------------------------------------------------------

TEST_CASE("Shared - a class with only POD fields is Shareable", "[shared][semantic]") {
    auto result = analyzeString(R"(
        class Vec3 { mut f64 x; mut f64 y; mut f64 z; Vec3(f64 a) { x = a; y = a; z = a; } }
        fn main() -> i32 { Shared<Vec3> s = Shared<Vec3>(1.0); return 0; }   // mut scalar fields OK
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Shared - a mutable reference field makes T non-Shareable", "[shared][semantic]") {
    // A mut owning-reference field in a shared type is the #2 rebind double-free hazard → rejected.
    // `next` is nullable so it's exempt from the constructor-must-initialize check (isolating the
    // Shareable gate as the sole error).
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Node { mut i32 v; mut Node&? next; Node(i32 x) { v = x; } }
        fn main() -> i32 { Shared<Node> s = Shared<Node>(1); return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("mutable reference"));
}

TEST_CASE("Shared - a non-mut reference field to a Shareable pointee is allowed", "[shared][semantic]") {
    auto result = analyzeString(R"(
        class Leaf { mut i32 v; Leaf(i32 x) { v = x; } }
        class Holder { Leaf& leaf; Holder(Leaf& l) { leaf = l; } }   // const ref field
        fn main() -> i32 {
            Leaf& l = new Leaf(1);
            Shared<Holder> s = Shared<Holder>(l);
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Shared - a raw ptr field makes T non-Shareable", "[shared][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        extern malloc(u64 sz) -> ptr;
        class Buf { mut ptr<i32> data; Buf() { data = malloc(16); } }
        fn main() -> i32 { Shared<Buf> s = Shared<Buf>(); return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("raw pointer"));
}

TEST_CASE("Shared - a mut scalar field is fine, a mut Shared field is not", "[shared][semantic]") {
    // mut scalar: races to garbage but memory-safe → allowed.
    auto ok = analyzeString(R"(
        class C { mut i32 n; C(i32 x) { n = x; } }
        fn main() -> i32 { Shared<C> s = Shared<C>(1); return 0; }
    )");
    REQUIRE_FALSE(ok.hadError);
    // mut Shared field (a mutable reference-like slot) in a shared type → rejected.
    StderrCapture cap;
    auto bad = analyzeString(R"(
        class Player { mut i32 hp; Player(i32 h) { hp = h; } }
        class Team { mut Shared<Player> captain; Team(Shared<Player> p) { captain = p; } }
        fn main() -> i32 { Shared<Team> s = Shared<Team>(Shared<Player>(1)); return 0; }
    )");
    REQUIRE(bad.hadError);
}

TEST_CASE("Shared - a self-referential class with a non-mut ref is Shareable (cycle)", "[shared][semantic]") {
    // The Shareable fixpoint must terminate on a reference cycle and accept it (a non-mut link means
    // an immutable structure — safe to share read-only).
    auto result = analyzeString(R"(
        class Node { mut i32 v; Node&? next; Node(i32 x) { v = x; } }
        fn main() -> i32 { Shared<Node> s = Shared<Node>(1); return 0; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Shared - enum and str fields are Shareable (immutable), even mut", "[shared][semantic]") {
    auto result = analyzeString(R"(
        enum Color { RED, GREEN, BLUE }
        class C { mut Color c; str name; C() { c = Color.RED; name = "x"; } }
        fn main() -> i32 { Shared<C> s = Shared<C>(); return 0; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Shared - a mut POD value-object field is Shareable", "[shared][semantic]") {
    // A mut embedded value object with only scalar fields races to garbage but is memory-safe.
    auto result = analyzeString(R"(
        class Point { mut i32 x; mut i32 y; Point(i32 a) { x = a; y = a; } }
        class GameObject { mut Point pos; GameObject(Point p) { pos = p; } }
        fn main() -> i32 { Point p(1); Shared<GameObject> s = Shared<GameObject>(p); return 0; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Shared - non-Shareability is transitive through a value-object field", "[shared][semantic]") {
    // Outer embeds Inner by value; Inner has a mut reference → Outer is not Shareable either.
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Counter { mut i32 n; Counter(i32 x) { n = x; } }
        class Inner { mut i32 v; mut Counter&? c; Inner(i32 x) { v = x; } }
        class Outer { Inner inner; Outer(Inner i) { inner = i; } }
        fn main() -> i32 { Inner i(5); Shared<Outer> s = Shared<Outer>(i); return 0; }
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("Shared - a static field does not affect Shareability", "[shared][semantic]") {
    auto result = analyzeString(R"(
        class C { mut static i32 count; mut i32 v; C(i32 x) { v = x; } }
        fn main() -> i32 { Shared<C> s = Shared<C>(1); return 0; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Shared - a Shared of a generic instantiation works", "[shared][semantic]") {
    auto result = analyzeString(R"(
        class Box<T> { mut T v; Box(T x) { v = x; } }
        fn main() -> i32 { Shared<Box<i32>> s = Shared<Box<i32>>(5); return 0; }
    )");
    REQUIRE_FALSE(result.hadError);
}

// ------------------------------------------------------------
// Codegen — construction + atomic runtime
// ------------------------------------------------------------

TEST_CASE("Shared - construction emits gg_alloc + ctor + atomic release of the temp", "[shared][codegen]") {
    std::string ir = codegenString(R"(
        class Player { mut i32 hp; Player(i32 h) { hp = h; } }
        fn main() -> i32 {
            Shared<Player>(7);    // discarded temp → constructed then atomically released
            return 0;
        }
    )");
    REQUIRE(ir.find("call ptr @gg_alloc(") != std::string::npos);       // heap alloc + ctor
    REQUIRE(ir.find("@Player_Player(") != std::string::npos);           // constructor ran
    REQUIRE(ir.find("@gg_release_shared(") != std::string::npos);       // temp released ATOMICALLY
    // The shared temp must NOT be released via the plain non-atomic CALL (the `define void
    // @gg_release` runtime is still present — Shared uses gg_alloc — but nothing calls it here).
    REQUIRE(ir.find("call void @gg_release(ptr") == std::string::npos);
}

TEST_CASE("Shared - the atomic refcount runtime is emitted when Shared is used", "[shared][codegen]") {
    std::string ir = codegenString(R"(
        class Player { mut i32 hp; Player(i32 h) { hp = h; } }
        fn main() -> i32 { Shared<Player>(1); return 0; }
    )");
    REQUIRE(ir.find("define void @gg_retain_shared(") != std::string::npos);
    REQUIRE(ir.find("define void @gg_release_shared(") != std::string::npos);
    // Atomic ops with the right orderings (relaxed inc; release dec + acquire fence).
    REQUIRE(ir.find("atomicrmw add ptr") != std::string::npos);
    REQUIRE(ir.find("atomicrmw sub ptr") != std::string::npos);
    REQUIRE(ir.find("fence acquire") != std::string::npos);
}

TEST_CASE("Shared - a local binding retains/releases atomically at scope exit", "[shared][codegen]") {
    std::string ir = codegenString(R"(
        class Player { mut i32 hp; Player(i32 h) { hp = h; } }
        fn main() -> i32 {
            Shared<Player> p = Shared<Player>(7);   // +1 claimed
            Shared<Player> q = p;                    // copy → atomic retain
            return 0;
        }
    )");
    REQUIRE(ir.find("@gg_retain_shared(") != std::string::npos);        // copy retains atomically
    REQUIRE(ir.find("@gg_release_shared(") != std::string::npos);       // scope-exit release atomic
    REQUIRE(ir.find("call void @gg_retain(ptr") == std::string::npos);  // never the non-atomic call
}

TEST_CASE("Shared - rebind and +1 return are atomic", "[shared][codegen]") {
    std::string ir = codegenString(R"(
        class Player { mut i32 hp; Player(i32 h) { hp = h; } }
        fn make(i32 h) -> Shared<Player> {
            Shared<Player> p = Shared<Player>(h);
            return p;                                // +1 return → atomic retain
        }
        fn main() -> i32 {
            mut Shared<Player> m = Shared<Player>(1);
            m = Shared<Player>(2);                    // rebind → atomic release old + (claimed) new
            Shared<Player> r = make(9);
            return 0;
        }
    )");
    REQUIRE(ir.find("@gg_retain_shared(") != std::string::npos);
    REQUIRE(ir.find("@gg_release_shared(") != std::string::npos);
    REQUIRE(ir.find("call void @gg_retain(ptr") == std::string::npos);
    REQUIRE(ir.find("call void @gg_release(ptr") == std::string::npos);
}

TEST_CASE("Shared - a Shared field is retained/released atomically in clone and dtor", "[shared][codegen]") {
    std::string ir = codegenString(R"(
        class Player { mut i32 hp; Player(i32 h) { hp = h; } }
        class Holder {
            mut Shared<Player> c;
            Holder(Shared<Player> x) { c = x; }
        }
        fn main() -> i32 {
            Shared<Player> s = Shared<Player>(1);
            Holder h(s);
            Holder h2 = h;    // clone → the Shared field retains atomically
            return 0;
        }
    )");
    // The generated @Holder_clone and @Holder_dtor manage the Shared field atomically.
    REQUIRE(ir.find("@Holder_clone(") != std::string::npos);
    REQUIRE(ir.find("@Holder_dtor(")  != std::string::npos);
    REQUIRE(ir.find("@gg_retain_shared(")  != std::string::npos);
    REQUIRE(ir.find("@gg_release_shared(") != std::string::npos);
    // No non-atomic retain/release CALL anywhere (the only owning handle is the Shared field).
    REQUIRE(ir.find("call void @gg_retain(ptr")  == std::string::npos);
    REQUIRE(ir.find("call void @gg_release(ptr") == std::string::npos);
}

TEST_CASE("Shared - an implicit-this constructor field store is atomic", "[shared][codegen]") {
    // `c = x` (implicit this) inside a ctor is a DISTINCT code path from `this.c = x` — it flows
    // through genAssign's implicit-field fallback, not genMemberAssign. Verify it still retains the
    // Shared field atomically. No clone/copy here, so the only retain is the ctor's field store.
    std::string ir = codegenString(R"(
        class Player { mut i32 hp; Player(i32 h) { hp = h; } }
        class Holder {
            mut Shared<Player> c;
            Holder(Shared<Player> x) { c = x; }   // implicit-this field store → atomic retain
        }
        fn main() -> i32 {
            Shared<Player> s = Shared<Player>(1);
            Holder h(s);
            return 0;
        }
    )");
    REQUIRE(ir.find("@gg_retain_shared(")        != std::string::npos);
    REQUIRE(ir.find("call void @gg_retain(ptr")  == std::string::npos);
    REQUIRE(ir.find("call void @gg_release(ptr") == std::string::npos);
}

TEST_CASE("Shared - a nullable Shared field is null-safe in clone/dtor", "[shared][codegen]") {
    // The atomic retain/release helpers are null-safe, so a nullable Shared field (which may be null)
    // is handled without a guard at the field site.
    std::string ir = codegenString(R"(
        class Player { mut i32 hp; Player(i32 h) { hp = h; } }
        class Box {
            mut Shared<Player>? c;
            Box() { c = null; }
        }
        fn main() -> i32 { Box b(); Box b2 = b; return 0; }
    )");
    REQUIRE(ir.find("@Box_dtor(")   != std::string::npos);   // has a dtor (owns a shared field)
    REQUIRE(ir.find("@gg_release_shared(") != std::string::npos);
}

TEST_CASE("Shared - atomic runtime is NOT emitted without Shared", "[shared][codegen]") {
    std::string ir = codegenString(R"(
        class Player { mut i32 hp; Player(i32 h) { hp = h; } }
        fn main() -> i32 { Player& p = new Player(1); return 0; }
    )");
    REQUIRE(ir.find("gg_retain_shared") == std::string::npos);
    REQUIRE(ir.find("atomicrmw") == std::string::npos);
    REQUIRE(ir.find("define void @gg_retain(") != std::string::npos);   // plain runtime still there
}

TEST_CASE("Shared - typeName and mangleType round-trip", "[shared][type]") {
    Type s = makeSharedType("Player");
    REQUIRE(typeName(s) == "Shared<Player>");
    REQUIRE(mangleType(s) == "Player.shared");

    // synthTypeToken -> decodeSynthesizedType round-trips to the same Type.
    Token tok = synthTypeToken(s, 1);
    REQUIRE(tok.lexeme == "shared:Player");
    Type back = decodeSynthesizedType(tok);
    REQUIRE(isShared(back));
    REQUIRE(back.className == "Player");
    REQUIRE(back == s);
}
