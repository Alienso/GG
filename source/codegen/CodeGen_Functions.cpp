#include "CodeGen.h"

// ============================================================
// Function / extern / class codegen
// ============================================================

void CodeGen::genExternDecl(const ExternFuncDeclStmt& externDecl) {
    std::string returnIrType = irTypeName(typeFromToken(externDecl.returnType.type));

    std::string parameterString;
    bool first = true;
    for (const auto& param : externDecl.params) {
        if (!first) parameterString += ", ";
        first = false;
        parameterString += irTypeName(typeFromToken(param.typeName.type));
    }

    module.declares.push_back(
        "declare " + returnIrType + " @" + externDecl.name.lexeme + "(" + parameterString + ")");
}

void CodeGen::genClassDecl(const ClassDeclStmt& classDecl) {
    const std::string& className = classDecl.name.lexeme;

    // Emit struct type declaration: %ClassName = type { irType1, irType2, ... }
    auto cgIt = cgClasses_.find(className);
    if (cgIt != cgClasses_.end()) {
        std::string typeBody;
        bool first = true;
        for (const auto& [fieldName, fieldType] : cgIt->second.fields) {
            if (!first) typeBody += ", ";
            first = false;
            typeBody += irTypeName(fieldType);
        }
        module.typeDecls.push_back("%" + className + " = type { " + typeBody + " }");

        // Emit one global per static field: @ClassName$field = global <ir> zeroinitializer.
        // Non-zero initializers are applied at runtime in @gg_static_init.
        for (const auto& [fieldName, fieldType] : cgIt->second.staticFields) {
            module.globals.push_back(
                "@" + className + "$" + fieldName + " = global "
                + irTypeName(fieldType) + " zeroinitializer");
        }
    }

    // Emit each method. The destructor is generated separately (genDestructor)
    // so that reference-field releases can be appended to its body.
    for (const MethodDecl& md : classDecl.methods)
        if (!md.isDestructor) genMethod(className, md);

    if (cgIt != cgClasses_.end() && cgIt->second.needsDtor)
        genDestructor(classDecl);
}

void CodeGen::genEnumDecl(const EnumDeclStmt& enumDecl) {
    const std::string& enumName = enumDecl.name.lexeme;

    // Emit struct type declaration: %Enum = type { irType1, ... }
    auto cgIt = cgClasses_.find(enumName);
    if (cgIt != cgClasses_.end()) {
        std::string typeBody;
        bool first = true;
        for (const auto& [fieldName, fieldType] : cgIt->second.fields) {
            if (!first) typeBody += ", ";
            first = false;
            typeBody += irTypeName(fieldType);
        }
        // A fieldless enum would lower to a zero-sized struct; distinct singletons
        // could then share one address (defeating identity comparison). Give it a
        // single padding byte so every variant gets a unique address.
        if (typeBody.empty()) typeBody = "i8";
        module.typeDecls.push_back("%" + enumName + " = type { " + typeBody + " }");
    }

    // Emit one global singleton per variant: @Enum$VARIANT = global %Enum zeroinitializer
    for (const EnumVariant& v : enumDecl.variants) {
        module.globals.push_back(
            "@" + enumName + "$" + v.name.lexeme
            + " = global %" + enumName + " zeroinitializer");
    }

    // Emit constructor + regular methods (mangled exactly like class methods).
    for (const MethodDecl& md : enumDecl.methods)
        genMethod(enumName, md);
}

