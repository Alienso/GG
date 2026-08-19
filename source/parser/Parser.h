//
// Created by Vladimir Arsenijevic on 28.5.2026.
//

#ifndef GG_PARSER_H
#define GG_PARSER_H

#include "Ast.h"
#include "../lexer/Token.h"
#include "../semantic/Type.h"
#include "../CompileError.h"

#include <vector>
#include <stdexcept>
#include <initializer_list>
#include <unordered_set>
#include <unordered_map>
#include <string>

class ParseError : public CompileError {
public:
    explicit ParseError(const std::string& msg) : CompileError(msg) {}
};

// ---- Generics registry (shared across files for cross-file generics) ----
// A generic declaration is captured as raw tokens; each use site records an
// instantiation request. After all files are parsed, the worklist substitutes
// the type arguments and re-parses each request into a concrete declaration,
// so semantic analysis and codegen only ever see ordinary (mangled) decls.
struct GenericTemplate {
    std::vector<std::string>              typeParams;
    std::vector<std::vector<std::string>> bounds;   // bounds[i] = trait names required of typeParams[i]
    std::vector<bool>                     isPack;    // isPack[i] = typeParams[i] is a variadic pack `...T`
    std::vector<Token>                    tokens;    // decl tokens; tokens[1] is the name; <...> stripped
};
struct GenericInstantiation {
    std::string                     templateName;
    std::string                     mangledName;
    std::vector<std::vector<Token>> args;     // each type argument's token slice
    // Generic METHOD instantiation: when non-empty, this request is a `fn m<T>` on a class, not a
    // free-function/class template. `ownerClass` is the concrete (possibly mangled) class the
    // instantiated method must be injected into; `bareMethodName` is the source method name (so the
    // template key is `ownerClass + "::" + bareMethodName` and `mangledName` is e.g. `m$i32`).
    std::string                     ownerClass;
    std::string                     bareMethodName;
};
// A generic `impl<T…> Trait for Class<T…> { … }`. Captured like a class template and
// instantiated automatically whenever `Class<args>` is instantiated: the impl's type
// parameters are substituted with the class's concrete arguments and the body re-parses
// as an ordinary concrete impl (so semantics/codegen never see the type parameters).
struct GenericImplTemplate {
    std::vector<std::string>              typeParams;        // from `impl<…>`
    std::vector<std::vector<std::string>> bounds;            // parallel to typeParams (reserved)
    std::string                           targetClass;       // e.g. "Array"
    std::vector<std::string>              targetParamAtPos;  // target arg position → impl type-param name
    std::vector<Token>                    tokens;            // `impl <Trait> for <Class<…>> { … }` (header `<…>` stripped)
    std::string                           traitName;         // the trait this blanket impl provides (for specialization skip)
};
struct GenericRegistry {
    std::unordered_map<std::string, GenericTemplate> templates;     // by template name (fn or class)
    std::unordered_set<std::string>                  funcNames;     // generic function names
    std::unordered_set<std::string>                  classNames;    // generic class names
    // Top-level free functions with at least one ORDINARY (non-generic) declaration somewhere in the
    // program — a bare name may be BOTH a generic template (in funcNames) AND have plain overloads
    // under GG's overload model (e.g. a stdlib module declaring `fn print(i32)` in one file and
    // `fn print<...Ts>(...)` in another, both loaded together). The call-site inference gate in
    // Parser_Expr.cpp consults this before hard-erroring on a failed generic-inference attempt, so a
    // call that doesn't fit the generic template's shape can still fall through to the ordinary
    // overload instead of being permanently captured by the generic path.
    std::unordered_set<std::string>                  ordinaryFuncNames;
    // ---- Generic methods (`fn m<T>` on a class) ----
    // A generic method is captured as a template keyed by `OwnerClass::method` (the owner is the
    // possibly-mangled class the method is declared in — e.g. `Box::wrap` or, during a generic
    // class's re-parse, `Array$i32::wrap`). At a call site the receiver's class is resolved at parse
    // time (parser-visible receivers only) and a method instantiation is queued; the concrete method
    // is re-parsed and injected into the owner class's `ClassDeclStmt::methods` so semantics/codegen
    // treat it as an ordinary method. `genericMethodNames`/`genericMethodKeys` gate the `obj.m<T>(…)`
    // call-site disambiguation (a bare name set for a cheap check, a class-qualified set for a precise
    // one), populated by the brace-aware prescan.
    std::unordered_map<std::string, GenericTemplate> methodTemplates;    // "Owner::method" → template
    std::unordered_set<std::string>                  genericMethodNames; // bare "wrap"
    std::unordered_set<std::string>                  genericMethodKeys;  // "Owner::wrap" (base owner name)
    // Variadic methods (`fn m<...Ts>(Ts... args)`) — a subset, called WITHOUT explicit `<…>` (the pack
    // is inferred from the trailing args, like a variadic free function). Keyed by the base (unmangled)
    // owner so a call `arr.emplaceBack(1,2)` on `Array$Point` is recognized before `Array$Point` exists;
    // `fixedCount` is the number of non-pack params (so the trailing args are split off as the pack).
    std::unordered_set<std::string>                  variadicMethodNames; // bare "emplaceBack"
    std::unordered_set<std::string>                  variadicMethodKeys;  // "Array::emplaceBack" (base owner)
    std::unordered_map<std::string, size_t>          variadicMethodFixedCount; // key → # fixed params
    std::vector<GenericInstantiation>                worklist;      // instantiation requests
    std::unordered_set<std::string>                  instantiated;  // mangled names already queued
    std::vector<GenericImplTemplate>                 implTemplates;    // generic `impl<T> … for Class<T>` blocks
    std::unordered_set<std::string>                  implInstantiated; // dedup key "<implIndex>@<mangledClass>"
    std::unordered_set<std::string>                  emittedCallTraits; // canonical Call$… traits emitted (shared dedup)
    std::unordered_set<std::string>                  lambdaClassNames;  // generated `__lambda_N` class names (shared, seeded at monomorphization)
    int                                              lambdaCounter = 0;    // unique `__lambda_N` names (shared)
    // Canonical Call$… trait name → its (parameter type tokens, return type token), so an untyped
    // lambda argument can infer its parameter/return types from the callee's `Call(…)` bound.
    std::unordered_map<std::string, std::pair<std::vector<Token>, Token>> callTraitSigs;

