#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// Parser tests
// ============================================================

TEST_CASE("Enum - basic enum parses to EnumDeclStmt", "[enum][parser]") {
    auto prog = parseString(R"(
        enum Color {
            RED,
            GREEN,
            BLUE
        }
    )");
    REQUIRE(prog.declarations.size() == 1);
    const auto& en = asStmt<EnumDeclStmt>(prog.declarations[0]);
    REQUIRE(en.name.lexeme == "Color");
    REQUIRE(en.variants.size() == 3);
    REQUIRE(en.variants[0].name.lexeme == "RED");
    REQUIRE(en.variants[1].name.lexeme == "GREEN");
    REQUIRE(en.variants[2].name.lexeme == "BLUE");
    REQUIRE(en.variants[0].args.empty());
    REQUIRE(en.fields.empty());
    REQUIRE(en.methods.empty());
}

TEST_CASE("Enum - variants with constructor args, fields and methods parse", "[enum][parser]") {
    auto prog = parseString(R"(
        enum Planet {
            EARTH(5.976, 6.37814),
            MARS(0.642, 3.397);

            f64 mass;
            f64 radius;

            Planet(f64 mass, f64 radius) {
                this.mass = mass;
                this.radius = radius;
            }

            fn getMass() -> f64 { return this.mass; }
        }
    )");
    REQUIRE(prog.declarations.size() == 1);
    const auto& en = asStmt<EnumDeclStmt>(prog.declarations[0]);
    REQUIRE(en.variants.size() == 2);
    REQUIRE(en.variants[0].args.size() == 2);
    REQUIRE(en.variants[1].args.size() == 2);
    REQUIRE(en.fields.size() == 2);
    REQUIRE(en.methods.size() == 2);   // constructor + getMass
    // constructor is one of the methods
    bool hasCtor = false;
    for (const auto& m : en.methods) if (m.isConstructor) hasCtor = true;
    REQUIRE(hasCtor);
}

// ============================================================
// Semantic tests
// ============================================================

TEST_CASE("Enum - valid enum with fields and methods passes", "[enum][semantic]") {
    auto result = analyzeString(R"(
        enum Planet {
            EARTH(5.976, 6.37814),
            MARS(0.642, 3.397);

            f64 mass;
            f64 radius;

            Planet(f64 mass, f64 radius) {
                this.mass = mass;
                this.radius = radius;
            }

            fn getMass() -> f64 { return this.mass; }
        }
    )");
    REQUIRE_FALSE(result.hadError);
    REQUIRE(result.enumRegistry.count("Planet") == 1);
}

TEST_CASE("Enum - static variant access yields the enum type", "[enum][semantic]") {
    auto result = analyzeString(R"(
        enum Color { RED, GREEN, BLUE }
        fn f() {
            Color c = Color.GREEN;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Enum - identity comparison of same enum is allowed", "[enum][semantic]") {
    auto result = analyzeString(R"(
        enum Color { RED, GREEN }
        fn f() -> bool {
            Color c = Color.RED;
            return c == Color.GREEN;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Enum - unknown variant is an error", "[enum][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        enum Color { RED, GREEN }
        fn f() {
            Color c = Color.PURPLE;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("no variant"));
}

TEST_CASE("Enum - direct construction is an error", "[enum][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        enum Color { RED, GREEN }
        fn f() {
            Color c = Color();
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("cannot construct enum"));
}

TEST_CASE("Enum - new on an enum is an error", "[enum][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        enum Color { RED, GREEN }
        fn f() {
            Color c = new Color();
        }
    )");
    REQUIRE(result.hadError);
    // Assert the `new`-specific message, not just "enum" (which also matches the neighbouring
    // "cannot construct enum" diagnostic).
    REQUIRE(cap.contains("'new' an enum"));
}

TEST_CASE("Enum - assigning to an enum field outside the constructor is an error", "[enum][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        enum Planet {
            EARTH(5.976);
            f64 mass;
            Planet(f64 mass) { this.mass = mass; }
        }
        fn f() {
            Planet p = Planet.EARTH;
            p.mass = 1.0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("immutable"));
}

TEST_CASE("Enum - variant arg count mismatch is an error", "[enum][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        enum Planet {
            EARTH(5.976, 6.37814),
            MARS(0.642);
            f64 mass;
            f64 radius;
            Planet(f64 mass, f64 radius) {
                this.mass = mass;
                this.radius = radius;
            }
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("constructor expects"));
}

TEST_CASE("Enum - field not initialised in constructor is an error", "[enum][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        enum Planet {
            EARTH(5.976);
            f64 mass;
            f64 radius;
            Planet(f64 mass) { this.mass = mass; }
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("not initialised"));
}

TEST_CASE("Enum - destructor is rejected", "[enum][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        enum Color {
            RED;
            ~Color() {}
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("destructor"));
}

// ============================================================
// CodeGen tests
// ============================================================

