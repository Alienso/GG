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

bool isBoolCompatible(const Type& t) {
    return t.kind == TypeKind::Bool || isNumeric(t.kind);
}

bool isError(const Type& t) { return t.kind == TypeKind::Error; }

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

CastResult canImplicitlyCast(const Type& from, const Type& to) {
    if (from == to)                          return CastResult::Silent;  // identity
    if (isError(from) || isError(to))        return CastResult::None;

    // char and u32 share the same underlying representation (char = Unicode code point)
    TypeKind f = from.kind, t = to.kind;
    if ((f == TypeKind::Char && t == TypeKind::U32) ||
        (f == TypeKind::U32  && t == TypeKind::Char))
        return CastResult::Silent;

    // Heap reference → value of the same class: permitted. In an assignment or
    // initializer this performs a deep copy (clone); when passed to a value
    // parameter it borrows. (The reverse, value → reference, requires explicit
    // `new Class(value)`.)
    if (f == TypeKind::Reference && t == TypeKind::Object && from.className == to.className)
        return CastResult::Silent;

    // Typed raw pointers ptr<T> are interchangeable with the opaque ptr type and
    // with each other (they are all just `ptr` in the IR). These are internal,
    // low-level conversions used by container implementations.
    if (f == TypeKind::TypedPtr && t == TypeKind::TypedPtr) return CastResult::Silent;
    if (f == TypeKind::TypedPtr && t == TypeKind::Ptr)      return CastResult::Silent;
    if (f == TypeKind::Ptr      && t == TypeKind::TypedPtr) return CastResult::Silent;

    // Any integer → float (silent widening)
    if (isInteger(f) && isFloat(t))          return CastResult::Silent;

    // f32 → f64 (silent float widening)
    if (f == TypeKind::F32 && t == TypeKind::F64) return CastResult::Silent;

    // f64 → f32 (warn — narrowing float, may lose precision)
    if (f == TypeKind::F64 && t == TypeKind::F32) return CastResult::Warn;

    // float → any integer (warn — floor/truncate toward −∞)
    if (isFloat(f) && isInteger(t))          return CastResult::Warn;

    // Signed integer widening: i8 → i16 → i32 → i64 (silent)
    // Signed integer narrowing: i32 → i8 etc. (warn — may lose data)
    if (isSignedInt(f) && isSignedInt(t)) {
        if (bitWidth(t) > bitWidth(f))  return CastResult::Silent;
        if (bitWidth(t) < bitWidth(f))  return CastResult::Warn;
    }

    // Unsigned integer widening: u8 → u16 → u32 → u64 (silent)
    // Unsigned integer narrowing: u32 → u8 etc. (warn — may lose data)
    if (isUnsignedInt(f) && isUnsignedInt(t)) {
        if (bitWidth(t) > bitWidth(f))  return CastResult::Silent;
        if (bitWidth(t) < bitWidth(f))  return CastResult::Warn;
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
// canPassArgument
// ============================================================

CastResult canPassArgument(const Type& from, const Type& to) {
    // Borrow a value object as a reference of the same class — address-of, no copy, no
    // refcount change. Argument position only (see the header note); refcount safety relies
    // on the callee treating the parameter as a pure borrow.
    if (from.kind == TypeKind::Object && to.kind == TypeKind::Reference
        && from.className == to.className)
        return CastResult::Silent;
    return canImplicitlyCast(from, to);
}

// ============================================================
// commonArithmeticType
// ============================================================

Type commonArithmeticType(const Type& a, const Type& b) {
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
        case TokenType::VOID:        return Type{TypeKind::Void};
        case TokenType::PTR:         return Type{TypeKind::Ptr};
        default:                     return Type{TypeKind::Error};
    }
}

// ============================================================
// typeName
// ============================================================

std::string typeName(const Type& t) {
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
        case TypeKind::Ptr:    return "ptr";
        case TypeKind::Array:  return typeName(Type{t.elementKind}) + "[" + std::to_string(t.arraySize) + "]";
        case TypeKind::Object: return t.className;
        case TypeKind::Enum:   return t.className;
        case TypeKind::Reference: return "Ref<" + t.className + ">";
        case TypeKind::TypedPtr: {
            Type elem = typedPtrElement(t);
            return "ptr<" + typeName(elem) + ">";
        }
        case TypeKind::Void:   return "void";
        case TypeKind::Error:  return "<error>";
    }
    return "<unknown>";
}

