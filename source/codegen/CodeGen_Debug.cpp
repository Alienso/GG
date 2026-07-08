//
// Debug-info emission (DWARF via LLVM metadata). All output here is gated by
// CompilerOptions::debugInfo (mirrored into CodeGen::debug_); with --debug off
// none of these helpers run and the emitted IR is byte-identical to before.
//
// Tier 1 (line-level): a !DICompileUnit / !DIFile, one !DISubprogram per user
// function attached to its `define`, and a !DILocation on every instruction
// (statement granularity, from the existing Token.line).
// Tier 2 (variables & types): !DIBasicType / !DICompositeType / pointer
// !DIDerivedType, a !DISubroutineType with real param/return types, and
// !DILocalVariable + #dbg_declare records for parameters and locals.
//

#include "CodeGen.h"
#include <filesystem>

// ------------------------------------------------------------
// Small helpers
// ------------------------------------------------------------

// Escape a string for an LLVM metadata string literal ("...").
static std::string dbgEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    return out;
}

// A stable cache key for a GG type (kind + class/element identity).
static std::string dbgTypeKey(const Type& t) {
    std::string k = std::to_string(static_cast<int>(t.kind));
    if (!t.className.empty()) k += ":" + t.className;
    k += "/" + std::to_string(static_cast<int>(t.elementKind));
    return k;
}

// Representative source line of an expression (leading token). 0 = unknown.
static int exprLine(const Expr& e) {
    if (!e.node) return 0;
    const auto& v = *e.node;
    if (auto* p = std::get_if<LiteralExpr>(&v))       return p->token.line;
    if (auto* p = std::get_if<IdentifierExpr>(&v))    return p->name.line;
    if (auto* p = std::get_if<UnaryExpr>(&v))         return p->operatorToken.line;
    if (auto* p = std::get_if<BinaryExpr>(&v))        return p->left ? exprLine(*p->left) : p->operatorToken.line;
    if (auto* p = std::get_if<AssignExpr>(&v))        return p->name.line;
    if (auto* p = std::get_if<CompoundAssignExpr>(&v))return p->name.line;
    if (auto* p = std::get_if<PostfixExpr>(&v))       return p->operand ? exprLine(*p->operand) : p->operatorToken.line;
    if (auto* p = std::get_if<CallExpr>(&v))          return p->callee.line;
    if (auto* p = std::get_if<VarDeclExpr>(&v))       return p->typeName.line;
    if (auto* p = std::get_if<IndexExpr>(&v))         return p->object ? exprLine(*p->object) : 0;
    if (auto* p = std::get_if<IndexAssignExpr>(&v))   return p->object ? exprLine(*p->object) : 0;
    if (auto* p = std::get_if<ThisExpr>(&v))          return p->keyword.line;
    if (auto* p = std::get_if<MemberAccessExpr>(&v))  return p->object ? exprLine(*p->object) : p->field.line;
    if (auto* p = std::get_if<MemberAssignExpr>(&v))  return p->object ? exprLine(*p->object) : p->field.line;
    if (auto* p = std::get_if<MethodCallExpr>(&v))    return p->object ? exprLine(*p->object) : p->method.line;
    if (auto* p = std::get_if<CastExpr>(&v))          return p->operand ? exprLine(*p->operand) : p->targetType.line;
    if (auto* p = std::get_if<NewExpr>(&v))           return p->keyword.line;
    if (auto* p = std::get_if<SizeofExpr>(&v))        return p->keyword.line;
    if (auto* p = std::get_if<SwitchExpr>(&v))        return p->keyword.line;
    return 0;
}

