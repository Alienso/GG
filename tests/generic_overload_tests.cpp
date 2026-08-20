#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// Generic free-function template OVERLOAD SETS.
// A bare name may have MULTIPLE `fn name<...>` templates (e.g. two `fn print<...Ts>` — a "no
// cursor" entry point that forwards into a "with cursor" recursive one, both loaded from the same
// stdlib module). Previously the second declaration silently overwrote the first in the parser's
// template registry. Disambiguation at each call site: arity/pack-shape gate -> fixed-parameter
// type-compatibility filter -> largest-fixedCount tie-break; a genuine remaining tie is a clean
// parser error, never a silent pick.
// ============================================================

TEST_CASE("GenOverload - two pack overloads coexist and dispatch correctly (IO2.gg repro)",
          "[genoverload][codegen]") {
    // `print<...Ts>(str* formatString, Ts... args)` (fixedCount=1) forwards into
    // `print<...Ts>(str* formatString, i32 cursor, Ts... args)` (fixedCount=2). The external call
    // below passes a non-i32 second argument, so pure arity is ambiguous (both fit) but the
    // fixed-parameter type filter eliminates the two-fixed-param overload (its `i32 cursor` doesn't
    // match a `str` argument) — external call resolves to the one-fixed-param overload; its own
    // internal forwarding call (spread arity: argCount == fixedCount+1) resolves to the
    // two-fixed-param overload.
    std::string ir = codegenString(R"(
        fn print<...Ts> (str* formatString, Ts... args) {
            print(formatString, 0, args...);
        }
        fn print<...Ts> (str* formatString, i32 cursor, Ts... args) {
        }
        fn main() -> i32 {
            str s = "hi";
            print(s, s);
            return 0;
        }
    )");
    // Both templates survive as distinct symbols — direct regression check for the silent-overwrite
    // bug (previously the second `print<...Ts>` declaration replaced the first with no diagnostic).
    // The internal forwarding call spreads the SAME pack tuple through (no re-tupling), so the
    // two-fixed-param overload's instantiation is also keyed on `Tuple$str`.
    REQUIRE(ir.find("define void @print$Tuple$str.ov0(") != std::string::npos);
    REQUIRE(ir.find("define void @print$Tuple$str.ov1(") != std::string::npos);
    // The external call (non-i32 second argument) resolves to the one-fixed-param overload.
    REQUIRE(ir.find("call void @print$Tuple$str.ov0(") != std::string::npos);
    // The internal forwarding call (spread arity) resolves to the two-fixed-param overload.
    REQUIRE(ir.find("call void @print$Tuple$str.ov1(") != std::string::npos);
}

TEST_CASE("GenOverload - non-pack <T> templates overload by arity", "[genoverload][codegen]") {
    std::string ir = codegenString(R"(
        class Box { mut i32 v; Box(i32 x) { this.v = x; } }
        fn foo<T>(T& x) -> i32 { return 1; }
        fn foo<T>(T& x, T& y) -> i32 { return 2; }
        fn main() -> i32 {
            mut Box a = Box(1); mut Box b = Box(2);
            return foo(a) + foo(a, b);
        }
    )");
    REQUIRE(ir.find("define i32 @foo$Box.ov0(") != std::string::npos);
    REQUIRE(ir.find("define i32 @foo$Box.ov1(") != std::string::npos);
}

TEST_CASE("GenOverload - fixed-parameter type mismatch disambiguates a same-arity tie",
          "[genoverload][codegen]") {
    // Both candidates take (T&, <something>) — arity alone ties (both fixedCount=2) — but the
    // second fixed parameter's declared type (i32 vs Bag&) only matches one call's argument type.
    std::string ir = codegenString(R"(
        class Box { mut i32 v; Box(i32 x) { this.v = x; } }
        class Bag { mut i32 w; Bag(i32 x) { this.w = x; } }
        fn pick<T>(T& a, i32 n) -> i32 { return 1; }
        fn pick<T>(T& a, Bag& b) -> i32 { return 2; }
        fn main() -> i32 {
            mut Box x = Box(1); mut Bag y = Bag(2);
            return pick(x, 5) + pick(x, y);
        }
    )");
    REQUIRE(ir.find("define i32 @pick$Box.ov0(") != std::string::npos);
    REQUIRE(ir.find("define i32 @pick$Box.ov1(") != std::string::npos);
}

TEST_CASE("GenOverload - explicit <...> call disambiguates by type-parameter count",
          "[genoverload][codegen]") {
    std::string ir = codegenString(R"(
        class Box { mut i32 v; Box(i32 x) { this.v = x; } }
        fn foo<T>(T& x) -> i32 { return 1; }
        fn foo<A, B>(A& x, B& y) -> i32 { return 2; }
        fn main() -> i32 {
            mut Box a = Box(1); mut Box b = Box(2);
            return foo<Box>(a) + foo<Box, Box>(a, b);
        }
    )");
    REQUIRE(ir.find("define i32 @foo$Box.ov0(") != std::string::npos);
    REQUIRE(ir.find("define i32 @foo$Box$Box.ov1(") != std::string::npos);
}

