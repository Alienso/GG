//
// Created by Vladimir Arsenijevic on 01.6.2026.
//

#ifndef GG_TYPE_H
#define GG_TYPE_H

#include <string>
#include <cstddef>
#include "../lexer/Token.h"

// ---- TypeKind ----

enum class TypeKind {
    I8, I16, I32, I64,
    U8, U16, U32, U64,
    F32, F64,
    Bool, Char,
    Ptr,    // opaque pointer — maps to LLVM's ptr; used for FFI / CRT bindings
    Array,  // fixed-size stack array: element type + count stored in Type struct
    Object, // class instance — stack-allocated; className stores the class name
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

    bool operator==(const Type&) const = default;
};

// Convenience constructor for array types.
inline Type makeArrayType(TypeKind elementKind, size_t size) {
    return Type{TypeKind::Array, false, false, elementKind, size};
}

// Convenience constructor for class instance types.
inline Type makeObjectType(const std::string& name) {
    Type t;
    t.kind      = TypeKind::Object;
    t.className = name;
    return t;
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
Type        commonArithmeticType(const Type& a, const Type& b);
Type        typeFromToken(TokenType tt);
std::string typeName(const Type& t);

#endif //GG_TYPE_H