// Representative source line of a statement. 0 = no single line (e.g. a block).
static int stmtLine(const Stmt& s) {
    if (!s.node) return 0;
    const auto& v = *s.node;
    if (auto* p = std::get_if<ExprStmt>(&v))     return exprLine(p->expression);
    if (auto* p = std::get_if<ReturnStmt>(&v))   return p->keyword.line;
    if (auto* p = std::get_if<IfStmt>(&v))       return exprLine(p->condition);
    if (auto* p = std::get_if<WhileStmt>(&v))    return exprLine(p->condition);
    if (auto* p = std::get_if<ForStmt>(&v))      return p->init ? stmtLine(*p->init) : 0;
    if (auto* p = std::get_if<SwitchStmt>(&v))   return p->keyword.line;
    if (auto* p = std::get_if<YieldStmt>(&v))    return p->keyword.line;
    if (auto* p = std::get_if<BreakStmt>(&v))    return p->keyword.line;
    if (auto* p = std::get_if<ContinueStmt>(&v)) return p->keyword.line;
    return 0;
}

// ------------------------------------------------------------
// Metadata builders
// ------------------------------------------------------------

int CodeGen::dbgAdd(const std::string& body) {
    int id = dbgNextId_++;
    module.debugMeta.push_back("!" + std::to_string(id) + " = " + body);
    return id;
}

void CodeGen::dbgInit() {
    namespace fs = std::filesystem;
    std::string filename = "unknown.gg", directory = ".";
    if (!dbgSourceFile_.empty()) {
        try {
            fs::path p(dbgSourceFile_);
            filename  = p.filename().string();
            directory = p.parent_path().string();
        } catch (...) { filename = dbgSourceFile_; }
    }
    if (filename.empty())  filename  = "unknown.gg";
    if (directory.empty()) directory = ".";

    dbgFileId_ = dbgAdd("!DIFile(filename: \"" + dbgEscape(filename)
                        + "\", directory: \"" + dbgEscape(directory) + "\")");
    dbgCUId_   = dbgAdd("distinct !DICompileUnit(language: DW_LANG_C, file: !"
                        + std::to_string(dbgFileId_)
                        + ", producer: \"GG\", isOptimized: false, runtimeVersion: 0, "
                          "emissionKind: FullDebug)");
    int flags  = dbgAdd("!{i32 2, !\"Debug Info Version\", i32 3}");
    module.namedMeta.push_back("!llvm.dbg.cu = !{!" + std::to_string(dbgCUId_) + "}");
    module.namedMeta.push_back("!llvm.module.flags = !{!" + std::to_string(flags) + "}");
}

std::pair<long long, long long> CodeGen::dbgSizeAlign(const Type& t) {
    switch (t.kind) {
        case TypeKind::I8: case TypeKind::U8: case TypeKind::Bool:  return {1, 1};
        case TypeKind::I16: case TypeKind::U16:                     return {2, 2};
        case TypeKind::I32: case TypeKind::U32:
        case TypeKind::F32: case TypeKind::Char:                    return {4, 4};
        case TypeKind::I64: case TypeKind::U64: case TypeKind::F64: return {8, 8};
        case TypeKind::Ptr: case TypeKind::Reference: case TypeKind::Enum:
        case TypeKind::TypedPtr: case TypeKind::TypeParam:          return {8, 8};
        case TypeKind::Object: {
            auto it = cgClasses_.find(t.className);
            if (it == cgClasses_.end()) return {8, 8};
            long long off = 0, align = 1;
            for (const auto& f : it->second.fields) {
                auto [sz, al] = dbgSizeAlign(f.second);
                if (al > 0) off = ((off + al - 1) / al) * al;
                off += sz;
                if (al > align) align = al;
            }
            if (align < 1) align = 1;
            long long size = ((off + align - 1) / align) * align;
            return {size == 0 ? 1 : size, align};
        }
        default: return {4, 4};
    }
}

