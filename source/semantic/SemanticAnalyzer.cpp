//
// Created by Vladimir Arsenijevic on 01.6.2026.
//

#include "SemanticAnalyzer.h"
#include <iostream>

// ============================================================
// Internal helper — first (leftmost) token in an expression
// Used for error reporting when the AST node stores no keyword token.
// ============================================================

static const Token& firstToken(const Expr& expr) {
    struct Visitor {
        const Token& operator()(const LiteralExpr& literal)            const { return literal.token; }
        const Token& operator()(const IdentifierExpr& identifier)      const { return identifier.name; }
        const Token& operator()(const UnaryExpr& unary)                const { return unary.operatorToken; }
        const Token& operator()(const BinaryExpr& binary)              const { return firstToken(*binary.left); }
        const Token& operator()(const AssignExpr& assign)              const { return assign.name; }
        const Token& operator()(const CompoundAssignExpr& compoundAssign) const { return compoundAssign.name; }
        const Token& operator()(const PostfixExpr& postfix)            const { return firstToken(*postfix.operand); }
        const Token& operator()(const CallExpr& call)                  const { return call.callee; }
        const Token& operator()(const VarDeclExpr& varDecl)            const { return varDecl.typeName; }
        const Token& operator()(const IndexExpr& indexExpr)         const { return indexExpr.name; }
        const Token& operator()(const IndexAssignExpr& indexAssign)  const { return indexAssign.name; }
    };
    return std::visit(Visitor{}, *expr.node);
}

// ============================================================
// Internal helper — control-flow return analysis
// Returns true when every execution path through `stmt` ends in a return.
// Conservative: loops are treated as "may not execute" so a lone
// while-loop body is not considered a guaranteed return path.
// ============================================================

static bool alwaysReturns(const Stmt& stmt);

static bool alwaysReturns(const BlockStmt& block) {
    for (const auto& stmtPtr : block.body)
        if (alwaysReturns(*stmtPtr)) return true;
    return false;
}

static bool alwaysReturns(const Stmt& stmt) {
    return std::visit(overloaded{
        [](const ReturnStmt&)          { return true; },
        [](const BlockStmt& block)     { return alwaysReturns(block); },
        [](const IfStmt& ifStmt)       {
            return ifStmt.elseBranch != nullptr
                   && alwaysReturns(*ifStmt.thenBranch)
                   && alwaysReturns(*ifStmt.elseBranch);
        },
        [](const WhileStmt&)           { return false; },  // may not execute
        [](const ForStmt&)             { return false; },  // may not execute
        [](const BreakStmt&)           { return false; },
        [](const ContinueStmt&)        { return false; },
        [](const ExprStmt&)            { return false; },
        [](const FunctionDeclStmt&)    { return false; },
        [](const ExternFuncDeclStmt&)  { return false; },
        [](const ImportStmt&)          { return false; },
    }, *stmt.node);
}

// ============================================================
// Public entry point
// ============================================================

SemanticResult SemanticAnalyzer::analyze(const Program& program) {
    symbolTable       = SymbolTable{};
    typeMap.clear();
    hadError          = false;
    currentReturnType = std::nullopt;
    loopDepth         = 0;

    symbolTable.enterScope();   // global scope

    collectFunctions(program);  // pass 1: hoist function signatures

    for (const Stmt& stmt : program.declarations)
        analyzeStmt(stmt);      // pass 2: full analysis

    symbolTable.exitScope();

    return SemanticResult{ hadError, std::move(typeMap) };
}

// ============================================================
// Pass 1 — collect top-level function signatures
// ============================================================

