#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// Return slots — object-by-value returns via a caller-allocated slot (sret).
//
// Syntax: `name(params) -> RetType slot { ... return slot; }`. The object result is
// written in place into a caller-provided slot, so `return` copies nothing. Lowers to
// an LLVM function returning `void` with a hidden first `ptr` parameter (the slot);
// the caller passes its destination alloca and skips the clone helper.
// ============================================================

// ------------------------------------------------------------
// Parser
// ------------------------------------------------------------

TEST_CASE("ReturnSlot - free function arrow form parses with slot name", "[return-slot][parser]") {
    auto prog = parseString(R"(
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } }
        fn make(i32 a, i32 b) -> Point p { p.x = a; p.y = b; return p; }
    )");
    // Two declarations: the class and the arrow-form function.
    REQUIRE(prog.declarations.size() == 2);
    const auto& fn = asStmt<FunctionDeclStmt>(prog.declarations[1]);
    REQUIRE(fn.name.lexeme == "make");
    REQUIRE(fn.hasReturnSlot);
    REQUIRE(fn.returnSlotName == "p");
    REQUIRE(fn.returnType.lexeme == "Point");
    REQUIRE(fn.params.size() == 2);
}

TEST_CASE("ReturnSlot - method arrow form parses with slot name", "[return-slot][parser]") {
    auto prog = parseString(R"(
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } }
        class Boxed {
            mut i32 v;
            Boxed(i32 a) { v = a; }
            fn toPoint() -> Point q { q.x = v; q.y = v; return q; }
        }
    )");
    const auto& cls = asStmt<ClassDeclStmt>(prog.declarations[1]);
    const MethodDecl* m = nullptr;
    for (const auto& md : cls.methods) if (md.name.lexeme == "toPoint") m = &md;
    REQUIRE(m != nullptr);
    REQUIRE(m->hasReturnSlot);
    REQUIRE(m->returnSlotName == "q");
    REQUIRE(m->returnType.lexeme == "Point");
}

TEST_CASE("ReturnSlot - a plain call statement is not mistaken for a slot function", "[return-slot][parser]") {
    // `foo(x);` inside a body has no '->' after ')', so it stays a call statement.
    auto prog = parseString(R"(
        fn foo(i32 x) { }
        fn main() { foo(3); }
    )");
    REQUIRE(prog.declarations.size() == 2);
    REQUIRE(std::holds_alternative<FunctionDeclStmt>(*prog.declarations[1].node));
}

// ------------------------------------------------------------
// Semantic — accepted
// ------------------------------------------------------------

TEST_CASE("ReturnSlot - filled slot returned is accepted", "[return-slot][semantic]") {
    auto result = analyzeString(R"(
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } fn sum() -> i32 { return x + y; } }
        fn make(i32 a, i32 b) -> Point p { p.x = a; p.y = b; return p; }
        fn main() -> i32 { Point q = make(3, 4); return q.sum(); }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("ReturnSlot - bare return and fall-through are accepted", "[return-slot][semantic]") {
    auto result = analyzeString(R"(
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } }
        fn zero() -> Point p { }               // fall through — slot is zero-initialised
        fn one(i32 a) -> Point p { p.x = a; return; }   // bare return
        fn main() -> i32 { Point z = zero(); Point o = one(5); return 0; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("ReturnSlot - method return slot is accepted", "[return-slot][semantic]") {
    auto result = analyzeString(R"(
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } fn sum() -> i32 { return x + y; } }
        class Boxed {
            mut i32 v;
            Boxed(i32 a) { v = a; }
            fn toPoint() -> Point q { q.x = v; q.y = v; return q; }
        }
        fn main() -> i32 { Boxed& b = new Boxed(4); Point p = b.toPoint(); return p.sum(); }
    )");
    REQUIRE_FALSE(result.hadError);
}

// ------------------------------------------------------------
// Semantic — rejected
// ------------------------------------------------------------

TEST_CASE("ReturnSlot - object return without a slot is an error", "[return-slot][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class P { mut i32 x; P(i32 a) { x = a; } }
        fn foo(i32 a) -> P { P p(a); return p; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("requires a return alias"));
}

TEST_CASE("ReturnSlot - a void function cannot declare a return alias", "[return-slot][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn build(i32 a) -> void s { return; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("void function cannot declare a return alias"));
}