    // Tuple types: synthesized value-object class name (e.g. "Tuple$i32$str") → its ordered element
    // TYPE tokens. Recorded when a `(T1, T2, …)` tuple type is parsed; the concrete class (fields
    // `_0 … _n` + a positional constructor) is synthesized ONCE in runMonomorphization and appended
    // to the Program, so semantics/codegen treat a tuple as an ordinary value object (struct layout,
    // clone, dtor, structeq — no back-end changes). Shared across files; deduped by the mangled name.
    std::unordered_map<std::string, std::vector<Token>> tupleRequests;

    // ---- Module namespacing (shared across files) ----
    // module name → simple top-level decl names it declares, split by kind because they qualify
    // differently: a TYPE name (class/enum/trait/annotation) is qualified in every position, while a
    // FUNCTION name is qualified only in call/generic position (`(`/`<`) so a field/local named like
    // a free function is not mistakenly rewritten. `extern` and `main` are never recorded. Populated
    // per file in Parser::parse and, for a multi-file build, up-front by ImportResolver so a file can
    // qualify references to a sibling file's same-module symbols. "" is the global namespace.
    std::unordered_map<std::string, std::unordered_set<std::string>> moduleTypes;
    std::unordered_map<std::string, std::unordered_set<std::string>> moduleFuncs;
    std::unordered_set<std::string>                                  moduleNames;  // every declared module name
};

class Parser {
public:
    // Pre-register class names from imported files so that cross-file constructor
    // calls (e.g. "String s(...)") are recognised as VarDecl. A shared GenericRegistry
    // (when provided) lets generics span files; otherwise an internal one is used.
    explicit Parser(std::unordered_set<std::string> initialClassNames = {},
                    GenericRegistry* sharedRegistry = nullptr);
    // runMonomorphization=false defers expansion (the caller invokes monomorphize()
    // once, after every file has been parsed into the shared registry).
    [[nodiscard]] Program parse(const std::vector<Token>& inputTokens,
                                const std::string& filename = "",
                                bool runMonomorphization = true);

    // Pre-register generic template names from a token stream so that use sites
    // are recognised regardless of which file declares the template.
    void prescanTemplateNames(const std::vector<Token>& inputTokens);
    // Expand all pending instantiations, appending concrete declarations to `program`.
    // `filename` labels monomorphization parse errors (reported at the use-site line).
    void monomorphize(Program& program, const std::string& filename = "");

