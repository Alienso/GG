#include "SemanticAnalyzer.h"

// ============================================================
// Internal helper — first (leftmost) token in an expression
// Used for error reporting when the AST node stores no keyword token.
// ============================================================

const Token& exprFirstToken(const Expr& expr) {
    struct Visitor {
        const Token& operator()(const LiteralExpr& literal)            const { return literal.token; }
        const Token& operator()(const IdentifierExpr& identifier)      const { return identifier.name; }
        const Token& operator()(const UnaryExpr& unary)                const { return unary.operatorToken; }
        const Token& operator()(const BinaryExpr& binary)              const { return exprFirstToken(*binary.left); }
        const Token& operator()(const AssignExpr& assign)              const { return assign.name; }
        const Token& operator()(const CompoundAssignExpr& compoundAssign) const { return compoundAssign.name; }
        const Token& operator()(const PostfixExpr& postfix)            const { return exprFirstToken(*postfix.operand); }
        const Token& operator()(const CallExpr& call)                  const { return call.callee; }
        const Token& operator()(const VarDeclExpr& varDecl)            const { return varDecl.typeName; }
        const Token& operator()(const IndexExpr& indexExpr)            const { return exprFirstToken(*indexExpr.object); }
        const Token& operator()(const IndexAssignExpr& indexAssign)    const { return exprFirstToken(*indexAssign.object); }
        const Token& operator()(const ThisExpr& thisExpr)              const { return thisExpr.keyword; }
        const Token& operator()(const MemberAccessExpr& ma)            const { return exprFirstToken(*ma.object); }
        const Token& operator()(const MemberAssignExpr& ma)            const { return exprFirstToken(*ma.object); }
        const Token& operator()(const MethodCallExpr& mc)              const { return exprFirstToken(*mc.object); }
        const Token& operator()(const RefStoreExpr& rs)                const { return exprFirstToken(*rs.target); }
        const Token& operator()(const CastExpr& castExpr)              const { return exprFirstToken(*castExpr.operand); }
        const Token& operator()(const NewExpr& newExpr)                const { return newExpr.keyword; }
        const Token& operator()(const SizeofExpr& sizeofExpr)          const { return sizeofExpr.keyword; }
        const Token& operator()(const SwitchExpr& switchExpr)          const { return switchExpr.keyword; }
    };
    return std::visit(Visitor{}, *expr.node);
}

