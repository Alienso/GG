#include "CodeGen.h"
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>

// ============================================================
// Shared helpers
// ============================================================

std::string CodeGen::freshAllocaName(const std::string& varName) {
    std::string ptrName = "%" + varName + ".addr";
    if (usedAllocaNames.count(ptrName)) {
        int suffix = 1;
        while (usedAllocaNames.count(ptrName + "." + std::to_string(suffix)))
            ++suffix;
        ptrName += "." + std::to_string(suffix);
    }
    usedAllocaNames.insert(ptrName);
    return ptrName;
}

std::string CodeGen::buildArgString(const std::vector<std::unique_ptr<Expr>>& args,
                                     const std::vector<Type>* declaredParamTypes,
                                     const std::vector<const Expr*>* defaults,
                                     const std::vector<int>* order) {
    auto paramTypeAt = [&](size_t slot) -> const Type* {
        return (declaredParamTypes && slot < declaredParamTypes->size())
                   ? &(*declaredParamTypes)[slot] : nullptr;
    };
    // Emit one argument expression as the IR operand "<irtype> <value>", cast to the parameter
    // type it fills (`slot`). A primitive-borrow parameter takes the argument's ADDRESS.
    auto emitOne = [&](const Expr& arg, size_t slot) -> std::string {
        Type argType = exprType(arg);
        const Type* pt = paramTypeAt(slot);
        if (pt && isPrimitiveBorrow(*pt) && !isBorrow(argType))
            return "ptr " + genBorrowSource(arg);
        std::string value = genExpr(arg);
        if (pt) { value = emitCast(value, argType, *pt); argType = *pt; }
        return paramIrType(argType) + " " + value;   // objects pass by address (value is a ptr)
    };

    std::string out;
    bool first = true;
    auto append = [&](const std::string& s) { if (!first) out += ", "; first = false; out += s; };

    if (order) {
        // Named-argument call: evaluate the WRITTEN arguments in source order (so side effects
        // occur in the order the programmer wrote them), then assemble them in parameter order.
        const size_t total = order->size();
        std::vector<int> slotOfWritten(args.size(), -1);
        for (size_t i = 0; i < total; ++i)
            if ((*order)[i] >= 0 && size_t((*order)[i]) < args.size()) slotOfWritten[(*order)[i]] = int(i);
        std::vector<std::string> written(args.size());
        for (size_t k = 0; k < args.size(); ++k)
            written[k] = emitOne(*args[k], slotOfWritten[k] >= 0 ? size_t(slotOfWritten[k]) : k);
        for (size_t i = 0; i < total; ++i) {
            if ((*order)[i] >= 0)                      append(written[(*order)[i]]);
            else if (defaults && i < defaults->size() && (*defaults)[i]) append(emitOne(*(*defaults)[i], i));
        }
        return out;
    }

    // Positional call: arguments in written order, then any omitted trailing params from defaults.
    size_t slot = 0;
    for (const auto& arg : args) append(emitOne(*arg, slot++));
    if (defaults && declaredParamTypes)
        for (size_t i = args.size(); i < declaredParamTypes->size(); ++i)
            if (i < defaults->size() && (*defaults)[i]) append(emitOne(*(*defaults)[i], i));
    return out;
}

const std::vector<const Expr*>* CodeGen::defaultsFor(const std::string& emittedName) const {
    auto it = funcDefaults_.find(emittedName);
    return it != funcDefaults_.end() ? &it->second : nullptr;
}

// `x!!` — non-null assertion. Reference/enum nullables share the `ptr` representation, so this is
// a machine-null check that aborts on failure; the value passes through unchanged when non-null.
std::string CodeGen::genUnwrap(const UnwrapExpr& unwrap) {
    Type t = exprType(*unwrap.operand);
    std::string v = genExpr(*unwrap.operand);
    if (!t.isNullable) return v;   // already non-null (e.g. smart-cast) — the assertion is a no-op
    bool optPrim = t.isNullable && (isNumeric(t.kind) || t.kind == TypeKind::Bool || t.kind == TypeKind::Char);
    ensureAbortDeclared();
    std::string cmp        = freshTemp();
    std::string okLabel    = freshLabel("nn.ok");
    std::string panicLabel = freshLabel("nn.null");
    if (optPrim) {
        std::string tag = freshTemp();
        emit("%" + tag + " = extractvalue " + irTypeName(t) + " " + v + ", 0");
        emit("%" + cmp + " = icmp eq i1 %" + tag + ", false");   // absent?
    } else {
        emit("%" + cmp + " = icmp eq ptr " + v + ", null");
    }
    emitCondBr("%" + cmp, panicLabel, okLabel);
    switchBlock(panicLabel);
    emit("call void @abort()");
    emit("unreachable");
    currentBasicBlock->terminated = true;
    switchBlock(okLabel);
    if (optPrim) {                                   // extract the payload (proven present)
        std::string p = freshTemp();
        emit("%" + p + " = extractvalue " + irTypeName(t) + " " + v + ", 1");
        return "%" + p;
    }
    return v;   // reference: same representation, now known non-null
}

// `a ?: b` — Elvis. For reference/enum nullables (all `ptr`), select `a` when non-null else `b`,
// via a result slot. (Refcount adjustment for owning-reference operands is deferred; the common
// case is a nullable field/borrow with a plain default.)
std::string CodeGen::genElvis(const ElvisExpr& elvis, const Type& resolvedType) {
    // A non-null left (e.g. smart-cast) never uses the default — just yield it.
    if (!exprType(*elvis.left).isNullable)
        return emitCast(genExpr(*elvis.left), exprType(*elvis.left), resolvedType);
    std::string resultIr = irTypeName(resolvedType);
    std::string slot = "%" + freshTemp();
    emitAlloca(slot, resultIr);

    Type lt = exprType(*elvis.left);
    bool optPrim = lt.isNullable && (isNumeric(lt.kind) || lt.kind == TypeKind::Bool || lt.kind == TypeKind::Char);
    std::string lv = genExpr(*elvis.left);
    std::string cmp      = freshTemp();
    std::string useLeft  = freshLabel("elvis.left");
    std::string useRight = freshLabel("elvis.right");
    std::string merge    = freshLabel("elvis.merge");
    if (optPrim) {
        std::string tag = freshTemp();
        emit("%" + tag + " = extractvalue " + irTypeName(lt) + " " + lv + ", 0");
        emit("%" + cmp + " = icmp ne i1 %" + tag + ", false");   // present?
    } else {
        emit("%" + cmp + " = icmp ne ptr " + lv + ", null");
    }
    emitCondBr("%" + cmp, useLeft, useRight);

    switchBlock(useLeft);
    emitStore(resultIr, emitCast(lv, lt, resolvedType), slot);   // unwrap opt-prim / pass ref through
    emitBr(merge);

    switchBlock(useRight);
    Type rt = exprType(*elvis.right);
    std::string rv = emitCast(genExpr(*elvis.right), rt, resolvedType);
    emitStore(resultIr, rv, slot);
    emitBr(merge);

    switchBlock(merge);
    return emitLoad(resultIr, slot);
}

const std::vector<int>* CodeGen::orderFor(const void* node) const {
    if (!callArgOrder_) return nullptr;
    auto it = callArgOrder_->find(node);
    return it != callArgOrder_->end() ? &it->second : nullptr;
}

// ============================================================
// Expression codegen
// ============================================================

std::string CodeGen::genExpr(const Expr& expr) {
    Type resolvedType = exprType(expr);
    return std::visit(overloaded{
        [&](const LiteralExpr& literal)              -> std::string { return genLiteral(literal, resolvedType); },
        [&](const IdentifierExpr& identifier)        -> std::string { return genIdentifier(identifier, resolvedType); },
        [&](const UnaryExpr& unary)                  -> std::string { return genUnary(unary, resolvedType); },
        [&](const BinaryExpr& binary)                -> std::string { return genBinary(binary, resolvedType); },
        [&](const AssignExpr& assign)                -> std::string { return genAssign(assign); },
        [&](const CompoundAssignExpr& compoundAssign)-> std::string { return genCompoundAssign(compoundAssign); },
        [&](const PostfixExpr& postfix)              -> std::string { return genPostfix(postfix); },
        [&](const CallExpr& call)                    -> std::string { return genCall(call, resolvedType); },
        [&](const VarDeclExpr& varDecl)              -> std::string { return genVarDecl(varDecl); },
        [&](const IndexExpr& indexExpr)              -> std::string { return genIndex(indexExpr); },
        [&](const IndexAssignExpr& indexAssign)      -> std::string { return genIndexAssign(indexAssign); },
        [&](const ThisExpr& thisExpr)                -> std::string { return genThis(thisExpr); },
        [&](const MemberAccessExpr& memberAccess)    -> std::string { return genMemberAccess(memberAccess, resolvedType); },
        [&](const MemberAssignExpr& memberAssign)    -> std::string { return genMemberAssign(memberAssign); },
        [&](const RefStoreExpr& refStore)            -> std::string { return genRefStore(refStore); },
        [&](const BraceInitExpr& braceInit)          -> std::string { return genBraceInit(braceInit); },
        [&](const MethodCallExpr& methodCall)        -> std::string { return genMethodCall(methodCall, resolvedType); },
        [&](const CastExpr& castExpr)                -> std::string { return genCast(castExpr, resolvedType); },
        [&](const NewExpr& newExpr)                  -> std::string { return genNew(newExpr, resolvedType); },
        [&](const SizeofExpr& sizeofExpr)            -> std::string { return genSizeof(sizeofExpr); },
        [&](const ReflectExpr& reflect)              -> std::string { return genReflect(reflect); },
        [&](const SwitchExpr& switchExpr)            -> std::string { return genSwitchExpr(switchExpr, resolvedType); },
        [&](const NullLiteralExpr&)                  -> std::string { return "null"; },
        [&](const UnwrapExpr& unwrap)                -> std::string { return genUnwrap(unwrap); },
        [&](const ElvisExpr& elvis)                  -> std::string { return genElvis(elvis, resolvedType); },
    }, *expr.node);
}

// ---- Literal ----

