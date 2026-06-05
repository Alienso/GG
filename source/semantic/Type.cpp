//
// Created by Vladimir Arsenijevic on 01.6.2026.
//

#include "Type.h"

// ============================================================
// Classification predicates
// ============================================================

bool isSignedInt(TypeKind k) {
    return k == TypeKind::I8  || k == TypeKind::I16 ||
           k == TypeKind::I32 || k == TypeKind::I64;
}

bool isUnsignedInt(TypeKind k) {
    return k == TypeKind::U8   || k == TypeKind::U16  ||
           k == TypeKind::U32  || k == TypeKind::U64  ||
           k == TypeKind::Char;
}

bool isInteger(TypeKind k) { return isSignedInt(k) || isUnsignedInt(k); }

bool isFloat(TypeKind k) {
    return k == TypeKind::F32 || k == TypeKind::F64;
}

bool isNumeric(TypeKind k) { return isInteger(k) || isFloat(k); }

bool isBoolCompatible(Type t) {
    return t.kind == TypeKind::Bool || isNumeric(t.kind);
}

bool isError(Type t) { return t.kind == TypeKind::Error; }

// ============================================================
// Internal helper: bit-width of an integer or float kind
// ============================================================

static int bitWidth(TypeKind k) {
    switch (k) {
        case TypeKind::I8:  case TypeKind::U8:               return 8;
        case TypeKind::I16: case TypeKind::U16:              return 16;
        case TypeKind::I32: case TypeKind::U32:
        case TypeKind::Char:                                  return 32; // char = u32
        case TypeKind::I64: case TypeKind::U64:              return 64;
        case TypeKind::F32:                                   return 32;
        case TypeKind::F64:                                   return 64;
        default:                                              return 0;
    }
}

// ============================================================
// canImplicitlyCast
// ============================================================

CastResult canImplicitlyCast(Type from, Type to) {
    if (from == to)                          return CastResult::Silent;  // identity
    if (isError(from) || isError(to))        return CastResult::None;

    // char and u32 share the same underlying representation (char = Unicode code point)
    TypeKind f = from.kind, t = to.kind;
    if ((f == TypeKind::Char && t == TypeKind::U32) ||
        (f == TypeKind::U32  && t == TypeKind::Char))
        return CastResult::Silent;

    // string and ptr are both 'ptr' in LLVM IR — interchangeable at the FFI boundary
    if ((f == TypeKind::String && t == TypeKind::Ptr) ||
        (f == TypeKind::Ptr    && t == TypeKind::String))
        return CastResult::Silent;

    // Any integer → float (silent widening)
    if (isInteger(f) && isFloat(t))          return CastResult::Silent;

    // f32 → f64 (silent float widening)
    if (f == TypeKind::F32 && t == TypeKind::F64) return CastResult::Silent;

    // f64 → f32 (warn — narrowing float, may lose precision)
    if (f == TypeKind::F64 && t == TypeKind::F32) return CastResult::Warn;

    // float → any integer (warn — floor/truncate toward −∞)
    if (isFloat(f) && isInteger(t))          return CastResult::Warn;

    // Signed integer widening: i8 → i16 → i32 → i64
    if (isSignedInt(f) && isSignedInt(t)) {
        if (bitWidth(t) > bitWidth(f))       return CastResult::Silent;
    }

    // Unsigned integer widening: u8 → u16 → u32 → u64
    if (isUnsignedInt(f) && isUnsignedInt(t)) {
        if (bitWidth(t) > bitWidth(f))       return CastResult::Silent;
    }

    // Signed → unsigned (warn — value may be negative at runtime)
    if (isSignedInt(f) && isUnsignedInt(t)) return CastResult::Warn;

    // Unsigned → signed
    if (isUnsignedInt(f) && isSignedInt(t)) {
        int fw = bitWidth(f);
        int tw = bitWidth(t);
        if (tw > fw)  return CastResult::Silent;  // signed strictly wider → always fits
        if (tw == fw) return CastResult::Warn;    // same size → may overflow
        // signed narrower than unsigned → None (not safe)
    }

    return CastResult::None;
}

