#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// Reference-counting codegen regressions.
//
// The authoritative leak coverage is the runtime program e2e/refcount_leak_test.gg (a live-object
// counter proves every heap allocation is released exactly once). These IR-level unit tests lock
// the two specific ownership bugs that test surfaced, so a regression fails fast in the unit suite:
//
//   1. A reference RETURN ALIAS (`fn f() -> T& r { ... return r; }`) must TRANSFER the +1 it
//      already owns — not retain again. The old code retained on return but never released the
//      alias, returning a +2 object → a one-object leak per call.
//   2. A `new Outer(new Inner())` in a value-producing position must CLAIM the outer object (the
//      returned/bound value), leaving only the inner temp to be released. The old claimTemp only
//      matched the most-recent pending temp, so the inner `new` shadowed the outer one → the outer
//      object was released as a temp and the caller got freed memory (double-free / heap corruption).
// ============================================================

namespace {
    // Count non-overlapping occurrences of `needle` in `haystack`.
    size_t countOccurrences(const std::string& haystack, const std::string& needle) {
        size_t n = 0;
        for (size_t pos = haystack.find(needle); pos != std::string::npos;
             pos = haystack.find(needle, pos + needle.size()))
            ++n;
        return n;
    }

    // Extract the body of the first LLVM function whose definition line contains `signature`,
    // from that line up to (and including) the closing "\n}".
    std::string functionBody(const std::string& ir, const std::string& signature) {
        size_t start = ir.find(signature);
        if (start == std::string::npos) return "";
        size_t end = ir.find("\n}", start);
        return end == std::string::npos ? ir.substr(start) : ir.substr(start, end - start);
    }
}

TEST_CASE("Refcount - a reference return alias transfers ownership (no spurious retain)",
          "[refcount][codegen][return-slot]") {
    std::string ir = codegenString(R"(
        class Node { mut i32 v; Node(i32 x) { v = x; } ~Node() { } fn get() -> i32 { return v; } }
        fn makeAlias(i32 x) -> Node& r { r = new Node(x); return r; }
        fn main() -> i32 { Node& n = makeAlias(3); return n.get(); }
    )");
    std::string body = functionBody(ir, "define ptr @makeAlias(");
    REQUIRE_FALSE(body.empty());
    // The alias's +1 comes from `new` (claimed by the assignment) and is handed to the caller
    // directly. No retain anywhere in the function — an extra one is the leak bug.
    REQUIRE(countOccurrences(body, "call void @gg_retain") == 0);
    REQUIRE(body.find("ret ptr") != std::string::npos);
}

TEST_CASE("Refcount - a borrowed reference return alias retains exactly once",
          "[refcount][codegen][return-slot]") {
    // When the alias is assigned a BORROWED reference (not a fresh `new`), the assignment retains
    // it once to produce the +1; that single +1 is then transferred on return (no second retain).
    std::string ir = codegenString(R"(
        class Node { mut i32 v; Node(i32 x) { v = x; } ~Node() { } fn get() -> i32 { return v; } }
        fn choose(Node& a, Node& b, bool first) -> Node& r {
            if (first) { r = a as mut Node&; } else { r = b as mut Node&; }
            return r;
        }
        fn main() -> i32 { return 0; }
    )");
    std::string body = functionBody(ir, "define ptr @choose(");
    REQUIRE_FALSE(body.empty());
    // One retain per branch that assigns the borrow (2 branches) — and crucially NOT a further
    // retain on the `return r` path.
    REQUIRE(countOccurrences(body, "call void @gg_retain") == 2);
}

TEST_CASE("Refcount - nested new in a reference return claims the outer object",
          "[refcount][codegen]") {
    std::string ir = codegenString(R"(
        class Node { mut i32 v; Node(i32 x) { v = x; } ~Node() { } fn get() -> i32 { return v; } }
        class Box { mut Node& item; Box(Node& n) { item = n; } fn kid() -> Node& { return item; } }
        fn make() -> Box& { return new Box(new Node(1)); }
        fn main() -> i32 { return 0; }
    )");
    std::string body = functionBody(ir, "define ptr @make(");
    REQUIRE_FALSE(body.empty());
    // Exactly one release inside make: the inner `new Node` temp (its +1 was consumed by Box's
    // field retain). The outer `new Box` — the returned value — must be CLAIMED, not released.
    // The bug released both, so this was 2 and the returned pointer was freed before `ret`.
    REQUIRE(countOccurrences(body, "call void @gg_release") == 1);
    REQUIRE(body.find("ret ptr") != std::string::npos);
}

TEST_CASE("Refcount - nested new in a variable initializer claims the outer object",
          "[refcount][codegen]") {
    std::string ir = codegenString(R"(
        class Node { mut i32 v; Node(i32 x) { v = x; } ~Node() { } }
        class Box { mut Node& item; Box(Node& n) { item = n; } }
        fn use() { Box& b = new Box(new Node(1)); }
        fn main() -> i32 { return 0; }
    )");
    std::string body = functionBody(ir, "define void @use(");
    REQUIRE_FALSE(body.empty());
    // The inner Node temp is released once inside the full expression; the outer Box is claimed by
    // the binding `b` and released once at scope exit → two releases total, none of them freeing a
    // still-referenced object. (The bug freed the Box mid-initializer.)
    REQUIRE(countOccurrences(body, "call void @gg_release") == 2);
}
