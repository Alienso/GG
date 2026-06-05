//
// Created by Vladimir Arsenijevic on 01.6.2026.
//

#ifndef GG_SEMANTICANALYZER_H
#define GG_SEMANTICANALYZER_H

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "SymbolTable.h"
#include "Type.h"
#include "../parser/Ast.h"

// Maps each Expr variant pointer to its resolved Type.
// Key = expr.node.get() — stable because the AST is never mutated during analysis.
using ExprTypeMap = std::unordered_map<const Expr::Variant*, Type>;

// ---- ClassInfo: semantic information about a class ----

struct ClassInfo {
    struct Field {
        bool  isPublic;
        Type  type;
        int   index;    // field index in the struct (0-based, declaration order)
        Token decl;     // the field name token, for error reporting
    };
    struct Method {
        bool             isPublic;
        bool             isConstructor;
        bool             isDestructor;
        Type             returnType;
        std::vector<Type> paramTypes;
        Token            decl;     // method name token
    };
    std::vector<std::string>              fieldOrder;   // preserves declaration order
    std::unordered_map<std::string, Field>  fields;
    std::unordered_map<std::string, Method> methods;
    std::optional<Method>                   destructor; // at most one per class
};

struct SemanticResult {
    bool        hadError = false;
    ExprTypeMap typeMap;
    std::unordered_map<std::string, ClassInfo> classRegistry;
};

class SemanticAnalyzer {
public:
    SemanticResult analyze(const Program& program);  // resets all state per call

    // Access class registry (used by CodeGen)
    const std::unordered_map<std::string, ClassInfo>& getClassRegistry() const { return classRegistry_; }

private:
    SymbolTable         symbolTable;
    ExprTypeMap         typeMap;
    bool                hadError          = false;
    std::optional<Type> currentReturnType; // nullopt = top-level (not inside a function)
    int                 loopDepth         = 0;  // > 0 while inside a while/for loop
    std::string         currentClassName_;       // set while analysing a class body
    std::unordered_map<std::string, ClassInfo> classRegistry_;

    // Pass 0: collect class declarations (before collectFunctions)
    void collectClasses(const Program& program);

    // Pass 1: hoist top-level function signatures into the global scope
    void collectFunctions(const Program& program);

    // Statement analysis
    void analyzeStmt(const Stmt& stmt);
    void analyzeBlock(const BlockStmt& block);
    void analyzeIf(const IfStmt& ifStmt);
    void analyzeWhile(const WhileStmt& whileStmt);
    void analyzeFor(const ForStmt& forStmt);
    void analyzeReturn(const ReturnStmt& returnStmt);
    void analyzeBreak(const BreakStmt& breakStmt);
    void analyzeContinue(const ContinueStmt& continueStmt);
    void analyzeFunctionDecl(const FunctionDeclStmt& functionDecl);
    void analyzeExternFuncDecl(const ExternFuncDeclStmt& externDecl);
    void analyzeClassDecl(const ClassDeclStmt& classDecl);

    // Expression analysis — returns resolved Type and records it in typeMap.
    // Not [[nodiscard]] because it is intentionally called for side effects
    // in error-recovery paths (e.g. analysing arguments after a type error).
    Type analyzeExpr(const Expr& expr);
    [[nodiscard]] Type analyzeLiteral(const LiteralExpr& literal);
    [[nodiscard]] Type analyzeIdentifier(const IdentifierExpr& identifier);
    [[nodiscard]] Type analyzeUnary(const UnaryExpr& unary);
    [[nodiscard]] Type analyzeBinary(const BinaryExpr& binary);
    [[nodiscard]] Type analyzeAssign(const AssignExpr& assign);
    [[nodiscard]] Type analyzeCompoundAssign(const CompoundAssignExpr& compoundAssign);
    [[nodiscard]] Type analyzePostfix(const PostfixExpr& postfix);
    [[nodiscard]] Type analyzeCall(const CallExpr& call);
    [[nodiscard]] Type analyzeVarDecl(const VarDeclExpr& varDecl);
    [[nodiscard]] Type analyzeIndex(const IndexExpr& indexExpr);
    [[nodiscard]] Type analyzeIndexAssign(const IndexAssignExpr& indexAssign);
    [[nodiscard]] Type analyzeThis(const ThisExpr& thisExpr);
    [[nodiscard]] Type analyzeMemberAccess(const MemberAccessExpr& memberAccess);
    [[nodiscard]] Type analyzeMemberAssign(const MemberAssignExpr& memberAssign);
    [[nodiscard]] Type analyzeMethodCall(const MethodCallExpr& methodCall);

    // Helpers
    void          enterScope();
    void          exitScope();
    const Symbol* lookupSymbol(const Token& nameToken);  // emits error if missing
    void          error(const Token& token, const std::string& message);
    void          warn(const Token& token, const std::string& message);
    void          recordType(const Expr& expr, Type type);
    void          checkCast(Type from, Type to, const Token& site,
                            const std::string& context);
    // Resolve a type token — handles IDENTIFIER tokens that name a known class.
    [[nodiscard]] Type resolveTypeToken(const Token& typeToken) const;
};

#endif //GG_SEMANTICANALYZER_H