std::string CodeGen::genLiteral(const LiteralExpr& literal, const Type& resolvedType) {
    const std::string& lexeme = literal.token.lexeme;

    switch (literal.token.type) {
        case TokenType::NUMBER: {
            bool isDecimal = lexeme.find('.') != std::string::npos
                          || lexeme.find('e') != std::string::npos
                          || lexeme.find('E') != std::string::npos;

            // f32 target: emit an LLVM hex-float constant (the double encoding of the value rounded
            // to float). LLVM rejects a plain decimal that isn't exactly representable as `float`,
            // and a direct f32 literal (`f32 f = 0.1;`) is now possible via literal-type adoption —
            // previously f32 was only ever reached by fptrunc from an f64 constant.
            if (resolvedType.kind == TypeKind::F32) {
                float    fv = std::strtof(lexeme.c_str(), nullptr);
                double   dv = static_cast<double>(fv);
                uint64_t bits;
                std::memcpy(&bits, &dv, sizeof(bits));
                char buf[24];
                std::snprintf(buf, sizeof(buf), "0x%016llX",
                              static_cast<unsigned long long>(bits));
                return std::string(buf);
            }

            // f64 target (or a decimal literal with no numeric context) → a `double` constant.
            if (resolvedType.kind == TypeKind::F64 || isDecimal) {
                std::string value = lexeme;
                if (!value.empty() && value.back() == '.') value += '0';   // "1." → "1.0"
                if (value.find('.') == std::string::npos && !isDecimal)
                    value += ".0";   // integer lexeme in a double slot → a float constant
                return value;
            }

            // Integer target — emit the value masked to the target width, so the constant is
            // always a valid `iN` literal even when the source literal is out of range (semantic
            // has already warned in that case). Masking an in-range value is a no-op.
            int bits;
            switch (resolvedType.kind) {
                case TypeKind::I8:  case TypeKind::U8:  bits = 8;  break;
                case TypeKind::I16: case TypeKind::U16: bits = 16; break;
                case TypeKind::I64: case TypeKind::U64: bits = 64; break;
                default:                                bits = 32; break;   // i32/u32/no-context
            }
            unsigned long long v = 0;
            try { v = std::stoull(lexeme); } catch (...) { v = 0; }   // > u64 → 0 (warned garbage)
            if (bits < 64) v &= ((1ULL << bits) - 1);
            return std::to_string(v);
        }
        case TokenType::TRUE:  return "1";
        case TokenType::FALSE: return "0";

        case TokenType::CHAR: {
            // The lexer stores the char lexeme WITHOUT the surrounding single quotes.
            // char is a 32-bit Unicode code point (u32).
            if (lexeme.empty()) return "0";

            // ---- Escape sequences ----
            if (lexeme[0] == '\\' && lexeme.size() >= 2) {
                switch (lexeme[1]) {
                    case 'n':  return "10";
                    case 't':  return "9";
                    case 'r':  return "13";
                    case '\\': return "92";
                    case '\'': return "39";
                    case '"':  return "34";
                    case '0':  return "0";
                    default:   return std::to_string(static_cast<uint32_t>(
                                    static_cast<unsigned char>(lexeme[1])));
                }
            }

            // ---- UTF-8 → Unicode code point decoding ----
            const auto* bytes = reinterpret_cast<const unsigned char*>(lexeme.data());
            const size_t  len = lexeme.size();
            uint32_t cp = 0;

            if (len >= 1 && (bytes[0] & 0x80) == 0x00) {
                // 1-byte: 0xxxxxxx
                cp = bytes[0];
            } else if (len >= 2 && (bytes[0] & 0xE0) == 0xC0) {
                // 2-byte: 110xxxxx 10xxxxxx
                cp = static_cast<uint32_t>((bytes[0] & 0x1F) << 6)
                   | static_cast<uint32_t>( bytes[1] & 0x3F);
            } else if (len >= 3 && (bytes[0] & 0xF0) == 0xE0) {
                // 3-byte: 1110xxxx 10xxxxxx 10xxxxxx
                cp = static_cast<uint32_t>((bytes[0] & 0x0F) << 12)
                   | static_cast<uint32_t>((bytes[1] & 0x3F) <<  6)
                   | static_cast<uint32_t>( bytes[2] & 0x3F);
            } else if (len >= 4 && (bytes[0] & 0xF8) == 0xF0) {
                // 4-byte: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
                cp = static_cast<uint32_t>((bytes[0] & 0x07) << 18)
                   | static_cast<uint32_t>((bytes[1] & 0x3F) << 12)
                   | static_cast<uint32_t>((bytes[2] & 0x3F) <<  6)
                   | static_cast<uint32_t>( bytes[3] & 0x3F);
            } else {
                cp = bytes[0];  // invalid UTF-8 — use first byte as-is
            }

            return std::to_string(cp);
        }

        case TokenType::STRING: {
            // The lexer stores the string lexeme WITHOUT the surrounding double quotes.
            // Convert GG escape sequences to LLVM hex escape sequences.
            std::string content;
            int byteCount = 0;
            for (size_t i = 0; i < lexeme.size(); ++i) {
                if (lexeme[i] == '\\' && i + 1 < lexeme.size()) {
                    char escaped = lexeme[++i];
                    switch (escaped) {
                        case 'n':  content += "\\0A"; break;
                        case 't':  content += "\\09"; break;
                        case '\\': content += "\\5C"; break;
                        case '"':  content += "\\22"; break;
                        case '0':  content += "\\00"; break;
                        default:   content += lexeme[i]; break;
                    }
                } else {
                    content += lexeme[i];
                }
                ++byteCount;
            }
            int totalBytes = byteCount + 1;  // +1 for null terminator

            std::string globalName = "@.str." + std::to_string(stringCounter++);
            module.globals.push_back(
                globalName + " = private unnamed_addr constant ["
                + std::to_string(totalBytes) + " x i8] c\""
                + content + "\\00\", align 1");

            std::string tempName = freshTemp();
            emit("%" + tempName + " = getelementptr inbounds ["
                + std::to_string(totalBytes) + " x i8], ptr "
                + globalName + ", i32 0, i32 0");
            return "%" + tempName;
        }

        default:
            (void)resolvedType;
            return "0";
    }
}

// ---- Identifier ----

// Load an implicit-`this` member (`name` without an explicit `this.`). Static field →
// load the global; instance field → GEP on `this` + load. Returns "" if not a member.
std::string CodeGen::genImplicitFieldLoad(const std::string& name) {
    if (const Type* sft = findStaticField(currentClassName_, name))
        return emitLoad(irTypeName(*sft), "@" + currentClassName_ + "$" + name);
    Type fieldType;
    std::string gep = genImplicitFieldPtr(name, fieldType);
    if (gep.empty()) return "";
    // An embedded value-object field's value is its address (like an explicit `this.field`).
    if (fieldType.kind == TypeKind::Object) return gep;
    return emitLoad(irTypeName(fieldType), gep);
}

std::string CodeGen::genImplicitFieldPtr(const std::string& name, Type& fieldTypeOut) {
    auto thisIt = allocaMap.find("this");
    if (thisIt == allocaMap.end() || currentClassName_.empty()) return "";
    auto [gep, ft] = resolveFieldGEP(thisIt->second, currentClassName_, name);
    if (ft.kind == TypeKind::Error) return "";
    fieldTypeOut = ft;
    return gep;
}

std::string CodeGen::genIdentifier(const IdentifierExpr& identifier, const Type& resolvedType) {
    auto allocaIt  = allocaMap.find(identifier.name.lexeme);
    if (allocaIt == allocaMap.end()) {
        // Implicit `this`: bare reference to a field of the enclosing class.
        std::string loaded = genImplicitFieldLoad(identifier.name.lexeme);
        return loaded.empty() ? "0" : loaded;
    }

    auto varTypeIt = varTypeMap.find(identifier.name.lexeme);
    if (varTypeIt == varTypeMap.end()) return "0";

    const Type& varType = varTypeIt->second;

    // Object types: the alloca IS the struct pointer — return it directly (no load)
    if (varType.kind == TypeKind::Object) {
        return allocaIt->second;
    }

    // `ref <primitive>` read: the alloca holds the referent pointer; load it, then load the value
    // through it (lvalue-to-rvalue deref, like C++ using `int& r` as an `int`).
    if (isPrimitiveBorrow(varType)) {
        std::string referent = emitLoad("ptr", allocaIt->second);
        return emitLoad(irTypeName(borrowElementType(varType)), referent);
    }

    // Nullable primitive (`i32?`) storage is a `{ i1, iN }` value. Load it; if the use has been
    // smart-cast to the non-null primitive (`resolvedType` no longer nullable), extract the payload
    // (proven present by the narrowing — no runtime check).
    bool storageOptPrim = varType.isNullable
        && (isNumeric(varType.kind) || varType.kind == TypeKind::Bool || varType.kind == TypeKind::Char);
    if (storageOptPrim) {
        std::string structVal = emitLoad(irTypeName(varType), allocaIt->second);
        if (!resolvedType.isNullable) {
            std::string p = freshTemp();
            emit("%" + p + " = extractvalue " + irTypeName(varType) + " " + structVal + ", 1");
            return "%" + p;
        }
        return structVal;
    }

    std::string irType  = irTypeName(varType);
    std::string ptrName = allocaIt->second;
    return emitLoad(irType, ptrName);
}

// ---- Unary ----

