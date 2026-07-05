#include "CodeGen.h"
#include <cstdint>

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
                                     const std::vector<const Expr*>* defaults) {
    std::string argumentString;
    bool   first      = true;
    size_t paramIndex = 0;

    auto emitArg = [&](const Expr& arg) {
        if (!first) argumentString += ", ";
        first = false;
        Type        argType = exprType(arg);
        std::string value   = genExpr(arg);
        // Cast to the declared parameter type when the IR types differ.
        if (declaredParamTypes && paramIndex < declaredParamTypes->size()) {
            const Type& paramType = (*declaredParamTypes)[paramIndex];
            value   = emitCast(value, argType, paramType);
            argType = paramType;
        }
        ++paramIndex;
        // Objects pass by reference: `value` is already the object's address.
        argumentString += paramIrType(argType) + " " + value;
    };

    for (const auto& arg : args) emitArg(*arg);

    // Fill omitted trailing parameters by emitting their default-value expressions (evaluated
    // here, at the call site). The default nodes come from the callee's declaration.
    if (defaults && declaredParamTypes) {
        for (size_t i = args.size(); i < declaredParamTypes->size(); ++i) {
            if (i < defaults->size() && (*defaults)[i]) emitArg(*(*defaults)[i]);
        }
    }
    return argumentString;
}

const std::vector<const Expr*>* CodeGen::defaultsFor(const std::string& emittedName) const {
    auto it = funcDefaults_.find(emittedName);
    return it != funcDefaults_.end() ? &it->second : nullptr;
}

// ============================================================
// Expression codegen
// ============================================================

std::string CodeGen::genExpr(const Expr& expr) {
    Type resolvedType = exprType(expr);
    return std::visit(overloaded{
        [&](const LiteralExpr& literal)              -> std::string { return genLiteral(literal, resolvedType); },
        [&](const IdentifierExpr& identifier)        -> std::string { return genIdentifier(identifier); },
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
        [&](const MemberAccessExpr& memberAccess)    -> std::string { return genMemberAccess(memberAccess); },
        [&](const MemberAssignExpr& memberAssign)    -> std::string { return genMemberAssign(memberAssign); },
        [&](const MethodCallExpr& methodCall)        -> std::string { return genMethodCall(methodCall, resolvedType); },
        [&](const CastExpr& castExpr)                -> std::string { return genCast(castExpr, resolvedType); },
        [&](const NewExpr& newExpr)                  -> std::string { return genNew(newExpr, resolvedType); },
        [&](const SizeofExpr& sizeofExpr)            -> std::string { return genSizeof(sizeofExpr); },
    }, *expr.node);
}

// ---- Literal ----

