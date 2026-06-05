#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// Parser tests
// ============================================================

TEST_CASE("Class - basic class parses to ClassDeclStmt", "[class][parser]") {
    auto prog = parseString(R"(
        class Point {
            public f32 x;
            public f32 y;
        }
    )");
    REQUIRE(prog.declarations.size() == 1);
    const auto& cls = asStmt<ClassDeclStmt>(prog.declarations[0]);
    REQUIRE(cls.name.lexeme == "Point");
    REQUIRE(cls.fields.size() == 2);
    REQUIRE(cls.fields[0].name.lexeme == "x");
    REQUIRE(cls.fields[1].name.lexeme == "y");
    REQUIRE(cls.fields[0].isPublic);
    REQUIRE(cls.fields[1].isPublic);
}

TEST_CASE("Class - constructor is parsed as MethodDecl with isConstructor=true", "[class][parser]") {
    auto prog = parseString(R"(
        class Vec {
            public f32 x;
            public Vec(f32 x) { this.x = x; }
        }
    )");
    REQUIRE(prog.declarations.size() == 1);
    const auto& cls = asStmt<ClassDeclStmt>(prog.declarations[0]);
    REQUIRE(cls.methods.size() == 1);
    REQUIRE(cls.methods[0].isConstructor);
    REQUIRE(cls.methods[0].name.lexeme == "Vec");
    REQUIRE(cls.methods[0].params.size() == 1);
}

TEST_CASE("Class - regular method is parsed as MethodDecl with isConstructor=false", "[class][parser]") {
    auto prog = parseString(R"(
        class Counter {
            public i32 value;
            public i32 get() { return this.value; }
        }
    )");
    REQUIRE(prog.declarations.size() == 1);
    const auto& cls = asStmt<ClassDeclStmt>(prog.declarations[0]);
    REQUIRE(cls.methods.size() == 1);
    REQUIRE_FALSE(cls.methods[0].isConstructor);
    REQUIRE(cls.methods[0].name.lexeme == "get");
}

TEST_CASE("Class - private member parses with isPublic=false", "[class][parser]") {
    auto prog = parseString(R"(
        class Foo {
            private i32 secret;
            public i32 visible;
        }
    )");
    const auto& cls = asStmt<ClassDeclStmt>(prog.declarations[0]);
    REQUIRE_FALSE(cls.fields[0].isPublic);  // secret
    REQUIRE(cls.fields[1].isPublic);        // visible
}

TEST_CASE("Class - field access parses to MemberAccessExpr", "[class][parser]") {
    auto prog = parseString(R"(
        class P { public f32 x; }
        f32 foo() {
            P p;
            return p.x;
        }
    )");
    // second declaration is the function
    const auto& func = asStmt<FunctionDeclStmt>(prog.declarations[1]);
    // second statement is return p.x
    const auto& ret = asStmt<ReturnStmt>(*func.body.body[1]);
    const auto& ma  = asExpr<MemberAccessExpr>(*ret.value);
    REQUIRE(ma.field.lexeme == "x");
    const auto& obj = asExpr<IdentifierExpr>(*ma.object);
    REQUIRE(obj.name.lexeme == "p");
}

TEST_CASE("Class - method call parses to MethodCallExpr", "[class][parser]") {
    auto prog = parseString(R"(
        class C { public i32 val() { return 0; } }
        i32 main() {
            C c;
            return c.val();
        }
    )");
    const auto& func = asStmt<FunctionDeclStmt>(prog.declarations[1]);
    const auto& ret  = asStmt<ReturnStmt>(*func.body.body[1]);
    const auto& mc   = asExpr<MethodCallExpr>(*ret.value);
    REQUIRE(mc.method.lexeme == "val");
}

TEST_CASE("Class - field assignment parses to MemberAssignExpr", "[class][parser]") {
    auto prog = parseString(R"(
        class P { public f32 x; }
        void foo() {
            P p;
            p.x = 1.0;
        }
    )");
    const auto& func  = asStmt<FunctionDeclStmt>(prog.declarations[1]);
    const auto& exprS = asStmt<ExprStmt>(*func.body.body[1]);
    const auto& ma    = asExpr<MemberAssignExpr>(exprS.expression);
    REQUIRE(ma.field.lexeme == "x");
}

TEST_CASE("Class - constructor call syntax parses to VarDecl with CallExpr initializer", "[class][parser]") {
    auto prog = parseString(R"(
        class P { public f32 x; public P(f32 x) { this.x = x; } }
        void main() {
            P p(1.0);
        }
    )");
    const auto& func  = asStmt<FunctionDeclStmt>(prog.declarations[1]);
    const auto& exprS = asStmt<ExprStmt>(*func.body.body[0]);
    const auto& vd    = asExpr<VarDeclExpr>(exprS.expression);
    REQUIRE(vd.typeName.lexeme == "P");
    REQUIRE(vd.name.lexeme == "p");
    REQUIRE(vd.initializer != nullptr);
    const auto& ctor = asExpr<CallExpr>(*vd.initializer);
    REQUIRE(ctor.callee.lexeme == "P");
    REQUIRE(ctor.args.size() == 1);
}

