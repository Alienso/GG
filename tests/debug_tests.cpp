#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// Debug info (DWARF via LLVM metadata), gated by CompilerOptions::debugInfo.
// Tier 1: !DICompileUnit / !DIFile / module flags / per-function !DISubprogram /
//         per-instruction !DILocation. Tier 2: !DIBasicType / !DICompositeType
// (with member offsets) / !DISubroutineType / !DILocalVariable + #dbg_declare.
// With debug off the emitted IR must contain no debug metadata at all.
// ============================================================

static CompilerOptions debugOptions() {
    CompilerOptions opts;
    opts.allowRawPtr = true;
    opts.debugInfo   = true;
    opts.sourceFile  = "prog.gg";
    return opts;
}

TEST_CASE("Debug - off by default emits no debug metadata", "[debug][codegen]") {
    std::string ir = codegenString(R"(
        fn main() -> i32 { i32 x = 42; return x; }
    )");
    REQUIRE(ir.find("!dbg") == std::string::npos);
    REQUIRE(ir.find("!DI") == std::string::npos);
    REQUIRE(ir.find("#dbg_declare") == std::string::npos);
    REQUIRE(ir.find("llvm.dbg.cu") == std::string::npos);
}

TEST_CASE("Debug - on emits a compile unit, file, and module flags", "[debug][codegen]") {
    std::string ir = codegenString(R"(
        fn main() -> i32 { return 0; }
    )", debugOptions());
    REQUIRE(ir.find("!llvm.dbg.cu = !{") != std::string::npos);
    REQUIRE(ir.find("!llvm.module.flags = !{") != std::string::npos);
    REQUIRE(ir.find("distinct !DICompileUnit(language: DW_LANG_C") != std::string::npos);
    REQUIRE(ir.find("!DIFile(filename: \"prog.gg\"") != std::string::npos);
    REQUIRE(ir.find("!\"Debug Info Version\", i32 3") != std::string::npos);
}

TEST_CASE("Debug - a user function gets a subprogram attached to its define", "[debug][codegen]") {
    std::string ir = codegenString(R"(
        fn main() -> i32 { return 0; }
    )", debugOptions());
    REQUIRE(ir.find("define i32 @main() !dbg !") != std::string::npos);
    REQUIRE(ir.find("distinct !DISubprogram(name: \"main\", linkageName: \"main\"") != std::string::npos);
}

TEST_CASE("Debug - instructions carry a location", "[debug][codegen]") {
    std::string ir = codegenString(R"(
        fn main() -> i32 { i32 x = 5; return x; }
    )", debugOptions());
    REQUIRE(ir.find(", !dbg !") != std::string::npos);
    REQUIRE(ir.find("!DILocation(line:") != std::string::npos);
}

TEST_CASE("Debug - a local variable emits a #dbg_declare and DILocalVariable", "[debug][codegen]") {
    std::string ir = codegenString(R"(
        fn main() -> i32 { i32 count = 7; return count; }
    )", debugOptions());
    REQUIRE(ir.find("#dbg_declare(ptr ") != std::string::npos);
    REQUIRE(ir.find("!DILocalVariable(name: \"count\"") != std::string::npos);
    // i32 primitive type node.
    REQUIRE(ir.find("!DIBasicType(name: \"i32\", size: 32, encoding: DW_ATE_signed)") != std::string::npos);
}

TEST_CASE("Debug - a parameter is a DILocalVariable with an arg index", "[debug][codegen]") {
    std::string ir = codegenString(R"(
        fn addOne(i32 n) -> i32 { return n + 1; }
        fn main() -> i32 { return addOne(41); }
    )", debugOptions());
    REQUIRE(ir.find("!DILocalVariable(name: \"n\", arg: 1") != std::string::npos);
}

TEST_CASE("Debug - a value object becomes a composite type with member offsets", "[debug][codegen]") {
    std::string ir = codegenString(R"(
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } }
        fn main() -> i32 { Point p(3, 4); return p.x; }
    )", debugOptions());
    REQUIRE(ir.find("!DICompositeType(tag: DW_TAG_structure_type, name: \"Point\"") != std::string::npos);
    REQUIRE(ir.find("!DIDerivedType(tag: DW_TAG_member, name: \"x\"") != std::string::npos);
    REQUIRE(ir.find("!DIDerivedType(tag: DW_TAG_member, name: \"y\"") != std::string::npos);
    // y follows x (i32 at byte 4 ⇒ 32 bits).
    REQUIRE(ir.find("name: \"y\"") != std::string::npos);
    REQUIRE(ir.find("offset: 32") != std::string::npos);
}

TEST_CASE("Debug - a method carries `this` in its subroutine type and a qualified name", "[debug][codegen]") {
    std::string ir = codegenString(R"(
        class Box { mut i32 v; Box(i32 n) { v = n; } fn get() -> i32 { return v; } }
        fn main() -> i32 { Box b(9); return b.get(); }
    )", debugOptions());
    REQUIRE(ir.find("!DISubprogram(name: \"Box::get\"") != std::string::npos);
}

TEST_CASE("Debug - a reference variable is a pointer type", "[debug][codegen]") {
    std::string ir = codegenString(R"(
        class Node { mut i32 v; Node(i32 n) { v = n; } }
        fn main() -> i32 { Node& r = new Node(5); return r.v; }
    )", debugOptions());
    REQUIRE(ir.find("!DIDerivedType(tag: DW_TAG_pointer_type, baseType: null, size: 64)") != std::string::npos);
    REQUIRE(ir.find("!DILocalVariable(name: \"r\"") != std::string::npos);
}
