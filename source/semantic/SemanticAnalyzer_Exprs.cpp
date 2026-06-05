#include "SemanticAnalyzer.h"

// ============================================================
// Expression analysis
// ============================================================

Type SemanticAnalyzer::analyzeExpr(const Expr& expr) {
    Type resolvedType = std::visit(overloaded{
        [&](const LiteralExpr& literal)               { return analyzeLiteral(literal); },
        [&](const IdentifierExpr& identifier)         { return analyzeIdentifier(identifier); },
        [&](const UnaryExpr& unary)                   { return analyzeUnary(unary); },
        [&](const BinaryExpr& binary)                 { return analyzeBinary(binary); },
        [&](const AssignExpr& assign)                 { return analyzeAssign(assign); },
        [&](const CompoundAssignExpr& compoundAssign) { return analyzeCompoundAssign(compoundAssign); },
        [&](const PostfixExpr& postfix)               { return analyzePostfix(postfix); },
        [&](const CallExpr& call)                     { return analyzeCall(call); },
        [&](const VarDeclExpr& varDecl)               { return analyzeVarDecl(varDecl); },
        [&](const IndexExpr& indexExpr)               { return analyzeIndex(indexExpr); },
        [&](const IndexAssignExpr& indexAssign)        { return analyzeIndexAssign(indexAssign); },
        [&](const ThisExpr& thisExpr)                 { return analyzeThis(thisExpr); },
        [&](const MemberAccessExpr& ma)               { return analyzeMemberAccess(ma); },
        [&](const MemberAssignExpr& ma)               { return analyzeMemberAssign(ma); },
        [&](const MethodCallExpr& mc)                 { return analyzeMethodCall(mc); },
    }, *expr.node);
    recordType(expr, resolvedType);
    return resolvedType;
}

Type SemanticAnalyzer::analyzeLiteral(const LiteralExpr& literal) {
    switch (literal.token.type) {
        case TokenType::TRUE:
        case TokenType::FALSE:
            return Type{TypeKind::Bool};
        case TokenType::NUMBER:
            // Decimal point present → floating-point literal → f64
            if (literal.token.lexeme.find('.') != std::string::npos)
                return Type{TypeKind::F64};
            return Type{TypeKind::I32};
        case TokenType::STRING:
            return Type{TypeKind::Ptr};
        case TokenType::CHAR:
            return Type{TypeKind::Char};
        default:
            return Type{TypeKind::Error};
    }
}

Type SemanticAnalyzer::analyzeIdentifier(const IdentifierExpr& identifier) {
    const Symbol* sym = symbolTable.lookup(identifier.name.lexeme);
    if (!sym) {
        error(identifier.name, "use of undeclared identifier '" + identifier.name.lexeme + "'");
        return Type{TypeKind::Error};
    }
    if (sym->kind == Symbol::Kind::Function) {
        error(identifier.name, "cannot use function '" + identifier.name.lexeme + "' as a value");
        return Type{TypeKind::Error};
    }
    return sym->type;
}

Type SemanticAnalyzer::analyzeUnary(const UnaryExpr& unary) {
    Type operandType = analyzeExpr(*unary.operand);

    switch (unary.operatorToken.type) {
        case TokenType::BANG:
            if (!isError(operandType) && !isBoolCompatible(operandType)) {
                error(unary.operatorToken, "operand of '!' must be bool-compatible, got " + typeName(operandType));
                return Type{TypeKind::Error};
            }
            return Type{TypeKind::Bool};

        case TokenType::MINUS:
            if (!isError(operandType) && !isNumeric(operandType.kind)) {
                error(unary.operatorToken, "operand of unary '-' must be numeric, got " + typeName(operandType));
                return Type{TypeKind::Error};
            }
            return operandType;

        case TokenType::TILDE:
            if (!isError(operandType) && !isInteger(operandType.kind)) {
                error(unary.operatorToken, "operand of '~' must be integer, got " + typeName(operandType));
                return Type{TypeKind::Error};
            }
            return operandType;

        case TokenType::INCREMENT:
        case TokenType::DECREMENT:
            if (!std::holds_alternative<IdentifierExpr>(*unary.operand->node)) {
                error(unary.operatorToken, "operand of '" + unary.operatorToken.lexeme + "' must be an identifier");
                return Type{TypeKind::Error};
            }
            if (!isError(operandType) && !isNumeric(operandType.kind)) {
                error(unary.operatorToken, "operand of '" + unary.operatorToken.lexeme + "' must be numeric, got "
                      + typeName(operandType));
                return Type{TypeKind::Error};
            }
            return operandType;

        default:
            return Type{TypeKind::Error};
    }
}