    // ---- Module namespacing (public statics so ImportResolver can reuse them) ----
    // Scan a file's tokens for its `module NAME;` (→ outModule) and every `import a.B;` symbol
    // import (→ outBindings simpleName→qualified; a name imported from >1 module → outAmbiguous).
    static void scanModuleDirectives(const std::vector<Token>& toks, std::string& outModule,
                                     std::unordered_map<std::string, std::string>& outBindings,
                                     std::unordered_set<std::string>& outAmbiguous);
    // Record a file's top-level decl names (excluding extern + `main`) into types[module] /
    // funcs[module] (by kind) and add `module` to names. Registers the module even with no members.
    static void scanModuleMembers(const std::vector<Token>& toks, const std::string& module,
                                  std::unordered_map<std::string, std::unordered_set<std::string>>& types,
                                  std::unordered_map<std::string, std::unordered_set<std::string>>& funcs,
                                  std::unordered_set<std::string>& names);
    // Rewrite a token stream so every name carries its module prefix (mirrors generic mangling):
    // fold a fully-qualified `mod.Name` (mod ∈ moduleNames) into one `"mod.Name"` token, then
    // qualify each bare reference/decl name. A TYPE name is qualified in any position; a FUNCTION
    // name only in call/generic position (`(`/`<`); a method/enum-member decl name (preceded by
    // `fn` inside a body) and any name after `.`/`::`, plus `main`, are left untouched.
    static std::vector<Token> qualifyTokens(
        const std::vector<Token>& toks, const std::string& module,
        const std::unordered_map<std::string, std::string>& bindings,
        const std::unordered_set<std::string>& ambiguous,
        const std::unordered_map<std::string, std::unordered_set<std::string>>& types,
        const std::unordered_map<std::string, std::unordered_set<std::string>>& funcs,
        const std::unordered_set<std::string>& moduleNames);

private:
    std::vector<Token>             tokens;
    size_t                         current = 0;
    std::string                    filename;               // source filename for error messages
    std::unordered_set<std::string> classNames;   // class names registered during parse
    bool                           insideFunction = false;  // true when parsing a function/method body

    // ---- Module namespacing (per file) ----
    std::string                                  currentModule_;    // this file's module ("" = global)
    std::unordered_map<std::string, std::string> importBindings_;   // simple name → qualified target (`import a.B;`)
    std::unordered_set<std::string>              ambiguousImports_; // names imported from >1 module (must qualify)

    // ---- Generics (monomorphization) ----
    // Generic state lives in a registry that may be shared across files. By default
    // each parser owns its own; ImportResolver passes a shared one for cross-file use.
    GenericRegistry  ownedGenerics_;
    GenericRegistry* gen_ = &ownedGenerics_;
    int pendingCloseAngles_ = 0;       // virtual '>' tokens from splitting a '>>'

    // ---- Callable (`Call` trait) support ----
    // `Call(P…)->R` is sugar for a canonical user trait `Call$P…$ret$R` with a `call(P…)->R`
    // method. Generated once per distinct signature and appended to the Program, so a bound, an
    // `impl Call`, and a lambda that share a signature all name the same trait.
    std::vector<Stmt>               pendingCallTraits_;   // canonical Call trait decls to append (this file)
    std::vector<Stmt>               pendingLambdaDecls_;  // generated `__lambda_N` class + impl decls

