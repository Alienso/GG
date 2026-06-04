//
// Created by Vladimir Arsenijevic on 4.6.2026.
//

#include "CodeGen.h"
#include "../semantic/Type.h"

#include <cassert>
#include <sstream>
#include <stdexcept>

// ============================================================
// Public entry point
// ============================================================

IRModule CodeGen::generate(const Program& program, const ExprTypeMap& typeMap) {
    module_     = {};
    typeMap_    = &typeMap;
    strCounter_ = 0;

    for (const auto& decl : program.declarations) {
        if (decl.node && std::holds_alternative<FunctionDeclStmt>(*decl.node)) {
            genFunction(std::get<FunctionDeclStmt>(*decl.node));
        }
    }
    return std::move(module_);
}

// ============================================================
// Function codegen
// ============================================================

void CodeGen::genFunction(const FunctionDeclStmt& f) {
    // Reset per-function state
    tempCounter_        = 0;
    labelCounter_       = 0;
    allocaMap_.clear();
    varTypeMap_.clear();

    // Return type
    currentReturnType_  = typeFromToken(f.returnType.type);
    std::string retIrT  = irTypeName(currentReturnType_);

    // Build parameter list string
    std::string paramStr;
    for (size_t i = 0; i < f.params.size(); ++i) {
        if (i > 0) paramStr += ", ";
        paramStr += irTypeName(typeFromToken(f.params[i].typeName.type));
        paramStr += " %";
        paramStr += f.params[i].name.lexeme;
    }

    IRFunction fn;
    fn.signature = "define " + retIrT + " @" + f.name.lexeme + "(" + paramStr + ")";

    module_.functions.push_back(std::move(fn));
    currentFn_ = &module_.functions.back();

    // Create the entry basic block
    currentFn_->blocks.push_back(BasicBlock{"entry", {}, false});
    currentBB_ = &currentFn_->blocks.back();

    // Spill each parameter into an alloca so it can be reassigned.
    for (const auto& p : f.params) {
        Type    paramType = typeFromToken(p.typeName.type);
        std::string irT   = irTypeName(paramType);
        std::string ptrReg = "%" + p.name.lexeme + ".addr";
        emitAlloca(ptrReg, irT);
        allocaMap_[p.name.lexeme]   = ptrReg;
        varTypeMap_[p.name.lexeme]  = paramType;
        emitStore(irT, "%" + p.name.lexeme, ptrReg);
    }

    // Codegen body statements
    for (const auto& stmtPtr : f.body.body) {
        if (stmtPtr) genStmt(*stmtPtr);
    }

    // Ensure each block is terminated
    if (currentBB_ && !currentBB_->terminated) {
        if (retIrT == "void") {
            emit("ret void");
        } else {
            // Semantic pass should have caught missing return; emit unreachable
            emit("unreachable");
        }
        currentBB_->terminated = true;
    }

    currentFn_ = nullptr;
    currentBB_ = nullptr;
}

// ============================================================
// Statement codegen
// ============================================================

void CodeGen::genStmt(const Stmt& s) {
    // We need the function's return IR type for genReturn.
    // Extract it from the current function's signature lazily.
    std::visit(overloaded{
        [&](const ExprStmt& es)      { genExpr(es.expression); },
        [&](const BlockStmt& bs)     { genBlock(bs); },
        [&](const IfStmt& is)        { genIf(is); },
        [&](const WhileStmt& ws)     { genWhile(ws); },
        [&](const ForStmt& fs)       { genFor(fs); },
        [&](const ReturnStmt& rs)    { genReturn(rs); },
        [&](const FunctionDeclStmt&) { /* nested functions not supported */ },
    }, *s.node);
}

void CodeGen::genBlock(const BlockStmt& b) {
    // Snapshot current scope so inner declarations / shadows are undone on exit.
    auto savedAllocas = allocaMap_;
    auto savedTypes   = varTypeMap_;

    for (const auto& stmtPtr : b.body)
        if (stmtPtr) genStmt(*stmtPtr);

    // Restore: removes names added in this block and restores any shadowed names.
    // Allocas themselves remain in the IR (they live in the entry block), but they
    // become unreachable via the map after the scope exits — correct scoping.
    allocaMap_ = std::move(savedAllocas);
    varTypeMap_ = std::move(savedTypes);
}

