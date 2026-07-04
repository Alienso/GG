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
        [&](const CastExpr& castExpr)                 { return analyzeCast(castExpr); },
        [&](const NewExpr& newExpr)                   { return analyzeNew(newExpr); },
        [&](const SizeofExpr&)                        { return Type{TypeKind::U64}; },
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

const ClassInfo::Field* SemanticAnalyzer::currentInstanceField(const std::string& name) const {
    if (currentMethodIsStatic || currentClassName.empty()) return nullptr;
    auto cit = classRegistry.find(currentClassName);
    if (cit == classRegistry.end()) return nullptr;
    auto fit = cit->second.fields.find(name);
    return fit == cit->second.fields.end() ? nullptr : &fit->second;
}

const Type* SemanticAnalyzer::currentStaticFieldType(const std::string& name) const {
    if (currentClassName.empty()) return nullptr;
    auto cit = classRegistry.find(currentClassName);
    if (cit == classRegistry.end()) return nullptr;
    auto sfit = cit->second.staticFields.find(name);
    return sfit == cit->second.staticFields.end() ? nullptr : &sfit->second.type;
}

const ClassInfo::Method* SemanticAnalyzer::currentClassMethod(const std::string& name) const {
    if (currentClassName.empty()) return nullptr;
    auto cit = classRegistry.find(currentClassName);
    if (cit == classRegistry.end()) return nullptr;
    auto mit = cit->second.methods.find(name);
    return mit == cit->second.methods.end() ? nullptr : &mit->second;
}

Type SemanticAnalyzer::analyzeIdentifier(const IdentifierExpr& identifier) {
    const Symbol* sym = symbolTable.lookup(identifier.name.lexeme);
    if (!sym) {
        // Implicit `this`: a bare name may refer to a member of the enclosing class
        // (lowest priority — only when no local/param/function shadows it).
        if (const ClassInfo::Field* f = currentInstanceField(identifier.name.lexeme))
            return f->type;
        if (const Type* sft = currentStaticFieldType(identifier.name.lexeme))
            return *sft;
        error(identifier.name, "use of undeclared identifier '" + identifier.name.lexeme + "'");
        return Type{TypeKind::Error};
    }
    if (sym->kind == Symbol::Kind::Function) {
        error(identifier.name, "cannot use function '" + identifier.name.lexeme + "' as a value");
        return Type{TypeKind::Error};
    }
    if (!sym->isInitialized) {
        error(identifier.name, "variable '" + identifier.name.lexeme
              + "' is used before it has been assigned a value");
        // Return the declared type anyway so downstream analysis uses the right type
        // and does not cascade into spurious "undeclared identifier" errors.
    }
    return sym->type;
}

