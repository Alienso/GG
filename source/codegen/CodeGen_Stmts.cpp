#include <ranges>

#include "CodeGen.h"

// ============================================================
// Statement codegen
// ============================================================

void CodeGen::genStmt(const Stmt& stmt) {
    if (debug_) dbgStmtLine(stmt);
    std::visit(overloaded{
        [&](const ExprStmt& exprStmt)      { genExpr(exprStmt.expression); flushTempReleases(); },
        [&](const BlockStmt& blockStmt)    { genBlock(blockStmt); },
        [&](const IfStmt& ifStmt)          { genIf(ifStmt); },
        [&](const WhileStmt& whileStmt)    { genWhile(whileStmt); },
        [&](const ForStmt& forStmt)        { genFor(forStmt); },
        [&](const InlineForStmt&)          { /* expanded to plain statements by the parser */ },
        [&](const ReturnStmt& returnStmt)    { genReturn(returnStmt); },
        [&](const BreakStmt& breakStmt)     { genBreak(breakStmt); },
        [&](const ContinueStmt& continueStmt) { genContinue(continueStmt); },
        [&](const SwitchStmt& switchStmt)   { genSwitchStmt(switchStmt); },
        [&](const MatchStmt& matchStmt)     { genMatchStmt(matchStmt); },
        [&](const YieldStmt& yieldStmt)     { genYield(yieldStmt); },
        [&](const FunctionDeclStmt&)         { /* nested functions not supported */ },
        [&](const ExternFuncDeclStmt&)       { /* handled at module level in generate() */ },
        [&](const ImportStmt&)               { /* resolved before codegen pass */ },
        [&](const ClassDeclStmt&)            { /* handled at module level in generate() */ },
        [&](const EnumDeclStmt&)             { /* handled at module level in generate() */ },
        [&](const TraitDeclStmt&)            { /* no code — a contract only */ },
        [&](const AnnotationDeclStmt&)       { /* no code — compile-time metadata only */ },
        [&](const ImplDeclStmt&)             { /* handled at module level in generate() */ },
    }, *stmt.node);
}

void CodeGen::genBlock(const BlockStmt& blockStmt) {
    // Snapshot current scope so inner declarations / shadows are undone on exit.
    auto savedAllocas = allocaMap;
    auto savedTypes   = varTypeMap;

    dtorScopes_.emplace_back();   // open inner dtor scope

    for (const auto& stmtPtr : blockStmt.body) {
        if (stmtPtr) genStmt(*stmtPtr);
    }

    // Emit destructor calls in reverse declaration order before the block ends.
    emitDtorsForScope(dtorScopes_.back());
    dtorScopes_.pop_back();      // close inner dtor scope

    // Restore: removes names added in this block and restores any shadowed names.
    allocaMap  = std::move(savedAllocas);
    varTypeMap = std::move(savedTypes);
}

void CodeGen::genIf(const IfStmt& ifStmt) {
    int labelIndex          = ++labelCounter;
    std::string thenLabel   = "if.then."  + std::to_string(labelIndex);
    std::string elseLabel   = "if.else."  + std::to_string(labelIndex);
    std::string mergeLabel  = "if.merge." + std::to_string(labelIndex);

    Type        conditionType  = exprType(ifStmt.condition);
    std::string conditionValue = genExpr(ifStmt.condition);
    std::string conditionBool  = emitToBool(conditionValue, conditionType);
    flushTempReleases();   // release reference temporaries created in the condition

    if (ifStmt.elseBranch)
        emitCondBr(conditionBool, thenLabel, elseLabel);
    else
        emitCondBr(conditionBool, thenLabel, mergeLabel);

    // Then block
    switchBlock(thenLabel);
    genStmt(*ifStmt.thenBranch);
    if (!currentBasicBlock->terminated) emitBr(mergeLabel);

    // Else block
    if (ifStmt.elseBranch) {
        switchBlock(elseLabel);
        genStmt(*ifStmt.elseBranch);
        if (!currentBasicBlock->terminated) emitBr(mergeLabel);
    }

    // Merge block
    switchBlock(mergeLabel);
}

