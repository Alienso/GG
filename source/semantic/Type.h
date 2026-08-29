//
// Created by Vladimir Arsenijevic on 01.6.2026.
//

#ifndef GG_TYPE_H
#define GG_TYPE_H

#include <string>
#include <cstddef>
#include <vector>
#include "../lexer/Token.h"

// ---- TypeKind ----

enum class TypeKind {
    I8, I16, I32, I64,
    U8, U16, U32, U64,
    F32, F64,
    Bool, Char,
    Ptr,    // opaque pointer — maps to LLVM's ptr; used for FFI / CRT bindings
    Str,    // compile-time string literal: an immutable view over static bytes — { ptr, u64 len }.
            // A string literal has this type; `.data` decays to a (NUL-terminated) ptr, `.len` is
            // the byte length. Trivially copyable, no ownership/refcount (like a nullable primitive).
    Array,  // fixed-size stack array: element type + count stored in Type struct
    Object, // class instance — value, lives with its owner; className stores the class name
    Enum,   // enum value — a pointer to a global singleton variant; className stores the enum name
    Reference, // heap reference to a class instance (Ref<Class>, refcounted); className stores the pointee class
    TypedPtr,  // typed raw pointer ptr<T> (internal); elementKind (+ className) describe the element
    TypeParam, // abstract generic type parameter (e.g. `T`) during generic-body checking; className
               // holds the parameter name. Semantic-only — never reaches codegen (which sees only
               // monomorphized concrete decls). Bounds are looked up by name in the analyzer.
    Void,   // for functions that return nothing
    Null,   // the type of the `null` literal — assignable only to a nullable type `T?`
    Error   // sentinel: suppresses cascading errors
};

// ---- Type ----

struct Type {
    TypeKind    kind        = TypeKind::Error;
    bool        isConst     = false;
    bool        isNullable  = false;
    bool        borrow      = false;           // `ref T`: a non-owning borrow (only when kind ==
                                               // Reference). Same IR as an owning `Class&` (a ptr),
                                               // but never retained/released; `ref → Class&` forbidden.
                                               // A CLASS borrow sets className; a PRIMITIVE borrow
                                               // (`ref i32`) leaves className empty and stores the
                                               // element kind in elementKind (an lvalue ref, like
                                               // C++'s `int&` — auto-derefs on read, stores through
                                               // on write).
    bool        shared      = false;           // `Shared<Class>`: an OWNING heap reference like
                                               // `Class&`, but ATOMICALLY refcounted and "born
                                               // shared" (constructed in place, never aliased as a
                                               // raw `Class&`) — the cross-thread ownership handle.
                                               // Only when kind == Reference; className is the
                                               // pointee class. Reuses the reference machinery
                                               // (member access / dispatch); differs only at
                                               // refcount (atomic), construction (build-in-place),
                                               // cast (no coercion to `Class&`), and the
                                               // Sendable/Shareable markers. Mutually exclusive with
                                               // `borrow` in Phase 1. See docs/concurrency.md.
    TypeKind    elementKind = TypeKind::Error;  // only valid when kind == Array
    size_t      arraySize   = 0;               // only valid when kind == Array
    std::string className;                     // only valid when kind == Object

    // Default-constructs to TypeKind::Error (used as a sentinel throughout).
    Type() = default;

    // Single-kind constructor — avoids aggregate-init warnings when className
    // is not needed (the common case).
    explicit Type(TypeKind k) noexcept : kind(k) {}

    bool operator==(const Type&) const = default;
};

// Convenience constructor for array types.
inline Type makeArrayType(TypeKind elementKind, size_t size) {
    Type t{TypeKind::Array};
    t.elementKind = elementKind;
    t.arraySize   = size;
    return t;
}

// Convenience constructor for class instance types.
inline Type makeObjectType(const std::string& name) {
    Type t;
    t.kind      = TypeKind::Object;
    t.className = name;
    return t;
}

// Convenience constructor for enum value types. An enum value is a pointer to a
// global singleton variant; className stores the enum name.
inline Type makeEnumType(const std::string& name) {
    Type t;
    t.kind      = TypeKind::Enum;
    t.className = name;
    return t;
}

// Convenience constructor for heap reference types — Ref<Class>.
// Internally a refcounted pointer to a heap-allocated instance of `name`.
inline Type makeReferenceType(const std::string& name) {
    Type t;
    t.kind      = TypeKind::Reference;
    t.className = name;
    return t;
}

