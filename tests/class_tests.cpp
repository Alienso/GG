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

// ============================================================
// Destructor tests
// ============================================================

TEST_CASE("Destructor - parser sets isDestructor=true on MethodDecl", "[class][destructor][parser]") {
    auto prog = parseString(R"(
        class Box {
            public i32 value;
            public Box(i32 v) { this.value = v; }
            public ~Box() { }
        }
    )");
    REQUIRE(prog.declarations.size() == 1);
    const auto& cls = asStmt<ClassDeclStmt>(prog.declarations[0]);
    // Should have 2 methods: constructor + destructor
    REQUIRE(cls.methods.size() == 2);
    bool foundDtor = false;
    for (const auto& m : cls.methods) {
        if (m.isDestructor) {
            foundDtor = true;
            REQUIRE_FALSE(m.isConstructor);
            REQUIRE(m.params.empty());
            REQUIRE(m.name.lexeme == "Box");
        }
    }
    REQUIRE(foundDtor);
}

TEST_CASE("Destructor - class without destructor has no _dtor function in IR", "[class][destructor][codegen]") {
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
    REQUIRE(ir.find("_dtor") == std::string::npos);
}

TEST_CASE("Destructor - @ClassName_dtor function is emitted", "[class][destructor][codegen]") {
    std::string ir = codegenString(R"(
        class Box {
            public i32 value;
            public ~Box() { }
        }
        i32 main() {
            Box b;
            return 0;
        }
    )");
    REQUIRE(ir.find("define void @Box_dtor(ptr %self)") != std::string::npos);
}

TEST_CASE("Destructor - called at end of enclosing block", "[class][destructor][codegen]") {
    std::string ir = codegenString(R"(
        class Box {
            public i32 value;
            public ~Box() { }
        }
        i32 main() {
            Box b;
            return 0;
        }
    )");
    // The dtor call must appear before the final ret
    auto dtorPos = ir.find("call void @Box_dtor(ptr %b.addr)");
    auto retPos  = ir.find("ret i32");
    REQUIRE(dtorPos != std::string::npos);
    REQUIRE(retPos  != std::string::npos);
    REQUIRE(dtorPos < retPos);
}

TEST_CASE("Destructor - called before early return", "[class][destructor][codegen]") {
    std::string ir = codegenString(R"(
        class Box {
            public i32 value;
            public ~Box() { }
        }
        i32 main() {
            Box b;
            return 1;
        }
    )");
    auto dtorPos = ir.find("call void @Box_dtor(ptr %b.addr)");
    auto retPos  = ir.find("ret i32 1");
    REQUIRE(dtorPos != std::string::npos);
    REQUIRE(retPos  != std::string::npos);
    REQUIRE(dtorPos < retPos);
}

TEST_CASE("Destructor - multiple objects destroyed in reverse order", "[class][destructor][codegen]") {
    std::string ir = codegenString(R"(
        class A {
            public i32 v;
            public ~A() { }
        }
        class B {
            public i32 v;
            public ~B() { }
        }
        i32 main() {
            A a;
            B b;
            return 0;
        }
    )");
    auto dtorA = ir.find("call void @A_dtor(ptr %a.addr)");
    auto dtorB = ir.find("call void @B_dtor(ptr %b.addr)");
    REQUIRE(dtorA != std::string::npos);
    REQUIRE(dtorB != std::string::npos);
    // B declared after A, so B must be destroyed first (dtorB < dtorA)
    REQUIRE(dtorB < dtorA);
}