void CodeGen::genWhile(const WhileStmt& whileStmt) {
    int labelIndex          = ++labelCounter;
    std::string condLabel   = "while.cond."  + std::to_string(labelIndex);
    std::string bodyLabel   = "while.body."  + std::to_string(labelIndex);
    std::string mergeLabel  = "while.merge." + std::to_string(labelIndex);

    emitBr(condLabel);

    switchBlock(condLabel);
    Type        conditionType  = exprType(whileStmt.condition);
    std::string conditionValue = genExpr(whileStmt.condition);
    std::string conditionBool  = emitToBool(conditionValue, conditionType);
    flushTempReleases();
    emitCondBr(conditionBool, bodyLabel, mergeLabel);

    switchBlock(bodyLabel);
    breakLabelStack.push_back(mergeLabel);
    continueLabelStack.push_back(condLabel);
    genStmt(*whileStmt.body);
    breakLabelStack.pop_back();
    continueLabelStack.pop_back();
    if (!currentBasicBlock->terminated) emitBr(condLabel);

    switchBlock(mergeLabel);
}

void CodeGen::genFor(const ForStmt& forStmt) {
    // The for-init variable belongs to the for scope (not the enclosing scope).
    auto savedAllocas = allocaMap;
    auto savedTypes   = varTypeMap;

    int labelIndex          = ++labelCounter;
    std::string condLabel   = "for.cond."  + std::to_string(labelIndex);
    std::string bodyLabel   = "for.body."  + std::to_string(labelIndex);
    std::string incLabel    = "for.inc."   + std::to_string(labelIndex);
    std::string mergeLabel  = "for.merge." + std::to_string(labelIndex);

    if (forStmt.init) genStmt(*forStmt.init);

    emitBr(condLabel);

    switchBlock(condLabel);
    if (forStmt.condition.has_value()) {
        Type        conditionType  = exprType(*forStmt.condition);
        std::string conditionValue = genExpr(*forStmt.condition);
        std::string conditionBool  = emitToBool(conditionValue, conditionType);
        flushTempReleases();
        emitCondBr(conditionBool, bodyLabel, mergeLabel);
    } else {
        emitBr(bodyLabel);  // for(;;)
    }

    switchBlock(bodyLabel);
    breakLabelStack.push_back(mergeLabel);
    continueLabelStack.push_back(incLabel);
    genStmt(*forStmt.body);
    breakLabelStack.pop_back();
    continueLabelStack.pop_back();
    if (!currentBasicBlock->terminated) emitBr(incLabel);

    switchBlock(incLabel);
    if (forStmt.increment.has_value()) { genExpr(*forStmt.increment); flushTempReleases(); }
    emitBr(condLabel);

    switchBlock(mergeLabel);

    // Restore scope — for-init variable goes out of scope
    allocaMap  = std::move(savedAllocas);
    varTypeMap = std::move(savedTypes);
}

void CodeGen::genReturn(const ReturnStmt& returnStmt) {
    // Return-slot (sret) function: the result is already written in place into the caller's
    // slot, so `return slot;`/`return;` is `ret void`. The slot is NOT a local (it's the sret
    // param), so it is not registered for scope-exit destruction — ownership passes to the
    // caller. Other locals are still destroyed here.
    if (currentFnHasReturnSlot_) {
        flushTempReleases();
        for (auto& dtorScope : std::ranges::reverse_view(dtorScopes_))
            emitDtorsForScope(dtorScope);
        emit("ret void");
        if (currentBasicBlock) currentBasicBlock->terminated = true;
        return;
    }

    // Primitive/reference return alias: `return;` / `return alias;` both return the alias local.
    if (!currentReturnAliasLocal_.empty()) {
        emitReturnAlias();
        return;
    }

    // Evaluate return value first (before cleanup that could clobber temps).
    std::string retVal;
    if (returnStmt.value.has_value() && isPrimitiveBorrow(currentReturnType)) {
        // `ref <primitive>` return: hand back the referent pointer (address of the borrowed
        // lvalue / element). No load, no retain — a borrow owns nothing.
        retVal = genBorrowSource(*returnStmt.value);
    } else if (returnStmt.value.has_value()) {
        Type returnValueType = exprType(*returnStmt.value);
        retVal               = genExpr(*returnStmt.value);
        retVal               = emitCast(retVal, returnValueType, currentReturnType);

        // Reference return: hand the caller an owned (+1) reference.
        //   +1 producer (new / ref-returning call) → take ownership of its pending release.
        //   borrowed reference (variable / field / param) → retain to produce the +1.
        // A `ref` (borrow) return owns nothing — return the address as-is, no retain.
        if (currentReturnType.kind == TypeKind::Reference && !currentReturnType.borrow) {
            if (producesPlusOne(*returnStmt.value)) claimTemp(retVal);
            else                                    emit("call void @gg_retain(ptr " + retVal + ")");
        }
    }

    // Release any leaked reference temporaries from the return expression, then
    // release all live locals (innermost scope first). The returned reference was
    // either claimed (removed from pending) or retained, so it survives both.
    flushTempReleases();
    for (auto & dtorScope : std::ranges::reverse_view(dtorScopes_))
        emitDtorsForScope(dtorScope);

    if (returnStmt.value.has_value())
        emit("ret " + irTypeName(currentReturnType) + " " + retVal);
    else
        emit("ret void");

    if (currentBasicBlock) currentBasicBlock->terminated = true;
}

