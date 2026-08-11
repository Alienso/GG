#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// Generic methods — `fn method<T>(...)` on a class (its own type parameters, distinct from any class
// type params). Captured as a per-class method template in the parser; a call `obj.m<i32>(...)`
// resolves the receiver's class at parse time (parser-visible receivers only), mangles `m$i32`, and
// the concrete method is re-parsed + injected into the owner class so semantics/codegen treat it as
// an ordinary `@Class_m$i32`. Works on generic classes too (deferral fixpoint at monomorphization).
// ============================================================

TEST_CASE("genericmethod - instance method on a non-generic class emits the mangled symbol", "[genericmethod][codegen]") {
    std::string ir = codegenString(R"(
        class Box { mut i32 v; Box(i32 x) { v = x; } fn wrap<T>(T x) -> T { return x; } }
        fn main() -> i32 { Box b(1); return b.wrap<i32>(5); }
    )");
    REQUIRE(ir.find("define i32 @Box_wrap$i32(ptr ") != std::string::npos);
    REQUIRE(ir.find("call i32 @Box_wrap$i32(ptr ") != std::string::npos);
}

TEST_CASE("genericmethod - two instantiations are both emitted and deduped", "[genericmethod][codegen]") {
    std::string ir = codegenString(R"(
        class Box { fn wrap<T>(T x) -> T { return x; } }
        fn main() -> i32 {
            Box b;
            i32 a = b.wrap<i32>(5);
            i32 c = b.wrap<i32>(6);   // same instantiation — deduped
            bool d = b.wrap<bool>(true);
            return a + c;
        }
    )");
    REQUIRE(ir.find("define i32 @Box_wrap$i32(") != std::string::npos);
    REQUIRE(ir.find("define i1 @Box_wrap$bool(") != std::string::npos);
    // Deduped: only one definition of the i32 instantiation.
    size_t first = ir.find("define i32 @Box_wrap$i32(");
    REQUIRE(ir.find("define i32 @Box_wrap$i32(", first + 1) == std::string::npos);
}

TEST_CASE("genericmethod - static generic method via Class::make<T>()", "[genericmethod][codegen]") {
    std::string ir = codegenString(R"(
        class Factory { fn static make<T>(T x) -> T { return x; } }
        fn main() -> i32 { return Factory::make<i32>(3); }
    )");
    // A static method takes no receiver.
    REQUIRE(ir.find("define i32 @Factory_make$i32(i32 ") != std::string::npos);
    REQUIRE(ir.find("call i32 @Factory_make$i32(i32 ") != std::string::npos);
}

TEST_CASE("genericmethod - a this-receiver generic call resolves the current class", "[genericmethod][codegen]") {
    std::string ir = codegenString(R"(
        class C {
            fn selfCall<T>(T x) -> T { return x; }
            fn viaThis(i32 n) -> i32 { return this.selfCall<i32>(n); }
        }
        fn main() -> i32 { C c; return c.viaThis(4); }
    )");
    REQUIRE(ir.find("define i32 @C_selfCall$i32(ptr ") != std::string::npos);
    REQUIRE(ir.find("call i32 @C_selfCall$i32(ptr ") != std::string::npos);   // from viaThis via `this`
}

TEST_CASE("genericmethod - a bare instance-field receiver resolves via classFieldScope", "[genericmethod][codegen]") {
    std::string ir = codegenString(R"(
        class Inner { fn wrap<T>(T x) -> T { return x; } }
        class Outer {
            Inner inner;
            fn run(i32 n) -> i32 { return inner.wrap<i32>(n); }   // bare field as receiver
        }
        fn main() -> i32 { Outer o; return o.run(7); }
    )");
    REQUIRE(ir.find("define i32 @Inner_wrap$i32(ptr ") != std::string::npos);
}

TEST_CASE("genericmethod - generic method on a GENERIC class (deferral fixpoint)", "[genericmethod][codegen]") {
    std::string ir = codegenString(R"(
        class Holder<E> {
            mut E value;
            Holder(E v) { value = v; }
            fn convert<U>(U y) -> U { return y; }
        }
        fn main() -> i32 { Holder<i32> h(1); return h.convert<i32>(7); }
    )");
    // The method instantiates into the concrete (mangled) class after it is re-parsed.
    REQUIRE(ir.find("define i32 @Holder$i32_convert$i32(ptr ") != std::string::npos);
    REQUIRE(ir.find("call i32 @Holder$i32_convert$i32(ptr ") != std::string::npos);
}