// A non-owning borrow `ref T`: a reference at the IR level (a ptr to the object body) that never
// participates in reference counting and cannot be widened to an owning `Class&`.
inline Type makeBorrowType(const std::string& name) {
    Type t;
    t.kind      = TypeKind::Reference;
    t.className = name;
    t.borrow    = true;
    return t;
}

// A non-owning borrow of a PRIMITIVE, `ref i32` (an lvalue reference like C++'s `int&`): a ptr to
// the primitive that never participates in refcounting. className stays empty; elementKind holds
// the borrowed primitive kind. Reads auto-deref (load); writes store through the pointer.
inline Type makePrimitiveBorrow(TypeKind elementKind) {
    Type t;
    t.kind        = TypeKind::Reference;
    t.borrow      = true;
    t.elementKind = elementKind;
    return t;
}

// An owning, atomically-refcounted heap reference `Shared<Class>` — the cross-thread ownership
// handle. Internally a Reference (a ptr to the heap body) with the `shared` flag; retain/release
// emit atomic ops, construction builds in place (born-shared), and it does not coerce to `Class&`.
inline Type makeSharedType(const std::string& name) {
    Type t;
    t.kind      = TypeKind::Reference;
    t.className = name;
    t.shared    = true;
    return t;
}

// True for a `Shared<Class>` handle.
inline bool isShared(const Type& t) { return t.kind == TypeKind::Reference && t.shared; }

// True for any `ref T` (class or primitive).
inline bool isBorrow(const Type& t) { return t.kind == TypeKind::Reference && t.borrow; }
// True only for `ref <primitive>` (an lvalue reference to a primitive; className empty).
inline bool isPrimitiveBorrow(const Type& t) { return isBorrow(t) && t.className.empty(); }
// The borrowed primitive's value type (only meaningful when isPrimitiveBorrow(t)).
inline Type borrowElementType(const Type& t) { return Type{t.elementKind}; }
// Lvalue-to-rvalue decay: reading a `ref <primitive>` in a value context yields the primitive
// (a load). Class borrows and everything else pass through unchanged.
inline Type decayPrimitiveBorrow(const Type& t) { return isPrimitiveBorrow(t) ? borrowElementType(t) : t; }

// Convenience constructor for typed pointers — ptr<T> (internal). The element is
// described by elementKind (and className when the element is a class/reference).
inline Type makeTypedPtr(TypeKind elementKind, const std::string& className = "") {
    Type t;
    t.kind        = TypeKind::TypedPtr;
    t.elementKind = elementKind;
    t.className   = className;
    return t;
}

// Convenience constructor for an abstract generic type parameter (`T`). Used only while
// checking a generic body against its bounds; the parameter's bounds are resolved by name.
inline Type makeTypeParam(const std::string& name) {
    Type t;
    t.kind      = TypeKind::TypeParam;
    t.className = name;
    return t;
}

// ---- Nullability (`T?`) ----
// A nullable type `T?` may hold `null` in addition to the values of `T`. For reference-like types
// (Reference/borrow/Enum) this is a machine-null pointer — the SAME representation as `T`, so
// `T` and `T?` are bit-identical (nullability is purely a compile-time distinction). `null` itself
// has TypeKind::Null and is assignable only where a nullable type is expected.
inline Type makeNullable(const Type& t) { Type n = t; n.isNullable = true; return n; }
inline bool isNullable(const Type& t)   { return t.isNullable; }
// The narrowed (non-null) form — used by `!!`, smart-casts, and `?:`.
inline Type stripNullable(const Type& t) { Type n = t; n.isNullable = false; return n; }
// The type of the `null` literal.
inline Type makeNullType() { return Type{TypeKind::Null}; }

// The element type of a ptr<T>.
inline Type typedPtrElement(const Type& t) {
    if (t.elementKind == TypeKind::Reference) return makeReferenceType(t.className);
    if (t.elementKind == TypeKind::Object)    return makeObjectType(t.className);
    return Type{t.elementKind};
}

// ---- CastResult ----

enum class CastResult { None, Silent, Warn };

// ---- Classification predicates ----

bool isSignedInt(TypeKind k);
bool isUnsignedInt(TypeKind k);
bool isInteger(TypeKind k);
bool isFloat(TypeKind k);
bool isNumeric(TypeKind k);
bool isBoolCompatible(const Type& t);  // bool or any numeric
bool isError(const Type& t);

// ---- Type operations ----