TEST_CASE("Destructor - semantic error: duplicate destructor", "[class][destructor][semantic]") {
    auto result = analyzeString(R"(
        class Box {
            public i32 value;
            public ~Box() { }
            public ~Box() { }
        }
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("Destructor - parser: destructor with params produces no destructor in class", "[class][destructor][parser]") {
    // The parser enforces no-params for destructors via a ParseError that is then
    // synchronised over.  The resulting ClassDeclStmt will therefore have no
    // destructor MethodDecl (the partial parse is discarded during synchronize()).
    auto prog = parseString(R"(
        class Box {
            public i32 value;
            public ~Box(i32 x) { }
        }
    )");
    // The class may not even be in the declarations (ParseError clears it).
    // What matters is that no destructor with params made it through.
    bool dtorWithParamsFound = false;
    for (const auto& decl : prog.declarations) {
        if (!std::holds_alternative<ClassDeclStmt>(*decl.node)) continue;
        const auto& cls = std::get<ClassDeclStmt>(*decl.node);
        for (const auto& m : cls.methods)
            if (m.isDestructor && !m.params.empty()) dtorWithParamsFound = true;
    }
    REQUIRE_FALSE(dtorWithParamsFound);
}

// ============================================================
// Pass-by-reference for objects (basic types stay by value)
// ============================================================

TEST_CASE("PassByRef - object parameter lowers to ptr in signature", "[class][codegen][byref]") {
    std::string ir = codegenString(R"(
        class Point { public f32 x; public f32 y; }
        void use(Point p) { p.x = 1.0; }
    )");
    REQUIRE(ir.find("define void @use(ptr %p)") != std::string::npos);
}

TEST_CASE("PassByRef - object parameter is not copied into a local alloca", "[class][codegen][byref]") {
    std::string ir = codegenString(R"(
        class Point { public f32 x; public f32 y; }
        void use(Point p) { p.x = 1.0; }
    )");
    // The @use body must NOT allocate or copy a %Point for the parameter.
    auto usePos = ir.find("define void @use(ptr %p)");
    REQUIRE(usePos != std::string::npos);
    auto useBody = ir.substr(usePos);
    auto nextDef = useBody.find("\ndefine ", 1);
    if (nextDef != std::string::npos) useBody = useBody.substr(0, nextDef);
    REQUIRE(useBody.find("alloca %Point") == std::string::npos);
    REQUIRE(useBody.find("store %Point %p") == std::string::npos);
    // Field write GEPs straight into the incoming pointer %p.
    REQUIRE(useBody.find("getelementptr %Point, ptr %p") != std::string::npos);
}

TEST_CASE("PassByRef - basic-type parameter is still passed by value", "[class][codegen][byref]") {
    std::string ir = codegenString(R"(
        void use(i32 n) { n = n + 1; }
    )");
    REQUIRE(ir.find("define void @use(i32 %n)") != std::string::npos);
    // Value params are spilled to an alloca so they remain locally mutable.
    REQUIRE(ir.find("%n.addr = alloca i32")    != std::string::npos);
    REQUIRE(ir.find("store i32 %n, ptr %n.addr") != std::string::npos);
}

TEST_CASE("PassByRef - mixed object + basic params", "[class][codegen][byref]") {
    std::string ir = codegenString(R"(
        class Point { public f32 x; public f32 y; }
        void use(Point p, i32 n) { }
    )");
    REQUIRE(ir.find("define void @use(ptr %p, i32 %n)") != std::string::npos);
}

TEST_CASE("PassByRef - call site passes the object's address", "[class][codegen][byref]") {
    std::string ir = codegenString(R"(
        class Point { public f32 x; public f32 y; public Point(f32 a, f32 b) { this.x = a; this.y = b; } }
        void use(Point p) { }
        void main() {
            Point p(1.0, 2.0);
            use(p);
        }
    )");
    REQUIRE(ir.find("call void @use(ptr %p.addr)") != std::string::npos);
}

TEST_CASE("PassByRef - method taking an object parameter lowers to ptr", "[class][codegen][byref]") {
    std::string ir = codegenString(R"(
        class Point { public f32 x; public f32 y; }
        class Line {
            public f32 len;
            public void from(Point a) { this.len = a.x; }
        }
    )");
    REQUIRE(ir.find("define void @Line_from(ptr %self, ptr %a)") != std::string::npos);
}

TEST_CASE("PassByRef - mutating an object parameter's field is allowed", "[class][semantic][byref]") {
    auto result = analyzeString(R"(
        class Point { public f32 x; public f32 y; }
        void use(Point p) { p.x = 5.0; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("PassByRef - reassigning an object parameter is an error", "[class][semantic][byref]") {
    auto result = analyzeString(R"(
        class Point { public f32 x; public f32 y; public Point(f32 a, f32 b) { this.x = a; this.y = b; } }
        void use(Point p) {
            Point q(1.0, 2.0);
            p = q;
        }
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("PassByRef - reassigning a basic-type parameter is still allowed", "[class][semantic][byref]") {
    auto result = analyzeString(R"(
        void use(i32 n) { n = 5; }
    )");
    REQUIRE_FALSE(result.hadError);
}