// ============================================================
// commonArithmeticType
// ============================================================

Type commonArithmeticType(Type a, Type b) {
    if (isError(a) || isError(b)) return Type{TypeKind::Error};

    // Bool == Bool stays as Bool (i1 comparison in IR)
    if (a.kind == TypeKind::Bool && b.kind == TypeKind::Bool) return Type{TypeKind::Bool};

    // Float dominates
    if (a.kind == TypeKind::F64 || b.kind == TypeKind::F64) return Type{TypeKind::F64};
    if (a.kind == TypeKind::F32 || b.kind == TypeKind::F32) return Type{TypeKind::F32};

    // Both must be integers at this point
    if (!isInteger(a.kind) || !isInteger(b.kind)) return Type{TypeKind::Error};

    int wa = bitWidth(a.kind);
    int wb = bitWidth(b.kind);

    // Both signed → wider wins
    if (isSignedInt(a.kind) && isSignedInt(b.kind))
        return Type{ wa >= wb ? a.kind : b.kind };

    // Both unsigned → wider wins
    if (isUnsignedInt(a.kind) && isUnsignedInt(b.kind))
        return Type{ wa >= wb ? a.kind : b.kind };

    // Mixed signed/unsigned — unsigned wins when width ≥ signed (mirrors C rules)
    TypeKind uk = isUnsignedInt(a.kind) ? a.kind : b.kind;
    TypeKind sk = isSignedInt(a.kind)   ? a.kind : b.kind;
    return bitWidth(uk) >= bitWidth(sk) ? Type{uk} : Type{sk};
}

// ============================================================
// typeFromToken
// ============================================================

Type typeFromToken(TokenType tt) {
    switch (tt) {
        case TokenType::I8:          return Type{TypeKind::I8};
        case TokenType::I16:         return Type{TypeKind::I16};
        case TokenType::I32:         return Type{TypeKind::I32};
        case TokenType::I64:         return Type{TypeKind::I64};
        case TokenType::U8:          return Type{TypeKind::U8};
        case TokenType::U16:         return Type{TypeKind::U16};
        case TokenType::U32:         return Type{TypeKind::U32};
        case TokenType::U64:         return Type{TypeKind::U64};
        case TokenType::F32:         return Type{TypeKind::F32};
        case TokenType::F64:         return Type{TypeKind::F64};
        case TokenType::BOOL:        return Type{TypeKind::Bool};
        case TokenType::CHAR_TYPE:   return Type{TypeKind::Char};
        case TokenType::STRING_TYPE: return Type{TypeKind::String};
        case TokenType::VOID:        return Type{TypeKind::Void};
        case TokenType::PTR:         return Type{TypeKind::Ptr};
        default:                     return Type{TypeKind::Error};
    }
}

// ============================================================
// typeName
// ============================================================

std::string typeName(Type t) {
    switch (t.kind) {
        case TypeKind::I8:     return "i8";
        case TypeKind::I16:    return "i16";
        case TypeKind::I32:    return "i32";
        case TypeKind::I64:    return "i64";
        case TypeKind::U8:     return "u8";
        case TypeKind::U16:    return "u16";
        case TypeKind::U32:    return "u32";
        case TypeKind::U64:    return "u64";
        case TypeKind::F32:    return "f32";
        case TypeKind::F64:    return "f64";
        case TypeKind::Bool:   return "bool";
        case TypeKind::Char:   return "char";
        case TypeKind::String: return "string";
        case TypeKind::Ptr:    return "ptr";
        case TypeKind::Array:  return typeName(Type{t.elementKind}) + "[" + std::to_string(t.arraySize) + "]";
        case TypeKind::Void:   return "void";
        case TypeKind::Error:  return "<error>";
    }
    return "<unknown>";
}
