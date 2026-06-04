//
// Created by Vladimir Arsenijevic on 01.6.2026.
//

#ifndef GG_SEMANTICANALYZER_H
#define GG_SEMANTICANALYZER_H

#include <optional>
#include <unordered_map>
#include "SymbolTable.h"
#include "../parser/Ast.h"

// Maps each Expr variant pointer to its resolved Type.
// Key = expr.node.get() — stable because the AST is never mutated during analysis.
using ExprTypeMap = std::unordered_map<const Expr::Variant*, Type>;

struct SemanticResult {
    bool        hadError = false;
    ExprTypeMap typeMap;
};

class SemanticAnalyzer {
public:
    SemanticResult analyze(const Program& program);  // resets all state per call

private:
    SymbolTable         symbolTable_;
    ExprTypeMap         typeMap_;
    bool                hadError_          = false;
    std::optional<Type> currentReturnType_; // nullopt = top-level (not inside a function)

    // Pass 1: hoist top-level function signatures into the global scope
    void collectFunctions(const Program& program);

    // Statement analysis
    void analyzeStmt(const Stmt& stmt);
    void analyzeBlock(const BlockStmt& block);
    void analyzeIf(const IfStmt& s);
    void analyzeWhile(const WhileStmt& s);
    void analyzeFor(const ForStmt& s);
    void analyzeReturn(const ReturnStmt& s);
    void analyzeFunctionDecl(const FunctionDeclStmt& s);

    // Expression analysis — returns resolved Type and records it in typeMap_.
    // Not [[nodiscard]] because it is intentionally called for side effects
    // in error-recovery paths (e.g. analysing arguments after a type error).
    Type analyzeExpr(const Expr& expr);
    [[nodiscard]] Type analyzeLiteral(const LiteralExpr& e);
    [[nodiscard]] Type analyzeIdentifier(const IdentifierExpr& e);
    [[nodiscard]] Type analyzeUnary(const UnaryExpr& e);
    [[nodiscard]] Type analyzeBinary(const BinaryExpr& e);
    [[nodiscard]] Type analyzeAssign(const AssignExpr& e);
    [[nodiscard]] Type analyzeCompoundAssign(const CompoundAssignExpr& e);
    [[nodiscard]] Type analyzePostfix(const PostfixExpr& e);
    [[nodiscard]] Type analyzeCall(const CallExpr& e);
    [[nodiscard]] Type analyzeVarDecl(const VarDeclExpr& e);

    // Helpers
    void          enterScope();
    void          exitScope();
    const Symbol* lookupSymbol(const Token& nameToken);  // emits error if missing
    void          error(const Token& token, const std::string& message);
    void          warn(const Token& token, const std::string& message);
    void          recordType(const Expr& expr, Type t);
    void          checkCast(Type from, Type to, const Token& site,
                            const std::string& context);
};

#endif //GG_SEMANTICANALYZER_H
