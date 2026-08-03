#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// Variadic parameters — compile-time type packs (V1 checkpoint: pack plumbing).
// `fn f<...Ts>(Ts... args)` monomorphizes the pack into a TUPLE: a call `f(1, 2, 3)` collects the
// trailing arguments into `Tuple$i32$i32$i32`, mangles `f$Tuple$…`, and the callee receives the pack
// as a tuple borrow. Consumption via cons-`match` is a later phase; here the pack just plumbs through.
// ============================================================

TEST_CASE("variadic - a call collects trailing args into a pack tuple", "[variadic][codegen]") {
    std::string ir = codegenString(R"(
        fn take<...Ts>(Ts... args) -> i32 { return 0; }
        fn main() -> i32 { return take(1, 2, 3); }
    )");
    // The pack tuple type is synthesized, and the monomorphized callee takes it by borrow (ptr).
    REQUIRE(ir.find("%Tuple$i32$i32$i32 = type { i32, i32, i32 }") != std::string::npos);
    REQUIRE(ir.find("@take$Tuple$i32$i32$i32(ptr") != std::string::npos);
}

TEST_CASE("variadic - the pack arrives as an accessible tuple", "[variadic][codegen]") {
    std::string ir = codegenString(R"(
        fn firstOf<...Ts>(Ts... args) -> i32 { return args._0; }
        fn main() -> i32 { return firstOf(42, 7); }
    )");
    // Accessing `args._0` GEPs into the pack tuple.
    REQUIRE(ir.find("getelementptr %Tuple$i32$i32") != std::string::npos);
}

TEST_CASE("variadic - a heterogeneous pack synthesizes a mixed tuple", "[variadic][codegen]") {
    std::string ir = codegenString(R"(
        fn take<...Ts>(Ts... args) -> i32 { return 0; }
        fn main() -> i32 { return take(1, true); }
    )");
    REQUIRE(ir.find("%Tuple$i32$bool = type { i32, i1 }") != std::string::npos);
    REQUIRE(ir.find("@take$Tuple$i32$bool(ptr") != std::string::npos);
}

TEST_CASE("variadic - arity 1 packs into a 1-tuple", "[variadic][codegen]") {
    std::string ir = codegenString(R"(
        fn take<...Ts>(Ts... args) -> i32 { return 0; }
        fn main() -> i32 { return take(5); }
    )");
    REQUIRE(ir.find("%Tuple$i32 = type { i32 }") != std::string::npos);
    REQUIRE(ir.find("@take$Tuple$i32(ptr") != std::string::npos);
}

TEST_CASE("variadic - an empty pack synthesizes the unit tuple", "[variadic][codegen]") {
    std::string ir = codegenString(R"(
        fn take<...Ts>(Ts... args) -> i32 { return 0; }
        fn main() -> i32 { return take(); }
    )");
    // The unit pack is the 0-field tuple class `Tuple`.
    REQUIRE(ir.find("%Tuple = type {") != std::string::npos);
    REQUIRE(ir.find("@take$Tuple(ptr") != std::string::npos);
}