// Effective mutability of a parameter. `mut` on a primitive makes it reassignable; on a
// reference (`mut Point&`) it makes it a mutable borrow (its object's mut fields may be
// written). A reference parameter still may not be *rebound* (that would corrupt the
// borrow's refcount) — that is enforced separately in analyzeAssign. Object *value*
// parameters are rejected earlier (they must be references), so `mut` there is moot.
bool SemanticAnalyzer::paramIsMutable(const ParamDecl& param, const Type& /*resolvedType*/) {
    return param.isMut;
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
        [](const SwitchStmt& sw)       {
            // Exhaustive (has default) and every arm's body always returns.
            bool hasDefault = false;
            for (const SwitchArm& arm : sw.arms) {
                if (arm.isDefault) hasDefault = true;
                if (!(arm.block && alwaysReturns(*arm.block))) return false;
            }
            return hasDefault;
        },
        [](const YieldStmt&)           { return false; },  // exits the switch expr, not the fn
        [](const ExprStmt&)            { return false; },
        [](const FunctionDeclStmt&)    { return false; },
        [](const ExternFuncDeclStmt&)  { return false; },
        [](const ImportStmt&)          { return false; },
        [](const ClassDeclStmt&)       { return false; },
        [](const EnumDeclStmt&)        { return false; },
        [](const TraitDeclStmt&)       { return false; },
        [](const ImplDeclStmt&)        { return false; },
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
        [&](const SwitchStmt& switchStmt)           { analyzeSwitchStmt(switchStmt); },
        [&](const YieldStmt& yieldStmt)             { analyzeYield(yieldStmt); },
        [&](const FunctionDeclStmt& functionDecl)    { analyzeFunctionDecl(functionDecl); },
        [&](const ExternFuncDeclStmt& externDecl)    { analyzeExternFuncDecl(externDecl); },
        [&](const ImportStmt&)                       { /* resolved before semantic pass */ },
        [&](const ClassDeclStmt& classDecl)          { analyzeClassDecl(classDecl); },
        [&](const EnumDeclStmt& enumDecl)            { analyzeEnumDecl(enumDecl); },
        [&](const TraitDeclStmt&)                    { /* validated in collectTraits */ },
        [&](const ImplDeclStmt& impl)                { analyzeImplDecl(impl); },
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
        error(exprFirstToken(ifStmt.condition),
              "if condition must be bool-compatible, got " + typeName(conditionType));
    }

    // Definite-assignment analysis across branches.
    // A variable is definitely initialized after an if-else only if it is
    // initialized in BOTH the then-branch and the else-branch.
    // With no else, the then-branch may not run — so nothing is newly guaranteed.
    auto snapBefore = symbolTable.captureInitState();
    analyzeStmt(*ifStmt.thenBranch);

    if (ifStmt.elseBranch) {
        auto snapAfterThen = symbolTable.captureInitState();
        symbolTable.restoreInitState(snapBefore);
        analyzeStmt(*ifStmt.elseBranch);
        auto snapAfterElse = symbolTable.captureInitState();

        // Merge: definitely initialized iff initialized in BOTH branches.
        // (If a variable was already initialized before the if, both snapshots
        //  carry that fact, so it stays initialized.)
        SymbolTable::InitSnapshot merged;
        for (const auto& [name, initThen] : snapAfterThen) {
            auto it = snapAfterElse.find(name);
            bool initElse = (it != snapAfterElse.end()) && it->second;
            merged[name] = initThen && initElse;
        }
        symbolTable.restoreInitState(merged);
    } else {
        // No else branch: then-branch may not run — revert to pre-if state.
        symbolTable.restoreInitState(snapBefore);
    }
}

void SemanticAnalyzer::analyzeWhile(const WhileStmt& whileStmt) {
    Type conditionType = analyzeExpr(whileStmt.condition);
    if (!isError(conditionType) && !isBoolCompatible(conditionType)) {
        error(exprFirstToken(whileStmt.condition),
              "while condition must be bool-compatible, got " + typeName(conditionType));
    }
    // The loop body may never execute, so assignments inside it do not count as
    // definite initialization.  Analyse the body (to catch errors in it) but then
    // restore the pre-loop initialization state.
    auto snapBefore = symbolTable.captureInitState();
    loopDepth++;
    analyzeStmt(*whileStmt.body);
    loopDepth--;
    symbolTable.restoreInitState(snapBefore);
}

void SemanticAnalyzer::analyzeFor(const ForStmt& forStmt) {
    enterScope();   // scope for the init variable

    if (forStmt.init) analyzeStmt(*forStmt.init);

    if (forStmt.condition) {
        Type conditionType = analyzeExpr(*forStmt.condition);
        if (!isError(conditionType) && !isBoolCompatible(conditionType)) {
            error(exprFirstToken(*forStmt.condition),
                  "for condition must be bool-compatible, got " + typeName(conditionType));
        }
    }

    if (forStmt.increment) analyzeExpr(*forStmt.increment);

    // The loop body may never execute: analyse it (to catch errors) but restore
    // the pre-body initialization state so nothing is spuriously deemed initialized.
    auto snapBefore = symbolTable.captureInitState();
    loopDepth++;
    analyzeStmt(*forStmt.body);
    loopDepth--;
    symbolTable.restoreInitState(snapBefore);

    exitScope();
}