    // ---- Lexical scope tracking (for lambda capture analysis) ----
    // A stack of in-scope local/parameter names → their declared type token. Pushed per block
    // (parseBlockBody) and seeded from the enclosing function's params. Used only to compute a
    // lambda's captured free variables; empty of overhead outside function bodies.
    std::vector<std::unordered_map<std::string, Token>> scopes_;
    std::vector<std::pair<std::string, Token>>          pendingScopeSeed_;  // params to seed next block scope
    // Instance fields of the class currently being parsed (name → type token), so a lambda inside
    // a method can capture an enclosing field by value. Populated as fields are parsed.
    std::unordered_map<std::string, Token>              classFieldScope_;
    // Name of the class currently being parsed (possibly the mangled name during a generic class's
    // re-parse), or empty at top level. Set/restored around a class/impl member list; lets a generic
    // method call resolve a `this` receiver's class at parse time (deduceArgTypeToken's ThisExpr case).
    std::string                                         currentClassName_;
    // Generic-method instantiations whose owner class / method template is not yet available (a
    // generic class's methods are registered only when its `Class$args` body is re-parsed). Drained
    // to a fixpoint after the main worklist. See runMonomorphization.
    std::vector<GenericInstantiation>                   pendingMethodInsts_;
    // Active while parsing a lambda body: names resolved to a scope index < captureBase_ are
    // captured by value into `captures_` (deduped). Nested lambdas are rejected in v1.
    bool                                       capturing_   = false;
    size_t                                     captureBase_ = 0;
    std::vector<std::pair<std::string, Token>> captures_;
    void recordLocal(const Token& typeToken, const Token& name);
    // Expected `Call(…)` signature for a lambda argument at the current call site (param type
    // tokens + return type token), or nullptr. Lets an untyped lambda infer its parameter/return
    // types. Points into `gen_->callTraitSigs` (stable), saved/restored around each call's args.
    const std::pair<std::vector<Token>, Token>* expectedLambdaSig_ = nullptr;
    // True while parsing a switch case *label*, where a trailing `->` is the arm separator, not a
    // lambda arrow (a bare-identifier label `case x ->` must not be read as a lambda `x -> …`).
    bool parsingCaseLabel_ = false;
    // True while parsing the method bodies of an `impl` block. Used only to enrich a
    // "not a known type" diagnostic: `impl` blocks do not introduce type parameters, so a
    // bare `T` in `impl Trait for Foo<T>` is unresolved.
    bool inImplBlock_ = false;
    // Throw a "type expected" error that names the offending token and, inside an `impl`,
    // explains that generic type parameters are not in scope there. `what` names the position,
    // e.g. "a return type after '->'".
    [[noreturn]] void throwTypeExpected(const std::string& what);
    // True if the tokens at the current `(` form a lambda `( … ) -> …` (vs a grouped expression).
    [[nodiscard]] bool isLambdaAhead() const;
    // Parse the parenthesized parameter list of a lambda, allowing typed (`i32 x`) and untyped (`x`)
    // parameters (nullopt type ⇒ inferred from the expected signature).
    [[nodiscard]] std::vector<std::pair<Token, std::optional<Token>>> parseLambdaParamList();
    // Finish a lambda given its parameters (types optional): parse `-> [Ret] { body }`, resolve any
    // omitted parameter/return types from the expected signature, generate the class + impl, and
    // return the stack-construction expression `__lambda_N(captures…)`.
    [[nodiscard]] Expr finishLambda(std::vector<std::pair<Token, std::optional<Token>>> params, int line);
    // Parse a lambda literal `(params) -> [Ret] { body }` (parenthesized-parameter form).
    [[nodiscard]] Expr parseLambda();
    // Canonical `Call$…` trait name for a call signature (param type tokens + return type token);
    // registers the canonical trait decl for emission on first sight.
    std::string canonicalCallTrait(const std::vector<Token>& paramTypeTokens, const Token& retType);
    // Read one Call-signature type at raw token index k (a base type optionally followed by '&'),
    // returning a synthesized type token and advancing k past it; nullopt if k is not a type.
    [[nodiscard]] std::optional<Token> readCallSigType(size_t& k) const;
    // Parse a `Call(P…)->R` bound starting at token index k (positioned at `Call`), advancing k
    // past it; returns the canonical `Call$…` trait name, or nullopt if the signature is malformed.
    [[nodiscard]] std::optional<std::string> scanCallBound(size_t& k);

    // ---- Token stream navigation ----
    [[nodiscard]] const Token& peek() const;
    [[nodiscard]] const Token& peekNext() const;
    [[nodiscard]] const Token& previous() const;
    [[nodiscard]] bool         isAtEnd() const;
                  const Token& advance();
    [[nodiscard]] bool         check(TokenType type) const;
    [[nodiscard]] bool         match(std::initializer_list<TokenType> types);
                  const Token& consume(TokenType type, const std::string& msg);

    // ---- Error handling ----
    [[nodiscard]] ParseError error(const Token& token, const std::string& msg);
                  void       synchronize();

    // ---- Helpers ----
    [[nodiscard]] bool isTypeName() const;
    // Consume a type at the current position: a base type token optionally
    // followed by '&' (a reference, Ref<Class>). For a reference, returns a
    // single synthesized IDENTIFIER token with lexeme "<Class>&".
    Token consumeType();
    Token consumeTypeCore();   // consumeType without the trailing `?` (nullable) handling
    // A tuple type `(T1, T2, …)` (arity ≥ 2). Parses the element types, mangles a canonical class
    // name `Tuple$<m1>$<m2>…`, records a synthesis request in `gen_->tupleRequests`, and returns a
    // single synthesized IDENTIFIER token with that name (an ordinary value-object type downstream).
    Token consumeTupleType();

