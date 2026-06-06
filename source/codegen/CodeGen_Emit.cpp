#include "CodeGen.h"

// ---- Bounds check helpers ----

void CodeGen::ensureAbortDeclared() {
    static const std::string abortDecl = "declare void @abort()";
    for (const auto& declaration : module.declares)
        if (declaration == abortDecl) return;
    module.declares.push_back(abortDecl);
}

void CodeGen::emitBoundsCheck(const std::string& indexValue, size_t arraySize) {
    std::string sizeStr  = std::to_string(arraySize);
    std::string okLabel  = freshLabel("bounds.ok");
    std::string oobLabel = freshLabel("bounds.oob");

    std::string cmp = freshTemp();
    // icmp ult catches negative indices too: negative i64 values are huge unsigned numbers
    emit("%" + cmp + " = icmp ult i64 " + indexValue + ", " + sizeStr);
    emitCondBr("%" + cmp, okLabel, oobLabel);

    switchBlock(oobLabel);
    emit("call void @abort()");
    emit("unreachable");
    currentBasicBlock->terminated = true;

    switchBlock(okLabel);
}

// ============================================================
// Reference-counting runtime (emitted once, when `new` is used)
// ============================================================

void CodeGen::emitRefcountRuntime() {
    // Ensure malloc/free are declared, deduplicating against extern-imported decls.
    const std::string mallocDecl = "declare ptr @malloc(i64)";
    const std::string freeDecl   = "declare void @free(ptr)";
    bool haveMalloc = false, haveFree = false;
    for (const auto& d : module.declares) {
        if (d == mallocDecl) haveMalloc = true;
        if (d == freeDecl)   haveFree = true;
    }
    if (!haveMalloc) module.declares.push_back(mallocDecl);
    if (!haveFree)   module.declares.push_back(freeDecl);

    // Intrusive header layout: [ i64 refcount ][ object body ].
    // gg_alloc returns a pointer to the body; the count lives at body-8.
    module.runtime.push_back(
        "define ptr @gg_alloc(i64 %size) {\n"
        "entry:\n"
        "  %total = add i64 %size, 8\n"
        "  %raw = call ptr @malloc(i64 %total)\n"
        "  store i64 1, ptr %raw\n"
        "  %body = getelementptr i8, ptr %raw, i64 8\n"
        "  ret ptr %body\n"
        "}\n");

    // retain/release are null-safe so uninitialised/null reference slots are harmless.
    module.runtime.push_back(
        "define void @gg_retain(ptr %obj) {\n"
        "entry:\n"
        "  %isnull = icmp eq ptr %obj, null\n"
        "  br i1 %isnull, label %done, label %inc\n"
        "inc:\n"
        "  %hdr = getelementptr i8, ptr %obj, i64 -8\n"
        "  %cnt = load i64, ptr %hdr\n"
        "  %n = add i64 %cnt, 1\n"
        "  store i64 %n, ptr %hdr\n"
        "  br label %done\n"
        "done:\n"
        "  ret void\n"
        "}\n");

    module.runtime.push_back(
        "define void @gg_release(ptr %obj, ptr %dtor) {\n"
        "entry:\n"
        "  %isnull = icmp eq ptr %obj, null\n"
        "  br i1 %isnull, label %done, label %dec\n"
        "dec:\n"
        "  %hdr = getelementptr i8, ptr %obj, i64 -8\n"
        "  %cnt = load i64, ptr %hdr\n"
        "  %n = sub i64 %cnt, 1\n"
        "  store i64 %n, ptr %hdr\n"
        "  %zero = icmp eq i64 %n, 0\n"
        "  br i1 %zero, label %dealloc, label %done\n"
        "dealloc:\n"
        "  %hasdtor = icmp ne ptr %dtor, null\n"
        "  br i1 %hasdtor, label %calldtor, label %freeit\n"
        "calldtor:\n"
        "  call void %dtor(ptr %obj)\n"
        "  br label %freeit\n"
        "freeit:\n"
        "  call void @free(ptr %hdr)\n"
        "  br label %done\n"
        "done:\n"
        "  ret void\n"
        "}\n");
}

// ============================================================
// Low-level emit helpers
// ============================================================

void CodeGen::emit(const std::string& instruction) {
    if (currentBasicBlock && !currentBasicBlock->terminated)
        currentBasicBlock->instructions.push_back("  " + instruction);
}

void CodeGen::emitAlloca(const std::string& ptrName, const std::string& irType) {
    if (currentFunction)
        currentFunction->allocas.push_back("  " + ptrName + " = alloca " + irType);
}

void CodeGen::emitStore(const std::string& irType, const std::string& value, const std::string& ptr) {
    emit("store " + irType + " " + value + ", ptr " + ptr);
}

std::string CodeGen::emitLoad(const std::string& irType, const std::string& ptr) {
    std::string tempName = freshTemp();
    emit("%" + tempName + " = load " + irType + ", ptr " + ptr);
    return "%" + tempName;
}