std::string CodeGen::genUnary(const UnaryExpr& unary, const Type& resolvedType) {
    switch (unary.operatorToken.type) {
        case TokenType::MINUS: {
            Type operandType = exprType(*unary.operand);
            // Operator overloading: unary '-' on a class → the Neg trait's `neg` method.
            if (operandType.kind == TypeKind::Object || operandType.kind == TypeKind::Reference) {
                std::string recv = genExpr(*unary.operand);
                Type callRet;
                return genTraitMethodCall(&unary, operandType.className, "neg", recv, {}, {}, callRet);
            }
            std::string value      = genExpr(*unary.operand);
            std::string irType     = irTypeName(operandType);
            if (isFloat(operandType.kind)) {
                std::string tempName = freshTemp();
                emit("%" + tempName + " = fneg " + irType + " " + value);
                return "%" + tempName;
            }
            // Checked signed negation catches -INT_MIN (0 - INT_MIN overflows). Unsigned unary
            // minus is left as a plain wrapping `sub` (negating an unsigned is a degenerate op).
            if (overflowChecks_ && isSignedInt(operandType.kind))
                return emitCheckedArith(TokenType::MINUS, operandType, "0", value);
            std::string tempName = freshTemp();
            emit("%" + tempName + " = sub " + irType + " 0, " + value);
            return "%" + tempName;
        }
        case TokenType::BANG: {
            std::string value      = genExpr(*unary.operand);
            Type        operandType = exprType(*unary.operand);
            std::string boolValue  = emitToBool(value, operandType);
            std::string tempName   = freshTemp();
            emit("%" + tempName + " = xor i1 " + boolValue + ", true");
            return "%" + tempName;
        }
        case TokenType::TILDE: {
            std::string value      = genExpr(*unary.operand);
            Type        operandType = exprType(*unary.operand);
            std::string irType     = irTypeName(operandType);
            std::string tempName   = freshTemp();
            emit("%" + tempName + " = xor " + irType + " " + value + ", -1");
            return "%" + tempName;
        }
        case TokenType::INCREMENT:
        case TokenType::DECREMENT: {
            // Prefix ++/-- : load, modify, store, return new value
            const auto& id       = std::get<IdentifierExpr>(*unary.operand->node);
            Type        variableType;
            std::string ptrName;
            if (!resolveAssignTarget(id.name.lexeme, ptrName, variableType)) return "0";
            std::string irType       = irTypeName(variableType);
            std::string oldValue     = emitLoad(irType, ptrName);
            // Checked ++/-- on integers trap on overflow, matching `x += 1` / `x -= 1`.
            if (overflowChecks_ && isInteger(variableType.kind)) {
                TokenType op = (unary.operatorToken.type == TokenType::INCREMENT)
                             ? TokenType::PLUS : TokenType::MINUS;
                std::string nv = emitCheckedArith(op, variableType, oldValue, "1");
                emitStore(irType, nv, ptrName);
                return nv;
            }
            std::string tempName     = freshTemp();
            std::string one          = isFloat(variableType.kind) ? "1.0" : "1";
            std::string instruction  = (unary.operatorToken.type == TokenType::INCREMENT) ? "add" : "sub";
            if (isFloat(variableType.kind))
                instruction = (unary.operatorToken.type == TokenType::INCREMENT) ? "fadd" : "fsub";
            emit("%" + tempName + " = " + instruction + " " + irType + " " + oldValue + ", " + one);
            emitStore(irType, "%" + tempName, ptrName);
            return "%" + tempName;
        }
        default:
            (void)resolvedType;
            return genExpr(*unary.operand);
    }
}

// ---- Binary ----

// Binary operator → trait method name, or nullptr if the operator isn't overloadable.
static const char* binaryOperatorMethod(TokenType op) {
    switch (op) {
        case TokenType::PLUS:    return "add";
        case TokenType::MINUS:   return "sub";
        case TokenType::STAR:    return "mul";
        case TokenType::SLASH:   return "div";
        case TokenType::PERCENT: return "rem";
        case TokenType::EQUAL_EQUAL:
        case TokenType::BANG_EQUAL:    return "eq";
        case TokenType::LESS:
        case TokenType::LESS_EQUAL:
        case TokenType::GREATER:
        case TokenType::GREATER_EQUAL: return "cmp";
        default: return nullptr;
    }
}

// Emit `lhs == rhs` as an i1, choosing the path recorded by semantics. Operands are already-
// evaluated SSA values (value objects → their address). Shared by genBinary and switch labels.
std::string CodeGen::emitEquality(const void* nodeKey, const std::string& lval, const Type& lt,
                                  const std::string& rval, const Type& rt, TokenType op) {
    const bool ne = (op == TokenType::BANG_EQUAL);
    // Value-object memberwise structural equality (generated @Class_structeq → i1, then negate).
    if (structuralValueCmp_ && structuralValueCmp_->count(nodeKey)) {
        structEqNeeded_.insert(lt.className);
        std::string eq = freshTemp();
        emit("%" + eq + " = call i1 @" + lt.className + "_structeq(ptr " + lval + ", ptr " + rval + ")");
        if (!ne) return "%" + eq;
        std::string t = freshTemp();
        emit("%" + t + " = xor i1 %" + eq + ", true");
        return "%" + t;
    }
    // Nullable primitive compared with `null` — test the presence tag (`{i1,iN}` field 0).
    {
        auto isOptPrim = [](const Type& t) {
            return t.isNullable && (isNumeric(t.kind) || t.kind == TypeKind::Bool || t.kind == TypeKind::Char);
        };
        if (lt.kind == TypeKind::Null || rt.kind == TypeKind::Null) {
            bool leftNull = lt.kind == TypeKind::Null;
            const Type&        ot = leftNull ? rt : lt;
            const std::string& ov = leftNull ? rval : lval;
            if (isOptPrim(ot)) {
                std::string tag = freshTemp();
                emit("%" + tag + " = extractvalue " + irTypeName(ot) + " " + ov + ", 0");
                std::string t = freshTemp();
                // `== null` → present==false (absent); `!= null` → present==true.
                emit("%" + t + " = icmp eq i1 %" + tag + ", " + (ne ? "true" : "false"));
                return "%" + t;
            }
        }
    }
    // Reference address identity (recorded by semantics) or enum singleton identity.
    bool addrId = addressIdentityCmp_ && addressIdentityCmp_->count(nodeKey);
    if (addrId || lt.kind == TypeKind::Enum || rt.kind == TypeKind::Enum) {
        std::string t = freshTemp();
        emit("%" + t + " = icmp " + (ne ? "ne" : "eq") + " ptr " + lval + ", " + rval);
        return "%" + t;
    }
    // Eq-trait impl on a class → @Class_eq(recv, arg) -> i1 (then negate for `!=`).
    if (lt.kind == TypeKind::Object || lt.kind == TypeKind::Reference) {
        Type callRet;
        std::string res = genTraitMethodCall(nodeKey, lt.className, "eq", lval, { rt }, { rval }, callRet);
        if (!ne) return res;
        std::string t = freshTemp();
        emit("%" + t + " = xor i1 " + res + ", true");
        return "%" + t;
    }
    // Primitive equality: widen to a common type, then icmp/fcmp eq/ne directly.
    Type common   = commonArithmeticType(lt, rt);
    std::string l = emitCast(lval, lt, common);
    std::string r = emitCast(rval, rt, common);
    std::string t = freshTemp();
    emit("%" + t + " = " + cmpInstr(op, common) + " "
         + irTypeName(common) + " " + l + ", " + r);
    return "%" + t;
}

std::string CodeGen::genBinary(const BinaryExpr& binary, const Type& resolvedType) {
    TokenType operatorType = binary.operatorToken.type;

    // Equality (==/!=) → shared classifier (structural / identity / Eq-impl / primitive).
    if (operatorType == TokenType::EQUAL_EQUAL || operatorType == TokenType::BANG_EQUAL) {
        Type lt = exprType(*binary.left);
        Type rt = exprType(*binary.right);
        std::string lval = genExpr(*binary.left);
        std::string rval = genExpr(*binary.right);
        // A `ref <primitive>` operand decays to its value (load) before comparison.
        if (isPrimitiveBorrow(lt)) { lval = emitLoad(irTypeName(borrowElementType(lt)), lval); lt = borrowElementType(lt); }
        if (isPrimitiveBorrow(rt)) { rval = emitLoad(irTypeName(borrowElementType(rt)), rval); rt = borrowElementType(rt); }
        return emitEquality(&binary, lval, lt, rval, rt, operatorType);
    }

    // Operator overloading: a class-typed left operand desugars to a trait method call
    // (Add/Sub/Mul/Div/Rem/Ord — equality handled above).
    {
        Type leftType = decayPrimitiveBorrow(exprType(*binary.left));   // a primitive borrow is not a class operand
        const char* method = binaryOperatorMethod(operatorType);
        bool isAddrIdentity = addressIdentityCmp_ && addressIdentityCmp_->count(&binary);
        if (method && !isAddrIdentity
            && (leftType.kind == TypeKind::Object || leftType.kind == TypeKind::Reference)) {
            std::string recv   = genExpr(*binary.left);
            Type        rt     = exprType(*binary.right);
            std::string rv     = genExpr(*binary.right);
            Type        callRet;
            std::string res = genTraitMethodCall(&binary, leftType.className, method, recv,
                                                 { rt }, { rv }, callRet);
            if (operatorType == TokenType::EQUAL_EQUAL) return res;   // eq → bool
            if (operatorType == TokenType::BANG_EQUAL) {
                std::string t = freshTemp();
                emit("%" + t + " = xor i1 " + res + ", true");
                return "%" + t;
            }
            if (operatorType == TokenType::LESS || operatorType == TokenType::LESS_EQUAL
                || operatorType == TokenType::GREATER || operatorType == TokenType::GREATER_EQUAL) {
                const char* pred = operatorType == TokenType::LESS        ? "slt"
                                 : operatorType == TokenType::LESS_EQUAL  ? "sle"
                                 : operatorType == TokenType::GREATER     ? "sgt" : "sge";
                std::string t = freshTemp();
                emit("%" + t + " = icmp " + pred + " " + irTypeName(callRet) + " " + res + ", 0");
                return "%" + t;
            }
            return res;   // arithmetic — result is the method's return value
        }
    }

    // Short-circuit logical ops
    if (operatorType == TokenType::AND || operatorType == TokenType::OR) {
        // Eager evaluation — emit and i1 / or i1
        Type        leftType  = exprType(*binary.left);
        Type        rightType = exprType(*binary.right);
        std::string leftValue  = genExpr(*binary.left);
        std::string rightValue = genExpr(*binary.right);
        std::string leftBool   = emitToBool(leftValue,  leftType);
        std::string rightBool  = emitToBool(rightValue, rightType);
        std::string tempName   = freshTemp();
        std::string instruction = (operatorType == TokenType::AND) ? "and" : "or";
        emit("%" + tempName + " = " + instruction + " i1 " + leftBool + ", " + rightBool);
        return "%" + tempName;
    }

    // Comparison ops — result is Bool (i1)
    bool isComparison = (operatorType == TokenType::EQUAL_EQUAL || operatorType == TokenType::BANG_EQUAL ||
                         operatorType == TokenType::LESS         || operatorType == TokenType::LESS_EQUAL  ||
                         operatorType == TokenType::GREATER      || operatorType == TokenType::GREATER_EQUAL);

    Type leftRaw   = exprType(*binary.left);
    Type rightRaw  = exprType(*binary.right);
    Type leftType  = decayPrimitiveBorrow(leftRaw);    // `ref <primitive>` operands decay to their value type
    Type rightType = decayPrimitiveBorrow(rightRaw);

    // Identity comparison (==/!=) lowering to `icmp eq/ne ptr`: enum singletons, or two class
    // references with no `Eq` impl (address identity — recorded by semantics). Never a primitive
    // borrow (decayed above), so the raw operands are safe to compare as pointers here.
    if (isComparison
        && (leftType.kind == TypeKind::Enum || rightType.kind == TypeKind::Enum
            || (addressIdentityCmp_ && addressIdentityCmp_->count(&binary)))) {
        std::string l = genExpr(*binary.left);
        std::string r = genExpr(*binary.right);
        std::string t = freshTemp();
        std::string op = (operatorType == TokenType::EQUAL_EQUAL) ? "eq" : "ne";
        emit("%" + t + " = icmp " + op + " ptr " + l + ", " + r);
        return "%" + t;
    }

    // Determine the arithmetic type for the operation
    Type operandType = isComparison
                     ? commonArithmeticType(leftType, rightType)
                     : resolvedType;

    std::string leftValue  = genExpr(*binary.left);
    std::string rightValue = genExpr(*binary.right);

    // Load through a `ref <primitive>` operand (deref) before the common-type cast.
    if (isPrimitiveBorrow(leftRaw))  leftValue  = emitLoad(irTypeName(leftType),  leftValue);
    if (isPrimitiveBorrow(rightRaw)) rightValue = emitLoad(irTypeName(rightType), rightValue);

    // Cast operands to the common type
    leftValue  = emitCast(leftValue,  leftType,  operandType);
    rightValue = emitCast(rightValue, rightType, operandType);

    std::string irType   = irTypeName(operandType);
    std::string tempName = freshTemp();

    if (isComparison) {
        std::string comparisonInstruction = cmpInstr(operatorType, operandType);
        emit("%" + tempName + " = " + comparisonInstruction + " " + irType + " " + leftValue + ", " + rightValue);
    } else {
        // Checked integer +/-/* trap on overflow (opt-in). Division/remainder, floats, and the
        // bitwise/shift operators keep the plain instruction.
        if (overflowChecks_ && isInteger(operandType.kind)
            && (operatorType == TokenType::PLUS || operatorType == TokenType::MINUS
                || operatorType == TokenType::STAR))
            return emitCheckedArith(operatorType, operandType, leftValue, rightValue);
        std::string arithmeticInstruction = arithInstr(operatorType, operandType);
        emit("%" + tempName + " = " + arithmeticInstruction + " " + irType + " " + leftValue + ", " + rightValue);
    }
    return "%" + tempName;
}

