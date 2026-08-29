#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// Mutex<T> — safe mutable shared state (concurrency Phase 2).
//
// Mutex<T> is an ordinary stdlib generic class `{ ptr handle; T value }` recognised by the compiler
// by simple name; the compiler supplies the OS-lock runtime, the Shareable-gate exemption (a Mutex is
// inherently Shareable — the lock serialises its interior), and the `.with((mut T*) -> void)` scoped
// access lowering (acquire → closure.call(&interior) → release, with the borrow confined to the
// closure). These tests use a synthetic Mutex (codegenString/analyzeString have no ImportResolver).
// ============================================================

// A synthetic stand-in for stdlib `std.sync.Mutex`, recognised by simple name.
static const char* MUTEX_PRELUDE = R"(
    extern gg_mutex_create() -> ptr;
    extern gg_mutex_destroy(ptr h);
    extern gg_mutex_lock(ptr h);
    extern gg_mutex_unlock(ptr h);
    class Mutex<T> {
        private mut ptr handle;
        private T value;
        Mutex(T v) { handle = gg_mutex_create(); value = v; }
        ~Mutex() { gg_mutex_destroy(handle); }
    }
    class MutexGuard<T> {
        private mut ptr handle;
        private mut T* interior;
        ~MutexGuard() { gg_mutex_unlock(handle); }
    }
)";

// ------------------------------------------------------------
// Shareable gate — Mutex is the interior-mutability escape hatch
// ------------------------------------------------------------