void CodeGen::genEnumInit(const Program& program) {
    // Collect every (enum, variant) pair that needs a constructor call.
    struct InitCall { const EnumDeclStmt* en; const EnumVariant* variant; };
    std::vector<InitCall> calls;
    for (const auto& decl : program.declarations) {
        if (!decl.node) continue;
        if (!std::holds_alternative<EnumDeclStmt>(*decl.node)) continue;
        const auto& en = std::get<EnumDeclStmt>(*decl.node);
        bool hasCtor = funcParamTypes.count(en.name.lexeme + "_" + en.name.lexeme) > 0;
        if (!hasCtor) continue;   // fieldless enum: zeroinitializer is the final value
        for (const EnumVariant& v : en.variants)
            calls.push_back({ &en, &v });
    }
    if (calls.empty()) return;

    // Reset per-function state and emit @gg_enum_init.
    tempCounter = 0; labelCounter = 0;
    allocaMap.clear(); varTypeMap.clear();
    usedAllocaNames.clear();
    breakLabelStack.clear(); continueLabelStack.clear();
    dtorScopes_.clear();
    pendingTemps_.clear();
    currentClassName_ = "";
    currentReturnType = Type{TypeKind::Void};

    IRFunction irFunc;
    irFunc.signature = "define void @gg_enum_init()";
    module.functions.push_back(std::move(irFunc));
    currentFunction = &module.functions.back();
    currentFunction->blocks.push_back(BasicBlock{"entry", {}, false});
    currentBasicBlock = &currentFunction->blocks.back();

    for (const InitCall& c : calls) {
        const std::string& enumName = c.en->name.lexeme;
        std::string mangledCtor = enumName + "_" + enumName;
        auto funcIt = funcParamTypes.find(mangledCtor);
        const std::vector<Type>* ctorParams =
            funcIt != funcParamTypes.end() ? &funcIt->second : nullptr;
        std::string argStr = buildArgString(c.variant->args, ctorParams, defaultsFor(mangledCtor));
        std::string self   = "@" + enumName + "$" + c.variant->name.lexeme;
        emit("call void @" + mangledCtor + "(ptr " + self
             + (argStr.empty() ? "" : ", " + argStr) + ")");
    }
    emit("ret void");
    currentBasicBlock->terminated = true;
    currentFunction   = nullptr;
    currentBasicBlock = nullptr;

    // Register the initializer to run before main (combined with others later).
    globalCtors_.push_back("gg_enum_init");
}

void CodeGen::genStaticInit(const Program& program) {
    // Collect every static field that carries an initializer expression.
    struct InitField { std::string className; const FieldDecl* field; };
    std::vector<InitField> inits;
    for (const auto& decl : program.declarations) {
        if (!decl.node) continue;
        if (!std::holds_alternative<ClassDeclStmt>(*decl.node)) continue;
        const auto& cls = std::get<ClassDeclStmt>(*decl.node);
        for (const FieldDecl& fd : cls.fields)
            if (fd.isStatic && fd.initializer)
                inits.push_back({ cls.name.lexeme, &fd });
    }
    // Static locals (collected during function codegen) also initialise here.
    if (inits.empty() && staticLocalInits_.empty()) return;

    // Reset per-function state and emit @gg_static_init.
    tempCounter = 0; labelCounter = 0;
    allocaMap.clear(); varTypeMap.clear();
    usedAllocaNames.clear();
    breakLabelStack.clear(); continueLabelStack.clear();
    dtorScopes_.clear();
    pendingTemps_.clear();
    currentClassName_ = "";
    currentReturnType = Type{TypeKind::Void};

    IRFunction irFunc;
    irFunc.signature = "define void @gg_static_init()";
    module.functions.push_back(std::move(irFunc));
    currentFunction = &module.functions.back();
    currentFunction->blocks.push_back(BasicBlock{"entry", {}, false});
    currentBasicBlock = &currentFunction->blocks.back();

    for (const InitField& f : inits) {
        Type        fieldType = exprType(*f.field->initializer);
        // The global's declared type comes from the field, not the initializer.
        auto cgIt = cgClasses_.find(f.className);
        Type globalType = fieldType;
        if (cgIt != cgClasses_.end())
            for (const auto& [n, t] : cgIt->second.staticFields)
                if (n == f.field->name.lexeme) { globalType = t; break; }

        std::string value = genExpr(*f.field->initializer);
        value = emitCast(value, fieldType, globalType);
        emitStore(irTypeName(globalType), value,
                  "@" + f.className + "$" + f.field->name.lexeme);
    }

    // Static-local initializers: store each constant into its persistent global.
    for (const StaticLocalInit& sl : staticLocalInits_) {
        Type        initType = exprType(*sl.init);
        std::string value    = genExpr(*sl.init);
        value = emitCast(value, initType, sl.type);
        emitStore(irTypeName(sl.type), value, sl.global);
    }

    emit("ret void");
    currentBasicBlock->terminated = true;
    currentFunction   = nullptr;
    currentBasicBlock = nullptr;

    globalCtors_.push_back("gg_static_init");
}