bool SemanticAnalyzer::incDecTargetOk(const Token& op, const std::string& name) {
    if (const Symbol* sym = symbolTable.lookup(name)) {
        if (sym->kind == Symbol::Kind::Variable && !sym->isMutable) {
            error(op, "cannot mutate immutable variable '" + name + "'; declare it 'mut' to allow mutation");
            return false;
        }
        return true;
    }
    // Implicit `this` field: `++`/`--` always mutates → needs a mut field in a mut method.
    if (const ClassInfo::Field* f = currentInstanceField(name)) {
        if (!f->isMut) {
            error(op, "cannot mutate immutable field '" + name + "'; declare it 'mut'");
            return false;
        }
        if (!currentThisMutable) {
            error(op, "cannot write to field '" + name + "' in a read-only method; declare the method 'mut'");
            return false;
        }
    }
    // Static field → mutable; not-a-field → analyzeExpr(operand) already reported it.
    return true;
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
        case TokenType::DECREMENT: {
            if (!std::holds_alternative<IdentifierExpr>(*unary.operand->node)) {
                error(unary.operatorToken, "operand of '" + unary.operatorToken.lexeme + "' must be an identifier");
                return Type{TypeKind::Error};
            }
            if (!isError(operandType) && !isNumeric(operandType.kind)) {
                error(unary.operatorToken, "operand of '" + unary.operatorToken.lexeme + "' must be numeric, got "
                      + typeName(operandType));
                return Type{TypeKind::Error};
            }
            // '++'/'--' mutate an existing value, so the target must be `mut`.
            const auto& ident = std::get<IdentifierExpr>(*unary.operand->node);
            if (!incDecTargetOk(unary.operatorToken, ident.name.lexeme))
                return Type{TypeKind::Error};
            return operandType;
        }

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
            // Enum identity comparison: both operands must be the same enum type.
            if (leftType.kind == TypeKind::Enum || rightType.kind == TypeKind::Enum) {
                bool sameEnum = leftType.kind == TypeKind::Enum
                             && rightType.kind == TypeKind::Enum
                             && leftType.className == rightType.className;
                if (!sameEnum) {
                    error(binary.operatorToken, "incompatible types for '" + binary.operatorToken.lexeme + "': "
                          + typeName(leftType) + " and " + typeName(rightType));
                }
                return Type{TypeKind::Bool};
            }
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
        // Implicit `this`: a bare name may be a field of the enclosing class.
        if (const ClassInfo::Field* f = currentInstanceField(assign.name.lexeme)) {
            if (!f->isMut && !inConstructor) {
                error(assign.name, "cannot assign to immutable field '" + assign.name.lexeme
                      + "'; declare it 'mut'");
                analyzeExpr(*assign.value);
                return f->type;
            }
            if (!currentThisMutable) {
                error(assign.name, "cannot write to field '" + assign.name.lexeme
                      + "' in a read-only method; declare the method 'mut'");
                analyzeExpr(*assign.value);
                return f->type;
            }
            Type rhs = analyzeExpr(*assign.value);
            checkCast(rhs, f->type, assign.name, "field assignment");
            return f->type;
        }
        if (const Type* sft = currentStaticFieldType(assign.name.lexeme)) {
            Type rhs = analyzeExpr(*assign.value);
            checkCast(rhs, *sft, assign.name, "static field assignment");
            return *sft;
        }
        error(assign.name, "use of undeclared identifier '" + assign.name.lexeme + "'");
        analyzeExpr(*assign.value);
        return Type{TypeKind::Error};
    }
    if (sym->kind == Symbol::Kind::Function) {
        error(assign.name, "cannot assign to function '" + assign.name.lexeme + "'");
        analyzeExpr(*assign.value);
        return Type{TypeKind::Error};
    }

    // Object/reference parameters may not be *rebound* (obj = ...), even when declared
    // `mut` — a reference parameter is a borrow, so rebinding it would corrupt refcounts.
    // (`mut` on such a parameter only unlocks writes to the object's mut fields.)
    if (sym->isParameter
        && (sym->type.kind == TypeKind::Object || sym->type.kind == TypeKind::Reference)) {
        std::string kindWord = sym->type.kind == TypeKind::Object ? "object" : "reference";
        error(assign.name, "cannot rebind " + kindWord + " parameter '" + assign.name.lexeme
              + "': it is a borrow, not an owning binding");
        analyzeExpr(*assign.value);
        return sym->type;
    }

    // Const bindings permit exactly one defining assignment: allowed only while the
    // variable is not yet initialized. `mut` bindings may be reassigned freely.
    if (!sym->isMutable && sym->isInitialized) {
        error(assign.name, "cannot reassign immutable variable '" + assign.name.lexeme
              + "'; declare it 'mut' to allow reassignment");
        analyzeExpr(*assign.value);
        return sym->type;
    }

    Type lhsType = sym->type;
    Type rhsType = analyzeExpr(*assign.value);
    checkCast(rhsType, lhsType, assign.name, "assignment");
    // Rebinding a `mut` reference from a read-only reference is a const→mut coercion.
    if (sym->isMutable)
        warnConstToMut(assign.name, *assign.value, lhsType);
    // Any successful assignment makes the variable definitely initialized.
    if (Symbol* mut = symbolTable.lookupMutable(assign.name.lexeme))
        mut->isInitialized = true;
    return lhsType;
}

