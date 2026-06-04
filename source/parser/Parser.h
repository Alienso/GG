//
// Created by Vladimir Arsenijevic on 28.5.2026.
//

#ifndef GG_PARSER_H
#define GG_PARSER_H

#include "Ast.h"
#include "../lexer/Token.h"

#include <vector>
#include <stdexcept>
#include <initializer_list>

class ParseError : public std::runtime_error {
public:
    explicit ParseError(const std::string& msg) : std::runtime_error(msg) {}
};

class Parser {
public:
    Parser();
    [[nodiscard]] Program parse(const std::vector<Token>& inputTokens);

private:
    std::vector<Token> tokens;
    size_t             current = 0;

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

    // ---- Statement parsers ----
    [[nodiscard]] Stmt      parseDeclaration();
    [[nodiscard]] Stmt      parseFunctionDecl(Token returnType, Token name);
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
    [[nodiscard]] Expr parseUnary();
    [[nodiscard]] Expr parsePostfix();
    [[nodiscard]] Expr parsePrimary();
};

#endif //GG_PARSER_H