// ---- Assign ----

bool CodeGen::resolveAssignTarget(const std::string& name, std::string& ptrOut, Type& typeOut) {
    auto varTypeIt = varTypeMap.find(name);
    auto allocaIt  = allocaMap.find(name);
    if (varTypeIt != varTypeMap.end() && allocaIt != allocaMap.end()) {
        typeOut = varTypeIt->second;
        ptrOut  = allocaIt->second;
        return true;
    }
    // Implicit `this`: a static field global, or an instance field GEP on `this`.
    if (const Type* sft = findStaticField(currentClassName_, name)) {
        typeOut = *sft;
        ptrOut  = "@" + currentClassName_ + "$" + name;
        return true;
    }
    Type ft;
    std::string gep = genImplicitFieldPtr(name, ft);
    if (!gep.empty()) { typeOut = ft; ptrOut = gep; return true; }
    return false;
}

std::string CodeGen::genBraceInit(const BraceInitExpr& braceInit) {
    // Untyped `{args}` — the class was deduced by semantics. Construct it as a temp value object
    // (like a `Class{args}` constructor rvalue) and return its address, so it borrows correctly
    // into a `Class&` parameter / value initializer.
    if (!braceInitClass_) return "0";
    auto it = braceInitClass_->find(&braceInit);
    if (it == braceInitClass_->end()) return "0";
    const std::string& cn = it->second;

    std::string tmp = freshAllocaName("objtmp");
    emitAlloca(tmp, "%" + cn);
    emit("store %" + cn + " zeroinitializer, ptr " + tmp);
    auto cgIt = cgClasses_.find(cn);
    if (cgIt != cgClasses_.end() && cgIt->second.needsDtor && !dtorScopes_.empty())
        dtorScopes_.back().push_back({ tmp, cn, /*isReference=*/false });
    std::string mangledCtor = calleeName(&braceInit, cn + "_" + cn);
    auto ctorIt = funcParamTypes.find(mangledCtor);
    if (ctorIt != funcParamTypes.end()) {
        std::string argStr = buildArgString(braceInit.args, &ctorIt->second, defaultsFor(mangledCtor));
        emit("call void @" + mangledCtor + "(ptr " + tmp + (argStr.empty() ? "" : ", " + argStr) + ")");
    }
    return tmp;
}

std::string CodeGen::genRefStore(const RefStoreExpr& rs) {
    // Store through a reference-valued expression (e.g. `v.at(i) = x`). Semantic analysis has
    // verified the target is a primitive borrow; evaluate it to the referent pointer and store.
    Type targetType = exprType(*rs.target);
    std::string referent = genExpr(*rs.target);   // the referent pointer
    Type elem = borrowElementType(targetType);
    Type rhsType = exprType(*rs.value);
    std::string val = genExpr(*rs.value);
    val = emitCast(val, rhsType, elem);
    emitStore(irTypeName(elem), val, referent);
    return val;
}

std::string CodeGen::genBorrowSource(const Expr& source) {
    // Already a borrow / reference value (a `ref`-returning call, or an expression the analyzer
    // typed as a borrow): the produced value IS the referent pointer.
    if (isBorrow(exprType(source))) return genExpr(source);

    if (source.node) {
        if (const auto* id = std::get_if<IdentifierExpr>(source.node.get())) {
            // A `ref <primitive>` local holds the referent pointer — load it to pass it onward.
            auto vt = varTypeMap.find(id->name.lexeme);
            if (vt != varTypeMap.end() && isPrimitiveBorrow(vt->second)) {
                auto a = allocaMap.find(id->name.lexeme);
                if (a != allocaMap.end()) return emitLoad("ptr", a->second);
            }
            // Otherwise a plain addressable lvalue — its storage address.
            std::string ptr; Type t;
            if (resolveAssignTarget(id->name.lexeme, ptr, t)) return ptr;
        }
        if (const auto* idx = std::get_if<IndexExpr>(source.node.get())) {
            std::string elemIr;
            return genElementAddress(*idx->object, *idx->index, elemIr);
        }
    }
    return "";  // not addressable (a temporary) — semantic analysis rejects this
}

std::string CodeGen::genAssign(const AssignExpr& assign) {
    Type        lhsType;
    std::string ptrName;
    if (!resolveAssignTarget(assign.name.lexeme, ptrName, lhsType)) return "0";
    std::string irType  = irTypeName(lhsType);

    // Write THROUGH a `ref <primitive>` (like C++ `int& r; r = 5;`): load the referent pointer,
    // store the value there. This is not a rebind — the borrow keeps pointing at the same slot.
    if (isPrimitiveBorrow(lhsType)) {
        Type        elem    = borrowElementType(lhsType);
        Type        rhsType = exprType(*assign.value);
        std::string val     = genExpr(*assign.value);
        val = emitCast(val, rhsType, elem);
        std::string referent = emitLoad("ptr", ptrName);
        emitStore(irTypeName(elem), val, referent);
        return val;
    }

    // Borrow rebind (`ref` local): just re-point it — no retain/release (it doesn't own the target).
    if (lhsType.kind == TypeKind::Reference && lhsType.borrow) {
        Type        rhsType = exprType(*assign.value);
        std::string newVal  = genExpr(*assign.value);
        newVal = emitCast(newVal, rhsType, lhsType);
        emitStore("ptr", newVal, ptrName);
        return newVal;
    }

    // Reference rebind: retain the new target, release the old, then store.
    // retain-before-release keeps self-assignment (a = a) safe.
    if (lhsType.kind == TypeKind::Reference) {
        usesRefcount_ = true;
        bool plusOne = producesPlusOne(*assign.value);
        Type        rhsType = exprType(*assign.value);
        std::string newVal  = genExpr(*assign.value);
        newVal = emitCast(newVal, rhsType, lhsType);
        if (plusOne) claimTemp(newVal);
        else         emit("call void @gg_retain(ptr " + newVal + ")");

        std::string oldVal  = emitLoad("ptr", ptrName);
        auto        cgIt    = cgClasses_.find(lhsType.className);
        std::string dtorArg = (cgIt != cgClasses_.end() && cgIt->second.needsDtor)
                            ? ("@" + lhsType.className + "_dtor") : "null";
        emit("call void @gg_release(ptr " + oldVal + ", ptr " + dtorArg + ")");

        emitStore("ptr", newVal, ptrName);
        return newVal;
    }

    // Value-object copy assignment: deep-copy via clone (handles value = value
    // and value = ref). clone releases the destination's old reference fields.
    if (lhsType.kind == TypeKind::Object) {
        std::string src = genExpr(*assign.value);   // Object→alloca; Reference→loaded heap ptr
        clonesNeeded_.insert(lhsType.className);
        emit("call void @" + lhsType.className + "_clone(ptr " + ptrName + ", ptr " + src + ")");
        return ptrName;
    }

    Type        rhsType = exprType(*assign.value);
    std::string value   = genExpr(*assign.value);
    value = emitCast(value, rhsType, lhsType);

    emitStore(irType, value, ptrName);
    return value;
}

// ---- CompoundAssign ----

