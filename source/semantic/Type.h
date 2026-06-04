//
// Created by Vladimir Arsenijevic on 01.6.2026.
//

#ifndef GG_TYPE_H
#define GG_TYPE_H

#include <string>
#include "../lexer/Token.h"

// ---- TypeKind ----

enum class TypeKind {
    I8, I16, I32, I64,
    U8, U16, U32, U64,
    F32, F64,
    Bool, Char, String,
    Void,   // for functions that return nothing (future)
    Error   // sentinel: suppresses cascading errors
};

// ---- Type ----

struct Type {
    TypeKind kind;
    bool isConst    = false;
    bool isNullable = false;

    bool operator==(const Type&) const = default;
};

// ---- CastResult ----

enum class CastResult { None, Silent, Warn };

// ---- Classification predicates ----

bool isSignedInt(TypeKind k);
bool isUnsignedInt(TypeKind k);
bool isInteger(TypeKind k);
bool isFloat(TypeKind k);
bool isNumeric(TypeKind k);
bool isBoolCompatible(Type t);  // bool or any numeric
bool isError(Type t);

// ---- Type operations ----

CastResult  canImplicitlyCast(Type from, Type to);
Type        commonArithmeticType(Type a, Type b);
Type        typeFromToken(TokenType tt);
std::string typeName(Type t);

#endif //GG_TYPE_H