Type SemanticAnalyzer::analyzeBinary(const BinaryExpr& binary) {
    Type leftType  = analyzeExpr(*binary.left);
    Type rightType = analyzeExpr(*binary.right);

    if (isError(leftType) || isError(rightType)) return Type{TypeKind::Error};

    switch (binary.operatorToken.type) {
        // Arithmetic
        case TokenType::PLUS:
        case TokenType::MINUS:
        case TokenType::STAR:
        case TokenType::SLASH:
        case TokenType::PERCENT: {
            if (!isNumeric(leftType.kind)) {
                error(binary.operatorToken, "left operand of '" + binary.operatorToken.lexeme + "' must be numeric, got "
                      + typeName(leftType));
                return Type{TypeKind::Error};
            }
            if (!isNumeric(rightType.kind)) {
                error(binary.operatorToken, "right operand of '" + binary.operatorToken.lexeme + "' must be numeric, got "
                      + typeName(rightType));
                return Type{TypeKind::Error};
            }
            return commonArithmeticType(leftType, rightType);
        }

        // Bitwise
        case TokenType::PIPE:
        case TokenType::CARET:
        case TokenType::AMPERSAND:
        case TokenType::SHIFT_LEFT:
        case TokenType::SHIFT_RIGHT: {
            if (!isInteger(leftType.kind)) {
                error(binary.operatorToken, "left operand of '" + binary.operatorToken.lexeme + "' must be integer, got "
                      + typeName(leftType));
                return Type{TypeKind::Error};
            }
            if (!isInteger(rightType.kind)) {
                error(binary.operatorToken, "right operand of '" + binary.operatorToken.lexeme + "' must be integer, got "
                      + typeName(rightType));
                return Type{TypeKind::Error};
            }
            return commonArithmeticType(leftType, rightType);
        }

        // Ordering comparisons
        case TokenType::LESS:
        case TokenType::LESS_EQUAL:
        case TokenType::GREATER:
        case TokenType::GREATER_EQUAL: {
            if (!isNumeric(leftType.kind))
                error(binary.operatorToken, "left operand of '" + binary.operatorToken.lexeme + "' must be numeric, got "
                      + typeName(leftType));
            if (!isNumeric(rightType.kind))
                error(binary.operatorToken, "right operand of '" + binary.operatorToken.lexeme + "' must be numeric, got "
                      + typeName(rightType));
            return Type{TypeKind::Bool};
        }

        // Equality comparisons
        case TokenType::EQUAL_EQUAL:
        case TokenType::BANG_EQUAL: {
            bool compatible =
                (leftType.kind == rightType.kind) ||
                (isNumeric(leftType.kind) && isNumeric(rightType.kind));
            if (!compatible) {
                error(binary.operatorToken, "incompatible types for '" + binary.operatorToken.lexeme + "': "
                      + typeName(leftType) + " and " + typeName(rightType));
            }
            return Type{TypeKind::Bool};
        }

        // Logical
        case TokenType::AND:
        case TokenType::OR: {
            if (!isBoolCompatible(leftType))
                error(binary.operatorToken, "left operand of '" + binary.operatorToken.lexeme
                      + "' must be bool-compatible, got " + typeName(leftType));
            if (!isBoolCompatible(rightType))
                error(binary.operatorToken, "right operand of '" + binary.operatorToken.lexeme
                      + "' must be bool-compatible, got " + typeName(rightType));
            return Type{TypeKind::Bool};
        }

        default:
            return Type{TypeKind::Error};
    }
}

