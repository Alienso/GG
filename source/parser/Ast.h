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

// Store through a reference-valued expression: `<target> = <value>` where `target` is not a plain
// name/index/member but an expression that evaluates to a reference/borrow (e.g. a call returning
// `ref T` — `v.at(i) = x`). The value is stored into the referent. `op` is the '=' token (for
// diagnostics / line info).
struct RefStoreExpr {
    std::unique_ptr<Expr> target;
    Token                 op;
    std::unique_ptr<Expr> value;
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

// One arm of a `switch`. `labels` empty ⇒ the `default` arm. Exactly one of
// `valueExpr` (arrow `-> expr` form) or `block` (arrow `-> { ... }` form) is set.
// All members are pointers so the struct is usable while Expr/Stmt are incomplete
// (SwitchExpr is itself an Expr variant alternative). Move-only (Token const members).
struct SwitchArm {
    std::vector<std::unique_ptr<Expr>> labels;      // compared against the scrutinee
    bool                               isDefault = false;
    std::unique_ptr<Expr>              valueExpr;    // `-> expr ;` form (nullptr if block)
    std::unique_ptr<Stmt>              block;        // `-> { ... }` form (nullptr if expr)
    Token                              arrow;        // the '->' token, for diagnostics
};

// Switch expression: `switch (scrutinee) { arms }` producing a value (see SwitchArm).
struct SwitchExpr {
    Token                  keyword;     // the 'switch' token
    std::unique_ptr<Expr>  scrutinee;
    std::deque<SwitchArm>  arms;        // deque: SwitchArm is move-only (Token const members)
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
        RefStoreExpr,
        CastExpr,
        NewExpr,
        SizeofExpr,
        SwitchExpr
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

// Switch statement: `switch (scrutinee) { arms }`. Arrow-form arms (SwitchArm),
// value discarded. `default` optional (no exhaustiveness requirement).
struct SwitchStmt {
    Token                 keyword;     // the 'switch' token
    Expr                  scrutinee;
    std::deque<SwitchArm> arms;
};

// `yield expr;` — produces the value of the enclosing switch-expression arm.
struct YieldStmt {
    Token keyword;
    Expr  value;
};

struct ParamDecl {
    Token typeName;
    Token name;
    bool  isMut = false;   // `mut` — reassignable inside the body; otherwise const
    // Default value (`i32 a = 0`) — used to fill omitted trailing arguments at call sites.
    // nullptr = no default. Defaults must form a contiguous trailing run (enforced in the parser)
    // and may not reference the function's own parameters (analyzed in the enclosing scope).
    std::unique_ptr<Expr> defaultValue;
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
        SwitchStmt,
        YieldStmt,
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

// A generic template surfaced for definition-time body checking against its bounds. `decl` is the
// template's original body re-parsed with its type-parameter names registered as types (no
// substitution); `typeParams`/`bounds` are parallel (bounds[i] = trait names on typeParams[i]).
// Only templates with at least one bounded parameter are surfaced. See
// SemanticAnalyzer::checkGenericBodies.
struct GenericTemplateDecl {
    Stmt                                  decl;        // FunctionDeclStmt or ClassDeclStmt
    std::vector<std::string>              typeParams;
    std::vector<std::vector<std::string>> bounds;
};

struct Program {
    std::vector<Stmt>                  declarations;
    std::vector<GenericBoundCheck>     genericBoundChecks;
    std::vector<GenericTemplateDecl>   genericTemplates;   // bounded templates, for body checking
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