TEST_CASE("variadic - a variadic pack must be the last type parameter", "[variadic][semantic]") {
    StderrCapture cap;
    (void)analyzeString(R"(
        fn bad<...Ts, U>(Ts... args) -> i32 { return 0; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(cap.contains("must be the last type parameter"));
}

TEST_CASE("variadic - fixed parameters precede the pack", "[variadic][codegen]") {
    std::string ir = codegenString(R"(
        fn tagged<...Ts>(i32 tag, Ts... args) -> i32 { return tag; }
        fn main() -> i32 { return tagged(9, 1, 2); }   // tag=9, pack (1,2)
    )");
    // The instantiation takes the fixed `i32` then the pack tuple by borrow (ptr).
    REQUIRE(ir.find("@tagged$Tuple$i32$i32(i32") != std::string::npos);
    REQUIRE(ir.find("%Tuple$i32$i32 = type { i32, i32 }") != std::string::npos);
}

TEST_CASE("variadic - an object (String) pack element is cloned into the tuple", "[variadic][codegen]") {
    std::string ir = codegenString(R"(
        extern malloc(u64 size) -> ptr;
        extern memcpy(ptr dest, ptr src, u64 n) -> ptr;
        extern free(ptr p);
        class Str2 {
            mut ptr buf;
            Str2(str s) { buf = malloc(s.len + 1); memcpy(buf, s.data, s.len + 1); }
            ~Str2() { free(buf); }
        }
        fn take<...Ts>(Ts... args) -> i32 { return 0; }
        fn main() -> i32 { Str2 s("hi"); return take(s, 1); }
    )");
    // The pack tuple embeds the object element by value and clones it in via the tuple constructor.
    REQUIRE(ir.find("%Tuple$Str2$i32 = type { %Str2, i32 }") != std::string::npos);
    REQUIRE(ir.find("@take$Tuple$Str2$i32(ptr") != std::string::npos);
}

TEST_CASE("variadic - explicit type arguments on a variadic are rejected", "[variadic][semantic]") {
    // Explicit `<…>` can't spell a pack and would mis-bind it; reject with a clear message.
    StderrCapture cap;
    (void)analyzeString(R"(
        fn take<...Ts>(Ts... args) -> i32 { return 0; }
        fn main() -> i32 { return take<i32>(5); }
    )");
    REQUIRE(cap.contains("does not take explicit type arguments"));
}

TEST_CASE("variadic - cons-match recursion unrolls at compile time", "[variadic][codegen]") {
    std::string ir = codegenString(R"(
        fn count<...Ts>(Ts... args) -> i32 {
            match args {
                ()     -> { return 0; }
                (x:xs) -> { return 1 + count(xs...); }
            }
        }
        fn main() -> i32 { return count(1, "hi", true); }
    )");
    // Each shorter pack is its own concrete function — the recursion is fully unrolled to the unit
    // base case, with no runtime branch.
    REQUIRE(ir.find("@count$Tuple$i32$str$bool(") != std::string::npos);
    REQUIRE(ir.find("@count$Tuple$str$bool(")     != std::string::npos);
    REQUIRE(ir.find("@count$Tuple$bool(")         != std::string::npos);
    REQUIRE(ir.find("@count$Tuple(")              != std::string::npos);   // unit base case
}

TEST_CASE("variadic - per-element overload resolution in a cons-match fold", "[variadic][codegen]") {
    std::string ir = codegenString(R"(
        fn weight(i32 x)  -> i32 { return 1; }
        fn weight(bool b) -> i32 { return 10; }
        fn sumW<...Ts>(Ts... args) -> i32 {
            match args {
                ()     -> { return 0; }
                (x:xs) -> { return weight(x) + sumW(xs...); }
            }
        }
        fn main() -> i32 { return sumW(1, true); }
    )");
    // The head `x` (= args._0) resolves to the right `weight` overload per concrete element type.
    REQUIRE(ir.find("@weight$i32")  != std::string::npos);
    REQUIRE(ir.find("@weight$bool") != std::string::npos);
}

TEST_CASE("variadic - the head binding reads args._0", "[variadic][codegen]") {
    std::string ir = codegenString(R"(
        fn firstOf<...Ts>(Ts... args) -> i32 {
            match args {
                ()     -> { return 0 - 1; }
                (x:xs) -> { return x; }
            }
        }
        fn main() -> i32 { return firstOf(42, 7); }
    )");
    REQUIRE(ir.find("getelementptr %Tuple$i32$i32") != std::string::npos);   // args._0
}

TEST_CASE("variadic - a fixed accumulator threads through the recursion", "[variadic][codegen]") {
    // The fold/format shape: a fixed parameter carried alongside the shrinking pack (the recursive
    // call passes `acc + x` as the fixed arg and spreads `xs...`).
    std::string ir = codegenString(R"(
        fn go<...Ts>(i32 acc, Ts... args) -> i32 {
            match args {
                ()     -> { return acc; }
                (x:xs) -> { return go(acc + x, xs...); }
            }
        }
        fn main() -> i32 { return go(0, 1, 2, 3); }
    )");
    // Every instantiation keeps the fixed `i32 acc` and takes the (shrinking) pack tuple by borrow.
    REQUIRE(ir.find("@go$Tuple$i32$i32$i32(i32") != std::string::npos);
    REQUIRE(ir.find("@go$Tuple$i32$i32(i32")     != std::string::npos);
    REQUIRE(ir.find("@go$Tuple$i32(i32")         != std::string::npos);
    REQUIRE(ir.find("@go$Tuple(i32")             != std::string::npos);   // base case keeps `acc`
}

TEST_CASE("variadic - a pack match missing the empty arm errors", "[variadic][semantic]") {
    StderrCapture cap;
    (void)analyzeString(R"(
        fn count<...Ts>(Ts... args) -> i32 {
            match args { (x:xs) -> { return 1 + count(xs...); } }
        }
        fn main() -> i32 { return count(1); }
    )");
    REQUIRE(cap.contains("requires a '()' arm"));
}

TEST_CASE("variadic - a non-deducible pack argument is a clean error", "[variadic][semantic]") {
    // A pack argument whose type isn't parser-visible (a call to a non-constructor function) can't be
    // deduced at parse time → a clear error, not a miscompile.
    StderrCapture cap;
    (void)analyzeString(R"(
        fn other() -> i32 { return 1; }
        fn take<...Ts>(Ts... args) -> i32 { return 0; }
        fn main() -> i32 { return take(other()); }
    )");
    REQUIRE(cap.contains("cannot infer the type of a variadic argument"));
}
