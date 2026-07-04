#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// Static member fields (Phase 1)
// ============================================================
// A `static` field is class-level storage: a single global shared by all
// instances, accessed as `ClassName::field` or `obj.field`. Constant
// initializers run before main in @gg_static_init.

// ------------------------------------------------------------
// Parser tests
// ------------------------------------------------------------

TEST_CASE("Static - field parses with isStatic flag and initializer", "[static][parser]") {
    auto prog = parseString(R"(
        class Counter {
            static i32 count = 0;
            i32 id;
        }
    )");
    REQUIRE(prog.declarations.size() == 1);
    const auto& cls = asStmt<ClassDeclStmt>(prog.declarations[0]);
    REQUIRE(cls.fields.size() == 2);
    REQUIRE(cls.fields[0].name.lexeme == "count");
    REQUIRE(cls.fields[0].isStatic);
    REQUIRE(cls.fields[0].initializer != nullptr);
    REQUIRE_FALSE(cls.fields[1].isStatic);   // instance field
    REQUIRE(cls.fields[1].initializer == nullptr);
}

TEST_CASE("Static - 'private static' order is accepted", "[static][parser]") {
    auto prog = parseString(R"(
        class C {
            private static i32 secret = 7;
        }
    )");
    REQUIRE(prog.declarations.size() == 1);
    const auto& cls = asStmt<ClassDeclStmt>(prog.declarations[0]);
    REQUIRE(cls.fields.size() == 1);
    REQUIRE(cls.fields[0].isStatic);
    REQUIRE_FALSE(cls.fields[0].isPublic);
}

TEST_CASE("Static - method parses with isStatic flag", "[static][parser]") {
    auto prog = parseString(R"(
        class C {
            static i32 get() { return 0; }
            i32 inst() { return 1; }
        }
    )");
    REQUIRE(prog.declarations.size() == 1);
    const auto& cls = asStmt<ClassDeclStmt>(prog.declarations[0]);
    REQUIRE(cls.methods.size() == 2);
    REQUIRE(cls.methods[0].name.lexeme == "get");
    REQUIRE(cls.methods[0].isStatic);
    REQUIRE_FALSE(cls.methods[1].isStatic);   // instance method
}

// ------------------------------------------------------------
// Semantic tests
// ------------------------------------------------------------

TEST_CASE("Static - field registered in classRegistry.staticFields", "[static][semantic]") {
    auto result = analyzeString(R"(
        class Counter { static i32 count = 0; }
    )");
    REQUIRE_FALSE(result.hadError);
    REQUIRE(result.classRegistry.count("Counter") == 1);
    const auto& info = result.classRegistry.at("Counter");
    REQUIRE(info.staticFields.count("count") == 1);
    REQUIRE(info.fields.count("count") == 0);   // not an instance field
}