void CodeGen::emitGlobalCtors() {
    if (globalCtors_.empty()) return;
    std::string n = std::to_string(globalCtors_.size());
    std::string entries;
    for (size_t i = 0; i < globalCtors_.size(); ++i) {
        if (i) entries += ", ";
        entries += "{ i32, ptr, ptr } { i32 65535, ptr @" + globalCtors_[i] + ", ptr null }";
    }
    module.globals.push_back(
        "@llvm.global_ctors = appending global [" + n + " x { i32, ptr, ptr }] ["
        + entries + "]");
}

// True if the return-type token names a class *value* (sret alias) rather than a
// primitive / reference / ptr / enum (ordinary returned-local alias).
bool CodeGen::isObjectReturnType(const Token& typeToken) const {
    return typeToken.type == TokenType::IDENTIFIER
        && cgClasses_.count(typeToken.lexeme) > 0
        && cgEnumNames_.count(typeToken.lexeme) == 0
        && isError(decodeSynthesizedType(typeToken));   // exclude "Class&" / "ptr<...>"
}

// Allocate + zero/null-init an ordinary returned-local alias (primitive or reference).
void CodeGen::setupReturnAliasLocal(const std::string& aliasName, const Type& aliasType) {
    std::string irt     = irTypeName(aliasType);
    std::string ptrName = freshAllocaName(aliasName);
    emitAlloca(ptrName, irt);
    allocaMap[aliasName]  = ptrName;
    varTypeMap[aliasName] = aliasType;
    currentReturnAliasLocal_ = aliasName;
    if (aliasType.kind == TypeKind::Reference) {
        emitStore("ptr", "null", ptrName);                       // null until assigned
        // A reference alias is NOT registered for scope-exit destruction: it always owns
        // exactly one +1 (every `r = <expr>` either claims a +1 producer or retains a borrow,
        // see genAssign), and that +1 is transferred to the caller on return (emitReturnAlias),
        // never released in-function. Any intermediate value from a re-assignment is released
        // by genAssign's rebind (release-old) — not here.
    } else {
        emitStore(irt, isFloat(aliasType.kind) ? "0.0" : "0", ptrName);   // zero-init
    }
}

// Return the current returned-local alias (bare `return;` / `return alias;` / fall-through):
// load it, apply the +1 convention for references, run dtors, then `ret`.
void CodeGen::emitReturnAlias() {
    std::string ptr = allocaMap[currentReturnAliasLocal_];
    Type        t   = currentReturnType;                 // = the alias's type
    std::string val = emitLoad(irTypeName(t), ptr);
    // A reference alias already owns the +1 it will hand to the caller (see setupReturnAliasLocal
    // / genAssign) — transfer that ownership directly, with NO extra retain and NO release of the
    // alias. Releasing the body's other locals below cannot touch it (it is not in any dtor
    // scope). An extra retain here would return a +2 object → a one-object leak per call.
    flushTempReleases();
    for (auto it = dtorScopes_.rbegin(); it != dtorScopes_.rend(); ++it)
        emitDtorsForScope(*it);
    emit("ret " + irTypeName(t) + " " + val);
    if (currentBasicBlock) currentBasicBlock->terminated = true;
}