std::string CodeGen::genCompoundAssign(const CompoundAssignExpr& compoundAssign) {
    Type        lhsType;
    std::string ptrName;
    if (!resolveAssignTarget(compoundAssign.name.lexeme, ptrName, lhsType)) return "0";
    std::string irType       = irTypeName(lhsType);

    // Load current value
    std::string currentValue = emitLoad(irType, ptrName);

    // Evaluate RHS
    Type        rhsType    = exprType(*compoundAssign.value);
    std::string rightValue = genExpr(*compoundAssign.value);
    rightValue = emitCast(rightValue, rhsType, lhsType);

    // Apply the base operation
    TokenType   baseOperatorType        = compoundBaseOp(compoundAssign.operatorToken.type);
    // Checked integer +=/-=/*= trap on overflow (opt-in), mirroring the binary path.
    if (overflowChecks_ && isInteger(lhsType.kind)
        && (baseOperatorType == TokenType::PLUS || baseOperatorType == TokenType::MINUS
            || baseOperatorType == TokenType::STAR)) {
        std::string checked = emitCheckedArith(baseOperatorType, lhsType, currentValue, rightValue);
        emitStore(irType, checked, ptrName);
        return checked;
    }
    std::string arithmeticInstruction   = arithInstr(baseOperatorType, lhsType);
    std::string tempName                = freshTemp();
    emit("%" + tempName + " = " + arithmeticInstruction + " " + irType + " " + currentValue + ", " + rightValue);

    emitStore(irType, "%" + tempName, ptrName);
    return "%" + tempName;
}

// ---- Postfix ----

std::string CodeGen::genPostfix(const PostfixExpr& postfix) {
    const auto& id       = std::get<IdentifierExpr>(*postfix.operand->node);
    Type        variableType;
    std::string ptrName;
    if (!resolveAssignTarget(id.name.lexeme, ptrName, variableType)) return "0";
    std::string irType       = irTypeName(variableType);

    // Load old value (this is the result of the postfix expression)
    std::string oldValue = emitLoad(irType, ptrName);

    // Compute new value — checked ++/-- on integers trap on overflow (as the prefix form does).
    if (overflowChecks_ && isInteger(variableType.kind)) {
        TokenType op = (postfix.operatorToken.type == TokenType::INCREMENT)
                     ? TokenType::PLUS : TokenType::MINUS;
        std::string nv = emitCheckedArith(op, variableType, oldValue, "1");
        emitStore(irType, nv, ptrName);
        return oldValue;   // postfix yields the OLD value
    }
    std::string tempName    = freshTemp();
    std::string one         = isFloat(variableType.kind) ? "1.0" : "1";
    std::string instruction = (postfix.operatorToken.type == TokenType::INCREMENT) ? "add" : "sub";
    if (isFloat(variableType.kind))
        instruction = (postfix.operatorToken.type == TokenType::INCREMENT) ? "fadd" : "fsub";
    emit("%" + tempName + " = " + instruction + " " + irType + " " + oldValue + ", " + one);
    emitStore(irType, "%" + tempName, ptrName);

    return oldValue;  // return OLD value
}

// ---- Call ----

std::string CodeGen::genCall(const CallExpr& call, const Type& resolvedType) {
    // Callable-object invocation `f(args)` → `f.call(args)` (recorded by semantics). The callee
    // variable is the receiver: a value object's alloca is its address; a reference is loaded.
    if (callableCalls_) {
        auto cit = callableCalls_->find(&call);
        if (cit != callableCalls_->end()) {
            const std::string& className = cit->second;
            std::string recv;
            auto aIt = allocaMap.find(call.callee.lexeme);
            Type vt{TypeKind::Object};
            auto vtIt = varTypeMap.find(call.callee.lexeme);
            if (vtIt != varTypeMap.end()) vt = vtIt->second;
            if (aIt != allocaMap.end())
                recv = (vt.kind == TypeKind::Reference) ? emitLoad("ptr", aIt->second) : aIt->second;
            std::vector<Type>        argTypes;
            std::vector<std::string> argVals;
            for (const auto& a : call.args) { argTypes.push_back(exprType(*a)); argVals.push_back(genExpr(*a)); }
            Type ret;
            return genTraitMethodCall(&call, className, "call", recv, argTypes, argVals, ret);
        }
    }

    std::string returnIrType = irTypeName(resolvedType);

    // Constructor call used as an rvalue (`Class(args)` outside a variable initializer, e.g. as a
    // function argument): materialize a temp value object and return its ADDRESS. Value objects are
    // represented by address throughout, so this borrows correctly as a `Class&` argument.
    if (resolvedType.kind == TypeKind::Object && cgClasses_.count(call.callee.lexeme)) {
        const std::string& cn = call.callee.lexeme;
        std::string tmp = freshAllocaName("objtmp");
        emitAlloca(tmp, "%" + cn);
        emit("store %" + cn + " zeroinitializer, ptr " + tmp);
        auto cgIt = cgClasses_.find(cn);
        if (cgIt != cgClasses_.end() && cgIt->second.needsDtor && !dtorScopes_.empty())
            dtorScopes_.back().push_back({ tmp, cn, /*isReference=*/false });
        std::string mangledCtor = calleeName(&call, cn + "_" + cn);
        auto ctorIt = funcParamTypes.find(mangledCtor);
        if (ctorIt != funcParamTypes.end()) {
            std::string argStr = buildArgString(call.args, &ctorIt->second, defaultsFor(mangledCtor),
                                                orderFor(&call));
            emit("call void @" + mangledCtor + "(ptr " + tmp + (argStr.empty() ? "" : ", " + argStr) + ")");
        }
        return tmp;
    }

    // Return-slot (sret) call used as a value (not a plain variable initializer — that path
    // writes in place via emitSlotCall): materialize the result into a temp object.
    {
        std::string fn, recv;
        if (!freeFnBases_.count(call.callee.lexeme) && !currentClassName_.empty()) {
            std::string mName = calleeName(&call, currentClassName_ + "_" + call.callee.lexeme);
            if (slotReturningFns_.count(mName)) {
                auto cgIt = cgClasses_.find(currentClassName_);
                bool isStatic = cgIt != cgClasses_.end()
                             && cgIt->second.staticMethods.count(call.callee.lexeme) > 0;
                fn = mName;
                recv = isStatic ? "" : (allocaMap.count("this") ? allocaMap["this"] : "null");
            }
        }
        if (fn.empty()) {
            std::string fnName = calleeName(&call, call.callee.lexeme);
            if (slotReturningFns_.count(fnName)) fn = fnName;
        }
        if (!fn.empty()) {
            std::string tmp = materializeSlotTemp(resolvedType.className);
            emitSretCall(fn, call.args, tmp, recv, orderFor(&call));
            return tmp;
        }
    }

    // Implicit `this`: a bare call with no matching free function targets a method of the
    // enclosing class (free functions take priority — mirrors the semantic resolution).
    if (!freeFnBases_.count(call.callee.lexeme) && !currentClassName_.empty()) {
        std::string mName = calleeName(&call, currentClassName_ + "_" + call.callee.lexeme);
        auto mp = funcParamTypes.find(mName);
        if (mp != funcParamTypes.end()) {
            std::string argStr = buildArgString(call.args, &mp->second, defaultsFor(mName),
                                                orderFor(&call));
            auto cgIt = cgClasses_.find(currentClassName_);
            bool isStatic = cgIt != cgClasses_.end()
                         && cgIt->second.staticMethods.count(call.callee.lexeme) > 0;
            std::string fullArgs = argStr;
            if (!isStatic) {
                std::string thisPtr = allocaMap.count("this") ? allocaMap["this"] : "null";
                fullArgs = "ptr " + thisPtr + (argStr.empty() ? "" : ", " + argStr);
            }
            if (returnIrType == "void") {
                emit("call void @" + mName + "(" + fullArgs + ")");
                return "";
            }
            std::string t = freshTemp();
            emit("%" + t + " = call " + returnIrType + " @" + mName + "(" + fullArgs + ")");
            if (resolvedType.kind == TypeKind::Reference && !resolvedType.borrow)
                pendingTemps_.push_back({ "%" + t, resolvedType.className });
            return "%" + t;
        }
    }

    // Free-function call — use the resolved (possibly overload-mangled) symbol name.
    std::string fnName = calleeName(&call, call.callee.lexeme);
    auto funcIt = funcParamTypes.find(fnName);
    const std::vector<Type>* declaredParams =
        funcIt != funcParamTypes.end() ? &funcIt->second : nullptr;

    std::string argStr = buildArgString(call.args, declaredParams, defaultsFor(fnName), orderFor(&call));

    if (returnIrType == "void") {
        emit("call void @" + fnName + "(" + argStr + ")");
        return "";
    }
    std::string t = freshTemp();
    emit("%" + t + " = call " + returnIrType + " @" + fnName + "(" + argStr + ")");
    if (resolvedType.kind == TypeKind::Reference && !resolvedType.borrow)   // owning ref-call hands back a +1 (a borrow owns nothing)
        pendingTemps_.push_back({ "%" + t, resolvedType.className });
    return "%" + t;
}

// ---- Return-slot (sret) call emission ----

void CodeGen::emitSretCall(const std::string& fn,
                           const std::vector<std::unique_ptr<Expr>>& args,
                           const std::string& slotPtr, const std::string& recvPtr,
                           const std::vector<int>* order) {
    auto it = funcParamTypes.find(fn);
    std::string argStr = buildArgString(args, it != funcParamTypes.end() ? &it->second : nullptr,
                                        defaultsFor(fn), order);
    std::string full = "ptr " + slotPtr;
    if (!recvPtr.empty()) full += ", ptr " + recvPtr;
    if (!argStr.empty())  full += ", " + argStr;
    emit("call void @" + fn + "(" + full + ")");
}

std::string CodeGen::materializeSlotTemp(const std::string& className) {
    std::string tmp = freshAllocaName("slottmp");
    emitAlloca(tmp, "%" + className);
    emit("store %" + className + " zeroinitializer, ptr " + tmp);
    auto cgIt = cgClasses_.find(className);
    if (cgIt != cgClasses_.end() && cgIt->second.needsDtor && !dtorScopes_.empty())
        dtorScopes_.back().push_back({ tmp, className, /*isReference=*/false });
    return tmp;
}