void CodeGen::emitBr(const std::string& label) {
    if (currentBasicBlock && !currentBasicBlock->terminated) {
        emit("br label %" + label);
        currentBasicBlock->terminated = true;
    }
}

void CodeGen::emitCondBr(const std::string& cond,
                          const std::string& trueLabel,
                          const std::string& falseLabel) {
    if (currentBasicBlock && !currentBasicBlock->terminated) {
        emit("br i1 " + cond + ", label %" + trueLabel + ", label %" + falseLabel);
        currentBasicBlock->terminated = true;
    }
}

void CodeGen::switchBlock(const std::string& label) {
    if (currentFunction) {
        currentFunction->blocks.push_back(BasicBlock{label, {}, false});
        currentBasicBlock = &currentFunction->blocks.back();
    }
}

// ============================================================
// Value / type helpers
// ============================================================

std::string CodeGen::freshTemp() {
    return "t" + std::to_string(tempCounter++);
}

std::string CodeGen::freshLabel(const std::string& hint) {
    return hint + "." + std::to_string(++labelCounter);
}

Type CodeGen::exprType(const Expr& expression) const {
    if (!expression.node || !typeMap) return Type{TypeKind::Error};
    auto it = typeMap->find(expression.node.get());
    if (it == typeMap->end()) return Type{TypeKind::Error};
    return it->second;
}

Type CodeGen::resolveParamType(const ParamDecl& param) const {
    // Reference type: a synthesized "<Class>&" token from the parser.
    if (param.typeName.type == TokenType::IDENTIFIER && !param.typeName.lexeme.empty()
        && param.typeName.lexeme.back() == '&')
        return makeReferenceType(param.typeName.lexeme.substr(0, param.typeName.lexeme.size() - 1));
    if (param.typeName.type == TokenType::IDENTIFIER && cgClasses_.count(param.typeName.lexeme))
        return makeObjectType(param.typeName.lexeme);
    return typeFromToken(param.typeName.type);
}

Type CodeGen::resolveReturnType(const Token& typeToken) const {
    // "Class&" → reference return (lowers to ptr). Bare class returns (by value) are
    // unsupported and fall through to typeFromToken (Error); primitives/void resolve normally.
    if (typeToken.type == TokenType::IDENTIFIER && !typeToken.lexeme.empty()
        && typeToken.lexeme.back() == '&')
        return makeReferenceType(typeToken.lexeme.substr(0, typeToken.lexeme.size() - 1));
    return typeFromToken(typeToken.type);
}

// ============================================================
// Reference-return / temporary lifetime helpers
// ============================================================

bool CodeGen::producesPlusOne(const Expr& e) const {
    if (!e.node) return false;
    if (std::holds_alternative<NewExpr>(*e.node)) return true;   // `new` → +1
    if ((std::holds_alternative<CallExpr>(*e.node)
         || std::holds_alternative<MethodCallExpr>(*e.node))
        && exprType(e).kind == TypeKind::Reference)
        return true;                                              // reference-returning call → +1
    return false;
}

void CodeGen::claimTemp(const std::string& ptr) {
    // The just-produced +1 temporary is the most recently registered one.
    if (!pendingTemps_.empty() && pendingTemps_.back().ptr == ptr)
        pendingTemps_.pop_back();
}

void CodeGen::flushTempReleases() {
    for (const auto& t : pendingTemps_) {
        auto it = cgClasses_.find(t.className);
        std::string dtorArg = (it != cgClasses_.end() && it->second.needsDtor)
                            ? ("@" + t.className + "_dtor") : "null";
        emit("call void @gg_release(ptr " + t.ptr + ", ptr " + dtorArg + ")");
    }
    pendingTemps_.clear();
}

std::string CodeGen::emitCast(const std::string& value, const Type& from, const Type& to) {
    if (from == to) return value;
    if (isError(from) || isError(to)) return value;

    // If both types map to the same LLVM IR type (e.g. string ↔ ptr, i32 ↔ u32,
    // char ↔ u32) no cast instruction is needed — bits are already identical.
    if (irTypeName(from) == irTypeName(to)) return value;

    auto getBitWidth = [](TypeKind kind) -> int {
        switch (kind) {
            case TypeKind::I8:
            case TypeKind::U8:
                return 8;
            case TypeKind::I16:
            case TypeKind::U16:
                return 16;
            case TypeKind::I32:
            case TypeKind::U32:
            case TypeKind::Char:
                return 32;
            case TypeKind::I64:
            case TypeKind::U64:
                return 64;
            case TypeKind::F32:
                return 32;
            case TypeKind::F64:
                return 64;
            case TypeKind::Bool:
                return 1;
            default:
                return 32;
        }
    };

    std::string fromIrType = irTypeName(from);
    std::string toIrType   = irTypeName(to);
    std::string instruction;

    if (isInteger(from.kind) && isInteger(to.kind)) {
        int fromBits = getBitWidth(from.kind);
        int toBits   = getBitWidth(to.kind);
        if (toBits > fromBits)
            instruction = isUnsignedInt(from.kind) ? "zext" : "sext";
        else if (toBits < fromBits)
            instruction = "trunc";
        else
            return value;  // Same IR bit-width — just reinterpret; no instruction needed
    } else if (from.kind == TypeKind::Bool && isInteger(to.kind)) {
        instruction = "zext";
    } else if (isInteger(from.kind) && to.kind == TypeKind::Bool) {
        // Convert to i1 via icmp ne
        return emitToBool(value, from);
    } else if (isFloat(from.kind) && isFloat(to.kind)) {
        int fromBits = getBitWidth(from.kind);
        int toBits   = getBitWidth(to.kind);
        instruction  = (toBits > fromBits) ? "fpext" : "fptrunc";
    } else if (isInteger(from.kind) && isFloat(to.kind)) {
        instruction = isSignedInt(from.kind) ? "sitofp" : "uitofp";
    } else if (isFloat(from.kind) && isInteger(to.kind)) {
        instruction = isSignedInt(to.kind) ? "fptosi" : "fptoui";
    } else {
        return value;  // no known cast
    }

    std::string tempName = freshTemp();
    emit("%" + tempName + " = " + instruction + " " + fromIrType + " " + value + " to " + toIrType);
    return "%" + tempName;
}

