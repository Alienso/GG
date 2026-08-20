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

TEST_CASE("variadic - a field access is deducible as a pack argument type", "[variadic][codegen]") {
    // `p.x` (a MemberAccessExpr) previously had no case in Parser::deduceArgTypeToken, so passing a
    // field straight into a pack argument (e.g. `printf("{}", v.x)`) failed with "cannot infer the
    // type of a variadic argument" even though the field's type is perfectly well known at parse
    // time. deduceArgTypeToken now resolves the receiver's class (recursively, so it also works
    // through `this`/another field) and looks up the field's declared type in the class's own
    // member list (GenericRegistry::classFieldTypes), regardless of which class is being parsed when
    // the call site is reached.
    std::string ir = codegenString(R"(
        class Point { i32 x; i32 y; Point(i32 x, i32 y) { this.x = x; this.y = y; } }
        fn take<...Ts>(Ts... args) -> i32 { return 0; }
        fn main() -> i32 {
            Point p(1, 2);
            return take(p.x, p.y);
        }
    )");
    REQUIRE(ir.find("%Tuple$i32$i32 = type { i32, i32 }") != std::string::npos);
    REQUIRE(ir.find("@take$Tuple$i32$i32(ptr") != std::string::npos);
}

TEST_CASE("variadic - a reference-typed field decays to a plain class element in a pack",
          "[variadic][codegen]") {
    // A field's OWN declared type must be decayed the same way an identifier's declared type is
    // (strip a trailing '&'/'*' or nullable '?') before it reaches requestTupleType's mangling —
    // otherwise a reference-typed field (`Inner& item;`) would produce an unparsable tuple class
    // name like "Tuple$Inner&$i32" ('&' is not a valid unquoted LLVM identifier character). Caught
    // by re-checking the MemberAccessExpr fix for edge cases: the first version returned the field's
    // raw type token verbatim, which happened to work for the reported `v.x`/`v.y` case (plain
    // primitive fields) but would have silently emitted broken IR for a reference-typed field.
    std::string ir = codegenString(R"(
        class Inner { i32 v; Inner(i32 v) { this.v = v; } }
        class Holder { Inner& item; Holder(Inner& i) { item = i; } }
        fn take<...Ts>(Ts... args) -> i32 { return 0; }
        fn main() -> i32 {
            Inner& n = new Inner(5);
            Holder h(n);
            return take(h.item, 1);
        }
    )");
    REQUIRE(ir.find("%Tuple$Inner$i32 = type { %Inner, i32 }") != std::string::npos);
    REQUIRE(ir.find("@take$Tuple$Inner$i32(ptr") != std::string::npos);
}