Type SemanticAnalyzer::analyzeCompoundAssign(const CompoundAssignExpr& compoundAssign) {
    const Symbol* sym = symbolTable.lookup(compoundAssign.name.lexeme);
    Type lhsType;
    if (sym) {
        if (sym->kind == Symbol::Kind::Function) {
            error(compoundAssign.name, "cannot assign to function '" + compoundAssign.name.lexeme + "'");
            analyzeExpr(*compoundAssign.value);
            return Type{TypeKind::Error};
        }
        // Compound assignment always mutates an existing value, so the target must be `mut`.
        if (!sym->isMutable) {
            error(compoundAssign.name, "cannot mutate immutable variable '" + compoundAssign.name.lexeme
                  + "'; declare it 'mut' to allow mutation");
            analyzeExpr(*compoundAssign.value);
            return sym->type;
        }
        // Compound assignment reads the variable before writing — check initialization.
        if (!sym->isInitialized) {
            error(compoundAssign.name, "variable '" + compoundAssign.name.lexeme
                  + "' is used before it has been assigned a value");
        }
        lhsType = sym->type;
    } else if (const ClassInfo::Field* f = currentInstanceField(compoundAssign.name.lexeme)) {
        // Implicit `this.field op= v` — always mutates, so needs a mut field in a mut method.
        if (!f->isMut) {
            error(compoundAssign.name, "cannot mutate immutable field '" + compoundAssign.name.lexeme
                  + "'; declare it 'mut'");
            analyzeExpr(*compoundAssign.value);
            return f->type;
        }
        if (!currentThisMutable) {
            error(compoundAssign.name, "cannot write to field '" + compoundAssign.name.lexeme
                  + "' in a read-only method; declare the method 'mut'");
            analyzeExpr(*compoundAssign.value);
            return f->type;
        }
        lhsType = f->type;
    } else if (const Type* sft = currentStaticFieldType(compoundAssign.name.lexeme)) {
        lhsType = *sft;
    } else {
        error(compoundAssign.name, "use of undeclared identifier '" + compoundAssign.name.lexeme + "'");
        analyzeExpr(*compoundAssign.value);
        return Type{TypeKind::Error};
    }
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
    // Compound assignment writes to the variable — mark it as definitely initialized.
    if (Symbol* mut = symbolTable.lookupMutable(compoundAssign.name.lexeme))
        mut->isInitialized = true;
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
    // Postfix '++'/'--' mutate an existing value, so the target must be `mut`.
    const auto& ident = std::get<IdentifierExpr>(*postfix.operand->node);
    if (!incDecTargetOk(postfix.operatorToken, ident.name.lexeme))
        return Type{TypeKind::Error};
    // Postfix writes back to the variable — mark as initialized to suppress
    // cascading "uninitialized" errors on subsequent reads.
    if (Symbol* mut = symbolTable.lookupMutable(ident.name.lexeme))
        mut->isInitialized = true;
    return operandType;
}

Type SemanticAnalyzer::analyzeCall(const CallExpr& call) {
    // Enums cannot be constructed directly — variants are the only instances.
    if (enumRegistry.count(call.callee.lexeme)) {
        error(call.callee, "cannot construct enum '" + call.callee.lexeme
              + "' directly; use one of its variants");
        for (const auto& arg : call.args) analyzeExpr(*arg);
        return makeEnumType(call.callee.lexeme);
    }
    // Constructor call: callee is a class name
    if (classRegistry.count(call.callee.lexeme)) {
        const ClassInfo& cls = classRegistry.at(call.callee.lexeme);
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
                        "constructor '" + call.callee.lexeme + "'", ctor.paramMut);
        return makeObjectType(call.callee.lexeme);
    }

    const Symbol* sym = symbolTable.lookup(call.callee.lexeme);
    if (!sym) {
        // Implicit `this`: a bare call may target a method of the enclosing class
        // (only when no local/parameter/free-function shadows the name).
        if (const ClassInfo::Method* m = currentClassMethod(call.callee.lexeme)) {
            if (!m->isStatic && currentMethodIsStatic) {
                error(call.callee, "cannot call instance method '" + call.callee.lexeme
                      + "' from a static method");
                for (const auto& arg : call.args) analyzeExpr(*arg);
                return m->returnType;
            }
            if (m->isMut && !currentThisMutable) {
                error(call.callee, "cannot call mutating method '" + call.callee.lexeme
                      + "' on 'this' in a read-only method; declare the calling method 'mut'");
                for (const auto& arg : call.args) analyzeExpr(*arg);
                return m->returnType;
            }
            if (call.args.size() != m->paramTypes.size()) {
                error(call.callee, "method '" + call.callee.lexeme + "' expects "
                      + std::to_string(m->paramTypes.size()) + " argument(s), got "
                      + std::to_string(call.args.size()));
                for (const auto& arg : call.args) analyzeExpr(*arg);
                return m->returnType;
            }
            analyzeCallArgs(call.args, m->paramTypes, call.callee,
                            "method '" + call.callee.lexeme + "'", m->paramMut);
            return m->returnType;
        }
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
                    "'" + call.callee.lexeme + "'", sym->paramMut);

    return sym->type;
}

bool SemanticAnalyzer::isConstantExpr(const Expr& expr) {
    return std::visit(overloaded{
        [](const LiteralExpr&)            { return true; },
        [](const UnaryExpr& u)            { return isConstantExpr(*u.operand); },
        [](const BinaryExpr& b)           { return isConstantExpr(*b.left) && isConstantExpr(*b.right); },
        [](const CastExpr& c)             { return isConstantExpr(*c.operand); },
        [](const auto&)                   { return false; },
    }, *expr.node);
}

