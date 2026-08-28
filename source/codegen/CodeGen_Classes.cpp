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

std::string CodeGen::genMemberAccess(const MemberAccessExpr& ma, const Type& resolvedType) {
    // Safe access `x?.field`: evaluate the (nullable) receiver once; if null the result is the
    // empty value, else read the field and wrap it. Result flows through a slot.
    if (ma.safe) {
        Type recvType = stripNullable(exprType(*ma.object));
        std::string recv = genExpr(*ma.object);
        std::string resIr = irTypeName(resolvedType);
        bool optPrim = resolvedType.isNullable
            && (isNumeric(resolvedType.kind) || resolvedType.kind == TypeKind::Bool
                || resolvedType.kind == TypeKind::Char);
        std::string slot = "%" + freshTemp();
        emitAlloca(slot, resIr);
        emitStore(resIr, optPrim ? "zeroinitializer" : "null", slot);   // empty default
        std::string cmp = freshTemp();
        std::string present = freshLabel("safe.some"), merge = freshLabel("safe.merge");
        emit("%" + cmp + " = icmp ne ptr " + recv + ", null");
        emitCondBr("%" + cmp, present, merge);
        switchBlock(present);
        auto [gepReg, fieldType] = resolveFieldGEP(recv, recvType.className, ma.field.lexeme);
        if (fieldType.kind != TypeKind::Error) {
            std::string val = (fieldType.kind == TypeKind::Object) ? gepReg
                                                                   : emitLoad(irTypeName(fieldType), gepReg);
            emitStore(resIr, emitCast(val, fieldType, resolvedType), slot);
        }
        emitBr(merge);
        switchBlock(merge);
        return emitLoad(resIr, slot);
    }

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
    // `str` view: extract `.data` (field 0, ptr) or `.len` (field 1, i64) from the { ptr, i64 } value.
    if (objType.kind == TypeKind::Str) {
        int idx = (ma.field.lexeme == "len") ? 1 : 0;
        std::string ir = (idx == 1) ? "i64" : "ptr";
        std::string t = freshTemp();
        emit("%" + t + " = extractvalue { ptr, i64 } " + objPtr + ", " + std::to_string(idx));
        (void)ir;
        return "%" + t;
    }
    if (objType.kind != TypeKind::Object && objType.kind != TypeKind::Reference
        && objType.kind != TypeKind::Enum) return "0";
    // Static field read through an instance: obj.staticField → the shared global.
    if (const Type* sft = findStaticField(objType.className, ma.field.lexeme))
        return emitLoad(irTypeName(*sft), "@" + objType.className + "$" + ma.field.lexeme);
    auto [gepReg, fieldType] = resolveFieldGEP(objPtr, objType.className, ma.field.lexeme);
    if (fieldType.kind == TypeKind::Error) return "0";
    // An embedded value-object field's value IS its address (a GEP into the parent) — return it
    // directly so `.sub` chains, copies (clone), and value→reference borrows all work, exactly
    // like a local value object returns its alloca.
    if (fieldType.kind == TypeKind::Object) return gepReg;
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

    // Borrow field (`Class*`): non-owning — just store the pointer (no retain of the new, no release of
    // the old). A borrow does not own its target, so touching the refcount would leak (the dtor skips
    // borrow fields) or corrupt. A `+1` producer stored here is NOT claimed — the borrow takes no
    // ownership, so the temp is still released at the full-expression boundary (the borrow's lifetime
    // is the programmer's responsibility under --unsafe-ptr).
    if (fieldType.kind == TypeKind::Reference && fieldType.borrow) {
        Type        valueType = exprType(*ma.value);
        std::string newVal    = genExpr(*ma.value);
        newVal = emitCast(newVal, valueType, fieldType);
        emitStore("ptr", newVal, gepReg);
        return newVal;
    }

    // Reference field: co-ownership — retain the new target, release the old (atomic for Shared<T>).
    if (fieldType.kind == TypeKind::Reference) {
        usesRefcount_ = true;
        bool sh = fieldType.shared;
        if (sh) sharedUsed_ = true;
        bool plusOne = producesPlusOne(*ma.value);
        Type        valueType = exprType(*ma.value);
        std::string newVal    = genExpr(*ma.value);
        newVal = emitCast(newVal, valueType, fieldType);
        if (plusOne) claimTemp(newVal);
        else         emit(std::string("call void @") + retainFn(sh) + "(ptr " + newVal + ")");
        std::string oldVal = emitLoad("ptr", gepReg);
        auto fcgIt = cgClasses_.find(fieldType.className);
        std::string dtorArg = (fcgIt != cgClasses_.end() && fcgIt->second.needsDtor)
                            ? ("@" + fieldType.className + "_dtor") : "null";
        emit(std::string("call void @") + releaseFn(sh) + "(ptr " + oldVal + ", ptr " + dtorArg + ")");
        emitStore("ptr", newVal, gepReg);
        return newVal;
    }

    // Embedded value-object field: deep-copy the RHS into the field in place (the clone
    // releases the field's old reference targets and copies/retains the new ones).
    if (fieldType.kind == TypeKind::Object) {
        clonesNeeded_.insert(fieldType.className);
        std::string src = genExpr(*ma.value);   // Object→alloca; Reference→loaded heap ptr
        emit("call void @" + fieldType.className + "_clone(ptr " + gepReg + ", ptr " + src + ")");
        return gepReg;
    }

    Type        valueType = exprType(*ma.value);
    std::string value     = genExpr(*ma.value);
    value = emitCast(value, valueType, fieldType);
    emitStore(irTypeName(fieldType), value, gepReg);
    return value;
}