std::string CodeGen::genLiteral(const LiteralExpr& literal, const Type& resolvedType) {
    const std::string& lexeme = literal.token.lexeme;

    switch (literal.token.type) {
        case TokenType::NUMBER: {
            if (lexeme.find('.') != std::string::npos) {
                // Float literal — ensure at least one digit after '.'
                std::string value = lexeme;
                if (!value.empty() && value.back() == '.') value += '0';
                return value;
            }
            // Integer literal
            return lexeme;
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

std::string CodeGen::genIdentifier(const IdentifierExpr& identifier) {
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
            std::string tempName   = freshTemp();
            if (isFloat(operandType.kind))
                emit("%" + tempName + " = fneg " + irType + " " + value);
            else
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

std::string CodeGen::genBinary(const BinaryExpr& binary, const Type& resolvedType) {
    TokenType operatorType = binary.operatorToken.type;

    // Operator overloading: a class-typed left operand desugars to a trait method call.
    {
        Type leftType = exprType(*binary.left);
        const char* method = binaryOperatorMethod(operatorType);
        // A reference `==`/`!=` with no `Eq` impl is address identity (recorded by semantics) —
        // skip the trait desugar and fall through to the pointer-compare path below.
        bool isAddrIdentity = addressIdentityCmp_ && addressIdentityCmp_->count(&binary);
        // A value-object `==`/`!=` with no `Eq` impl → generated memberwise structural equality.
        if (structuralValueCmp_ && structuralValueCmp_->count(&binary)) {
            std::string a = genExpr(*binary.left);   // both operands are addresses of the struct
            std::string b = genExpr(*binary.right);  // (value → alloca/GEP; reference → heap body)
            structEqNeeded_.insert(leftType.className);
            std::string eq = freshTemp();
            emit("%" + eq + " = call i1 @" + leftType.className + "_structeq(ptr " + a + ", ptr " + b + ")");
            if (operatorType == TokenType::BANG_EQUAL) {
                std::string t = freshTemp();
                emit("%" + t + " = xor i1 %" + eq + ", true");
                return "%" + t;
            }
            return "%" + eq;
        }
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

    Type leftType  = exprType(*binary.left);
    Type rightType = exprType(*binary.right);

    // Identity comparison (==/!=) lowering to `icmp eq/ne ptr`: enum singletons, or two class
    // references with no `Eq` impl (address identity — recorded by semantics).
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

    // Cast operands to the common type
    leftValue  = emitCast(leftValue,  leftType,  operandType);
    rightValue = emitCast(rightValue, rightType, operandType);

    std::string irType   = irTypeName(operandType);
    std::string tempName = freshTemp();

    if (isComparison) {
        std::string comparisonInstruction = cmpInstr(operatorType, operandType);
        emit("%" + tempName + " = " + comparisonInstruction + " " + irType + " " + leftValue + ", " + rightValue);
    } else {
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

std::string CodeGen::genAssign(const AssignExpr& assign) {
    Type        lhsType;
    std::string ptrName;
    if (!resolveAssignTarget(assign.name.lexeme, ptrName, lhsType)) return "0";
    std::string irType  = irTypeName(lhsType);

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

    // Compute new value
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
    std::string returnIrType = irTypeName(resolvedType);

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
            emitSretCall(fn, call.args, tmp, recv);
            return tmp;
        }
    }

    // Implicit `this`: a bare call with no matching free function targets a method of the
    // enclosing class (free functions take priority — mirrors the semantic resolution).
    if (!freeFnBases_.count(call.callee.lexeme) && !currentClassName_.empty()) {
        std::string mName = calleeName(&call, currentClassName_ + "_" + call.callee.lexeme);
        auto mp = funcParamTypes.find(mName);
        if (mp != funcParamTypes.end()) {
            std::string argStr = buildArgString(call.args, &mp->second, defaultsFor(mName));
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
            if (resolvedType.kind == TypeKind::Reference)
                pendingTemps_.push_back({ "%" + t, resolvedType.className });
            return "%" + t;
        }
    }

    // Free-function call — use the resolved (possibly overload-mangled) symbol name.
    std::string fnName = calleeName(&call, call.callee.lexeme);
    auto funcIt = funcParamTypes.find(fnName);
    const std::vector<Type>* declaredParams =
        funcIt != funcParamTypes.end() ? &funcIt->second : nullptr;

    std::string argStr = buildArgString(call.args, declaredParams, defaultsFor(fnName));

    if (returnIrType == "void") {
        emit("call void @" + fnName + "(" + argStr + ")");
        return "";
    }
    std::string t = freshTemp();
    emit("%" + t + " = call " + returnIrType + " @" + fnName + "(" + argStr + ")");
    if (resolvedType.kind == TypeKind::Reference)   // a reference-returning call hands back a +1
        pendingTemps_.push_back({ "%" + t, resolvedType.className });
    return "%" + t;
}

// ---- Return-slot (sret) call emission ----

void CodeGen::emitSretCall(const std::string& fn,
                           const std::vector<std::unique_ptr<Expr>>& args,
                           const std::string& slotPtr, const std::string& recvPtr) {
    auto it = funcParamTypes.find(fn);
    std::string argStr = buildArgString(args, it != funcParamTypes.end() ? &it->second : nullptr,
                                        defaultsFor(fn));
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
                emitSretCall(mName, call.args, slotPtr, recv);
                return true;
            }
        }
        std::string fnName = calleeName(&call, call.callee.lexeme);
        if (!slotReturningFns_.count(fnName)) return false;
        emitSretCall(fnName, call.args, slotPtr, "");
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
                emitSretCall(mName, mc.args, slotPtr, "");
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
        emitSretCall(mName, mc.args, slotPtr, recv);
        return true;
    }

    return false;
}

// ---- VarDecl ----

std::string CodeGen::genVarDecl(const VarDeclExpr& varDecl) {
    // ---- C-style static local (persistent global) ----
    // Semantics guarantee a scalar primitive type here.
    if (varDecl.isStatic) return genStaticLocal(varDecl);

    // ---- Array declaration ----
    if (varDecl.arraySize > 0) {
        TypeKind elementKind = typeFromToken(varDecl.typeName.type).kind;
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

    // ---- Reference (Class&) declaration ----
    if (varDecl.typeName.type == TokenType::IDENTIFIER
        && !varDecl.typeName.lexeme.empty() && varDecl.typeName.lexeme.back() == '&') {
        usesRefcount_ = true;
        std::string className = varDecl.typeName.lexeme.substr(0, varDecl.typeName.lexeme.size() - 1);
        Type        refType   = makeReferenceType(className);
        std::string name      = varDecl.name.lexeme;
        std::string ptrName   = freshAllocaName(name);

        emitAlloca(ptrName, "ptr");
        allocaMap[name]  = ptrName;
        varTypeMap[name] = refType;

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

    // ---- Typed raw pointer (ptr<T>) declaration ----
    {
        Type synth = decodeSynthesizedType(varDecl.typeName);
        if (synth.kind == TypeKind::TypedPtr) {
            std::string name    = varDecl.name.lexeme;
            std::string ptrName = freshAllocaName(name);
            emitAlloca(ptrName, "ptr");
            allocaMap[name]  = ptrName;
            varTypeMap[name] = synth;
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

    // ---- Enum variable declaration ----
    // An enum variable holds a `ptr` to a global singleton variant.
    if (varDecl.typeName.type == TokenType::IDENTIFIER
        && cgEnumNames_.count(varDecl.typeName.lexeme)) {
        const std::string& enumName = varDecl.typeName.lexeme;
        Type        enumType = makeEnumType(enumName);
        std::string name     = varDecl.name.lexeme;
        std::string ptrName  = freshAllocaName(name);

        emitAlloca(ptrName, "ptr");
        allocaMap[name]  = ptrName;
        varTypeMap[name] = enumType;

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

    // ---- Object (class) declaration ----
    if (varDecl.typeName.type == TokenType::IDENTIFIER) {
        // Class type — varDecl.typeName.lexeme is the class name
        const std::string& className  = varDecl.typeName.lexeme;
        Type               objectType = makeObjectType(className);
        std::string        name       = varDecl.name.lexeme;

        std::string ptrName = freshAllocaName(name);

        emitAlloca(ptrName, "%" + className);
        allocaMap[name]  = ptrName;
        varTypeMap[name] = objectType;

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
                const std::vector<Type>* ctorParams =
                    funcIt != funcParamTypes.end() ? &funcIt->second : nullptr;
                std::string argStr = buildArgString(ctorCall.args, ctorParams, defaultsFor(mangledCtor));
                emit("call void @" + mangledCtor + "(ptr " + ptrName
                     + (argStr.empty() ? "" : ", " + argStr) + ")");
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
    Type        declaredType = typeFromToken(varDecl.typeName.type);
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
    Type        declaredType = typeFromToken(varDecl.typeName.type);
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

    // Numeric / bool / char conversions — emitCast covers all remaining cases.
    return emitCast(value, fromType, toType);
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
        std::string argStr = buildArgString(newExpr.args, &funcIt->second, defaultsFor(mangledCtor));
        emit("call void @" + mangledCtor + "(ptr %" + body
             + (argStr.empty() ? "" : ", " + argStr) + ")");
    }

    return "%" + body;
}