// Return-alias body setup. The alias is a named result binding declared in the function
// scope. Object aliases lower to a caller-allocated sret slot; primitive aliases are
// zero-initialized locals; reference aliases are null-initialized and MUST be assigned
// before being returned (isInitialized=false ⇒ the definite-assignment analysis flags it).
void SemanticAnalyzer::setupReturnSlot(bool hasReturnSlot, const std::string& slotName,
                                       const Type& returnType, const Token& nameToken) {
    currentReturnSlotName_.clear();
    currentReturnAliasIsRef_ = false;
    if (hasReturnSlot) {
        if (returnType.kind == TypeKind::Void) {
            error(nameToken, "a void function cannot declare a return alias");
            return;
        }
        currentReturnSlotName_   = slotName;
        currentReturnAliasIsRef_ = (returnType.kind == TypeKind::Reference);
        bool initialized = (returnType.kind != TypeKind::Reference);   // ref must be assigned first
        Token slotTok{TokenType::IDENTIFIER, slotName, nameToken.line};
        symbolTable.declare(slotName, Symbol{
            Symbol::Kind::Variable, returnType, slotTok, {},
            /*isParameter=*/false, /*isInitialized=*/initialized, /*isMutable=*/true});
    } else if (returnType.kind == TypeKind::Object) {
        error(nameToken, "returning an object by value requires a return alias: '"
              + nameToken.lexeme + "(...) -> " + returnType.className + " alias'");
    }
}

void SemanticAnalyzer::analyzeReturn(const ReturnStmt& returnStmt) {
    if (!currentReturnType) {
        error(returnStmt.keyword, "return statement outside of function");
        return;
    }

    // Return-alias function: only `return <alias>;` or bare `return;` — the result is the
    // named alias binding.
    if (!currentReturnSlotName_.empty()) {
        if (returnStmt.value) {
            const auto& v = *returnStmt.value->node;
            if (!std::holds_alternative<IdentifierExpr>(v)
                || std::get<IdentifierExpr>(v).name.lexeme != currentReturnSlotName_) {
                error(returnStmt.keyword, "a function with a return alias may only 'return "
                      + currentReturnSlotName_ + ";' or 'return;'");
                return;
            }
        }
        // A reference alias must be definitely assigned before it is returned.
        if (currentReturnAliasIsRef_) {
            const Symbol* aliasSym = symbolTable.lookup(currentReturnSlotName_);
            if (aliasSym && !aliasSym->isInitialized)
                error(returnStmt.keyword, "return alias '" + currentReturnSlotName_
                      + "' is returned before it is assigned");
        }
        return;
    }

    if (!returnStmt.value) {
        if (currentReturnType->kind != TypeKind::Void) {
            error(returnStmt.keyword, "return with no value in function returning "
                  + typeName(*currentReturnType));
        }
        return;
    }

    // The function's return type is the overload-resolution context for the value.
    Type actualType = analyzeWithExpected(*returnStmt.value, *currentReturnType);
    checkCast(actualType, *currentReturnType, returnStmt.keyword, "return");
}

void SemanticAnalyzer::analyzeSwitchStmt(const SwitchStmt& switchStmt) {
    Type scrutineeType = analyzeExpr(switchStmt.scrutinee);
    checkDuplicateLabels(switchStmt.arms);
    // Statement form: `default` optional, no exhaustiveness requirement.
    for (const SwitchArm& arm : switchStmt.arms)
        analyzeSwitchArm(arm, scrutineeType, /*expectedResult=*/nullptr, switchStmt.keyword);
}

void SemanticAnalyzer::analyzeYield(const YieldStmt& yieldStmt) {
    if (switchExprResultStack_.empty()) {
        error(yieldStmt.keyword, "'yield' is only valid inside a switch expression arm");
        analyzeExpr(yieldStmt.value);
        return;
    }
    Type expected = switchExprResultStack_.back();
    Type v = (expected.kind == TypeKind::Error)
               ? analyzeExpr(yieldStmt.value)
               : analyzeWithExpected(yieldStmt.value, expected);
    if (!isError(expected) && !isError(v))
        checkCast(v, expected, yieldStmt.keyword, "yield value");
}