// ---- Field default initializer (injected into every constructor's prologue) ----

// Unlike genMemberAssign's Object-field branch (always construct-a-temp-then-clone — correct for an
// ordinary `this.field = ...` write, which may be overwriting a live value), a field's OWN default
// initializer runs on freshly zero-initialized storage, so it gets the zero-copy direct-construct
// fast path (the same one a local's defining assignment uses) first, falling back to clone only for
// a non-bare-constructor-call RHS shape.
void CodeGen::genFieldInitializer(const std::string& className, const std::string& fieldName,
                                  const Expr& init) {
    auto [gep, fieldType] = resolveFieldGEP("%self", className, fieldName);
    if (fieldType.kind == TypeKind::Error) return;

    if (fieldType.kind == TypeKind::Object) {
        if (!emitObjectDirectInit(init, gep, fieldType.className)) {
            clonesNeeded_.insert(fieldType.className);
            std::string src = genExpr(init);
            emit("call void @" + fieldType.className + "_clone(ptr " + gep + ", ptr " + src + ")");
        }
        return;
    }

    // Borrow field (`Class*`): non-owning — just store the pointer.
    if (fieldType.kind == TypeKind::Reference && fieldType.borrow) {
        Type        valueType = exprType(init);
        std::string newVal    = genExpr(init);
        newVal = emitCast(newVal, valueType, fieldType);
        emitStore("ptr", newVal, gep);
        return;
    }

    // Owning reference field: retain the new target. The field is freshly zero-initialized (this
    // prologue runs before any user code), so there is no old target to release — gg_release on the
    // still-null slot would be a safe no-op anyway, but skipping it documents that invariant here.
    if (fieldType.kind == TypeKind::Reference) {
        usesRefcount_ = true;
        if (fieldType.shared) sharedUsed_ = true;
        bool        plusOne  = producesPlusOne(init);
        Type        valueType = exprType(init);
        std::string newVal    = genExpr(init);
        newVal = emitCast(newVal, valueType, fieldType);
        if (plusOne) claimTemp(newVal);
        else         emit(std::string("call void @") + retainFn(fieldType.shared) + "(ptr " + newVal + ")");
        emitStore("ptr", newVal, gep);
        return;
    }

    Type        valueType = exprType(init);
    std::string value     = genExpr(init);
    value = emitCast(value, valueType, fieldType);
    emitStore(irTypeName(fieldType), value, gep);
}

// ---- Method call ----

std::string CodeGen::genTraitMethodCall(const void* node, const std::string& className,
                                        const std::string& method, const std::string& recvPtr,
                                        const std::vector<Type>& argTypes,
                                        const std::vector<std::string>& argVals, Type& retOut) {
    std::string sym = calleeName(node, className + "_" + method);
    auto pit = funcParamTypes.find(sym);
    Type ret = funcReturnTypes.count(sym) ? funcReturnTypes.at(sym) : Type{TypeKind::Void};
    retOut = ret;

    // Object-value return: the operator method uses the sret convention (hidden slot `ptr`
    // first, then the receiver, then declared args; `void` LLVM return). Materialize a temp
    // slot, fill it in place, and hand back its address — no heap allocation, no by-value
    // struct return. Without this the call would be emitted as `call %T @op(...)` and the
    // struct value would flow where a `ptr` is expected.
    if (ret.kind == TypeKind::Object && slotReturningFns_.count(sym)) {
        std::string slot   = materializeSlotTemp(ret.className);
        std::string argStr = "ptr " + slot + ", ptr " + recvPtr;
        for (size_t i = 0; i < argVals.size(); ++i) {
            Type pt = (pit != funcParamTypes.end() && i < pit->second.size()) ? pit->second[i] : argTypes[i];
            argStr += ", " + lowerArgOperand(argVals[i], argTypes[i], pt);
        }
        emit("call void @" + sym + "(" + argStr + ")");
        return slot;
    }

    std::string argStr = "ptr " + recvPtr;
    for (size_t i = 0; i < argVals.size(); ++i) {
        Type pt = (pit != funcParamTypes.end() && i < pit->second.size()) ? pit->second[i] : argTypes[i];
        argStr += ", " + lowerArgOperand(argVals[i], argTypes[i], pt);
    }
    std::string retIr = irTypeName(ret);
    if (retIr == "void") { emit("call void @" + sym + "(" + argStr + ")"); return ""; }
    std::string t = freshTemp();
    emit("%" + t + " = call " + retIr + " @" + sym + "(" + argStr + ")");
    if (ret.kind == TypeKind::Reference && !ret.borrow) pendingTemps_.push_back({ "%" + t, ret.className });
    return "%" + t;
}