TEST_CASE("Static - access via ClassName::field type-checks", "[static][semantic]") {
    auto result = analyzeString(R"(
        class Counter { static i32 count = 0; }
        i32 f() { return Counter::count; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Static - write via ClassName::field is allowed (mutable)", "[static][semantic]") {
    auto result = analyzeString(R"(
        class Counter { static i32 count = 0; }
        void f() { Counter::count = 5; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Static - read through an instance obj.field type-checks", "[static][semantic]") {
    auto result = analyzeString(R"(
        class Counter { static i32 count = 0; }
        i32 f(Counter& c) { return c.count; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Static - unknown static member is an error", "[static][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Counter { static i32 count = 0; }
        i32 f() { return Counter::nope; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("no static member"));
}

TEST_CASE("Static - initializer type mismatch is reported", "[static][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class C { static bool flag = 3; }
    )");
    // bool <- i32 is an invalid implicit conversion (error, not just a warning).
    REQUIRE(result.hadError);
}

TEST_CASE("Static - enums cannot declare static fields", "[static][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        enum Color { RED, GREEN; static i32 n = 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("enums cannot declare static fields"));
}

// ------------------------------------------------------------
// CodeGen tests
// ------------------------------------------------------------

TEST_CASE("Static - field is emitted as a global", "[static][codegen]") {
    auto ir = codegenString(R"(
        class Counter { static i32 count = 0; }
        i32 f() { return Counter::count; }
    )");
    REQUIRE(ir.find("@Counter$count = global i32 zeroinitializer") != std::string::npos);
}

TEST_CASE("Static - initializer runs in gg_static_init via global_ctors", "[static][codegen]") {
    auto ir = codegenString(R"(
        class Counter { static i32 count = 5; }
        i32 f() { return Counter::count; }
    )");
    REQUIRE(ir.find("define void @gg_static_init()") != std::string::npos);
    REQUIRE(ir.find("store i32 5, ptr @Counter$count") != std::string::npos);
    REQUIRE(ir.find("@llvm.global_ctors") != std::string::npos);
    REQUIRE(ir.find("@gg_static_init") != std::string::npos);
}

TEST_CASE("Static - field with no initializer emits no gg_static_init", "[static][codegen]") {
    auto ir = codegenString(R"(
        class Counter { static i32 count; }
        i32 f() { return Counter::count; }
    )");
    REQUIRE(ir.find("@Counter$count = global i32 zeroinitializer") != std::string::npos);
    REQUIRE(ir.find("@gg_static_init") == std::string::npos);
}

TEST_CASE("Static - read via :: lowers to a load of the global", "[static][codegen]") {
    auto ir = codegenString(R"(
        class Counter { static i32 count = 0; }
        i32 f() { return Counter::count; }
    )");
    REQUIRE(ir.find("load i32, ptr @Counter$count") != std::string::npos);
}

TEST_CASE("Static - write via :: lowers to a store to the global", "[static][codegen]") {
    auto ir = codegenString(R"(
        class Counter { static i32 count = 0; }
        void f() { Counter::count = 9; }
    )");
    REQUIRE(ir.find("store i32 9, ptr @Counter$count") != std::string::npos);
}

TEST_CASE("Static - access through an instance also targets the global", "[static][codegen]") {
    auto ir = codegenString(R"(
        class Counter { static i32 count = 0; i32 id; }
        i32 f(Counter& c) { return c.count; }
    )");
    REQUIRE(ir.find("load i32, ptr @Counter$count") != std::string::npos);
}

TEST_CASE("Static - field is excluded from the instance struct layout", "[static][codegen]") {
    auto ir = codegenString(R"(
        class Counter { static i32 count = 0; i32 id; }
        i32 f(Counter& c) { return c.id; }
    )");
    // Only the instance field 'id' occupies the struct; the static is a global.
    REQUIRE(ir.find("%Counter = type { i32 }") != std::string::npos);
}

TEST_CASE("Static - enum init and static init share one global_ctors array", "[static][codegen]") {
    auto ir = codegenString(R"(
        enum Planet {
            EARTH(5.976);
            f64 mass;
            Planet(f64 mass) { this.mass = mass; }
        }
        class Counter { static i32 count = 5; }
        i32 f() { return Counter::count; }
    )");
    REQUIRE(ir.find("@gg_enum_init") != std::string::npos);
    REQUIRE(ir.find("@gg_static_init") != std::string::npos);
    // Exactly one global_ctors definition (two entries).
    REQUIRE(ir.find("[2 x { i32, ptr, ptr }]") != std::string::npos);
}

// ============================================================
// Static class methods (Phase 2)
// ============================================================
// A `static` method is class-level: no implicit `this`, called as
// `ClassName::method(args)` (or `obj.method(args)` for symmetry). It may read
// and write static fields but has no receiver.

// ---- Semantic ----

TEST_CASE("Static - method registered with isStatic in classRegistry", "[static][semantic]") {
    auto result = analyzeString(R"(
        class C { static i32 get() { return 0; } i32 inst() { return 1; } }
    )");
    REQUIRE_FALSE(result.hadError);
    const auto& info = result.classRegistry.at("C");
    REQUIRE(info.methods.at("get").front().isStatic);
    REQUIRE_FALSE(info.methods.at("inst").front().isStatic);
}

TEST_CASE("Static - call via ClassName::method type-checks", "[static][semantic]") {
    auto result = analyzeString(R"(
        class C { static i32 get() { return 42; } }
        i32 f() { return C::get(); }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Static - method can read a static field", "[static][semantic]") {
    auto result = analyzeString(R"(
        class C {
            static i32 count = 7;
            static i32 get() { return C::count; }
        }
        i32 f() { return C::get(); }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Static - 'this' inside a static method is an error", "[static][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class C { i32 id; static i32 get() { return this.id; } }
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("Static - calling an instance method via :: is an error", "[static][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class C { i32 inst() { return 1; } }
        i32 f() { return C::inst(); }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("not static"));
}

TEST_CASE("Static - unknown static method is an error", "[static][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class C { static i32 get() { return 0; } }
        i32 f() { return C::nope(); }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("no static method"));
}

TEST_CASE("Static - method argument count mismatch is reported", "[static][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class C { static i32 add(i32 a, i32 b) { return a + b; } }
        i32 f() { return C::add(1); }
    )");
    REQUIRE(result.hadError);
}

// ---- CodeGen ----

TEST_CASE("Static - method emitted without an implicit self parameter", "[static][codegen]") {
    auto ir = codegenString(R"(
        class C { static i32 get() { return 42; } }
        i32 f() { return C::get(); }
    )");
    REQUIRE(ir.find("define i32 @C_get()") != std::string::npos);
}

TEST_CASE("Static - instance method still takes ptr self", "[static][codegen]") {
    auto ir = codegenString(R"(
        class C { i32 id; i32 inst() { return this.id; } }
        i32 f(C& c) { return c.inst(); }
    )");
    REQUIRE(ir.find("define i32 @C_inst(ptr %self)") != std::string::npos);
}

TEST_CASE("Static - call via :: lowers to a call without a receiver", "[static][codegen]") {
    auto ir = codegenString(R"(
        class C { static i32 add(i32 a, i32 b) { return a + b; } }
        i32 f() { return C::add(2, 3); }
    )");
    REQUIRE(ir.find("call i32 @C_add(i32 2, i32 3)") != std::string::npos);
}

TEST_CASE("Static - method mutating a static field stores to the global", "[static][codegen]") {
    auto ir = codegenString(R"(
        class C {
            static i32 count = 0;
            static void bump() { C::count = C::count + 1; }
        }
        void f() { C::bump(); }
    )");
    REQUIRE(ir.find("define void @C_bump()") != std::string::npos);
    REQUIRE(ir.find("store i32 %") != std::string::npos);
    REQUIRE(ir.find("@C$count") != std::string::npos);
}

TEST_CASE("Static - call through an instance also omits the receiver", "[static][codegen]") {
    auto ir = codegenString(R"(
        class C { i32 id; static i32 get() { return 9; } }
        i32 f(C& c) { return c.get(); }
    )");
    REQUIRE(ir.find("call i32 @C_get()") != std::string::npos);
}

// ============================================================
// Static local variables (Phase 3, C-style)
// ============================================================
// `static T name = const;` inside a function body is a single persistent global
// shared across every call to that function. Initialised once before main.

// ---- Parser ----

TEST_CASE("Static - local var parses with isStatic flag", "[static][parser]") {
    auto prog = parseString(R"(
        i32 next() {
            static i32 counter = 0;
            counter = counter + 1;
            return counter;
        }
    )");
    REQUIRE(prog.declarations.size() == 1);
    const auto& fn = asStmt<FunctionDeclStmt>(prog.declarations[0]);
    REQUIRE(fn.body.body.size() >= 1);
    const auto& es = asStmt<ExprStmt>(*fn.body.body[0]);
    const auto& vd = asExpr<VarDeclExpr>(es.expression);
    REQUIRE(vd.name.lexeme == "counter");
    REQUIRE(vd.isStatic);
    REQUIRE(vd.initializer != nullptr);
}

// ---- Semantic ----

TEST_CASE("Static - local with constant initializer type-checks", "[static][semantic]") {
    auto result = analyzeString(R"(
        i32 next() { static mut i32 c = 0; c = c + 1; return c; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Static - local with no initializer is allowed (zero-init)", "[static][semantic]") {
    auto result = analyzeString(R"(
        i32 next() { static mut i32 c; c = c + 1; return c; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Static - local with non-constant initializer is an error", "[static][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        i32 next(i32 seed) { static i32 c = seed; return c; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("constant initializer"));
}

TEST_CASE("Static - local arithmetic constant initializer is accepted", "[static][semantic]") {
    auto result = analyzeString(R"(
        i32 next() { static i32 c = 2 + 3 * 4; return c; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Static - non-primitive static local is an error", "[static][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class P { i32 x; }
        i32 f() { static P p; return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("primitive type"));
}

// ---- CodeGen ----

TEST_CASE("Static - local is emitted as an internal global", "[static][codegen]") {
    auto ir = codegenString(R"(
        i32 next() { static mut i32 counter = 0; counter = counter + 1; return counter; }
    )");
    REQUIRE(ir.find("@next$counter = internal global i32 zeroinitializer") != std::string::npos);
}

TEST_CASE("Static - local non-zero initializer runs in gg_static_init", "[static][codegen]") {
    auto ir = codegenString(R"(
        i32 next() { static i32 counter = 5; return counter; }
    )");
    REQUIRE(ir.find("define void @gg_static_init()") != std::string::npos);
    REQUIRE(ir.find("store i32 5, ptr @next$counter") != std::string::npos);
    REQUIRE(ir.find("@llvm.global_ctors") != std::string::npos);
}

TEST_CASE("Static - local read/write target the global", "[static][codegen]") {
    auto ir = codegenString(R"(
        i32 next() { static mut i32 counter = 0; counter = counter + 1; return counter; }
    )");
    REQUIRE(ir.find("load i32, ptr @next$counter") != std::string::npos);
    REQUIRE(ir.find("store i32 %") != std::string::npos);
    // No alloca for the static local (it is a global, not stack storage).
    REQUIRE(ir.find("%counter.addr = alloca") == std::string::npos);
}

TEST_CASE("Static - local with no initializer emits no gg_static_init", "[static][codegen]") {
    auto ir = codegenString(R"(
        i32 next() { static mut i32 counter; counter = counter + 1; return counter; }
    )");
    REQUIRE(ir.find("@next$counter = internal global i32 zeroinitializer") != std::string::npos);
    REQUIRE(ir.find("@gg_static_init") == std::string::npos);
}

TEST_CASE("Static - same-named locals in different functions get distinct globals", "[static][codegen]") {
    auto ir = codegenString(R"(
        i32 a() { static mut i32 c = 0; c = c + 1; return c; }
        i32 b() { static mut i32 c = 0; c = c + 2; return c; }
    )");
    REQUIRE(ir.find("@a$c = internal global i32 zeroinitializer") != std::string::npos);
    REQUIRE(ir.find("@b$c = internal global i32 zeroinitializer") != std::string::npos);
}

TEST_CASE("Static - local inside a method mangles with the method prefix", "[static][codegen]") {
    auto ir = codegenString(R"(
        class Counter {
            i32 tick() { static mut i32 n = 0; n = n + 1; return n; }
        }
    )");
    REQUIRE(ir.find("@Counter_tick$n = internal global i32 zeroinitializer") != std::string::npos);
}
