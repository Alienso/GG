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
    this->addressIdentityCmp_ = &semanticResult.addressIdentityCmp;
    this->structuralValueCmp_ = &semanticResult.structuralValueCmp;
    this->eqImplementors_ = &semanticResult.eqImplementors;
    stringCounter    = 0;
    currentClassName_ = "";
    boundsCheck      = options.boundsCheck;
    usesRefcount_    = false;
    clonesNeeded_.clear();
    funcParamTypes.clear();
    funcReturnTypes.clear();
    overloadedBases_.clear();
    freeFnBases_.clear();
    slotReturningFns_.clear();
    currentFnHasReturnSlot_ = false;
    currentReturnAliasLocal_.clear();
    cgClasses_.clear();
    cgEnumNames_.clear();
    globalCtors_.clear();
    staticLocalInits_.clear();
    usedStaticGlobals_.clear();

    // Pre-scan all class / enum names so a field type can name a value-object / enum type
    // declared later (forward reference), matching the semantic analyzer.
    std::unordered_set<std::string> classNames;
    std::unordered_set<std::string> enumNames;
    for (const auto& decl : program.declarations) {
        if (!decl.node) continue;
        if (std::holds_alternative<ClassDeclStmt>(*decl.node))
            classNames.insert(std::get<ClassDeclStmt>(*decl.node).name.lexeme);
        else if (std::holds_alternative<EnumDeclStmt>(*decl.node))
            enumNames.insert(std::get<EnumDeclStmt>(*decl.node).name.lexeme);
    }

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
                // Bare type name: value-object field (embedding) or enum-value field.
                const std::string& lex = fd.typeName.lexeme;
                if (classNames.count(lex))      fieldType = makeObjectType(lex);
                else if (enumNames.count(lex))  fieldType = makeEnumType(lex);
                else                            fieldType = typeFromToken(fd.typeName.type);
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
            if (isError(fieldType)) {
                const std::string& lex = fd.typeName.lexeme;
                if (classNames.count(lex))      fieldType = makeObjectType(lex);
                else if (enumNames.count(lex))  fieldType = makeEnumType(lex);
                else                            fieldType = typeFromToken(fd.typeName.type);
            }
            cgi.fields.emplace_back(fd.name.lexeme, fieldType);
        }
        cgClasses_[en.name.lexeme] = std::move(cgi);
    }

    // Transitive `needsDtor`: a class that embeds a value-object field whose class needs a
    // destructor must itself run one (to destroy the embedded sub-object). Fixpoint — the
    // value-embedding graph is acyclic (checkValueFieldCycles), so this converges.
    for (bool changed = true; changed; ) {
        changed = false;
        for (auto& [name, cgi] : cgClasses_) {
            if (cgi.needsDtor) continue;
            for (const auto& [fieldName, fieldType] : cgi.fields) {
                if (fieldType.kind != TypeKind::Object) continue;
                auto fit = cgClasses_.find(fieldType.className);
                if (fit != cgClasses_.end() && fit->second.needsDtor) {
                    cgi.needsDtor = true;
                    changed = true;
                    break;
                }
            }
        }
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

    // Collect per-parameter default-value expressions (nullptr = none) from a params list, so a
    // call that omits trailing arguments can fill them at the call site.
    auto collectDefaults = [](const std::vector<ParamDecl>& params) {
        std::vector<const Expr*> ds;
        ds.reserve(params.size());
        for (const auto& p : params) ds.push_back(p.defaultValue ? p.defaultValue.get() : nullptr);
        return ds;
    };

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
            bool objSlot = function.hasReturnSlot && isObjectReturnType(function.returnType);
            Type ret = objSlot ? makeObjectType(function.returnType.lexeme)
                               : resolveReturnType(function.returnType);
            std::string name = overloadEmittedName(function.name.lexeme, paramTypes, ret);
            if (objSlot) slotReturningFns_.insert(name);
            funcReturnTypes[name] = ret;
            funcParamTypes[name] = std::move(paramTypes);
            funcDefaults_[name] = collectDefaults(function.params);
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
                bool objSlot = md.hasReturnSlot && !md.isConstructor && !md.isDestructor
                            && isObjectReturnType(md.returnType);
                Type ret = (md.isConstructor || md.isDestructor) ? Type{TypeKind::Void}
                         : objSlot ? makeObjectType(md.returnType.lexeme)
                         : resolveReturnType(md.returnType);
                std::string en2 = overloadEmittedName(base, paramTypes, ret);
                if (objSlot) slotReturningFns_.insert(en2);
                funcReturnTypes[en2] = ret;
                funcParamTypes[en2] = std::move(paramTypes);
                funcDefaults_[en2] = collectDefaults(md.params);
            }
        } else if (std::holds_alternative<EnumDeclStmt>(*decl.node)) {
            const auto& en = std::get<EnumDeclStmt>(*decl.node);
            for (const MethodDecl& md : en.methods) {
                std::string mangledName = en.name.lexeme + "_" + md.name.lexeme;
                std::vector<Type> paramTypes;
                for (const auto& param : md.params)
                    paramTypes.push_back(resolveParamType(param));
                bool objSlot = md.hasReturnSlot && !md.isConstructor
                            && isObjectReturnType(md.returnType);
                funcReturnTypes[mangledName] = objSlot ? makeObjectType(md.returnType.lexeme)
                                                       : resolveReturnType(md.returnType);
                if (objSlot) slotReturningFns_.insert(mangledName);
                funcParamTypes[mangledName] = std::move(paramTypes);
                funcDefaults_[mangledName] = collectDefaults(md.params);
            }
        } else if (std::holds_alternative<ImplDeclStmt>(*decl.node)) {
            const auto& impl = std::get<ImplDeclStmt>(*decl.node);
            currentClassName_ = impl.typeName.lexeme;   // so `Self` in signatures resolves
            for (const MethodDecl& md : impl.methods) {
                std::string base = impl.typeName.lexeme + "_" + md.name.lexeme;
                std::vector<Type> paramTypes;
                for (const auto& param : md.params)
                    paramTypes.push_back(resolveParamType(param));
                bool objSlot = md.hasReturnSlot && isObjectReturnType(md.returnType);
                Type ret = objSlot ? makeObjectType(md.returnType.lexeme)
                                   : resolveReturnType(md.returnType);
                std::string en3 = overloadEmittedName(base, paramTypes, ret);
                if (objSlot) slotReturningFns_.insert(en3);
                funcReturnTypes[en3] = ret;
                funcParamTypes[en3] = std::move(paramTypes);
                funcDefaults_[en3] = collectDefaults(md.params);
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

    // Emit any clone helpers requested during codegen (may set usesRefcount_). Generating a
    // clone can request more clones (an embedded value-object field's clone is recursive), so
    // drain a worklist rather than iterate a fixed range.
    {
        std::unordered_set<std::string> emitted;
        bool progressed = true;
        while (progressed) {
            progressed = false;
            // Snapshot: genCloneFunction may insert into clonesNeeded_ mid-iteration.
            std::vector<std::string> pending(clonesNeeded_.begin(), clonesNeeded_.end());
            for (const std::string& cn : pending) {
                if (!emitted.insert(cn).second) continue;
                genCloneFunction(cn);
                progressed = true;
            }
        }
    }

    // Emit any structural-equality helpers requested during codegen. Generating one can request
    // more (an embedded value-object field's structeq is recursive), so drain a worklist.
    {
        std::unordered_set<std::string> emitted;
        bool progressed = true;
        while (progressed) {
            progressed = false;
            std::vector<std::string> pending(structEqNeeded_.begin(), structEqNeeded_.end());
            for (const std::string& cn : pending) {
                if (!emitted.insert(cn).second) continue;
                genStructEqFunction(cn);
                progressed = true;
            }
        }
    }

    // Emit the refcount runtime if any `new`/retain/release was lowered.
    if (usesRefcount_) emitRefcountRuntime();

    // Register every pre-main initializer in one @llvm.global_ctors array.
    emitGlobalCtors();

    return std::move(module);
}
