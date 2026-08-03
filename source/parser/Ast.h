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
    // Parallel to `args`: the parameter name for a named argument (`f(x: 1)`), or an empty-lexeme
    // token for a positional one. Empty vector ⇒ all positional (the common case). Named args must
    // follow positional ones. Reordered to parameter order in the semantic pass.
    std::vector<Token> argNames{};   // default-init: omitting it in aggregate init is intentional
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
    bool                  safe = false;   // `x?.field` — null-propagating access on a nullable receiver
};

// `null` — the null literal (typed TypeKind::Null; assignable only where a `T?` is expected).
struct NullLiteralExpr {
    Token keyword;   // the 'null' token
};

// `x!!` — non-null assertion. Unwraps a `T?` to `T`, aborting the program if it is null.
struct UnwrapExpr {
    std::unique_ptr<Expr> operand;
    Token                 op;   // the '!!' token
};

// `a ?: b` — Elvis: evaluates to `a` when non-null, else `b`. Result is the non-null form.
struct ElvisExpr {
    std::unique_ptr<Expr> left;
    Token                 op;   // the '?:' token
    std::unique_ptr<Expr> right;
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
    std::vector<Token>                 argNames;   // parallel to args; see CallExpr::argNames
    bool                               safe = false;   // `x?.m(...)` — null-propagating call
};

// An untyped brace initializer `{ args }` whose class is deduced from the expected type at the use
// site (a constructor argument, a var initializer, or a return). `Point{args}` (typed) is parsed
// as an ordinary constructor CallExpr instead — this node is only the type-elided form, e.g. the
// inner `{0,0}` in `Line l{ {0,0}, {1,1} }`.
struct BraceInitExpr {
    std::vector<std::unique_ptr<Expr>> args;
    Token                              brace;   // the '{' token, for diagnostics
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
    std::vector<Token>                 argNames;   // parallel to args; see CallExpr::argNames
};

// `sizeof(T)` — the size in bytes of a type. Evaluates to u64.
struct SizeofExpr {
    Token keyword;    // the 'sizeof' token
    Token typeName;   // the (possibly synthesized) type token
};

// `destroy(place)` — run the destructor on an object/`ptr<T>`-element lvalue in place (no free).
// Unsafe container primitive (requires --unsafe-ptr); a no-op for primitive / no-dtor places.
// Evaluates to void.
struct DestroyExpr {
    Token keyword;                 // the 'destroy' token
    std::unique_ptr<Expr> place;   // the object lvalue to destroy
};

// Compile-time reflection builtin (`@…`). TRANSIENT: folded/lowered away by the parser's
// reflection-expansion pass, so it never reaches semantic analysis or codegen.
//   @typeName(T)      -> string literal
//   @fieldCount(T)    -> integer literal (u64)
//   @hasField(T,"x")  -> bool literal
//   @field(v, name)   -> ordinary MemberAccessExpr / MemberAssignExpr (an lvalue)
//   @compileError(m)  -> emits a diagnostic and aborts compilation
enum class ReflectKind {
    TypeName, FieldCount, HasField, Field, CompileError,
    // Phase 2 scalar queries (fold to constants like the above):
    VariantCount, AlignOf, OffsetOf, Implements,
    IsInteger, IsFloat, IsClass, IsEnum, IsPrimitive,
    // Annotations: `@hasAnnotation(T, Ann)` → bool (does the type or any member carry `Ann`).
    HasAnnotation
};
struct ReflectExpr {
    Token                              at;         // the '@' token, for diagnostics
    ReflectKind                        kind;
    std::vector<Token>                 typeArgs;   // type-token args (e.g. T in @fields(T))
    std::vector<std::unique_ptr<Expr>> valueArgs;  // value args (e.g. v, "x", or f.name)
};

// ---- Patterns (for `match`) ----
// A destructuring pattern. Recursive (tuple/struct patterns hold sub-patterns), so sub-patterns are
// held as `unique_ptr<Pattern>` to break the type-completeness cycle (mirrors how Expr nests via
// `unique_ptr<Expr>`). Lowers to a test-tree + bindings — never a runtime type tag.
struct Pattern;
struct WildcardPat { Token token; };                       // `_` — matches anything, binds nothing
struct BindingPat  { Token name;  };                       // `x` — matches anything, binds x
struct LiteralPat  { std::unique_ptr<Expr> literal; };     // `0`, `"s"`, … — compared via ==
struct TuplePat    { std::deque<std::unique_ptr<Pattern>> elems; Token paren; };   // `(p0, p1, …)`
struct StructPat {                                         // `Class{ field: subpat, field, … }`
    Token typeName;
    std::deque<std::pair<Token, std::unique_ptr<Pattern>>> fields;   // (field name, sub-pattern)
    Token brace;
};
struct Pattern {
    using Variant = std::variant<WildcardPat, BindingPat, LiteralPat, TuplePat, StructPat>;
    std::unique_ptr<Variant> node;
};

// One arm of a `match`: a pattern plus a body. Exactly one of `valueExpr` (`-> expr` form) or
// `block` (`-> { ... }` form) is set. Move-only (Pattern + unique_ptrs + Token const members).
struct MatchArm {
    Pattern               pattern;
    std::unique_ptr<Expr> valueExpr;   // `-> expr ;` form (nullptr if block)
    std::unique_ptr<Stmt> block;       // `-> { ... }` form (nullptr if expr)
    Token                 arrow;       // the '->' token, for diagnostics
};