CastResult  canImplicitlyCast(const Type& from, const Type& to);
// Argument-position conversions. Same as canImplicitlyCast, but additionally permits a value
// object to be *borrowed* as a reference of the same class (`Vec2` → `Vec2&`): the callee
// receives the object's address (no copy, no refcount change). This is valid ONLY when passing
// an argument — the callee must treat the parameter as a pure borrow (never retain, store, or
// +1-return it, since a stack object has no refcount header at body-8). Binding contexts (var
// init, assignment, `return`) deliberately keep using canImplicitlyCast so they still reject it.
CastResult  canPassArgument(const Type& from, const Type& to);
// Overload mangling: a symbol-safe encoding of a type / of a full signature. Used by both
// the semantic analyzer (to name the chosen overload) and codegen (to name definitions).
std::string mangleType(const Type& t);
std::string mangleOverload(const std::string& base, const std::vector<Type>& params, const Type& ret);
Type        commonArithmeticType(const Type& a, const Type& b);
Type        typeFromToken(TokenType tt);
std::string typeName(const Type& t);

// Maps a primitive type keyword spelling ("i32", "ptr", …) to its TypeKind, or Error.
TypeKind    typeKindFromName(const std::string& name);

// Whether a non-negative integer literal of the given magnitude fits in integer type `t`.
// Signed types allow up to 2^(bits-1) — the magnitude of the most-negative value — so a negated
// literal at the boundary (e.g. `-128` for i8) is accepted. Non-integer `t` returns false.
bool        integerLiteralFits(unsigned long long magnitude, TypeKind t);

// Removes `_` digit-separator characters from a numeric literal lexeme (e.g. "1_000_000" →
// "1000000"). Safe to call on any lexeme (decimal, hex, or octal) — used both before parsing a
// literal's numeric value and before emitting it verbatim as IR text (a `_` is not valid in an
// LLVM constant).
std::string stripDigitSeparators(const std::string& lexeme);

// Whether a numeric-literal lexeme has a `0x`/`0X` (hex) or `0o`/`0O` (octal) prefix — NOT a
// legacy C-style "leading zero means octal" (deliberately: that's an ambiguity footgun, and a
// leading zero would also collide with `0` as a plain decimal literal).
bool        isPrefixedIntegerLiteral(const std::string& lexeme);

// Parses an integer literal lexeme — decimal, or `0x`/`0X`-hex, or `0o`/`0O`-octal, with any `_`
// digit-separators stripped first — into `magnitude`. Returns false (magnitude left at 0) on a
// malformed or too-large (> u64) literal, mirroring the try/catch-around-std::stoull pattern every
// call site used before this helper existed.
bool        parseIntegerLiteral(const std::string& lexeme, unsigned long long& magnitude);

// The stdlib synchronisation cells (`std.sync.Mutex<T>` / `RwLock<T>`) are recognised by the compiler
// by SIMPLE name — the last `.`-segment of the class name, with any `$…` monomorphization suffix
// stripped (so `std.sync.Mutex$Counter` and a bare `Mutex$i32` both match). They are inherently
// Shareable (the interior-mutability escape hatch) and their `.with`/`.read`/`.write` accessors are
// compiler-lowered. `isSyncCellName` = either; `isMutexName` / `isRwLockName` distinguish them.
bool        isMutexName(const std::string& className);
bool        isRwLockName(const std::string& className);
bool        isSyncCellName(const std::string& className);
// RAII lock guards (Phase 2.5) — recognised by simple name like the sync cells.
bool        isMutexGuardName(const std::string& className);
bool        isRwReadGuardName(const std::string& className);
bool        isRwWriteGuardName(const std::string& className);
bool        isGuardName(const std::string& className);
bool        isMutableGuardName(const std::string& className);
std::string guardOrCellElement(const std::string& className);
std::string guardClassForCell(const std::string& cellClass, const std::string& kind);
// Decodes a parser-synthesized type token: "Class&" → Reference, "ptr<Elem>" → TypedPtr.
// Returns Type{Error} when `tok` is not such a synthesized token.
Type        decodeSynthesizedType(const Token& tok);

// The inverse of the type-token resolvers: encodes a resolved Type back into a single Token whose
// (type, lexeme) round-trips through typeFromToken / decodeSynthesizedType to the same Type. Used
// by `var` local inference so the inferred type can be handed to codegen as an ordinary type token
// (all of genVarDecl's existing branches then work unchanged).
Token       synthTypeToken(const Type& t, int line);

#endif //GG_TYPE_H
