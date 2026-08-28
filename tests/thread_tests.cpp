#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// Threads — the compiler "function-pointer part" of the stdlib `Thread` class.
//
// The stdlib `Thread` (std.thread) is ordinary GG; the compiler provides only:
//   * two intrinsics used INSIDE a generic factory where the closure's concrete class F is known:
//       __gg_heap_closure(closure) -> ptr   — heap-copy the closure so it outlives the spawning frame
//       __gg_trampoline(closure)   -> ptr   — emit the closure's C-ABI trampoline, return its address
//   * a per-closure trampoline `@__thread_entry$Class` (calls `@Class_call`, then releases the closure)
//   * the platform-neutral `@gg_thread_create` / `@gg_thread_join` runtime (defined, replacing the
//     stdlib `extern` declares)
//   * lambda-literal inference for a single-`Call`-bounded generic STATIC method (so `C.run(() -> {})`
//     infers F = the lambda class, the same way a generic free function already does)
//   * a Sendable boundary check: every value a thread closure captures must be Sendable.
// See docs/concurrency.md §5.
// ============================================================

// A minimal generic-free-function spawner used by several cases below.
static const char* SPAWN_PRELUDE = R"(
    extern gg_thread_create(ptr entry, ptr arg) -> ptr;
    extern gg_thread_join(ptr handle);
    fn runThread<F: Call() -> void>(F closure) -> ptr {
        ptr clo   = __gg_heap_closure(closure);
        ptr entry = __gg_trampoline(closure);
        return gg_thread_create(entry, clo);
    }
)";

// ------------------------------------------------------------
// Core: intrinsics + trampoline + runtime codegen
// ------------------------------------------------------------

TEST_CASE("Thread - a generic factory with the intrinsics + a lambda emits a trampoline", "[thread][codegen]") {
    std::string ir = codegenString(std::string(SPAWN_PRELUDE) + R"(
        fn main() -> i32 {
            ptr h = runThread(() -> {});
            gg_thread_join(h);
            return 0;
        }
    )");
    // The per-closure trampoline is emitted (default host target = Windows → i32 return).
    REQUIRE(ir.find("define i32 @__thread_entry$__lambda") != std::string::npos);
    // It calls the closure's `call`, then releases the heap closure.
    REQUIRE(ir.find("_call(ptr %arg)") != std::string::npos);
    REQUIRE(ir.find("@gg_release(ptr %arg") != std::string::npos);
    // The runtime is DEFINED (not just declared) and threads it through CreateThread.
    REQUIRE(ir.find("define ptr @gg_thread_create(") != std::string::npos);
    REQUIRE(ir.find("define void @gg_thread_join(") != std::string::npos);
    REQUIRE(ir.find("@CreateThread(") != std::string::npos);
    // The handle-release helper is emitted too (called by `~Thread` to CloseHandle on Windows).
    REQUIRE(ir.find("define void @gg_thread_dispose(") != std::string::npos);
    REQUIRE(ir.find("@CloseHandle(") != std::string::npos);
}

TEST_CASE("Thread - __gg_heap_closure heap-copies via the closure's clone", "[thread][codegen]") {
    std::string ir = codegenString(std::string(SPAWN_PRELUDE) + R"(
        fn main() -> i32 {
            ptr h = runThread(() -> {});
            gg_thread_join(h);
            return 0;
        }
    )");
    REQUIRE(ir.find("@gg_alloc(") != std::string::npos);
    REQUIRE(ir.find("@__lambda") != std::string::npos);
    REQUIRE(ir.find("_clone(ptr ") != std::string::npos);   // deep-copy into the heap slot
}

TEST_CASE("Thread - the stdlib extern gg_thread_create declare is replaced by the definition", "[thread][codegen]") {
    std::string ir = codegenString(std::string(SPAWN_PRELUDE) + R"(
        fn main() -> i32 { ptr h = runThread(() -> {}); gg_thread_join(h); return 0; }
    )");
    // Exactly one `gg_thread_create` symbol, and it is a define (LLVM forbids declare + define).
    REQUIRE(ir.find("declare ptr @gg_thread_create(") == std::string::npos);
    REQUIRE(ir.find("define ptr @gg_thread_create(") != std::string::npos);
}

// ------------------------------------------------------------
// Lambda inference for a single-`Call`-bounded generic STATIC method (the `Thread.create` surface)
// ------------------------------------------------------------

