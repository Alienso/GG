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
    std::vector<Token>                    tokens;    // decl tokens; tokens[1] is the name; <...> stripped
};
struct GenericInstantiation {
    std::string                     templateName;
    std::string                     mangledName;
    std::vector<std::vector<Token>> args;     // each type argument's token slice
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
};
struct GenericRegistry {
    std::unordered_map<std::string, GenericTemplate> templates;     // by template name (fn or class)
    std::unordered_set<std::string>                  funcNames;     // generic function names
    std::unordered_set<std::string>                  classNames;    // generic class names
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
    void monomorphize(Program& program);

private:
    std::vector<Token>             tokens;
    size_t                         current = 0;
    std::string                    filename;               // source filename for error messages
    std::unordered_set<std::string> classNames;   // class names registered during parse
    bool                           insideFunction = false;  // true when parsing a function/method body

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

    // ---- Generics helpers ----
    bool tryCaptureFunctionTemplate();                       // capture a generic fn decl
    bool tryCaptureClassTemplate();                          // capture a generic class decl
    bool tryCaptureImplTemplate();                           // capture a generic `impl<T> … for C<T>`
    // Scan a `<T, U: Trait + ...>` param list from index `from`; fills typeParams +
    // parallel bounds, returns the closing '>' index (0 if malformed).
    size_t scanTypeParamList(size_t from, std::vector<std::string>& typeParams,
                             std::vector<std::vector<std::string>>& bounds);
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
    void runMonomorphization(Program& program);

    // ---- Statement parsers ----
    [[nodiscard]] Stmt      parseDeclaration();
    [[nodiscard]] Stmt      parseClassDecl();
    [[nodiscard]] Stmt      parseEnumDecl();
    [[nodiscard]] Stmt      parseTraitDecl();
    [[nodiscard]] Stmt      parseImplDecl();
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
    [[nodiscard]] Stmt      parseReturnStmt();
    [[nodiscard]] Stmt      parseBreakStmt();
    [[nodiscard]] Stmt      parseContinueStmt();
    [[nodiscard]] Stmt      parseSwitchStmt();
    [[nodiscard]] Stmt      parseYieldStmt();
    [[nodiscard]] Expr      parseSwitchExpr();
    [[nodiscard]] std::deque<SwitchArm> parseSwitchArmBlock();
    [[nodiscard]] Stmt      parseExprStmt();

    // ---- Expression parsers (low to high precedence) ----
    [[nodiscard]] Expr parseExpression();
    [[nodiscard]] Expr parseAssignment();
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
};

#endif //GG_PARSER_H