TEST_CASE("variadic - a static field via Class::field is deducible as a pack argument type",
          "[variadic][codegen]") {
    // `Class::field` parses to the same MemberAccessExpr node as `obj.field`, but the object names a
    // class directly rather than a variable — deduceArgTypeToken's IdentifierExpr case only searches
    // scopes_/classFieldScope_ (locals/params/instance fields), so it would never resolve "Holder" on
    // its own. The MemberAccessExpr case special-cases an object that is itself a known class name.
    std::string ir = codegenString(R"(
        class Holder { static mut i32 counter = 7; }
        fn take<...Ts>(Ts... args) -> i32 { return 0; }
        fn main() -> i32 { return take(Holder::counter, 2); }
    )");
    REQUIRE(ir.find("%Tuple$i32$i32 = type { i32, i32 }") != std::string::npos);
    REQUIRE(ir.find("@take$Tuple$i32$i32(ptr") != std::string::npos);
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

// ============================================================
// Spread into a NON-pack-bearing target: `f(fixed…, xs...)` where `f` is NOT itself pack-bearing
// unwraps `xs` into N ordinary positional arguments (`xs._0, …, xs._{N-1}`) at parse time, so normal
// overload resolution does all arity/type checking. Packs are v1-scoped to free functions only, so a
// constructor/method/`new` target can never be pack-bearing — only the bare `name(args)` call form
// (shared with free-function calls) needs the "is this a pack template" check.
// ============================================================

TEST_CASE("variadic - spread unwraps into an ordinary free function", "[variadic][codegen]") {
    std::string ir = codegenString(R"(
        fn add3(i32 a, i32 b, i32 c) -> i32 { return a + b + c; }
        fn wrap<...Ts>(Ts... args) -> i32 { return add3(args...); }
        fn main() -> i32 { return wrap(1, 2, 3); }
    )");
    // Three discrete i32 operands from the tuple's fields, not the tuple pointer itself.
    REQUIRE(ir.find("call i32 @add3(i32 ") != std::string::npos);
    REQUIRE(ir.find("@add3(ptr") == std::string::npos);
}

TEST_CASE("variadic - spread unwraps into a bare constructor call", "[variadic][codegen]") {
    // `Point q = Point(args...);` — a var-decl `=` initializer whose RHS is a bare ctor call — hits
    // genVarDecl's own ctor branch (constructs directly into `q`, no clone), distinct from the
    // parens-sugar `Point q(args...)` call site tested separately below.
    std::string ir = codegenString(R"(
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } }
        fn wrap<...Ts>(Ts... args) -> i32 {
            Point q = Point(args...);
            return q.x;
        }
        fn main() -> i32 { return wrap(3, 4); }
    )");
    REQUIRE(ir.find("getelementptr %Tuple$i32$i32") != std::string::npos);   // args._0 / args._1
    REQUIRE(ir.find("call void @Point_Point(ptr ") != std::string::npos);
    REQUIRE(ir.find("call void @Point_clone(") == std::string::npos);       // constructed, not cloned
}

TEST_CASE("variadic - spread unwraps into the var-decl constructor sugar", "[variadic][codegen]") {
    std::string ir = codegenString(R"(
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } }
        fn wrap<...Ts>(Ts... args) { Point q(args...); }
        fn main() -> i32 { wrap(3, 4); return 0; }
    )");
    REQUIRE(ir.find("call void @Point_Point(ptr ") != std::string::npos);
    REQUIRE(ir.find("call void @Point_clone(") == std::string::npos);
}

TEST_CASE("variadic - spread unwraps into new", "[variadic][codegen]") {
    std::string ir = codegenString(R"(
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } }
        fn makeHeap<...Ts>(Ts... args) -> Point& { return new Point(args...); }
        fn main() -> i32 { Point& p = makeHeap(3, 4); return p.x; }
    )");
    REQUIRE(ir.find("call void @Point_Point(ptr ") != std::string::npos);
    REQUIRE(ir.find("call void @Point_clone(") == std::string::npos);
}

TEST_CASE("variadic - spread unwraps into a method call", "[variadic][codegen]") {
    std::string ir = codegenString(R"(
        class Adder { fn add(i32 a, i32 b, i32 c) -> i32 { return a + b + c; } }
        fn wrap<...Ts>(Adder& ad, Ts... args) -> i32 { return ad.add(args...); }
        fn main() -> i32 { Adder& ad = new Adder(); return wrap(ad, 1, 2, 3); }
    )");
    REQUIRE(ir.find("call i32 @Adder_add(ptr ") != std::string::npos);
}

TEST_CASE("variadic - spread unwraps into an explicit-<T> generic call", "[variadic][codegen]") {
    std::string ir = codegenString(R"(
        fn identity<T>(T x) -> T { return x; }
        fn wrap<...Ts>(Ts... args) -> i32 { return identity<i32>(args...); }
        fn main() -> i32 { return wrap(9); }
    )");
    REQUIRE(ir.find("@identity$i32(i32") != std::string::npos);
    REQUIRE(ir.find("@identity$i32(ptr") == std::string::npos);
}

