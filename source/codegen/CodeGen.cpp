//
// Created by Vladimir Arsenijevic on 4.6.2026.
//

#include "CodeGen.h"

// ============================================================
// Public entry point
// ============================================================

std::string CodeGen::overloadEmittedName(const std::string& base,
                                         const std::vector<Type>& params, const Type& ret) const {
    return overloadedBases_.count(base) ? mangleOverload(base, params, ret) : base;
}

std::string CodeGen::calleeName(const void* node, const std::string& plainBase) const {
    if (resolvedCallee_) {
        auto it = resolvedCallee_->find(node);
        if (it != resolvedCallee_->end() && !it->second.empty()) return it->second;
    }
    return plainBase;
}

IRModule CodeGen::generate(const Program& program, const SemanticResult& semanticResult, const CompilerOptions& options) {
    module           = {};
    this->typeMap    = &semanticResult.typeMap;
    this->resolvedCallee_ = &semanticResult.resolvedCallee;
    stringCounter    = 0;
    currentClassName_ = "";
    boundsCheck      = options.boundsCheck;
    usesRefcount_    = false;
    clonesNeeded_.clear();
    funcParamTypes.clear();
    funcReturnTypes.clear();
    overloadedBases_.clear();
    freeFnBases_.clear();
    cgClasses_.clear();
    cgEnumNames_.clear();
    globalCtors_.clear();
    staticLocalInits_.clear();
    usedStaticGlobals_.clear();

    // Build CGClassInfo for each class (field order + types for GEP).
    for (const auto& decl : program.declarations) {
        if (!decl.node) continue;
        if (!std::holds_alternative<ClassDeclStmt>(*decl.node)) continue;
        const auto& cls = std::get<ClassDeclStmt>(*decl.node);
        CGClassInfo cgi;
        cgi.irTypeName = "%" + cls.name.lexeme;
        bool hasRefField = false;
        for (const FieldDecl& fd : cls.fields) {
            Type fieldType  = decodeSynthesizedType(fd.typeName);
            if (fieldType.kind == TypeKind::Reference) {
                if (!fd.isStatic) hasRefField = true;
            } else if (isError(fieldType)) {
                fieldType = typeFromToken(fd.typeName.type);
            }
            // Static fields are class-level globals, not part of the struct layout.
            if (fd.isStatic) {
                cgi.staticFields.emplace_back(fd.name.lexeme, fieldType);
                continue;
            }
            // TypedPtr fields decode to a plain `ptr` in the struct (no refcount).
            cgi.fields.emplace_back(fd.name.lexeme, fieldType);
        }
        // A class needs a destructor if it declares one or owns reference fields.
        for (const MethodDecl& md : cls.methods)
            if (md.isDestructor) { cgi.hasDestructor = true; break; }
        // Record static methods so calls omit the implicit `this` argument.
        for (const MethodDecl& md : cls.methods)
            if (md.isStatic) cgi.staticMethods.insert(md.name.lexeme);
        cgi.needsDtor = cgi.hasDestructor || hasRefField;
        cgClasses_[cls.name.lexeme] = std::move(cgi);
    }

    // Build CGClassInfo for each enum (reuses the class machinery for fields/GEP)
    // and record the enum name. Enum singletons are never destroyed, so no dtor.
    for (const auto& decl : program.declarations) {
        if (!decl.node) continue;
        if (!std::holds_alternative<EnumDeclStmt>(*decl.node)) continue;
        const auto& en = std::get<EnumDeclStmt>(*decl.node);
        cgEnumNames_.insert(en.name.lexeme);
        CGClassInfo cgi;
        cgi.irTypeName = "%" + en.name.lexeme;
        for (const FieldDecl& fd : en.fields) {
            Type fieldType = decodeSynthesizedType(fd.typeName);
            if (isError(fieldType)) fieldType = typeFromToken(fd.typeName.type);
            cgi.fields.emplace_back(fd.name.lexeme, fieldType);
        }
        cgClasses_[en.name.lexeme] = std::move(cgi);
    }

    // Determine which base symbol names are overloaded (declared more than once) — only
    // those get overload-mangled names. Bases: free-fn name, `Class_method` / `Class_dtor`.
    {
        std::unordered_map<std::string, int> baseCount;
        for (const auto& decl : program.declarations) {
            if (!decl.node) continue;
            if (std::holds_alternative<FunctionDeclStmt>(*decl.node))
                baseCount[std::get<FunctionDeclStmt>(*decl.node).name.lexeme]++;
            else if (std::holds_alternative<ExternFuncDeclStmt>(*decl.node))
                baseCount[std::get<ExternFuncDeclStmt>(*decl.node).name.lexeme]++;
            else if (std::holds_alternative<ClassDeclStmt>(*decl.node)) {
                const auto& cls = std::get<ClassDeclStmt>(*decl.node);
                for (const MethodDecl& md : cls.methods)
                    baseCount[md.isDestructor ? cls.name.lexeme + "_dtor"
                                              : cls.name.lexeme + "_" + md.name.lexeme]++;
            } else if (std::holds_alternative<EnumDeclStmt>(*decl.node)) {
                const auto& en = std::get<EnumDeclStmt>(*decl.node);
                for (const MethodDecl& md : en.methods)
                    baseCount[en.name.lexeme + "_" + md.name.lexeme]++;
            } else if (std::holds_alternative<ImplDeclStmt>(*decl.node)) {
                const auto& impl = std::get<ImplDeclStmt>(*decl.node);
                for (const MethodDecl& md : impl.methods)
                    baseCount[impl.typeName.lexeme + "_" + md.name.lexeme]++;
            }
        }
        for (const auto& [b, c] : baseCount) if (c > 1) overloadedBases_.insert(b);
    }

    // Build function parameter type table (used in genCall to cast arguments), keyed by the
    // emitted symbol name (overload-mangled when the base is overloaded).
    for (const auto& decl : program.declarations) {
        if (!decl.node) continue;
        if (std::holds_alternative<FunctionDeclStmt>(*decl.node)) {
            const auto& function = std::get<FunctionDeclStmt>(*decl.node);
            freeFnBases_.insert(function.name.lexeme);
            std::vector<Type> paramTypes;
            for (const auto& param : function.params)
                paramTypes.push_back(resolveParamType(param));
            Type ret = resolveReturnType(function.returnType);
            std::string name = overloadEmittedName(function.name.lexeme, paramTypes, ret);
            funcReturnTypes[name] = ret;
            funcParamTypes[name] = std::move(paramTypes);
        } else if (std::holds_alternative<ExternFuncDeclStmt>(*decl.node)) {
            const auto& externDecl = std::get<ExternFuncDeclStmt>(*decl.node);
            freeFnBases_.insert(externDecl.name.lexeme);
            std::vector<Type> paramTypes;
            for (const auto& param : externDecl.params)
                paramTypes.push_back(typeFromToken(param.typeName.type));
            funcReturnTypes[externDecl.name.lexeme] = typeFromToken(externDecl.returnType.type);
            funcParamTypes[externDecl.name.lexeme] = std::move(paramTypes);
        } else if (std::holds_alternative<ClassDeclStmt>(*decl.node)) {
            const auto& cls = std::get<ClassDeclStmt>(*decl.node);
            for (const MethodDecl& md : cls.methods) {
                std::string base = md.isDestructor ? cls.name.lexeme + "_dtor"
                                                   : cls.name.lexeme + "_" + md.name.lexeme;
                std::vector<Type> paramTypes;
                for (const auto& param : md.params)
                    paramTypes.push_back(resolveParamType(param));
                Type ret = (md.isConstructor || md.isDestructor)
                         ? Type{TypeKind::Void} : resolveReturnType(md.returnType);
                std::string en2 = overloadEmittedName(base, paramTypes, ret);
                funcReturnTypes[en2] = ret;
                funcParamTypes[en2] = std::move(paramTypes);
            }
        } else if (std::holds_alternative<EnumDeclStmt>(*decl.node)) {
            const auto& en = std::get<EnumDeclStmt>(*decl.node);
            for (const MethodDecl& md : en.methods) {
                std::string mangledName = en.name.lexeme + "_" + md.name.lexeme;
                std::vector<Type> paramTypes;
                for (const auto& param : md.params)
                    paramTypes.push_back(resolveParamType(param));
                funcReturnTypes[mangledName] = resolveReturnType(md.returnType);
                funcParamTypes[mangledName] = std::move(paramTypes);
            }
        } else if (std::holds_alternative<ImplDeclStmt>(*decl.node)) {
            const auto& impl = std::get<ImplDeclStmt>(*decl.node);
            currentClassName_ = impl.typeName.lexeme;   // so `Self` in signatures resolves
            for (const MethodDecl& md : impl.methods) {
                std::string base = impl.typeName.lexeme + "_" + md.name.lexeme;
                std::vector<Type> paramTypes;
                for (const auto& param : md.params)
                    paramTypes.push_back(resolveParamType(param));
                Type ret = resolveReturnType(md.returnType);
                std::string en3 = overloadEmittedName(base, paramTypes, ret);
                funcReturnTypes[en3] = ret;
                funcParamTypes[en3] = std::move(paramTypes);
            }
            currentClassName_ = "";
        }
    }

    // Emit class + enum type declarations first, then regular declarations.
    for (const auto& decl : program.declarations) {
        if (!decl.node) continue;
        if (std::holds_alternative<ClassDeclStmt>(*decl.node))
            genClassDecl(std::get<ClassDeclStmt>(*decl.node));
        else if (std::holds_alternative<EnumDeclStmt>(*decl.node))
            genEnumDecl(std::get<EnumDeclStmt>(*decl.node));
    }

    for (const auto& decl : program.declarations) {
        if (!decl.node) continue;
        if (std::holds_alternative<FunctionDeclStmt>(*decl.node))
            genFunction(std::get<FunctionDeclStmt>(*decl.node));
        else if (std::holds_alternative<ExternFuncDeclStmt>(*decl.node))
            genExternDecl(std::get<ExternFuncDeclStmt>(*decl.node));
        else if (std::holds_alternative<ImplDeclStmt>(*decl.node)) {
            // An impl block's methods are emitted as methods on the target class.
            const auto& impl = std::get<ImplDeclStmt>(*decl.node);
            for (const MethodDecl& md : impl.methods)
                genMethod(impl.typeName.lexeme, md);
        }
    }

    // Emit the pre-main initializers (enum variant singletons, then static fields).
    // Each records its name in globalCtors_; a single @llvm.global_ctors is emitted last.
    genEnumInit(program);
    genStaticInit(program);

    // Emit any clone helpers requested during codegen (may set usesRefcount_).
    for (const auto& cn : clonesNeeded_) genCloneFunction(cn);

    // Emit the refcount runtime if any `new`/retain/release was lowered.
    if (usesRefcount_) emitRefcountRuntime();

    // Register every pre-main initializer in one @llvm.global_ctors array.
    emitGlobalCtors();

    return std::move(module);
}
