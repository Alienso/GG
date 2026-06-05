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
                                     const std::vector<Type>* declaredParamTypes) {
    std::string argumentString;
    bool   first      = true;
    size_t paramIndex = 0;
    for (const auto& arg : args) {
        if (!first) argumentString += ", ";
        first = false;
        Type        argType = exprType(*arg);
        std::string value   = genExpr(*arg);

        // Cast to the declared parameter type when the IR types differ.
        if (declaredParamTypes && paramIndex < declaredParamTypes->size()) {
            const Type& paramType = (*declaredParamTypes)[paramIndex];
            value   = emitCast(value, argType, paramType);
            argType = paramType;
        }
        ++paramIndex;

        argumentString += irTypeName(argType) + " " + value;
    }
    return argumentString;
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

std::string CodeGen::genIdentifier(const IdentifierExpr& identifier) {
    auto allocaIt  = allocaMap.find(identifier.name.lexeme);
    if (allocaIt == allocaMap.end()) return "0";  // undefined — semantic pass should catch this

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
            std::string value      = genExpr(*unary.operand);
            Type        operandType = exprType(*unary.operand);
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
            auto        varTypeIt = varTypeMap.find(id.name.lexeme);
            auto        allocaIt  = allocaMap.find(id.name.lexeme);
            if (varTypeIt == varTypeMap.end() || allocaIt == allocaMap.end()) return "0";
            Type        variableType = varTypeIt->second;
            std::string irType       = irTypeName(variableType);
            std::string ptrName      = allocaIt->second;
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

std::string CodeGen::genBinary(const BinaryExpr& binary, const Type& resolvedType) {
    TokenType operatorType = binary.operatorToken.type;

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

std::string CodeGen::genAssign(const AssignExpr& assign) {
    auto varTypeIt = varTypeMap.find(assign.name.lexeme);
    auto allocaIt  = allocaMap.find(assign.name.lexeme);
    if (varTypeIt == varTypeMap.end() || allocaIt == allocaMap.end()) return "0";

    Type        lhsType = varTypeIt->second;
    std::string irType  = irTypeName(lhsType);
    std::string ptrName = allocaIt->second;

    Type        rhsType = exprType(*assign.value);
    std::string value   = genExpr(*assign.value);
    value = emitCast(value, rhsType, lhsType);

    emitStore(irType, value, ptrName);
    return value;
}

// ---- CompoundAssign ----

std::string CodeGen::genCompoundAssign(const CompoundAssignExpr& compoundAssign) {
    auto varTypeIt = varTypeMap.find(compoundAssign.name.lexeme);
    auto allocaIt  = allocaMap.find(compoundAssign.name.lexeme);
    if (varTypeIt == varTypeMap.end() || allocaIt == allocaMap.end()) return "0";

    Type        lhsType      = varTypeIt->second;
    std::string irType       = irTypeName(lhsType);
    std::string ptrName      = allocaIt->second;

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
    auto        varTypeIt = varTypeMap.find(id.name.lexeme);
    auto        allocaIt  = allocaMap.find(id.name.lexeme);
    if (varTypeIt == varTypeMap.end() || allocaIt == allocaMap.end()) return "0";

    Type        variableType = varTypeIt->second;
    std::string irType       = irTypeName(variableType);
    std::string ptrName      = allocaIt->second;

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
    auto funcIt = funcParamTypes.find(call.callee.lexeme);
    const std::vector<Type>* declaredParams =
        funcIt != funcParamTypes.end() ? &funcIt->second : nullptr;

    std::string argStr = buildArgString(call.args, declaredParams);

    if (returnIrType == "void") {
        emit("call void @" + call.callee.lexeme + "(" + argStr + ")");
        return "";
    }
    std::string t = freshTemp();
    emit("%" + t + " = call " + returnIrType + " @" + call.callee.lexeme + "(" + argStr + ")");
    return "%" + t;
}

// ---- VarDecl ----

std::string CodeGen::genVarDecl(const VarDeclExpr& varDecl) {
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
            if (cgIt != cgClasses_.end() && cgIt->second.hasDestructor && !dtorScopes_.empty())
                dtorScopes_.back().emplace_back(ptrName, className);
        }

        // If initializer is a constructor CallExpr, emit the constructor call
        if (varDecl.initializer) {
            if (std::holds_alternative<CallExpr>(*varDecl.initializer->node)) {
                const auto& ctorCall = std::get<CallExpr>(*varDecl.initializer->node);
                std::string mangledCtor = className + "_" + className;
                auto funcIt = funcParamTypes.find(mangledCtor);
                const std::vector<Type>* ctorParams =
                    funcIt != funcParamTypes.end() ? &funcIt->second : nullptr;
                std::string argStr = buildArgString(ctorCall.args, ctorParams);
                emit("call void @" + mangledCtor + "(ptr " + ptrName
                     + (argStr.empty() ? "" : ", " + argStr) + ")");
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

// ---- Index (array read) ----

std::string CodeGen::genIndex(const IndexExpr& indexExpr) {
    const std::string& name = indexExpr.name.lexeme;
    auto varTypeIt = varTypeMap.find(name);
    auto allocaIt  = allocaMap.find(name);
    if (varTypeIt == varTypeMap.end() || allocaIt == allocaMap.end()) return "0";

    Type        arrayType     = varTypeIt->second;
    Type        elementType{arrayType.elementKind};
    std::string elementIrType = irTypeName(elementType);
    std::string arrayIrType   = irTypeName(arrayType);
    std::string ptrName       = allocaIt->second;

    // Evaluate and widen the index to i64 for GEP (icmp ult also needs i64)
    Type        indexType  = exprType(*indexExpr.index);
    std::string indexValue = genExpr(*indexExpr.index);
    indexValue = emitCast(indexValue, indexType, Type{TypeKind::I64});

    if (boundsCheck) {
        ensureAbortDeclared();
        emitBoundsCheck(indexValue, arrayType.arraySize);
    }

    std::string elemPtr = freshTemp();
    emit("%" + elemPtr + " = getelementptr " + arrayIrType + ", ptr " + ptrName
         + ", i32 0, i64 " + indexValue);

    return emitLoad(elementIrType, "%" + elemPtr);
}

// ---- IndexAssign (array write) ----

std::string CodeGen::genIndexAssign(const IndexAssignExpr& indexAssign) {
    const std::string& name = indexAssign.name.lexeme;
    auto varTypeIt = varTypeMap.find(name);
    auto allocaIt  = allocaMap.find(name);
    if (varTypeIt == varTypeMap.end() || allocaIt == allocaMap.end()) return "0";

    Type        arrayType     = varTypeIt->second;
    Type        elementType{arrayType.elementKind};
    std::string elementIrType = irTypeName(elementType);
    std::string arrayIrType   = irTypeName(arrayType);
    std::string ptrName       = allocaIt->second;

    // Index (widen to i64)
    Type        indexType  = exprType(*indexAssign.index);
    std::string indexValue = genExpr(*indexAssign.index);
    indexValue = emitCast(indexValue, indexType, Type{TypeKind::I64});

    if (boundsCheck) {
        ensureAbortDeclared();
        emitBoundsCheck(indexValue, arrayType.arraySize);
    }

    std::string elemPtr = freshTemp();
    emit("%" + elemPtr + " = getelementptr " + arrayIrType + ", ptr " + ptrName
         + ", i32 0, i64 " + indexValue);

    // Value (cast to element type)
    Type        valueType = exprType(*indexAssign.value);
    std::string value     = genExpr(*indexAssign.value);
    value = emitCast(value, valueType, elementType);

    emitStore(elementIrType, value, "%" + elemPtr);
    return value;  // assignment expression returns the stored value
}
