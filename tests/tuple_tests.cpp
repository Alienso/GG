#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// Tuple types & literals — anonymous `(T1, T2, …)` value objects (arity ≥ 2). A tuple type
// synthesizes an ordinary value-object class `Tuple$T1$T2…` (fields `_0`, `_1`, … + a positional
// constructor); a tuple literal `(a, b)` lowers to a BraceInitExpr whose class is the expected
// tuple type. `._0`/`._1` are plain member accesses. Everything reuses value-object machinery.
// ============================================================

TEST_CASE("tuple - a tuple type synthesizes a value-object struct once", "[tuple][codegen]") {
    std::string ir = codegenString(R"(
        fn divmod(i32 a, i32 b) -> (i32, i32) out { out = (a / b, a % b); return out; }
        fn main() -> i32 { (i32, i32) r = divmod(17, 5); return r._0 + r._1 - 5; }
    )");
    // Struct type emitted exactly once, with the element IR types in order.
    REQUIRE(ir.find("%Tuple$i32$i32 = type { i32, i32 }") != std::string::npos);
    REQUIRE(ir.find("%Tuple$i32$i32 = type", ir.find("%Tuple$i32$i32 = type") + 1) == std::string::npos);
    // A positional constructor exists.
    REQUIRE(ir.find("@Tuple$i32$i32_Tuple$i32$i32(") != std::string::npos);
    // `._0` is a field GEP (index 0) on the tuple struct.
    REQUIRE(ir.find("getelementptr %Tuple$i32$i32") != std::string::npos);
}

TEST_CASE("tuple - nested tuple embeds the inner tuple by value", "[tuple][codegen]") {
    std::string ir = codegenString(R"(
        fn main() -> i32 {
            ((i32, i32), i32) n = ((1, 2), 3);
            return n._0._1 + n._1 - 5;    // 2 + 3 - 5
        }
    )");
    // Outer struct embeds the inner tuple struct inline (not a pointer).
    REQUIRE(ir.find("%Tuple$Tuple$i32$i32$i32 = type { %Tuple$i32$i32, i32 }") != std::string::npos);
    REQUIRE(ir.find("%Tuple$i32$i32 = type { i32, i32 }") != std::string::npos);
}

TEST_CASE("tuple - identical shapes share one synthesized class", "[tuple][codegen]") {
    std::string ir = codegenString(R"(
        fn a() -> (i32, i32) out { out = (1, 2); return out; }
        fn b() -> (i32, i32) out { out = (3, 4); return out; }
        fn main() -> i32 { return 0; }
    )");
    // Two `(i32,i32)` uses → one class definition (dedup by mangled name).
    size_t first = ir.find("%Tuple$i32$i32 = type");
    REQUIRE(first != std::string::npos);
    REQUIRE(ir.find("%Tuple$i32$i32 = type", first + 1) == std::string::npos);
}

TEST_CASE("tuple - an object element is cloned into the value field", "[tuple][codegen]") {
    // Point has a reference field, so its clone is non-trivial and the tuple's generated clone must
    // route the object element through @Point_clone (embedded value-object field).
    std::string ir = codegenString(R"(
        class Node { i32 v; Node(i32 x) { v = x; } }
        class Point { Node& n; Point(Node& node) { n = node; } }
        fn main() -> i32 {
            Node& nd = new Node(1);      // heap reference (a stack value would escape via n)
            Point p(nd);
            (Point, i32) a = (p, 5);
            (Point, i32) b = a;          // clone the tuple → clone the Point element
            return 0;
        }
    )");
    // The tuple field for the object element is the inline %Point struct.
    REQUIRE(ir.find("%Tuple$Point$i32 = type { %Point, i32 }") != std::string::npos);
    // The tuple's generated clone delegates the object element to @Point_clone.
    size_t cloneDef = ir.find("define void @Tuple$Point$i32_clone");
    REQUIRE(cloneDef != std::string::npos);
    size_t cloneEnd = ir.find("define ", cloneDef + 1);
    REQUIRE(ir.substr(cloneDef, cloneEnd - cloneDef).find("call void @Point_clone(ptr ") != std::string::npos);
}

TEST_CASE("tuple - a tuple literal fills the class via its constructor", "[tuple][codegen]") {
    std::string ir = codegenString(R"(
        fn main() -> i32 { (i32, i32) p = (7, 8); return p._0 - 7; }
    )");
    // The literal `(7, 8)` calls the synthesized positional constructor.
    REQUIRE(ir.find("call void @Tuple$i32$i32_Tuple$i32$i32(ptr ") != std::string::npos);
}

TEST_CASE("tuple - a single parenthesized expression is still plain grouping", "[tuple][codegen]") {
    // The comma-detection in the grouping branch must not disturb ordinary `(expr)` grouping (arity 1).
    std::string ir = codegenString(R"(
        fn main() -> i32 { i32 x = (2 + 3); return x - 5; }
    )");
    // No tuple class synthesized for a lone grouped expression.
    REQUIRE(ir.find("%Tuple$") == std::string::npos);
}

TEST_CASE("tuple - a reference element type is rejected in v1", "[tuple][semantic]") {
    // Parse-time rejection (parseString swallows CompileError to stderr and returns an empty program),
    // so assert on the captured message rather than on the analyze result.
    StderrCapture cap;
    (void)analyzeString(R"(
        class C { i32 v; C(i32 x) { v = x; } }
        fn main() -> i32 { (C&, i32) t = (new C(1), 2); return 0; }
    )");
    REQUIRE(cap.contains("tuple elements must be value or primitive"));
}
