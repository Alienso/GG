#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// The built-in `Clone` trait — a user deep-copy hook (copy constructor). A class that owns a raw
// buffer `impl Clone` to deep-copy itself; the impl's `clone` method mangles to `@Class_clone` and
// transparently REPLACES the generated memberwise clone (identical (dest, src) ABI), so every clone
// site (value copy, container element) routes to it — recursively. Also relaxes the owning-`ptr`
// element guard.
// ============================================================

static const char* BYTES_CLONE = R"(
    extern malloc(u64 size) -> ptr;
    extern free(ptr p);
    extern memcpy(ptr dest, ptr src, u64 n) -> ptr;
    class Bytes {
        mut ptr<i32> buf;
        mut u64 len;
        Bytes(u64 n) { len = n; buf = malloc(n * sizeof(i32)); }
        ~Bytes() { free(buf); }
    }
    impl Clone for Bytes {
        fn clone(Bytes& src) mut {
            free(buf);
            len = src.len;
            buf = malloc(len * sizeof(i32));
            memcpy(buf, src.buf, len * sizeof(i32));
        }
    }
)";

TEST_CASE("Clone - a Clone impl replaces the generated clone (one deep-copy def)", "[clone][codegen]") {
    std::string ir = codegenString(std::string(BYTES_CLONE) + R"(
        fn main() -> i32 { Bytes a(4); Bytes b = a; return 0; }   // value copy → deep clone
    )");
    // Exactly one @Bytes_clone, and it is the user's deep copy (malloc + memcpy), not memberwise.
    size_t first = ir.find("define void @Bytes_clone");
    REQUIRE(first != std::string::npos);
    REQUIRE(ir.find("define void @Bytes_clone", first + 1) == std::string::npos);
    REQUIRE(ir.find("call ptr @memcpy") != std::string::npos);
    // The value copy `Bytes b = a` routes through @Bytes_clone.
    REQUIRE(ir.find("call void @Bytes_clone(ptr ") != std::string::npos);
}

