//
// Created by Vladimir Arsenijevic on 01.6.2026.
//

#include "SemanticAnalyzer.h"
#include <iostream>

// ============================================================
// Public entry point
// ============================================================

SemanticResult SemanticAnalyzer::analyze(const Program& program,
                                          const std::string& filenameParam,
                                          const CompilerOptions& options) {
    symbolTable       = SymbolTable{};
    typeMap.clear();
    hadError          = false;
    filename          = filenameParam;
    currentReturnType = std::nullopt;
    loopDepth         = 0;
    currentClassName  = "";
    classRegistry.clear();
    allowRawPtr_      = options.allowRawPtr;

    symbolTable.enterScope();   // global scope

    collectClasses(program);    // pass 0: build class registry
    collectFunctions(program);  // pass 1: hoist function signatures

    for (const Stmt& stmt : program.declarations)
        analyzeStmt(stmt);      // pass 2: full analysis

    symbolTable.exitScope();

    return SemanticResult{hadError, std::move(typeMap), classRegistry };
}

// ============================================================
// Pass 0 — collect class declarations into classRegistry_
// ============================================================

void SemanticAnalyzer::collectClasses(const Program& program) {
    for (const Stmt& stmt : program.declarations) {
        if (!std::holds_alternative<ClassDeclStmt>(*stmt.node)) continue;
        const auto& cls = std::get<ClassDeclStmt>(*stmt.node);

        ClassInfo info;
        int fieldIndex = 0;
        for (const FieldDecl& fd : cls.fields) {
            const std::string& lex = fd.typeName.lexeme;
            Type fieldType;
            Type synth = decodeSynthesizedType(fd.typeName);
            if (!isError(synth)) {
                // Reference field (Class&) or typed-pointer field (ptr<T>).
                fieldType = synth;
            } else if (fd.typeName.type == TokenType::IDENTIFIER) {
                // Bare class name → value-object field (embedding) — not supported yet.
                error(fd.name, "object value fields are not supported; declare it as a reference '"
                      + lex + "& " + fd.name.lexeme + "'");
                fieldType = Type{TypeKind::Error};
            } else {
                fieldType = typeFromToken(fd.typeName.type);  // primitive / ptr
            }
            info.fieldOrder.push_back(fd.name.lexeme);
            // emplace constructs in-place, avoiding copy/move-assignment of Token
            info.fields.emplace(fd.name.lexeme,
                ClassInfo::Field{fd.isPublic, fieldType, fieldIndex++, fd.name});
        }
        for (const MethodDecl& md : cls.methods) {
            if (md.isDestructor) {
                // Validate: no params, at most one destructor per class.
                if (!md.params.empty()) {
                    error(md.name, "destructor '" + cls.name.lexeme + "::~" + md.name.lexeme
                          + "' must take no parameters");
                    continue;
                }
                if (info.destructor.has_value()) {
                    error(md.name, "class '" + cls.name.lexeme
                          + "' already has a destructor (duplicate ~" + md.name.lexeme + ")");
                    continue;
                }
                info.destructor.emplace(ClassInfo::Method{
                    md.isPublic, Type{TypeKind::Void}, std::vector<Type>{}, md.name
                });
                continue;
            }
            Type returnType = md.isConstructor
                ? Type{TypeKind::Void}
                : resolveTypeToken(md.returnType);
            std::vector<Type> paramTypes;
            for (const ParamDecl& p : md.params)
                paramTypes.push_back(resolveTypeToken(p.typeName));
            info.methods.emplace(md.name.lexeme,
                ClassInfo::Method{md.isPublic, returnType, std::move(paramTypes), md.name});
        }
        classRegistry.emplace(cls.name.lexeme, std::move(info));
    }
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
                paramTypes.push_back(resolveTypeToken(param.typeName));

            Symbol sym{
                Symbol::Kind::Function,
                resolveTypeToken(function.returnType),
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
    std::string prefix = filename.empty() ? "" : filename + ":";
    std::string formatted = prefix + std::to_string(token.line) + ": error: " + message;
    throw CompileError(formatted);
}

void SemanticAnalyzer::warn(const Token& token, const std::string& message) {
    std::string prefix = filename.empty() ? "" : filename + ":";
    std::cerr << prefix << token.line << ": Warning: " << message << '\n';
}

void SemanticAnalyzer::checkRawPtrAllowed(const Token& typeToken, const Token& site) {
    if (allowRawPtr_) return;
    Type t = resolveTypeToken(typeToken);
    if (t.kind == TypeKind::Ptr || t.kind == TypeKind::TypedPtr) {
        error(site, "'" + typeName(t) + "' is a raw pointer type and requires --unsafe-ptr "
              "(raw pointers are for stdlib/internal use only)");
    }
}

Type SemanticAnalyzer::resolveTypeToken(const Token& typeToken) const {
    // Parser-synthesized types: "<Class>&" (Reference) and "ptr<Elem>" (TypedPtr).
    Type synth = decodeSynthesizedType(typeToken);
    if (!isError(synth)) return synth;
    if (typeToken.type == TokenType::IDENTIFIER && classRegistry.count(typeToken.lexeme))
        return makeObjectType(typeToken.lexeme);
    return typeFromToken(typeToken.type);
}

void SemanticAnalyzer::recordType(const Expr& expr, const Type& type) {
    typeMap[expr.node.get()] = type;
}

void SemanticAnalyzer::checkCast(const Type& from, const Type& to,
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

const ClassInfo* SemanticAnalyzer::lookupObjectClass(Type objectType, const Token& site) {
    // Both value objects (Point) and heap references (Point&) carry a className
    // and support member/method access.
    if (objectType.kind != TypeKind::Object && objectType.kind != TypeKind::Reference) {
        error(site, "member access on non-class type '" + typeName(objectType) + "'");
        return nullptr;
    }
    auto it = classRegistry.find(objectType.className);
    if (it == classRegistry.end()) {
        error(site, "unknown class '" + objectType.className + "'");
        return nullptr;
    }
    return &it->second;
}

void SemanticAnalyzer::analyzeCallArgs(
    const std::vector<std::unique_ptr<Expr>>& args,
    const std::vector<Type>& paramTypes,
    const Token& callee,
    const std::string& context)
{
    for (size_t i = 0; i < args.size(); ++i) {
        Type argType = analyzeExpr(*args[i]);
        checkCast(argType, paramTypes[i], callee,
                  "argument " + std::to_string(i + 1) + " of " + context);
    }
}

void SemanticAnalyzer::checkConstantIndexBounds(
    const Expr& indexExpr, size_t arraySize)
{
    if (!indexExpr.node) return;
    if (!std::holds_alternative<LiteralExpr>(*indexExpr.node)) return;
    const auto& lit = std::get<LiteralExpr>(*indexExpr.node);
    if (lit.token.type != TokenType::NUMBER) return;
    if (lit.token.lexeme.find('.') != std::string::npos) return;
    long long idx = 0;
    try {
        idx = std::stoll(lit.token.lexeme);
    } catch (const std::exception&) {
        return;  // non-integer or overflow — skip bounds check
    }
    if (idx < 0 || static_cast<size_t>(idx) >= arraySize)
        error(lit.token, "array index " + lit.token.lexeme
              + " is out of bounds for array of size " + std::to_string(arraySize));
}
