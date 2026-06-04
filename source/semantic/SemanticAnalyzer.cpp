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
        const Token& operator()(const LiteralExpr& e)        const { return e.token; }
        const Token& operator()(const IdentifierExpr& e)     const { return e.name; }
        const Token& operator()(const UnaryExpr& e)          const { return e.op; }
        const Token& operator()(const BinaryExpr& e)         const { return firstToken(*e.left); }
        const Token& operator()(const AssignExpr& e)         const { return e.name; }
        const Token& operator()(const CompoundAssignExpr& e) const { return e.name; }
        const Token& operator()(const PostfixExpr& e)        const { return firstToken(*e.operand); }
        const Token& operator()(const CallExpr& e)           const { return e.callee; }
        const Token& operator()(const VarDeclExpr& e)        const { return e.typeName; }
    };
    return std::visit(Visitor{}, *expr.node);
}

// ============================================================
// Public entry point
// ============================================================

SemanticResult SemanticAnalyzer::analyze(const Program& program) {
    symbolTable_       = SymbolTable{};
    typeMap_.clear();
    hadError_          = false;
    currentReturnType_ = std::nullopt;

    symbolTable_.enterScope();   // global scope

    collectFunctions(program);   // pass 1: hoist function signatures

    for (const Stmt& stmt : program.declarations) {
        analyzeStmt(stmt);       // pass 2: full analysis
    }

    symbolTable_.exitScope();

    return SemanticResult{ hadError_, std::move(typeMap_) };
}

// ============================================================
// Pass 1 — collect top-level function signatures
// ============================================================

void SemanticAnalyzer::collectFunctions(const Program& program) {
    for (const Stmt& stmt : program.declarations) {
        if (!std::holds_alternative<FunctionDeclStmt>(*stmt.node)) continue;
        const auto& f = std::get<FunctionDeclStmt>(*stmt.node);

        std::vector<Type> paramTypes;
        for (const ParamDecl& p : f.params)
            paramTypes.push_back(typeFromToken(p.typeName.type));

        Symbol sym{
            Symbol::Kind::Function,
            typeFromToken(f.returnType.type),
            f.name,
            std::move(paramTypes)
        };

        if (!symbolTable_.declare(f.name.lexeme, sym)) {
            const Symbol* prev = symbolTable_.lookupCurrentScope(f.name.lexeme);
            error(f.name, "function '" + f.name.lexeme + "' already declared in this scope"
                  + (prev ? " (previously declared at line "
                           + std::to_string(prev->declarationToken.line) + ")" : ""));
        }
    }
}

// ============================================================
// Statement analysis
// ============================================================

void SemanticAnalyzer::analyzeStmt(const Stmt& stmt) {
    std::visit(overloaded{
        [&](const ExprStmt& s)         { analyzeExpr(s.expression); },
        [&](const BlockStmt& s)        { analyzeBlock(s); },
        [&](const IfStmt& s)           { analyzeIf(s); },
        [&](const WhileStmt& s)        { analyzeWhile(s); },
        [&](const ForStmt& s)          { analyzeFor(s); },
        [&](const ReturnStmt& s)       { analyzeReturn(s); },
        [&](const FunctionDeclStmt& s) { analyzeFunctionDecl(s); },
    }, *stmt.node);
}

void SemanticAnalyzer::analyzeBlock(const BlockStmt& block) {
    enterScope();
    for (const auto& s : block.body) analyzeStmt(*s);
    exitScope();
}

void SemanticAnalyzer::analyzeIf(const IfStmt& s) {
    Type condType = analyzeExpr(s.condition);
    if (!isError(condType) && !isBoolCompatible(condType)) {
        error(firstToken(s.condition),
              "if condition must be bool-compatible, got " + typeName(condType));
    }
    analyzeStmt(*s.thenBranch);
    if (s.elseBranch) analyzeStmt(*s.elseBranch);
}

void SemanticAnalyzer::analyzeWhile(const WhileStmt& s) {
    Type condType = analyzeExpr(s.condition);
    if (!isError(condType) && !isBoolCompatible(condType)) {
        error(firstToken(s.condition),
              "while condition must be bool-compatible, got " + typeName(condType));
    }
    analyzeStmt(*s.body);
}