// A reference return alias that can be reached by fall-through must be definitely assigned.
void SemanticAnalyzer::checkReturnAliasAssignedAtExit(const BlockStmt& body, const Token& where) {
    if (!currentReturnAliasIsRef_ || currentReturnSlotName_.empty()) return;
    if (alwaysReturns(body)) return;   // end is unreachable — each return already checked
    const Symbol* aliasSym = symbolTable.lookup(currentReturnSlotName_);
    if (aliasSym && !aliasSym->isInitialized)
        error(where, "return alias '" + currentReturnSlotName_
              + "' may be unassigned when the function returns");
}

void SemanticAnalyzer::analyzeParamDefaults(const std::vector<ParamDecl>& params) {
    // Analyze defaults with no params / this / instance fields visible: clear currentClassName so a
    // bare name can't resolve to an instance field, and do it before params are declared in scope.
    std::string savedClassName = currentClassName;
    currentClassName = "";
    for (const ParamDecl& p : params) {
        if (!p.defaultValue) continue;
        Type valueType = analyzeExpr(*p.defaultValue);
        Type paramType = resolveTypeToken(p.typeName);
        checkCast(valueType, paramType, p.name,
                  "default value of parameter '" + p.name.lexeme + "'");
    }
    currentClassName = savedClassName;
}

