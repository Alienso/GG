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

    // ---- Nullability ----
    // `null` → any nullable target.
    if (from.kind == TypeKind::Null)
        return to.isNullable ? CastResult::Silent : CastResult::None;
    // A nullable value never implicitly narrows to a non-nullable one (must use `!!` or a
    // smart-cast). `T?` identity was already handled by the `from == to` check above.
    if (from.isNullable && !to.isNullable)   return CastResult::None;
    // `T → T?` (implicit wrap) and `T? → U?` (compatible underlying): compare the non-null forms.
    if (to.isNullable)
        return canImplicitlyCast(stripNullable(from), stripNullable(to));

    TypeKind f = from.kind, t = to.kind;

    // `Shared<T>` (owning atomic handle): it only converts to the SAME `Shared<T>` (a retaining
    // copy — the exact-identity case already returned Silent above; this also blesses the `T → T?`
    // wrap after the nullable recursion). It never coerces to/from a plain `Class&`, a borrow, or a
    // value `Object` — that's the born-shared + no-leak rule (a shared object has no raw alias, and
    // you can't smuggle one out). Guarded here so the Reference→Object / value-borrow rules below
    // can't mis-fire on a shared handle (both are Reference-kind with the same className).
    if (from.shared || to.shared) {
        // Shared<T> -> Shared<T> (same class): a retaining copy.
        if (from.shared && to.shared && from.className == to.className) return CastResult::Silent;
        // Shared<T> -> a non-owning borrow `Class*` (same class): a safe view — no refcount, no
        // ownership extracted (escape analysis confines the borrow), exactly like `Class& -> Class*`.
        if (from.shared && !to.shared && t == TypeKind::Reference && to.borrow
            && from.className == to.className)
            return CastResult::Silent;
        // Shared<T> -> value `Object` (same class): deref + clone (an independent copy), like
        // `Class& -> Object`.
        if (from.shared && t == TypeKind::Object && from.className == to.className)
            return CastResult::Silent;
        // Everything else involving a Shared is forbidden: `-> Class&` (would create a NON-atomic
        // co-owner of an atomic object) and anything `-> Shared` (born-shared: no promoting an alias).
        return CastResult::None;
    }

    // Generic-body checking only: a type parameter `T` and a reference/value of that same
    // parameter (`T&` / `T`) are interchangeable — the value-vs-reference distinction is a
    // concrete-lowering detail not knowable abstractly (a bound method may return `Self` or
    // `Self&`). Gated on a TypeParam being involved, so it never affects concrete-typed code.
    if ((f == TypeKind::TypeParam || t == TypeKind::TypeParam)
        && from.className == to.className
        && (f == TypeKind::TypeParam || f == TypeKind::Reference || f == TypeKind::Object)
        && (t == TypeKind::TypeParam || t == TypeKind::Reference || t == TypeKind::Object))
        return CastResult::Silent;

    // char and u32 share the same underlying representation (char = Unicode code point)
    if ((f == TypeKind::Char && t == TypeKind::U32) ||
        (f == TypeKind::U32  && t == TypeKind::Char))
        return CastResult::Silent;

    // A `ref T` (non-owning borrow) is the target: an owning reference, a value object, or another
    // borrow of the same class may all be borrowed into it (address-of, no retain/release). The
    // reverse — widening a borrow to an owning `Class&` — is NOT allowed (it doesn't own the target),
    // and falls through to None below.
    if (t == TypeKind::Reference && to.borrow && !to.className.empty() && from.className == to.className
        && (f == TypeKind::Reference || f == TypeKind::Object))
        return CastResult::Silent;

    // Borrow of a PRIMITIVE (`ref i32`, an lvalue reference like C++'s `int&`):
    //   • into it — a primitive lvalue of the same kind may be borrowed (address-of; the binding
    //     site separately checks the source is addressable, since a temporary has no address);
    //   • out of it — `ref P` used where `P` is expected is a load (lvalue-to-rvalue deref), so
    //     `i32 y = arr.get(i)` and arithmetic on a borrowed element work transparently.
    if (t == TypeKind::Reference && to.borrow && to.className.empty() && f == to.elementKind)
        return CastResult::Silent;
    if (f == TypeKind::Reference && from.borrow && from.className.empty()
        && !to.borrow && from.elementKind == t)
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

    // `str` decays to a raw `ptr` (its NUL-terminated `.data` pointer) for FFI / CRT calls and
    // any `ptr` context. The reverse (ptr → str) is forbidden: a raw pointer has no known length.
    if (f == TypeKind::Str && t == TypeKind::Ptr)           return CastResult::Silent;

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
    // NOT for a Shared<T> target: born-shared forbids borrowing a stack value as a shared handle.
    if (from.kind == TypeKind::Object && to.kind == TypeKind::Reference && !to.shared
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

    // Mixed signed/unsigned — GG departs from C here: the SIGNED interpretation wins, so
    // `-6 / 3u == -2` rather than a huge unsigned result. The common type is a SIGNED integer of
    // the wider of the two operands' widths (the unsigned operand is reinterpreted as signed).
    switch (wa >= wb ? wa : wb) {
        case 8:  return Type{TypeKind::I8};
        case 16: return Type{TypeKind::I16};
        case 64: return Type{TypeKind::I64};
        default: return Type{TypeKind::I32};
    }
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
        case TokenType::STR:         return Type{TypeKind::Str};
        default:                     return Type{TypeKind::Error};
    }
}