void CodeGen::genIf(const IfStmt& s) {
    int n = ++labelCounter_;
    std::string thenLabel  = "if.then."  + std::to_string(n);
    std::string elseLabel  = "if.else."  + std::to_string(n);
    std::string mergeLabel = "if.merge." + std::to_string(n);

    Type condTy     = exprType(s.condition);
    std::string cv  = genExpr(s.condition);
    std::string cb  = emitToBool(cv, condTy);

    if (s.elseBranch) {
        emitCondBr(cb, thenLabel, elseLabel);
    } else {
        emitCondBr(cb, thenLabel, mergeLabel);
    }

    // Then block
    switchBlock(thenLabel);
    genStmt(*s.thenBranch);
    if (!currentBB_->terminated) emitBr(mergeLabel);

    // Else block
    if (s.elseBranch) {
        switchBlock(elseLabel);
        genStmt(*s.elseBranch);
        if (!currentBB_->terminated) emitBr(mergeLabel);
    }

    // Merge block
    switchBlock(mergeLabel);
}

void CodeGen::genWhile(const WhileStmt& s) {
    int n = ++labelCounter_;
    std::string condLabel  = "while.cond."  + std::to_string(n);
    std::string bodyLabel  = "while.body."  + std::to_string(n);
    std::string mergeLabel = "while.merge." + std::to_string(n);

    emitBr(condLabel);

    switchBlock(condLabel);
    Type condTy     = exprType(s.condition);
    std::string cv  = genExpr(s.condition);
    std::string cb  = emitToBool(cv, condTy);
    emitCondBr(cb, bodyLabel, mergeLabel);

    switchBlock(bodyLabel);
    genStmt(*s.body);
    if (!currentBB_->terminated) emitBr(condLabel);

    switchBlock(mergeLabel);
}

void CodeGen::genFor(const ForStmt& s) {
    // The for-init variable belongs to the for scope (not the enclosing scope).
    auto savedAllocas = allocaMap_;
    auto savedTypes   = varTypeMap_;

    int n = ++labelCounter_;
    std::string condLabel  = "for.cond."  + std::to_string(n);
    std::string bodyLabel  = "for.body."  + std::to_string(n);
    std::string incLabel   = "for.inc."   + std::to_string(n);
    std::string mergeLabel = "for.merge." + std::to_string(n);

    if (s.init) genStmt(*s.init);

    emitBr(condLabel);

    switchBlock(condLabel);
    if (s.condition.has_value()) {
        Type condTy     = exprType(*s.condition);
        std::string cv  = genExpr(*s.condition);
        std::string cb  = emitToBool(cv, condTy);
        emitCondBr(cb, bodyLabel, mergeLabel);
    } else {
        emitBr(bodyLabel);  // for(;;)
    }

    switchBlock(bodyLabel);
    genStmt(*s.body);
    if (!currentBB_->terminated) emitBr(incLabel);

    switchBlock(incLabel);
    if (s.increment.has_value()) genExpr(*s.increment);
    emitBr(condLabel);

    switchBlock(mergeLabel);

    // Restore scope — for-init variable goes out of scope
    allocaMap_ = std::move(savedAllocas);
    varTypeMap_ = std::move(savedTypes);
}

void CodeGen::genReturn(const ReturnStmt& s) {
    if (s.value.has_value()) {
        Type valTy      = exprType(*s.value);
        std::string val = genExpr(*s.value);
        // Cast the value to the declared return type if needed
        val = emitCast(val, valTy, currentReturnType_);
        emit("ret " + irTypeName(currentReturnType_) + " " + val);
    } else {
        emit("ret void");
    }
    if (currentBB_) currentBB_->terminated = true;
}

// ============================================================
// Expression codegen
// ============================================================