Type SemanticAnalyzer::analyzeVarDecl(const VarDeclExpr& varDecl) {
    // Resolve the declared type — handles class names (Object) and Class& (Reference).
    Type elementType = resolveTypeToken(varDecl.typeName);
    Type declaredType = varDecl.arraySize > 0
        ? makeArrayType(elementType.kind, varDecl.arraySize)
        : elementType;

    if (elementType.kind == TypeKind::Void) {
        error(varDecl.typeName, "variable '" + varDecl.name.lexeme + "' cannot have type 'void'");
        if (varDecl.initializer) analyzeExpr(*varDecl.initializer);
        return Type{TypeKind::Error};
    }

    // C-style static local: persistent single-storage variable. Phase 3 supports
    // scalar primitive (numeric / bool / char) static locals with a constant
    // initializer that runs once before main.
    if (varDecl.isStatic) {
        bool primitive = varDecl.arraySize == 0
                      && (isNumeric(elementType.kind)
                          || elementType.kind == TypeKind::Bool
                          || elementType.kind == TypeKind::Char);
        if (!primitive)
            error(varDecl.typeName, "static local variable '" + varDecl.name.lexeme
                  + "' must have a primitive type (numeric, bool or char)");
        if (varDecl.initializer && !isConstantExpr(*varDecl.initializer))
            error(varDecl.name, "static local variable '" + varDecl.name.lexeme
                  + "' requires a constant initializer");
    }

    // Raw pointer types are gated behind --unsafe-ptr.
    checkRawPtrAllowed(varDecl.typeName, varDecl.name);

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
        // Initialising a `mut` reference from a read-only reference is a const→mut coercion.
        if (varDecl.isMut)
            warnConstToMut(varDecl.name, *varDecl.initializer, declaredType);
    }

    // Decide whether the variable starts as definitely initialized:
    //   - explicit initializer present                → yes
    //   - Object (class value): zero-initialized struct → yes
    //   - Array: zero-initialized by the runtime       → yes
    //   - Everything else (primitives, references)     → no (must be assigned before use)
    bool isInit = varDecl.initializer != nullptr
               || elementType.kind == TypeKind::Object
               || varDecl.arraySize > 0
               || varDecl.isStatic;   // static locals are zero-initialised storage

    symbolTable.declare(varDecl.name.lexeme, Symbol{
        Symbol::Kind::Variable,
        declaredType,
        varDecl.name,
        {},
        /*isParameter=*/false,
        /*isInitialized=*/isInit,
        /*isMutable=*/varDecl.isMut
    });

    return declaredType;
}

Type SemanticAnalyzer::analyzeIndex(const IndexExpr& indexExpr) {
    const Token& site = exprFirstToken(*indexExpr.object);
    Type objectType   = analyzeExpr(*indexExpr.object);

    // Index must be an integer (validate before bailing on object errors).
    Type indexType = analyzeExpr(*indexExpr.index);
    if (!isError(indexType) && !isInteger(indexType.kind))
        error(site, "index must be an integer type, got " + typeName(indexType));

    if (isError(objectType)) return Type{TypeKind::Error};

    if (objectType.kind == TypeKind::Array) {
        if (!isError(indexType))
            checkConstantIndexBounds(*indexExpr.index, objectType.arraySize);
        return Type{objectType.elementKind};
    }

    if (objectType.kind == TypeKind::TypedPtr)
        return typedPtrElement(objectType);

    error(site, "cannot index a value of type " + typeName(objectType));
    return Type{TypeKind::Error};
}

Type SemanticAnalyzer::analyzeIndexAssign(const IndexAssignExpr& indexAssign) {
    const Token& site = exprFirstToken(*indexAssign.object);
    Type objectType   = analyzeExpr(*indexAssign.object);

    Type indexType = analyzeExpr(*indexAssign.index);
    if (!isError(indexType) && !isInteger(indexType.kind))
        error(site, "index must be an integer type, got " + typeName(indexType));

    if (isError(objectType)) {
        analyzeExpr(*indexAssign.value);
        return Type{TypeKind::Error};
    }

    Type elementType;
    if (objectType.kind == TypeKind::Array) {
        if (!isError(indexType))
            checkConstantIndexBounds(*indexAssign.index, objectType.arraySize);
        elementType = Type{objectType.elementKind};
    } else if (objectType.kind == TypeKind::TypedPtr) {
        elementType = typedPtrElement(objectType);
    } else {
        error(site, "cannot index a value of type " + typeName(objectType));
        analyzeExpr(*indexAssign.value);
        return Type{TypeKind::Error};
    }

    // Value type must be assignable to element type
    Type valueType = analyzeExpr(*indexAssign.value);
    checkCast(valueType, elementType, site, "element assignment");

    return elementType;
}

// ============================================================
// Class expression analysis
// ============================================================

