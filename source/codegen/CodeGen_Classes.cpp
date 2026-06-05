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

std::string CodeGen::genMemberAccess(const MemberAccessExpr& ma) {
    std::string objPtr = genExpr(*ma.object);
    Type objType = exprType(*ma.object);
    if (objType.kind != TypeKind::Object) return "0";
    auto [gepReg, fieldType] = resolveFieldGEP(objPtr, objType.className, ma.field.lexeme);
    if (fieldType.kind == TypeKind::Error) return "0";
    return emitLoad(irTypeName(fieldType), gepReg);
}

// ---- Member assign (field write) ----

std::string CodeGen::genMemberAssign(const MemberAssignExpr& ma) {
    std::string objPtr = genExpr(*ma.object);
    Type objType = exprType(*ma.object);
    if (objType.kind != TypeKind::Object) return "0";
    auto [gepReg, fieldType] = resolveFieldGEP(objPtr, objType.className, ma.field.lexeme);
    if (fieldType.kind == TypeKind::Error) return "0";
    Type        valueType = exprType(*ma.value);
    std::string value     = genExpr(*ma.value);
    value = emitCast(value, valueType, fieldType);
    emitStore(irTypeName(fieldType), value, gepReg);
    return value;
}

// ---- Method call ----

std::string CodeGen::genMethodCall(const MethodCallExpr& mc, const Type& resolvedType) {
    std::string objPtr = genExpr(*mc.object);
    Type objType = exprType(*mc.object);
    if (objType.kind != TypeKind::Object) return "0";

    std::string mangledName  = objType.className + "_" + mc.method.lexeme;
    std::string returnIrType = irTypeName(resolvedType);

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
    return "%" + t;
}