std::string CodeGen::genMethodCall(const MethodCallExpr& mc, const Type& resolvedType) {
    // Scoped sync-cell access `cell.with/read/write(closure)` (Phase 2): acquire the lock, invoke the
    // closure with a borrow of the guarded interior, then release. Lowered here (not an ordinary
    // method call) because a closure type is unspellable and a lock guard must never be a first-class
    // value; the borrow's confinement to this call was checked in semantics.
    if (syncAccessCalls_) {
        auto it = syncAccessCalls_->find(&mc);
        if (it != syncAccessCalls_->end()) {
            const std::string& kind = it->second;            // "with"|"write" (exclusive), "read" (shared)
            std::string cellClass = exprType(*mc.object).className;
            std::string recv = genExpr(*mc.object);          // ptr to the cell (a Shared<> auto-derefs)
            auto [handleGep, ht] = resolveFieldGEP(recv, cellClass, "handle");
            std::string handle   = emitLoad("ptr", handleGep);       // the OS lock handle
            auto [valueGep, vt]  = resolveFieldGEP(recv, cellClass, "value");   // &interior : [mut] T*
            std::string closureClass = exprType(*mc.args[0]).className;
            std::string closure  = genExpr(*mc.args[0]);     // ptr to the materialized closure value
            // Lock mode by access kind. A Windows SRWLOCK needs a mode-matched release, so read and
            // write have distinct unlock helpers (POSIX maps both to pthread_rwlock_unlock).
            std::string lockFn, unlockFn;
            if (isRwLockName(cellClass)) {
                if (kind == "read") { lockFn = "gg_rwlock_rdlock"; unlockFn = "gg_rwlock_rdunlock"; }
                else                { lockFn = "gg_rwlock_wrlock"; unlockFn = "gg_rwlock_wrunlock"; }
            } else            { lockFn = "gg_mutex_lock"; unlockFn = "gg_mutex_unlock"; }
            emit("call void @" + lockFn + "(ptr " + handle + ")");
            emit("call void @" + closureClass + "_call(ptr " + closure + ", ptr " + valueGep + ")");
            emit("call void @" + unlockFn + "(ptr " + handle + ")");
            return "";
        }
    }

    // Built-in `obj.clone()`: materialize a fresh value-object temp, deep-copy the receiver into it
    // via `@Class_clone(dest, src)` (generated memberwise clone, or the user's `impl Clone`), and
    // yield the temp's address. Same `(dest, src)` shape as an sret return slot.
    if (builtinCloneCalls_) {
        auto it = builtinCloneCalls_->find(&mc);
        if (it != builtinCloneCalls_->end()) {
            const std::string& cn = it->second;
            std::string recv = genExpr(*mc.object);       // receiver's address (value obj or ref ptr)
            std::string slot = materializeSlotTemp(cn);   // alloca %cn, zero-init, dtor-scoped
            clonesNeeded_.insert(cn);
            emit("call void @" + cn + "_clone(ptr " + slot + ", ptr " + recv + ")");
            return slot;
        }
    }

    // Safe call `x?.m(args)`: evaluate the (nullable) receiver once; if null the result is empty
    // (or the void call is skipped), else call the instance method and wrap the result. `?.` never
    // yields a value object (rejected in semantics), so there is no sret path here.
    if (mc.safe) {
        std::string className = stripNullable(exprType(*mc.object)).className;
        std::string recv      = genExpr(*mc.object);
        bool isVoid  = resolvedType.kind == TypeKind::Void;
        bool optPrim = resolvedType.isNullable
            && (isNumeric(resolvedType.kind) || resolvedType.kind == TypeKind::Bool
                || resolvedType.kind == TypeKind::Char);
        std::string resIr = isVoid ? "" : irTypeName(resolvedType);
        std::string slot;
        if (!isVoid) {
            slot = "%" + freshTemp();
            emitAlloca(slot, resIr);
            emitStore(resIr, optPrim ? "zeroinitializer" : "null", slot);
        }
        std::string cmp = freshTemp();
        std::string present = freshLabel("safe.some"), merge = freshLabel("safe.merge");
        emit("%" + cmp + " = icmp ne ptr " + recv + ", null");
        emitCondBr("%" + cmp, present, merge);
        switchBlock(present);
        {
            std::string mName = calleeName(&mc, className + "_" + mc.method.lexeme);
            auto funcIt = funcParamTypes.find(mName);
            const std::vector<Type>* declaredParams =
                funcIt != funcParamTypes.end() ? &funcIt->second : nullptr;
            std::string argStr   = buildArgString(mc.args, declaredParams, defaultsFor(mName), orderFor(&mc));
            std::string fullArgs = "ptr " + recv + (argStr.empty() ? "" : ", " + argStr);
            Type        underlying = stripNullable(resolvedType);
            if (isVoid) {
                emit("call void @" + mName + "(" + fullArgs + ")");
            } else {
                std::string t = freshTemp();
                emit("%" + t + " = call " + irTypeName(underlying) + " @" + mName + "(" + fullArgs + ")");
                emitStore(resIr, emitCast("%" + t, underlying, resolvedType), slot);
            }
        }
        emitBr(merge);
        switchBlock(merge);
        if (isVoid) return "";
        std::string res = emitLoad(resIr, slot);
        // An owning-reference result carries a +1 (on the present path; null on the other) — hand it
        // to the caller as a pending temp; gg_release is null-safe so the null path is harmless.
        Type underlying = stripNullable(resolvedType);
        if (underlying.kind == TypeKind::Reference && !underlying.borrow)
            pendingTemps_.push_back({ res, underlying.className });
        return res;
    }

    std::string returnIrType = irTypeName(resolvedType);

    // Return-slot (sret) method used as a value (not a plain variable initializer — that path
    // writes in place via emitSlotCall): materialize the result into a temp object. Checked
    // before the ordinary receiver evaluation below.
    if (resolvedType.kind == TypeKind::Object) {
        // Static call via type name: Class::make(...).
        if (std::holds_alternative<IdentifierExpr>(*mc.object->node)) {
            const auto& id = std::get<IdentifierExpr>(*mc.object->node);
            auto cgIt = cgClasses_.find(id.name.lexeme);
            if (cgIt != cgClasses_.end() && cgIt->second.staticMethods.count(mc.method.lexeme)) {
                std::string mName = calleeName(&mc, id.name.lexeme + "_" + mc.method.lexeme);
                if (slotReturningFns_.count(mName)) {
                    std::string tmp = materializeSlotTemp(resolvedType.className);
                    emitSretCall(mName, mc.args, tmp, "");
                    return tmp;
                }
            }
        } else {
            Type objType = exprType(*mc.object);
            if (objType.kind == TypeKind::Object || objType.kind == TypeKind::Reference) {
                std::string mName = calleeName(&mc, objType.className + "_" + mc.method.lexeme);
                if (slotReturningFns_.count(mName)) {
                    auto cgIt = cgClasses_.find(objType.className);
                    bool isStatic = cgIt != cgClasses_.end()
                                 && cgIt->second.staticMethods.count(mc.method.lexeme) > 0;
                    std::string recv = isStatic ? "" : genExpr(*mc.object);
                    std::string tmp  = materializeSlotTemp(resolvedType.className);
                    emitSretCall(mName, mc.args, tmp, recv);
                    return tmp;
                }
            }
        }
    }

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

    std::string argStr   = buildArgString(mc.args, declaredParams, defaultsFor(mangledName), orderFor(&mc));
    std::string fullArgs = "ptr " + objPtr + (argStr.empty() ? "" : ", " + argStr);

    if (returnIrType == "void") {
        emit("call void @" + mangledName + "(" + fullArgs + ")");
        return "";
    }
    std::string t = freshTemp();
    emit("%" + t + " = call " + returnIrType + " @" + mangledName + "(" + fullArgs + ")");
    if (resolvedType.kind == TypeKind::Reference && !resolvedType.borrow)   // owning ref-return hands back a +1 (a borrow owns nothing)
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

    std::string fullArgs = buildArgString(mc.args, declaredParams, defaultsFor(mangledName), orderFor(&mc));

    if (returnIrType == "void") {
        emit("call void @" + mangledName + "(" + fullArgs + ")");
        return "";
    }
    std::string t = freshTemp();
    emit("%" + t + " = call " + returnIrType + " @" + mangledName + "(" + fullArgs + ")");
    if (resolvedType.kind == TypeKind::Reference && !resolvedType.borrow)
        pendingTemps_.push_back({ "%" + t, resolvedType.className });
    return "%" + t;
}
