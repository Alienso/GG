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
    Array,  // fixed-size stack array: element type + count stored in Type struct
    Object, // class instance — value, lives with its owner; className stores the class name
    Enum,   // enum value — a pointer to a global singleton variant; className stores the enum name
    Reference, // heap reference to a class instance (Ref<Class>, refcounted); className stores the pointee class
    TypedPtr,  // typed raw pointer ptr<T> (internal); elementKind (+ className) describe the element
    TypeParam, // abstract generic type parameter (e.g. `T`) during generic-body checking; className
               // holds the parameter name. Semantic-only — never reaches codegen (which sees only
               // monomorphized concrete decls). Bounds are looked up by name in the analyzer.
    Void,   // for functions that return nothing
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
// Decodes a parser-synthesized type token: "Class&" → Reference, "ptr<Elem>" → TypedPtr.
// Returns Type{Error} when `tok` is not such a synthesized token.
Type        decodeSynthesizedType(const Token& tok);

#endif //GG_TYPE_H