// A pattern that matches every value of its type (a wildcard/binding, or a tuple/struct whose
// sub-patterns are all irrefutable). Drives match-expression exhaustiveness + arm reachability.
inline bool patternIsIrrefutable(const Pattern& pattern) {
    return std::visit(overloaded{
        [](const WildcardPat&) { return true; },
        [](const BindingPat&)  { return true; },
        [](const LiteralPat&)  { return false; },
        [](const TuplePat& t)  {
            for (const auto& e : t.elems) if (!patternIsIrrefutable(*e)) return false;
            return true;
        },
        [](const StructPat& s) {
            for (const auto& fp : s.fields) if (!patternIsIrrefutable(*fp.second)) return false;
            return true;
        },
    }, *pattern.node);
}

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

// Match expression: `match (scrutinee) { pattern -> value ; … }` producing a value. Like SwitchExpr
// but arms carry destructuring patterns (MatchArm) instead of equality labels.
struct MatchExpr {
    Token                 keyword;      // the 'match' token
    std::unique_ptr<Expr> scrutinee;
    std::deque<MatchArm>  arms;
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
        BraceInitExpr,
        NullLiteralExpr,
        UnwrapExpr,
        ElvisExpr,
        CastExpr,
        NewExpr,
        SizeofExpr,
        DestroyExpr,
        ReflectExpr,
        SwitchExpr,
        MatchExpr
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

// Compile-time reflection unroll: `inline for (v in @fields(T)) { body }`. TRANSIENT — the parser's
// reflection-expansion pass replaces it (per field, once each) with ordinary statements, so it
// never reaches semantic analysis or codegen. The body is kept as a raw token slice (not parsed)
// so it can be re-parsed once per field with the loop binding substituted.
struct InlineForStmt {
    Token              keyword;      // the 'inline' token
    Token              loopVar;      // the binding name (e.g. `f` / `v`)
    Token              targetType;   // the type token inside @fields(T) / @variants(E)
    std::vector<Token> bodyTokens;   // the `{ … }` body, captured verbatim (braces included)
    bool               overVariants = false;  // false: @fields(T); true: @variants(E)
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

// Match statement: `match (scrutinee) { pattern -> … }`. Arrow-form arms carrying destructuring
// patterns (MatchArm); value discarded. Never required exhaustive (like a switch statement).
struct MatchStmt {
    Token                keyword;      // the 'match' token
    Expr                 scrutinee;
    std::deque<MatchArm> arms;
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
    bool  isVariadic = false;   // `Ts... args` — a variadic pack param (must be last; monomorphizes to a tuple)
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
    // `fn private name(...)` — public by default. A private free function is file-local:
    // calling it from a different source file is a warning (not an error), like a private
    // field accessed outside its class. `sourceFile` is the canonical path of the declaring
    // file, used to detect the cross-file boundary during analysis.
    bool        isPublic = true;
    std::string sourceFile;
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

// One applied annotation: `@Name(arg, …)` prefixed on a declaration. Compile-time metadata only —
// no runtime representation. `args` are compile-time-constant expressions (validated in semantics);
// empty for a marker (`@Skip`). Move-only (owns the arg Exprs).
struct AnnotationApp {
    Token                              name;   // the annotation type name (`Rename`)
    std::vector<std::unique_ptr<Expr>> args;   // positional const args (`"user_name"`) — for semantics
    // Raw source tokens of each positional arg, captured at parse time. `f.get(Ann).field` splices
    // the tokens of the arg at that field's position back into the expanded code (no Expr→token
    // reconstruction needed). Parallel to `args`.
    std::vector<std::vector<Token>>    argTokens{};
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
    std::deque<AnnotationApp> annotations{};   // `@Name(...)` prefixes (compile-time reflection)
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
    std::string            returnSlotName{};   // default-init: omitting it in aggregate init is fine
    std::deque<AnnotationApp> annotations{};  // `@Name(...)` prefixes
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
    std::deque<AnnotationApp> annotations{};  // `@Name(...)` prefixes on the class
};

// A compile-time annotation type: `annotation Name { <const fields> }`. Purely reflective — it
// emits NO IR (no struct, no ctor); it is collected into the annotation registry and read only by
// `@`-position applications + `f.has`/`f.get`/`@hasAnnotation`. Fields carry the annotation's data.
struct AnnotationDeclStmt {
    Token                 name;
    std::deque<FieldDecl> fields;   // e.g. `str key;` — const-typed, no methods
};

// A single enum variant: the name plus the constructor arguments used to
// initialise its singleton instance (e.g. EARTH(5.976e24, 6.37814e6)).
struct EnumVariant {
    Token                              name;
    std::vector<std::unique_ptr<Expr>> args;   // empty for fieldless variants
    std::deque<AnnotationApp>         annotations{};  // `@Name(...)` prefixes on the variant
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
        InlineForStmt,
        BreakStmt,
        ContinueStmt,
        ReturnStmt,
        SwitchStmt,
        MatchStmt,
        YieldStmt,
        FunctionDeclStmt,
        ExternFuncDeclStmt,
        ImportStmt,
        ClassDeclStmt,
        EnumDeclStmt,
        TraitDeclStmt,
        ImplDeclStmt,
        AnnotationDeclStmt
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