Type SemanticAnalyzer::analyzeAssign(const AssignExpr& assign) {
    const Symbol* sym = symbolTable.lookup(assign.name.lexeme);
    if (!sym) {
        error(assign.name, "use of undeclared identifier '" + assign.name.lexeme + "'");
        analyzeExpr(*assign.value);
        return Type{TypeKind::Error};
    }
    if (sym->kind == Symbol::Kind::Function) {
        error(assign.name, "cannot assign to function '" + assign.name.lexeme + "'");
        analyzeExpr(*assign.value);
        return Type{TypeKind::Error};
    }

    Type lhsType = sym->type;
    Type rhsType = analyzeExpr(*assign.value);
    checkCast(rhsType, lhsType, assign.name, "assignment");
    return lhsType;
}

Type SemanticAnalyzer::analyzeCompoundAssign(const CompoundAssignExpr& compoundAssign) {
    const Symbol* sym = symbolTable.lookup(compoundAssign.name.lexeme);
    if (!sym) {
        error(compoundAssign.name, "use of undeclared identifier '" + compoundAssign.name.lexeme + "'");
        analyzeExpr(*compoundAssign.value);
        return Type{TypeKind::Error};
    }
    if (sym->kind == Symbol::Kind::Function) {
        error(compoundAssign.name, "cannot assign to function '" + compoundAssign.name.lexeme + "'");
        analyzeExpr(*compoundAssign.value);
        return Type{TypeKind::Error};
    }

    Type lhsType = sym->type;
    Type rhsType = analyzeExpr(*compoundAssign.value);

    if (isError(lhsType) || isError(rhsType)) return Type{TypeKind::Error};

    bool isArith =
        compoundAssign.operatorToken.type == TokenType::PLUS_EQUAL   ||
        compoundAssign.operatorToken.type == TokenType::MINUS_EQUAL  ||
        compoundAssign.operatorToken.type == TokenType::STAR_EQUAL   ||
        compoundAssign.operatorToken.type == TokenType::SLASH_EQUAL  ||
        compoundAssign.operatorToken.type == TokenType::PERCENT_EQUAL;

    bool isBitw =
        compoundAssign.operatorToken.type == TokenType::CARET_EQUAL     ||
        compoundAssign.operatorToken.type == TokenType::AMPERSAND_EQUAL ||
        compoundAssign.operatorToken.type == TokenType::PIPE_EQUAL;

    if (isArith) {
        if (!isNumeric(lhsType.kind))
            error(compoundAssign.operatorToken, "left operand of '" + compoundAssign.operatorToken.lexeme
                  + "' must be numeric, got " + typeName(lhsType));
        if (!isNumeric(rhsType.kind))
            error(compoundAssign.operatorToken, "right operand of '" + compoundAssign.operatorToken.lexeme
                  + "' must be numeric, got " + typeName(rhsType));
    } else if (isBitw) {
        if (!isInteger(lhsType.kind))
            error(compoundAssign.operatorToken, "left operand of '" + compoundAssign.operatorToken.lexeme
                  + "' must be integer, got " + typeName(lhsType));
        if (!isInteger(rhsType.kind))
            error(compoundAssign.operatorToken, "right operand of '" + compoundAssign.operatorToken.lexeme
                  + "' must be integer, got " + typeName(rhsType));
    }

    checkCast(rhsType, lhsType, compoundAssign.operatorToken, "compound assignment");
    return lhsType;
}