void SemanticAnalyzer::analyzeFunctionDecl(const FunctionDeclStmt& functionDecl) {
    // Signature is already registered in the global scope by collectFunctions.
    analyzeParamDefaults(functionDecl.params);   // before the function scope opens (no params visible)
    std::optional<Type> savedReturnType = currentReturnType;
    int                 savedLoopDepth  = loopDepth;
    std::string         savedSlot       = currentReturnSlotName_;
    bool                savedAliasRef   = currentReturnAliasIsRef_;
    currentReturnType = resolveTypeToken(functionDecl.returnType);
    loopDepth         = 0;  // loops in the outer scope do not extend into this function

    // Gate raw pointer types behind --unsafe-ptr.
    checkRawPtrAllowed(functionDecl.returnType, functionDecl.name);

    enterScope();  // function scope — parameters live here

    // Arrow-form return slot: inject the slot binding; else reject a bare object return.
    setupReturnSlot(functionDecl.hasReturnSlot, functionDecl.returnSlotName,
                    *currentReturnType, functionDecl.name);

    for (const ParamDecl& param : functionDecl.params) {
        checkRawPtrAllowed(param.typeName, param.name);
        Type paramType = resolveTypeToken(param.typeName);
        if (paramType.kind == TypeKind::Void) {
            error(param.typeName, "parameter '" + param.name.lexeme + "' cannot have type 'void'");
            paramType = Type{TypeKind::Error};  // suppress cascading errors in the body
        }
        if (paramType.kind == TypeKind::Object) {
            error(param.typeName, "object parameter '" + param.name.lexeme
                  + "' must be passed by reference; declare it as '" + paramType.className + "&'");
            paramType = Type{TypeKind::Error};
        }
        bool paramMutable = paramIsMutable(param, paramType);
        Symbol sym{
            Symbol::Kind::Variable,
            paramType,
            param.name,
            {},
            /*isParameter=*/true,
            /*isInitialized=*/true,   // parameters are always initialized at call entry
            /*isMutable=*/paramMutable
        };
        if (!symbolTable.declare(param.name.lexeme, sym))
            error(param.name, "duplicate parameter name '" + param.name.lexeme + "'");
    }

    // Analyse body statements directly — do NOT call analyzeBlock to avoid
    // opening a second scope on top of the function scope.
    for (const auto& statement : functionDecl.body.body) analyzeStmt(*statement);

    // Warn when a non-void function may fall off the end without returning.
    // This is conservative: loops are never treated as guaranteed returns.
    // Arrow-form (slot) functions are exempt — the slot is always a valid result.
    if (currentReturnSlotName_.empty()
        && currentReturnType->kind != TypeKind::Void && !alwaysReturns(functionDecl.body))
        warn(functionDecl.name, "function '" + functionDecl.name.lexeme
             + "' does not always return a value");
    checkReturnAliasAssignedAtExit(functionDecl.body, functionDecl.name);

    exitScope();

    currentReturnType        = savedReturnType;
    loopDepth                = savedLoopDepth;
    currentReturnSlotName_   = savedSlot;
    currentReturnAliasIsRef_ = savedAliasRef;
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

    // Gate raw pointer field types behind --unsafe-ptr, and type-check the constant
    // initializer of each static field against its declared type.
    for (const FieldDecl& fd : classDecl.fields) {
        checkRawPtrAllowed(fd.typeName, fd.name);
        if (fd.isStatic && fd.initializer) {
            Type fieldType = resolveTypeToken(fd.typeName);
            Type initType  = analyzeExpr(*fd.initializer);
            checkCast(initType, fieldType, fd.name, "static field initializer");
        }
    }

    for (const MethodDecl& md : classDecl.methods) {
        analyzeParamDefaults(md.params);   // defaults can't see params / this / fields
        std::optional<Type> savedReturnType = currentReturnType;
        int                 savedLoopDepth  = loopDepth;
        bool                savedStatic     = currentMethodIsStatic;
        bool                savedInCtor     = inConstructor;
        bool                savedThisMut    = currentThisMutable;
        std::string         savedSlot       = currentReturnSlotName_;
        bool                savedAliasRef   = currentReturnAliasIsRef_;
        currentMethodIsStatic               = md.isStatic;
        inConstructor                       = md.isConstructor;
        // `this` is mutable inside a `mut` method, a constructor, or a destructor.
        currentThisMutable                  = md.isMut || md.isConstructor || md.isDestructor;

        currentReturnType = (md.isConstructor || md.isDestructor)
                                ? Type{TypeKind::Void}
                                : resolveTypeToken(md.returnType);
        loopDepth         = 0;

        // Gate raw pointer types in method signatures behind --unsafe-ptr.
        if (!md.isConstructor && !md.isDestructor)
            checkRawPtrAllowed(md.returnType, md.name);
        for (const ParamDecl& param : md.params)
            checkRawPtrAllowed(param.typeName, param.name);

        enterScope();  // method scope

        // Inject 'this' as a variable with type Object{className}. Static methods
        // are class-level: they have no receiver, so no 'this' is in scope.
        if (!md.isStatic) {
            symbolTable.declare("this", Symbol{
                Symbol::Kind::Variable,
                makeObjectType(className),
                classDecl.name,
                {},
                /*isParameter=*/false,
                /*isInitialized=*/true   // 'this' is always valid inside a method
            });
        }

        // Declare parameters
        for (const ParamDecl& param : md.params) {
            Type paramType = resolveTypeToken(param.typeName);
            if (paramType.kind == TypeKind::Void) {
                error(param.typeName, "parameter '" + param.name.lexeme + "' cannot have type 'void'");
                paramType = Type{TypeKind::Error};
            }
            if (paramType.kind == TypeKind::Object) {
                error(param.typeName, "object parameter '" + param.name.lexeme
                      + "' must be passed by reference; declare it as '" + paramType.className + "&'");
                paramType = Type{TypeKind::Error};
            }
            if (!symbolTable.declare(param.name.lexeme, Symbol{
                    Symbol::Kind::Variable, paramType, param.name, {},
                    /*isParameter=*/true, /*isInitialized=*/true,
                    /*isMutable=*/paramIsMutable(param, paramType)}))
                error(param.name, "duplicate parameter name '" + param.name.lexeme + "'");
        }

        // Arrow-form return slot: inject the slot binding; else reject a bare object return.
        setupReturnSlot(md.hasReturnSlot, md.returnSlotName, *currentReturnType, md.name);

        // Analyse body
        for (const auto& stmtPtr : md.body.body) analyzeStmt(*stmtPtr);

        // Warn on missing return for non-void non-constructor non-destructor methods
        // (arrow-form slot methods are exempt — the slot is always a valid result).
        if (!md.isConstructor && !md.isDestructor && currentReturnSlotName_.empty()
            && currentReturnType->kind != TypeKind::Void
            && !alwaysReturns(md.body)) {
            warn(md.name, "method '" + md.name.lexeme
                 + "' does not always return a value");
        }
        if (!md.isConstructor && !md.isDestructor)
            checkReturnAliasAssignedAtExit(md.body, md.name);

        exitScope();

        currentReturnType        = savedReturnType;
        loopDepth                = savedLoopDepth;
        currentMethodIsStatic    = savedStatic;
        inConstructor            = savedInCtor;
        currentThisMutable       = savedThisMut;
        currentReturnSlotName_   = savedSlot;
        currentReturnAliasIsRef_ = savedAliasRef;
    }

    currentClassName = savedClassName;
}