TEST_CASE("Enum - struct type and variant globals are emitted", "[enum][codegen]") {
    auto ir = codegenString(R"(
        enum Planet {
            EARTH(5.976, 6.37814),
            MARS(0.642, 3.397);
            f64 mass;
            f64 radius;
            Planet(f64 mass, f64 radius) {
                this.mass = mass;
                this.radius = radius;
            }
        }
    )");
    REQUIRE(ir.find("%Planet = type { double, double }") != std::string::npos);
    REQUIRE(ir.find("@Planet$EARTH = global %Planet zeroinitializer") != std::string::npos);
    REQUIRE(ir.find("@Planet$MARS = global %Planet zeroinitializer") != std::string::npos);
}

TEST_CASE("Enum - fieldless enum gets a padding byte for distinct addresses", "[enum][codegen]") {
    auto ir = codegenString(R"(
        enum Color { RED, GREEN, BLUE }
        fn f() { Color c = Color.RED; }
    )");
    REQUIRE(ir.find("%Color = type { i8 }") != std::string::npos);
}

TEST_CASE("Enum - constructor runs in gg_enum_init registered in global_ctors", "[enum][codegen]") {
    auto ir = codegenString(R"(
        enum Planet {
            EARTH(5.976, 6.37814);
            f64 mass;
            f64 radius;
            Planet(f64 mass, f64 radius) {
                this.mass = mass;
                this.radius = radius;
            }
        }
    )");
    REQUIRE(ir.find("define void @gg_enum_init()") != std::string::npos);
    REQUIRE(ir.find("call void @Planet_Planet(ptr @Planet$EARTH") != std::string::npos);
    REQUIRE(ir.find("@llvm.global_ctors") != std::string::npos);
    REQUIRE(ir.find("@gg_enum_init") != std::string::npos);
}

TEST_CASE("Enum - fieldless enum emits no gg_enum_init", "[enum][codegen]") {
    auto ir = codegenString(R"(
        enum Color { RED, GREEN, BLUE }
        fn f() { Color c = Color.RED; }
    )");
    REQUIRE(ir.find("@gg_enum_init") == std::string::npos);
    REQUIRE(ir.find("@llvm.global_ctors") == std::string::npos);
}

TEST_CASE("Enum - static variant access lowers to the global address", "[enum][codegen]") {
    auto ir = codegenString(R"(
        enum Color { RED, GREEN, BLUE }
        fn f() { Color c = Color.GREEN; }
    )");
    REQUIRE(ir.find("store ptr @Color$GREEN") != std::string::npos);
}

TEST_CASE("Enum - identity comparison lowers to icmp on ptr", "[enum][codegen]") {
    auto ir = codegenString(R"(
        enum Color { RED, GREEN }
        fn f() -> bool {
            Color c = Color.RED;
            return c == Color.GREEN;
        }
    )");
    REQUIRE(ir.find("icmp eq ptr") != std::string::npos);
}

TEST_CASE("Enum - passed by value as a function parameter", "[enum][semantic]") {
    auto result = analyzeString(R"(
        enum Color { RED, GREEN, BLUE }
        fn isRed(Color c) -> bool { return c == Color.RED; }
        fn f() { bool b = isRed(Color.GREEN); }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Enum - returned by value from a function", "[enum][semantic]") {
    auto result = analyzeString(R"(
        enum Color { RED, GREEN, BLUE }
        fn pick(bool b) -> Color {
            if (b) { return Color.GREEN; }
            return Color.BLUE;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Enum - value param lowers to ptr", "[enum][codegen]") {
    auto ir = codegenString(R"(
        enum Color { RED, GREEN, BLUE }
        fn isRed(Color c) -> bool { return c == Color.RED; }
    )");
    REQUIRE(ir.find("define i1 @isRed(ptr %c)") != std::string::npos);
}

TEST_CASE("Enum - return-by-value lowers to ptr (not i32)", "[enum][codegen]") {
    auto ir = codegenString(R"(
        enum Color { RED, GREEN, BLUE }
        fn pick(bool b) -> Color {
            if (b) { return Color.GREEN; }
            return Color.BLUE;
        }
    )");
    REQUIRE(ir.find("define ptr @pick(i1 %b)") != std::string::npos);
    REQUIRE(ir.find("ret ptr @Color$GREEN") != std::string::npos);
    REQUIRE(ir.find("ret i32 @Color") == std::string::npos);
}

TEST_CASE("Enum - method call dispatches on the variant", "[enum][codegen]") {
    auto ir = codegenString(R"(
        enum Planet {
            EARTH(5.976);
            f64 mass;
            Planet(f64 mass) { this.mass = mass; }
            fn getMass() -> f64 { return this.mass; }
        }
        fn f() -> f64 { return Planet.EARTH.getMass(); }
    )");
    REQUIRE(ir.find("define double @Planet_getMass(ptr %self)") != std::string::npos);
    REQUIRE(ir.find("call double @Planet_getMass(ptr @Planet$EARTH)") != std::string::npos);
}
