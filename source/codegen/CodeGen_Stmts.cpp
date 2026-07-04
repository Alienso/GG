#include <ranges>

#include "CodeGen.h"

// ============================================================
// Statement codegen
// ============================================================

void CodeGen::genStmt(const Stmt& stmt) {
    std::visit(overloaded{
        [&](const ExprStmt& exprStmt)      { genExpr(exprStmt.expression); flushTempReleases(); },
        [&](const BlockStmt& blockStmt)    { genBlock(blockStmt); },
        [&](const IfStmt& ifStmt)          { genIf(ifStmt); },
        [&](const WhileStmt& whileStmt)    { genWhile(whileStmt); },
        [&](const ForStmt& forStmt)        { genFor(forStmt); },
        [&](const ReturnStmt& returnStmt)    { genReturn(returnStmt); },
        [&](const BreakStmt& breakStmt)     { genBreak(breakStmt); },
        [&](const ContinueStmt& continueStmt) { genContinue(continueStmt); },
        [&](const FunctionDeclStmt&)         { /* nested functions not supported */ },
        [&](const ExternFuncDeclStmt&)       { /* handled at module level in generate() */ },
        [&](const ImportStmt&)               { /* resolved before codegen pass */ },
        [&](const ClassDeclStmt&)            { /* handled at module level in generate() */ },
        [&](const EnumDeclStmt&)             { /* handled at module level in generate() */ },
        [&](const TraitDeclStmt&)            { /* no code — a contract only */ },
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
    // Evaluate return value first (before cleanup that could clobber temps).
    std::string retVal;
    if (returnStmt.value.has_value()) {
        Type returnValueType = exprType(*returnStmt.value);
        retVal               = genExpr(*returnStmt.value);
        retVal               = emitCast(retVal, returnValueType, currentReturnType);

        // Reference return: hand the caller an owned (+1) reference.
        //   +1 producer (new / ref-returning call) → take ownership of its pending release.
        //   borrowed reference (variable / field / param) → retain to produce the +1.
        if (currentReturnType.kind == TypeKind::Reference) {
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