TEST_CASE("Clone - an owning-ptr value object with a Clone impl is allowed as a ptr<T> element", "[clone][semantic]") {
    auto result = analyzeString(std::string(BYTES_CLONE) + R"(
        class Vec<T> {
            private mut ptr<T> data;
            private mut u64 count;
            Vec() { count = 0; data = malloc(64 * sizeof(T)); }
            fn push(T* value) mut { data[count] = value; count = count + 1; }
        }
        fn main() -> i32 { mut Vec<Bytes>& v = new Vec<Bytes>(); Bytes b(4); v.push(b); return 0; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Clone - an owning-ptr value object WITHOUT a Clone impl is still rejected", "[clone][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        extern malloc(u64 size) -> ptr;
        class Buf { mut ptr<i32> raw; Buf() { raw = malloc(16); } }
        class Vec<T> {
            private mut ptr<T> data;
            private mut u64 count;
            Vec() { count = 0; data = malloc(64 * sizeof(T)); }
            fn push(T* value) mut { data[count] = value; count = count + 1; }
        }
        fn main() -> i32 { mut Vec<Buf>& v = new Vec<Buf>(); Buf b(); v.push(b); return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("impl Clone"));
}

TEST_CASE("Clone - a value copy of a Clone type routes through the impl (deep, not shallow)", "[clone][codegen]") {
    std::string ir = codegenString(std::string(BYTES_CLONE) + R"(
        fn dup(Bytes& src) -> Bytes out { out = src; return out; }   // clone into the sret slot
        fn main() -> i32 { Bytes a(3); Bytes c = dup(a); return 0; }
    )");
    REQUIRE(ir.find("call void @Bytes_clone(") != std::string::npos);
    // No generated memberwise fallback that would shallow-copy the `buf` pointer.
    REQUIRE(ir.find("define void @Bytes_clone", ir.find("define void @Bytes_clone") + 1) == std::string::npos);
}

TEST_CASE("Clone - an embedded Clone field routes the generated memberwise clone to the impl", "[clone][codegen]") {
    // `Wrap` does NOT impl Clone, so it gets the GENERATED memberwise clone — which, for its
    // embedded `Bytes b` field, must call @Bytes_clone (the user's deep copy). Recursion composes
    // through plain embedding, not just Array elements.
    std::string ir = codegenString(std::string(BYTES_CLONE) + R"(
        class Wrap { mut Bytes b; Wrap(u64 n) { b = Bytes(n); } }
        fn main() -> i32 { Wrap x(2); Wrap y = x; return 0; }   // value copy → Wrap_clone → Bytes_clone
    )");
    // Wrap's clone is generated (memberwise); Bytes's is the user impl. Both exist.
    REQUIRE(ir.find("define void @Wrap_clone") != std::string::npos);
    REQUIRE(ir.find("define void @Bytes_clone") != std::string::npos);
    // The generated Wrap_clone delegates the embedded field to Bytes's clone.
    size_t wrapDef = ir.find("define void @Wrap_clone");
    size_t wrapEnd = ir.find("define ", wrapDef + 1);
    std::string wrapBody = ir.substr(wrapDef, wrapEnd - wrapDef);
    REQUIRE(wrapBody.find("call void @Bytes_clone(ptr ") != std::string::npos);
    // The copy destination is zero-initialized before the clone, so Bytes_clone's `free(this.buf)`
    // is a null no-op on the fresh slot (not a free of garbage).
    REQUIRE(ir.find("store %Wrap zeroinitializer") != std::string::npos);
}

TEST_CASE("Clone - reassigning a live value object routes through the impl (copy-assign)", "[clone][codegen]") {
    std::string ir = codegenString(std::string(BYTES_CLONE) + R"(
        fn main() -> i32 { mut Bytes a(4); Bytes b(2); a = b; return 0; }   // live dest → clone-assign
    )");
    // The reassignment `a = b` deep-copies b into a via @Bytes_clone (which releases a's old buffer).
    REQUIRE(ir.find("call void @Bytes_clone(ptr ") != std::string::npos);
}

TEST_CASE("Clone - the built-in trait cannot be redeclared", "[clone][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        trait Clone { fn clone(Self& src) mut; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("built-in"));
}

// ============================================================
// Copy guard: a value object that (transitively) owns a raw ptr and does NOT impl Clone cannot be
// COPIED by value — memberwise clone shallow-copies the pointer, aliasing the buffer (leak + double
// free). This turns a silent heap-corruption crash into a clean compile error. Constructing a fresh
// value, or binding/passing by reference, is unaffected; a Clone impl deep-copies and is allowed.
// ============================================================

static const char* RAW_NO_CLONE = R"(
    extern malloc(u64 size) -> ptr;
    extern free(ptr p);
    class Buf {
        mut ptr<i32> data;
        Buf(i32 s) { data = malloc(16); }
        ~Buf() { free(data); }
    }
)";

TEST_CASE("Clone - assigning a raw-ptr class without Clone is a clean error", "[clone][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(std::string(RAW_NO_CLONE) + R"(
        fn main() -> i32 { mut Buf b(1); b = Buf(2); return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("impl Clone for Buf"));
}

TEST_CASE("Clone - copy-initializing a raw-ptr class without Clone is a clean error", "[clone][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(std::string(RAW_NO_CLONE) + R"(
        fn main() -> i32 { Buf a(1); Buf c = a; return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("owns a raw pointer"));
}

TEST_CASE("Clone - directly constructing a raw-ptr class without Clone is allowed (no copy)", "[clone][semantic]") {
    // `Buf a = Buf(1)` builds the object in place — no copy, no alias — so it must NOT be rejected.
    auto result = analyzeString(std::string(RAW_NO_CLONE) + R"(
        fn main() -> i32 { Buf a = Buf(1); return 0; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Clone - binding a raw-ptr class by reference is allowed (no copy)", "[clone][semantic]") {
    auto result = analyzeString(std::string(RAW_NO_CLONE) + R"(
        fn take(Buf* b) -> i32 { return 0; }
        fn main() -> i32 { Buf a(1); Buf* r = a; return take(r); }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Clone - copying a raw-ptr class WITH a Clone impl is allowed", "[clone][semantic]") {
    // Bytes owns a raw ptr but impls Clone → assignment/copy deep-copies via the impl, so it's fine.
    auto result = analyzeString(std::string(BYTES_CLONE) + R"(
        fn main() -> i32 { mut Bytes a(2); Bytes b(3); a = b; Bytes c = b; return 0; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Clone - copying a class embedding a Clone field is allowed (recursion is safe)", "[clone][semantic]") {
    // `Wrap` doesn't impl Clone, but its only raw ptr is inside an embedded `Bytes` field that DOES —
    // the generated memberwise clone recurses through @Bytes_clone (deep). So a copy is safe.
    auto result = analyzeString(std::string(BYTES_CLONE) + R"(
        class Wrap { mut Bytes b; Wrap() { b = Bytes(0); } }
        fn main() -> i32 { mut Wrap x(); Wrap y = x; return 0; }
    )");
    REQUIRE_FALSE(result.hadError);
}