int CodeGen::dbgTypeOf(const Type& t) {
    if (t.kind == TypeKind::Void) return -1;   // DWARF `null` type

    std::string key = dbgTypeKey(t);
    auto cached = dbgTypeCache_.find(key);
    if (cached != dbgTypeCache_.end()) return cached->second;

    auto basic = [&](const char* name, int bits, const char* enc) {
        return dbgAdd(std::string("!DIBasicType(name: \"") + name + "\", size: "
                      + std::to_string(bits) + ", encoding: " + enc + ")");
    };

    int id;
    switch (t.kind) {
        case TypeKind::I8:   id = basic("i8",   8,  "DW_ATE_signed");   break;
        case TypeKind::I16:  id = basic("i16",  16, "DW_ATE_signed");   break;
        case TypeKind::I32:  id = basic("i32",  32, "DW_ATE_signed");   break;
        case TypeKind::I64:  id = basic("i64",  64, "DW_ATE_signed");   break;
        case TypeKind::U8:   id = basic("u8",   8,  "DW_ATE_unsigned"); break;
        case TypeKind::U16:  id = basic("u16",  16, "DW_ATE_unsigned"); break;
        case TypeKind::U32:  id = basic("u32",  32, "DW_ATE_unsigned"); break;
        case TypeKind::U64:  id = basic("u64",  64, "DW_ATE_unsigned"); break;
        case TypeKind::F32:  id = basic("f32",  32, "DW_ATE_float");    break;
        case TypeKind::F64:  id = basic("f64",  64, "DW_ATE_float");    break;
        case TypeKind::Bool: id = basic("bool", 8,  "DW_ATE_boolean");  break;
        case TypeKind::Char: id = basic("char", 32, "DW_ATE_signed");   break;

        // Pointer-shaped types: reference/enum/ptr become an opaque pointer (address only).
        case TypeKind::Reference: case TypeKind::Enum:
        case TypeKind::Ptr: case TypeKind::TypedPtr: case TypeKind::TypeParam:
            id = dbgAdd("!DIDerivedType(tag: DW_TAG_pointer_type, baseType: null, size: 64)");
            break;

        case TypeKind::Object: {
            auto it = cgClasses_.find(t.className);
            if (it == cgClasses_.end()) {
                id = dbgAdd("!DIDerivedType(tag: DW_TAG_pointer_type, baseType: null, size: 64)");
                break;
            }
            // Reserve the composite's id first so a field that (indirectly) refers back
            // resolves through the cache instead of recursing forever.
            id = dbgNextId_++;
            dbgTypeCache_[key] = id;

            auto [totalSize, totalAlign] = dbgSizeAlign(t);
            long long off = 0;
            std::string memberIds;
            for (const auto& f : it->second.fields) {
                auto [sz, al] = dbgSizeAlign(f.second);
                if (al > 0) off = ((off + al - 1) / al) * al;
                int base = dbgTypeOf(f.second);
                int mem = dbgAdd("!DIDerivedType(tag: DW_TAG_member, name: \"" + dbgEscape(f.first)
                                 + "\", file: !" + std::to_string(dbgFileId_)
                                 + ", baseType: !" + std::to_string(base < 0 ? id : base)
                                 + ", size: " + std::to_string(sz * 8)
                                 + ", align: " + std::to_string(al * 8)
                                 + ", offset: " + std::to_string(off * 8) + ")");
                if (!memberIds.empty()) memberIds += ", ";
                memberIds += "!" + std::to_string(mem);
                off += sz;
            }
            int elems = dbgAdd("!{" + memberIds + "}");
            module.debugMeta.push_back(
                "!" + std::to_string(id) + " = !DICompositeType(tag: DW_TAG_structure_type, name: \""
                + dbgEscape(t.className) + "\", file: !" + std::to_string(dbgFileId_)
                + ", size: " + std::to_string(totalSize * 8)
                + ", align: " + std::to_string(totalAlign * 8)
                + ", elements: !" + std::to_string(elems) + ")");
            return id;   // already cached
        }

        default:
            id = basic("i32", 32, "DW_ATE_signed");
            break;
    }
    dbgTypeCache_[key] = id;
    return id;
}

// ------------------------------------------------------------
// Per-function scope
// ------------------------------------------------------------

