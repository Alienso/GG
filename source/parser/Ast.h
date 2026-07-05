//
// Created by Vladimir Arsenijevic on 31.5.2026.
//

#ifndef GG_AST_H
#define GG_AST_H

#include <deque>
#include <memory>
#include <vector>
#include <optional>
#include <variant>
#include "../lexer/Token.h"

// ---- Utility: overloaded helper for std::visit ----

template<typename... Ts>
struct overloaded : Ts... { using Ts::operator()...; };
template<typename... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

// ---- Forward declarations (needed for recursive unique_ptr members) ----

struct Expr;
struct Stmt;

// ============================================================
// Expression nodes
// ============================================================

struct LiteralExpr {
    Token token;           // NUMBER, STRING, CHAR, TRUE, FALSE
};

struct IdentifierExpr {
    Token name;
};

struct UnaryExpr {
    Token operatorToken;   // BANG, MINUS, TILDE, INCREMENT, DECREMENT (prefix)
    std::unique_ptr<Expr> operand;
};

struct BinaryExpr {
    std::unique_ptr<Expr> left;
    Token operatorToken;
    std::unique_ptr<Expr> right;
};

struct AssignExpr {
    Token name;
    std::unique_ptr<Expr> value;
};

struct CompoundAssignExpr {
    Token name;
    Token operatorToken;   // PLUS_EQUAL, MINUS_EQUAL, STAR_EQUAL, etc.
    std::unique_ptr<Expr> value;
};

struct PostfixExpr {
    std::unique_ptr<Expr> operand;
    Token operatorToken;   // INCREMENT, DECREMENT
};

struct CallExpr {
    Token callee;
    std::vector<std::unique_ptr<Expr>> args;
};

struct VarDeclExpr {
    Token typeName;
    Token name;
    std::unique_ptr<Expr> initializer;   // nullptr if absent
    size_t arraySize = 0;                // 0 = scalar; N > 0 = fixed-size array of N elements
    bool   isStatic  = false;            // C-style static local: single persistent global
    bool   isMut     = false;            // `mut` — reassignable; otherwise const (single-assignment)
};

struct IndexExpr {
    std::unique_ptr<Expr> object;  // indexed expression (array variable, ptr<T>, this.field, …)
    std::unique_ptr<Expr> index;   // subscript expression
};

struct IndexAssignExpr {
    std::unique_ptr<Expr> object;  // indexed expression
    std::unique_ptr<Expr> index;   // subscript expression
    std::unique_ptr<Expr> value;   // right-hand side value
};

struct ThisExpr {
    Token keyword;   // the 'this' token
};

struct MemberAccessExpr {
    std::unique_ptr<Expr> object;
    Token                 field;
};

struct MemberAssignExpr {
    std::unique_ptr<Expr> object;
    Token                 field;
    std::unique_ptr<Expr> value;
};

struct MethodCallExpr {
    std::unique_ptr<Expr>              object;
    Token                              method;
    std::vector<std::unique_ptr<Expr>> args;
};

struct CastExpr {
    std::unique_ptr<Expr> operand;
    Token                 targetType;   // type keyword token (i32, f32, ptr, …)
    bool                  isMut = false; // `expr as mut T` — casts to a mutable reference view
};

// Heap allocation operator: `new ClassName(args)` — allocates a refcounted
// heap instance and runs its constructor. Evaluates to a reference (Class&).
struct NewExpr {
    Token                              keyword;    // the 'new' token
    Token                              className;  // class being allocated
    std::vector<std::unique_ptr<Expr>> args;       // constructor arguments
};

// `sizeof(T)` — the size in bytes of a type. Evaluates to u64.
struct SizeofExpr {
    Token keyword;    // the 'sizeof' token
    Token typeName;   // the (possibly synthesized) type token
};

// ---- Expr wrapper ----

struct Expr {
    using Variant = std::variant<
        LiteralExpr,
        IdentifierExpr,
        UnaryExpr,
        BinaryExpr,
        AssignExpr,
        CompoundAssignExpr,
        PostfixExpr,
        CallExpr,
        VarDeclExpr,
        IndexExpr,
        IndexAssignExpr,
        ThisExpr,
        MemberAccessExpr,
        MemberAssignExpr,
        MethodCallExpr,
        CastExpr,
        NewExpr,
        SizeofExpr
    >;
    std::unique_ptr<Variant> node;
};

// ============================================================
// Statement nodes
// ============================================================

struct ExprStmt {
    Expr expression;
};

struct BlockStmt {
    std::vector<std::unique_ptr<Stmt>> body;
};

struct IfStmt {
    Expr condition;
    std::unique_ptr<Stmt> thenBranch;
    std::unique_ptr<Stmt> elseBranch;    // nullptr if no else
};

struct WhileStmt {
    Expr condition;
    std::unique_ptr<Stmt> body;
};

struct ForStmt {
    std::unique_ptr<Stmt> init;          // nullptr if absent (ExprStmt wrapping VarDeclExpr or plain expr)
    std::optional<Expr>   condition;
    std::optional<Expr>   increment;
    std::unique_ptr<Stmt> body;
};

struct BreakStmt {
    Token keyword;
};

struct ContinueStmt {
    Token keyword;
};

struct ReturnStmt {
    Token keyword;
    std::optional<Expr> value;
};

struct ParamDecl {
    Token typeName;
    Token name;
    bool  isMut = false;   // `mut` — reassignable inside the body; otherwise const
};