// Analyse the method bodies of an `impl Trait for Type { … }` block. The methods were
// already registered onto the target class in collectImpls; here we type-check bodies with
// `this`/`Self` bound to the target type. Mirrors analyzeClassDecl's per-method loop.
void SemanticAnalyzer::analyzeImplDecl(const ImplDeclStmt& impl) {
    const std::string& type = impl.typeName.lexeme;
    if (!classRegistry.count(type)) return;   // unknown target — collectImpls already reported it

    std::string savedClassName = currentClassName;
    std::string savedSelfType  = currentSelfType_;
    currentClassName = type;
    currentSelfType_ = type;

    for (const MethodDecl& md : impl.methods) {
        if (!md.hasBody) continue;   // impl methods always have bodies; defensive

        std::optional<Type> savedReturnType = currentReturnType;
        int                 savedLoopDepth  = loopDepth;
        bool                savedStatic     = currentMethodIsStatic;
        bool                savedInCtor     = inConstructor;
        bool                savedThisMut    = currentThisMutable;
        std::string         savedSlot       = currentReturnSlotName_;
        bool                savedAliasRef   = currentReturnAliasIsRef_;
        currentMethodIsStatic = md.isStatic;
        inConstructor         = false;
        currentThisMutable    = md.isMut;
        currentReturnType     = resolveTypeToken(md.returnType);
        loopDepth             = 0;

        checkRawPtrAllowed(md.returnType, md.name);
        for (const ParamDecl& param : md.params)
            checkRawPtrAllowed(param.typeName, param.name);

        enterScope();
        if (!md.isStatic)
            symbolTable.declare("this", Symbol{
                Symbol::Kind::Variable, makeObjectType(type), impl.typeName, {},
                /*isParameter=*/false, /*isInitialized=*/true});

        for (const ParamDecl& param : md.params) {
            Type paramType = resolveTypeToken(param.typeName);
            if (paramType.kind == TypeKind::Void) {
                error(param.typeName, "parameter '" + param.name.lexeme + "' cannot have type 'void'");
                paramType = Type{TypeKind::Error};
            }
            if (paramType.kind == TypeKind::Object) {
                error(param.typeName, "object parameter '" + param.name.lexeme
                      + "' must be passed by reference; declare it as '" + paramType.className + "&'");
                paramType = Type{TypeKind::Error};
            }
            if (!symbolTable.declare(param.name.lexeme, Symbol{
                    Symbol::Kind::Variable, paramType, param.name, {},
                    /*isParameter=*/true, /*isInitialized=*/true,
                    /*isMutable=*/paramIsMutable(param, paramType)}))
                error(param.name, "duplicate parameter name '" + param.name.lexeme + "'");
        }

        setupReturnSlot(md.hasReturnSlot, md.returnSlotName, *currentReturnType, md.name);

        for (const auto& stmtPtr : md.body.body) analyzeStmt(*stmtPtr);

        if (currentReturnSlotName_.empty()
            && currentReturnType->kind != TypeKind::Void && !alwaysReturns(md.body))
            warn(md.name, "method '" + md.name.lexeme + "' does not always return a value");
        checkReturnAliasAssignedAtExit(md.body, md.name);

        exitScope();

        currentReturnType        = savedReturnType;
        loopDepth                = savedLoopDepth;
        currentMethodIsStatic    = savedStatic;
        inConstructor            = savedInCtor;
        currentThisMutable       = savedThisMut;
        currentReturnSlotName_   = savedSlot;
        currentReturnAliasIsRef_ = savedAliasRef;
    }

    currentClassName = savedClassName;
    currentSelfType_ = savedSelfType;
}