TEST_CASE("genericmethod - a generic method coexists with a non-generic overload of the same name", "[genericmethod][codegen]") {
    std::string ir = codegenString(R"(
        class Box {
            fn wrap(i32 x) -> i32 { return x + 1; }   // non-generic
            fn wrap<T>(T x) -> T { return x; }          // generic
        }
        fn main() -> i32 {
            Box b;
            i32 a = b.wrap(10);        // non-generic → @Box_wrap
            i32 c = b.wrap<i32>(20);   // generic     → @Box_wrap$i32
            return a + c;
        }
    )");
    REQUIRE(ir.find("@Box_wrap$i32(") != std::string::npos);   // the generic instantiation
    REQUIRE(ir.find("@Box_wrap(")     != std::string::npos);   // the plain overload, distinct symbol
}

TEST_CASE("genericmethod - two classes with the same generic method name do not collide", "[genericmethod][codegen]") {
    std::string ir = codegenString(R"(
        class A { fn wrap<T>(T x) -> T { return x; } }
        class B { fn wrap<T>(T x) -> T { return x; } }
        fn main() -> i32 { A a; B b; return a.wrap<i32>(1) + b.wrap<i32>(2); }
    )");
    REQUIRE(ir.find("define i32 @A_wrap$i32(") != std::string::npos);
    REQUIRE(ir.find("define i32 @B_wrap$i32(") != std::string::npos);
}

TEST_CASE("genericmethod - object-return generic method uses the sret convention", "[genericmethod][codegen]") {
    std::string ir = codegenString(R"(
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } }
        class Maker { fn make<T>(i32 a, i32 b) -> T out { out = Point(a, b); } }
        fn main() -> i32 { Maker m; Point p = m.make<Point>(2, 3); return p.x; }
    )");
    // Object return → hidden sret slot pointer, void LLVM return.
    REQUIRE(ir.find("define void @Maker_make$Point(ptr ") != std::string::npos);
}

TEST_CASE("genericmethod - calling a mut generic method through an immutable binding errors", "[genericmethod][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Box { mut i32 v; Box() { v = 0; } fn set<T>(T x) mut { v = 1; } }
        fn main() -> i32 { Box b; b.set<i32>(5); return 0; }   // b is immutable
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("immutable binding"));
}

TEST_CASE("genericmethod - a chained/call-result receiver is a clean error", "[genericmethod][semantic]") {
    StderrCapture cap;
    (void)analyzeString(R"(
        class Box { fn wrap<T>(T x) -> T { return x; } }
        fn getBox() -> Box b { b = Box(); }
        fn main() -> i32 { return getBox().wrap<i32>(5); }
    )");
    REQUIRE(cap.contains("cannot determine the receiver's type for a generic-method call"));
}

TEST_CASE("genericmethod - a bounded generic method enforces its trait bound", "[genericmethod][semantic]") {
    // Satisfied: `Widget` implements `Show`, so `run<Widget>` compiles.
    auto ok = analyzeString(R"(
        trait Show { fn show() -> i32; }
        class Widget { mut i32 v; Widget() { v = 7; } }
        impl Show for Widget { fn show() -> i32 { return 7; } }
        class Runner { fn run<T: Show>(T* w) -> i32 { return w.show(); } }
        fn main() -> i32 { Runner r; Widget wi; return r.run<Widget>(wi); }
    )");
    REQUIRE_FALSE(ok.hadError);

    // Unsatisfied: `Plain` does not implement `Show` → a bound error at the instantiation.
    StderrCapture cap;
    auto bad = analyzeString(R"(
        trait Show { fn show() -> i32; }
        class Plain { mut i32 v; Plain() { v = 0; } }
        class Runner { fn run<T: Show>(T* w) -> i32 { return 0; } }
        fn main() -> i32 { Runner r; Plain p; return r.run<Plain>(p); }
    )");
    REQUIRE(bad.hadError);
    REQUIRE(cap.contains("does not satisfy bound 'Show'"));
}

TEST_CASE("genericmethod - a recursive `this.m<T>()` call resolves (shell named the real owner)", "[genericmethod][codegen]") {
    // The instantiated method is re-parsed inside a shell class named the REAL owner, so a
    // `this.countDown<i32>()` self-recursion inside the body resolves `this` to the right class.
    std::string ir = codegenString(R"(
        class C {
            fn countDown<T>(T x, i32 n) -> i32 {
                if (n <= 0) { return 0; }
                return 1 + this.countDown<T>(x, n - 1);
            }
        }
        fn main() -> i32 { C c; return c.countDown<i32>(9, 5); }
    )");
    REQUIRE(ir.find("define i32 @C_countDown$i32(ptr ") != std::string::npos);
    // The recursive call targets the same instantiation (not a re-mangled or missing symbol).
    size_t def = ir.find("define i32 @C_countDown$i32(");
    REQUIRE(ir.find("call i32 @C_countDown$i32(", def) != std::string::npos);
}

TEST_CASE("genericmethod - multiple type parameters mangle in order", "[genericmethod][codegen]") {
    std::string ir = codegenString(R"(
        class P { fn pick<K, V>(K a, V b) -> K { return a; } }
        fn main() -> i32 { P p; return p.pick<i32, bool>(7, true); }
    )");
    REQUIRE(ir.find("define i32 @P_pick$i32$bool(ptr ") != std::string::npos);
}