void SemanticAnalyzer::analyzeFor(const ForStmt& s) {
    enterScope();   // scope for the init variable

    if (s.init) analyzeStmt(*s.init);

    if (s.condition) {
        Type condType = analyzeExpr(*s.condition);
        if (!isError(condType) && !isBoolCompatible(condType)) {
            error(firstToken(*s.condition),
                  "for condition must be bool-compatible, got " + typeName(condType));
        }
    }

    if (s.increment) analyzeExpr(*s.increment);

    analyzeStmt(*s.body);

    exitScope();
}

void SemanticAnalyzer::analyzeReturn(const ReturnStmt& s) {
    if (!currentReturnType_) {
        error(s.keyword, "return statement outside of function");
        return;
    }

    if (!s.value) {
        if (currentReturnType_->kind != TypeKind::Void) {
            error(s.keyword, "return with no value in function returning "
                  + typeName(*currentReturnType_));
        }
        return;
    }

    Type actualType = analyzeExpr(*s.value);
    checkCast(actualType, *currentReturnType_, s.keyword, "return");
}

void SemanticAnalyzer::analyzeFunctionDecl(const FunctionDeclStmt& s) {
    // Signature is already registered in the global scope by collectFunctions.
    std::optional<Type> savedReturnType = currentReturnType_;
    currentReturnType_ = typeFromToken(s.returnType.type);

    enterScope();  // function scope — parameters live here

    for (const ParamDecl& p : s.params) {
        Symbol sym{
            Symbol::Kind::Variable,
            typeFromToken(p.typeName.type),
            p.name,
            {}
        };
        if (!symbolTable_.declare(p.name.lexeme, sym)) {
            error(p.name, "duplicate parameter name '" + p.name.lexeme + "'");
        }
    }

    // Analyse body statements directly — do NOT call analyzeBlock to avoid
    // opening a second scope on top of the function scope.
    for (const auto& stmt : s.body.body) analyzeStmt(*stmt);

    exitScope();

    currentReturnType_ = savedReturnType;
}

// ============================================================
// Expression analysis
// ============================================================

Type SemanticAnalyzer::analyzeExpr(const Expr& expr) {
    Type t = std::visit(overloaded{
        [&](const LiteralExpr& e)        { return analyzeLiteral(e); },
        [&](const IdentifierExpr& e)     { return analyzeIdentifier(e); },
        [&](const UnaryExpr& e)          { return analyzeUnary(e); },
        [&](const BinaryExpr& e)         { return analyzeBinary(e); },
        [&](const AssignExpr& e)         { return analyzeAssign(e); },
        [&](const CompoundAssignExpr& e) { return analyzeCompoundAssign(e); },
        [&](const PostfixExpr& e)        { return analyzePostfix(e); },
        [&](const CallExpr& e)           { return analyzeCall(e); },
        [&](const VarDeclExpr& e)        { return analyzeVarDecl(e); },
    }, *expr.node);
    recordType(expr, t);
    return t;
}

Type SemanticAnalyzer::analyzeLiteral(const LiteralExpr& e) {
    switch (e.token.type) {
        case TokenType::TRUE:
        case TokenType::FALSE:
            return Type{TypeKind::Bool};
        case TokenType::NUMBER:
            // Decimal point present → floating-point literal → f64
            if (e.token.lexeme.find('.') != std::string::npos)
                return Type{TypeKind::F64};
            return Type{TypeKind::I32};
        case TokenType::STRING:
            return Type{TypeKind::String};
        case TokenType::CHAR:
            return Type{TypeKind::Char};
        default:
            return Type{TypeKind::Error};
    }
}

Type SemanticAnalyzer::analyzeIdentifier(const IdentifierExpr& e) {
    const Symbol* sym = symbolTable_.lookup(e.name.lexeme);
    if (!sym) {
        error(e.name, "use of undeclared identifier '" + e.name.lexeme + "'");
        return Type{TypeKind::Error};
    }
    if (sym->kind == Symbol::Kind::Function) {
        error(e.name, "cannot use function '" + e.name.lexeme + "' as a value");
        return Type{TypeKind::Error};
    }
    return sym->type;
}