Type SemanticAnalyzer::analyzePostfix(const PostfixExpr& postfix) {
    Type operandType = analyzeExpr(*postfix.operand);

    if (!std::holds_alternative<IdentifierExpr>(*postfix.operand->node)) {
        error(postfix.operatorToken, "operand of '" + postfix.operatorToken.lexeme + "' must be an identifier");
        return Type{TypeKind::Error};
    }
    if (!isError(operandType) && !isNumeric(operandType.kind)) {
        error(postfix.operatorToken, "operand of '" + postfix.operatorToken.lexeme + "' must be numeric, got "
              + typeName(operandType));
        return Type{TypeKind::Error};
    }
    return operandType;
}

Type SemanticAnalyzer::analyzeCall(const CallExpr& call) {
    // Constructor call: callee is a class name
    if (classRegistry_.count(call.callee.lexeme)) {
        const ClassInfo& cls = classRegistry_.at(call.callee.lexeme);
        auto ctorIt = cls.methods.find(call.callee.lexeme);
        if (ctorIt == cls.methods.end()) {
            // No explicit constructor — only allow zero-arg call
            if (!call.args.empty()) {
                error(call.callee, "class '" + call.callee.lexeme
                      + "' has no constructor but was called with arguments");
            }
            for (const auto& arg : call.args) analyzeExpr(*arg);
            return makeObjectType(call.callee.lexeme);
        }
        const ClassInfo::Method& ctor = ctorIt->second;
        if (call.args.size() != ctor.paramTypes.size()) {
            error(call.callee, "constructor '" + call.callee.lexeme + "' expects "
                  + std::to_string(ctor.paramTypes.size()) + " argument(s), got "
                  + std::to_string(call.args.size()));
            for (const auto& arg : call.args) analyzeExpr(*arg);
            return makeObjectType(call.callee.lexeme);
        }
        analyzeCallArgs(call.args, ctor.paramTypes, call.callee,
                        "constructor '" + call.callee.lexeme + "'");
        return makeObjectType(call.callee.lexeme);
    }

    const Symbol* sym = symbolTable.lookup(call.callee.lexeme);
    if (!sym) {
        error(call.callee, "undeclared function '" + call.callee.lexeme + "'");
        for (const auto& arg : call.args) analyzeExpr(*arg);
        return Type{TypeKind::Error};
    }
    if (sym->kind != Symbol::Kind::Function) {
        error(call.callee, "'" + call.callee.lexeme + "' is not a function");
        for (const auto& arg : call.args) analyzeExpr(*arg);
        return Type{TypeKind::Error};
    }

    // Check argument count
    if (call.args.size() != sym->paramTypes.size()) {
        error(call.callee, "function '" + call.callee.lexeme + "' expects "
              + std::to_string(sym->paramTypes.size()) + " argument(s), got "
              + std::to_string(call.args.size()));
        for (const auto& arg : call.args) analyzeExpr(*arg);
        return sym->type;
    }

    analyzeCallArgs(call.args, sym->paramTypes, call.callee,
                    "'" + call.callee.lexeme + "'");

    return sym->type;
}