// Zero-copy path used by variable initialization: if `init` is a call to a return-slot
// function/method, emit it writing directly into `slotPtr` and return true. Determines
// slot-ness with side-effect-free lookups BEFORE emitting anything (so a non-slot call
// leaves the stream untouched for the caller's fallback path).
bool CodeGen::emitSlotCall(const Expr& init, const std::string& slotPtr) {
    if (!init.node) return false;
    const auto& node = *init.node;

    if (std::holds_alternative<CallExpr>(node)) {
        const auto& call = std::get<CallExpr>(node);
        // Implicit-`this` slot method: no matching free function, inside a class body.
        if (!freeFnBases_.count(call.callee.lexeme) && !currentClassName_.empty()) {
            std::string mName = calleeName(&call, currentClassName_ + "_" + call.callee.lexeme);
            if (slotReturningFns_.count(mName)) {
                auto cgIt = cgClasses_.find(currentClassName_);
                bool isStatic = cgIt != cgClasses_.end()
                             && cgIt->second.staticMethods.count(call.callee.lexeme) > 0;
                std::string recv = isStatic ? "" : (allocaMap.count("this") ? allocaMap["this"] : "null");
                emitSretCall(mName, call.args, slotPtr, recv, orderFor(&call));
                return true;
            }
        }
        std::string fnName = calleeName(&call, call.callee.lexeme);
        if (!slotReturningFns_.count(fnName)) return false;
        emitSretCall(fnName, call.args, slotPtr, "", orderFor(&call));
        return true;
    }

    if (std::holds_alternative<MethodCallExpr>(node)) {
        const auto& mc = std::get<MethodCallExpr>(node);
        // Static call via type name: Class::method(...).
        if (std::holds_alternative<IdentifierExpr>(*mc.object->node)) {
            const auto& id = std::get<IdentifierExpr>(*mc.object->node);
            auto cgIt = cgClasses_.find(id.name.lexeme);
            if (cgIt != cgClasses_.end() && cgIt->second.staticMethods.count(mc.method.lexeme)) {
                std::string mName = calleeName(&mc, id.name.lexeme + "_" + mc.method.lexeme);
                if (!slotReturningFns_.count(mName)) return false;
                emitSretCall(mName, mc.args, slotPtr, "", orderFor(&mc));
                return true;
            }
        }
        Type objType = exprType(*mc.object);
        if (objType.kind != TypeKind::Object && objType.kind != TypeKind::Reference) return false;
        auto cgIt = cgClasses_.find(objType.className);
        bool isStatic = cgIt != cgClasses_.end() && cgIt->second.staticMethods.count(mc.method.lexeme) > 0;
        std::string mName = calleeName(&mc, objType.className + "_" + mc.method.lexeme);
        if (!slotReturningFns_.count(mName)) return false;
        std::string recv = isStatic ? "" : genExpr(*mc.object);   // only evaluate once confirmed
        emitSretCall(mName, mc.args, slotPtr, recv, orderFor(&mc));
        return true;
    }

    return false;
}

// ---- VarDecl ----

// A `var` local carries a `var` sentinel type token; the semantic analyzer deduced its real type
// and recorded the synthesized type token. Swap it in so every branch below resolves the type as
// though it were written explicitly. A normal declaration returns its own token unchanged.
Token CodeGen::varDeclTypeToken(const VarDeclExpr& v) const {
    if (v.typeName.type == TokenType::VAR && inferredVarType_) {
        auto it = inferredVarType_->find(&v);
        if (it != inferredVarType_->end()) return it->second;
    }
    return v.typeName;
}

std::string CodeGen::genVarDecl(const VarDeclExpr& varDecl) {
    const Token typeTok = varDeclTypeToken(varDecl);
    // ---- C-style static local (persistent global) ----
    // Semantics guarantee a scalar primitive type here.
    if (varDecl.isStatic) return genStaticLocal(varDecl);

    // ---- Array declaration ----
    if (varDecl.arraySize > 0) {
        TypeKind elementKind = typeFromToken(typeTok.type).kind;
        Type     arrayType   = makeArrayType(elementKind, varDecl.arraySize);
        std::string arrayIrType = irTypeName(arrayType);
        std::string name        = varDecl.name.lexeme;

        std::string ptrName = freshAllocaName(name);

        emitAlloca(ptrName, arrayIrType);
        allocaMap[name]  = ptrName;
        varTypeMap[name] = arrayType;

        // Zero-initialise the entire array in one store
        emit("store " + arrayIrType + " zeroinitializer, ptr " + ptrName);

        return ptrName;
    }

    // ---- Borrow (`ref Class`) declaration ----
    // A non-owning reference: a ptr to the object body, NOT retained and NOT released at scope exit
    // (so it is never registered in a dtor scope). Same IR as a reference otherwise.
    {
        Type synth = decodeSynthesizedType(typeTok);
        if (synth.kind == TypeKind::Reference && synth.borrow) {
            std::string name    = varDecl.name.lexeme;
            std::string ptrName = freshAllocaName(name);
            emitAlloca(ptrName, "ptr");
            allocaMap[name]  = ptrName;
            varTypeMap[name] = synth;
            if (debug_) dbgDeclare(ptrName, name, synth, varDecl.name.line, 0);
            if (varDecl.initializer) {
                if (isPrimitiveBorrow(synth)) {
                    // `ref <primitive>`: store the referent pointer (address of the borrowed
                    // lvalue, or the pointer produced by a `ref`-returning call). No load, no copy.
                    std::string addr = genBorrowSource(*varDecl.initializer);
                    emitStore("ptr", addr, ptrName);
                } else {
                    Type        initType = exprType(*varDecl.initializer);
                    std::string value    = genExpr(*varDecl.initializer);
                    value = emitCast(value, initType, synth);   // object/ref → borrow: address, no-op
                    emitStore("ptr", value, ptrName);            // borrow: no retain
                }
            } else {
                emitStore("ptr", "null", ptrName);
            }
            return ptrName;
        }
    }

    // ---- Reference (Class&, or nullable Class&?) declaration ----
    {
        std::string lex = typeTok.lexeme;
        bool nullable = !lex.empty() && lex.back() == '?';
        if (nullable) lex.pop_back();   // `Class&?` → `Class&` (same IR; null is a valid ptr value)
        if (typeTok.type == TokenType::IDENTIFIER && !lex.empty() && lex.back() == '&') {
        usesRefcount_ = true;
        std::string className = lex.substr(0, lex.size() - 1);
        Type        refType   = makeReferenceType(className);
        if (nullable) refType = makeNullable(refType);
        std::string name      = varDecl.name.lexeme;
        std::string ptrName   = freshAllocaName(name);

        emitAlloca(ptrName, "ptr");
        allocaMap[name]  = ptrName;
        varTypeMap[name] = refType;
        if (debug_) dbgDeclare(ptrName, name, refType, varDecl.name.line, 0);

        // Every reference variable co-owns its target and is released at scope exit
        // (release is null-safe, so an uninitialised slot is harmless).
        if (!dtorScopes_.empty())
            dtorScopes_.back().push_back({ ptrName, className, /*isReference=*/true });

        if (varDecl.initializer) {
            bool plusOne = producesPlusOne(*varDecl.initializer);
            Type        initType = exprType(*varDecl.initializer);
            std::string value    = genExpr(*varDecl.initializer);
            value = emitCast(value, initType, refType);   // ref → ref: no-op

            // A +1 producer (`new` / reference-returning call) is taken over directly
            // (claim its pending release). Copying an existing reference co-owns it → retain.
            if (plusOne) claimTemp(value);
            else         emit("call void @gg_retain(ptr " + value + ")");

            emitStore("ptr", value, ptrName);
        } else {
            emitStore("ptr", "null", ptrName);   // uninitialised reference → null
        }

        return ptrName;
        }
    }

    // ---- Typed raw pointer (ptr<T>) declaration ----
    {
        Type synth = decodeSynthesizedType(typeTok);
        if (synth.kind == TypeKind::TypedPtr) {
            std::string name    = varDecl.name.lexeme;
            std::string ptrName = freshAllocaName(name);
            emitAlloca(ptrName, "ptr");
            allocaMap[name]  = ptrName;
            varTypeMap[name] = synth;
            if (debug_) dbgDeclare(ptrName, name, synth, varDecl.name.line, 0);
            if (varDecl.initializer) {
                Type        initType = exprType(*varDecl.initializer);
                std::string value    = genExpr(*varDecl.initializer);
                value = emitCast(value, initType, synth);   // all ptr forms are `ptr` in IR
                emitStore("ptr", value, ptrName);
            } else {
                emitStore("ptr", "null", ptrName);
            }
            return ptrName;
        }
    }

    // ---- Enum variable declaration (incl. nullable `Color?`) ----
    // An enum variable holds a `ptr` to a global singleton variant; nullable is the same IR (null
    // is a valid ptr), so strip a trailing `?`.
    if (typeTok.type == TokenType::IDENTIFIER) {
        std::string elex = typeTok.lexeme;
        bool enumNullable = !elex.empty() && elex.back() == '?';
        if (enumNullable) elex.pop_back();
        if (cgEnumNames_.count(elex)) {
        const std::string& enumName = elex;
        Type        enumType = enumNullable ? makeNullable(makeEnumType(enumName)) : makeEnumType(enumName);
        std::string name     = varDecl.name.lexeme;
        std::string ptrName  = freshAllocaName(name);

        emitAlloca(ptrName, "ptr");
        allocaMap[name]  = ptrName;
        varTypeMap[name] = enumType;
        if (debug_) dbgDeclare(ptrName, name, enumType, varDecl.name.line, 0);

        if (varDecl.initializer) {
            Type        initType = exprType(*varDecl.initializer);
            std::string value    = genExpr(*varDecl.initializer);
            value = emitCast(value, initType, enumType);   // enum → enum: no-op
            emitStore("ptr", value, ptrName);
        } else {
            emitStore("ptr", "null", ptrName);
        }
        return ptrName;
        }
    }

    // ---- Nullable primitive (`i32?`) declaration ----
    // A tagged optional `{ i1, iN }` value. (Nullable references/borrows/enums were handled by the
    // branches above; a nullable value object was rejected in semantics.)
    if (typeTok.type == TokenType::IDENTIFIER && !typeTok.lexeme.empty()
        && typeTok.lexeme.back() == '?'
        && typeKindFromName(typeTok.lexeme.substr(0, typeTok.lexeme.size()-1))
               != TypeKind::Error) {
        TypeKind prim = typeKindFromName(typeTok.lexeme.substr(0, typeTok.lexeme.size()-1));
        Type        declaredType = makeNullable(Type{prim});
        std::string irType       = irTypeName(declaredType);
        std::string name         = varDecl.name.lexeme;
        std::string ptrName      = freshAllocaName(name);
        emitAlloca(ptrName, irType);
        allocaMap[name]  = ptrName;
        varTypeMap[name] = declaredType;
        if (debug_) dbgDeclare(ptrName, name, declaredType, varDecl.name.line, 0);
        emit("store " + irType + " zeroinitializer, ptr " + ptrName);   // default empty (null)
        if (varDecl.initializer) {
            Type        initType = exprType(*varDecl.initializer);
            std::string value    = emitCast(genExpr(*varDecl.initializer), initType, declaredType);
            emitStore(irType, value, ptrName);
        }
        return ptrName;
    }

    // ---- Object (class) declaration ----
    if (typeTok.type == TokenType::IDENTIFIER) {
        // Class type — typeTok.lexeme is the class name
        const std::string& className  = typeTok.lexeme;
        Type               objectType = makeObjectType(className);
        std::string        name       = varDecl.name.lexeme;

        std::string ptrName = freshAllocaName(name);

        emitAlloca(ptrName, "%" + className);
        allocaMap[name]  = ptrName;
        varTypeMap[name] = objectType;
        if (debug_) dbgDeclare(ptrName, name, objectType, varDecl.name.line, 0);

        // Zero-initialise the struct
        emit("store %" + className + " zeroinitializer, ptr " + ptrName);

        // If this class has a destructor, register the variable for scope-exit cleanup.
        {
            auto cgIt = cgClasses_.find(className);
            if (cgIt != cgClasses_.end() && cgIt->second.needsDtor && !dtorScopes_.empty())
                dtorScopes_.back().push_back({ ptrName, className, /*isReference=*/false });
        }

        // Initializer: a return-slot call (written in place, no copy), a constructor call,
        // or a copy from a value/reference.
        if (varDecl.initializer) {
            if (emitSlotCall(*varDecl.initializer, ptrName)) {
                // Result written directly into this variable's storage — nothing to copy.
            } else if (std::holds_alternative<CallExpr>(*varDecl.initializer->node)) {
                const auto& ctorCall = std::get<CallExpr>(*varDecl.initializer->node);
                std::string mangledCtor = calleeName(&ctorCall, className + "_" + className);
                auto funcIt = funcParamTypes.find(mangledCtor);
                // No constructor exists (e.g. `C c{}` / `C c()` on a class with no ctor): the
                // storage is already zero-initialized above, so default-construction is a no-op.
                if (funcIt != funcParamTypes.end()) {
                    std::string argStr = buildArgString(ctorCall.args, &funcIt->second,
                                                        defaultsFor(mangledCtor), orderFor(&ctorCall));
                    emit("call void @" + mangledCtor + "(ptr " + ptrName
                         + (argStr.empty() ? "" : ", " + argStr) + ")");
                }
            } else {
                // Copy initialisation: Point p = <value/ref of same class> — deep copy.
                std::string src = genExpr(*varDecl.initializer);
                clonesNeeded_.insert(className);
                emit("call void @" + className + "_clone(ptr " + ptrName + ", ptr " + src + ")");
            }
        }

        return ptrName;
    }

    // ---- Scalar declaration (existing logic) ----
    Type        declaredType = typeFromToken(typeTok.type);
    std::string irType       = irTypeName(declaredType);
    std::string name         = varDecl.name.lexeme;

    // Build a unique alloca pointer name.
    // We consult usedAllocaNames (which persists across scope save/restore) so
    // that two variables with the same name in sibling scopes — e.g. two for-loops
    // both declaring 'i' — always get distinct LLVM value names within the function.
    std::string ptrName = freshAllocaName(name);

    emitAlloca(ptrName, irType);
    allocaMap[name]  = ptrName;
    varTypeMap[name] = declaredType;
    if (debug_) dbgDeclare(ptrName, name, declaredType, varDecl.name.line, 0);

    if (varDecl.initializer) {
        Type        initializerType = exprType(*varDecl.initializer);
        std::string value           = genExpr(*varDecl.initializer);
        value = emitCast(value, initializerType, declaredType);
        emitStore(irType, value, ptrName);
    }

    return ptrName;
}