TEST_CASE("Thread - a static generic method infers a lambda argument's class", "[thread][genericmethod]") {
    std::string ir = codegenString(R"(
        extern gg_thread_create(ptr entry, ptr arg) -> ptr;
        class Spawner {
            fn static run<F: Call() -> void>(F closure) -> ptr {
                return gg_thread_create(__gg_trampoline(closure), __gg_heap_closure(closure));
            }
        }
        fn main() -> i32 { ptr h = Spawner.run(() -> {}); return 0; }
    )");
    // The static-method call inferred F = the lambda class and instantiated `run$__lambda…`.
    REQUIRE(ir.find("@Spawner_run$__lambda") != std::string::npos);
    REQUIRE(ir.find("@__thread_entry$__lambda") != std::string::npos);
}

TEST_CASE("Thread - static-method inference also works via the :: form", "[thread][genericmethod]") {
    auto result = analyzeString(R"(
        extern gg_thread_create(ptr entry, ptr arg) -> ptr;
        class Spawner {
            fn static run<F: Call() -> void>(F closure) -> ptr {
                return gg_thread_create(__gg_trampoline(closure), __gg_heap_closure(closure));
            }
        }
        fn main() -> i32 { ptr h = Spawner::run(() -> {}); return 0; }
    )");
    REQUIRE_FALSE(result.hadError);
}

// ------------------------------------------------------------
// Sendable boundary — a thread closure's captures must be Sendable
// ------------------------------------------------------------