    // ---- Generics helpers ----
    bool tryCaptureFunctionTemplate();                       // capture a generic fn decl
    bool tryCaptureClassTemplate();                          // capture a generic class decl
    bool tryCaptureImplTemplate();                           // capture a generic `impl<T> … for C<T>`
    // Scan a `<T, U: Trait + ..., ...Ts>` param list from index `from`; fills typeParams +
    // parallel bounds + parallel isPack (a `...Ts` variadic pack), returns the closing '>' index
    // (0 if malformed).
    size_t scanTypeParamList(size_t from, std::vector<std::string>& typeParams,
                             std::vector<std::vector<std::string>>& bounds,
                             std::vector<bool>& isPack);
    // Number of tokens a complete type occupies at `from` (base + optional <...> + optional &),
    // or 0 if `from` is not the start of a type. Used by declaration-detection lookahead.
    [[nodiscard]] size_t typeSpanAt(size_t from) const;
    std::vector<std::vector<Token>> parseTypeArgList();      // at '<', returns arg slices through '>'
    std::vector<Token>              parseOneTypeArg();       // one arg; nested generics collapsed
    void                            consumeCloseAngle();     // consume '>' (splitting a '>>')
    [[nodiscard]] std::string mangleInstantiation(const std::string& base,
                                  const std::vector<std::vector<Token>>& args) const;
    void recordInstantiation(const std::string& templateName, const std::string& mangled,
                             std::vector<std::vector<Token>> args);
    // ---- Generic methods (`fn m<T>` on a class) ----
    // Capture a generic method template inside a class body (called from parseMemberList before the
    // param list is parsed). `ownerClass` is the enclosing (possibly mangled) class name. Stores the
    // synthetic method decl tokens under `ownerClass::name` and registers the call-site gates.
    void captureMethodTemplate(const std::string& ownerClass, bool isStatic, bool isPublic);
    // Queue a generic-method instantiation (`ownerClass::mangled`, e.g. `Box::wrap$i32`), deduped on
    // that class-qualified key so the same method name on two classes doesn't collide.
    void recordMethodInstantiation(const std::string& ownerClass, const std::string& bareMethodName,
                                   const std::string& mangled, std::vector<std::vector<Token>> args);
    // Substitute + re-parse a single generic-method instantiation and inject the concrete MethodDecl
    // into its owner class's ClassDeclStmt. Returns false if the owner class / method template is not
    // yet available (a generic class whose body hasn't been re-parsed) — the caller defers + retries.
    bool instantiateMethod(const GenericInstantiation& inst, Program& program);
    // Generic type-argument DEDUCTION for a `f(args)` call written WITHOUT explicit `<…>`.
    // Infers each type parameter of the template `fnName` from the call arguments' types, so
    // `f(x)` works like `f<T>(x)`. v1 handles the common shape: a type parameter appearing as a
    // top-level parameter type (`T`, `T&`, `T*`, `T?`) matched against a positional argument whose
    // type is syntactically known — an in-scope identifier, a `Class(...)`/`Class{...}` constructor
    // call, or `new Class(...)`. Fills `out` (one token-slice per type param, in order) and returns
    // true iff EVERY type parameter was deduced; false otherwise (caller then errors).
    bool inferGenericTypeArgs(const std::string& fnName,
                              const std::vector<std::unique_ptr<Expr>>& args,
                              std::vector<std::vector<Token>>& out);
    // ---- Variadic packs ----
    // Best-effort deduce a call argument's TYPE token (proper token kind so it re-parses/mangles
    // correctly): a literal by its default type (int→i32, decimal→f64, string→str, bool, char), an
    // in-scope identifier / instance field via `scopes_`, a `Class(...)` ctor call, or `new Class(...)`.
    // Returns nullopt if the type isn't parser-visible.
    [[nodiscard]] std::optional<Token> deduceArgTypeToken(const Expr* arg) const;
    // Record a tuple type from its ordered element type tokens (deduped) and return its mangled
    // `Tuple$…` class name — the direct-request counterpart of `consumeTupleType` (allows arity 0/1,
    // used only for internal pack tails/empty).
    std::string requestTupleType(const std::vector<Token>& elems);
    // If `fnName` is a variadic template, collect the trailing arguments into a pack tuple, deduce its
    // element types, record the tuple + the `fnName$Tuple$…` instantiation, and REWRITE `args` to
    // [fixed prefix…, one tuple literal]. A single trailing `xs...` spread (spreads[k]) passes an
    // existing pack tuple directly instead of re-tupling. Returns the mangled instantiation name, or
    // nullopt if `fnName` isn't variadic. Throws a clear error if a pack element's type can't be deduced.
    [[nodiscard]] std::optional<std::string> deduceVariadicInstantiation(
        const std::string& fnName, std::vector<std::unique_ptr<Expr>>& args,
        const std::vector<bool>& spreads);
    // Core of the above, shared with variadic METHODS: given the number of fixed (non-pack) params,
    // split the trailing args into the pack, deduce/collect its tuple type, REWRITE `args` to
    // [fixed prefix…, one tuple literal] (or pass a spread pack through), and return the type-arg slice
    // (the single `Tuple$…` token). nullopt if there are too few args (caller falls back to the normal
    // arity error). `diagName` labels errors. Throws on an undeducible / bad-spread pack argument.
    [[nodiscard]] std::optional<std::vector<std::vector<Token>>> deducePackTargs(
        size_t fixedCount, const std::string& diagName,
        std::vector<std::unique_ptr<Expr>>& args, const std::vector<bool>& spreads);
    // Variadic METHOD call `recv.m(fixed…, pack…)` (no explicit `<…>`): collect the pack into a tuple
    // and queue the `ownerClass::m$Tuple$…` method instantiation. Returns the mangled method name.
    [[nodiscard]] std::optional<std::string> deduceVariadicMethodInstantiation(
        const std::string& ownerClass, const std::string& methodName, size_t fixedCount,
        std::vector<std::unique_ptr<Expr>>& args, const std::vector<bool>& spreads);
    // Spread into a NON-pack-bearing target: if `spreads` marks a spread argument, unwraps its (bare
    // identifier, tuple-typed) target into N positional `xs._0, …, xs._{N-1}` arguments in place and
    // returns true; returns false (no-op) if `spreads` has no spread flag. Throws a clear error for
    // more than one spread, a named spread, a non-identifier spread target, or a non-tuple target.
    // Callers are responsible for first ruling out that the callee is itself a pack-bearing template
    // (see deduceVariadicInstantiation) — every OTHER call form (ctor, method, `new`) can call this
    // unconditionally since packs are v1-scoped to free functions only.
    bool unwrapSpreadArgs(std::vector<std::unique_ptr<Expr>>& args, std::vector<Token>& argNames,
                         const std::vector<bool>& spreads);
    // Expand a compile-time cons-`match` over a variadic pack IN PLACE (token level, during
    // monomorphization where the pack arity is known): each `match <packValue> { () -> A; (x:xs) -> B }`
    // is replaced by the arm selected by the pack's arity — `A` for arity 0, else `B` with the head `x`
    // rewritten to `<packValue>._0` and the tail `xs` materialized as a tail-tuple local. `elems` are
    // the pack's element type tokens.
    void expandPackMatch(std::vector<Token>& body, const std::string& packValue,
                         const std::vector<Token>& elems);
    // The base type name a deduced argument contributes (strips a trailing `?`, a `ref:` borrow
    // prefix, and a trailing `&`): `User&` / `ref:User` / `User?` → `User`; `i32` → `i32`.
    [[nodiscard]] static std::string genericArgBaseName(const Token& typeToken);
    void runMonomorphization(Program& program);
    // Synthesize a concrete value-object ClassDeclStmt for every recorded tuple type (fields
    // `_0 … _n` + a positional constructor) and append it to `program.declarations`, deduped by the
    // mangled name. Object-typed elements take a reference constructor parameter (`ElemType&`) that
    // clones into the value field, mirroring an embedded value-object field. Runs after the
    // monomorphization worklist (so tuples discovered during it are included), before reflection.
    void synthesizeTupleClasses(Program& program);