TEST_CASE("GenOverload - a genuine remaining tie is a clean ambiguous error", "[genoverload][semantic]") {
    // Two candidates with identical shape (same fixedCount, both fixed params leniently
    // undistinguishable) — never a silent pick.
    StderrCapture cap;
    (void)analyzeString(R"(
        class Box { mut i32 v; Box(i32 x) { this.v = x; } }
        fn foo<T>(T& x) -> i32 { return 1; }
        fn foo<T>(T& x) -> i32 { return 2; }
        fn main() -> i32 { mut Box a = Box(1); return foo(a); }
    )");
    REQUIRE(cap.contains("ambiguous"));
}

TEST_CASE("GenOverload - a single-candidate generic mangles byte-identically (no .ov suffix)",
          "[genoverload][codegen]") {
    // Regression guard: the overload-set gate must never fire when a name has only one template —
    // the overwhelming common case for every pre-existing generic function/test.
    std::string ir = codegenString(R"(
        class Box { mut i32 v; Box(i32 x) { this.v = x; } }
        fn unwrap<T>(T& b) -> i32 { return b.v; }
        fn main() -> i32 { mut Box b = Box(7); return unwrap(b); }
    )");
    REQUIRE(ir.find("unwrap$Box(") != std::string::npos);
    REQUIRE(ir.find("unwrap$Box.ov") == std::string::npos);
}

TEST_CASE("GenOverload - a single-candidate variadic template mangles byte-identically",
          "[genoverload][codegen]") {
    std::string ir = codegenString(R"(
        fn take<...Ts>(Ts... args) -> i32 { return 0; }
        fn main() -> i32 { return take(1, 2, 3); }
    )");
    REQUIRE(ir.find("@take$Tuple$i32$i32$i32(ptr") != std::string::npos);
    REQUIRE(ir.find(".ov") == std::string::npos);
}

// ------------------------------------------------------------
// Edge cases found on review (not in the original 7 cases above)
// ------------------------------------------------------------

TEST_CASE("GenOverload - explicit <...> call disambiguates candidates that share type-parameter count",
          "[genoverload][codegen]") {
    // Both candidates have typeParams.size()==1 (only `T`), so the type-parameter-count filter alone
    // can't distinguish them — this exercises the "parse args + fixed-parameter type filter" branch
    // of the EXPLICIT call path specifically (distinct code from the inferred-call path already
    // covered by the "fixed-parameter type mismatch" test above).
    std::string ir = codegenString(R"(
        class Box { mut i32 v; Box(i32 x) { this.v = x; } }
        class Bag { mut i32 w; Bag(i32 x) { this.w = x; } }
        fn pick<T>(T& a, i32 n) -> i32 { return 1; }
        fn pick<T>(T& a, Bag& b) -> i32 { return 2; }
        fn main() -> i32 {
            mut Box x = Box(1); mut Bag y = Bag(2);
            return pick<Box>(x, 5) + pick<Box>(x, y);
        }
    )");
    REQUIRE(ir.find("define i32 @pick$Box.ov0(") != std::string::npos);
    REQUIRE(ir.find("define i32 @pick$Box.ov1(") != std::string::npos);
}

TEST_CASE("GenOverload - an ambiguous EXPLICIT <...> call is a clean error",
          "[genoverload][semantic]") {
    // Two identical-shape candidates sharing typeParams.size()==1: an explicit `foo<Box>(a)` reaches
    // the "multiple candidates share this type-argument count" branch and must still error cleanly,
    // not silently pick one (distinct from the inferred-call ambiguity test above, which never
    // reaches the explicit-call code path at all).
    StderrCapture cap;
    (void)analyzeString(R"(
        class Box { mut i32 v; Box(i32 x) { this.v = x; } }
        fn foo<T>(T& x) -> i32 { return 1; }
        fn foo<T>(T& x) -> i32 { return 2; }
        fn main() -> i32 { mut Box a = Box(1); return foo<Box>(a); }
    )");
    REQUIRE(cap.contains("ambiguous"));
}

TEST_CASE("GenOverload - explicit <...> call with no matching argument shape is a clean error",
          "[genoverload][semantic]") {
    // Both candidates share typeParams.size()==1 (so the type-arg-count filter alone can't reject
    // either), but the call's arity (3 args) matches NEITHER candidate's fixedCount (1 and 2) — must
    // hit the NoMatch path, not silently instantiate a mismatched candidate.
    StderrCapture cap;
    (void)analyzeString(R"(
        class Box { mut i32 v; Box(i32 x) { this.v = x; } }
        fn foo<T>(T& x) -> i32 { return 1; }
        fn foo<T>(T& x, i32 n) -> i32 { return 2; }
        fn main() -> i32 { mut Box a = Box(1); return foo<Box>(a, 1, 2); }
    )");
    REQUIRE(cap.contains("no generic template"));
}