// ============================================================
// Semantic tests
// ============================================================

TEST_CASE("Class - valid class with constructor and method passes", "[class][semantic]") {
    auto result = analyzeString(R"(
        class Point {
            public f32 x;
            public f32 y;
            public Point(f32 x, f32 y) {
                this.x = x;
                this.y = y;
            }
            public f32 sum() {
                return this.x + this.y;
            }
        }
        void main() {
            Point p(1.0, 2.0);
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Class - accessing undeclared member is an error", "[class][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Foo { public i32 a; }
        void main() {
            Foo f;
            i32 x = f.noSuchField;
        }
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("Class - accessing private field from outside class is an error", "[class][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Foo { private i32 secret; }
        void main() {
            Foo f;
            i32 x = f.secret;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("private"));
}

TEST_CASE("Class - accessing private field from inside class is valid", "[class][semantic]") {
    auto result = analyzeString(R"(
        class Foo {
            private i32 secret;
            public i32 get() { return this.secret; }
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Class - constructor argument count mismatch is an error", "[class][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Vec {
            public f32 x;
            public Vec(f32 x) { this.x = x; }
        }
        void main() {
            Vec v(1.0, 2.0);
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("expects"));
}

TEST_CASE("Class - method argument count mismatch is an error", "[class][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Calc {
            public i32 add(i32 a, i32 b) { return a + b; }
        }
        void main() {
            Calc c;
            c.add(1);
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("expects"));
}

TEST_CASE("Class - 'this' outside class method is an error", "[class][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        void foo() { i32 x = this.y; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("outside of a class"));
}

TEST_CASE("Class - zero-argument constructor call is valid when no constructor defined", "[class][semantic]") {
    auto result = analyzeString(R"(
        class Empty { public i32 x; }
        void main() { Empty e; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Class - field type mismatch in assignment is an error", "[class][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class P { public i32 x; }
        void main() {
            P p;
            p.x = "hello";
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("cannot implicitly convert"));
}

// ============================================================
// CodeGen tests
// ============================================================

TEST_CASE("Class - struct type declaration emitted in IR", "[class][codegen]") {
    std::string ir = codegenString(R"(
        class Point {
            public f32 x;
            public f32 y;
        }
    )");
    REQUIRE(ir.find("%Point = type { float, float }") != std::string::npos);
}

TEST_CASE("Class - method emitted with mangled name and ptr self parameter", "[class][codegen]") {
    std::string ir = codegenString(R"(
        class Counter {
            public i32 value;
            public i32 get() { return this.value; }
        }
    )");
    REQUIRE(ir.find("@Counter_get(ptr %self)") != std::string::npos);
}

TEST_CASE("Class - constructor emitted as void mangled function", "[class][codegen]") {
    std::string ir = codegenString(R"(
        class Vec {
            public f32 x;
            public Vec(f32 x) { this.x = x; }
        }
    )");
    REQUIRE(ir.find("define void @Vec_Vec(ptr %self") != std::string::npos);
}

TEST_CASE("Class - stack alloca emitted for object variable", "[class][codegen]") {
    std::string ir = codegenString(R"(
        class Point { public f32 x; public f32 y; }
        void main() { Point p; }
    )");
    REQUIRE(ir.find("%p.addr = alloca %Point") != std::string::npos);
}

TEST_CASE("Class - zeroinitializer emitted for object variable", "[class][codegen]") {
    std::string ir = codegenString(R"(
        class Point { public f32 x; public f32 y; }
        void main() { Point p; }
    )");
    REQUIRE(ir.find("store %Point zeroinitializer, ptr %p.addr") != std::string::npos);
}

TEST_CASE("Class - constructor call emitted after alloca + zeroinit", "[class][codegen]") {
    std::string ir = codegenString(R"(
        class Point {
            public f32 x;
            public f32 y;
            public Point(f32 x, f32 y) { this.x = x; this.y = y; }
        }
        void main() { Point p(1.0, 2.0); }
    )");
    REQUIRE(ir.find("call void @Point_Point(ptr %p.addr") != std::string::npos);
}

TEST_CASE("Class - field read produces getelementptr + load", "[class][codegen]") {
    std::string ir = codegenString(R"(
        class Point { public f32 x; public f32 y; }
        f32 getX(Point p) {
            return p.x;
        }
    )");
    REQUIRE(ir.find("getelementptr %Point") != std::string::npos);
    REQUIRE(ir.find("load float") != std::string::npos);
}

TEST_CASE("Class - method call emitted with self pointer as first argument", "[class][codegen]") {
    std::string ir = codegenString(R"(
        class Counter {
            public i32 value;
            public i32 get() { return this.value; }
        }
        i32 main() {
            Counter c;
            return c.get();
        }
    )");
    REQUIRE(ir.find("call i32 @Counter_get(ptr %c.addr)") != std::string::npos);
}
