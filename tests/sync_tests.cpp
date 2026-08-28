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