void CodeGen::genFunction(const FunctionDeclStmt& function) {
    // Reset per-function state
    tempCounter = 0; labelCounter = 0;
    allocaMap.clear(); varTypeMap.clear();
    usedAllocaNames.clear();
    breakLabelStack.clear(); continueLabelStack.clear();
    dtorScopes_.clear();
    pendingTemps_.clear();

    // An object return alias lowers to sret (void return, hidden slot pointer). A
    // primitive/reference alias is an ordinary returned local. Everything else is a plain return.
    bool sret       = function.hasReturnSlot && isObjectReturnType(function.returnType);
    bool localAlias = function.hasReturnSlot && !sret;
    currentFnHasReturnSlot_ = sret;
    currentReturnAliasLocal_.clear();

    Type logicalRet = sret ? makeObjectType(function.returnType.lexeme)
                           : resolveReturnType(function.returnType);
    currentReturnType        = sret ? Type{TypeKind::Void} : logicalRet;
    std::string returnIrType = sret ? "void" : irTypeName(currentReturnType);

    std::vector<Type> paramTypes;
    std::string paramStr;
    if (sret) paramStr = "ptr %" + function.returnSlotName;  // sret slot first
    for (const auto& p : function.params) {
        Type pt = resolveParamType(p);
        paramTypes.push_back(pt);
        if (!paramStr.empty()) paramStr += ", ";
        paramStr += paramIrType(pt) + " %" + p.name.lexeme;
    }

    // Overloaded free functions emit an overload-mangled symbol; others keep the plain name.
    std::string emitName = overloadEmittedName(function.name.lexeme, paramTypes, logicalRet);
    currentStaticPrefix_     = emitName;

    IRFunction irFunc;
    irFunc.signature = "define " + returnIrType + " @" + emitName + "(" + paramStr + ")";
    module.functions.push_back(std::move(irFunc));
    currentFunction = &module.functions.back();
    currentFunction->blocks.push_back(BasicBlock{"entry", {}, false});
    currentBasicBlock = &currentFunction->blocks.back();

    if (debug_)
        dbgBeginFunction(function.name.lexeme, emitName, function.name.line,
                         paramTypes, logicalRet, false, "");

    // Bind the slot name directly to the sret pointer and zero-init the object in place.
    if (sret) {
        allocaMap[function.returnSlotName]  = "%" + function.returnSlotName;
        varTypeMap[function.returnSlotName] = logicalRet;
        emit("store %" + function.returnType.lexeme + " zeroinitializer, ptr %"
             + function.returnSlotName);
    }

    spillParamsToAllocas(function.params);
    if (debug_)
        for (size_t i = 0; i < function.params.size(); ++i) {
            auto it = allocaMap.find(function.params[i].name.lexeme);
            if (it != allocaMap.end())
                dbgDeclare(it->second, function.params[i].name.lexeme, paramTypes[i],
                           function.params[i].name.line, static_cast<int>(i) + 1);
        }
    if (localAlias) setupReturnAliasLocal(function.returnSlotName, logicalRet);
    emitFunctionBody(function.body, returnIrType);

    if (debug_) dbgEndFunction();
    currentFunction = nullptr;
    currentBasicBlock = nullptr;
}