TEST_CASE("ReturnSlot - returning a non-slot expression is an error", "[return-slot][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class P { mut i32 x; P(i32 a) { x = a; } }
        fn mk() -> P p { P other(9); return other; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("may only 'return"));
}

// ------------------------------------------------------------
// CodeGen
// ------------------------------------------------------------

TEST_CASE("ReturnSlot - function lowers to void with a hidden slot parameter", "[return-slot][codegen]") {
    std::string ir = codegenString(R"(
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } }
        fn make(i32 a, i32 b) -> Point p { p.x = a; p.y = b; return p; }
        fn main() -> i32 { Point q = make(3, 4); return 0; }
    )");
    REQUIRE(ir.find("define void @make(ptr %p") != std::string::npos);
    REQUIRE(ir.find("store %Point zeroinitializer, ptr %p") != std::string::npos);
    REQUIRE(ir.find("ret void") != std::string::npos);
}

TEST_CASE("ReturnSlot - caller passes its slot and skips the clone helper", "[return-slot][codegen]") {
    std::string ir = codegenString(R"(
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } }
        fn make(i32 a, i32 b) -> Point p { p.x = a; p.y = b; return p; }
        fn main() -> i32 { Point q = make(3, 4); return 0; }
    )");
    // The result is written into the caller's own alloca — no @Point_clone on the return path.
    REQUIRE(ir.find("call void @make(ptr %q") != std::string::npos);
    REQUIRE(ir.find("@Point_clone") == std::string::npos);
}

TEST_CASE("ReturnSlot - method lowers with slot before the receiver", "[return-slot][codegen]") {
    std::string ir = codegenString(R"(
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } }
        class Boxed {
            mut i32 v;
            Boxed(i32 a) { v = a; }
            fn toPoint() -> Point q { q.x = v; q.y = v; return q; }
        }
        fn main() -> i32 { Boxed& b = new Boxed(4); Point p = b.toPoint(); return 0; }
    )");
    REQUIRE(ir.find("define void @Boxed_toPoint(ptr %q, ptr %self)") != std::string::npos);
}

// ------------------------------------------------------------
// Return slots on generics (templates)
// ------------------------------------------------------------

TEST_CASE("ReturnSlot - generic function with an arrow return slot is accepted", "[return-slot][generic][semantic]") {
    auto result = analyzeString(R"(
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } fn sum() -> i32 { return x + y; } }
        fn dup<T>(T v) -> Point p { p.x = v; p.y = v; return p; }
        fn main() -> i32 { Point a = dup<i32>(7); return a.sum(); }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("ReturnSlot - generic function monomorphizes to per-type sret functions", "[return-slot][generic][codegen]") {
    std::string ir = codegenString(R"(
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } }
        fn dup<T>(T v) -> Point p { p.x = v; p.y = v; return p; }
        fn main() -> i32 { Point a = dup<i32>(7); return 0; }
    )");
    // The template lowers to a concrete, mangled, void-returning sret function.
    REQUIRE(ir.find("define void @dup$i32(ptr %p") != std::string::npos);
    REQUIRE(ir.find("call void @dup$i32(ptr %a") != std::string::npos);
}

TEST_CASE("ReturnSlot - slot type may be the type parameter", "[return-slot][generic][codegen]") {
    std::string ir = codegenString(R"(
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } }
        class Vec2 { mut i32 a; mut i32 b; Vec2(i32 x, i32 y) { a = x; b = y; } }
        fn zero<T>() -> T out { }
        fn main() -> i32 { Point p = zero<Point>(); Vec2 v = zero<Vec2>(); return 0; }
    )");
    // One monomorphization per instantiation, each writing into a caller slot of that type.
    REQUIRE(ir.find("define void @zero$Point(ptr %out)") != std::string::npos);
    REQUIRE(ir.find("define void @zero$Vec2(ptr %out)")  != std::string::npos);
    REQUIRE(ir.find("call void @zero$Point(ptr %p")      != std::string::npos);
}

