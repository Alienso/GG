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
    IRModule           module;
    const ExprTypeMap* typeMap      = nullptr;
    int                stringCounter = 0;

    // ---- Per-function state (reset in genFunction) ----
    IRFunction*  currentFunction    = nullptr;
    BasicBlock*  currentBasicBlock  = nullptr;
    int          tempCounter        = 0;
    int          labelCounter       = 0;
    Type         currentReturnType  = Type{TypeKind::Void};

    // varName → alloca pointer register name, e.g. "x" → "%x.addr"
    std::unordered_map<std::string, std::string> allocaMap;

    // varName → declared Type (needed to emit correct load/store sizes)
    std::unordered_map<std::string, Type>        varTypeMap;

    // Innermost loop exit label (for break) and re-entry label (for continue).
    // Pushed on loop entry, popped on loop exit — supports arbitrary nesting.
    std::vector<std::string> breakLabelStack;
    std::vector<std::string> continueLabelStack;

    // ---- Function codegen ----
    void genFunction(const FunctionDeclStmt& function);

    // ---- Statement codegen ----
    void genStmt(const Stmt& stmt);
    void genBlock(const BlockStmt& blockStmt);
    void genIf(const IfStmt& ifStmt);
    void genWhile(const WhileStmt& whileStmt);
    void genFor(const ForStmt& forStmt);
    void genReturn(const ReturnStmt& returnStmt);
    void genBreak(const BreakStmt& breakStmt);
    void genContinue(const ContinueStmt& continueStmt);

    // ---- Expression codegen — return SSA value string ("%t3", "42", …) ----
    std::string genExpr(const Expr& expr);
    std::string genLiteral(const LiteralExpr& literal, Type resolvedType);
    std::string genIdentifier(const IdentifierExpr& identifier);
    std::string genUnary(const UnaryExpr& unary, Type resolvedType);
    std::string genBinary(const BinaryExpr& binary, Type resolvedType);
    std::string genAssign(const AssignExpr& assign);
    std::string genCompoundAssign(const CompoundAssignExpr& compoundAssign);
    std::string genPostfix(const PostfixExpr& postfix);
    std::string genCall(const CallExpr& call, Type resolvedType);
    std::string genVarDecl(const VarDeclExpr& varDecl);

    // ---- Low-level emit helpers ----
    void        emit(const std::string& instruction);
    void        emitAlloca(const std::string& ptrName, const std::string& irType);
    void        emitStore(const std::string& irType,
                          const std::string& value,
                          const std::string& ptr);
    std::string emitLoad(const std::string& irType, const std::string& ptr);
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
    Type        exprType(const Expr& expression) const;

    // Insert a cast instruction if from != to; return (possibly unchanged) value reg.
    std::string emitCast(const std::string& value, Type from, Type to);

    // Ensure result is i1 (suitable for conditional branches).
    std::string emitToBool(const std::string& value, Type valueType);

    // Arithmetic instruction name (add/fadd/sub/…) for a binary op on type t.
    static std::string arithInstr(TokenType operatorType, Type type);

    // Comparison instruction fragment (icmp slt / fcmp olt / …).
    static std::string cmpInstr(TokenType operatorType, Type type);

    // Compound-assign base op (PLUS_EQUAL → PLUS, etc.).
    static TokenType compoundBaseOp(TokenType operatorType);
};


#endif //GG_CODE_GEN_H
