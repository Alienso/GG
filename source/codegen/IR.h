//
// Created by Vladimir Arsenijevic on 4.6.2026.
//

#ifndef GG_IR_H
#define GG_IR_H

#include <string>
#include <vector>
#include "../semantic/Type.h"

// ============================================================
// Type → LLVM IR type string
// ============================================================

inline std::string irTypeName(const Type& t) {
    switch (t.kind) {
        case TypeKind::I8:     return "i8";
        case TypeKind::I16:    return "i16";
        case TypeKind::I32:    return "i32";
        case TypeKind::I64:    return "i64";
        case TypeKind::U8:     return "i8";
        case TypeKind::U16:    return "i16";
        case TypeKind::U32:    return "i32";
        case TypeKind::U64:    return "i64";
        case TypeKind::F32:    return "float";
        case TypeKind::F64:    return "double";
        case TypeKind::Bool:   return "i1";
        case TypeKind::Char:   return "i32";
        case TypeKind::Ptr:    return "ptr";
        case TypeKind::Array:  return "[" + std::to_string(t.arraySize) + " x " + irTypeName(Type{t.elementKind}) + "]";
        case TypeKind::Object: return "%" + t.className;
        case TypeKind::Reference: return "ptr";   // refcounted heap reference — an opaque pointer to the object body
        case TypeKind::TypedPtr:  return "ptr";   // typed raw pointer ptr<T> — an opaque pointer at the IR level
        case TypeKind::Void:   return "void";
        case TypeKind::Error:  return "i32";   // fallback — suppressed errors
    }
    return "i32";
}

// ============================================================
// IR structures
// ============================================================

struct BasicBlock {
    std::string              label;         // e.g. "entry", "if.then.1"
    std::vector<std::string> instructions;  // one pre-formatted IR line each
    bool                     terminated = false;  // true once br or ret is emitted
};

struct IRFunction {
    std::string              signature;  // "define i32 @add(i32 %a, i32 %b)"
    std::vector<std::string> allocas;   // alloca lines — prepended to entry block
    std::vector<BasicBlock>  blocks;
};

struct IRModule {
    std::vector<std::string> typeDecls;  // %ClassName = type { ... } — struct type definitions
    std::vector<std::string> declares;   // declare <retType> @<name>(<params>) — extern functions
    std::vector<std::string> globals;    // @.str.N = private unnamed_addr constant …
    std::vector<IRFunction>  functions;
    std::vector<std::string> runtime;    // verbatim runtime helper definitions (e.g. gg_alloc/retain/release)
};

#endif