#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// `str` — the compile-time string-literal type
// ============================================================
// A string literal has type `str`: an immutable view over static bytes, lowered to a
// { ptr, i64 } value (data pointer + byte length). `.data` decays to a NUL-terminated `ptr`
// (for FFI / CRT), `.len` is the byte length (u64). `str` is safe (not --unsafe-ptr gated) and
// implicitly decays to `ptr`; the reverse (ptr -> str) is forbidden. Trivially copyable, no
// ownership / refcount.

// ------------------------------------------------------------
// Literal typing + representation
// ------------------------------------------------------------

TEST_CASE("str - a string literal lowers to a { ptr, i64 } view with its byte length", "[str]") {
    auto ir = codegenString(R"(
        fn main() -> i32 { str s = "hello"; return 0; }
    )");
    REQUIRE(ir.find("insertvalue { ptr, i64 }") != std::string::npos);
    REQUIRE(ir.find("i64 5, 1") != std::string::npos);   // "hello" -> byte length 5
}

TEST_CASE("str - a multibyte literal counts BYTES, not codepoints", "[str]") {
    // "café" is 5 bytes (é is 2 UTF-8 bytes), 4 codepoints — str.len is the byte length.
    auto ir = codegenString(R"(
        fn main() -> i32 { str s = "café"; return 0; }
    )");
    REQUIRE(ir.find("i64 5, 1") != std::string::npos);
}

TEST_CASE("str - a string literal is inferred safe (var, no --unsafe-ptr)", "[str][var]") {
    auto result = analyzeString(R"(
        fn main() -> i32 { var s = "literal"; return 0; }
    )", CompilerOptions{});   // allowRawPtr = false
    REQUIRE_FALSE(result.hadError);
}

// ------------------------------------------------------------
// .len / .data accessors
// ------------------------------------------------------------

TEST_CASE("str - .len extracts field 1 (u64)", "[str]") {
    auto ir = codegenString(R"(
        fn main() -> i32 { str s = "abcd"; u64 n = s.len; return 0; }
    )");
    REQUIRE(ir.find("extractvalue { ptr, i64 }") != std::string::npos);
    REQUIRE(ir.find(", 1") != std::string::npos);   // field index 1
}

TEST_CASE("str - .data extracts field 0 (ptr)", "[str]") {
    auto ir = codegenString(R"(
        fn main() -> i32 { str s = "abc"; ptr p = s.data; return 0; }
    )", defaultTestOptions());   // ptr local needs allowRawPtr
    REQUIRE(ir.find("extractvalue { ptr, i64 }") != std::string::npos);
}

TEST_CASE("str - an unknown member is an error", "[str][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 { str s = "x"; return s.bogus; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("has no member"));
}

// ------------------------------------------------------------
// str -> ptr decay (and the forbidden reverse)
// ------------------------------------------------------------

TEST_CASE("str - decays to ptr when passed to a ptr parameter (FFI)", "[str]") {
    // A string literal passed to an extern `ptr` parameter decays via its data pointer.
    auto ir = codegenString(R"(
        extern puts(ptr s) -> i32;
        fn main() -> i32 { puts("hi"); return 0; }
    )");
    REQUIRE(ir.find("extractvalue { ptr, i64 }") != std::string::npos);   // decay = field 0
    REQUIRE(ir.find("@puts") != std::string::npos);
}

TEST_CASE("str - a raw ptr does NOT implicitly convert to str", "[str][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        extern grab() -> ptr;
        fn main() -> i32 { str s = grab(); return 0; }
    )", defaultTestOptions());
    REQUIRE(result.hadError);
}

TEST_CASE("str - `str*` (borrowing a str) is a parse error", "[str][parser]") {
    StderrCapture cap;
    parseString("fn f(str* s) { }");
    REQUIRE(cap.contains("cannot be borrowed"));
}

// ------------------------------------------------------------
// str as a parameter / return type
// ------------------------------------------------------------

TEST_CASE("str - is usable as a parameter and return type", "[str]") {
    auto ir = codegenString(R"(
        fn tag() -> str { return "T"; }
        fn width(str s) -> u64 { return s.len; }
        fn main() -> i32 { str t = tag(); u64 w = width(t); return 0; }
    )");
    // Param + return lower to the { ptr, i64 } value type.
    REQUIRE(ir.find("{ ptr, i64 }") != std::string::npos);
}

// ------------------------------------------------------------
// String(str) construction
// ------------------------------------------------------------

// ------------------------------------------------------------
// Edge cases / clean rejections (Phase 1 defers these — they must ERROR, not miscompile)
// ------------------------------------------------------------

TEST_CASE("str - `==` / `!=` is rejected cleanly (not a bogus numeric compare)", "[str][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 { if ("a" == "b") { return 1; } return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("not supported on 'str'"));
}

TEST_CASE("str - a relational operator on str is rejected", "[str][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 { if ("a" < "b") { return 1; } return 0; }
    )");
    REQUIRE(result.hadError);   // "must be numeric, got str"
}