void CodeGen::genMethod(const std::string& className, const MethodDecl& method) {
    // Reset per-function state
    tempCounter = 0; labelCounter = 0;
    allocaMap.clear(); varTypeMap.clear();
    usedAllocaNames.clear();
    breakLabelStack.clear(); continueLabelStack.clear();
    dtorScopes_.clear();
    pendingTemps_.clear();
    currentClassName_ = className;

    bool isVoidLike = method.isConstructor || method.isDestructor;
    // An object return alias lowers to sret; a primitive/reference alias is a returned local.
    bool sret       = method.hasReturnSlot && isObjectReturnType(method.returnType);
    bool localAlias = method.hasReturnSlot && !sret;
    currentFnHasReturnSlot_ = sret;
    currentReturnAliasLocal_.clear();
    Type logicalRet = sret       ? makeObjectType(method.returnType.lexeme)
                    : isVoidLike ? Type{TypeKind::Void}
                    : resolveReturnType(method.returnType);
    currentReturnType        = sret ? Type{TypeKind::Void} : logicalRet;
    std::string returnIrType = (isVoidLike || sret) ? "void" : irTypeName(currentReturnType);

    std::string base = method.isDestructor
        ? className + "_dtor"
        : className + "_" + method.name.lexeme;

    // Param order: sret slot (if any), then implicit `this`, then declared params. Static
    // methods carry no `this`.
    std::vector<Type> paramTypes;
    std::string paramStr;
    if (sret) paramStr = "ptr %" + method.returnSlotName;
    if (!method.isStatic) {
        if (!paramStr.empty()) paramStr += ", ";
        paramStr += "ptr %self";
    }
    for (const auto& p : method.params) {
        Type pt = resolveParamType(p);
        paramTypes.push_back(pt);
        if (!paramStr.empty()) paramStr += ", ";
        paramStr += paramIrType(pt) + " %" + p.name.lexeme;
    }

    // Overloaded methods/constructors emit an overload-mangled symbol.
    std::string mangledName = overloadEmittedName(base, paramTypes, logicalRet);
    currentStaticPrefix_ = mangledName;

    IRFunction irFunc;
    irFunc.signature = "define " + returnIrType + " @" + mangledName + "(" + paramStr + ")";
    module.functions.push_back(std::move(irFunc));
    currentFunction = &module.functions.back();
    currentFunction->blocks.push_back(BasicBlock{"entry", {}, false});
    currentBasicBlock = &currentFunction->blocks.back();

    if (debug_) {
        std::string pretty = method.isDestructor ? ("~" + className)
                           : method.isConstructor ? className
                           : (className + "::" + method.name.lexeme);
        dbgBeginFunction(pretty, mangledName, method.name.line, paramTypes, logicalRet,
                         !method.isStatic, className);
    }

    if (!method.isStatic) {
        allocaMap["this"]  = "%self";
        varTypeMap["this"] = cgEnumNames_.count(className)
                           ? makeEnumType(className)
                           : makeObjectType(className);
    }

    // Bind the slot name to the sret pointer and zero-init the object in place.
    if (sret) {
        allocaMap[method.returnSlotName]  = "%" + method.returnSlotName;
        varTypeMap[method.returnSlotName] = logicalRet;
        emit("store %" + method.returnType.lexeme + " zeroinitializer, ptr %"
             + method.returnSlotName);
    }

    spillParamsToAllocas(method.params);
    if (debug_) {
        int argBase = method.isStatic ? 1 : 2;   // `this` occupies arg 1 when present
        for (size_t i = 0; i < method.params.size(); ++i) {
            auto it = allocaMap.find(method.params[i].name.lexeme);
            if (it != allocaMap.end())
                dbgDeclare(it->second, method.params[i].name.lexeme, paramTypes[i],
                           method.params[i].name.line, argBase + static_cast<int>(i));
        }
    }
    if (localAlias) setupReturnAliasLocal(method.returnSlotName, logicalRet);
    emitFunctionBody(method.body, returnIrType);

    if (debug_) dbgEndFunction();
    currentFunction = nullptr;
    currentBasicBlock = nullptr;
    currentClassName_ = "";
}