Type SemanticAnalyzer::analyzeUnary(const UnaryExpr& e) {
    Type operandType = analyzeExpr(*e.operand);

    switch (e.op.type) {
        case TokenType::BANG:
            if (!isError(operandType) && !isBoolCompatible(operandType)) {
                error(e.op, "operand of '!' must be bool-compatible, got " + typeName(operandType));
                return Type{TypeKind::Error};
            }
            return Type{TypeKind::Bool};

        case TokenType::MINUS:
            if (!isError(operandType) && !isNumeric(operandType.kind)) {
                error(e.op, "operand of unary '-' must be numeric, got " + typeName(operandType));
                return Type{TypeKind::Error};
            }
            return operandType;

        case TokenType::TILDE:
            if (!isError(operandType) && !isInteger(operandType.kind)) {
                error(e.op, "operand of '~' must be integer, got " + typeName(operandType));
                return Type{TypeKind::Error};
            }
            return operandType;

        case TokenType::INCREMENT:
        case TokenType::DECREMENT:
            if (!std::holds_alternative<IdentifierExpr>(*e.operand->node)) {
                error(e.op, "operand of '" + e.op.lexeme + "' must be an identifier");
                return Type{TypeKind::Error};
            }
            if (!isError(operandType) && !isNumeric(operandType.kind)) {
                error(e.op, "operand of '" + e.op.lexeme + "' must be numeric, got "
                      + typeName(operandType));
                return Type{TypeKind::Error};
            }
            return operandType;

        default:
            return Type{TypeKind::Error};
    }
}

Type SemanticAnalyzer::analyzeBinary(const BinaryExpr& e) {
    Type lt = analyzeExpr(*e.left);
    Type rt = analyzeExpr(*e.right);

    if (isError(lt) || isError(rt)) return Type{TypeKind::Error};

    switch (e.op.type) {
        // Arithmetic
        case TokenType::PLUS: case TokenType::MINUS:
        case TokenType::STAR: case TokenType::SLASH: case TokenType::PERCENT: {
            if (!isNumeric(lt.kind)) {
                error(e.op, "left operand of '" + e.op.lexeme + "' must be numeric, got "
                      + typeName(lt));
                return Type{TypeKind::Error};
            }
            if (!isNumeric(rt.kind)) {
                error(e.op, "right operand of '" + e.op.lexeme + "' must be numeric, got "
                      + typeName(rt));
                return Type{TypeKind::Error};
            }
            return commonArithmeticType(lt, rt);
        }

        // Bitwise
        case TokenType::PIPE: case TokenType::CARET: case TokenType::AMPERSAND:
        case TokenType::SHIFT_LEFT: case TokenType::SHIFT_RIGHT: {
            if (!isInteger(lt.kind)) {
                error(e.op, "left operand of '" + e.op.lexeme + "' must be integer, got "
                      + typeName(lt));
                return Type{TypeKind::Error};
            }
            if (!isInteger(rt.kind)) {
                error(e.op, "right operand of '" + e.op.lexeme + "' must be integer, got "
                      + typeName(rt));
                return Type{TypeKind::Error};
            }
            return commonArithmeticType(lt, rt);
        }

        // Ordering comparisons
        case TokenType::LESS: case TokenType::LESS_EQUAL:
        case TokenType::GREATER: case TokenType::GREATER_EQUAL: {
            if (!isNumeric(lt.kind))
                error(e.op, "left operand of '" + e.op.lexeme + "' must be numeric, got "
                      + typeName(lt));
            if (!isNumeric(rt.kind))
                error(e.op, "right operand of '" + e.op.lexeme + "' must be numeric, got "
                      + typeName(rt));
            return Type{TypeKind::Bool};
        }

        // Equality comparisons
        case TokenType::EQUAL_EQUAL: case TokenType::BANG_EQUAL: {
            bool compatible =
                (lt.kind == rt.kind) ||
                (isNumeric(lt.kind) && isNumeric(rt.kind));
            if (!compatible) {
                error(e.op, "incompatible types for '" + e.op.lexeme + "': "
                      + typeName(lt) + " and " + typeName(rt));
            }
            return Type{TypeKind::Bool};
        }

        // Logical
        case TokenType::AND: case TokenType::OR: {
            if (!isBoolCompatible(lt))
                error(e.op, "left operand of '" + e.op.lexeme
                      + "' must be bool-compatible, got " + typeName(lt));
            if (!isBoolCompatible(rt))
                error(e.op, "right operand of '" + e.op.lexeme
                      + "' must be bool-compatible, got " + typeName(rt));
            return Type{TypeKind::Bool};
        }

        default:
            return Type{TypeKind::Error};
    }
}