void CodeGen::dbgBeginFunction(const std::string& prettyName, const std::string& linkageName,
                               int line, const std::vector<Type>& paramTypes,
                               const Type& returnType, bool hasThis, const std::string& thisClass) {
    if (!debug_ || !currentFunction) return;
    dbgLineCache_.clear();
    if (line <= 0) line = 1;

    std::string types;
    auto addType = [&](int id) {
        if (!types.empty()) types += ", ";
        types += (id < 0 ? "null" : "!" + std::to_string(id));
    };
    addType(dbgTypeOf(returnType));                      // element 0 = return type
    if (hasThis) {
        Type self{TypeKind::Reference}; self.className = thisClass;
        addType(dbgTypeOf(self));                        // implicit `this`
    }
    for (const auto& pt : paramTypes) addType(dbgTypeOf(pt));

    int typesTuple = dbgAdd("!{" + types + "}");
    int subrType   = dbgAdd("!DISubroutineType(types: !" + std::to_string(typesTuple) + ")");
    int sp = dbgAdd("distinct !DISubprogram(name: \"" + dbgEscape(prettyName)
                    + "\", linkageName: \"" + dbgEscape(linkageName)
                    + "\", scope: !" + std::to_string(dbgFileId_)
                    + ", file: !"  + std::to_string(dbgFileId_)
                    + ", line: "   + std::to_string(line)
                    + ", type: !"  + std::to_string(subrType)
                    + ", scopeLine: " + std::to_string(line)
                    + ", unit: !"  + std::to_string(dbgCUId_)
                    + ", spFlags: DISPFlagDefinition)");
    currentSubprogram_ = sp;
    currentFunction->dbg = " !dbg !" + std::to_string(sp);
    dbgSetLine(line);
}

void CodeGen::dbgEndFunction() {
    currentSubprogram_ = -1;
    currentDbgLoc_.clear();
    dbgLineCache_.clear();
}

void CodeGen::dbgSetLine(int line) {
    if (!debug_ || currentSubprogram_ < 0) return;
    if (line <= 0) line = 1;
    auto it = dbgLineCache_.find(line);
    int loc;
    if (it != dbgLineCache_.end()) {
        loc = it->second;
    } else {
        loc = dbgAdd("!DILocation(line: " + std::to_string(line)
                     + ", column: 1, scope: !" + std::to_string(currentSubprogram_) + ")");
        dbgLineCache_[line] = loc;
    }
    currentDbgLoc_ = ", !dbg !" + std::to_string(loc);
}

void CodeGen::dbgStmtLine(const Stmt& stmt) {
    if (!debug_ || currentSubprogram_ < 0) return;
    int line = stmtLine(stmt);
    if (line > 0) dbgSetLine(line);
}

void CodeGen::dbgDeclare(const std::string& allocaPtr, const std::string& name,
                         const Type& t, int line, int argIndex) {
    if (!debug_ || currentSubprogram_ < 0) return;
    if (!currentBasicBlock || currentBasicBlock->terminated) return;
    int ty = dbgTypeOf(t);
    if (ty < 0) return;   // void — nothing to describe
    if (line <= 0) line = 1;

    std::string argPart = argIndex > 0 ? (", arg: " + std::to_string(argIndex)) : "";
    int var = dbgAdd("!DILocalVariable(name: \"" + dbgEscape(name) + "\"" + argPart
                     + ", scope: !" + std::to_string(currentSubprogram_)
                     + ", file: !"  + std::to_string(dbgFileId_)
                     + ", line: "   + std::to_string(line)
                     + ", type: !"  + std::to_string(ty) + ")");

    // A DILocation whose scope is this subprogram (required by #dbg_declare).
    int loc;
    auto it = dbgLineCache_.find(line);
    if (it != dbgLineCache_.end()) {
        loc = it->second;
    } else {
        loc = dbgAdd("!DILocation(line: " + std::to_string(line)
                     + ", column: 1, scope: !" + std::to_string(currentSubprogram_) + ")");
        dbgLineCache_[line] = loc;
    }

    currentBasicBlock->instructions.push_back(
        "    #dbg_declare(ptr " + allocaPtr + ", !" + std::to_string(var)
        + ", !DIExpression(), !" + std::to_string(loc) + ")");
}