TEST_CASE("variadic - spread of a cons-match tail pack (not just the top-level pack)", "[variadic][codegen]") {
    std::string ir = codegenString(R"(
        class Pair { mut i32 a; mut i32 b; Pair(i32 x, i32 y) { a = x; b = y; } }
        fn firstPair<...Ts>(Ts... args) -> i32 {
            match args {
                ()     -> { return 0; }
                (x:xs) -> { Pair p = Pair(xs...); return p.a; }
            }
        }
        fn main() -> i32 { return firstPair(1, 2, 3); }
    )");
    // The (x:xs) arm for a 3-element pack has a 2-element tail — Pair(xs...) unwraps to Pair(xs._0, xs._1).
    REQUIRE(ir.find("call void @Pair_Pair(ptr ") != std::string::npos);
    REQUIRE(ir.find("call void @Pair_clone(") == std::string::npos);
}

TEST_CASE("variadic - spread of an ordinary tuple local (unrelated to any pack)", "[variadic][codegen]") {
    std::string ir = codegenString(R"(
        fn add2(i32 a, i32 b) -> i32 { return a + b; }
        fn main() -> i32 {
            (i32, i32) t = (1, 2);
            return add2(t...);
        }
    )");
    REQUIRE(ir.find("call i32 @add2(i32 ") != std::string::npos);
    REQUIRE(ir.find("@add2(ptr") == std::string::npos);
}

TEST_CASE("variadic - an empty pack spread contributes zero arguments", "[variadic][codegen]") {
    std::string ir = codegenString(R"(
        fn one(i32 a) -> i32 { return a; }
        fn wrapMix<...Ts>(i32 fixedArg, Ts... args) -> i32 { return one(fixedArg, args...); }
        fn main() -> i32 { return wrapMix(5); }
    )");
    // The unit-pack instantiation calls `one` with just the fixed argument.
    REQUIRE(ir.find("@wrapMix$Tuple(i32") != std::string::npos);
    REQUIRE(ir.find("call i32 @one(i32 ") != std::string::npos);
}

TEST_CASE("variadic - without '...' a tuple stays an ordinary (mismatched) single argument", "[variadic][semantic]") {
    // A bare `Point(args)` — no ellipsis — must NOT auto-unwrap; it stays a single tuple-typed
    // argument, which fails ordinary arity checking exactly as any other wrong-arity call would.
    StderrCapture cap;
    (void)analyzeString(R"(
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } }
        fn bad<...Ts>(Ts... args) -> Point p { p = Point(args); }
        fn main() -> i32 { Point q = bad(1, 2); return q.x; }
    )");
    REQUIRE(cap.contains("argument(s), got 1"));
}

TEST_CASE("variadic - spread into an ALSO-pack-bearing target still pack-forwards (regression)", "[variadic][codegen]") {
    std::string ir = codegenString(R"(
        fn passthrough<...Ts>(Ts... args) -> i32 { return 1; }
        fn wrapPass<...Ts>(Ts... args) -> i32 { return passthrough(args...); }
        fn main() -> i32 { return wrapPass(1, 2); }
    )");
    // Unchanged existing behavior: the pack tuple is forwarded WHOLESALE, not unwrapped.
    REQUIRE(ir.find("@passthrough$Tuple$i32$i32(ptr") != std::string::npos);
}

TEST_CASE("variadic - a named spread argument is a clean error", "[variadic][semantic]") {
    StderrCapture cap;
    (void)analyzeString(R"(
        fn add2(i32 a, i32 b) -> i32 { return a + b; }
        fn wrap<...Ts>(Ts... args) -> i32 { return add2(x: args...); }
        fn main() -> i32 { return wrap(1, 2); }
    )");
    REQUIRE(cap.contains("'...' spread cannot be used as a named argument"));
}