// ============================================================
// Enum analysis
// ============================================================

// Collect the names of fields assigned via `this.field = ...` anywhere in a
// statement/expression tree. Used to verify an enum constructor initialises every
// field. Conditional assignments count (a loose but useful phase-1 check).
static void collectThisAssignedFields(const Stmt& stmt, std::unordered_set<std::string>& out);

static void collectThisAssignedFields(const BlockStmt& block, std::unordered_set<std::string>& out) {
    for (const auto& s : block.body) if (s) collectThisAssignedFields(*s, out);
}

static void collectThisAssignedFields(const Expr& e, std::unordered_set<std::string>& out) {
    if (!e.node) return;
    if (const auto* ma = std::get_if<MemberAssignExpr>(e.node.get())) {
        if (ma->object && ma->object->node
            && std::holds_alternative<ThisExpr>(*ma->object->node))
            out.insert(ma->field.lexeme);
    }
}

static void collectThisAssignedFields(const Stmt& stmt, std::unordered_set<std::string>& out) {
    std::visit(overloaded{
        [&](const ExprStmt& s)  { collectThisAssignedFields(s.expression, out); },
        [&](const BlockStmt& s) { collectThisAssignedFields(s, out); },
        [&](const IfStmt& s)    {
            if (s.thenBranch) collectThisAssignedFields(*s.thenBranch, out);
            if (s.elseBranch) collectThisAssignedFields(*s.elseBranch, out);
        },
        [&](const WhileStmt& s) { if (s.body) collectThisAssignedFields(*s.body, out); },
        [&](const ForStmt& s)   { if (s.body) collectThisAssignedFields(*s.body, out); },
        [&](const auto&)        { },
    }, *stmt.node);
}