struct FunctionDeclStmt {
    Token returnType;
    Token name;
    std::vector<ParamDecl> params;
    BlockStmt body;
    // Return slot (sret / named-return-value): `name(params) -> RetType slot { }`.
    // When set, the object result is written in place into a caller-provided slot;
    // `returnType` holds the object type and `returnSlotName` the slot binding name.
    bool        hasReturnSlot = false;
    std::string returnSlotName;
};

struct ExternFuncDeclStmt {
    Token                  keyword;     // 'extern' token — used for error reporting
    Token                  returnType;
    Token                  name;
    std::vector<ParamDecl> params;
};

struct ImportStmt {
    Token keyword;   // 'import' token — used for error reporting
    Token path;      // STRING token — the file path (lexeme is the raw content, quotes stripped by lexer)
};

struct FieldDecl {
    bool  isPublic  = false;
    bool  isStatic  = false;   // `static T name;` — class-level storage, not per-instance
    bool  isMut     = false;   // `mut` — reassignable after construction; otherwise const
    Token typeName;   // type keyword token
    Token name;
    // Constant initializer for a static field (`static i32 count = 0;`), run in a
    // pre-main initializer. nullptr for instance fields and uninitialised statics.
    std::unique_ptr<Expr> initializer;
};

struct MethodDecl {
    bool                   isPublic      = false;
    bool                   isConstructor = false;  // true when name == class name
    bool                   isDestructor  = false;   // true for ~ClassName() — no params, no return type
    bool                   isStatic      = false;   // true for `static T method(...)` — no implicit `this`
    bool                   isMut         = false;   // true for `T method(...) mut` — may mutate `this`
    bool                   hasBody       = true;    // false for a trait's required (bodyless) method
    Token                  returnType;     // for constructors/destructors: class-name token
    Token                  name;
    std::vector<ParamDecl> params;
    BlockStmt              body;
    // Return slot (sret): `method(params) -> RetType slot { }`. See FunctionDeclStmt.
    bool                   hasReturnSlot = false;
    std::string            returnSlotName;
};

struct ClassDeclStmt {
    Token                  name;
    // std::deque (not vector) because FieldDecl now owns a unique_ptr initializer
    // and Token has const members — making it move-only and not move-assignable.
    std::deque<FieldDecl>  fields;
    // std::deque avoids moving existing elements on growth, which is needed
    // because MethodDecl contains BlockStmt (with unique_ptr) and Token
    // (const string members) — making it neither copyable nor noexcept-moveable.
    std::deque<MethodDecl> methods;
};

// A single enum variant: the name plus the constructor arguments used to
// initialise its singleton instance (e.g. EARTH(5.976e24, 6.37814e6)).
struct EnumVariant {
    Token                              name;
    std::vector<std::unique_ptr<Expr>> args;   // empty for fieldless variants
};

struct EnumDeclStmt {
    Token                    name;
    std::deque<EnumVariant>  variants;   // declaration order = ordinal order
    std::deque<FieldDecl>    fields;
    // std::deque for the same reason as ClassDeclStmt::methods.
    std::deque<MethodDecl>   methods;
};

// ---- Stmt wrapper ----

// A trait declaration: a named contract of method signatures (required = no body) and/or
// default methods (with a body). `Self` in signatures denotes the implementing type.
struct TraitDeclStmt {
    Token                  name;
    std::deque<MethodDecl> methods;
};

// An `impl Trait for Type { ... }` block: the methods become methods on `typeName`.
struct ImplDeclStmt {
    Token                  traitName;
    Token                  typeName;
    std::deque<MethodDecl> methods;
};

struct Stmt {
    using Variant = std::variant<
        ExprStmt,
        BlockStmt,
        IfStmt,
        WhileStmt,
        ForStmt,
        BreakStmt,
        ContinueStmt,
        ReturnStmt,
        FunctionDeclStmt,
        ExternFuncDeclStmt,
        ImportStmt,
        ClassDeclStmt,
        EnumDeclStmt,
        TraitDeclStmt,
        ImplDeclStmt
    >;
    std::unique_ptr<Variant> node;
};

// ---- Program root ----

// A generic trait-bound obligation recorded when a template is monomorphized:
// the concrete type argument `typeName` must implement trait `traitName`. Verified
// by the semantic analyzer (static dispatch — see SemanticAnalyzer::checkGenericBounds).
struct GenericBoundCheck {
    std::string typeName;    // concrete type argument (e.g. "Point", "i32", "Vec$i32")
    std::string traitName;   // required trait (user trait or built-in operator trait)
    std::string context;     // e.g. "maxOf<Point>" — for the error message
    int         line = 0;    // use-site line for diagnostics
};

struct Program {
    std::vector<Stmt>              declarations;
    std::vector<GenericBoundCheck> genericBoundChecks;
};

// ============================================================
// Factory helpers
// ============================================================

template<typename T>
Expr makeExpr(T&& node) {
    return Expr{ std::make_unique<Expr::Variant>(std::forward<T>(node)) };
}

template<typename T>
Stmt makeStmt(T&& node) {
    return Stmt{ std::make_unique<Stmt::Variant>(std::forward<T>(node)) };
}

// Box helpers: move an Expr/Stmt into a unique_ptr (for recursive node fields)
inline std::unique_ptr<Expr> box(Expr expression) { return std::make_unique<Expr>(std::move(expression)); }
inline std::unique_ptr<Stmt> box(Stmt statement)  { return std::make_unique<Stmt>(std::move(statement)); }

#endif //GG_AST_H