Type SemanticAnalyzer::analyzeVarDecl(const VarDeclExpr& varDecl) {
    Type elementType;
    // If the type token is an IDENTIFIER that names a class, resolve to Object type
    if (varDecl.typeName.type == TokenType::IDENTIFIER
        && classRegistry_.count(varDecl.typeName.lexeme)) {
        elementType = makeObjectType(varDecl.typeName.lexeme);
    } else {
        elementType = typeFromToken(varDecl.typeName.type);
    }
    Type declaredType = varDecl.arraySize > 0
        ? makeArrayType(elementType.kind, varDecl.arraySize)
        : elementType;

    if (elementType.kind == TypeKind::Void) {
        error(varDecl.typeName, "variable '" + varDecl.name.lexeme + "' cannot have type 'void'");
        if (varDecl.initializer) analyzeExpr(*varDecl.initializer);
        return Type{TypeKind::Error};
    }

    // Array initializers are not yet supported
    if (varDecl.arraySize > 0 && varDecl.initializer) {
        error(varDecl.typeName, "array initializers are not yet supported");
        analyzeExpr(*varDecl.initializer);
    }

    // Redeclaration in the same scope?
    const Symbol* existing = symbolTable.lookupCurrentScope(varDecl.name.lexeme);
    if (existing) {
        error(varDecl.name, "variable '" + varDecl.name.lexeme + "' already declared in this scope"
              + " (previously declared at line "
              + std::to_string(existing->declarationToken.line) + ")");
        if (varDecl.initializer && varDecl.arraySize == 0) analyzeExpr(*varDecl.initializer);
        return declaredType;
    }

    // Scalar: analyse initializer
    if (varDecl.arraySize == 0 && varDecl.initializer) {
        Type initializerType = analyzeExpr(*varDecl.initializer);
        checkCast(initializerType, declaredType, varDecl.name, "variable initializer");
    }

    symbolTable.declare(varDecl.name.lexeme, Symbol{
        Symbol::Kind::Variable,
        declaredType,
        varDecl.name,
        {}
    });

    return declaredType;
}

Type SemanticAnalyzer::analyzeIndex(const IndexExpr& indexExpr) {
    const Symbol* sym = symbolTable.lookup(indexExpr.name.lexeme);
    if (!sym) {
        error(indexExpr.name, "use of undeclared identifier '" + indexExpr.name.lexeme + "'");
        analyzeExpr(*indexExpr.index);
        return Type{TypeKind::Error};
    }
    if (sym->type.kind != TypeKind::Array) {
        error(indexExpr.name, "'" + indexExpr.name.lexeme + "' is not an array");
        analyzeExpr(*indexExpr.index);
        return Type{TypeKind::Error};
    }

    // Index must be an integer
    Type indexType = analyzeExpr(*indexExpr.index);
    if (!isError(indexType) && !isInteger(indexType.kind))
        error(indexExpr.name, "array index must be an integer type, got " + typeName(indexType));

    // Compile-time bounds check for constant literal index
    if (!isError(indexType))
        checkConstantIndexBounds(*indexExpr.index, sym->type.arraySize);

    return Type{sym->type.elementKind};
}

Type SemanticAnalyzer::analyzeIndexAssign(const IndexAssignExpr& indexAssign) {
    const Symbol* sym = symbolTable.lookup(indexAssign.name.lexeme);
    if (!sym) {
        error(indexAssign.name, "use of undeclared identifier '" + indexAssign.name.lexeme + "'");
        analyzeExpr(*indexAssign.index);
        analyzeExpr(*indexAssign.value);
        return Type{TypeKind::Error};
    }
    if (sym->type.kind != TypeKind::Array) {
        error(indexAssign.name, "'" + indexAssign.name.lexeme + "' is not an array");
        analyzeExpr(*indexAssign.index);
        analyzeExpr(*indexAssign.value);
        return Type{TypeKind::Error};
    }

    // Validate index type
    Type indexType = analyzeExpr(*indexAssign.index);
    if (!isError(indexType) && !isInteger(indexType.kind))
        error(indexAssign.name, "array index must be an integer type, got " + typeName(indexType));

    // Compile-time bounds check for constant literal index
    if (!isError(indexType))
        checkConstantIndexBounds(*indexAssign.index, sym->type.arraySize);

    // Value type must be assignable to element type
    Type elementType{sym->type.elementKind};
    Type valueType = analyzeExpr(*indexAssign.value);
    checkCast(valueType, elementType, indexAssign.name, "array element assignment");

    return elementType;
}

// ============================================================
// Class expression analysis
// ============================================================

Type SemanticAnalyzer::analyzeThis(const ThisExpr& thisExpr) {
    if (currentClassName_.empty()) {
        error(thisExpr.keyword, "'this' used outside of a class method");
        return Type{TypeKind::Error};
    }
    return makeObjectType(currentClassName_);
}