void SemanticAnalyzer::collectFunctions(const Program& program) {
    for (const Stmt& stmt : program.declarations) {
        if (std::holds_alternative<FunctionDeclStmt>(*stmt.node)) {
            const auto& function = std::get<FunctionDeclStmt>(*stmt.node);

            std::vector<Type> paramTypes;
            for (const ParamDecl& param : function.params)
                paramTypes.push_back(typeFromToken(param.typeName.type));

            Symbol sym{
                Symbol::Kind::Function,
                typeFromToken(function.returnType.type),
                function.name,
                std::move(paramTypes)
            };

            if (!symbolTable.declare(function.name.lexeme, sym)) {
                const Symbol* prev = symbolTable.lookupCurrentScope(function.name.lexeme);
                error(function.name, "function '" + function.name.lexeme + "' already declared in this scope"
                      + (prev ? " (previously declared at line "
                               + std::to_string(prev->declarationToken.line) + ")" : ""));
            }
        }
        else if (std::holds_alternative<ExternFuncDeclStmt>(*stmt.node)) {
            const auto& externDecl = std::get<ExternFuncDeclStmt>(*stmt.node);

            std::vector<Type> paramTypes;
            for (const ParamDecl& param : externDecl.params)
                paramTypes.push_back(typeFromToken(param.typeName.type));

            Symbol sym{
                Symbol::Kind::Function,
                typeFromToken(externDecl.returnType.type),
                externDecl.name,
                std::move(paramTypes)
            };

            if (!symbolTable.declare(externDecl.name.lexeme, sym)) {
                const Symbol* prev = symbolTable.lookupCurrentScope(externDecl.name.lexeme);
                error(externDecl.name, "extern '" + externDecl.name.lexeme + "' already declared in this scope"
                      + (prev ? " (previously declared at line "
                               + std::to_string(prev->declarationToken.line) + ")" : ""));
            }
        }
    }
}

// ============================================================
// Statement analysis
// ============================================================

void SemanticAnalyzer::analyzeStmt(const Stmt& stmt) {
    std::visit(overloaded{
        [&](const ExprStmt& exprStmt)              { analyzeExpr(exprStmt.expression); },
        [&](const BlockStmt& blockStmt)            { analyzeBlock(blockStmt); },
        [&](const IfStmt& ifStmt)                  { analyzeIf(ifStmt); },
        [&](const WhileStmt& whileStmt)            { analyzeWhile(whileStmt); },
        [&](const ForStmt& forStmt)                { analyzeFor(forStmt); },
        [&](const ReturnStmt& returnStmt)            { analyzeReturn(returnStmt); },
        [&](const BreakStmt& breakStmt)             { analyzeBreak(breakStmt); },
        [&](const ContinueStmt& continueStmt)       { analyzeContinue(continueStmt); },
        [&](const FunctionDeclStmt& functionDecl)    { analyzeFunctionDecl(functionDecl); },
        [&](const ExternFuncDeclStmt& externDecl)    { analyzeExternFuncDecl(externDecl); },
        [&](const ImportStmt&)                       { /* resolved before semantic pass */ },
    }, *stmt.node);
}

void SemanticAnalyzer::analyzeBlock(const BlockStmt& block) {
    enterScope();
    for (const auto& statement : block.body) analyzeStmt(*statement);
    exitScope();
}

void SemanticAnalyzer::analyzeIf(const IfStmt& ifStmt) {
    Type conditionType = analyzeExpr(ifStmt.condition);
    if (!isError(conditionType) && !isBoolCompatible(conditionType)) {
        error(firstToken(ifStmt.condition),
              "if condition must be bool-compatible, got " + typeName(conditionType));
    }
    analyzeStmt(*ifStmt.thenBranch);
    if (ifStmt.elseBranch) analyzeStmt(*ifStmt.elseBranch);
}

void SemanticAnalyzer::analyzeWhile(const WhileStmt& whileStmt) {
    Type conditionType = analyzeExpr(whileStmt.condition);
    if (!isError(conditionType) && !isBoolCompatible(conditionType)) {
        error(firstToken(whileStmt.condition),
              "while condition must be bool-compatible, got " + typeName(conditionType));
    }
    loopDepth++;
    analyzeStmt(*whileStmt.body);
    loopDepth--;
}