// ============================================================
// typeName
// ============================================================

std::string typeName(const Type& t) {
    if (t.isNullable) return typeName(stripNullable(t)) + "?";
    switch (t.kind) {
        case TypeKind::Null:   return "null";
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
        case TypeKind::Str:    return "str";
        case TypeKind::Array:  return typeName(Type{t.elementKind}) + "[" + std::to_string(t.arraySize) + "]";
        case TypeKind::Object: return t.className;
        case TypeKind::Enum:   return t.className;
        case TypeKind::Reference:
            // A `Shared<Class>` handle renders as `Shared<Class>`; a non-owning borrow renders with a
            // postfix `*` (`i32*`, `Point*`); an owning heap reference renders with a postfix `&`.
            if (t.shared) return "Shared<" + t.className + ">";
            if (t.borrow) return (t.className.empty() ? typeName(Type{t.elementKind}) : t.className) + "*";
            return t.className + "&";
        case TypeKind::TypedPtr: {
            Type elem = typedPtrElement(t);
            return "ptr<" + typeName(elem) + ">";
        }
        case TypeKind::TypeParam: return t.className;   // the parameter name, e.g. "T"
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
    // Nullable types: mangle the underlying type + a `.opt` marker. `typeName` would render `i32?`,
    // but `?` is NOT a valid unquoted LLVM identifier character — it must never reach a symbol name.
    // `.` is valid (used for `.ref`/`.arr`), so `i32?` → `i32.opt`, `Class&?` → `Class.ref.opt`.
    if (t.isNullable) {
        Type base = t;
        base.isNullable = false;
        return mangleType(base) + ".opt";
    }
    switch (t.kind) {
        case TypeKind::Object:    return t.className;
        case TypeKind::Enum:      return t.className;
        case TypeKind::Reference:
            // A non-owning borrow (`Class*` / `i32*` / a bare-object param) must mangle DISTINCTLY
            // from an owning heap reference (`Class&`) — both are `ptr` in IR, so if they shared a
            // mangled suffix, overloading one against the other would emit two definitions with the
            // same symbol (a raw clang "invalid redefinition"). A PRIMITIVE borrow has an empty
            // className, so it must encode its element type too, or `i32*` and `f64*` would collide.
            if (t.shared)
                return t.className + ".shared";   // atomically-refcounted owning handle (distinct symbol)
            if (t.borrow)
                return (t.className.empty() ? typeName(Type{t.elementKind}) : t.className) + ".brw";
            return t.className + ".ref";   // owning heap reference
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

bool integerLiteralFits(unsigned long long magnitude, TypeKind t) {
    switch (t) {
        // Signed: allow up to 2^(bits-1) so the negated boundary value fits (e.g. -128 for i8).
        case TypeKind::I8:  return magnitude <= 128ULL;
        case TypeKind::I16: return magnitude <= 32768ULL;
        case TypeKind::I32: return magnitude <= 2147483648ULL;
        case TypeKind::I64: return magnitude <= 9223372036854775808ULL;
        // Unsigned: the full 2^bits - 1 range.
        case TypeKind::U8:  return magnitude <= 255ULL;
        case TypeKind::U16: return magnitude <= 65535ULL;
        case TypeKind::U32: return magnitude <= 4294967295ULL;
        case TypeKind::U64: return true;   // any unsigned long long fits u64
        // char is a 32-bit Unicode code point (unsigned) — same range as u32.
        case TypeKind::Char: return magnitude <= 4294967295ULL;
        default:            return false;
    }
}

std::string stripDigitSeparators(const std::string& lexeme) {
    std::string out;
    out.reserve(lexeme.size());
    for (char c : lexeme) if (c != '_') out += c;
    return out;
}

bool isPrefixedIntegerLiteral(const std::string& lexeme) {
    return lexeme.size() > 1 && lexeme[0] == '0'
        && (lexeme[1] == 'x' || lexeme[1] == 'X' || lexeme[1] == 'o' || lexeme[1] == 'O');
}

bool parseIntegerLiteral(const std::string& lexeme, unsigned long long& magnitude) {
    std::string clean = stripDigitSeparators(lexeme);
    int base = 10;
    size_t digitsStart = 0;
    if (clean.size() > 1 && clean[0] == '0' && (clean[1] == 'x' || clean[1] == 'X')) {
        base = 16;
        digitsStart = 2;
    } else if (clean.size() > 1 && clean[0] == '0' && (clean[1] == 'o' || clean[1] == 'O')) {
        base = 8;
        digitsStart = 2;
    }
    if (digitsStart >= clean.size()) return false;   // e.g. "0x" with no digits
    try {
        magnitude = std::stoull(clean.substr(digitsStart), nullptr, base);
        return true;
    } catch (...) {
        return false;
    }
}

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
    if (name == "str")  return TypeKind::Str;
    if (name == "void") return TypeKind::Void;
    return TypeKind::Error;
}

// ============================================================
// Sync-cell recognition — the stdlib Mutex<T> / RwLock<T> types
// ============================================================

// The simple name of a class: the last '.'-segment (drops a module prefix), with any '$…'
// monomorphization suffix stripped. `std.sync.Mutex$Counter` → "Mutex"; `Mutex$i32` → "Mutex".
static std::string simpleClassName(const std::string& className) {
    std::string base = className.substr(0, className.find('$'));
    auto dot = base.rfind('.');
    return dot == std::string::npos ? base : base.substr(dot + 1);
}
bool isMutexName(const std::string& className)   { return simpleClassName(className) == "Mutex"; }
bool isRwLockName(const std::string& className)  { return simpleClassName(className) == "RwLock"; }
bool isSyncCellName(const std::string& className) {
    std::string s = simpleClassName(className);
    return s == "Mutex" || s == "RwLock";
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

    // Nullable: a trailing `?` (`Class&?`, `ref:Class?`, `Color?`). Decode the inner type and mark
    // it nullable. A bare class/enum name inner isn't a *synthesized* token, so decoding returns
    // Error and the caller (resolveTypeToken / the codegen resolvers) applies nullability itself.
    if (!s.empty() && s.back() == '?') {
        Token inner{TokenType::IDENTIFIER, s.substr(0, s.size() - 1), tok.line};
        Type t = decodeSynthesizedType(inner);
        if (t.kind != TypeKind::Error) return makeNullable(t);
        return Type{TypeKind::Error};
    }

    // Borrow: "ref:T" — a non-owning reference (`ref T`). A primitive inner (`ref i32`) is an
    // lvalue reference to that primitive; anything else is a class borrow.
    if (s.size() > 4 && s.compare(0, 4, "ref:") == 0) {
        std::string inner = s.substr(4);
        TypeKind prim = typeKindFromName(inner);
        if (prim != TypeKind::Error && prim != TypeKind::Ptr)
            return makePrimitiveBorrow(prim);
        return makeBorrowType(inner);
    }

    // Shared handle: "shared:Class" — an owning, atomically-refcounted heap reference (born-shared).
    if (s.size() > 7 && s.compare(0, 7, "shared:") == 0)
        return makeSharedType(s.substr(7));

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

// ============================================================
// synthTypeToken — resolved Type → single type token (inverse of the resolvers)
// ============================================================

// Primitive TypeKind → its type-keyword TokenType (IDENTIFIER for non-primitives).
static TokenType primitiveTokenType(TypeKind k) {
    switch (k) {
        case TypeKind::I8:   return TokenType::I8;
        case TypeKind::I16:  return TokenType::I16;
        case TypeKind::I32:  return TokenType::I32;
        case TypeKind::I64:  return TokenType::I64;
        case TypeKind::U8:   return TokenType::U8;
        case TypeKind::U16:  return TokenType::U16;
        case TypeKind::U32:  return TokenType::U32;
        case TypeKind::U64:  return TokenType::U64;
        case TypeKind::F32:  return TokenType::F32;
        case TypeKind::F64:  return TokenType::F64;
        case TypeKind::Bool: return TokenType::BOOL;
        case TypeKind::Char: return TokenType::CHAR_TYPE;
        case TypeKind::Ptr:  return TokenType::PTR;
        case TypeKind::Str:  return TokenType::STR;
        case TypeKind::Void: return TokenType::VOID;
        default:             return TokenType::IDENTIFIER;
    }
}

Token synthTypeToken(const Type& t, int line) {
    // Nullable wrapper: encode the inner type, append '?'. A nullable reference/enum/primitive all
    // spell as "<inner>?" (which resolveTypeToken / the codegen resolvers strip back off).
    if (t.isNullable) {
        Token inner = synthTypeToken(stripNullable(t), line);
        return Token{ TokenType::IDENTIFIER, inner.lexeme + "?", line };
    }
    switch (t.kind) {
        case TypeKind::Object:
        case TypeKind::Enum:
        case TypeKind::TypeParam:
            // A plain class / enum / type-parameter name.
            return Token{ TokenType::IDENTIFIER, t.className, line };
        case TypeKind::Reference:
            if (t.shared)
                // Owning atomic handle → the internal "shared:<Class>" spelling.
                return Token{ TokenType::IDENTIFIER, "shared:" + t.className, line };
            if (t.borrow) {
                // Non-owning borrow → the internal "ref:<inner>" spelling.
                std::string inner = t.className.empty()
                    ? typeName(Type{t.elementKind})   // primitive borrow (`i32*` → "ref:i32")
                    : t.className;                     // class borrow  (`Point*` → "ref:Point")
                return Token{ TokenType::IDENTIFIER, "ref:" + inner, line };
            }
            return Token{ TokenType::IDENTIFIER, t.className + "&", line };   // owning heap reference
        case TypeKind::TypedPtr: {
            Type elem = typedPtrElement(t);
            std::string es;
            if      (elem.kind == TypeKind::Reference) es = elem.className + ".ref";
            else if (elem.kind == TypeKind::Object)    es = elem.className;
            else if (elem.kind == TypeKind::Ptr)       es = "void";
            else                                       es = typeName(elem);   // primitive element
            return Token{ TokenType::IDENTIFIER, "ptr<" + es + ">", line };
        }
        default:
            // Primitives / void / opaque ptr — the token *type* carries the meaning.
            return Token{ primitiveTokenType(t.kind), typeName(t), line };
    }
}