std::string CodeGen::genExpr(const Expr& e) {
    Type resolvedType = exprType(e);
    return std::visit(overloaded{
        [&](const LiteralExpr& x)        -> std::string { return genLiteral(x, resolvedType); },
        [&](const IdentifierExpr& x)     -> std::string { return genIdentifier(x); },
        [&](const UnaryExpr& x)          -> std::string { return genUnary(x, resolvedType); },
        [&](const BinaryExpr& x)         -> std::string { return genBinary(x, resolvedType); },
        [&](const AssignExpr& x)         -> std::string { return genAssign(x); },
        [&](const CompoundAssignExpr& x) -> std::string { return genCompoundAssign(x); },
        [&](const PostfixExpr& x)        -> std::string { return genPostfix(x); },
        [&](const CallExpr& x)           -> std::string { return genCall(x, resolvedType); },
        [&](const VarDeclExpr& x)        -> std::string { return genVarDecl(x); },
    }, *e.node);
}

// ---- Literal ----

std::string CodeGen::genLiteral(const LiteralExpr& e, Type resolvedType) {
    const std::string& lex = e.token.lexeme;

    switch (e.token.type) {
        case TokenType::NUMBER: {
            if (lex.find('.') != std::string::npos) {
                // Float literal — ensure at least one digit after '.'
                std::string val = lex;
                if (!val.empty() && val.back() == '.') val += '0';
                return val;
            }
            // Integer literal
            return lex;
        }
        case TokenType::TRUE:  return "1";
        case TokenType::FALSE: return "0";

        case TokenType::CHAR: {
            // The lexer stores the char lexeme WITHOUT the surrounding single quotes.
            // e.g., 'A' → lexeme "A", '\n' → lexeme "\n" (backslash + n).
            if (!lex.empty()) {
                if (lex[0] == '\\' && lex.size() >= 2) {
                    // Escape sequence
                    switch (lex[1]) {
                        case 'n':  return "10";
                        case 't':  return "9";
                        case '\\': return "92";
                        case '\'': return "39";
                        case '0':  return "0";
                        default:   return std::to_string(static_cast<int>((unsigned char)lex[1]));
                    }
                }
                return std::to_string(static_cast<int>((unsigned char)lex[0]));
            }
            return "0";
        }

        case TokenType::STRING: {
            // The lexer stores the string lexeme WITHOUT the surrounding double quotes.
            // e.g., "Hello" → lexeme "Hello".
            // Convert GG escape sequences to LLVM hex escape sequences.
            std::string content;
            int byteCount = 0;
            for (size_t i = 0; i < lex.size(); ++i) {
                if (lex[i] == '\\' && i + 1 < lex.size()) {
                    char esc = lex[++i];
                    switch (esc) {
                        case 'n':  content += "\\0A"; break;
                        case 't':  content += "\\09"; break;
                        case '\\': content += "\\5C"; break;
                        case '"':  content += "\\22"; break;
                        case '0':  content += "\\00"; break;
                        default:   content += lex[i]; break;
                    }
                } else {
                    content += lex[i];
                }
                ++byteCount;
            }
            int totalBytes = byteCount + 1;  // +1 for null terminator

            std::string globalName = "@.str." + std::to_string(strCounter_++);
            module_.globals.push_back(
                globalName + " = private unnamed_addr constant ["
                + std::to_string(totalBytes) + " x i8] c\""
                + content + "\\00\", align 1");

            std::string t = freshTemp();
            emit("%" + t + " = getelementptr inbounds ["
                + std::to_string(totalBytes) + " x i8], ptr "
                + globalName + ", i32 0, i32 0");
            return "%" + t;
        }

        default:
            (void)resolvedType;
            return "0";
    }
}

// ---- Identifier ----

std::string CodeGen::genIdentifier(const IdentifierExpr& e) {
    auto it = allocaMap_.find(e.name.lexeme);
    if (it == allocaMap_.end()) return "0";  // undefined — semantic pass should catch this

    auto tit = varTypeMap_.find(e.name.lexeme);
    if (tit == varTypeMap_.end()) return "0";

    std::string irT   = irTypeName(tit->second);
    std::string ptrReg = it->second;
    return emitLoad(irT, ptrReg);
}

// ---- Unary ----

