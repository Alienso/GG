//
// Created by Vladimir Arsenijevic on 28.5.2026.
//

#ifndef GG_PARSER_H
#define GG_PARSER_H

#include "Ast.h"
#include "../lexer/Token.h"
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

class Parser {
public:
    Parser() = default;
    // Pre-register class names from imported files so that cross-file constructor
    // calls (e.g. "String s(...)") are recognised as VarDecl, not misidentified
    // as a function declaration or expression.
    explicit Parser(std::unordered_set<std::string> initialClassNames);
    [[nodiscard]] Program parse(const std::vector<Token>& inputTokens, const std::string& filename = "");

private:
    std::vector<Token>             tokens;
    size_t                         current = 0;
    std::string                    filename;               // source filename for error messages
    std::unordered_set<std::string> classNames;   // class names registered during parse
    bool                           insideFunction = false;  // true when parsing a function/method body

    // ---- Generics (monomorphization) ----
    // A generic declaration is captured as raw tokens; each use site records an
    // instantiation request. After the main parse, the worklist substitutes the
    // type arguments and re-parses each request into a concrete declaration, so
    // semantic analysis and codegen only ever see ordinary (mangled) decls.
    struct Template {
        std::vector<std::string> typeParams;
        std::vector<Token>       tokens;   // decl tokens; tokens[1] is the name; <...> stripped
    };
    std::unordered_map<std::string, Template> templates_;          // by template name (fn or class)
    std::unordered_set<std::string>           templateFuncNames_;  // generic function names
    std::unordered_set<std::string>           templateClassNames_; // generic class names

    struct PendingInstantiation {
        std::string                     templateName;
        std::string                     mangledName;
        std::vector<std::vector<Token>> args;     // each type argument's token slice
    };
    std::vector<PendingInstantiation> instantiationWorklist_;
    std::unordered_set<std::string>   instantiated_;   // mangled names already queued

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
    // Number of tokens a complete type occupies at `from` (base + optional <...> + optional &),
    // or 0 if `from` is not the start of a type. Used by declaration-detection lookahead.
    [[nodiscard]] size_t typeSpanAt(size_t from) const;
    std::vector<std::vector<Token>> parseTypeArgList();      // at '<', returns arg slices through '>'
    [[nodiscard]] std::string mangleInstantiation(const std::string& base,
                                  const std::vector<std::vector<Token>>& args) const;
    void recordInstantiation(const std::string& templateName, const std::string& mangled,
                             std::vector<std::vector<Token>> args);
    void runMonomorphization(Program& program);

    // ---- Statement parsers ----
    [[nodiscard]] Stmt      parseDeclaration();
    [[nodiscard]] Stmt      parseClassDecl();
    [[nodiscard]] Stmt      parseFunctionDecl(const Token& returnType, const Token& name);
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