TEST_CASE("variadic - spreading a non-identifier expression is a clean error", "[variadic][semantic]") {
    StderrCapture cap;
    (void)analyzeString(R"(
        extern malloc(u64 size) -> ptr;
        fn add2(i32 a, i32 b) -> i32 { return a + b; }
        fn main() -> i32 {
            ptr<i32> data = malloc(8);
            return add2(data[0]...);
        }
    )");
    REQUIRE(cap.contains("can only spread a simple pack variable"));
}

TEST_CASE("variadic - spreading a non-tuple-typed variable is a clean error", "[variadic][semantic]") {
    StderrCapture cap;
    (void)analyzeString(R"(
        fn add2(i32 a, i32 b) -> i32 { return a + b; }
        fn main() -> i32 {
            i32 x = 5;
            return add2(x...);
        }
    )");
    REQUIRE(cap.contains("can only spread a variadic pack"));
}

TEST_CASE("variadic - more than one spread in a call is a clean error", "[variadic][semantic]") {
    StderrCapture cap;
    (void)analyzeString(R"(
        fn add4(i32 a, i32 b, i32 c, i32 d) -> i32 { return a + b + c + d; }
        fn wrap<...Ts>(Ts... args) -> i32 { return add4(args..., args...); }
        fn main() -> i32 { return wrap(1, 2); }
    )");
    REQUIRE(cap.contains("at most one '...' spread is supported"));
}

TEST_CASE("variadic - a spread need not be the last argument (in-place splice)", "[variadic][codegen]") {
    // `sum3(args..., 9)` — a fixed argument AFTER the spread. The pack's fields are spliced in place,
    // and the trailing `9` stays as the last argument (unlike the pack-FORWARD case, where the spread
    // must be trailing). Distinct from every other spread test, which spreads in the last position.
    std::string ir = codegenString(R"(
        fn sum3(i32 a, i32 b, i32 c) -> i32 { return a + b + c; }
        fn wrap<...Ts>(Ts... args) -> i32 { return sum3(args..., 9); }
        fn main() -> i32 { return wrap(1, 2); }
    )");
    // Two unwrapped fields then the literal 9, in order — the `9` is the THIRD argument.
    REQUIRE(ir.find(", i32 9)") != std::string::npos);
    REQUIRE(ir.find("call i32 @sum3(i32 ") != std::string::npos);
}

TEST_CASE("variadic - spread of an object (value-class) pack element borrows each field", "[variadic][codegen]") {
    // A pack of value objects: each `args._k` must yield the tuple FIELD's address (a borrow into a
    // `Point&` param), not a load and not the whole-tuple pointer. All other spread tests use i32 packs.
    std::string ir = codegenString(R"(
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } }
        fn combine(Point& a, Point& b) -> i32 { return a.x + b.y; }
        fn wrap<...Ts>(Ts... args) -> i32 { return combine(args...); }
        fn main() -> i32 { Point p1(1, 2); Point p2(3, 4); return wrap(p1, p2); }
    )");
    // The second field is GEP'd (proving unwrap, not whole-tuple pass), and combine takes ptr args.
    REQUIRE(ir.find("getelementptr %Tuple$Point$Point, ptr ") != std::string::npos);
    REQUIRE(ir.find(", i32 0, i32 1") != std::string::npos);        // args._1 field access
    REQUIRE(ir.find("call i32 @combine(ptr ") != std::string::npos);
}

TEST_CASE("variadic - a wrong-arity spread is a clean error after unwrap", "[variadic][semantic]") {
    // The core promise of the design: unwrap first, THEN ordinary arity checking. A 2-element pack
    // spread into a 3-argument function is an ordinary arity mismatch, not a miscompile.
    StderrCapture cap;
    (void)analyzeString(R"(
        fn sum3(i32 a, i32 b, i32 c) -> i32 { return a + b + c; }
        fn wrap<...Ts>(Ts... args) -> i32 { return sum3(args...); }
        fn main() -> i32 { return wrap(1, 2); }
    )");
    REQUIRE(cap.contains("expects 3 argument(s), got 2"));
}

