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
    }

    // Emit each method. The destructor is generated separately (genDestructor)
    // so that reference-field releases can be appended to its body.
    for (const MethodDecl& md : classDecl.methods)
        if (!md.isDestructor) genMethod(className, md);

    if (cgIt != cgClasses_.end() && cgIt->second.needsDtor)
        genDestructor(classDecl);
}

void CodeGen::genFunction(const FunctionDeclStmt& function) {
    // Reset per-function state
    tempCounter = 0; labelCounter = 0;
    allocaMap.clear(); varTypeMap.clear();
    usedAllocaNames.clear();
    breakLabelStack.clear(); continueLabelStack.clear();
    dtorScopes_.clear();

    currentReturnType        = typeFromToken(function.returnType.type);
    std::string returnIrType = irTypeName(currentReturnType);

    std::string paramStr;
    bool first = true;
    for (const auto& p : function.params) {
        if (!first) paramStr += ", ";
        first = false;
        paramStr += paramIrType(resolveParamType(p)) + " %" + p.name.lexeme;
    }

    IRFunction irFunc;
    irFunc.signature = "define " + returnIrType + " @" + function.name.lexeme + "(" + paramStr + ")";
    module.functions.push_back(std::move(irFunc));
    currentFunction = &module.functions.back();
    currentFunction->blocks.push_back(BasicBlock{"entry", {}, false});
    currentBasicBlock = &currentFunction->blocks.back();

    spillParamsToAllocas(function.params);
    emitFunctionBody(function.body, returnIrType);

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
    currentClassName_ = className;

    bool isVoidLike = method.isConstructor || method.isDestructor;
    currentReturnType        = isVoidLike ? Type{TypeKind::Void} : typeFromToken(method.returnType.type);
    std::string returnIrType = isVoidLike ? "void" : irTypeName(currentReturnType);

    std::string mangledName = method.isDestructor
        ? className + "_dtor"
        : className + "_" + method.name.lexeme;

    std::string paramStr = "ptr %self";
    for (const auto& p : method.params) {
        paramStr += ", " + paramIrType(resolveParamType(p)) + " %" + p.name.lexeme;
    }

    IRFunction irFunc;
    irFunc.signature = "define " + returnIrType + " @" + mangledName + "(" + paramStr + ")";
    module.functions.push_back(std::move(irFunc));
    currentFunction = &module.functions.back();
    currentFunction->blocks.push_back(BasicBlock{"entry", {}, false});
    currentBasicBlock = &currentFunction->blocks.back();

    allocaMap["this"]  = "%self";
    varTypeMap["this"] = makeObjectType(className);

    spillParamsToAllocas(method.params);
    emitFunctionBody(method.body, returnIrType);

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
            if (fields[i].second.kind != TypeKind::Reference) continue;
            usesRefcount_ = true;
            std::string gep = freshTemp();
            emit("%" + gep + " = getelementptr %" + className + ", ptr %self, i32 0, i32 "
                 + std::to_string(i));
            std::string val = emitLoad("ptr", "%" + gep);
            const std::string& fieldClass = fields[i].second.className;
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

        if (ft.kind == TypeKind::Reference) {
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
        emitDtorsForScope(dtorScopes_.back());
        emit(returnIrType == "void" ? "ret void" : "unreachable");
        currentBasicBlock->terminated = true;
    }
    dtorScopes_.pop_back();
}
