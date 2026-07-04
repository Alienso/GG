#include "CodeGen.h"

// ============================================================
// Shared helper — GEP for a named field
// ============================================================

std::pair<std::string, Type> CodeGen::resolveFieldGEP(const std::string& objPtr,
                                                        const std::string& className,
                                                        const std::string& fieldName) {
    auto cgIt = cgClasses_.find(className);
    if (cgIt == cgClasses_.end()) return {"0", Type{TypeKind::Error}};

    int  fieldIndex = -1;
    Type fieldType{TypeKind::Error};
    for (int i = 0; i < static_cast<int>(cgIt->second.fields.size()); ++i) {
        if (cgIt->second.fields[i].first == fieldName) {
            fieldIndex = i;
            fieldType  = cgIt->second.fields[i].second;
            break;
        }
    }
    if (fieldIndex < 0) return {"0", Type{TypeKind::Error}};

    std::string gepName = freshTemp();
    emit("%" + gepName + " = getelementptr %" + className + ", ptr " + objPtr
         + ", i32 0, i32 " + std::to_string(fieldIndex));
    return {"%" + gepName, fieldType};
}

// ============================================================
// Class expression codegen
// ============================================================

// ---- this ----

std::string CodeGen::genThis(const ThisExpr&) {
    auto it = allocaMap.find("this");
    return it != allocaMap.end() ? it->second : "null";
}

// ---- Member access (field read) ----

// Look up a static field's type for `className`. Returns nullptr if not static.
const Type* CodeGen::findStaticField(const std::string& className,
                                     const std::string& fieldName) const {
    auto cgIt = cgClasses_.find(className);
    if (cgIt == cgClasses_.end()) return nullptr;
    for (const auto& [n, t] : cgIt->second.staticFields)
        if (n == fieldName) return &t;
    return nullptr;
}

std::string CodeGen::genMemberAccess(const MemberAccessExpr& ma) {
    if (std::holds_alternative<IdentifierExpr>(*ma.object->node)) {
        const auto& id = std::get<IdentifierExpr>(*ma.object->node);
        // Static enum variant access: EnumName.VARIANT → address of the global singleton.
        if (cgEnumNames_.count(id.name.lexeme))
            return "@" + id.name.lexeme + "$" + ma.field.lexeme;
        // Static field access via type name: ClassName::field → load from the global.
        if (const Type* sft = findStaticField(id.name.lexeme, ma.field.lexeme))
            return emitLoad(irTypeName(*sft), "@" + id.name.lexeme + "$" + ma.field.lexeme);
    }

    std::string objPtr = genExpr(*ma.object);
    Type objType = exprType(*ma.object);
    if (objType.kind != TypeKind::Object && objType.kind != TypeKind::Reference
        && objType.kind != TypeKind::Enum) return "0";
    // Static field read through an instance: obj.staticField → the shared global.
    if (const Type* sft = findStaticField(objType.className, ma.field.lexeme))
        return emitLoad(irTypeName(*sft), "@" + objType.className + "$" + ma.field.lexeme);
    auto [gepReg, fieldType] = resolveFieldGEP(objPtr, objType.className, ma.field.lexeme);
    if (fieldType.kind == TypeKind::Error) return "0";
    return emitLoad(irTypeName(fieldType), gepReg);
}

// ---- Member assign (field write) ----

std::string CodeGen::genMemberAssign(const MemberAssignExpr& ma) {
    // Static field write via type name: ClassName::field = value → store to the global.
    if (std::holds_alternative<IdentifierExpr>(*ma.object->node)) {
        const auto& id = std::get<IdentifierExpr>(*ma.object->node);
        if (const Type* sft = findStaticField(id.name.lexeme, ma.field.lexeme)) {
            Type        valueType = exprType(*ma.value);
            std::string value     = genExpr(*ma.value);
            value = emitCast(value, valueType, *sft);
            emitStore(irTypeName(*sft), value, "@" + id.name.lexeme + "$" + ma.field.lexeme);
            return value;
        }
    }

    std::string objPtr = genExpr(*ma.object);
    Type objType = exprType(*ma.object);
    if (objType.kind != TypeKind::Object && objType.kind != TypeKind::Reference
        && objType.kind != TypeKind::Enum) return "0";
    // Static field write through an instance: obj.staticField = value.
    if (const Type* sft = findStaticField(objType.className, ma.field.lexeme)) {
        Type        valueType = exprType(*ma.value);
        std::string value     = genExpr(*ma.value);
        value = emitCast(value, valueType, *sft);
        emitStore(irTypeName(*sft), value, "@" + objType.className + "$" + ma.field.lexeme);
        return value;
    }
    auto [gepReg, fieldType] = resolveFieldGEP(objPtr, objType.className, ma.field.lexeme);
    if (fieldType.kind == TypeKind::Error) return "0";

    // Reference field: co-ownership — retain the new target, release the old.
    if (fieldType.kind == TypeKind::Reference) {
        usesRefcount_ = true;
        bool plusOne = producesPlusOne(*ma.value);
        Type        valueType = exprType(*ma.value);
        std::string newVal    = genExpr(*ma.value);
        newVal = emitCast(newVal, valueType, fieldType);
        if (plusOne) claimTemp(newVal);
        else         emit("call void @gg_retain(ptr " + newVal + ")");
        std::string oldVal = emitLoad("ptr", gepReg);
        auto fcgIt = cgClasses_.find(fieldType.className);
        std::string dtorArg = (fcgIt != cgClasses_.end() && fcgIt->second.needsDtor)
                            ? ("@" + fieldType.className + "_dtor") : "null";
        emit("call void @gg_release(ptr " + oldVal + ", ptr " + dtorArg + ")");
        emitStore("ptr", newVal, gepReg);
        return newVal;
    }

    Type        valueType = exprType(*ma.value);
    std::string value     = genExpr(*ma.value);
    value = emitCast(value, valueType, fieldType);
    emitStore(irTypeName(fieldType), value, gepReg);
    return value;
}