TEST_CASE("genericmethod - a generic method may call another generic method via this", "[genericmethod][codegen]") {
    std::string ir = codegenString(R"(
        class C {
            fn inner<T>(T x) -> T { return x; }
            fn outer<T>(T x) -> T { return this.inner<T>(x); }
        }
        fn main() -> i32 { C c; return c.outer<i32>(6); }
    )");
    REQUIRE(ir.find("define i32 @C_outer$i32(") != std::string::npos);
    REQUIRE(ir.find("define i32 @C_inner$i32(") != std::string::npos);
    REQUIRE(ir.find("call i32 @C_inner$i32(") != std::string::npos);
}

TEST_CASE("genericmethod - a default parameter value is filled at the call site", "[genericmethod][codegen]") {
    std::string ir = codegenString(R"(
        class C { fn wrap<T>(T x, i32 add = 3) -> i32 { return add; } }
        fn main() -> i32 { C c; return c.wrap<i32>(5); }   // add defaults to 3
    )");
    REQUIRE(ir.find("i32 5, i32 3") != std::string::npos);   // the default is materialized in the call
}

TEST_CASE("genericmethod - a `?.` safe call on a nullable receiver works", "[genericmethod][codegen]") {
    std::string ir = codegenString(R"(
        class Box { mut i32 v; Box(i32 x) { v = x; } fn wrap<T>(T x) -> T { return x; } }
        fn main() -> i32 {
            Box&? b = new Box(1);
            i32? r = b?.wrap<i32>(9);
            if (r == null) { return 0; }
            return r!!;
        }
    )");
    REQUIRE(ir.find("@Box_wrap$i32(") != std::string::npos);
    REQUIRE(ir.find("icmp ne ptr") != std::string::npos);   // the null-check of the `?.` receiver
}

TEST_CASE("genericmethod - a generic free function and a generic method of the same name coexist", "[genericmethod][codegen]") {
    std::string ir = codegenString(R"(
        fn wrap<T>(T x) -> T { return x; }              // free
        class Box { fn wrap<T>(T x) -> T { return x + 1; } }   // method
        fn main() -> i32 { Box b; return wrap<i32>(10) + b.wrap<i32>(20); }
    )");
    REQUIRE(ir.find("define i32 @wrap$i32(i32 ") != std::string::npos);       // free (no receiver)
    REQUIRE(ir.find("define i32 @Box_wrap$i32(ptr ") != std::string::npos);   // method (receiver)
}

TEST_CASE("genericmethod - a nested generic type argument mangles", "[genericmethod][codegen]") {
    std::string ir = codegenString(R"(
        class Holder<E> { mut E v; Holder(E x) { v = x; } }
        class Box { fn stash<T>(T* x) -> i32 { return 0; } }
        fn main() -> i32 { Box b; Holder<i32> h(5); return b.stash<Holder<i32>>(h); }
    )");
    REQUIRE(ir.find("@Box_stash$Holder$i32(") != std::string::npos);
}

TEST_CASE("genericmethod - a param-typed receiver resolves", "[genericmethod][codegen]") {
    std::string ir = codegenString(R"(
        class Box { fn wrap<T>(T x) -> T { return x; } }
        fn run(Box& b) -> i32 { return b.wrap<i32>(8); }   // `b` is a parameter
        fn main() -> i32 { Box& b = new Box(); return run(b); }
    )");
    REQUIRE(ir.find("@Box_wrap$i32(ptr ") != std::string::npos);
}

TEST_CASE("genericmethod - a var-typed receiver is a clean error (v1 limit)", "[genericmethod][semantic]") {
    // `var` records a sentinel type token at parse time, so the receiver's class isn't parser-visible
    // — the same limitation as generic type-argument inference over `var`. A clean, actionable error.
    StderrCapture cap;
    (void)analyzeString(R"(
        class Box { fn wrap<T>(T x) -> T { return x; } }
        fn main() -> i32 { var b = Box(); return b.wrap<i32>(5); }
    )");
    REQUIRE(cap.contains("cannot determine the receiver's type"));
}

TEST_CASE("genericmethod - a `<` comparison of a member named like a generic method still parses", "[genericmethod][codegen]") {
    // `b.count < n` must stay a less-than comparison even though some class has a generic method named
    // `count` — the disambiguation only treats `<` as type-args when it closes into a `(`.
    std::string ir = codegenString(R"(
        class Other { fn count<T>(T x) -> T { return x; } }
        class Box { mut i32 count; Box() { count = 3; } fn c() -> i32 { return count; } }
        fn main() -> i32 { Box b; i32 n = 5; if (b.c() < n) { return 1; } return 0; }
    )");
    REQUIRE(ir.find("icmp slt") != std::string::npos);   // the comparison survived
}