std::string CodeGen::genUnary(const UnaryExpr& e, Type resolvedType) {
    switch (e.op.type) {
        case TokenType::MINUS: {
            std::string val = genExpr(*e.operand);
            Type opTy = exprType(*e.operand);
            std::string irT = irTypeName(opTy);
            std::string t = freshTemp();
            if (isFloat(opTy.kind)) {
                emit("%" + t + " = fneg " + irT + " " + val);
            } else {
                emit("%" + t + " = sub " + irT + " 0, " + val);
            }
            return "%" + t;
        }
        case TokenType::BANG: {
            std::string val = genExpr(*e.operand);
            Type opTy = exprType(*e.operand);
            std::string bv  = emitToBool(val, opTy);
            std::string t   = freshTemp();
            emit("%" + t + " = xor i1 " + bv + ", true");
            return "%" + t;
        }
        case TokenType::TILDE: {
            std::string val = genExpr(*e.operand);
            Type opTy = exprType(*e.operand);
            std::string irT = irTypeName(opTy);
            std::string t   = freshTemp();
            emit("%" + t + " = xor " + irT + " " + val + ", -1");
            return "%" + t;
        }
        case TokenType::INCREMENT:
        case TokenType::DECREMENT: {
            // Prefix ++/-- : load, modify, store, return new value
            const auto& id = std::get<IdentifierExpr>(*e.operand->node);
            auto tit  = varTypeMap_.find(id.name.lexeme);
            auto ait  = allocaMap_.find(id.name.lexeme);
            if (tit == varTypeMap_.end() || ait == allocaMap_.end()) return "0";
            Type t2     = tit->second;
            std::string irT  = irTypeName(t2);
            std::string ptrR = ait->second;
            std::string old  = emitLoad(irT, ptrR);
            std::string t    = freshTemp();
            std::string one  = isFloat(t2.kind) ? "1.0" : "1";
            std::string op   = (e.op.type == TokenType::INCREMENT) ? "add" : "sub";
            if (isFloat(t2.kind)) op = (e.op.type == TokenType::INCREMENT) ? "fadd" : "fsub";
            emit("%" + t + " = " + op + " " + irT + " " + old + ", " + one);
            emitStore(irT, "%" + t, ptrR);
            return "%" + t;
        }
        default:
            (void)resolvedType;
            return genExpr(*e.operand);
    }
}

// ---- Binary ----

std::string CodeGen::genBinary(const BinaryExpr& e, Type resolvedType) {
    TokenType op = e.op.type;

    // Short-circuit logical ops
    if (op == TokenType::AND || op == TokenType::OR) {
        // Eager evaluation — emit and i1 / or i1
        Type lTy    = exprType(*e.left);
        Type rTy    = exprType(*e.right);
        std::string lv = genExpr(*e.left);
        std::string rv = genExpr(*e.right);
        std::string lb = emitToBool(lv, lTy);
        std::string rb = emitToBool(rv, rTy);
        std::string t  = freshTemp();
        std::string inst = (op == TokenType::AND) ? "and" : "or";
        emit("%" + t + " = " + inst + " i1 " + lb + ", " + rb);
        return "%" + t;
    }

    // Comparison ops — result is Bool (i1)
    bool isComparison = (op == TokenType::EQUAL_EQUAL || op == TokenType::BANG_EQUAL ||
                         op == TokenType::LESS         || op == TokenType::LESS_EQUAL  ||
                         op == TokenType::GREATER      || op == TokenType::GREATER_EQUAL);

    Type lTy = exprType(*e.left);
    Type rTy = exprType(*e.right);

    // Determine the arithmetic type for the operation
    Type opTy = isComparison
                ? commonArithmeticType(lTy, rTy)
                : resolvedType;

    std::string lv = genExpr(*e.left);
    std::string rv = genExpr(*e.right);

    // Cast operands to the common type
    lv = emitCast(lv, lTy, opTy);
    rv = emitCast(rv, rTy, opTy);

    std::string irT = irTypeName(opTy);
    std::string t   = freshTemp();

    if (isComparison) {
        std::string cmp = cmpInstr(op, opTy);
        emit("%" + t + " = " + cmp + " " + irT + " " + lv + ", " + rv);
    } else {
        std::string arith = arithInstr(op, opTy);
        emit("%" + t + " = " + arith + " " + irT + " " + lv + ", " + rv);
    }
    return "%" + t;
}

// ---- Assign ----

