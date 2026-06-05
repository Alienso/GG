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
#include "../CompileError.h"

// Maps each Expr variant pointer to its resolved Type.
// Key = expr.node.get() — stable because the AST is never mutated during analysis.
using ExprTypeMap = std::unordered_map<const Expr::Variant*, Type>;

// ---- ClassInfo: semantic information about a class ----

struct ClassInfo {
    struct Field {
        bool  isPublic = false;
        Type  type;
        int   index    = 0;  // field index in the struct (0-based, declaration order)
        Token decl;          // the field name token, for error reporting
    };
    struct Method {
        bool             isPublic  = false;
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
    SemanticResult analyze(const Program& program, const std::string& filename = "");  // resets all state per call

private:
    SymbolTable         symbolTable;
    ExprTypeMap         typeMap;
    bool                hadError          = false;
    std::string         filename;                // source filename for error messages
    std::optional<Type> currentReturnType; // nullopt = top-level (not inside a function)
    int                 loopDepth         = 0;  // > 0 while inside a while/for loop
    std::string         currentClassName;       // set while analysing a class body
    std::unordered_map<std::string, ClassInfo> classRegistry;

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
    [[nodiscard]] Type analyzeCast(const CastExpr& castExpr);

    // Helpers
    void          enterScope();
    void          exitScope();
    const Symbol* lookupSymbol(const Token& nameToken);  // emits error if missing
    void          error(const Token& token, const std::string& message);
    void          warn(const Token& token, const std::string& message);
    void          recordType(const Expr& expr, const Type& type);
    void          checkCast(const Type& from, const Type& to, const Token& site,
                            const std::string& context);
    // Resolve a type token — handles IDENTIFIER tokens that name a known class.
    [[nodiscard]] Type resolveTypeToken(const Token& typeToken) const;

    // Resolve an Object type to its ClassInfo; emits error and returns nullptr if not an Object or class not found.
    [[nodiscard]] const ClassInfo* lookupObjectClass(Type objectType, const Token& site);
    // Type-check and analyse a call's arguments against declared param types.
    void analyzeCallArgs(const std::vector<std::unique_ptr<Expr>>& args,
                         const std::vector<Type>& paramTypes,
                         const Token& callee,
                         const std::string& context);
    // Emit a compile-time out-of-bounds error if `indexExpr` is a constant literal outside [0, arraySize).
    void checkConstantIndexBounds(const Expr& indexExpr, size_t arraySize);
};

#endif //GG_SEMANTICANALYZER_H