TEST_CASE("variadic - spreading into an INFERRED generic function is a clean error (v1 limit)", "[variadic][semantic]") {
    // `identity(args...)` (no explicit `<i32>`) unwraps to `identity(args._0)`, but the unwrapped
    // `args._0` is a tuple-field access whose type generic inference can't read (it handles only
    // identifiers / ctor / new, like deduceArgTypeToken) — so the type argument can't be inferred.
    // A clean error, not a miscompile; the explicit form `identity<i32>(args...)` works (tested above).
    // v1 limitation: spread into an inferred (no-`<…>`) generic requires the explicit type argument.
    StderrCapture cap;
    (void)analyzeString(R"(
        fn identity<T>(T x) -> T { return x; }
        fn wrap<...Ts>(Ts... args) -> i32 { return identity(args...); }
        fn main() -> i32 { return wrap(9); }
    )");
    REQUIRE(cap.contains("cannot infer type argument(s) for generic function 'identity'"));
}

// ============================================================
// Variadic METHODS — `fn m<...Ts>(Ts... args)` on a class, called without explicit `<…>` (the pack is
// inferred from the trailing args). Built on generic methods + the pack machinery: the concrete
// `Owner::m$Tuple$…` is injected into the class. The flagship use is `Array<T>::emplaceBack`.
// ============================================================

// A self-contained mini growable vector with a variadic emplaceBack (the stdlib Array<T> shape).
static const char* MINI_ARR = R"(
    extern malloc(u64 size) -> ptr;
    extern free(ptr p);
    class Vec<T> {
        private mut ptr<T> data;
        private mut u64 count;
        Vec() { count = 0; data = malloc(64 * sizeof(T)); }
        fn emplaceBack<...Ts>(Ts... args) mut { data[count] = T(args...); count = count + 1; }
        fn at(i32 i) -> T* { return data[i]; }
        fn size() -> u64 { return count; }
    }
)";

TEST_CASE("variadic-method - emplaceBack direct-constructs into the buffer (no temp, no clone)", "[variadic][genericmethod]") {
    std::string ir = codegenString(std::string(MINI_ARR) + R"(
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } }
        fn main() -> i32 { mut Vec<Point>& v = new Vec<Point>(); v.emplaceBack(3, 4); return 0; }
    )");
    // The variadic method takes the pack as a tuple borrow and constructs Point straight into the slot:
    // zero-init the buffer element then run its ctor in place — no element temp, no clone. (An `objtmp`
    // DOES appear, but only for the tuple pack ARGUMENT at the call site, not for the Point element.)
    REQUIRE(ir.find("define void @Vec$Point_emplaceBack$Tuple$i32$i32(ptr ") != std::string::npos);
    REQUIRE(ir.find("store %Point zeroinitializer") != std::string::npos);   // element built in place...
    REQUIRE(ir.find("call void @Point_Point(ptr ") != std::string::npos);    // ...via its ctor on the slot
    REQUIRE(ir.find("@Point_clone(") == std::string::npos);                  // no clone of the element
}

TEST_CASE("variadic-method - the call site collects the trailing args into a pack tuple", "[variadic][genericmethod]") {
    std::string ir = codegenString(std::string(MINI_ARR) + R"(
        class Pair { mut i32 a; mut i32 b; Pair(i32 x, i32 y) { a = x; b = y; } }
        fn main() -> i32 {
            mut Vec<Pair>& v = new Vec<Pair>();
            v.emplaceBack(1, 2);
            return 0;
        }
    )");
    REQUIRE(ir.find("%Tuple$i32$i32 = type { i32, i32 }") != std::string::npos);
    REQUIRE(ir.find("@Vec$Pair_emplaceBack$Tuple$i32$i32(ptr") != std::string::npos);
}