Type SemanticAnalyzer::analyzeAssign(const AssignExpr& e) {
    const Symbol* sym = symbolTable_.lookup(e.name.lexeme);
    if (!sym) {
        error(e.name, "use of undeclared identifier '" + e.name.lexeme + "'");
        analyzeExpr(*e.value);
        return Type{TypeKind::Error};
    }
    if (sym->kind == Symbol::Kind::Function) {
        error(e.name, "cannot assign to function '" + e.name.lexeme + "'");
        analyzeExpr(*e.value);
        return Type{TypeKind::Error};
    }

    Type lhsType = sym->type;
    Type rhsType = analyzeExpr(*e.value);
    checkCast(rhsType, lhsType, e.name, "assignment");
    return lhsType;
}

Type SemanticAnalyzer::analyzeCompoundAssign(const CompoundAssignExpr& e) {
    const Symbol* sym = symbolTable_.lookup(e.name.lexeme);
    if (!sym) {
        error(e.name, "use of undeclared identifier '" + e.name.lexeme + "'");
        analyzeExpr(*e.value);
        return Type{TypeKind::Error};
    }
    if (sym->kind == Symbol::Kind::Function) {
        error(e.name, "cannot assign to function '" + e.name.lexeme + "'");
        analyzeExpr(*e.value);
        return Type{TypeKind::Error};
    }

    Type lhsType = sym->type;
    Type rhsType = analyzeExpr(*e.value);

    if (isError(lhsType) || isError(rhsType)) return Type{TypeKind::Error};

    bool isArith =
        e.op.type == TokenType::PLUS_EQUAL   || e.op.type == TokenType::MINUS_EQUAL  ||
        e.op.type == TokenType::STAR_EQUAL   || e.op.type == TokenType::SLASH_EQUAL  ||
        e.op.type == TokenType::PERCENT_EQUAL;

    bool isBitw =
        e.op.type == TokenType::CARET_EQUAL  ||
        e.op.type == TokenType::AMPERSAND_EQUAL ||
        e.op.type == TokenType::PIPE_EQUAL;

    if (isArith) {
        if (!isNumeric(lhsType.kind))
            error(e.op, "left operand of '" + e.op.lexeme
                  + "' must be numeric, got " + typeName(lhsType));
        if (!isNumeric(rhsType.kind))
            error(e.op, "right operand of '" + e.op.lexeme
                  + "' must be numeric, got " + typeName(rhsType));
    } else if (isBitw) {
        if (!isInteger(lhsType.kind))
            error(e.op, "left operand of '" + e.op.lexeme
                  + "' must be integer, got " + typeName(lhsType));
        if (!isInteger(rhsType.kind))
            error(e.op, "right operand of '" + e.op.lexeme
                  + "' must be integer, got " + typeName(rhsType));
    }

    checkCast(rhsType, lhsType, e.op, "compound assignment");
    return lhsType;
}

Type SemanticAnalyzer::analyzePostfix(const PostfixExpr& e) {
    Type operandType = analyzeExpr(*e.operand);

    if (!std::holds_alternative<IdentifierExpr>(*e.operand->node)) {
        error(e.op, "operand of '" + e.op.lexeme + "' must be an identifier");
        return Type{TypeKind::Error};
    }
    if (!isError(operandType) && !isNumeric(operandType.kind)) {
        error(e.op, "operand of '" + e.op.lexeme + "' must be numeric, got "
              + typeName(operandType));
        return Type{TypeKind::Error};
    }
    return operandType;
}

