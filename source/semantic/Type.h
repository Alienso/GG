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
    Void,   // for functions that return nothing
    Error   // sentinel: suppresses cascading errors
};

// ---- Type ----

struct Type {
    TypeKind    kind        = TypeKind::Error;
    bool        isConst     = false;
    bool        isNullable  = false;
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

// Convenience constructor for typed pointers — ptr<T> (internal). The element is
// described by elementKind (and className when the element is a class/reference).
inline Type makeTypedPtr(TypeKind elementKind, const std::string& className = "") {
    Type t;
    t.kind        = TypeKind::TypedPtr;
    t.elementKind = elementKind;
    t.className   = className;
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