void CodeGen::emitDtorsForScope(const std::vector<DtorEntry>& scope) {
    // Emit in reverse declaration order (last declared → first destroyed).
    for (const auto& entry : std::ranges::reverse_view(scope)) {
        if (entry.isReference) {
            // Reference variable: load the heap pointer and release it.
            std::string ref = emitLoad("ptr", entry.allocaPtr);
            auto cgIt = cgClasses_.find(entry.className);
            std::string dtorArg = (cgIt != cgClasses_.end() && cgIt->second.needsDtor)
                                ? ("@" + entry.className + "_dtor") : "null";
            emit("call void @gg_release(ptr " + ref + ", ptr " + dtorArg + ")");
        } else {
            // Value object living in its alloca: run its destructor directly.
            emit("call void @" + entry.className + "_dtor(ptr " + entry.allocaPtr + ")");
        }
    }
}

void CodeGen::genBreak(const BreakStmt&) {
    if (!breakLabelStack.empty())
        emitBr(breakLabelStack.back());
}

void CodeGen::genContinue(const ContinueStmt&) {
    if (!continueLabelStack.empty())
        emitBr(continueLabelStack.back());
}

// Comparison-chain skeleton shared by the statement and expression forms. The scrutinee is
// already evaluated once (`scrutVal`). Each non-default arm becomes a test block that ORs the
// per-label equality (via emitEquality, honoring the Eq/default decision recorded by semantics)
// and conditionally branches to the arm body. `default` (if any) is the final fallthrough.
void CodeGen::genSwitchArms(const std::deque<SwitchArm>& arms, const std::string& scrutVal,
                            const Type& scrutType, const std::string& mergeLabel,
                            const std::function<void(const SwitchArm&)>& emitArmBody) {
    const SwitchArm* defaultArm = nullptr;
    for (const SwitchArm& arm : arms) {
        if (arm.isDefault) { defaultArm = &arm; continue; }
        std::string cond;
        for (const auto& label : arm.labels) {
            Type        lblType = exprType(*label);
            std::string lblVal  = genExpr(*label);
            std::string eq = emitEquality(label->node.get(), scrutVal, scrutType, lblVal, lblType,
                                          TokenType::EQUAL_EQUAL);
            if (cond.empty()) { cond = eq; }
            else {
                std::string t = freshTemp();
                emit("%" + t + " = or i1 " + cond + ", " + eq);
                cond = "%" + t;
            }
        }
        std::string armLabel  = freshLabel("sw.arm");
        std::string nextLabel = freshLabel("sw.test");
        emitCondBr(cond, armLabel, nextLabel);
        switchBlock(armLabel);
        emitArmBody(arm);
        emitBr(mergeLabel);          // no-op if the arm already terminated (yield/return)
        switchBlock(nextLabel);
    }
    // We are now in the no-match block: run `default` (if present), then fall through to merge.
    if (defaultArm) emitArmBody(*defaultArm);
    emitBr(mergeLabel);
}

void CodeGen::genSwitchStmt(const SwitchStmt& switchStmt) {
    Type        scrutType = exprType(switchStmt.scrutinee);
    std::string scrutVal  = genExpr(switchStmt.scrutinee);
    std::string mergeLabel = freshLabel("sw.merge");

    genSwitchArms(switchStmt.arms, scrutVal, scrutType, mergeLabel,
        [&](const SwitchArm& arm) {
            if (arm.block)          genStmt(*arm.block);      // genBlock manages its own scope/dtors
            else if (arm.valueExpr) { genExpr(*arm.valueExpr); flushTempReleases(); }
        });

    switchBlock(mergeLabel);
    flushTempReleases();   // release scrutinee temporaries at the switch boundary
}