TEST_CASE("GenOverload - a named-argument call to a still-ambiguous EXPLICIT generic call is a clean error",
          "[genoverload][semantic]") {
    // The fixed-parameter type filter indexes arguments POSITIONALLY; a named, out-of-order argument
    // would silently misalign against the wrong parameter position and could select the wrong
    // overload. Must be rejected with a clear parse error instead, whenever disambiguation actually
    // needs the argument shapes (a uniquely-matching candidate is unaffected — see the
    // type-parameter-count disambiguation test above, which uses no named args).
    StderrCapture cap;
    (void)analyzeString(R"(
        class Box { mut i32 v; Box(i32 x) { this.v = x; } }
        class Bag { mut i32 w; Bag(i32 x) { this.w = x; } }
        fn pick<T>(T& a, i32 n) -> i32 { return 1; }
        fn pick<T>(T& a, Bag& b) -> i32 { return 2; }
        fn main() -> i32 { mut Box x = Box(1); return pick<Box>(a: x, n: 5); }
    )");
    REQUIRE(cap.contains("named-argument"));
}

TEST_CASE("GenOverload - a mixed pack/non-pack overload set still resolves an EXPLICIT call "
          "to its non-pack member", "[genoverload][codegen]") {
    // Regression test for a bug found on review: the explicit-call path used to reject the WHOLE
    // overload set with "does not take explicit type arguments" the moment ANY candidate under the
    // name was pack-bearing — even when a non-pack candidate could clearly serve the call. Fixed so
    // only a PURELY pack-bearing set (no non-pack alternative at all) gets that error.
    std::string ir = codegenString(R"(
        class Box { mut i32 v; Box(i32 x) { this.v = x; } }
        fn foo<T>(T& x) -> i32 { return 1; }
        fn foo<...Ts>(Ts... args) -> i32 { return 2; }
        fn main() -> i32 { mut Box a = Box(1); return foo<Box>(a); }
    )");
    REQUIRE(ir.find("define i32 @foo$Box.ov0(") != std::string::npos);
    REQUIRE(ir.find("call i32 @foo$Box.ov0(") != std::string::npos);
}

TEST_CASE("GenOverload - a purely pack-bearing name still rejects explicit type arguments",
          "[genoverload][semantic]") {
    // Regression guard for the fix above: when EVERY candidate under a name is pack-bearing (no
    // non-pack alternative at all), an explicit call must still get the friendlier
    // "the pack is inferred" message, not fall through to candidate selection.
    StderrCapture cap;
    (void)analyzeString(R"(
        fn foo<...Ts>(Ts... args) -> i32 { return 1; }
        fn main() -> i32 { return foo<i32>(1); }
    )");
    REQUIRE(cap.contains("does not take explicit type"));
}

TEST_CASE("GenOverload - a candidate that would reduce the pack to zero elements loses to one "
          "that wouldn't (IO2.gg printf repro)", "[genoverload][codegen]") {
    // `f<...Ts>(str* fmt, Ts... args)` (fixedCount=1) vs `f<...Ts>(str* fmt, i32 cursor, Ts...
    // args)` (fixedCount=2). Calling f(s, 5) is arity-2: the fixedCount=2 candidate's fixed
    // parameters ALSO type-match (str* then i32), so both survive stage 2 and the OLD
    // largest-fixedCount tie-break picked the two-fixed-param candidate — reducing ITS pack to
    // zero elements and silently swallowing "5" as `cursor` instead of a pack argument. That's
    // exactly the real stdlib/io/IO2.gg bug: an external `printf(fmt, 5)` call was hijacked by the
    // "// TODO private" cursor-carrying overload, since a lone i32 substitution value also fits
    // `i32 cursor` and a pack is satisfied trivially by zero elements. The fixedCount=1 candidate's
    // pack is non-empty ([5]) and must now win instead.
    std::string ir = codegenString(R"(
        fn f<...Ts> (str* formatString, Ts... args) {
        }
        fn f<...Ts> (str* formatString, i32 cursor, Ts... args) {
        }
        fn main() -> i32 {
            str s = "hi";
            f(s, 5);
            return 0;
        }
    )");
    REQUIRE(ir.find("call void @f$Tuple$i32.ov0(") != std::string::npos);
    REQUIRE(ir.find("call void @f$Tuple.ov1(") == std::string::npos);
}

TEST_CASE("GenOverload - a mixed pack/non-pack overload set's INFERRED call resolves via the pack "
          "path (documented v1 limitation)", "[genoverload][codegen]") {
    // Not a bug: deduceVariadicInstantiation is tried before inferGenericTypeArgs unconditionally, so
    // a name with both kinds of candidate resolves to the pack candidate whenever the pack's arity
    // gate is satisfiable at all — documented in CLAUDE.md as a v1 limitation. This test pins that
    // documented behavior down concretely so a future change can't silently alter it.
    std::string ir = codegenString(R"(
        class Box { mut i32 v; Box(i32 x) { this.v = x; } }
        fn foo<T>(T& x) -> i32 { return 100; }
        fn foo<...Ts>(Ts... args) -> i32 { return 200; }
        fn main() -> i32 { mut Box a = Box(1); return foo(a); }
    )");
    REQUIRE(ir.find("define i32 @foo$Tuple$Box.ov1(") != std::string::npos);
    REQUIRE(ir.find("call i32 @foo$Tuple$Box.ov1(") != std::string::npos);
}