Type SemanticAnalyzer::analyzeCall(const CallExpr& e) {
    const Symbol* sym = symbolTable_.lookup(e.callee.lexeme);
    if (!sym) {
        error(e.callee, "undeclared function '" + e.callee.lexeme + "'");
        for (const auto& arg : e.args) analyzeExpr(*arg);
        return Type{TypeKind::Error};
    }
    if (sym->kind != Symbol::Kind::Function) {
        error(e.callee, "'" + e.callee.lexeme + "' is not a function");
        for (const auto& arg : e.args) analyzeExpr(*arg);
        return Type{TypeKind::Error};
    }

    // Check argument count
    if (e.args.size() != sym->paramTypes.size()) {
        error(e.callee, "function '" + e.callee.lexeme + "' expects "
              + std::to_string(sym->paramTypes.size()) + " argument(s), got "
              + std::to_string(e.args.size()));
        for (const auto& arg : e.args) analyzeExpr(*arg);
        return sym->type;
    }

    // Check each argument type
    for (size_t i = 0; i < e.args.size(); ++i) {
        Type argType = analyzeExpr(*e.args[i]);
        checkCast(argType, sym->paramTypes[i], e.callee,
                  "argument " + std::to_string(i + 1)
                  + " of '" + e.callee.lexeme + "'");
    }

    return sym->type;
}

Type SemanticAnalyzer::analyzeVarDecl(const VarDeclExpr& e) {
    Type declaredType = typeFromToken(e.typeName.type);

    // Redeclaration in the same scope?
    const Symbol* existing = symbolTable_.lookupCurrentScope(e.name.lexeme);
    if (existing) {
        error(e.name, "variable '" + e.name.lexeme + "' already declared in this scope"
              + " (previously declared at line "
              + std::to_string(existing->declarationToken.line) + ")");
        if (e.initializer) analyzeExpr(*e.initializer);
        return declaredType;  // return declared type so downstream uses don't compound errors
    }

    // Analyse initializer
    if (e.initializer) {
        Type initType = analyzeExpr(*e.initializer);
        checkCast(initType, declaredType, e.name, "variable initializer");
    }

    symbolTable_.declare(e.name.lexeme, Symbol{
        Symbol::Kind::Variable,
        declaredType,
        e.name,
        {}
    });

    return declaredType;
}

// ============================================================
// Helpers
// ============================================================

void SemanticAnalyzer::enterScope() { symbolTable_.enterScope(); }
void SemanticAnalyzer::exitScope()  { symbolTable_.exitScope(); }

const Symbol* SemanticAnalyzer::lookupSymbol(const Token& nameToken) {
    const Symbol* sym = symbolTable_.lookup(nameToken.lexeme);
    if (!sym) error(nameToken, "use of undeclared identifier '" + nameToken.lexeme + "'");
    return sym;
}

void SemanticAnalyzer::error(const Token& token, const std::string& message) {
    hadError_ = true;
    std::cerr << "[line " << token.line << "] Error: " << message << '\n';
}

void SemanticAnalyzer::warn(const Token& token, const std::string& message) {
    std::cerr << "[line " << token.line << "] Warning: " << message << '\n';
}

void SemanticAnalyzer::recordType(const Expr& expr, Type t) {
    typeMap_[expr.node.get()] = t;
}

void SemanticAnalyzer::checkCast(Type from, Type to,
                                  const Token& site, const std::string& context) {
    if (isError(from) || isError(to)) return;
    CastResult cr = canImplicitlyCast(from, to);
    std::string ctx = context.empty() ? "" : " in " + context;
    if (cr == CastResult::None) {
        error(site, "cannot implicitly convert " + typeName(from)
              + " to " + typeName(to) + ctx);
    } else if (cr == CastResult::Warn) {
        warn(site, "implicit conversion from " + typeName(from)
             + " to " + typeName(to) + " may lose data" + ctx);
    }
}