TEST_CASE("variadic-method - on a non-generic class, consumed by cons-match (heterogeneous pack)", "[variadic][genericmethod]") {
    std::string ir = codegenString(R"(
        class Counter {
            fn count<...Ts>(Ts... args) -> i32 {
                match args {
                    ()     -> { return 0; }
                    (x:xs) -> { return 1 + this.count(xs...); }
                }
            }
        }
        fn main() -> i32 { Counter c; return c.count(1, "hi", true); }
    )");
    // The recursion unrolls to concrete per-arity method instantiations (this.count(xs...)).
    REQUIRE(ir.find("@Counter_count$Tuple$i32$str$bool(") != std::string::npos);
    REQUIRE(ir.find("@Counter_count$Tuple$str$bool(")     != std::string::npos);
    REQUIRE(ir.find("@Counter_count$Tuple(")              != std::string::npos);   // unit base case
}

TEST_CASE("variadic-method - a fixed prefix parameter precedes the pack", "[variadic][genericmethod]") {
    std::string ir = codegenString(R"(
        class Adder {
            fn sumFrom<...Ts>(i32 base, Ts... args) -> i32 {
                match args {
                    ()     -> { return base; }
                    (x:xs) -> { return this.sumFrom(base + x, xs...); }
                }
            }
        }
        fn main() -> i32 { Adder a; return a.sumFrom(100, 1, 2); }
    )");
    // Each instantiation keeps the fixed `i32 base` and takes the (shrinking) pack tuple by borrow.
    REQUIRE(ir.find("@Adder_sumFrom$Tuple$i32$i32(ptr %self, i32") != std::string::npos);
    REQUIRE(ir.find("@Adder_sumFrom$Tuple(ptr %self, i32")         != std::string::npos);   // base case
}

TEST_CASE("variadic-method - a variadic pack must be the last type parameter", "[variadic][genericmethod]") {
    StderrCapture cap;
    (void)analyzeString(R"(
        class C { fn bad<...Ts, U>(Ts... args) -> i32 { return 0; } }
        fn main() -> i32 { C c; return 0; }
    )");
    REQUIRE(cap.contains("must be the last type parameter"));
}

TEST_CASE("variadic-method - a STATIC variadic method resolves via Class::m(...)", "[variadic][genericmethod]") {
    // Regression: the prescan required `fn IDENTIFIER <` and captureMethodTemplate only backstopped the
    // GENERIC-method sets — so a `fn static total<...>` (a `static` modifier before the name) was never
    // registered as variadic and the `Sum::total(...)` call fell through to a plain static call → "no
    // static method 'total'". The prescan now skips `static`/`private`, and capture backstops the
    // variadic sets. The instantiations take the pack tuple with NO `%self` (static dispatch).
    std::string ir = codegenString(R"(
        class Sum {
            fn static total<...Ts>(Ts... args) -> i32 {
                match args {
                    ()     -> { return 0; }
                    (x:xs) -> { return x + Sum::total(xs...); }
                }
            }
        }
        fn main() -> i32 { return Sum::total(1, 2, 3, 4); }
    )");
    REQUIRE(ir.find("@Sum_total$Tuple$i32$i32$i32$i32(ptr %args)") != std::string::npos);  // no %self
    REQUIRE(ir.find("@Sum_total$Tuple(ptr %args)")                 != std::string::npos);  // base case
}

TEST_CASE("variadic-method - a PRIVATE variadic method is registered (prescan skips the modifier)", "[variadic][genericmethod]") {
    // Same prescan-modifier root cause as the static case: `fn private tally<...>` must still register.
    std::string ir = codegenString(R"(
        class Counter {
            fn private tally<...Ts>(Ts... args) -> i32 {
                match args { () -> { return 0; } (x:xs) -> { return 1 + this.tally(xs...); } }
            }
            fn go() -> i32 { return this.tally(5, 6, 7); }
        }
        fn main() -> i32 { Counter c; return c.go(); }
    )");
    REQUIRE(ir.find("@Counter_tally$Tuple$i32$i32$i32(") != std::string::npos);
}

