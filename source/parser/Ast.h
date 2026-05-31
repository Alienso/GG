//
// Created by Vladimir Arsenijevic on 31.5.2026.
//

#ifndef GG_AST_H
#define GG_AST_H

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
    Token op;              // BANG, MINUS, TILDE, INCREMENT, DECREMENT (prefix)
    std::unique_ptr<Expr> operand;
};

struct BinaryExpr {
    std::unique_ptr<Expr> left;
    Token op;
    std::unique_ptr<Expr> right;
};

struct AssignExpr {
    Token name;
    std::unique_ptr<Expr> value;
};

struct CompoundAssignExpr {
    Token name;
    Token op;              // PLUS_EQUAL, MINUS_EQUAL, STAR_EQUAL, etc.
    std::unique_ptr<Expr> value;
};

struct PostfixExpr {
    std::unique_ptr<Expr> operand;
    Token op;              // INCREMENT, DECREMENT
};

struct CallExpr {
    Token callee;
    std::vector<std::unique_ptr<Expr>> args;
};

struct VarDeclExpr {
    Token typeName;
    Token name;
    std::unique_ptr<Expr> initializer;   // nullptr if absent
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
        VarDeclExpr
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

struct ReturnStmt {
    Token keyword;
    std::optional<Expr> value;
};

struct ParamDecl {
    Token typeName;
    Token name;
};

struct FunctionDeclStmt {
    Token returnType;
    Token name;
    std::vector<ParamDecl> params;
    BlockStmt body;
};

// ---- Stmt wrapper ----

struct Stmt {
    using Variant = std::variant<
        ExprStmt,
        BlockStmt,
        IfStmt,
        WhileStmt,
        ForStmt,
        ReturnStmt,
        FunctionDeclStmt
    >;
    std::unique_ptr<Variant> node;
};

// ---- Program root ----

struct Program {
    std::vector<Stmt> declarations;
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
inline std::unique_ptr<Expr> box(Expr e) { return std::make_unique<Expr>(std::move(e)); }
inline std::unique_ptr<Stmt> box(Stmt s) { return std::make_unique<Stmt>(std::move(s)); }

#endif //GG_AST_H