void CodeGen::genDestructor(const ClassDeclStmt& classDecl) {
    const std::string& className = classDecl.name.lexeme;

    // Locate the user-written destructor body, if any.
    const MethodDecl* userDtor = nullptr;
    for (const MethodDecl& md : classDecl.methods)
        if (md.isDestructor) { userDtor = &md; break; }

    // Reset per-function state.
    tempCounter = 0; labelCounter = 0;
    allocaMap.clear(); varTypeMap.clear();
    usedAllocaNames.clear();
    breakLabelStack.clear(); continueLabelStack.clear();
    dtorScopes_.clear();
    currentClassName_  = className;
    currentReturnType  = Type{TypeKind::Void};
    currentFnHasReturnSlot_ = false;
    currentReturnAliasLocal_.clear();

    IRFunction irFunc;
    irFunc.signature = "define void @" + className + "_dtor(ptr %self)";
    module.functions.push_back(std::move(irFunc));
    currentFunction = &module.functions.back();
    currentFunction->blocks.push_back(BasicBlock{"entry", {}, false});
    currentBasicBlock = &currentFunction->blocks.back();

    allocaMap["this"]  = "%self";
    varTypeMap["this"] = makeObjectType(className);

    // 1) Run the user destructor body (if present).
    if (userDtor) {
        dtorScopes_.emplace_back();
        for (const auto& stmtPtr : userDtor->body.body)
            if (stmtPtr) genStmt(*stmtPtr);
        if (currentBasicBlock && !currentBasicBlock->terminated)
            emitDtorsForScope(dtorScopes_.back());
        dtorScopes_.pop_back();
    }

    // 2) Release reference fields in reverse declaration order.
    auto cgIt = cgClasses_.find(className);
    if (cgIt != cgClasses_.end() && currentBasicBlock && !currentBasicBlock->terminated) {
        const auto& fields = cgIt->second.fields;
        for (int i = static_cast<int>(fields.size()) - 1; i >= 0; --i) {
            const Type& ft = fields[i].second;
            if (ft.kind == TypeKind::Object) {
                // Embedded value object: destroy it in place (only if its class needs a dtor).
                auto fcgIt = cgClasses_.find(ft.className);
                if (fcgIt == cgClasses_.end() || !fcgIt->second.needsDtor) continue;
                std::string gep = freshTemp();
                emit("%" + gep + " = getelementptr %" + className + ", ptr %self, i32 0, i32 "
                     + std::to_string(i));
                emit("call void @" + ft.className + "_dtor(ptr %" + gep + ")");
                continue;
            }
            if (ft.kind != TypeKind::Reference) continue;
            usesRefcount_ = true;
            std::string gep = freshTemp();
            emit("%" + gep + " = getelementptr %" + className + ", ptr %self, i32 0, i32 "
                 + std::to_string(i));
            std::string val = emitLoad("ptr", "%" + gep);
            const std::string& fieldClass = ft.className;
            auto fcgIt = cgClasses_.find(fieldClass);
            std::string dtorArg = (fcgIt != cgClasses_.end() && fcgIt->second.needsDtor)
                                ? ("@" + fieldClass + "_dtor") : "null";
            emit("call void @gg_release(ptr " + val + ", ptr " + dtorArg + ")");
        }
    }

    if (currentBasicBlock && !currentBasicBlock->terminated) {
        emit("ret void");
        currentBasicBlock->terminated = true;
    }

    currentFunction   = nullptr;
    currentBasicBlock = nullptr;
    currentClassName_ = "";
}