Type SemanticAnalyzer::analyzeThis(const ThisExpr& thisExpr) {
    if (currentClassName.empty()) {
        error(thisExpr.keyword, "'this' used outside of a class method");
        return Type{TypeKind::Error};
    }
    if (currentMethodIsStatic) {
        error(thisExpr.keyword, "'this' cannot be used in a static method");
        return Type{TypeKind::Error};
    }
    if (currentClassIsEnum)
        return makeEnumType(currentClassName);
    return makeObjectType(currentClassName);
}

Type SemanticAnalyzer::analyzeMemberAccess(const MemberAccessExpr& memberAccess) {
    // Static access through a type name: EnumName.VARIANT or ClassName::field.
    if (std::holds_alternative<IdentifierExpr>(*memberAccess.object->node)) {
        const auto& ident = std::get<IdentifierExpr>(*memberAccess.object->node);
        auto enumIt = enumRegistry.find(ident.name.lexeme);
        if (enumIt != enumRegistry.end()) {
            // It's an enum name — the member must be a declared variant.
            if (!enumIt->second.variantSet.count(memberAccess.field.lexeme)) {
                error(memberAccess.field, "enum '" + ident.name.lexeme
                      + "' has no variant '" + memberAccess.field.lexeme + "'");
                return Type{TypeKind::Error};
            }
            return makeEnumType(ident.name.lexeme);
        }
        // ClassName::field — static member field access through the type name.
        auto clsIt = classRegistry.find(ident.name.lexeme);
        if (clsIt != classRegistry.end()) {
            auto sfIt = clsIt->second.staticFields.find(memberAccess.field.lexeme);
            if (sfIt == clsIt->second.staticFields.end()) {
                error(memberAccess.field, "class '" + ident.name.lexeme
                      + "' has no static member '" + memberAccess.field.lexeme + "'");
                return Type{TypeKind::Error};
            }
            const ClassInfo::StaticField& sf = sfIt->second;
            if (!sf.isPublic && currentClassName != ident.name.lexeme)
                warn(memberAccess.field, "static field '" + memberAccess.field.lexeme
                     + "' is private in class '" + ident.name.lexeme + "'");
            return sf.type;
        }
    }

    Type objectType = analyzeExpr(*memberAccess.object);
    if (isError(objectType)) return Type{TypeKind::Error};

    const ClassInfo* cls = lookupObjectClass(objectType, memberAccess.field);
    if (!cls) return Type{TypeKind::Error};

    // A static field may also be read through an instance: obj.staticField.
    auto staticIt = cls->staticFields.find(memberAccess.field.lexeme);
    if (staticIt != cls->staticFields.end()) {
        const ClassInfo::StaticField& sf = staticIt->second;
        if (!sf.isPublic && currentClassName != objectType.className)
            warn(memberAccess.field, "static field '" + memberAccess.field.lexeme
                 + "' is private in class '" + objectType.className + "'");
        return sf.type;
    }

    auto fieldIt = cls->fields.find(memberAccess.field.lexeme);
    if (fieldIt == cls->fields.end()) {
        error(memberAccess.field, "class '" + objectType.className
              + "' has no field '" + memberAccess.field.lexeme + "'");
        return Type{TypeKind::Error};
    }

    const ClassInfo::Field& field = fieldIt->second;
    // Access control: private fields emit a warning (not an error) when accessed from outside the class.
    if (!field.isPublic && currentClassName != objectType.className) {
        warn(memberAccess.field, "field '" + memberAccess.field.lexeme
             + "' is private in class '" + objectType.className + "'");
    }

    return field.type;
}

bool SemanticAnalyzer::exprIsMutablePlace(const Expr& expr) {
    const auto& node = *expr.node;
    // `this` is a mutable receiver only inside a `mut` method / ctor / dtor (Rust &mut self).
    if (std::holds_alternative<ThisExpr>(node))       return currentThisMutable;
    // Freshly-owned references: `new T(...)` and call/method results.
    if (std::holds_alternative<NewExpr>(node))        return true;
    if (std::holds_alternative<CallExpr>(node))       return true;
    if (std::holds_alternative<MethodCallExpr>(node)) return true;
    if (std::holds_alternative<IndexExpr>(node))
        return exprIsMutablePlace(*std::get<IndexExpr>(node).object);
    if (std::holds_alternative<CastExpr>(node))
        return std::get<CastExpr>(node).isMut;   // `x as mut T` yields a mutable view
    if (std::holds_alternative<IdentifierExpr>(node)) {
        const Symbol* s = symbolTable.lookup(std::get<IdentifierExpr>(node).name.lexeme);
        // Non-variable identifiers (e.g. class names for statics) are not gated here.
        return !s || s->kind != Symbol::Kind::Variable || s->isMutable;
    }
    if (std::holds_alternative<MemberAccessExpr>(node)) {
        const auto& ma = std::get<MemberAccessExpr>(node);
        if (!exprIsMutablePlace(*ma.object)) return false;
        // A field is a mutable place only if the field itself is `mut`.
        Type ownerT = analyzeExpr(*ma.object);
        if (ownerT.kind == TypeKind::Object || ownerT.kind == TypeKind::Reference) {
            auto cit = classRegistry.find(ownerT.className);
            if (cit != classRegistry.end()) {
                auto fit = cit->second.fields.find(ma.field.lexeme);
                if (fit != cit->second.fields.end()) return fit->second.isMut;
            }
        }
        return true;   // unknown shape → don't over-report
    }
    return true;
}