    // ---- Compile-time reflection expansion (runs at the end of runMonomorphization) ----
    // Build className -> ordered instance-field-name list from parsed ClassDeclStmts, then expand
    // every `inline for (v in @fields(T))` into ordinary statements (one copy per field, with the
    // binding token-substituted: `v.name` -> string literal, `@field(obj, v.name)` -> obj.field).
    // className -> ordered instance-field names; enumName -> ordered variant names.
    struct ReflectRegistry {
        std::unordered_map<std::string, std::vector<std::string>> fields;
        std::unordered_map<std::string, std::vector<std::string>> variants;
        // All declared `annotation` type names (to validate `f.has(Ann)` references).
        std::unordered_set<std::string> annotationTypes;
        // "TypeName#memberName" → set of annotation names on that member (drives `f.has`).
        std::unordered_map<std::string, std::unordered_set<std::string>> memberAnnotations;
        // annotationName → ordered field names (maps `f.get(Ann).field` to a positional arg).
        std::unordered_map<std::string, std::vector<std::string>> annotationFields;
        // annotationName → per-field default literal token (used by `f.get` on a member that lacks
        // the annotation — only reached in a false-`f.has`-guarded dead branch). Parallel to fields.
        std::unordered_map<std::string, std::vector<Token>> annotationFieldDefaults;
        // "TypeName#memberName" → annotationName → positional arg token-sequences (drives `f.get`).
        std::unordered_map<std::string,
            std::unordered_map<std::string, std::vector<std::vector<Token>>>> memberAnnotationArgs;
    };
    // Per-inline-for annotation context handed to substituteInlineForBody (transient; not stored).
    // Bundles the global annotation tables + the CURRENT member's annotation names/args.
    struct InlineAnnCtx {
        const std::unordered_set<std::string>& annTypes;
        const std::unordered_set<std::string>& memberAnns;
        const std::unordered_map<std::string, std::vector<std::vector<Token>>>& memberArgs;
        const std::unordered_map<std::string, std::vector<std::string>>& annFields;
        const std::unordered_map<std::string, std::vector<Token>>& annFieldDefaults;
    };
    void expandReflection(Program& program);
    void expandReflectionInStmt(Stmt& stmt, const ReflectRegistry& reg);
    void expandReflectionInBlock(BlockStmt& block, const ReflectRegistry& reg);
    // Substitute the inline-for binding in one member's copy of the captured body tokens.
    //   fields   (overVariants=false): `v.name`->string, `@field(o,v.name)`->o.member, bare v = error
    //   variants (overVariants=true):  `v.name`->string, bare v-> `Enum::member`, @field = error
    //   both: `v.has(Ann)` -> a bool literal (does the current member carry annotation `Ann`),
    //         `v.get(Ann).field` -> the const value of that annotation field (spliced arg tokens).
    // `memberAnns`=annotation names on this member; `annTypes`=all declared annotations;
    // `memberArgs`=annName→positional arg tokens for this member; `annFields`=annName→field names.
    std::vector<Token> substituteInlineForBody(const std::vector<Token>& bodyTokens,
                                               const std::string& loopVar, const std::string& memberName,
                                               bool overVariants, const std::string& enumName,
                                               const InlineAnnCtx& ann);