void CodeGen::genCloneFunction(const std::string& className) {
    auto cgIt = cgClasses_.find(className);
    if (cgIt == cgClasses_.end()) return;

    // Reset per-function state.
    tempCounter = 0; labelCounter = 0;
    allocaMap.clear(); varTypeMap.clear();
    usedAllocaNames.clear();
    breakLabelStack.clear(); continueLabelStack.clear();
    dtorScopes_.clear();
    currentClassName_  = "";
    currentReturnType  = Type{TypeKind::Void};

    IRFunction irFunc;
    irFunc.signature = "define void @" + className + "_clone(ptr %dest, ptr %src)";
    module.functions.push_back(std::move(irFunc));
    currentFunction = &module.functions.back();
    currentFunction->blocks.push_back(BasicBlock{"entry", {}, false});
    currentBasicBlock = &currentFunction->blocks.back();

    const auto& fields = cgIt->second.fields;
    for (size_t i = 0; i < fields.size(); ++i) {
        const Type& ft  = fields[i].second;
        std::string idx = std::to_string(i);

        if (ft.kind == TypeKind::Object) {
            // Embedded value object: deep-copy recursively via the field class's clone.
            clonesNeeded_.insert(ft.className);
            std::string dgep = freshTemp();
            emit("%" + dgep + " = getelementptr %" + className + ", ptr %dest, i32 0, i32 " + idx);
            std::string sgep = freshTemp();
            emit("%" + sgep + " = getelementptr %" + className + ", ptr %src, i32 0, i32 " + idx);
            emit("call void @" + ft.className + "_clone(ptr %" + dgep + ", ptr %" + sgep + ")");
        } else if (ft.kind == TypeKind::Reference) {
            usesRefcount_ = true;
            // new = load src.field; retain(new)
            std::string sgep = freshTemp();
            emit("%" + sgep + " = getelementptr %" + className + ", ptr %src, i32 0, i32 " + idx);
            std::string nv = emitLoad("ptr", "%" + sgep);
            emit("call void @gg_retain(ptr " + nv + ")");
            // old = load dest.field; release(old); store new
            std::string dgep = freshTemp();
            emit("%" + dgep + " = getelementptr %" + className + ", ptr %dest, i32 0, i32 " + idx);
            std::string ov = emitLoad("ptr", "%" + dgep);
            auto fcgIt = cgClasses_.find(ft.className);
            std::string dtorArg = (fcgIt != cgClasses_.end() && fcgIt->second.needsDtor)
                                ? ("@" + ft.className + "_dtor") : "null";
            emit("call void @gg_release(ptr " + ov + ", ptr " + dtorArg + ")");
            emit("store ptr " + nv + ", ptr %" + dgep);
        } else {
            // primitive / ptr: dest.field = src.field
            std::string ir   = irTypeName(ft);
            std::string sgep = freshTemp();
            emit("%" + sgep + " = getelementptr %" + className + ", ptr %src, i32 0, i32 " + idx);
            std::string v    = emitLoad(ir, "%" + sgep);
            std::string dgep = freshTemp();
            emit("%" + dgep + " = getelementptr %" + className + ", ptr %dest, i32 0, i32 " + idx);
            emit("store " + ir + " " + v + ", ptr %" + dgep);
        }
    }

    emit("ret void");
    currentBasicBlock->terminated = true;
    currentFunction   = nullptr;
    currentBasicBlock = nullptr;
}