std::string CodeGen::genSwitchExpr(const SwitchExpr& switchExpr, const Type& resolvedType) {
    std::string resultIr = irTypeName(resolvedType);
    std::string slot = "%" + freshTemp();
    emitAlloca(slot, resultIr);

    Type        scrutType = exprType(*switchExpr.scrutinee);
    std::string scrutVal  = genExpr(*switchExpr.scrutinee);
    std::string mergeLabel = freshLabel("sw.merge");

    switchExprStack_.push_back({ slot, resolvedType, mergeLabel });
    genSwitchArms(switchExpr.arms, scrutVal, scrutType, mergeLabel,
        [&](const SwitchArm& arm) {
            if (arm.valueExpr) {
                storeSwitchArmValue(*arm.valueExpr, slot, resolvedType);
                flushTempReleases();
            } else if (arm.block) {
                genStmt(*arm.block);   // a `yield` inside stores to the slot + branches to merge
            }
        });
    switchExprStack_.pop_back();

    switchBlock(mergeLabel);
    std::string result = emitLoad(resultIr, slot);
    // A reference result carries the +1 the winning arm transferred into the slot; hand it to the
    // consumer as a pending temp (claimed by a binding/return, else released at the boundary).
    if (resolvedType.kind == TypeKind::Reference && !resolvedType.borrow)
        pendingTemps_.push_back({ result, resolvedType.className });
    return result;
}

// ============================================================
// match / patterns
// ============================================================

std::string CodeGen::materializeScrutinee(const std::string& scrutVal, const Type& scrutType) {
    // An object's SSA value is already its address; every other value is a scalar, so spill it to a
    // fresh alloca. Result: `place` is uniformly a ptr to the value's storage (bindings alias it,
    // field GEPs recurse into it, literal tests load from it).
    if (scrutType.kind == TypeKind::Object) return scrutVal;
    std::string slot = "%" + freshTemp();
    emitAlloca(slot, irTypeName(scrutType));
    emitStore(irTypeName(scrutType), scrutVal, slot);
    return slot;
}

std::string CodeGen::emitPatternTest(const Pattern& pattern, const std::string& place,
                                     const Type& pType) {
    auto andCond = [&](const std::string& a, const std::string& b) -> std::string {
        if (a == "true") return b;
        if (b == "true") return a;
        std::string t = freshTemp();
        emit("%" + t + " = and i1 " + a + ", " + b);
        return "%" + t;
    };
    // The address to GEP into for destructuring: an object lives AT `place`; a reference `place`
    // holds the heap pointer, so load it to reach the object.
    auto objectAddr = [&]() -> std::string {
        return (pType.kind == TypeKind::Reference) ? emitLoad("ptr", place) : place;
    };

    return std::visit(overloaded{
        [&](const WildcardPat&) -> std::string { return "true"; },
        [&](const BindingPat& b) -> std::string {
            allocaMap[b.name.lexeme]  = place;      // read-only alias to the value's storage
            varTypeMap[b.name.lexeme] = pType;
            return "true";
        },
        [&](const LiteralPat& lit) -> std::string {
            Type        lt     = exprType(*lit.literal);
            std::string litVal = genExpr(*lit.literal);
            std::string scrutV = (pType.kind == TypeKind::Object) ? place
                                                                  : emitLoad(irTypeName(pType), place);
            return emitEquality(lit.literal->node.get(), scrutV, pType, litVal, lt, TokenType::EQUAL_EQUAL);
        },
        [&](const TuplePat& t) -> std::string {
            std::string addr = objectAddr();
            std::string cond = "true";
            for (size_t i = 0; i < t.elems.size(); ++i) {
                auto [fieldPtr, fieldType] = resolveFieldGEP(addr, pType.className, "_" + std::to_string(i));
                cond = andCond(cond, emitPatternTest(*t.elems[i], fieldPtr, fieldType));
            }
            return cond;
        },
        [&](const StructPat& s) -> std::string {
            std::string addr = objectAddr();
            std::string cond = "true";
            for (const auto& fp : s.fields) {
                auto [fieldPtr, fieldType] = resolveFieldGEP(addr, pType.className, fp.first.lexeme);
                cond = andCond(cond, emitPatternTest(*fp.second, fieldPtr, fieldType));
            }
            return cond;
        },
    }, *pattern.node);
}