TEST_CASE("str - `str?` (nullable str) is rejected", "[str][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 { str? s = null; return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("not yet nullable"));
}

TEST_CASE("str - .len counts escape sequences as single bytes", "[str]") {
    // "a\nb" is 3 bytes: 'a', newline, 'b'.
    auto ir = codegenString(R"(
        fn main() -> i32 { str s = "a\nb"; return 0; }
    )");
    REQUIRE(ir.find("i64 3, 1") != std::string::npos);
}

TEST_CASE("str - is usable as a value-object field", "[str]") {
    auto ir = codegenString(R"(
        class C { str name; C(str n) { this.name = n; } fn width() -> u64 { return this.name.len; } }
        fn main() -> i32 { C c("hello"); return 0; }
    )", defaultTestOptions());
    REQUIRE(ir.find("%C = type") != std::string::npos);       // str field is part of the struct layout
    REQUIRE(ir.find("@C_C") != std::string::npos);
}

TEST_CASE("str - String(str) constructs from a literal without strlen", "[str][stdlib]") {
    // The stdlib String ctor takes a `str`; it uses `.len` (no strlen call needed).
    auto ir = codegenString(R"(
        class String {
            private ptr data; private u64 size;
            String(str literal) { this.size = literal.len; }
            fn size2() -> u64 { return this.size; }
        }
        fn main() -> i32 { String s("hello"); return 0; }
    )", defaultTestOptions());
    REQUIRE(ir.find("@String_String") != std::string::npos);
    REQUIRE(ir.find("extractvalue { ptr, i64 }") != std::string::npos);   // reads literal.len
}

// ------------------------------------------------------------
// Indexing — `s[i]` is the i-th byte as a `char`
// ------------------------------------------------------------

TEST_CASE("str - indexing yields a char", "[str]") {
    auto result = analyzeString(R"(
        fn main() -> i32 { str s = "ABC"; char c = s[0]; return 0; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("str - indexing lowers to extract data ptr + GEP + load i8 + zext", "[str][codegen]") {
    auto ir = codegenString(R"(
        fn main() -> i32 { str s = "ABC"; char c = s[1]; return 0; }
    )");
    REQUIRE(ir.find("extractvalue { ptr, i64 }") != std::string::npos);   // data pointer
    REQUIRE(ir.find("getelementptr i8, ptr")     != std::string::npos);   // byte address
    REQUIRE(ir.find("zext i8")                   != std::string::npos);   // byte -> char (i32)
}

TEST_CASE("str - indexing bounds-checks the index against .len", "[str][codegen]") {
    auto ir = codegenString(R"(
        fn main() -> i32 { str s = "ABC"; char c = s[1]; return 0; }
    )");
    // The byte length (field 1) is extracted and the index is `icmp ult`-compared against it,
    // branching to an abort() on out-of-bounds (traps like a fixed-size array index).
    REQUIRE(ir.find("extractvalue { ptr, i64 }") != std::string::npos);   // .len (field 1)
    REQUIRE(ir.find("icmp ult i64")              != std::string::npos);   // idx < len
    REQUIRE(ir.find("@abort()")                  != std::string::npos);   // OOB trap
    // The trap prints a diagnostic to stderr before aborting.
    REQUIRE(ir.find("GG runtime error: index out of bounds") != std::string::npos);
}

TEST_CASE("str - an indexed byte is usable in arithmetic as a char", "[str]") {
    // `(s[i] as i32) - 48` — the digit-parsing idiom.
    auto result = analyzeString(R"(
        fn main() -> i32 { str s = "7"; i32 d = (s[0] as i32) - 48; return d; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("str - a non-integer index is rejected", "[str][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 { str s = "ABC"; char c = s["x"]; return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("index must be an integer"));
}

TEST_CASE("str - write-through 's[i] = x' is rejected with a clear message", "[str][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 { str s = "ABC"; s[0] = 'Z'; return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("immutable view"));   // not "cannot be indexed" — reads DO work
}

TEST_CASE("str - a `str` field can be indexed", "[str]") {
    // `obj.field[i]` where the field is a str: the member access yields the { ptr, i64 } value,
    // then the str-index path applies.
    auto result = analyzeString(R"(
        class C { str name; C(str n) { this.name = n; } fn first() -> char { return this.name[0]; } }
        fn main() -> i32 { C c("hi"); return 0; }
    )", defaultTestOptions());
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("str - a call-result str can be indexed", "[str]") {
    // The receiver is evaluated once (the call), then the byte is extracted.
    auto result = analyzeString(R"(
        fn label() -> str { return "hi"; }
        fn main() -> i32 { char c = label()[0]; return 0; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("str - a reflection @typeName view is indexable", "[str][reflect]") {
    // @typeName(T) is a str, so it supports the same byte indexing.
    auto result = analyzeString(R"(
        fn main() -> i32 { char c = @typeName(i32)[0]; return 0; }
    )");
    REQUIRE_FALSE(result.hadError);
}