Type SemanticAnalyzer::analyzeMemberAccess(const MemberAccessExpr& memberAccess) {
    Type objectType = analyzeExpr(*memberAccess.object);
    if (isError(objectType)) return Type{TypeKind::Error};

    const ClassInfo* cls = lookupObjectClass(objectType, memberAccess.field);
    if (!cls) return Type{TypeKind::Error};

    auto fieldIt = cls->fields.find(memberAccess.field.lexeme);
    if (fieldIt == cls->fields.end()) {
        error(memberAccess.field, "class '" + objectType.className
              + "' has no field '" + memberAccess.field.lexeme + "'");
        return Type{TypeKind::Error};
    }

    const ClassInfo::Field& field = fieldIt->second;
    // Access control: private fields only accessible from within the same class
    if (!field.isPublic && currentClassName_ != objectType.className) {
        error(memberAccess.field, "field '" + memberAccess.field.lexeme
              + "' is private in class '" + objectType.className + "'");
    }

    return field.type;
}

Type SemanticAnalyzer::analyzeMemberAssign(const MemberAssignExpr& memberAssign) {
    Type objectType = analyzeExpr(*memberAssign.object);
    if (isError(objectType)) {
        analyzeExpr(*memberAssign.value);
        return Type{TypeKind::Error};
    }

    const ClassInfo* cls = lookupObjectClass(objectType, memberAssign.field);
    if (!cls) {
        analyzeExpr(*memberAssign.value);
        return Type{TypeKind::Error};
    }

    auto fieldIt = cls->fields.find(memberAssign.field.lexeme);
    if (fieldIt == cls->fields.end()) {
        error(memberAssign.field, "class '" + objectType.className
              + "' has no field '" + memberAssign.field.lexeme + "'");
        analyzeExpr(*memberAssign.value);
        return Type{TypeKind::Error};
    }

    const ClassInfo::Field& field = fieldIt->second;
    if (!field.isPublic && currentClassName_ != objectType.className) {
        error(memberAssign.field, "field '" + memberAssign.field.lexeme
              + "' is private in class '" + objectType.className + "'");
    }

    Type valueType = analyzeExpr(*memberAssign.value);
    checkCast(valueType, field.type, memberAssign.field, "field assignment");
    return field.type;
}

Type SemanticAnalyzer::analyzeMethodCall(const MethodCallExpr& methodCall) {
    Type objectType = analyzeExpr(*methodCall.object);
    if (isError(objectType)) {
        for (const auto& arg : methodCall.args) analyzeExpr(*arg);
        return Type{TypeKind::Error};
    }

    const ClassInfo* cls = lookupObjectClass(objectType, methodCall.method);
    if (!cls) {
        for (const auto& arg : methodCall.args) analyzeExpr(*arg);
        return Type{TypeKind::Error};
    }

    auto methodIt = cls->methods.find(methodCall.method.lexeme);
    if (methodIt == cls->methods.end()) {
        error(methodCall.method, "class '" + objectType.className
              + "' has no method '" + methodCall.method.lexeme + "'");
        for (const auto& arg : methodCall.args) analyzeExpr(*arg);
        return Type{TypeKind::Error};
    }

    const ClassInfo::Method& method = methodIt->second;
    if (!method.isPublic && currentClassName_ != objectType.className) {
        error(methodCall.method, "method '" + methodCall.method.lexeme
              + "' is private in class '" + objectType.className + "'");
    }

    if (methodCall.args.size() != method.paramTypes.size()) {
        error(methodCall.method, "method '" + methodCall.method.lexeme + "' expects "
              + std::to_string(method.paramTypes.size()) + " argument(s), got "
              + std::to_string(methodCall.args.size()));
        for (const auto& arg : methodCall.args) analyzeExpr(*arg);
        return method.returnType;
    }

    analyzeCallArgs(methodCall.args, method.paramTypes, methodCall.method,
                    "method '" + methodCall.method.lexeme + "'");
    return method.returnType;
}