std::string CodeGen::genAssign(const AssignExpr& e) {
    auto tit = varTypeMap_.find(e.name.lexeme);
    auto ait = allocaMap_.find(e.name.lexeme);
    if (tit == varTypeMap_.end() || ait == allocaMap_.end()) return "0";

    Type lhsTy = tit->second;
    std::string irT  = irTypeName(lhsTy);
    std::string ptrR = ait->second;

    Type rhsTy      = exprType(*e.value);
    std::string val = genExpr(*e.value);
    val = emitCast(val, rhsTy, lhsTy);

    emitStore(irT, val, ptrR);
    return val;
}

// ---- CompoundAssign ----

std::string CodeGen::genCompoundAssign(const CompoundAssignExpr& e) {
    auto tit = varTypeMap_.find(e.name.lexeme);
    auto ait = allocaMap_.find(e.name.lexeme);
    if (tit == varTypeMap_.end() || ait == allocaMap_.end()) return "0";

    Type lhsTy  = tit->second;
    std::string irT   = irTypeName(lhsTy);
    std::string ptrR  = ait->second;

    // Load current value
    std::string cur = emitLoad(irT, ptrR);

    // Evaluate RHS
    Type rhsTy      = exprType(*e.value);
    std::string rhs = genExpr(*e.value);
    rhs = emitCast(rhs, rhsTy, lhsTy);

    // Apply the base operation
    TokenType baseOp = compoundBaseOp(e.op.type);
    std::string arith = arithInstr(baseOp, lhsTy);
    std::string t = freshTemp();
    emit("%" + t + " = " + arith + " " + irT + " " + cur + ", " + rhs);

    emitStore(irT, "%" + t, ptrR);
    return "%" + t;
}

// ---- Postfix ----

std::string CodeGen::genPostfix(const PostfixExpr& e) {
    const auto& id = std::get<IdentifierExpr>(*e.operand->node);
    auto tit  = varTypeMap_.find(id.name.lexeme);
    auto ait  = allocaMap_.find(id.name.lexeme);
    if (tit == varTypeMap_.end() || ait == allocaMap_.end()) return "0";

    Type t2     = tit->second;
    std::string irT  = irTypeName(t2);
    std::string ptrR = ait->second;

    // Load old value (this is the result of the postfix expression)
    std::string old = emitLoad(irT, ptrR);

    // Compute new value
    std::string t   = freshTemp();
    std::string one = isFloat(t2.kind) ? "1.0" : "1";
    std::string op  = (e.op.type == TokenType::INCREMENT) ? "add" : "sub";
    if (isFloat(t2.kind)) op = (e.op.type == TokenType::INCREMENT) ? "fadd" : "fsub";
    emit("%" + t + " = " + op + " " + irT + " " + old + ", " + one);
    emitStore(irT, "%" + t, ptrR);

    return old;  // return OLD value
}

// ---- Call ----

std::string CodeGen::genCall(const CallExpr& e, Type resolvedType) {
    std::string irRetT = irTypeName(resolvedType);

    std::string argStr;
    for (size_t i = 0; i < e.args.size(); ++i) {
        if (i > 0) argStr += ", ";
        Type argTy      = exprType(*e.args[i]);
        std::string val = genExpr(*e.args[i]);
        argStr += irTypeName(argTy) + " " + val;
    }

    if (irRetT == "void") {
        emit("call void @" + e.callee.lexeme + "(" + argStr + ")");
        return "";  // void call has no result
    }

    std::string t = freshTemp();
    emit("%" + t + " = call " + irRetT + " @" + e.callee.lexeme + "(" + argStr + ")");
    return "%" + t;
}

// ---- VarDecl ----

std::string CodeGen::genVarDecl(const VarDeclExpr& e) {
    Type declTy      = typeFromToken(e.typeName.type);
    std::string irT  = irTypeName(declTy);
    std::string name = e.name.lexeme;

    // Build a unique alloca pointer name.
    // If the variable shadows a name from an enclosing scope, append a counter
    // so the alloca names remain unique within the function.
    std::string ptrName = "%" + name + ".addr";
    if (allocaMap_.count(name)) {
        ptrName = "%" + name + ".addr." + std::to_string(tempCounter_++);
    }

    emitAlloca(ptrName, irT);
    allocaMap_[name]  = ptrName;
    varTypeMap_[name] = declTy;

    if (e.initializer) {
        Type initTy     = exprType(*e.initializer);
        std::string val = genExpr(*e.initializer);
        val = emitCast(val, initTy, declTy);
        emitStore(irT, val, ptrName);
    }

    return ptrName;
}