TEST_CASE("ReturnSlot - arrow method inside a generic class monomorphizes", "[return-slot][generic][codegen]") {
    std::string ir = codegenString(R"(
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } }
        class Wrap<T> {
            mut i32 seed;
            Wrap(i32 s) { seed = s; }
            fn build() -> Point p { p.x = seed; p.y = seed + 1; return p; }
        }
        fn main() -> i32 { Wrap<i32>& w = new Wrap<i32>(10); Point q = w.build(); return 0; }
    )");
    // The generic class's arrow method is emitted on the monomorphized class, sret-lowered.
    REQUIRE(ir.find("define void @Wrap$i32_build(ptr %p, ptr %self)") != std::string::npos);
}

// ------------------------------------------------------------
// Unified signature: void = no arrow; primitive / reference aliases
// ------------------------------------------------------------

TEST_CASE("Unified - omitting the arrow means a void return", "[return-slot][parser]") {
    auto prog = parseString(R"( fn greet(ptr name) { } )");
    REQUIRE(prog.declarations.size() == 1);
    const auto& fn = asStmt<FunctionDeclStmt>(prog.declarations[0]);
    REQUIRE(fn.returnType.type == TokenType::VOID);
    REQUIRE_FALSE(fn.hasReturnSlot);
}

TEST_CASE("Unified - a primitive return alias parses", "[return-slot][parser]") {
    auto prog = parseString(R"( fn calc(i32 n) -> i32 r { r = n; return r; } )");
    const auto& fn = asStmt<FunctionDeclStmt>(prog.declarations[0]);
    REQUIRE(fn.hasReturnSlot);
    REQUIRE(fn.returnSlotName == "r");
    REQUIRE(fn.returnType.type == TokenType::I32);
}

TEST_CASE("Unified - primitive alias (zero-init) is accepted and returns by value", "[return-slot][semantic][codegen]") {
    std::string ir = codegenString(R"(
        fn calc(i32 n) -> i32 r { r = n * 2; return r; }
        fn zeroDefault() -> i32 r { }
        fn main() -> i32 { return calc(21) + zeroDefault(); }
    )");
    // Primitive alias is an ordinary by-value return (not sret): the function still returns i32.
    REQUIRE(ir.find("define i32 @calc(i32 %n)") != std::string::npos);
    REQUIRE(ir.find("ptr sret") == std::string::npos);
}

TEST_CASE("Unified - a reference return with no alias is accepted", "[return-slot][semantic]") {
    // The classic `-> Class&` return (no alias) still works — +1 reference convention.
    auto result = analyzeString(R"(
        class Node { mut i32 v; Node(i32 a) { v = a; } fn get() -> i32 { return v; } }
        fn firstOf(Node& a, Node& b) -> Node& { return a; }
        fn main() -> i32 { Node& a = new Node(1); Node& b = new Node(2); return firstOf(a, b).get(); }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Unified - reference alias assigned before return is accepted", "[return-slot][semantic]") {
    auto result = analyzeString(R"(
        class Node { mut i32 v; Node(i32 a) { v = a; } fn get() -> i32 { return v; } }
        fn pick(i32 n) -> Node& out { out = new Node(n); return out; }
        fn main() -> i32 { Node& r = pick(7); return r.get(); }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Unified - reference alias returned unassigned is an error", "[return-slot][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Node { mut i32 v; Node(i32 a) { v = a; } }
        fn bad() -> Node& r { return; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("returned before it is assigned"));
}

TEST_CASE("Unified - extern uses the arrow form", "[return-slot][parser]") {
    auto prog = parseString(R"( extern puts(ptr s) -> i32; )");
    REQUIRE(prog.declarations.size() == 1);
    const auto& ext = asStmt<ExternFuncDeclStmt>(prog.declarations[0]);
    REQUIRE(ext.name.lexeme == "puts");
    REQUIRE(ext.returnType.type == TokenType::I32);
}

TEST_CASE("Unified - a mutating method uses 'mut' before the arrow", "[return-slot][parser]") {
    auto prog = parseString(R"(
        class C { mut i32 v; C(i32 a){ v=a; } fn bump() mut -> i32 { v = v + 1; return v; } }
    )");
    const auto& cls = asStmt<ClassDeclStmt>(prog.declarations[0]);
    const MethodDecl* m = nullptr;
    for (const auto& md : cls.methods) if (md.name.lexeme == "bump") m = &md;
    REQUIRE(m != nullptr);
    REQUIRE(m->isMut);
    REQUIRE(m->returnType.type == TokenType::I32);
}
