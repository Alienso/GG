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

    // Emit each method
    for (const MethodDecl& md : classDecl.methods)
        genMethod(className, md);
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