void SemanticAnalyzer::warnConstToMut(const Token& at, const Expr& source, const Type& targetType) {
    if (targetType.kind != TypeKind::Reference) return;   // refs only (value copies are independent)
    if (std::holds_alternative<CastExpr>(*source.node)) return;   // explicit cast silences
    if (exprIsMutablePlace(source)) return;               // source already grants write access
    warn(at, "coercing a read-only (const) reference into a 'mut' binding; add an explicit "
             "'as mut " + typeName(targetType) + "' cast to silence this warning");
}

Type SemanticAnalyzer::analyzeMemberAssign(const MemberAssignExpr& memberAssign) {
    // Static field write through the type name: ClassName::field = value.
    if (std::holds_alternative<IdentifierExpr>(*memberAssign.object->node)) {
        const auto& ident = std::get<IdentifierExpr>(*memberAssign.object->node);
        auto clsIt = classRegistry.find(ident.name.lexeme);
        if (clsIt != classRegistry.end() && enumRegistry.find(ident.name.lexeme) == enumRegistry.end()) {
            auto sfIt = clsIt->second.staticFields.find(memberAssign.field.lexeme);
            if (sfIt == clsIt->second.staticFields.end()) {
                error(memberAssign.field, "class '" + ident.name.lexeme
                      + "' has no static member '" + memberAssign.field.lexeme + "'");
                analyzeExpr(*memberAssign.value);
                return Type{TypeKind::Error};
            }
            const ClassInfo::StaticField& sf = sfIt->second;
            if (!sf.isPublic && currentClassName != ident.name.lexeme)
                warn(memberAssign.field, "static field '" + memberAssign.field.lexeme
                     + "' is private in class '" + ident.name.lexeme + "'");
            Type valueType = analyzeExpr(*memberAssign.value);
            checkCast(valueType, sf.type, memberAssign.field, "static field assignment");
            return sf.type;
        }
    }

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

    // Static field write through an instance: obj.staticField = value.
    auto staticIt = cls->staticFields.find(memberAssign.field.lexeme);
    if (staticIt != cls->staticFields.end()) {
        const ClassInfo::StaticField& sf = staticIt->second;
        if (!sf.isPublic && currentClassName != objectType.className)
            warn(memberAssign.field, "static field '" + memberAssign.field.lexeme
                 + "' is private in class '" + objectType.className + "'");
        Type valueType = analyzeExpr(*memberAssign.value);
        checkCast(valueType, sf.type, memberAssign.field, "static field assignment");
        return sf.type;
    }

    auto fieldIt = cls->fields.find(memberAssign.field.lexeme);
    if (fieldIt == cls->fields.end()) {
        error(memberAssign.field, "class '" + objectType.className
              + "' has no field '" + memberAssign.field.lexeme + "'");
        analyzeExpr(*memberAssign.value);
        return Type{TypeKind::Error};
    }

    const ClassInfo::Field& field = fieldIt->second;
    // Enum fields are immutable: only assignable via 'this.field' inside the
    // enum's own constructor.
    if (objectType.kind == TypeKind::Enum) {
        bool isThis = std::holds_alternative<ThisExpr>(*memberAssign.object->node);
        if (!inEnumConstructor || !isThis) {
            error(memberAssign.field, "cannot assign to field '" + memberAssign.field.lexeme
                  + "' of enum '" + objectType.className
                  + "'; enum fields are immutable");
            analyzeExpr(*memberAssign.value);
            return field.type;
        }
    }
    // Instance fields are const by default: a non-`mut` field may be written only via
    // 'this.field' inside the class's own constructor (mirrors the enum-field rule).
    // Applies whether the instance is a value (`Object`) or a heap reference
    // (`Reference`); a reference target is never `this`, so it is always gated.
    if ((objectType.kind == TypeKind::Object || objectType.kind == TypeKind::Reference)
        && !field.isMut) {
        bool isThis = std::holds_alternative<ThisExpr>(*memberAssign.object->node);
        if (!inConstructor || !isThis) {
            error(memberAssign.field, "cannot assign to immutable field '" + memberAssign.field.lexeme
                  + "' of class '" + objectType.className
                  + "'; declare it 'mut' to allow mutation");
            analyzeExpr(*memberAssign.value);
            return field.type;
        }
    }
    // Transitive const: writing any field also requires the *receiver* to be a mutable
    // place — a `mut` local/borrow, or `this` inside a `mut` method / ctor / dtor.
    if ((objectType.kind == TypeKind::Object || objectType.kind == TypeKind::Reference)
        && !exprIsMutablePlace(*memberAssign.object)) {
        if (std::holds_alternative<ThisExpr>(*memberAssign.object->node))
            error(memberAssign.field, "cannot write to field '" + memberAssign.field.lexeme
                  + "' in a read-only method; declare the method 'mut'");
        else
            error(memberAssign.field, "cannot assign to field '" + memberAssign.field.lexeme
                  + "' through an immutable binding; declare the "
                  + (objectType.kind == TypeKind::Reference ? std::string("reference") : std::string("variable"))
                  + " 'mut'");
        analyzeExpr(*memberAssign.value);
        return field.type;
    }
    if (!field.isPublic && currentClassName != objectType.className) {
        warn(memberAssign.field, "field '" + memberAssign.field.lexeme
             + "' is private in class '" + objectType.className + "'");
    }

    Type valueType = analyzeExpr(*memberAssign.value);
    checkCast(valueType, field.type, memberAssign.field, "field assignment");
    return field.type;
}