TEST_CASE("Mutex - Shared<Mutex<T>> is Shareable (constructs without a gate error)", "[mutex][shared]") {
    auto result = analyzeString(std::string(MUTEX_PRELUDE) + R"(
        class Counter { mut i32 n; Counter() { n = 0; } }
        fn main() -> i32 {
            Shared<Mutex<Counter>> m = Shared<Mutex<Counter>>(Counter());
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Mutex - makes an otherwise non-Shareable T shareable", "[mutex][shared]") {
    // Node has a `mut` reference field → a bare Shared<Node> would be rejected; wrapping in a Mutex
    // (which serialises all access) makes Shared<Mutex<Node>> safe.
    auto result = analyzeString(std::string(MUTEX_PRELUDE) + R"(
        class Node { mut i32 v; mut Node&? next; Node() { v = 0; } }
        fn main() -> i32 {
            Shared<Mutex<Node>> m = Shared<Mutex<Node>>(Node());
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

// ------------------------------------------------------------
// .with lowering
// ------------------------------------------------------------

TEST_CASE("Mutex - .with lowers to acquire / closure.call(&interior) / release", "[mutex][codegen]") {
    std::string ir = codegenString(std::string(MUTEX_PRELUDE) + R"(
        class Counter { mut i32 n; Counter() { n = 0; } fn bump() mut { n = n + 1; } }
        fn main() -> i32 {
            Mutex<Counter> m = Mutex<Counter>(Counter());
            m.with((mut Counter* c) -> { c.bump(); });
            return 0;
        }
    )");
    REQUIRE(ir.find("call void @gg_mutex_lock(ptr ")   != std::string::npos);
    REQUIRE(ir.find("call void @gg_mutex_unlock(ptr ") != std::string::npos);
    REQUIRE(ir.find("_call(ptr ")                      != std::string::npos);   // the closure invocation
    // The OS-lock runtime is defined (declare-presence gate replaced the extern declares).
    REQUIRE(ir.find("define void @gg_mutex_lock(")     != std::string::npos);
}

TEST_CASE("Mutex - .with works with a primitive interior", "[mutex][codegen]") {
    std::string ir = codegenString(std::string(MUTEX_PRELUDE) + R"(
        fn main() -> i32 {
            Mutex<i32> m = Mutex<i32>(0);
            m.with((mut i32* v) -> { v = v + 1; });
            return 0;
        }
    )");
    REQUIRE(ir.find("call void @gg_mutex_lock(ptr ")   != std::string::npos);
    REQUIRE(ir.find("call void @gg_mutex_unlock(ptr ") != std::string::npos);
}

// ------------------------------------------------------------
// Closure validation + confinement
// ------------------------------------------------------------

TEST_CASE("Mutex - .with rejects a non-void closure (the borrow cannot be returned)", "[mutex][confine]") {
    StderrCapture cap;
    auto result = analyzeString(std::string(MUTEX_PRELUDE) + R"(
        class Counter { mut i32 n; Counter() { n = 0; } }
        fn main() -> i32 {
            Mutex<Counter> m = Mutex<Counter>(Counter());
            m.with((mut Counter* c) -> Counter* { return c; });
            return 0;
        }
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("Mutex - .with rejects a read-only closure parameter (must be mut)", "[mutex][confine]") {
    StderrCapture cap;
    auto result = analyzeString(std::string(MUTEX_PRELUDE) + R"(
        class Counter { mut i32 n; Counter() { n = 0; } fn get() -> i32 { return n; } }
        fn main() -> i32 {
            Mutex<Counter> m = Mutex<Counter>(Counter());
            m.with((Counter* c) -> { });
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.str().find("mut") != std::string::npos);
}

TEST_CASE("Mutex - .with rejects a closure whose parameter type is not the element", "[mutex][confine]") {
    StderrCapture cap;
    auto result = analyzeString(std::string(MUTEX_PRELUDE) + R"(
        class Counter { mut i32 n; Counter() { n = 0; } }
        class Other  { mut i32 k; Other() { k = 0; } }
        fn main() -> i32 {
            Mutex<Counter> m = Mutex<Counter>(Counter());
            m.with((mut Other* o) -> { });
            return 0;
        }
    )");
    REQUIRE(result.hadError);
}

// ------------------------------------------------------------
// Sendable — a Shared<Mutex<T>> may cross into a thread
// ------------------------------------------------------------

// ------------------------------------------------------------
// RwLock<T> — reader/writer variant, reusing the Mutex machinery
// ------------------------------------------------------------

static const char* RWLOCK_PRELUDE = R"(
    extern gg_rwlock_create() -> ptr;
    extern gg_rwlock_destroy(ptr h);
    extern gg_rwlock_rdlock(ptr h);
    extern gg_rwlock_rdunlock(ptr h);
    extern gg_rwlock_wrlock(ptr h);
    extern gg_rwlock_wrunlock(ptr h);
    class RwLock<T> {
        private mut ptr handle;
        private T value;
        RwLock(T v) { handle = gg_rwlock_create(); value = v; }
        ~RwLock() { gg_rwlock_destroy(handle); }
    }
    class RwReadGuard<T> {
        private mut ptr handle;
        private T* interior;
        ~RwReadGuard() { gg_rwlock_rdunlock(handle); }
    }
    class RwWriteGuard<T> {
        private mut ptr handle;
        private mut T* interior;
        ~RwWriteGuard() { gg_rwlock_wrunlock(handle); }
    }
)";

TEST_CASE("RwLock - Shared<RwLock<T>> is Shareable", "[rwlock][shared]") {
    auto result = analyzeString(std::string(RWLOCK_PRELUDE) + R"(
        class Data { mut i32 v; Data() { v = 0; } }
        fn main() -> i32 { Shared<RwLock<Data>> d = Shared<RwLock<Data>>(Data()); return 0; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("RwLock - .read takes a shared lock, .write an exclusive lock", "[rwlock][codegen]") {
    std::string ir = codegenString(std::string(RWLOCK_PRELUDE) + R"(
        class Data { mut i32 v; Data() { v = 0; } fn get() -> i32 { return v; } fn set(i32 x) mut { v = x; } }
        class G { mut static i32 s; }
        fn main() -> i32 {
            RwLock<Data> d = RwLock<Data>(Data());
            d.write((mut Data* p) -> { p.set(5); });
            d.read((Data* p) -> { G::s = p.get(); });
            return 0;
        }
    )");
    REQUIRE(ir.find("call void @gg_rwlock_wrlock(ptr ")   != std::string::npos);
    REQUIRE(ir.find("call void @gg_rwlock_wrunlock(ptr ") != std::string::npos);
    REQUIRE(ir.find("call void @gg_rwlock_rdlock(ptr ")   != std::string::npos);
    REQUIRE(ir.find("call void @gg_rwlock_rdunlock(ptr ") != std::string::npos);
}

TEST_CASE("RwLock - .read rejects a mut closure parameter (reads are read-only)", "[rwlock][confine]") {
    StderrCapture cap;
    auto result = analyzeString(std::string(RWLOCK_PRELUDE) + R"(
        class Data { mut i32 v; Data() { v = 0; } }
        fn main() -> i32 {
            RwLock<Data> d = RwLock<Data>(Data());
            d.read((mut Data* p) -> { });
            return 0;
        }
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("RwLock - .write requires a mut closure parameter", "[rwlock][confine]") {
    StderrCapture cap;
    auto result = analyzeString(std::string(RWLOCK_PRELUDE) + R"(
        class Data { mut i32 v; Data() { v = 0; } }
        fn main() -> i32 {
            RwLock<Data> d = RwLock<Data>(Data());
            d.write((Data* p) -> { });
            return 0;
        }
    )");
    REQUIRE(result.hadError);
}

// ------------------------------------------------------------
// Edge cases
// ------------------------------------------------------------

TEST_CASE("Mutex - a non-cell class with its own .with method is not intercepted", "[mutex][confine]") {
    // `.with` is recognised only on a sync cell; an ordinary class keeps its own `with` method.
    auto result = analyzeString(R"(
        class Widget { mut i32 n; Widget() { n = 0; } fn with(i32 x) mut { n = x; } }
        fn main() -> i32 { mut Widget w(); w.with(7); return 0; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Mutex - .with with the wrong argument count is rejected", "[mutex][confine]") {
    StderrCapture cap;
    auto result = analyzeString(std::string(MUTEX_PRELUDE) + R"(
        class C { mut i32 n; C() { n = 0; } }
        fn main() -> i32 { Mutex<C> m = Mutex<C>(C()); m.with(); return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.str().find("exactly one argument") != std::string::npos);
}

TEST_CASE("Mutex - .with rejects a primitive-element mismatch", "[mutex][confine]") {
    StderrCapture cap;
    auto result = analyzeString(std::string(MUTEX_PRELUDE) + R"(
        fn main() -> i32 { Mutex<i32> m = Mutex<i32>(0); m.with((mut i64* v) -> { }); return 0; }
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("Mutex - a nested .with (nested lambda) is rejected", "[mutex][confine]") {
    // Parse-time error → surfaced on stderr by parseString (it returns an empty program).
    StderrCapture cap;
    analyzeString(std::string(MUTEX_PRELUDE) + R"(
        class C { mut i32 n; C() { n = 0; } fn bump() mut { n = n + 1; } }
        fn main() -> i32 {
            Mutex<C> a = Mutex<C>(C());
            Mutex<C> b = Mutex<C>(C());
            a.with((mut C* p) -> { b.with((mut C* q) -> { q.bump(); }); });
            return 0;
        }
    )");
    REQUIRE(cap.str().find("nested lambdas") != std::string::npos);
}

// ------------------------------------------------------------
// Interprocedural borrow-confinement (checkSyncConfinement, Phase 2.5).
// The guarded borrow must not escape THROUGH a called function, not just the closure body itself.
// ------------------------------------------------------------

TEST_CASE("Mutex - .with rejects the borrow escaping via a called free function (store to static)", "[mutex][confine][interproc]") {
    StderrCapture cap;
    auto result = analyzeString(std::string(MUTEX_PRELUDE) + R"(
        class Counter { mut i32 n; Counter() { n = 0; } }
        class Reg { mut static Counter* held; }
        fn stash(Counter* p) { Reg::held = p; }
        fn main() -> i32 {
            Mutex<Counter> m = Mutex<Counter>(Counter());
            m.with((mut Counter* c) -> { stash(c); });
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.str().find("escapes the lock closure") != std::string::npos);
}

TEST_CASE("Mutex - .with rejects the borrow escaping via a called function that returns it", "[mutex][confine][interproc]") {
    StderrCapture cap;
    auto result = analyzeString(std::string(MUTEX_PRELUDE) + R"(
        class Counter { mut i32 n; Counter() { n = 0; } }
        fn ident(Counter* p) -> Counter* { return p; }
        fn main() -> i32 {
            Mutex<Counter> m = Mutex<Counter>(Counter());
            m.with((mut Counter* c) -> { ident(c); });
            return 0;
        }
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("Mutex - .with rejects a borrow escape two calls deep (transitive)", "[mutex][confine][interproc]") {
    StderrCapture cap;
    auto result = analyzeString(std::string(MUTEX_PRELUDE) + R"(
        class Counter { mut i32 n; Counter() { n = 0; } }
        class Reg { mut static Counter* held; }
        fn inner(Counter* q) { Reg::held = q; }
        fn outer(Counter* p) { inner(p); }
        fn main() -> i32 {
            Mutex<Counter> m = Mutex<Counter>(Counter());
            m.with((mut Counter* c) -> { outer(c); });
            return 0;
        }
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("Mutex - .with rejects the borrow escaping via a method that stashes `this`", "[mutex][confine][interproc]") {
    // A method called ON the guarded borrow whose `this` escapes (stored into a static) is caught
    // via the receiver-taint path.
    StderrCapture cap;
    auto result = analyzeString(std::string(MUTEX_PRELUDE) + R"(
        class Sink { mut static Counter* held; }
        class Counter { mut i32 n; Counter() { n = 0; } fn leak() { Sink::held = this; } }
        fn main() -> i32 {
            Mutex<Counter> m = Mutex<Counter>(Counter());
            m.with((mut Counter* c) -> { c.leak(); });
            return 0;
        }
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("Mutex - .with rejects the borrow stored into a field of an outliving object (via helper)", "[mutex][confine][interproc]") {
    StderrCapture cap;
    auto result = analyzeString(std::string(MUTEX_PRELUDE) + R"(
        class Counter { mut i32 n; Counter() { n = 0; } }
        class Box { mut Counter* slot; Box() { } fn put(Counter* p) mut { slot = p; } }
        fn stashInto(Box* b, Counter* p) { b.put(p); }
        fn main() -> i32 {
            Mutex<Counter> m = Mutex<Counter>(Counter());
            Box b();
            m.with((mut Counter* c) -> { stashInto(b, c); });
            return 0;
        }
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("Mutex - .with accepts a helper that only reads/mutates through the borrow (no escape)", "[mutex][confine][interproc]") {
    // The borrow flows into a helper that uses it (calls a method, passes it to a pure reader) but
    // never returns or stores it — this must NOT be flagged.
    auto result = analyzeString(std::string(MUTEX_PRELUDE) + R"(
        class Counter { mut i32 n; Counter() { n = 0; } fn set(i32 x) mut { n = x; } fn get() -> i32 { return n; } }
        fn readOnly(Counter* p) -> i32 { return p.get(); }
        fn bumpIt(mut Counter* p) { p.set(p.get() + 1); }
        fn main() -> i32 {
            Mutex<Counter> m = Mutex<Counter>(Counter());
            m.with((mut Counter* c) -> { bumpIt(c); });
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Mutex - .with accepts a direct method call on the borrow (no escape)", "[mutex][confine][interproc]") {
    // The common, safe case: the closure mutates the guarded value in place. Regression guard against
    // the interprocedural pass over-reporting on an ordinary `p.set(x)`.
    auto result = analyzeString(std::string(MUTEX_PRELUDE) + R"(
        class Counter { mut i32 n; Counter() { n = 0; } fn set(i32 x) mut { n = x; } }
        fn main() -> i32 {
            Mutex<Counter> m = Mutex<Counter>(Counter());
            mut i32 i = 5;
            m.with((mut Counter* c) -> { c.set(i); });
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("RwLock - .read rejects the borrow escaping via a called function", "[rwlock][confine][interproc]") {
    StderrCapture cap;
    auto result = analyzeString(std::string(RWLOCK_PRELUDE) + R"(
        class Data { mut i32 v; Data() { v = 0; } }
        class Reg { mut static Data* held; }
        fn stash(Data* p) { Reg::held = p; }
        fn main() -> i32 {
            RwLock<Data> d = RwLock<Data>(Data());
            d.read((Data* p) -> { stash(p); });
            return 0;
        }
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("Mutex - .with rejects an escape where the borrow is a NON-FIRST argument", "[mutex][confine][interproc]") {
    // The arg→param mapping must be positional (not always slot 0): the borrow is arg 1 here.
    StderrCapture cap;
    auto result = analyzeString(std::string(MUTEX_PRELUDE) + R"(
        class Counter { mut i32 n; Counter() { n = 0; } }
        class Reg { mut static Counter* held; }
        fn f(i32 x, Counter* p) { Reg::held = p; }
        fn main() -> i32 {
            Mutex<Counter> m = Mutex<Counter>(Counter());
            m.with((mut Counter* c) -> { f(1, c); });
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.str().find("escapes the lock closure") != std::string::npos);
}

TEST_CASE("Mutex - .with rejects an escape through a local alias of the borrow", "[mutex][confine][interproc]") {
    // Taint must flow through `var q = c;` (a local rebind), not only the direct parameter.
    StderrCapture cap;
    auto result = analyzeString(std::string(MUTEX_PRELUDE) + R"(
        class Counter { mut i32 n; Counter() { n = 0; } }
        class Reg { mut static Counter* held; }
        fn stash(Counter* p) { Reg::held = p; }
        fn main() -> i32 {
            Mutex<Counter> m = Mutex<Counter>(Counter());
            m.with((mut Counter* c) -> { var q = c; stash(q); });
            return 0;
        }
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("Mutex - .with rejects an escape reached through terminating recursion", "[mutex][confine][interproc]") {
    // The transitive walk must terminate on a cycle yet still find the base-case escape.
    StderrCapture cap;
    auto result = analyzeString(std::string(MUTEX_PRELUDE) + R"(
        class Counter { mut i32 n; Counter() { n = 0; } }
        class Reg { mut static Counter* held; }
        fn rec(i32 k, Counter* p) { if (k == 0) { Reg::held = p; } else { rec(k - 1, p); } }
        fn main() -> i32 {
            Mutex<Counter> m = Mutex<Counter>(Counter());
            m.with((mut Counter* c) -> { rec(3, c); });
            return 0;
        }
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("Mutex - .with accepts a helper that returns a value DERIVED from the borrow (no escape)", "[mutex][confine][interproc]") {
    // Returning `p.get()` (an i32) is not returning the borrow — must not be flagged.
    auto result = analyzeString(std::string(MUTEX_PRELUDE) + R"(
        class Counter { mut i32 n; Counter() { n = 0; } fn get() -> i32 { return n; } }
        fn readN(Counter* p) -> i32 { return p.get(); }
        fn main() -> i32 {
            Mutex<Counter> m = Mutex<Counter>(Counter());
            m.with((mut Counter* c) -> { i32 x = readN(c); });
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

// ------------------------------------------------------------
// RAII lock guards (Phase 2.5): `m.lock()` / `rw.rlock()` / `rw.wlock()` + auto-deref access.
// ------------------------------------------------------------

TEST_CASE("Guard - m.lock() acquires the lock and wires the guard; the guard dtor unlocks", "[guard][codegen]") {
    std::string ir = codegenString(std::string(MUTEX_PRELUDE) + R"(
        class Counter { mut i32 n; Counter() { n = 0; } }
        fn main() -> i32 {
            Mutex<Counter> m = Mutex<Counter>(Counter());
            { MutexGuard<Counter> g = m.lock(); }
            return 0;
        }
    )");
    REQUIRE(ir.find("call void @gg_mutex_lock(") != std::string::npos);
    REQUIRE(ir.find("call void @gg_mutex_unlock(") != std::string::npos);   // in the guard's dtor
}

TEST_CASE("Guard - auto-deref field write + method call through a MutexGuard", "[guard]") {
    auto result = analyzeString(std::string(MUTEX_PRELUDE) + R"(
        class Counter { mut i32 n; Counter() { n = 0; } fn bump() mut { n = n + 1; } fn get() -> i32 { return n; } }
        fn main() -> i32 {
            Mutex<Counter> m = Mutex<Counter>(Counter());
            { MutexGuard<Counter> g = m.lock(); g.n = 10; g.bump(); i32 x = g.get(); }
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Guard - a MutexGuard is non-copyable (owns the lock handle)", "[guard]") {
    StderrCapture cap;
    auto result = analyzeString(std::string(MUTEX_PRELUDE) + R"(
        class Counter { mut i32 n; Counter() { n = 0; } }
        fn main() -> i32 {
            Mutex<Counter> m = Mutex<Counter>(Counter());
            MutexGuard<Counter> a = m.lock();
            MutexGuard<Counter> b = a;   // copy → would double-unlock
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.str().find("owns a raw pointer") != std::string::npos);
}

TEST_CASE("Guard - a lock guard used as an argument is rejected (must be a scoped local)", "[guard][confine]") {
    StderrCapture cap;
    auto result = analyzeString(std::string(MUTEX_PRELUDE) + R"(
        class Counter { mut i32 n; Counter() { n = 0; } }
        fn sink(MutexGuard<Counter> g) { }
        fn main() -> i32 {
            Mutex<Counter> m = Mutex<Counter>(Counter());
            sink(m.lock());
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.str().find("must be bound to a local") != std::string::npos);
}

TEST_CASE("Guard - a discarded lock guard temp is rejected", "[guard][confine]") {
    StderrCapture cap;
    auto result = analyzeString(std::string(MUTEX_PRELUDE) + R"(
        class Counter { mut i32 n; Counter() { n = 0; } }
        fn main() -> i32 {
            Mutex<Counter> m = Mutex<Counter>(Counter());
            m.lock();
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.str().find("must be bound to a local") != std::string::npos);
}

TEST_CASE("Guard - lock() takes no arguments", "[guard]") {
    StderrCapture cap;
    auto result = analyzeString(std::string(MUTEX_PRELUDE) + R"(
        class Counter { mut i32 n; Counter() { n = 0; } }
        fn main() -> i32 {
            Mutex<Counter> m = Mutex<Counter>(Counter());
            MutexGuard<Counter> g = m.lock(5);
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.str().find("takes no arguments") != std::string::npos);
}

TEST_CASE("Guard - rlock/wlock recognised; write guard mutates, read guard is read-only", "[rwlock][guard]") {
    auto ok = analyzeString(std::string(RWLOCK_PRELUDE) + R"(
        class Data { mut i32 v; Data() { v = 0; } fn get() -> i32 { return v; } }
        fn main() -> i32 {
            RwLock<Data> rw = RwLock<Data>(Data());
            { RwWriteGuard<Data> w = rw.wlock(); w.v = 9; }
            mut i32 x = 0;
            { RwReadGuard<Data> r = rw.rlock(); x = r.get(); }
            return 0;
        }
    )");
    REQUIRE_FALSE(ok.hadError);
}

TEST_CASE("Guard - mutating through a RwReadGuard is rejected", "[rwlock][guard][confine]") {
    StderrCapture cap;
    auto result = analyzeString(std::string(RWLOCK_PRELUDE) + R"(
        class Data { mut i32 v; Data() { v = 0; } }
        fn main() -> i32 {
            RwLock<Data> rw = RwLock<Data>(Data());
            { RwReadGuard<Data> r = rw.rlock(); r.v = 5; }
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.str().find("read guard") != std::string::npos);
}

TEST_CASE("Guard - a primitive interior is rejected with an actionable message", "[guard]") {
    // A guard stores the interior as a `T*` field; a primitive `T` would need a (disallowed)
    // primitive-borrow field. The error should steer the user to the scoped closure form.
    StderrCapture cap;
    auto result = analyzeString(std::string(MUTEX_PRELUDE) + R"(
        fn main() -> i32 {
            Mutex<i32> m = Mutex<i32>(0);
            { MutexGuard<i32> g = m.lock(); }
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.str().find("only a CLASS interior") != std::string::npos);
}

TEST_CASE("Guard - var inference of a guard is a valid scoped local", "[guard][var]") {
    auto result = analyzeString(std::string(MUTEX_PRELUDE) + R"(
        class Counter { mut i32 n; Counter() { n = 0; } fn bump() mut { n = n + 1; } }
        fn main() -> i32 {
            Mutex<Counter> m = Mutex<Counter>(Counter());
            { var g = m.lock(); g.bump(); }
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Guard - auto-deref of a nested field + an sret method through a guard", "[guard]") {
    auto result = analyzeString(std::string(MUTEX_PRELUDE) + R"(
        class Inner { mut i32 x; Inner() { x = 0; } }
        class Box {
            mut i32 n; mut Inner inner;
            Box() { n = 0; inner = Inner(); }
            fn snapshot() -> Inner s { s.x = n; return s; }
        }
        fn main() -> i32 {
            Mutex<Box> m = Mutex<Box>(Box());
            {
                MutexGuard<Box> g = m.lock();
                g.inner.x = 7;                 // nested field write via auto-deref
                Inner snap = g.snapshot();     // sret method via auto-deref
            }
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Guard - rlock/wlock lower to the mode-matched unlock helpers", "[rwlock][guard][codegen]") {
    std::string ir = codegenString(std::string(RWLOCK_PRELUDE) + R"(
        class Data { mut i32 v; Data() { v = 0; } }
        fn main() -> i32 {
            RwLock<Data> rw = RwLock<Data>(Data());
            { RwWriteGuard<Data> w = rw.wlock(); }
            { RwReadGuard<Data> r = rw.rlock(); }
            return 0;
        }
    )");
    REQUIRE(ir.find("call void @gg_rwlock_wrlock(")   != std::string::npos);
    REQUIRE(ir.find("call void @gg_rwlock_wrunlock(") != std::string::npos);   // write guard dtor
    REQUIRE(ir.find("call void @gg_rwlock_rdlock(")   != std::string::npos);
    REQUIRE(ir.find("call void @gg_rwlock_rdunlock(") != std::string::npos);   // read guard dtor
}

TEST_CASE("Mutex - Mutex<Shared<T>> is rejected (Phase-1 generic-arg limit)", "[mutex][shared]") {
    StderrCapture cap;
    analyzeString(std::string(MUTEX_PRELUDE) + R"(
        class C { mut i32 n; C() { n = 0; } }
        fn main() -> i32 { Mutex<Shared<C>> m = Mutex<Shared<C>>(Shared<C>()); return 0; }
    )");
    REQUIRE(cap.str().find("Shared<T>") != std::string::npos);
}

TEST_CASE("Mutex - a Shared<Mutex<T>> capture is Sendable", "[mutex][sendable]") {
    auto result = analyzeString(std::string(MUTEX_PRELUDE) + R"(
        extern gg_thread_create(ptr entry, ptr arg) -> ptr;
        extern gg_thread_join(ptr handle);
        fn runThread<F: Call() -> void>(F closure) -> ptr {
            return gg_thread_create(__gg_trampoline(closure), __gg_heap_closure(closure));
        }
        class Counter { mut i32 n; Counter() { n = 0; } }
        fn use(Shared<Mutex<Counter>> m) -> i32 {
            ptr h = runThread(() -> { work(m); });
            gg_thread_join(h);
            return 0;
        }
        fn work(Shared<Mutex<Counter>> m) { m.with((mut Counter* c) -> { c.n = c.n + 1; }); }
        fn main() -> i32 {
            Shared<Mutex<Counter>> m = Shared<Mutex<Counter>>(Counter());
            return use(m);
        }
    )");
    REQUIRE_FALSE(result.hadError);
}