void CodeGen::genMatchArms(const std::deque<MatchArm>& arms, const std::string& scrutPlace,
                           const Type& scrutType, const std::string& mergeLabel,
                           const std::function<void(const MatchArm&)>& emitArmBody) {
    for (const MatchArm& arm : arms) {
        // Pattern bindings are registered into allocaMap in the test block (their GEPs dominate the
        // arm body). Snapshot/restore so one arm's bindings never leak into the next.
        auto savedAllocas = allocaMap;
        auto savedTypes   = varTypeMap;

        std::string cond      = emitPatternTest(arm.pattern, scrutPlace, scrutType);
        std::string armLabel  = freshLabel("mt.arm");
        std::string nextLabel = freshLabel("mt.test");
        emitCondBr(cond, armLabel, nextLabel);       // `cond` is the literal "true" for an irrefutable arm
        switchBlock(armLabel);
        emitArmBody(arm);
        emitBr(mergeLabel);                          // no-op if the arm already terminated
        switchBlock(nextLabel);

        allocaMap  = std::move(savedAllocas);
        varTypeMap = std::move(savedTypes);
    }
    emitBr(mergeLabel);   // no arm matched (a statement match need not be exhaustive)
}

void CodeGen::genMatchStmt(const MatchStmt& matchStmt) {
    Type        scrutType = exprType(matchStmt.scrutinee);
    std::string scrutVal  = genExpr(matchStmt.scrutinee);
    std::string place     = materializeScrutinee(scrutVal, scrutType);
    std::string mergeLabel = freshLabel("mt.merge");

    genMatchArms(matchStmt.arms, place, scrutType, mergeLabel,
        [&](const MatchArm& arm) {
            if (arm.block)          genStmt(*arm.block);
            else if (arm.valueExpr) { genExpr(*arm.valueExpr); flushTempReleases(); }
        });

    switchBlock(mergeLabel);
    flushTempReleases();   // release scrutinee temporaries at the match boundary
}

std::string CodeGen::genMatchExpr(const MatchExpr& matchExpr, const Type& resolvedType) {
    std::string resultIr = irTypeName(resolvedType);
    std::string slot = "%" + freshTemp();
    emitAlloca(slot, resultIr);

    Type        scrutType = exprType(*matchExpr.scrutinee);
    std::string scrutVal  = genExpr(*matchExpr.scrutinee);
    std::string place     = materializeScrutinee(scrutVal, scrutType);
    std::string mergeLabel = freshLabel("mt.merge");

    switchExprStack_.push_back({ slot, resolvedType, mergeLabel });   // `yield` targets this slot
    genMatchArms(matchExpr.arms, place, scrutType, mergeLabel,
        [&](const MatchArm& arm) {
            if (arm.valueExpr) {
                storeSwitchArmValue(*arm.valueExpr, slot, resolvedType);
                flushTempReleases();
            } else if (arm.block) {
                genStmt(*arm.block);   // a `yield` inside stores to the slot + branches to merge
            }
        });
    switchExprStack_.pop_back();

    switchBlock(mergeLabel);
    std::string result = emitLoad(resultIr, slot);
    if (resolvedType.kind == TypeKind::Reference && !resolvedType.borrow)
        pendingTemps_.push_back({ result, resolvedType.className });
    return result;
}

// Store an arm/yield value into the switch-expression result slot, taking ownership of exactly one
// +1 for a reference result (claim a +1 producer, else retain a borrow) so the slot owns it.
void CodeGen::storeSwitchArmValue(const Expr& value, const std::string& slot, const Type& resultType) {
    Type        vt = exprType(value);
    bool        plusOne = (resultType.kind == TypeKind::Reference) && producesPlusOne(value);
    std::string v  = genExpr(value);
    std::string cv = emitCast(v, vt, resultType);
    if (resultType.kind == TypeKind::Reference) {
        usesRefcount_ = true;
        if (plusOne) claimTemp(cv);
        else         emit("call void @gg_retain(ptr " + cv + ")");
    }
    emitStore(irTypeName(resultType), cv, slot);
}

void CodeGen::genYield(const YieldStmt& yieldStmt) {
    if (switchExprStack_.empty()) return;   // semantic already reported the error
    SwitchExprTarget tgt = switchExprStack_.back();
    storeSwitchArmValue(yieldStmt.value, tgt.slotPtr, tgt.resultType);
    flushTempReleases();
    emitBr(tgt.mergeLabel);
}