void SemanticAnalyzer::analyzeFor(const ForStmt& forStmt) {
    enterScope();   // scope for the init variable

    if (forStmt.init) analyzeStmt(*forStmt.init);

    if (forStmt.condition) {
        Type conditionType = analyzeExpr(*forStmt.condition);
        if (!isError(conditionType) && !isBoolCompatible(conditionType)) {
            error(firstToken(*forStmt.condition),
                  "for condition must be bool-compatible, got " + typeName(conditionType));
        }
    }

    if (forStmt.increment) analyzeExpr(*forStmt.increment);

    loopDepth++;
    analyzeStmt(*forStmt.body);
    loopDepth--;

    exitScope();
}

void SemanticAnalyzer::analyzeReturn(const ReturnStmt& returnStmt) {
    if (!currentReturnType) {
        error(returnStmt.keyword, "return statement outside of function");
        return;
    }

    if (!returnStmt.value) {
        if (currentReturnType->kind != TypeKind::Void) {
            error(returnStmt.keyword, "return with no value in function returning "
                  + typeName(*currentReturnType));
        }
        return;
    }

    Type actualType = analyzeExpr(*returnStmt.value);
    checkCast(actualType, *currentReturnType, returnStmt.keyword, "return");
}

void SemanticAnalyzer::analyzeFunctionDecl(const FunctionDeclStmt& functionDecl) {
    // Signature is already registered in the global scope by collectFunctions.
    std::optional<Type> savedReturnType = currentReturnType;
    int                 savedLoopDepth  = loopDepth;
    currentReturnType = typeFromToken(functionDecl.returnType.type);
    loopDepth         = 0;  // loops in the outer scope do not extend into this function

    enterScope();  // function scope — parameters live here

    for (const ParamDecl& param : functionDecl.params) {
        Type paramType = typeFromToken(param.typeName.type);
        if (paramType.kind == TypeKind::Void) {
            error(param.typeName, "parameter '" + param.name.lexeme + "' cannot have type 'void'");
            paramType = Type{TypeKind::Error};  // suppress cascading errors in the body
        }
        Symbol sym{
            Symbol::Kind::Variable,
            paramType,
            param.name,
            {}
        };
        if (!symbolTable.declare(param.name.lexeme, sym))
            error(param.name, "duplicate parameter name '" + param.name.lexeme + "'");
    }

    // Analyse body statements directly — do NOT call analyzeBlock to avoid
    // opening a second scope on top of the function scope.
    for (const auto& statement : functionDecl.body.body) analyzeStmt(*statement);

    // Warn when a non-void function may fall off the end without returning.
    // This is conservative: loops are never treated as guaranteed returns.
    if (currentReturnType->kind != TypeKind::Void && !alwaysReturns(functionDecl.body))
        warn(functionDecl.name, "function '" + functionDecl.name.lexeme
             + "' does not always return a value");

    exitScope();

    currentReturnType = savedReturnType;
    loopDepth         = savedLoopDepth;
}

void SemanticAnalyzer::analyzeExternFuncDecl(const ExternFuncDeclStmt& externDecl) {
    // Signature was already registered in pass 1 (collectFunctions).
    // Just validate that no parameter has type 'void'.
    for (const ParamDecl& param : externDecl.params) {
        Type paramType = typeFromToken(param.typeName.type);
        if (paramType.kind == TypeKind::Void)
            error(param.typeName, "parameter '" + param.name.lexeme + "' cannot have type 'void'");
    }
}

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

    // Check each argument type
    size_t argIndex = 0;
    for (const auto& arg : call.args) {
        Type argumentType = analyzeExpr(*arg);
        checkCast(argumentType, sym->paramTypes[argIndex], call.callee,
                  "argument " + std::to_string(argIndex + 1)
                  + " of '" + call.callee.lexeme + "'");
        ++argIndex;
    }

    return sym->type;
}