std::string CodeGen::emitToBool(const std::string& value, const Type& valueType) {
    if (valueType.kind == TypeKind::Bool) return value;

    std::string irType   = irTypeName(valueType);
    std::string tempName = freshTemp();
    if (isFloat(valueType.kind))
        emit("%" + tempName + " = fcmp une " + irType + " " + value + ", 0.0");
    else
        emit("%" + tempName + " = icmp ne " + irType + " " + value + ", 0");
    return "%" + tempName;
}

// ============================================================
// Arithmetic / comparison instruction selection
// ============================================================

std::string CodeGen::arithInstr(TokenType operatorType, const Type& type) {
    bool isFloat  = ::isFloat(type.kind);
    bool isSigned = isSignedInt(type.kind);

    switch (operatorType) {
        case TokenType::PLUS:        return isFloat ? "fadd" : "add";
        case TokenType::MINUS:       return isFloat ? "fsub" : "sub";
        case TokenType::STAR:        return isFloat ? "fmul" : "mul";
        case TokenType::SLASH:       return isFloat ? "fdiv" : (isSigned ? "sdiv" : "udiv");
        case TokenType::PERCENT:     return isFloat ? "frem" : (isSigned ? "srem" : "urem");
        case TokenType::AMPERSAND:   return "and";
        case TokenType::PIPE:        return "or";
        case TokenType::CARET:       return "xor";
        case TokenType::SHIFT_LEFT:  return "shl";
        case TokenType::SHIFT_RIGHT: return isSigned ? "ashr" : "lshr";
        default:                     return "add";  // fallback
    }
}

std::string CodeGen::cmpInstr(TokenType operatorType, const Type& type) {
    bool isFloat  = ::isFloat(type.kind);
    bool isSigned = isSignedInt(type.kind) || type.kind == TypeKind::Bool;

    if (isFloat) {
        // ordered comparisons (quiet NaN → false)
        switch (operatorType) {
            case TokenType::EQUAL_EQUAL:   return "fcmp oeq";
            case TokenType::BANG_EQUAL:    return "fcmp one";
            case TokenType::LESS:          return "fcmp olt";
            case TokenType::LESS_EQUAL:    return "fcmp ole";
            case TokenType::GREATER:       return "fcmp ogt";
            case TokenType::GREATER_EQUAL: return "fcmp oge";
            default:                       return "fcmp oeq";
        }
    } else {
        switch (operatorType) {
            case TokenType::EQUAL_EQUAL:   return "icmp eq";
            case TokenType::BANG_EQUAL:    return "icmp ne";
            case TokenType::LESS:          return isSigned ? "icmp slt" : "icmp ult";
            case TokenType::LESS_EQUAL:    return isSigned ? "icmp sle" : "icmp ule";
            case TokenType::GREATER:       return isSigned ? "icmp sgt" : "icmp ugt";
            case TokenType::GREATER_EQUAL: return isSigned ? "icmp sge" : "icmp uge";
            default:                       return "icmp eq";
        }
    }
}

TokenType CodeGen::compoundBaseOp(TokenType operatorType) {
    switch (operatorType) {
        case TokenType::PLUS_EQUAL:      return TokenType::PLUS;
        case TokenType::MINUS_EQUAL:     return TokenType::MINUS;
        case TokenType::STAR_EQUAL:      return TokenType::STAR;
        case TokenType::SLASH_EQUAL:     return TokenType::SLASH;
        case TokenType::PERCENT_EQUAL:   return TokenType::PERCENT;
        case TokenType::AMPERSAND_EQUAL: return TokenType::AMPERSAND;
        case TokenType::PIPE_EQUAL:      return TokenType::PIPE;
        case TokenType::CARET_EQUAL:     return TokenType::CARET;
        default:                         return TokenType::PLUS;
    }
}