void CodeGen::genStructEqFunction(const std::string& className) {
    auto cgIt = cgClasses_.find(className);
    if (cgIt == cgClasses_.end()) return;

    // Reset per-function state.
    tempCounter = 0; labelCounter = 0;
    allocaMap.clear(); varTypeMap.clear();
    usedAllocaNames.clear();
    breakLabelStack.clear(); continueLabelStack.clear();
    dtorScopes_.clear();
    currentClassName_  = "";
    currentReturnType  = Type{TypeKind::Void};

    IRFunction irFunc;
    irFunc.signature = "define i1 @" + className + "_structeq(ptr %a, ptr %b)";
    module.functions.push_back(std::move(irFunc));
    currentFunction = &module.functions.back();
    currentFunction->blocks.push_back(BasicBlock{"entry", {}, false});
    currentBasicBlock = &currentFunction->blocks.back();

    // Compare each field into an i1, then AND them all (no side effects → no short-circuit needed).
    // Field rules: primitive → value compare; embedded value object → recurse into its structeq;
    // reference / enum / ptr → address identity.
    const auto& fields = cgIt->second.fields;
    std::vector<std::string> conds;
    for (size_t i = 0; i < fields.size(); ++i) {
        const Type& ft  = fields[i].second;
        std::string idx = std::to_string(i);
        std::string agep = freshTemp();
        emit("%" + agep + " = getelementptr %" + className + ", ptr %a, i32 0, i32 " + idx);
        std::string bgep = freshTemp();
        emit("%" + bgep + " = getelementptr %" + className + ", ptr %b, i32 0, i32 " + idx);
        std::string ci = freshTemp();
        if (ft.kind == TypeKind::Object) {
            // An embedded value field whose class implements `Eq` is compared with that `eq`
            // (consistent with a direct `field == field`); otherwise recurse memberwise. The
            // funcReturnTypes guard degrades gracefully to memberwise if `eq` is overloaded
            // (mangled name), rather than emitting a call to a non-existent `@Class_eq`.
            bool fieldHasEq = eqImplementors_ && eqImplementors_->count(ft.className)
                              && funcReturnTypes.count(ft.className + "_eq");
            if (fieldHasEq) {
                emit("%" + ci + " = call i1 @" + ft.className + "_eq(ptr %" + agep + ", ptr %" + bgep + ")");
            } else {
                structEqNeeded_.insert(ft.className);
                emit("%" + ci + " = call i1 @" + ft.className + "_structeq(ptr %" + agep + ", ptr %" + bgep + ")");
            }
        } else if (ft.kind == TypeKind::Reference || ft.kind == TypeKind::Enum
                   || ft.kind == TypeKind::Ptr || ft.kind == TypeKind::TypedPtr) {
            std::string av = emitLoad("ptr", "%" + agep);
            std::string bv = emitLoad("ptr", "%" + bgep);
            emit("%" + ci + " = icmp eq ptr " + av + ", " + bv);
        } else {
            std::string ir = irTypeName(ft);
            std::string av = emitLoad(ir, "%" + agep);
            std::string bv = emitLoad(ir, "%" + bgep);
            std::string cmp = isFloat(ft.kind) ? "fcmp oeq " : "icmp eq ";
            emit("%" + ci + " = " + cmp + ir + " " + av + ", " + bv);
        }
        conds.push_back("%" + ci);
    }

    std::string result;
    if (conds.empty()) {
        result = "1";   // no fields → always equal
    } else {
        result = conds[0];
        for (size_t k = 1; k < conds.size(); ++k) {
            std::string t = freshTemp();
            emit("%" + t + " = and i1 " + result + ", " + conds[k]);
            result = "%" + t;
        }
    }
    emit("ret i1 " + result);
    currentBasicBlock->terminated = true;
    currentFunction   = nullptr;
    currentBasicBlock = nullptr;
}

void CodeGen::spillParamsToAllocas(const std::vector<ParamDecl>& params) {
    for (const auto& param : params) {
        Type paramType = resolveParamType(param);

        // Objects are passed by reference: the incoming `ptr %name` already points
        // at the caller's object. Bind the name directly to that pointer — no copy,
        // so field mutations propagate back to the caller (Java-style references).
        if (paramType.kind == TypeKind::Object) {
            allocaMap[param.name.lexeme]  = "%" + param.name.lexeme;
            varTypeMap[param.name.lexeme] = paramType;
            continue;
        }

        std::string irType  = irTypeName(paramType);
        std::string ptrName = freshAllocaName(param.name.lexeme);
        emitAlloca(ptrName, irType);
        allocaMap[param.name.lexeme]  = ptrName;
        varTypeMap[param.name.lexeme] = paramType;
        emitStore(irType, "%" + param.name.lexeme, ptrName);
    }
}

std::string CodeGen::paramIrType(const Type& type) {
    return type.kind == TypeKind::Object ? "ptr" : irTypeName(type);
}

void CodeGen::emitFunctionBody(const BlockStmt& body, const std::string& returnIrType) {
    dtorScopes_.emplace_back();

    for (const auto& stmtPtr : body.body)
        if (stmtPtr) genStmt(*stmtPtr);

    if (currentBasicBlock && !currentBasicBlock->terminated) {
        if (!currentReturnAliasLocal_.empty()) {
            emitReturnAlias();   // fall-through returns the named alias local (runs dtors + ret)
        } else {
            emitDtorsForScope(dtorScopes_.back());
            emit(returnIrType == "void" ? "ret void" : "unreachable");
            currentBasicBlock->terminated = true;
        }
    }
    dtorScopes_.pop_back();
}