// ---- C-style static local ----

std::string CodeGen::genStaticLocal(const VarDeclExpr& varDecl) {
    const Token typeTok = varDeclTypeToken(varDecl);
    Type        declaredType = typeFromToken(typeTok.type);
    std::string irType       = irTypeName(declaredType);
    const std::string& name  = varDecl.name.lexeme;

    // Mangle a unique global name "@<prefix>$<name>" (append .N on collision so
    // two same-named static locals in sibling scopes stay distinct).
    std::string base = "@" + currentStaticPrefix_ + "$" + name;
    std::string global = base;
    for (int n = 1; usedStaticGlobals_.count(global); ++n)
        global = base + "." + std::to_string(n);
    usedStaticGlobals_.insert(global);

    // Persistent zero-initialised storage; non-zero initializer runs in @gg_static_init.
    module.globals.push_back(global + " = internal global " + irType + " zeroinitializer");

    // Reads/writes of this name target the global directly (genIdentifier/genAssign
    // load and store through allocaMap[name]).
    allocaMap[name]  = global;
    varTypeMap[name] = declaredType;

    if (varDecl.initializer)
        staticLocalInits_.push_back({ global, declaredType, varDecl.initializer.get() });

    return global;
}

// ---- Index (array read) ----

// Compute the address of an indexed element. Handles fixed-size arrays
// (GEP into the array's storage) and typed raw pointers ptr<T> (GEP off the
// loaded buffer pointer). Returns the element pointer ("%tN") and writes the
// element's IR type into `elementIrTypeOut`.
std::string CodeGen::genElementAddress(const Expr& object, const Expr& index,
                                       std::string& elementIrTypeOut) {
    Type objType = exprType(object);

    // Evaluate and widen the index to i64 for GEP.
    Type        indexType  = exprType(index);
    std::string indexValue = genExpr(index);
    indexValue = emitCast(indexValue, indexType, Type{TypeKind::I64});

    if (objType.kind == TypeKind::Array) {
        Type        elementType{objType.elementKind};
        elementIrTypeOut = irTypeName(elementType);
        std::string arrayIrType = irTypeName(objType);

        // Arrays are lvalues stored in allocas — take the storage address.
        std::string base;
        if (std::holds_alternative<IdentifierExpr>(*object.node)) {
            auto it = allocaMap.find(std::get<IdentifierExpr>(*object.node).name.lexeme);
            base = it != allocaMap.end() ? it->second : "0";
        } else {
            base = genExpr(object);
        }

        if (boundsCheck) {
            ensureAbortDeclared();
            emitBoundsCheck(indexValue, objType.arraySize);
        }

        std::string elemPtr = freshTemp();
        emit("%" + elemPtr + " = getelementptr " + arrayIrType + ", ptr " + base
             + ", i32 0, i64 " + indexValue);
        return "%" + elemPtr;
    }

    // Typed raw pointer ptr<T>: GEP off the buffer pointer value (no bounds check).
    Type        elementType  = typedPtrElement(objType);
    elementIrTypeOut         = irTypeName(elementType);
    std::string buf          = genExpr(object);   // the pointer value
    std::string elemPtr      = freshTemp();
    emit("%" + elemPtr + " = getelementptr " + elementIrTypeOut + ", ptr " + buf
         + ", i64 " + indexValue);
    return "%" + elemPtr;
}

std::string CodeGen::genIndex(const IndexExpr& indexExpr) {
    // Operator overloading: a[i] on a class → the Index trait's `get` method.
    Type objType = exprType(*indexExpr.object);
    if (objType.kind == TypeKind::Object || objType.kind == TypeKind::Reference) {
        std::string recv = genExpr(*indexExpr.object);
        Type idxType = exprType(*indexExpr.index);
        std::string idx = genExpr(*indexExpr.index);
        Type callRet;
        return genTraitMethodCall(&indexExpr, objType.className, "get", recv,
                                  { idxType }, { idx }, callRet);
    }
    std::string elementIrType;
    std::string elemPtr = genElementAddress(*indexExpr.object, *indexExpr.index, elementIrType);
    return emitLoad(elementIrType, elemPtr);
}

// ---- IndexAssign (array / pointer write) ----

std::string CodeGen::genIndexAssign(const IndexAssignExpr& indexAssign) {
    // Operator overloading: a[i] = v on a class → the Index trait's `set(i, v)` method.
    {
        Type objType = exprType(*indexAssign.object);
        if (objType.kind == TypeKind::Object || objType.kind == TypeKind::Reference) {
            std::string recv    = genExpr(*indexAssign.object);
            Type        idxType = exprType(*indexAssign.index);
            std::string idx     = genExpr(*indexAssign.index);
            Type        valType = exprType(*indexAssign.value);
            std::string val     = genExpr(*indexAssign.value);
            Type callRet;
            genTraitMethodCall(&indexAssign, objType.className, "set", recv,
                               { idxType, valType }, { idx, val }, callRet);
            return val;   // the assignment expression yields the stored value
        }
    }
    std::string elementIrType;
    std::string elemPtr = genElementAddress(*indexAssign.object, *indexAssign.index, elementIrType);

    // Value (cast to element type)
    Type        objType   = exprType(*indexAssign.object);
    Type        elementType = objType.kind == TypeKind::TypedPtr
                                ? typedPtrElement(objType)
                                : Type{objType.elementKind};
    Type        valueType = exprType(*indexAssign.value);
    std::string value     = genExpr(*indexAssign.value);
    value = emitCast(value, valueType, elementType);

    emitStore(elementIrType, value, elemPtr);
    return value;  // assignment expression returns the stored value
}

// ---- Cast ----

std::string CodeGen::genCast(const CastExpr& castExpr, const Type& toType) {
    Type fromType = exprType(*castExpr.operand);

    if (isError(fromType) || isError(toType)) {
        genExpr(*castExpr.operand);
        return "0";
    }

    // Array → ptr: GEP to first element.
    // Must be handled before genExpr to avoid emitting a spurious array load.
    if (fromType.kind == TypeKind::Array && toType.kind == TypeKind::Ptr) {
        if (std::holds_alternative<IdentifierExpr>(*castExpr.operand->node)) {
            const auto& id = std::get<IdentifierExpr>(*castExpr.operand->node);
            auto it = allocaMap.find(id.name.lexeme);
            if (it != allocaMap.end()) {
                std::string elemPtr = freshTemp();
                emit("%" + elemPtr + " = getelementptr " + irTypeName(fromType)
                     + ", ptr " + it->second + ", i32 0, i32 0");
                return "%" + elemPtr;
            }
        }
        return "0";
    }

    std::string value = genExpr(*castExpr.operand);

    // Object → ptr: genIdentifier already returns the alloca pointer for Object types.
    if (fromType.kind == TypeKind::Object && toType.kind == TypeKind::Ptr)
        return value;

    // ptr → integer: ptrtoint
    if (fromType.kind == TypeKind::Ptr && isInteger(toType.kind)) {
        std::string tempName = freshTemp();
        emit("%" + tempName + " = ptrtoint ptr " + value + " to " + irTypeName(toType));
        return "%" + tempName;
    }

    // integer → ptr: inttoptr
    if (isInteger(fromType.kind) && toType.kind == TypeKind::Ptr) {
        std::string tempName = freshTemp();
        emit("%" + tempName + " = inttoptr " + irTypeName(fromType) + " " + value + " to ptr");
        return "%" + tempName;
    }

    // Numeric / bool / char conversions — emitCast covers all remaining cases. An explicit `as`
    // cast is an intentional truncation, so it is NOT overflow-checked (checked=false).
    return emitCast(value, fromType, toType, /*checked=*/false);
}

