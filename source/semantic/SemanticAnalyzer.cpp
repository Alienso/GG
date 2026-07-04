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
    currentClassIsEnum = false;
    inEnumConstructor  = false;
    classRegistry.clear();
    enumRegistry.clear();
    allowRawPtr_      = options.allowRawPtr;

    symbolTable.enterScope();   // global scope

    collectClasses(program);    // pass 0: build class registry
    collectFunctions(program);  // pass 1: hoist function signatures

    for (const Stmt& stmt : program.declarations)
        analyzeStmt(stmt);      // pass 2: full analysis

    symbolTable.exitScope();

    return SemanticResult{hadError, std::move(typeMap), classRegistry, enumRegistry };
}

// ============================================================
// Pass 0 — collect class declarations into classRegistry_
// ============================================================

// Build a ClassInfo (fields + methods + optional destructor) shared by classes and
// enums. `ownerName` is used in error messages; `allowDestructor` is false for enums.
ClassInfo SemanticAnalyzer::buildClassInfo(const std::string& ownerName,
                                           const std::deque<FieldDecl>& fields,
                                           const std::deque<MethodDecl>& methods,
                                           bool allowDestructor) {
    ClassInfo info;
    int fieldIndex = 0;
    for (const FieldDecl& fd : fields) {
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
        // Static fields are class-level storage (a global), not part of the struct
        // layout — they get no struct index and live in a separate registry.
        // (allowDestructor is false only for enums, which don't support statics yet.)
        if (fd.isStatic) {
            if (!allowDestructor) {
                error(fd.name, "enums cannot declare static fields");
                continue;
            }
            if (info.staticFields.count(fd.name.lexeme) || info.fields.count(fd.name.lexeme)) {
                error(fd.name, "duplicate field '" + fd.name.lexeme
                      + "' in '" + ownerName + "'");
                continue;
            }
            info.staticFields.emplace(fd.name.lexeme,
                ClassInfo::StaticField{fd.isPublic, fieldType, fd.name,
                                       /*hasInit=*/fd.initializer != nullptr});
            continue;
        }
        info.fieldOrder.push_back(fd.name.lexeme);
        // emplace constructs in-place, avoiding copy/move-assignment of Token
        info.fields.emplace(fd.name.lexeme,
            ClassInfo::Field{fd.isPublic, fd.isMut, fieldType, fieldIndex++, fd.name});
    }
    for (const MethodDecl& md : methods) {
        if (md.isDestructor) {
            if (!allowDestructor) {
                error(md.name, "enums cannot declare a destructor");
                continue;
            }
            // Validate: no params, at most one destructor per class.
            if (!md.params.empty()) {
                error(md.name, "destructor '" + ownerName + "::~" + md.name.lexeme
                      + "' must take no parameters");
                continue;
            }
            if (info.destructor.has_value()) {
                error(md.name, "class '" + ownerName
                      + "' already has a destructor (duplicate ~" + md.name.lexeme + ")");
                continue;
            }
            info.destructor.emplace(ClassInfo::Method{
                md.isPublic, /*isStatic=*/false, /*isMut=*/false, Type{TypeKind::Void},
                std::vector<Type>{}, std::vector<bool>{}, md.name
            });
            continue;
        }
        Type returnType = md.isConstructor
            ? Type{TypeKind::Void}
            : resolveTypeToken(md.returnType);
        std::vector<Type> paramTypes;
        std::vector<bool> paramMut;
        for (const ParamDecl& p : md.params) {
            paramTypes.push_back(resolveTypeToken(p.typeName));
            paramMut.push_back(p.isMut);
        }
        info.methods.emplace(md.name.lexeme,
            ClassInfo::Method{md.isPublic, md.isStatic, md.isMut, returnType,
                              std::move(paramTypes), std::move(paramMut), md.name});
    }
    return info;
}

void SemanticAnalyzer::collectClasses(const Program& program) {
    // Enums are registered first so that a class field/param of an enum type
    // resolves correctly, and vice-versa (both share classRegistry).
    for (const Stmt& stmt : program.declarations) {
        if (!std::holds_alternative<EnumDeclStmt>(*stmt.node)) continue;
        const auto& en = std::get<EnumDeclStmt>(*stmt.node);

        EnumInfo einfo{ {}, {}, en.name };
        for (const EnumVariant& v : en.variants) {
            if (!einfo.variantSet.insert(v.name.lexeme).second) {
                error(v.name, "duplicate enum variant '" + v.name.lexeme
                      + "' in enum '" + en.name.lexeme + "'");
                continue;
            }
            einfo.variantOrder.push_back(v.name.lexeme);
        }
        enumRegistry.emplace(en.name.lexeme, std::move(einfo));

        ClassInfo info = buildClassInfo(en.name.lexeme, en.fields, en.methods,
                                        /*allowDestructor=*/false);
        classRegistry.emplace(en.name.lexeme, std::move(info));
    }

    for (const Stmt& stmt : program.declarations) {
        if (!std::holds_alternative<ClassDeclStmt>(*stmt.node)) continue;
        const auto& cls = std::get<ClassDeclStmt>(*stmt.node);
        ClassInfo info = buildClassInfo(cls.name.lexeme, cls.fields, cls.methods,
                                        /*allowDestructor=*/true);
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
            std::vector<bool> paramMut;
            for (const ParamDecl& param : function.params) {
                paramTypes.push_back(resolveTypeToken(param.typeName));
                paramMut.push_back(param.isMut);
            }

            Symbol sym{
                Symbol::Kind::Function,
                resolveTypeToken(function.returnType),
                function.name,
                std::move(paramTypes)
            };
            sym.paramMut = std::move(paramMut);

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
    if (typeToken.type == TokenType::IDENTIFIER && enumRegistry.count(typeToken.lexeme))
        return makeEnumType(typeToken.lexeme);
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
    // Value objects (Point), heap references (Point&) and enum values all carry a
    // className and support member/method access.
    if (objectType.kind != TypeKind::Object && objectType.kind != TypeKind::Reference
        && objectType.kind != TypeKind::Enum) {
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
    const std::string& context,
    const std::vector<bool>& paramMut)
{
    for (size_t i = 0; i < args.size(); ++i) {
        Type argType = analyzeExpr(*args[i]);
        checkCast(argType, paramTypes[i], callee,
                  "argument " + std::to_string(i + 1) + " of " + context);
        // Passing a read-only reference into a `mut` reference parameter is a const→mut
        // coercion — warn unless the argument is an explicit cast.
        if (i < paramMut.size() && paramMut[i])
            warnConstToMut(callee, *args[i], paramTypes[i]);
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