// ============================================================
// Low-level emit helpers
// ============================================================

void CodeGen::emit(const std::string& instr) {
    if (currentBB_ && !currentBB_->terminated)
        currentBB_->instructions.push_back("  " + instr);
}

void CodeGen::emitAlloca(const std::string& ptrName, const std::string& irT) {
    if (currentFn_)
        currentFn_->allocas.push_back("  " + ptrName + " = alloca " + irT);
}

void CodeGen::emitStore(const std::string& irT, const std::string& val, const std::string& ptr) {
    emit("store " + irT + " " + val + ", ptr " + ptr);
}

std::string CodeGen::emitLoad(const std::string& irT, const std::string& ptr) {
    std::string t = freshTemp();
    emit("%" + t + " = load " + irT + ", ptr " + ptr);
    return "%" + t;
}

void CodeGen::emitBr(const std::string& label) {
    if (currentBB_ && !currentBB_->terminated) {
        emit("br label %" + label);
        currentBB_->terminated = true;
    }
}

void CodeGen::emitCondBr(const std::string& cond,
                          const std::string& trueLabel,
                          const std::string& falseLabel) {
    if (currentBB_ && !currentBB_->terminated) {
        emit("br i1 " + cond + ", label %" + trueLabel + ", label %" + falseLabel);
        currentBB_->terminated = true;
    }
}

void CodeGen::switchBlock(const std::string& label) {
    if (currentFn_) {
        currentFn_->blocks.push_back(BasicBlock{label, {}, false});
        currentBB_ = &currentFn_->blocks.back();
    }
}

// ============================================================
// Value / type helpers
// ============================================================

std::string CodeGen::freshTemp() {
    return "t" + std::to_string(tempCounter_++);
}

std::string CodeGen::freshLabel(const std::string& hint) {
    return hint + "." + std::to_string(++labelCounter_);
}

Type CodeGen::exprType(const Expr& e) const {
    if (!e.node || !typeMap_) return Type{TypeKind::Error};
    auto it = typeMap_->find(e.node.get());
    if (it == typeMap_->end()) return Type{TypeKind::Error};
    return it->second;
}

std::string CodeGen::emitCast(const std::string& val, Type from, Type to) {
    if (from == to) return val;
    if (isError(from) || isError(to)) return val;

    auto fw = [](TypeKind k) -> int {
        switch (k) {
            case TypeKind::I8:  case TypeKind::U8:  case TypeKind::Char: return 8;
            case TypeKind::I16: case TypeKind::U16: return 16;
            case TypeKind::I32: case TypeKind::U32: return 32;
            case TypeKind::I64: case TypeKind::U64: return 64;
            case TypeKind::F32: return 32;
            case TypeKind::F64: return 64;
            case TypeKind::Bool: return 1;
            default: return 32;
        }
    };

    std::string fromIr = irTypeName(from);
    std::string toIr   = irTypeName(to);
    std::string instr;

    if (isInteger(from.kind) && isInteger(to.kind)) {
        int fb = fw(from.kind), tb = fw(to.kind);
        if (tb > fb) {
            instr = isUnsignedInt(from.kind) ? "zext" : "sext";
        } else if (tb < fb) {
            instr = "trunc";
        } else {
            // Same IR bit-width — just reinterpret; no instruction needed
            return val;
        }
    } else if (from.kind == TypeKind::Bool && isInteger(to.kind)) {
        instr = "zext";
    } else if (isInteger(from.kind) && to.kind == TypeKind::Bool) {
        // Convert to i1 via icmp ne
        return emitToBool(val, from);
    } else if (isFloat(from.kind) && isFloat(to.kind)) {
        int fb = fw(from.kind), tb = fw(to.kind);
        instr = (tb > fb) ? "fpext" : "fptrunc";
    } else if (isInteger(from.kind) && isFloat(to.kind)) {
        instr = isSignedInt(from.kind) ? "sitofp" : "uitofp";
    } else if (isFloat(from.kind) && isInteger(to.kind)) {
        instr = isSignedInt(to.kind) ? "fptosi" : "fptoui";
    } else {
        return val;  // no known cast
    }

    std::string t = freshTemp();
    emit("%" + t + " = " + instr + " " + fromIr + " " + val + " to " + toIr);
    return "%" + t;
}