TEST_CASE("variadic-method - an empty pack (arity 0) and a single-element pack both instantiate", "[variadic][genericmethod]") {
    std::string ir = codegenString(R"(
        class C {
            fn count<...Ts>(Ts... args) -> i32 {
                match args { () -> { return 0; } (x:xs) -> { return 1 + this.count(xs...); } }
            }
        }
        fn main() -> i32 { C c; return c.count() + c.count(9); }
    )");
    REQUIRE(ir.find("@C_count$Tuple(")     != std::string::npos);   // arity 0 — the unit pack
    REQUIRE(ir.find("@C_count$Tuple$i32(") != std::string::npos);   // arity 1
}

TEST_CASE("variadic-method - an object-returning variadic method rides the sret convention", "[variadic][genericmethod]") {
    // A fixed prefix builds the object; the (here empty) pack still threads through. The return slot is
    // the hidden first `ptr` param, before `%self` and the fixed params.
    std::string ir = codegenString(R"(
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } }
        class Maker {
            fn make<...Ts>(i32 a, i32 b, Ts... args) -> Point p { p = Point(a, b); return p; }
        }
        fn main() -> i32 { Maker m; Point q = m.make(3, 4); return q.x + q.y; }
    )");
    REQUIRE(ir.find("define void @Maker_make$Tuple(ptr %p, ptr %self, i32 %a, i32 %b, ptr %args)") != std::string::npos);
}

TEST_CASE("variadic-method - a pack can be spread-forwarded into a variadic method", "[variadic][genericmethod]") {
    // A variadic FREE function forwards its pack into a variadic METHOD via `v.count(items...)` — the
    // method re-tuples and re-instantiates per arity, unrolling to the unit base case.
    std::string ir = codegenString(R"(
        class Vec {
            fn count<...Ts>(Ts... args) -> i32 {
                match args { () -> { return 0; } (x:xs) -> { return 1 + this.count(xs...); } }
            }
        }
        fn forward<...Us>(Vec* v, Us... items) -> i32 { return v.count(items...); }
        fn main() -> i32 { Vec v; return forward(v, 10, 20, 30); }
    )");
    REQUIRE(ir.find("@Vec_count$Tuple$i32$i32$i32(") != std::string::npos);
    REQUIRE(ir.find("@Vec_count$Tuple(")             != std::string::npos);
}

TEST_CASE("variadic-method - receiver resolves via a parameter and via a bare field", "[variadic][genericmethod]") {
    std::string ir = codegenString(R"(
        class Bag {
            fn count<...Ts>(Ts... args) -> i32 {
                match args { () -> { return 0; } (x:xs) -> { return 1 + this.count(xs...); } }
            }
        }
        fn viaParam(Bag* b) -> i32 { return b.count(1, 2); }
        class Holder { mut Bag bag; Holder() {} fn viaField() -> i32 { return bag.count(1, 2, 3); } }
        fn main() -> i32 { Bag b; Holder h; return viaParam(b) + h.viaField(); }
    )");
    REQUIRE(ir.find("@Bag_count$Tuple$i32$i32(")     != std::string::npos);   // via the param receiver
    REQUIRE(ir.find("@Bag_count$Tuple$i32$i32$i32(") != std::string::npos);   // via the field receiver
}

TEST_CASE("variadic-method - two classes sharing a variadic method name don't collide", "[variadic][genericmethod]") {
    std::string ir = codegenString(R"(
        class A { fn tag<...Ts>(Ts... args) -> i32 { return 1; } }
        class B { fn tag<...Ts>(Ts... args) -> i32 { return 2; } }
        fn main() -> i32 { A a; B b; return a.tag(1, 2) + b.tag(1, 2, 3); }
    )");
    REQUIRE(ir.find("@A_tag$Tuple$i32$i32(")     != std::string::npos);
    REQUIRE(ir.find("@B_tag$Tuple$i32$i32$i32(") != std::string::npos);
}