// ============================================================
// Overload name mangling — a symbol-safe, deterministic encoding of a type and of a
// full (name, params, return) signature. `$` separates components and `.` marks a
// reference/element; both are valid unquoted LLVM identifier characters.
// ============================================================

std::string mangleType(const Type& t) {
    switch (t.kind) {
        case TypeKind::Object:    return t.className;
        case TypeKind::Enum:      return t.className;
        case TypeKind::Reference: return t.className + ".ref";
        case TypeKind::Ptr:       return "ptr";
        case TypeKind::TypedPtr:  return "ptr." + mangleType(typedPtrElement(t));
        case TypeKind::Array:     return mangleType(Type{t.elementKind}) + ".arr" + std::to_string(t.arraySize);
        default:                  return typeName(t);   // primitives, void
    }
}

std::string mangleOverload(const std::string& base, const std::vector<Type>& params, const Type& ret) {
    std::string out = base;
    for (const Type& p : params) out += "$" + mangleType(p);
    out += "$ret$" + mangleType(ret);
    return out;
}

// ============================================================
// typeKindFromName — primitive keyword spelling → TypeKind
// ============================================================

TypeKind typeKindFromName(const std::string& name) {
    if (name == "i8")   return TypeKind::I8;
    if (name == "i16")  return TypeKind::I16;
    if (name == "i32")  return TypeKind::I32;
    if (name == "i64")  return TypeKind::I64;
    if (name == "u8")   return TypeKind::U8;
    if (name == "u16")  return TypeKind::U16;
    if (name == "u32")  return TypeKind::U32;
    if (name == "u64")  return TypeKind::U64;
    if (name == "f32")  return TypeKind::F32;
    if (name == "f64")  return TypeKind::F64;
    if (name == "bool") return TypeKind::Bool;
    if (name == "char") return TypeKind::Char;
    if (name == "ptr")  return TypeKind::Ptr;
    if (name == "void") return TypeKind::Void;
    return TypeKind::Error;
}

// ============================================================
// decodeSynthesizedType — parser-synthesized type token → Type
//
// Handles:
//   "Class&"       → Reference(Class)
//   "ptr<Elem>"    → TypedPtr whose element is described by Elem, where Elem is
//                    a primitive spelling, "Class" (Object), or "Class.ref"
//                    (Reference). Nested ptr elements decay to opaque Ptr.
// Returns Type{Error} when `tok` is not such a synthesized token.
// ============================================================

Type decodeSynthesizedType(const Token& tok) {
    const std::string& s = tok.lexeme;

    // Reference: "Class&"
    if (!s.empty() && s.back() == '&')
        return makeReferenceType(s.substr(0, s.size() - 1));

    // Typed pointer: "ptr<Elem>"
    if (s.size() > 5 && s.compare(0, 4, "ptr<") == 0 && s.back() == '>') {
        std::string elem = s.substr(4, s.size() - 5);

        // ptr<void> is an alias for the opaque ptr type.
        if (elem == "void") return Type{TypeKind::Ptr};

        // Reference element: "Elem.ref"
        if (elem.size() > 4 && elem.compare(elem.size() - 4, 4, ".ref") == 0) {
            std::string cls = elem.substr(0, elem.size() - 4);
            return makeTypedPtr(TypeKind::Reference, cls);
        }

        // Primitive element
        TypeKind pk = typeKindFromName(elem);
        if (pk != TypeKind::Error)
            return makeTypedPtr(pk);

        // Otherwise an object/class element
        return makeTypedPtr(TypeKind::Object, elem);
    }

    return Type{TypeKind::Error};
}
