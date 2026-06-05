#include "SemanticAnalyzer.h"

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
        const Token& operator()(const IndexExpr& indexExpr)            const { return indexExpr.name; }
        const Token& operator()(const IndexAssignExpr& indexAssign)    const { return indexAssign.name; }
        const Token& operator()(const ThisExpr& thisExpr)              const { return thisExpr.keyword; }
        const Token& operator()(const MemberAccessExpr& ma)            const { return firstToken(*ma.object); }
        const Token& operator()(const MemberAssignExpr& ma)            const { return firstToken(*ma.object); }
        const Token& operator()(const MethodCallExpr& mc)              const { return firstToken(*mc.object); }
        const Token& operator()(const CastExpr& castExpr)              const { return firstToken(*castExpr.operand); }
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
        [](const ClassDeclStmt&)       { return false; },
    }, *stmt.node);
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
        [&](const ClassDeclStmt& classDecl)          { analyzeClassDecl(classDecl); },
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
    currentReturnType = resolveTypeToken(functionDecl.returnType);
    loopDepth         = 0;  // loops in the outer scope do not extend into this function

    enterScope();  // function scope — parameters live here

    for (const ParamDecl& param : functionDecl.params) {
        Type paramType = resolveTypeToken(param.typeName);
        if (paramType.kind == TypeKind::Void) {
            error(param.typeName, "parameter '" + param.name.lexeme + "' cannot have type 'void'");
            paramType = Type{TypeKind::Error};  // suppress cascading errors in the body
        }
        Symbol sym{
            Symbol::Kind::Variable,
            paramType,
            param.name,
            {},
            /*isParameter=*/true
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

void SemanticAnalyzer::analyzeClassDecl(const ClassDeclStmt& classDecl) {
    // Registry was built in collectClasses. Here we fully analyse each method body.
    const std::string& className = classDecl.name.lexeme;

    std::string savedClassName = currentClassName;
    currentClassName          = className;

    for (const MethodDecl& md : classDecl.methods) {
        std::optional<Type> savedReturnType = currentReturnType;
        int                 savedLoopDepth  = loopDepth;

        currentReturnType = (md.isConstructor || md.isDestructor)
                                ? Type{TypeKind::Void}
                                : resolveTypeToken(md.returnType);
        loopDepth         = 0;

        enterScope();  // method scope

        // Inject 'this' as a variable with type Object{className}
        symbolTable.declare("this", Symbol{
            Symbol::Kind::Variable,
            makeObjectType(className),
            classDecl.name,
            {}
        });

        // Declare parameters
        for (const ParamDecl& param : md.params) {
            Type paramType = resolveTypeToken(param.typeName);
            if (paramType.kind == TypeKind::Void) {
                error(param.typeName, "parameter '" + param.name.lexeme + "' cannot have type 'void'");
                paramType = Type{TypeKind::Error};
            }
            if (!symbolTable.declare(param.name.lexeme, Symbol{
                    Symbol::Kind::Variable, paramType, param.name, {}, /*isParameter=*/true}))
                error(param.name, "duplicate parameter name '" + param.name.lexeme + "'");
        }

        // Analyse body
        for (const auto& stmtPtr : md.body.body) analyzeStmt(*stmtPtr);

        // Warn on missing return for non-void non-constructor non-destructor methods
        if (!md.isConstructor && !md.isDestructor
            && currentReturnType->kind != TypeKind::Void
            && !alwaysReturns(md.body)) {
            warn(md.name, "method '" + md.name.lexeme
                 + "' does not always return a value");
        }

        exitScope();

        currentReturnType = savedReturnType;
        loopDepth         = savedLoopDepth;
    }

    currentClassName = savedClassName;
}