Type SemanticAnalyzer::analyzeVarDecl(const VarDeclExpr& varDecl) {
    Type elementType  = typeFromToken(varDecl.typeName.type);
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
    if (!isError(indexType) && indexExpr.index->node
        && std::holds_alternative<LiteralExpr>(*indexExpr.index->node)) {
        const auto& lit = std::get<LiteralExpr>(*indexExpr.index->node);
        if (lit.token.type == TokenType::NUMBER
            && lit.token.lexeme.find('.') == std::string::npos) {
            try {
                long long indexVal = std::stoll(lit.token.lexeme);
                if (indexVal < 0 || static_cast<size_t>(indexVal) >= sym->type.arraySize)
                    error(lit.token, "array index " + lit.token.lexeme
                          + " is out of bounds for array of size "
                          + std::to_string(sym->type.arraySize));
            } catch (...) {}
        }
    }

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
    if (!isError(indexType) && indexAssign.index->node
        && std::holds_alternative<LiteralExpr>(*indexAssign.index->node)) {
        const auto& lit = std::get<LiteralExpr>(*indexAssign.index->node);
        if (lit.token.type == TokenType::NUMBER
            && lit.token.lexeme.find('.') == std::string::npos) {
            try {
                long long indexVal = std::stoll(lit.token.lexeme);
                if (indexVal < 0 || static_cast<size_t>(indexVal) >= sym->type.arraySize)
                    error(lit.token, "array index " + lit.token.lexeme
                          + " is out of bounds for array of size "
                          + std::to_string(sym->type.arraySize));
            } catch (...) {}
        }
    }

    // Value type must be assignable to element type
    Type elementType{sym->type.elementKind};
    Type valueType = analyzeExpr(*indexAssign.value);
    checkCast(valueType, elementType, indexAssign.name, "array element assignment");

    return elementType;
}

// ============================================================
// Helpers
// ============================================================

void SemanticAnalyzer::analyzeBreak(const BreakStmt& breakStmt) {
    if (loopDepth == 0)
        error(breakStmt.keyword, "'break' used outside of a loop");
}

void SemanticAnalyzer::analyzeContinue(const ContinueStmt& continueStmt) {
    if (loopDepth == 0)
        error(continueStmt.keyword, "'continue' used outside of a loop");
}

void SemanticAnalyzer::enterScope() { symbolTable.enterScope(); }
void SemanticAnalyzer::exitScope()  { symbolTable.exitScope(); }

const Symbol* SemanticAnalyzer::lookupSymbol(const Token& nameToken) {
    const Symbol* sym = symbolTable.lookup(nameToken.lexeme);
    if (!sym) error(nameToken, "use of undeclared identifier '" + nameToken.lexeme + "'");
    return sym;
}

void SemanticAnalyzer::error(const Token& token, const std::string& message) {
    hadError = true;
    std::cerr << "[line " << token.line << "] Error: " << message << '\n';
}

void SemanticAnalyzer::warn(const Token& token, const std::string& message) {
    std::cerr << "[line " << token.line << "] Warning: " << message << '\n';
}

void SemanticAnalyzer::recordType(const Expr& expr, Type type) {
    typeMap[expr.node.get()] = type;
}

void SemanticAnalyzer::checkCast(Type from, Type to,
                                  const Token& site, const std::string& context) {
    if (isError(from) || isError(to)) return;
    CastResult castResult = canImplicitlyCast(from, to);
    std::string contextString = context.empty() ? "" : " in " + context;
    if (castResult == CastResult::None) {
        error(site, "cannot implicitly convert " + typeName(from)
              + " to " + typeName(to) + contextString);
    } else if (castResult == CastResult::Warn) {
        warn(site, "implicit conversion from " + typeName(from)
             + " to " + typeName(to) + " may lose data" + contextString);
    }
}