Type SemanticAnalyzer::analyzeMethodCall(const MethodCallExpr& methodCall) {
    // Static method call through the type name: ClassName::method(args).
    // The leading identifier names a class (and is not shadowed by a variable).
    if (std::holds_alternative<IdentifierExpr>(*methodCall.object->node)) {
        const auto& ident = std::get<IdentifierExpr>(*methodCall.object->node);
        if (!symbolTable.lookup(ident.name.lexeme)) {
            auto clsIt = classRegistry.find(ident.name.lexeme);
            if (clsIt != classRegistry.end()) {
                auto mIt = clsIt->second.methods.find(methodCall.method.lexeme);
                if (mIt == clsIt->second.methods.end()) {
                    error(methodCall.method, "class '" + ident.name.lexeme
                          + "' has no static method '" + methodCall.method.lexeme + "'");
                    for (const auto& arg : methodCall.args) analyzeExpr(*arg);
                    return Type{TypeKind::Error};
                }
                const ClassInfo::Method& sm = mIt->second;
                if (!sm.isStatic) {
                    error(methodCall.method, "method '" + methodCall.method.lexeme
                          + "' is not static; call it on an instance");
                    for (const auto& arg : methodCall.args) analyzeExpr(*arg);
                    return sm.returnType;
                }
                if (!sm.isPublic && currentClassName != ident.name.lexeme)
                    warn(methodCall.method, "static method '" + methodCall.method.lexeme
                         + "' is private in class '" + ident.name.lexeme + "'");
                if (methodCall.args.size() != sm.paramTypes.size()) {
                    error(methodCall.method, "static method '" + methodCall.method.lexeme
                          + "' expects " + std::to_string(sm.paramTypes.size())
                          + " argument(s), got " + std::to_string(methodCall.args.size()));
                    for (const auto& arg : methodCall.args) analyzeExpr(*arg);
                    return sm.returnType;
                }
                analyzeCallArgs(methodCall.args, sm.paramTypes, methodCall.method,
                                "static method '" + methodCall.method.lexeme + "'", sm.paramMut);
                return sm.returnType;
            }
        }
    }

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
    // A `mut` method mutates its receiver, so the receiver must be a mutable place —
    // a `mut` binding, or `this` inside a `mut` method / ctor / dtor.
    if (method.isMut
        && (objectType.kind == TypeKind::Object || objectType.kind == TypeKind::Reference)
        && !exprIsMutablePlace(*methodCall.object)) {
        if (std::holds_alternative<ThisExpr>(*methodCall.object->node))
            error(methodCall.method, "cannot call mutating method '" + methodCall.method.lexeme
                  + "' on 'this' in a read-only method; declare the calling method 'mut'");
        else
            error(methodCall.method, "cannot call mutating method '" + methodCall.method.lexeme
                  + "' through an immutable binding; declare it 'mut'");
        for (const auto& arg : methodCall.args) analyzeExpr(*arg);
        return method.returnType;
    }
    if (!method.isPublic && currentClassName != objectType.className) {
        warn(methodCall.method, "method '" + methodCall.method.lexeme
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
                    "method '" + methodCall.method.lexeme + "'", method.paramMut);
    return method.returnType;
}

// ============================================================
// Cast expression analysis
// ============================================================

Type SemanticAnalyzer::analyzeCast(const CastExpr& castExpr) {
    Type fromType = analyzeExpr(*castExpr.operand);
    Type toType   = resolveTypeToken(castExpr.targetType);

    if (isError(fromType) || isError(toType)) return Type{TypeKind::Error};

    // Cannot cast from or to void
    if (fromType.kind == TypeKind::Void)
        error(castExpr.targetType, "cannot cast from 'void'");
    if (toType.kind == TypeKind::Void)
        error(castExpr.targetType, "cannot cast to 'void'");

    // Identity — always fine (no-op)
    if (fromType == toType) return toType;

    // Numeric ↔ numeric (any combination of I8/I16/I32/I64/U8/U16/U32/U64/F32/F64)
    if (isNumeric(fromType.kind) && isNumeric(toType.kind)) return toType;

    // Char ↔ integer / numeric
    if (fromType.kind == TypeKind::Char && (isInteger(toType.kind) || isFloat(toType.kind))) return toType;
    if ((isInteger(fromType.kind) || isFloat(fromType.kind)) && toType.kind == TypeKind::Char) return toType;

    // Bool ↔ integer / float
    if (fromType.kind == TypeKind::Bool && isNumeric(toType.kind)) return toType;
    if (isNumeric(fromType.kind) && toType.kind == TypeKind::Bool) return toType;

    // Char ↔ Bool
    if (fromType.kind == TypeKind::Char && toType.kind == TypeKind::Bool) return toType;
    if (fromType.kind == TypeKind::Bool && toType.kind == TypeKind::Char) return toType;

    // Integer ↔ ptr
    if (isInteger(fromType.kind) && toType.kind == TypeKind::Ptr) return toType;
    if (fromType.kind == TypeKind::Ptr && isInteger(toType.kind)) return toType;

    // Object → ptr (take address of stack-allocated struct)
    if (fromType.kind == TypeKind::Object && toType.kind == TypeKind::Ptr) return toType;

    // Array → ptr (pointer to first element)
    if (fromType.kind == TypeKind::Array && toType.kind == TypeKind::Ptr) return toType;

    error(castExpr.targetType,
          "cannot cast '" + typeName(fromType) + "' to '" + typeName(toType) + "'");
    return Type{TypeKind::Error};
}

// ============================================================
// new expression analysis — allocates a heap instance (Class&)
// ============================================================

Type SemanticAnalyzer::analyzeNew(const NewExpr& newExpr) {
    const std::string& className = newExpr.className.lexeme;

    if (enumRegistry.count(className)) {
        error(newExpr.className, "cannot 'new' an enum '" + className
              + "'; use one of its variants");
        for (const auto& arg : newExpr.args) analyzeExpr(*arg);
        return Type{TypeKind::Error};
    }

    auto it = classRegistry.find(className);
    if (it == classRegistry.end()) {
        error(newExpr.className, "unknown class '" + className + "' in 'new' expression");
        for (const auto& arg : newExpr.args) analyzeExpr(*arg);
        return Type{TypeKind::Error};
    }

    const ClassInfo& cls = it->second;

    // Copy construction: new Class(x) where x is a value/reference of the same
    // class. Deep-copies x; bypasses regular constructor matching.
    if (newExpr.args.size() == 1) {
        Type argType = analyzeExpr(*newExpr.args[0]);
        if (!isError(argType)
            && (argType.kind == TypeKind::Object || argType.kind == TypeKind::Reference)
            && argType.className == className) {
            return makeReferenceType(className);
        }
        // Not a copy — fall through to regular constructor matching.
    }

    auto ctorIt = cls.methods.find(className);
    if (ctorIt == cls.methods.end()) {
        // No explicit constructor — only a zero-argument `new` is allowed.
        if (!newExpr.args.empty())
            error(newExpr.className, "class '" + className
                  + "' has no constructor but 'new' was given arguments");
        for (const auto& arg : newExpr.args) analyzeExpr(*arg);
        return makeReferenceType(className);
    }

    const ClassInfo::Method& ctor = ctorIt->second;
    if (newExpr.args.size() != ctor.paramTypes.size()) {
        error(newExpr.className, "constructor '" + className + "' expects "
              + std::to_string(ctor.paramTypes.size()) + " argument(s), got "
              + std::to_string(newExpr.args.size()));
        for (const auto& arg : newExpr.args) analyzeExpr(*arg);
        return makeReferenceType(className);
    }

    analyzeCallArgs(newExpr.args, ctor.paramTypes, newExpr.className,
                    "constructor '" + className + "'", ctor.paramMut);
    return makeReferenceType(className);
}