// ---- sizeof ----

std::string CodeGen::genSizeof(const SizeofExpr& sizeofExpr) {
    const Token& tok = sizeofExpr.typeName;
    // Resolve the type token to its IR type: Class& → ptr, class → %Class, else primitive.
    std::string irType;
    if (tok.type == TokenType::IDENTIFIER && !tok.lexeme.empty() && tok.lexeme.back() == '&')
        irType = "ptr";
    else if (tok.type == TokenType::IDENTIFIER && cgClasses_.count(tok.lexeme))
        irType = "%" + tok.lexeme;
    else
        irType = irTypeName(typeFromToken(tok.type));

    // sizeof via the null-GEP trick: address of element 1 of a null T-array, as i64.
    std::string gep = freshTemp();
    emit("%" + gep + " = getelementptr " + irType + ", ptr null, i32 1");
    std::string sz = freshTemp();
    emit("%" + sz + " = ptrtoint ptr %" + gep + " to i64");
    return "%" + sz;
}

// ---- compile-time reflection (scalar queries; inline for / @field are expanded in the parser) ----

std::string CodeGen::genReflect(const ReflectExpr& r) {
    auto baseName = [](std::string s) {
        if (!s.empty() && s.back() == '?') s.pop_back();
        if (s.rfind("ref:", 0) == 0)       s = s.substr(4);
        if (!s.empty() && s.back() == '&') s.pop_back();
        return s;
    };
    switch (r.kind) {
        case ReflectKind::TypeName: {
            // A private C-string constant of the type's display name (same shape as a "..." literal).
            // Resolve the token directly: synthesized forms (Point&, ptr<>, nullable) via
            // decodeSynthesizedType; a bare class/enum name is its own lexeme; else a primitive.
            std::string name;
            if (!r.typeArgs.empty()) {
                const Token& tok = r.typeArgs[0];
                Type synth = decodeSynthesizedType(tok);
                if (!isError(synth))                        name = typeName(synth);
                else if (tok.type == TokenType::IDENTIFIER) name = tok.lexeme;   // class / enum
                else                                        name = typeName(typeFromToken(tok.type));
            }
            int totalBytes = static_cast<int>(name.size()) + 1;   // + null terminator
            std::string globalName = "@.str." + std::to_string(stringCounter++);
            module.globals.push_back(globalName + " = private unnamed_addr constant ["
                + std::to_string(totalBytes) + " x i8] c\"" + name + "\\00\", align 1");
            std::string tempName = freshTemp();
            emit("%" + tempName + " = getelementptr inbounds [" + std::to_string(totalBytes)
                + " x i8], ptr " + globalName + ", i32 0, i32 0");
            return "%" + tempName;
        }
        case ReflectKind::FieldCount: {
            std::string cls = baseName(r.typeArgs.empty() ? "" : r.typeArgs[0].lexeme);
            auto it = cgClasses_.find(cls);
            return std::to_string(it != cgClasses_.end() ? it->second.fields.size() : 0);
        }
        case ReflectKind::HasField: {
            std::string cls = baseName(r.typeArgs.empty() ? "" : r.typeArgs[0].lexeme);
            std::string want;
            if (!r.valueArgs.empty())
                if (const auto* lit = std::get_if<LiteralExpr>(r.valueArgs[0]->node.get()))
                    want = lit->token.lexeme;
            auto it = cgClasses_.find(cls);
            bool has = false;
            if (it != cgClasses_.end())
                for (const auto& f : it->second.fields) if (f.first == want) { has = true; break; }
            return has ? "1" : "0";
        }
        case ReflectKind::VariantCount: {
            std::string en = baseName(r.typeArgs.empty() ? "" : r.typeArgs[0].lexeme);
            auto it = enumRegistry_ ? enumRegistry_->find(en) : std::unordered_map<std::string, EnumInfo>::const_iterator{};
            size_t n = (enumRegistry_ && it != enumRegistry_->end()) ? it->second.variantOrder.size() : 0;
            return std::to_string(n);   // u64 immediate
        }
        case ReflectKind::AlignOf: {
            // Resolve the type token, then reuse the natural-alignment layout computation.
            Type t = resolveReflectType(r.typeArgs.empty() ? Token{TokenType::VOID, "void", r.at.line}
                                                           : r.typeArgs[0]);
            auto [sz, al] = dbgSizeAlign(t);
            (void)sz;
            return std::to_string(al);   // u64 immediate
        }
        case ReflectKind::OffsetOf: {
            std::string cls = baseName(r.typeArgs.empty() ? "" : r.typeArgs[0].lexeme);
            std::string want;
            if (!r.valueArgs.empty())
                if (const auto* lit = std::get_if<LiteralExpr>(r.valueArgs[0]->node.get()))
                    want = lit->token.lexeme;
            long long off = 0;
            auto it = cgClasses_.find(cls);
            if (it != cgClasses_.end())
                for (const auto& f : it->second.fields) {
                    auto [sz, al] = dbgSizeAlign(f.second);
                    if (al > 0) off = ((off + al - 1) / al) * al;
                    if (f.first == want) break;
                    off += sz;
                }
            return std::to_string(off);   // u64 immediate (byte offset)
        }
        case ReflectKind::Implements: {
            std::string ty = baseName(r.typeArgs.empty() ? "" : r.typeArgs[0].lexeme);
            std::string trait = r.typeArgs.size() > 1 ? r.typeArgs[1].lexeme : "";
            bool ok = false;
            if (implementedTraits_) {
                auto it = implementedTraits_->find(ty);
                ok = it != implementedTraits_->end() && it->second.count(trait) > 0;
            }
            return ok ? "1" : "0";
        }
        case ReflectKind::IsInteger:
        case ReflectKind::IsFloat:
        case ReflectKind::IsClass:
        case ReflectKind::IsEnum:
        case ReflectKind::IsPrimitive: {
            std::string base = baseName(r.typeArgs.empty() ? "" : r.typeArgs[0].lexeme);
            TypeKind pk = typeKindFromName(base);   // Error unless a primitive spelling
            bool isEn  = cgEnumNames_.count(base) > 0;
            bool isCls = !isEn && cgClasses_.count(base) > 0;
            bool prim  = isInteger(pk) || isFloat(pk) || pk == TypeKind::Bool || pk == TypeKind::Char;
            bool ans = false;
            switch (r.kind) {
                case ReflectKind::IsInteger:   ans = isInteger(pk); break;
                case ReflectKind::IsFloat:     ans = isFloat(pk);   break;
                case ReflectKind::IsClass:     ans = isCls;         break;
                case ReflectKind::IsEnum:      ans = isEn;          break;
                case ReflectKind::IsPrimitive: ans = prim;          break;
                default: break;
            }
            return ans ? "1" : "0";
        }
        default:
            return "0";   // Field / CompileError never reach codegen
    }
}

// Resolve a reflection type-argument token to a Type (for layout queries). Synthesized forms
// (Point&, ptr<>, nullable) via decodeSynthesizedType; a bare identifier is a class/enum; else a
// primitive keyword.
Type CodeGen::resolveReflectType(const Token& tok) {
    Type synth = decodeSynthesizedType(tok);
    if (!isError(synth)) return synth;
    if (tok.type == TokenType::IDENTIFIER)
        return cgEnumNames_.count(tok.lexeme) ? makeEnumType(tok.lexeme) : makeObjectType(tok.lexeme);
    return typeFromToken(tok.type);
}

// ---- new (heap allocation) ----

std::string CodeGen::genNew(const NewExpr& newExpr, const Type& /*resolvedType*/) {
    usesRefcount_ = true;
    const std::string& className = newExpr.className.lexeme;

    // sizeof(%Class) via the null-GEP trick.
    std::string szPtr = freshTemp();
    emit("%" + szPtr + " = getelementptr %" + className + ", ptr null, i32 1");
    std::string szInt = freshTemp();
    emit("%" + szInt + " = ptrtoint ptr %" + szPtr + " to i64");

    // Allocate header+body on the heap (refcount = 1) and zero-initialise the body.
    std::string body = freshTemp();
    emit("%" + body + " = call ptr @gg_alloc(i64 %" + szInt + ")");
    emit("store %" + className + " zeroinitializer, ptr %" + body);

    // `new` yields a +1 reference; register it for release unless a consumer claims it.
    pendingTemps_.push_back({ "%" + body, className });

    // Copy construction: new Class(x) where x is a value/reference of the same class.
    // Deep-copy x's contents into the fresh allocation via @Class_clone.
    if (newExpr.args.size() == 1) {
        Type argType = exprType(*newExpr.args[0]);
        if ((argType.kind == TypeKind::Object || argType.kind == TypeKind::Reference)
            && argType.className == className) {
            std::string src = genExpr(*newExpr.args[0]);  // Object→alloca; Reference→loaded heap ptr
            clonesNeeded_.insert(className);
            emit("call void @" + className + "_clone(ptr %" + body + ", ptr " + src + ")");
            return "%" + body;
        }
    }

    // Run the constructor if the class defines one.
    std::string mangledCtor = calleeName(&newExpr, className + "_" + className);
    auto funcIt = funcParamTypes.find(mangledCtor);
    if (funcIt != funcParamTypes.end()) {
        std::string argStr = buildArgString(newExpr.args, &funcIt->second, defaultsFor(mangledCtor),
                                            orderFor(&newExpr));
        emit("call void @" + mangledCtor + "(ptr %" + body
             + (argStr.empty() ? "" : ", " + argStr) + ")");
    }

    return "%" + body;
}
