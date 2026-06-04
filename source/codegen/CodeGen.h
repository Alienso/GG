//
// Created by Vladimir Arsenijevic on 4.6.2026.
//

#ifndef GG_CODE_GEN_H
#define GG_CODE_GEN_H

#include <string>
#include <unordered_map>
#include "IR.h"
#include "../parser/Ast.h"
#include "../semantic/SemanticAnalyzer.h"

class CodeGen {
public:
    IRModule generate(const Program& program, const ExprTypeMap& typeMap);

private:
    // ---- Module-level state ----
    IRModule           module_;
    const ExprTypeMap* typeMap_     = nullptr;
    int                strCounter_  = 0;

    // ---- Per-function state (reset in genFunction) ----
    IRFunction*  currentFn_         = nullptr;
    BasicBlock*  currentBB_         = nullptr;
    int          tempCounter_       = 0;
    int          labelCounter_      = 0;
    Type         currentReturnType_ = Type{TypeKind::Void};

    // varName → alloca pointer register name, e.g. "x" → "%x.addr"
    std::unordered_map<std::string, std::string> allocaMap_;

    // varName → declared Type (needed to emit correct load/store sizes)
    std::unordered_map<std::string, Type>        varTypeMap_;

    // ---- Function codegen ----
    void genFunction(const FunctionDeclStmt& f);

    // ---- Statement codegen ----
    void genStmt(const Stmt& s);
    void genBlock(const BlockStmt& b);
    void genIf(const IfStmt& s);
    void genWhile(const WhileStmt& s);
    void genFor(const ForStmt& s);
    void genReturn(const ReturnStmt& s);

    // ---- Expression codegen — return SSA value string ("%t3", "42", …) ----
    std::string genExpr(const Expr& e);
    std::string genLiteral(const LiteralExpr& e, Type resolvedType);
    std::string genIdentifier(const IdentifierExpr& e);
    std::string genUnary(const UnaryExpr& e, Type resolvedType);
    std::string genBinary(const BinaryExpr& e, Type resolvedType);
    std::string genAssign(const AssignExpr& e);
    std::string genCompoundAssign(const CompoundAssignExpr& e);
    std::string genPostfix(const PostfixExpr& e);
    std::string genCall(const CallExpr& e, Type resolvedType);
    std::string genVarDecl(const VarDeclExpr& e);

    // ---- Low-level emit helpers ----
    void        emit(const std::string& instr);
    void        emitAlloca(const std::string& ptrName, const std::string& irT);
    void        emitStore(const std::string& irT,
                          const std::string& val,
                          const std::string& ptr);
    std::string emitLoad(const std::string& irT, const std::string& ptr);
    void        emitBr(const std::string& label);
    void        emitCondBr(const std::string& cond,
                           const std::string& trueLabel,
                           const std::string& falseLabel);
    void        switchBlock(const std::string& label);

    // ---- Value / type helpers ----
    std::string freshTemp();
    std::string freshLabel(const std::string& hint);

    // Look up the resolved type for an expression via the side-table.
    // Returns TypeKind::Error if not found (should not happen after semantic pass).
    Type        exprType(const Expr& e) const;

    // Insert a cast instruction if from != to; return (possibly unchanged) value reg.
    std::string emitCast(const std::string& val, Type from, Type to);

    // Ensure result is i1 (suitable for conditional branches).
    std::string emitToBool(const std::string& val, Type t);

    // Arithmetic instruction name (add/fadd/sub/…) for a binary op on type t.
    static std::string arithInstr(TokenType op, Type t);

    // Comparison instruction fragment (icmp slt / fcmp olt / …).
    static std::string cmpInstr(TokenType op, Type t);

    // Compound-assign base op (PLUS_EQUAL → PLUS, etc.).
    static TokenType compoundBaseOp(TokenType op);
};


#endif //GG_CODE_GEN_H