void SemanticAnalyzer::analyzeEnumDecl(const EnumDeclStmt& enumDecl) {
    const std::string& enumName = enumDecl.name.lexeme;

    std::string savedClassName = currentClassName;
    bool        savedIsEnum    = currentClassIsEnum;
    currentClassName   = enumName;
    currentClassIsEnum = true;

    // Gate raw pointer field types behind --unsafe-ptr.
    for (const FieldDecl& fd : enumDecl.fields)
        checkRawPtrAllowed(fd.typeName, fd.name);

    // Locate the constructor (if any) and resolve its parameter types.
    const MethodDecl* ctor = nullptr;
    for (const MethodDecl& md : enumDecl.methods)
        if (md.isConstructor) { ctor = &md; break; }

    std::vector<Type> ctorParamTypes;
    if (ctor)
        for (const ParamDecl& p : ctor->params)
            ctorParamTypes.push_back(resolveTypeToken(p.typeName));

    // An enum with fields must have a constructor to initialise them.
    if (!enumDecl.fields.empty() && !ctor)
        error(enumDecl.name, "enum '" + enumName
              + "' has fields but no constructor to initialise them");

    // Validate each variant's argument list against the constructor, and
    // type-check the argument expressions (evaluated at static-init time).
    for (const EnumVariant& v : enumDecl.variants) {
        size_t expected = ctorParamTypes.size();
        if (v.args.size() != expected)
            error(v.name, "enum variant '" + v.name.lexeme + "' passes "
                  + std::to_string(v.args.size()) + " argument(s) but the constructor expects "
                  + std::to_string(expected));
        for (size_t i = 0; i < v.args.size(); ++i) {
            Type argType = analyzeExpr(*v.args[i]);
            if (i < expected)
                checkCast(argType, ctorParamTypes[i], v.name,
                          "argument " + std::to_string(i + 1)
                          + " of enum variant '" + v.name.lexeme + "'");
        }
    }

    // Verify the constructor initialises every field.
    if (ctor && !enumDecl.fields.empty()) {
        std::unordered_set<std::string> assigned;
        collectThisAssignedFields(ctor->body, assigned);
        for (const FieldDecl& fd : enumDecl.fields)
            if (!assigned.count(fd.name.lexeme))
                error(fd.name, "enum field '" + fd.name.lexeme
                      + "' is not initialised in the constructor of '" + enumName + "'");
    }

    // Analyse method bodies (constructor + regular methods).
    for (const MethodDecl& md : enumDecl.methods) {
        std::optional<Type> savedReturnType = currentReturnType;
        int                 savedLoopDepth  = loopDepth;
        bool                savedInCtor     = inEnumConstructor;
        std::string         savedSlot       = currentReturnSlotName_;
        bool                savedAliasRef   = currentReturnAliasIsRef_;

        currentReturnType = md.isConstructor ? Type{TypeKind::Void}
                                             : resolveTypeToken(md.returnType);
        loopDepth         = 0;
        inEnumConstructor = md.isConstructor;

        if (!md.isConstructor)
            checkRawPtrAllowed(md.returnType, md.name);
        for (const ParamDecl& param : md.params)
            checkRawPtrAllowed(param.typeName, param.name);

        enterScope();  // method scope

        // 'this' inside an enum method is the enum value itself (a pointer to the singleton).
        symbolTable.declare("this", Symbol{
            Symbol::Kind::Variable, makeEnumType(enumName), enumDecl.name, {},
            /*isParameter=*/false, /*isInitialized=*/true});

        for (const ParamDecl& param : md.params) {
            Type paramType = resolveTypeToken(param.typeName);
            if (paramType.kind == TypeKind::Void) {
                error(param.typeName, "parameter '" + param.name.lexeme + "' cannot have type 'void'");
                paramType = Type{TypeKind::Error};
            }
            if (paramType.kind == TypeKind::Object) {
                error(param.typeName, "object parameter '" + param.name.lexeme
                      + "' must be passed by reference; declare it as '" + paramType.className + "&'");
                paramType = Type{TypeKind::Error};
            }
            if (!symbolTable.declare(param.name.lexeme, Symbol{
                    Symbol::Kind::Variable, paramType, param.name, {},
                    /*isParameter=*/true, /*isInitialized=*/true,
                    /*isMutable=*/paramIsMutable(param, paramType)}))
                error(param.name, "duplicate parameter name '" + param.name.lexeme + "'");
        }

        if (!md.isConstructor)
            setupReturnSlot(md.hasReturnSlot, md.returnSlotName, *currentReturnType, md.name);

        for (const auto& stmtPtr : md.body.body) analyzeStmt(*stmtPtr);

        if (!md.isConstructor && currentReturnSlotName_.empty()
            && currentReturnType->kind != TypeKind::Void && !alwaysReturns(md.body))
            warn(md.name, "method '" + md.name.lexeme + "' does not always return a value");
        if (!md.isConstructor)
            checkReturnAliasAssignedAtExit(md.body, md.name);

        exitScope();
        currentReturnType        = savedReturnType;
        loopDepth                = savedLoopDepth;
        inEnumConstructor        = savedInCtor;
        currentReturnSlotName_   = savedSlot;
        currentReturnAliasIsRef_ = savedAliasRef;
    }

    currentClassName   = savedClassName;
    currentClassIsEnum = savedIsEnum;
}