// ---- Method call ----

std::string CodeGen::genTraitMethodCall(const void* node, const std::string& className,
                                        const std::string& method, const std::string& recvPtr,
                                        const std::vector<Type>& argTypes,
                                        const std::vector<std::string>& argVals, Type& retOut) {
    std::string sym = calleeName(node, className + "_" + method);
    auto pit = funcParamTypes.find(sym);
    std::string argStr = "ptr " + recvPtr;
    for (size_t i = 0; i < argVals.size(); ++i) {
        Type pt = (pit != funcParamTypes.end() && i < pit->second.size()) ? pit->second[i] : argTypes[i];
        std::string v = emitCast(argVals[i], argTypes[i], pt);
        argStr += ", " + irTypeName(pt) + " " + v;
    }
    Type ret = funcReturnTypes.count(sym) ? funcReturnTypes.at(sym) : Type{TypeKind::Void};
    retOut = ret;
    std::string retIr = irTypeName(ret);
    if (retIr == "void") { emit("call void @" + sym + "(" + argStr + ")"); return ""; }
    std::string t = freshTemp();
    emit("%" + t + " = call " + retIr + " @" + sym + "(" + argStr + ")");
    if (ret.kind == TypeKind::Reference) pendingTemps_.push_back({ "%" + t, ret.className });
    return "%" + t;
}

std::string CodeGen::genMethodCall(const MethodCallExpr& mc, const Type& resolvedType) {
    std::string returnIrType = irTypeName(resolvedType);

    // Static call through the type name: ClassName::method(args) — no receiver.
    if (std::holds_alternative<IdentifierExpr>(*mc.object->node)) {
        const auto& id = std::get<IdentifierExpr>(*mc.object->node);
        auto cgIt = cgClasses_.find(id.name.lexeme);
        if (cgIt != cgClasses_.end() && cgIt->second.staticMethods.count(mc.method.lexeme))
            return genStaticCall(id.name.lexeme, mc, resolvedType, returnIrType);
    }

    std::string objPtr = genExpr(*mc.object);
    Type objType = exprType(*mc.object);
    if (objType.kind != TypeKind::Object && objType.kind != TypeKind::Reference
        && objType.kind != TypeKind::Enum) return "0";

    // Static call through an instance: obj.staticMethod(args) — drop the receiver.
    {
        auto cgIt = cgClasses_.find(objType.className);
        if (cgIt != cgClasses_.end() && cgIt->second.staticMethods.count(mc.method.lexeme))
            return genStaticCall(objType.className, mc, resolvedType, returnIrType);
    }

    std::string mangledName  = calleeName(&mc, objType.className + "_" + mc.method.lexeme);

    auto funcIt = funcParamTypes.find(mangledName);
    const std::vector<Type>* declaredParams =
        funcIt != funcParamTypes.end() ? &funcIt->second : nullptr;

    std::string argStr   = buildArgString(mc.args, declaredParams);
    std::string fullArgs = "ptr " + objPtr + (argStr.empty() ? "" : ", " + argStr);

    if (returnIrType == "void") {
        emit("call void @" + mangledName + "(" + fullArgs + ")");
        return "";
    }
    std::string t = freshTemp();
    emit("%" + t + " = call " + returnIrType + " @" + mangledName + "(" + fullArgs + ")");
    if (resolvedType.kind == TypeKind::Reference)   // reference-returning method hands back a +1
        pendingTemps_.push_back({ "%" + t, resolvedType.className });
    return "%" + t;
}

// Emit a static method call (no implicit `this`): @ClassName_method(args).
std::string CodeGen::genStaticCall(const std::string& className,
                                   const MethodCallExpr& mc,
                                   const Type& resolvedType,
                                   const std::string& returnIrType) {
    std::string mangledName = calleeName(&mc, className + "_" + mc.method.lexeme);

    auto funcIt = funcParamTypes.find(mangledName);
    const std::vector<Type>* declaredParams =
        funcIt != funcParamTypes.end() ? &funcIt->second : nullptr;

    std::string fullArgs = buildArgString(mc.args, declaredParams);

    if (returnIrType == "void") {
        emit("call void @" + mangledName + "(" + fullArgs + ")");
        return "";
    }
    std::string t = freshTemp();
    emit("%" + t + " = call " + returnIrType + " @" + mangledName + "(" + fullArgs + ")");
    if (resolvedType.kind == TypeKind::Reference)
        pendingTemps_.push_back({ "%" + t, resolvedType.className });
    return "%" + t;
}