    // ---- Statement parsers ----
    [[nodiscard]] Stmt      parseDeclaration();
    [[nodiscard]] Stmt      parseClassDecl();
    [[nodiscard]] Stmt      parseEnumDecl();
    [[nodiscard]] Stmt      parseTraitDecl();
    [[nodiscard]] Stmt      parseImplDecl();
    [[nodiscard]] Stmt      parseAnnotationDecl();   // `annotation Name { <const fields> }`
    // Parse a run of `@Name` / `@Name(args)` annotation prefixes in declaration position.
    [[nodiscard]] std::deque<AnnotationApp> parseAnnotationPrefixes();
    // Parse one method signature/definition inside a trait or impl body. In a trait, a `;`
    // after the header means a required (bodyless) method; a `{` means a default body.
    [[nodiscard]] MethodDecl parseTraitMethod(bool bodyOptional);
    // Parse the member list (fields, methods, optional constructor/destructor) of a
    // class or enum body until the closing '}'. `typeName` is used to detect the
    // constructor (a method whose name matches). Destructors are only valid for classes.
    void                    parseMemberList(const Token& typeName,
                                            std::deque<FieldDecl>& fields,
                                            std::deque<MethodDecl>& methods,
                                            bool allowDestructor,
                                            bool isEnum);
    // After the `fn` keyword: a free function `name(params) [-> RetType [alias]] { }`.
    [[nodiscard]] Stmt      parseFnDeclaration();
    // Parse the tail of a trait/impl method (`;` bodyless, or `{ body }`).
    void                    parseTraitMethodBody(bool bodyOptional, bool& hasBody, BlockStmt& body);
    // Parses `(param, ...)` — the parenthesised parameter list. `allowDefaults` gates `= expr`
    // default values (false for `extern`). Defaults must form a contiguous trailing run.
    [[nodiscard]] std::vector<ParamDecl> parseParamList(bool allowDefaults = true);
    // Parses an optional `-> RetType [alias]` return suffix. Returns the return type token
    // (a synthesized `void` token when the arrow is absent); sets hasAlias/aliasName.
    [[nodiscard]] Token     parseReturnSuffix(bool& hasAlias, std::string& aliasName);
    // Parses one parameter: an optional leading `mut`, then `<type> IDENTIFIER`.
    [[nodiscard]] ParamDecl parseParam();
    [[nodiscard]] Stmt      parseExternFuncDecl(const Token& keyword);
    [[nodiscard]] Stmt      parseImportStmt(const Token& keyword);
    [[nodiscard]] Stmt      parseStatement();
    [[nodiscard]] BlockStmt parseBlockBody();
    [[nodiscard]] Stmt      parseBlock();
    [[nodiscard]] Stmt      parseIfStmt();
    [[nodiscard]] Stmt      parseWhileStmt();
    [[nodiscard]] Stmt      parseForStmt();
    // `for (DECL : ITERABLE) BODY` — the Java-style range loop. Desugars (parser-only) to
    // `{ mut var __forit = (ITERABLE).iter(); while (__forit.hasNext()) { DECL = __forit.next(); BODY } }`.
    [[nodiscard]] Stmt      parseForInStmt();
    int                     forInCounter_ = 0;   // unique `__forit_N` cursor names
    [[nodiscard]] Stmt      parseInlineForStmt();   // `inline for (v in @fields(T)) { … }`
    [[nodiscard]] Stmt      parseReturnStmt();
    [[nodiscard]] Stmt      parseBreakStmt();
    [[nodiscard]] Stmt      parseContinueStmt();
    [[nodiscard]] Stmt      parseSwitchStmt();
    [[nodiscard]] Stmt      parseYieldStmt();
    [[nodiscard]] Expr      parseSwitchExpr();
    [[nodiscard]] std::deque<SwitchArm> parseSwitchArmBlock();
    // `match` — destructuring pattern match. Statement + expression forms share the arm-block parser
    // (mirrors switch); `parsePattern` is the recursive-descent pattern grammar.
    [[nodiscard]] Stmt      parseMatchStmt();
    [[nodiscard]] Expr      parseMatchExpr();
    [[nodiscard]] std::deque<MatchArm> parseMatchArmBlock();
    [[nodiscard]] Pattern   parsePattern();
    [[nodiscard]] Stmt      parseExprStmt();