std::string CodeGen::emitToBool(const std::string& val, Type t) {
    if (t.kind == TypeKind::Bool) return val;

    std::string irT = irTypeName(t);
    std::string tb  = freshTemp();
    if (isFloat(t.kind)) {
        emit("%" + tb + " = fcmp une " + irT + " " + val + ", 0.0");
    } else {
        // integer or char
        std::string zero = "0";
        emit("%" + tb + " = icmp ne " + irT + " " + val + ", " + zero);
    }
    return "%" + tb;
}

// ============================================================
// Arithmetic / comparison instruction selection
// ============================================================

std::string CodeGen::arithInstr(TokenType op, Type t) {
    bool fp  = isFloat(t.kind);
    bool sig = isSignedInt(t.kind);

    switch (op) {
        case TokenType::PLUS:         return fp ? "fadd" : "add";
        case TokenType::MINUS:        return fp ? "fsub" : "sub";
        case TokenType::STAR:         return fp ? "fmul" : "mul";
        case TokenType::SLASH:        return fp ? "fdiv" : (sig ? "sdiv" : "udiv");
        case TokenType::PERCENT:      return fp ? "frem" : (sig ? "srem" : "urem");
        case TokenType::AMPERSAND:    return "and";
        case TokenType::PIPE:         return "or";
        case TokenType::CARET:        return "xor";
        case TokenType::SHIFT_LEFT:   return "shl";
        case TokenType::SHIFT_RIGHT:  return sig ? "ashr" : "lshr";
        default:                      return "add";  // fallback
    }
}

std::string CodeGen::cmpInstr(TokenType op, Type t) {
    bool fp  = isFloat(t.kind);
    bool sig = isSignedInt(t.kind) || t.kind == TypeKind::Bool;

    if (fp) {
        // ordered comparisons (quiet NaN → false)
        switch (op) {
            case TokenType::EQUAL_EQUAL:   return "fcmp oeq";
            case TokenType::BANG_EQUAL:    return "fcmp one";
            case TokenType::LESS:          return "fcmp olt";
            case TokenType::LESS_EQUAL:    return "fcmp ole";
            case TokenType::GREATER:       return "fcmp ogt";
            case TokenType::GREATER_EQUAL: return "fcmp oge";
            default:                       return "fcmp oeq";
        }
    } else {
        switch (op) {
            case TokenType::EQUAL_EQUAL:   return "icmp eq";
            case TokenType::BANG_EQUAL:    return "icmp ne";
            case TokenType::LESS:          return sig ? "icmp slt" : "icmp ult";
            case TokenType::LESS_EQUAL:    return sig ? "icmp sle" : "icmp ule";
            case TokenType::GREATER:       return sig ? "icmp sgt" : "icmp ugt";
            case TokenType::GREATER_EQUAL: return sig ? "icmp sge" : "icmp uge";
            default:                       return "icmp eq";
        }
    }
}

TokenType CodeGen::compoundBaseOp(TokenType op) {
    switch (op) {
        case TokenType::PLUS_EQUAL:    return TokenType::PLUS;
        case TokenType::MINUS_EQUAL:   return TokenType::MINUS;
        case TokenType::STAR_EQUAL:    return TokenType::STAR;
        case TokenType::SLASH_EQUAL:   return TokenType::SLASH;
        case TokenType::PERCENT_EQUAL: return TokenType::PERCENT;
        case TokenType::AMPERSAND_EQUAL: return TokenType::AMPERSAND;
        case TokenType::PIPE_EQUAL:    return TokenType::PIPE;
        case TokenType::CARET_EQUAL:   return TokenType::CARET;
        default:                       return TokenType::PLUS;
    }
}