TEST_CASE("Thread - capturing a primitive (copied) is Sendable", "[thread][sendable]") {
    auto result = analyzeString(std::string(SPAWN_PRELUDE) + R"(
        class G { mut static i32 x; }
        fn main() -> i32 {
            i32 n = 5;
            ptr h = runThread(() -> { G::x = n; });
            gg_thread_join(h);
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Thread - capturing a Shared<T> is Sendable", "[thread][sendable]") {
    auto result = analyzeString(std::string(SPAWN_PRELUDE) + R"(
        class Box { mut i32 v; Box() { v = 0; } fn get() -> i32 { return v; } }
        class G { mut static i32 x; }
        fn main() -> i32 {
            Shared<Box> b = Shared<Box>();
            ptr h = runThread(() -> { G::x = b.get(); });
            gg_thread_join(h);
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Thread - capturing an owning reference is NOT Sendable", "[thread][sendable]") {
    StderrCapture cap;
    auto result = analyzeString(std::string(SPAWN_PRELUDE) + R"(
        class Box { mut i32 v; Box() { v = 0; } fn get() -> i32 { return v; } }
        class G { mut static i32 x; }
        fn main() -> i32 {
            Box& b = new Box();
            ptr h = runThread(() -> { G::x = b.get(); });
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.str().find("not Sendable") != std::string::npos);
}

TEST_CASE("Thread - capturing a borrow is NOT Sendable", "[thread][sendable]") {
    StderrCapture cap;
    auto result = analyzeString(std::string(SPAWN_PRELUDE) + R"(
        class Box { mut i32 v; Box() { v = 0; } fn get() -> i32 { return v; } }
        class G { mut static i32 x; }
        fn use(Box* b) -> i32 {
            ptr h = runThread(() -> { G::x = b.get(); });
            return 0;
        }
        fn main() -> i32 { Box bx(); return use(bx); }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.str().find("not Sendable") != std::string::npos);
}

TEST_CASE("Thread - capturing a value object with an owning-ref field is NOT Sendable", "[thread][sendable]") {
    StderrCapture cap;
    auto result = analyzeString(std::string(SPAWN_PRELUDE) + R"(
        class Leaf { mut i32 v; Leaf(i32 a) { v = a; } fn get() -> i32 { return v; } }
        class Wrap { mut Leaf& leaf; Wrap(Leaf& l) { leaf = l; } fn get() -> i32 { return leaf.get(); } }
        class G { mut static i32 x; }
        fn main() -> i32 {
            Wrap w = Wrap(new Leaf(3));
            ptr h = runThread(() -> { G::x = w.get(); });
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.str().find("not Sendable") != std::string::npos);
}

TEST_CASE("Thread - capturing a value object with only Sendable fields is Sendable", "[thread][sendable]") {
    auto result = analyzeString(std::string(SPAWN_PRELUDE) + R"(
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } fn sum() -> i32 { return x + y; } }
        class G { mut static i32 x; }
        fn main() -> i32 {
            Point p = Point(1, 2);
            ptr h = runThread(() -> { G::x = p.sum(); });
            gg_thread_join(h);
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Thread - capturing a value object embedding a Shared field is Sendable", "[thread][sendable]") {
    // Exercises the isSendableClass RECURSION positively: HasShared's only field is a Shared<Box>,
    // which is Sendable, so HasShared is Sendable.
    auto result = analyzeString(std::string(SPAWN_PRELUDE) + R"(
        class Box { mut i32 v; Box() { v = 0; } fn get() -> i32 { return v; } }
        class HasShared { Shared<Box> b; HasShared(Shared<Box> s) { b = s; } fn peek() -> i32 { return b.get(); } }
        class G { mut static i32 x; }
        fn main() -> i32 {
            HasShared hs = HasShared(Shared<Box>());
            ptr h = runThread(() -> { G::x = hs.peek(); });
            gg_thread_join(h);
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Thread - capturing str and enum is Sendable", "[thread][sendable]") {
    auto result = analyzeString(std::string(SPAWN_PRELUDE) + R"(
        enum Color { RED, GREEN, BLUE }
        class G { mut static i32 x; }
        fn main() -> i32 {
            str name = "hi";
            Color c = Color::GREEN;
            ptr h1 = runThread(() -> { G::x = name.len as i32; });
            ptr h2 = runThread(() -> { if (c == Color::GREEN) { G::x = 1; } });
            gg_thread_join(h1); gg_thread_join(h2);
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Thread - the Sendable error names the offending capture, not a Sendable sibling", "[thread][sendable]") {
    StderrCapture cap;
    auto result = analyzeString(std::string(SPAWN_PRELUDE) + R"(
        class Box { mut i32 v; Box() { v = 0; } fn get() -> i32 { return v; } }
        class G { mut static i32 x; }
        fn main() -> i32 {
            i32 okCount = 7;
            Box& bad = new Box();
            ptr h = runThread(() -> { G::x = okCount + bad.get(); });
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.str().find("'bad'") != std::string::npos);
    REQUIRE(cap.str().find("'okCount'") == std::string::npos);
}

TEST_CASE("Thread - a hand-written impl-Call callable (non-lambda) works with a generic spawner", "[thread][codegen]") {
    // A non-lambda callable reaches the intrinsics via ORDINARY generic inference (its class is an
    // in-scope identifier), not lambda inference — so the trampoline is emitted for the real class.
    std::string ir = codegenString(std::string(SPAWN_PRELUDE) + R"(
        class G { mut static i32 x; }
        class Greeter { mut i32 tag; Greeter(i32 t) { tag = t; } }
        impl Call for Greeter { fn call() { G::x = 1; } }
        fn main() -> i32 {
            Greeter g = Greeter(7);
            ptr h = runThread(g);
            gg_thread_join(h);
            return 0;
        }
    )");
    REQUIRE(ir.find("@runThread$Greeter") != std::string::npos);
    REQUIRE(ir.find("@__thread_entry$Greeter") != std::string::npos);
    REQUIRE(ir.find("@Greeter_call(ptr %arg)") != std::string::npos);
}

// ------------------------------------------------------------
// Static confinement — a thread body may only touch Sendable statics (transitively)
// ------------------------------------------------------------

TEST_CASE("Thread - touching a Sendable static (mut static i32) is allowed", "[thread][statics]") {
    auto result = analyzeString(std::string(SPAWN_PRELUDE) + R"(
        class Counter { mut static i32 n; }
        fn main() -> i32 {
            ptr h = runThread(() -> { Counter::n = 5; });
            gg_thread_join(h);
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Thread - touching a static Shared<T> is allowed", "[thread][statics]") {
    auto result = analyzeString(std::string(SPAWN_PRELUDE) + R"(
        class Player { mut i32 hp; Player(i32 h) { hp = h; } fn get() -> i32 { return hp; } }
        class Reg { static Shared<Player> p; }
        fn main() -> i32 {
            ptr h = runThread(() -> { i32 x = Reg::p.get(); });
            gg_thread_join(h);
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Thread - touching a non-Sendable static in the closure body is rejected", "[thread][statics]") {
    StderrCapture cap;
    auto result = analyzeString(std::string(SPAWN_PRELUDE) + R"(
        class Player { mut i32 hp; Player(i32 h) { hp = h; } }
        class Reg { mut static Player& current; }
        fn main() -> i32 {
            Reg::current = new Player(10);
            ptr h = runThread(() -> { Reg::current = new Player(20); });
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.str().find("not thread-safe") != std::string::npos);
    REQUIRE(cap.str().find("Reg::current") != std::string::npos);
}

TEST_CASE("Thread - a non-Sendable static reached through a called free function is rejected", "[thread][statics]") {
    StderrCapture cap;
    auto result = analyzeString(std::string(SPAWN_PRELUDE) + R"(
        class Player { mut i32 hp; Player(i32 h) { hp = h; } }
        class Reg { mut static Player& current; }
        fn resetGlobal() { Reg::current = new Player(0); }
        fn main() -> i32 {
            Reg::current = new Player(10);
            ptr h = runThread(() -> { resetGlobal(); });
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.str().find("not thread-safe") != std::string::npos);
}

TEST_CASE("Thread - a non-Sendable static reached through a called method is rejected", "[thread][statics]") {
    StderrCapture cap;
    auto result = analyzeString(std::string(SPAWN_PRELUDE) + R"(
        class Player { mut i32 hp; Player(i32 h) { hp = h; } }
        class Reg { mut static Player& current; fn static reset() { Reg::current = new Player(1); } }
        fn main() -> i32 {
            Reg::current = new Player(10);
            ptr h = runThread(() -> { Reg::reset(); });
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.str().find("not thread-safe") != std::string::npos);
}

TEST_CASE("Thread - rebinding a mut static Shared<T> from a thread is rejected", "[thread][statics]") {
    // A static is SHARED in place, not copied: a `mut` Shared static can be REBOUND from two threads
    // (torn store / double-free) even though the handle is atomic — so it is gated like a Shared<T>
    // field (isSharedSafeField), NOT by isSendableType (which would wrongly allow it).
    StderrCapture cap;
    auto result = analyzeString(std::string(SPAWN_PRELUDE) + R"(
        class Player { mut i32 hp; Player(i32 h) { hp = h; } }
        class Reg { mut static Shared<Player> p; }
        fn main() -> i32 {
            Reg::p = Shared<Player>(1);
            ptr h = runThread(() -> { Reg::p = Shared<Player>(2); });
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.str().find("not thread-safe") != std::string::npos);
}

TEST_CASE("Thread - a non-Sendable static reached two calls deep (through recursion) is rejected", "[thread][statics]") {
    // Exercises the transitive worklist's DEPTH and its recursion/cycle termination (deduped by
    // body pointer): closure -> recurse -> level1 -> level2 -> the bad static.
    StderrCapture cap;
    auto result = analyzeString(std::string(SPAWN_PRELUDE) + R"(
        class Player { mut i32 hp; Player(i32 h) { hp = h; } }
        class Reg { mut static Player& current; }
        fn level2() { Reg::current = new Player(2); }
        fn level1() { level2(); }
        fn recurse(i32 n) { if (n > 0) { recurse(n - 1); } level1(); }
        fn main() -> i32 {
            Reg::current = new Player(0);
            ptr h = runThread(() -> { recurse(3); });
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.str().find("not thread-safe") != std::string::npos);
}

TEST_CASE("Thread - implicit-bare and instance access to a Sendable static is allowed", "[thread][statics]") {
    // A static reached without the `Class::` prefix (implicit bare, inside the declaring class) and
    // one reached via an instance receiver (`obj.getN()`) both resolve to the static and are gated —
    // but a `mut static i32` is Sendable, so both are allowed.
    auto result = analyzeString(std::string(SPAWN_PRELUDE) + R"(
        class Bag {
            mut static i32 n;
            fn static bumpImplicit() { n = n + 1; }
            fn getN() -> i32 { return Bag::n; }
        }
        fn main() -> i32 {
            Bag b();
            ptr h1 = runThread(() -> { Bag::bumpImplicit(); });
            ptr h2 = runThread(() -> { i32 y = b.getN(); });
            gg_thread_join(h1); gg_thread_join(h2);
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Thread - a local shadowing a static's name is not falsely gated", "[thread][statics]") {
    // Inside Reg's own method a bare `shared` is a LOCAL, not the non-Sendable static Reg::shared —
    // the shadowing guard must not report it.
    auto result = analyzeString(std::string(SPAWN_PRELUDE) + R"(
        class Player { mut i32 hp; Player(i32 h) { hp = h; } }
        class Reg {
            mut static Player& shared;
            fn static work() -> i32 { i32 shared = 3; return shared + 1; }
        }
        fn main() -> i32 {
            Reg::shared = new Player(1);
            ptr h = runThread(() -> { i32 y = Reg::work(); });
            gg_thread_join(h);
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

// ------------------------------------------------------------
// Intrinsic arity
// ------------------------------------------------------------

TEST_CASE("Thread - __gg_heap_closure requires exactly one argument", "[thread][sendable]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 { ptr p = __gg_heap_closure(); return 0; }
    )");
    REQUIRE(result.hadError);
}