    // ---- Expression parsers (low to high precedence) ----
    [[nodiscard]] Expr parseExpression();
    [[nodiscard]] Expr parseAssignment();
    [[nodiscard]] Expr parseTernary();
    [[nodiscard]] Expr parseElvis();
    [[nodiscard]] Expr parseLogicalOr();
    [[nodiscard]] Expr parseLogicalAnd();
    [[nodiscard]] Expr parseBitwiseOr();
    [[nodiscard]] Expr parseBitwiseXor();
    [[nodiscard]] Expr parseBitwiseAnd();
    [[nodiscard]] Expr parseEquality();
    [[nodiscard]] Expr parseComparison();
    [[nodiscard]] Expr parseShift();
    [[nodiscard]] Expr parseAddSub();
    [[nodiscard]] Expr parseMulDiv();
    [[nodiscard]] Expr parseCast();
    [[nodiscard]] Expr parseUnary();
    [[nodiscard]] Expr parsePostfix();
    [[nodiscard]] Expr parsePrimary();
    [[nodiscard]] Expr parseReflectExpr();   // `@name(args)` compile-time reflection builtin

    // Parse a call's argument list body (the opening delimiter is already consumed) up to and
    // including `close`. Fills `names` parallel to the returned exprs: a name with a non-empty
    // lexeme marks a named argument (`f(x: 1)`), an empty-lexeme token a positional one. When
    // `allowNames` is false (e.g. brace-form constructor calls), `name:` is not recognised.
    // Enforces that a positional argument may not follow a named one.
    // `spreads` (optional, parallel to the returned args) records a trailing `xs...` spread on each
    // argument. When null, a `...` after an argument is an error (spreads are only valid in a call
    // that supports them — a variadic call).
    [[nodiscard]] std::vector<std::unique_ptr<Expr>> parseCallArgs(
        std::vector<Token>& names, TokenType close, bool allowNames,
        std::vector<bool>* spreads = nullptr);
};

#endif //GG_PARSER_H
