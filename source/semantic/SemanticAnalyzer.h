//
// Created by Vladimir Arsenijevic on 01.6.2026.
//

#ifndef GG_SEMANTICANALYZER_H
#define GG_SEMANTICANALYZER_H

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "SymbolTable.h"
#include "Type.h"
#include "../parser/Ast.h"
#include "../CompileError.h"
#include "../CompilerOptions.h"

// Maps each Expr variant pointer to its resolved Type.
// Key = expr.node.get() — stable because the AST is never mutated during analysis.
using ExprTypeMap = std::unordered_map<const Expr::Variant*, Type>;

// First (leftmost) token of an expression — for error reporting on nodes that
// store no keyword token of their own. Defined in SemanticAnalyzer_Stmts.cpp.
const Token& exprFirstToken(const Expr& expr);

// ---- ClassInfo: semantic information about a class ----

struct ClassInfo {
    struct Field {
        bool  isPublic = false;
        bool  isMut    = false; // `mut` — writable after construction; otherwise const
        Type  type;
        int   index    = 0;  // field index in the struct (0-based, declaration order)
        Token decl;          // the field name token, for error reporting
    };
    struct Method {
        bool             isPublic  = false;
        bool             isStatic  = false;   // class-level method (no implicit `this`)
        bool             isMut     = false;   // `T m(...) mut` — may mutate `this`
        Type             returnType;
        std::vector<Type> paramTypes;
        std::vector<bool> paramMut;           // per-parameter `mut` flag
        Token            decl;     // method name token
    };
    // A static (class-level) field: shared storage, not part of the struct layout.
    struct StaticField {
        bool  isPublic = false;
        Type  type;
        Token decl;          // field name token
        bool  hasInit  = false;  // true if it carries a constant initializer
    };
    std::vector<std::string>              fieldOrder;   // preserves declaration order
    std::unordered_map<std::string, Field>  fields;
    std::unordered_map<std::string, StaticField> staticFields;
    std::unordered_map<std::string, Method> methods;
    std::optional<Method>                   destructor; // at most one per class
};

// ---- EnumInfo: semantic information about a Java-style enum ----
// An enum reuses ClassInfo (stored in classRegistry) for its fields, constructor
// and methods. EnumInfo tracks the variant list and marks the name as an enum so
// that type resolution yields TypeKind::Enum and direct construction is rejected.
struct EnumInfo {
    std::vector<std::string>        variantOrder;  // declaration / ordinal order
    std::unordered_set<std::string> variantSet;    // membership test
    Token                           decl;          // enum name token
};

struct SemanticResult {
    bool        hadError = false;
    ExprTypeMap typeMap;
    std::unordered_map<std::string, ClassInfo> classRegistry;
    std::unordered_map<std::string, EnumInfo>  enumRegistry;
};

class SemanticAnalyzer {
public:
    SemanticResult analyze(const Program& program,
                           const std::string& filename = "",
                           const CompilerOptions& options = {});  // resets all state per call

private:
    SymbolTable         symbolTable;
    ExprTypeMap         typeMap;
    bool                hadError          = false;
    std::string         filename;                // source filename for error messages
    std::optional<Type> currentReturnType; // nullopt = top-level (not inside a function)
    int                 loopDepth         = 0;  // > 0 while inside a while/for loop
    std::string         currentClassName;       // set while analysing a class body
    bool                currentClassIsEnum = false; // true while analysing an enum body
    bool                currentMethodIsStatic = false; // true while analysing a static method body
    bool                inEnumConstructor  = false; // true while analysing an enum's constructor body
    bool                inConstructor      = false; // true while analysing a class's constructor body
    bool                currentThisMutable = false; // true while analysing a `mut` method / ctor / dtor
    std::unordered_map<std::string, ClassInfo> classRegistry;
    std::unordered_map<std::string, EnumInfo>  enumRegistry;
    bool                allowRawPtr_      = false; // set from CompilerOptions each call

    // Pass 0: collect class declarations (before collectFunctions)
    void collectClasses(const Program& program);
    // Build the shared ClassInfo (fields + methods + optional destructor) for a
    // class or enum body. allowDestructor is false for enums.
    [[nodiscard]] ClassInfo buildClassInfo(const std::string& ownerName,
                                           const std::deque<FieldDecl>& fields,
                                           const std::deque<MethodDecl>& methods,
                                           bool allowDestructor);

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
    void analyzeEnumDecl(const EnumDeclStmt& enumDecl);

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
    // Effective mutability of a parameter (`mut` flag); see the .cpp for the borrow rules.
    [[nodiscard]] bool paramIsMutable(const ParamDecl& param, const Type& resolvedType);
    // Implicit-`this` member resolution: a bare name (not shadowed by a local/param/function)
    // may refer to a member of the enclosing class. Returns nullptr when not applicable.
    [[nodiscard]] const ClassInfo::Field* currentInstanceField(const std::string& name) const;
    [[nodiscard]] const Type*             currentStaticFieldType(const std::string& name) const;
    [[nodiscard]] const ClassInfo::Method* currentClassMethod(const std::string& name) const;
    // Validate a `++`/`--` target (local, or implicit-`this` field). Emits an error and
    // returns false if the target is immutable; returns true otherwise.
    bool incDecTargetOk(const Token& op, const std::string& name);
    // True if `expr` denotes a mutable place — a `mut` binding, `this`, a freshly-owned
    // reference (`new`/call result), or a `mut`-field access chain whose root is mutable.
    // Used for transitive const (field-write receiver) and the const→mut cast warning.
    [[nodiscard]] bool exprIsMutablePlace(const Expr& expr);
    // Emit the const→mut coercion warning when a read-only reference `source` flows into a
    // `mut` reference target. No-op for non-reference targets, mutable sources, or an
    // explicit cast (`as mut T`), which is the sanctioned way to silence it.
    void warnConstToMut(const Token& at, const Expr& source, const Type& targetType);
    // True if `expr` is a compile-time constant (literal, or unary/binary/cast of
    // constants). Used to validate static-local initializers, which run pre-main.
    [[nodiscard]] static bool isConstantExpr(const Expr& expr);
    [[nodiscard]] Type analyzeIndex(const IndexExpr& indexExpr);
    [[nodiscard]] Type analyzeIndexAssign(const IndexAssignExpr& indexAssign);
    [[nodiscard]] Type analyzeThis(const ThisExpr& thisExpr);
    [[nodiscard]] Type analyzeMemberAccess(const MemberAccessExpr& memberAccess);
    [[nodiscard]] Type analyzeMemberAssign(const MemberAssignExpr& memberAssign);
    [[nodiscard]] Type analyzeMethodCall(const MethodCallExpr& methodCall);
    [[nodiscard]] Type analyzeCast(const CastExpr& castExpr);
    [[nodiscard]] Type analyzeNew(const NewExpr& newExpr);

    // Helpers
    // Emit an error if typeToken resolves to ptr/ptr<T> and --unsafe-ptr was not given.
    // Exempt from the check: extern declarations (CRT bindings always need ptr).
    void          checkRawPtrAllowed(const Token& typeToken, const Token& site);

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
                         const std::string& context,
                         const std::vector<bool>& paramMut = {});
    // Emit a compile-time out-of-bounds error if `indexExpr` is a constant literal outside [0, arraySize).
    void checkConstantIndexBounds(const Expr& indexExpr, size_t arraySize);
};

#endif //GG_SEMANTICANALYZER_H
