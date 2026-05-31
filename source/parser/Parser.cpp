//
// Created by Vladimir Arsenijevic on 28.5.2026.
//

#include "Parser.h"
#include <iostream>

Parser::Parser() {}

Program Parser::parse(const std::vector<Token>& tokens_) {
    tokens  = std::vector<Token>(tokens);
    current = 0;

    Program program;
    while (!isAtEnd()) {
        try {
            program.declarations.push_back(parseDeclaration());
        } catch (const ParseError&) {
            synchronize();
        }
    }
    return program;
}

// ============================================================
// Token stream navigation
// ============================================================

const Token& Parser::peek() const {
    return tokens[current];
}

const Token& Parser::peekNext() const {
    if (current + 1 >= tokens.size()) return tokens.back();
    return tokens[current + 1];
}

const Token& Parser::previous() const {
    return tokens[current - 1];
}

bool Parser::isAtEnd() const {
    return peek().type == TokenType::END_OF_FILE;
}

const Token& Parser::advance() {
    if (!isAtEnd()) current++;
    return previous();
}

bool Parser::check(TokenType type) const {
    return !isAtEnd() && peek().type == type;
}

bool Parser::match(std::initializer_list<TokenType> types) {
    for (TokenType type : types) {
        if (check(type)) { advance(); return true; }
    }
    return false;
}

const Token& Parser::consume(TokenType type, const std::string& msg) {
    if (check(type)) return advance();
    throw error(peek(), msg);
}

// ============================================================
// Error handling
// ============================================================

ParseError Parser::error(const Token& token, const std::string& msg) {
    std::string full = "[line " + std::to_string(token.line) + "] Error: " + msg;
    std::cerr << full << '\n';
    return ParseError(full);
}

void Parser::synchronize() {
    advance();
    while (!isAtEnd()) {
        if (previous().type == TokenType::SEMICOLON) return;
        switch (peek().type) {
            case TokenType::IF:
            case TokenType::WHILE:
            case TokenType::FOR:
            case TokenType::RETURN:
            case TokenType::I8:  case TokenType::I16: case TokenType::I32: case TokenType::I64:
            case TokenType::U8:  case TokenType::U16: case TokenType::U32: case TokenType::U64:
            case TokenType::F32: case TokenType::F64:
            case TokenType::BOOL: case TokenType::CHAR_TYPE: case TokenType::STRING_TYPE:
                return;
            default:
                break;
        }
        advance();
    }
}

// ============================================================
// Helpers
// ============================================================

bool Parser::isTypeName() const {
    switch (peek().type) {
        case TokenType::I8:  case TokenType::I16: case TokenType::I32: case TokenType::I64:
        case TokenType::U8:  case TokenType::U16: case TokenType::U32: case TokenType::U64:
        case TokenType::F32: case TokenType::F64:
        case TokenType::BOOL: case TokenType::CHAR_TYPE: case TokenType::STRING_TYPE:
            return true;
        default:
            return false;
    }
}

// ============================================================
// Statement parsers
// ============================================================

Stmt Parser::parseDeclaration() {
    // Function decl: typeName IDENTIFIER ( ...
    // Variable decl at top level falls through to parseStatement → parseExprStmt
    // since VarDecl is an expression.
    if (isTypeName()
        && peekNext().type == TokenType::IDENTIFIER
        && current + 2 < tokens.size()
        && tokens[current + 2].type == TokenType::LEFT_PAREN) {
        Token returnType = advance();
        Token name       = advance();
        return parseFunctionDecl(returnType, name);
    }
    return parseStatement();
}

Stmt Parser::parseFunctionDecl(Token returnType, Token name) {
    consume(TokenType::LEFT_PAREN, "expected '(' after function name");

    std::vector<ParamDecl> params;
    if (!check(TokenType::RIGHT_PAREN)) {
        do {
            if (!isTypeName()) throw error(peek(), "expected parameter type");
            Token pType = advance();
            Token pName = consume(TokenType::IDENTIFIER, "expected parameter name");
            params.push_back(ParamDecl{ pType, pName });
        } while (match({ TokenType::COMMA }));
    }
    consume(TokenType::RIGHT_PAREN, "expected ')' after parameters");
    consume(TokenType::LEFT_BRACE,  "expected '{' before function body");

    BlockStmt body = parseBlockBody();
    return makeStmt(FunctionDeclStmt{ returnType, name, std::move(params), std::move(body) });
}

Stmt Parser::parseStatement() {
    if (check(TokenType::LEFT_BRACE))   return parseBlock();
    if (match({ TokenType::IF     }))   return parseIfStmt();
    if (match({ TokenType::WHILE  }))   return parseWhileStmt();
    if (match({ TokenType::FOR    }))   return parseForStmt();
    if (match({ TokenType::RETURN }))   return parseReturnStmt();
    return parseExprStmt();
}

BlockStmt Parser::parseBlockBody() {
    // Called after '{' has already been consumed.
    std::vector<std::unique_ptr<Stmt>> body;
    while (!check(TokenType::RIGHT_BRACE) && !isAtEnd()) {
        try {
            body.push_back(std::make_unique<Stmt>(parseDeclaration()));
        } catch (const ParseError&) {
            synchronize();
        }
    }
    consume(TokenType::RIGHT_BRACE, "expected '}'");
    return BlockStmt{ std::move(body) };
}

Stmt Parser::parseBlock() {
    consume(TokenType::LEFT_BRACE, "expected '{'");
    return makeStmt(parseBlockBody());
}

Stmt Parser::parseIfStmt() {
    consume(TokenType::LEFT_PAREN, "expected '(' after 'if'");
    Expr condition = parseExpression();
    consume(TokenType::RIGHT_PAREN, "expected ')' after if condition");

    Stmt thenBranch = parseStatement();   // block or single statement

    std::unique_ptr<Stmt> elseBranch = nullptr;
    if (match({ TokenType::ELSE })) {
        elseBranch = box(parseStatement());
    }

    return makeStmt(IfStmt{
        std::move(condition),
        box(std::move(thenBranch)),
        std::move(elseBranch)
    });
}

Stmt Parser::parseWhileStmt() {
    consume(TokenType::LEFT_PAREN, "expected '(' after 'while'");
    Expr condition = parseExpression();
    consume(TokenType::RIGHT_PAREN, "expected ')' after while condition");

    Stmt body = parseStatement();         // block or single statement
    return makeStmt(WhileStmt{ std::move(condition), box(std::move(body)) });
}

Stmt Parser::parseForStmt() {
    consume(TokenType::LEFT_PAREN, "expected '(' after 'for'");

    // Initializer — VarDeclExpr and plain expressions both go through parseExprStmt
    std::unique_ptr<Stmt> init = nullptr;
    if (check(TokenType::SEMICOLON)) {
        advance();   // empty initializer
    } else {
        init = box(parseExprStmt());
    }

    // Condition
    std::optional<Expr> condition;
    if (!check(TokenType::SEMICOLON)) condition = parseExpression();
    consume(TokenType::SEMICOLON, "expected ';' after for condition");

    // Increment
    std::optional<Expr> increment;
    if (!check(TokenType::RIGHT_PAREN)) increment = parseExpression();
    consume(TokenType::RIGHT_PAREN, "expected ')' after for clauses");

    Stmt body = parseStatement();         // block or single statement
    return makeStmt(ForStmt{
        std::move(init),
        std::move(condition),
        std::move(increment),
        box(std::move(body))
    });
}

Stmt Parser::parseReturnStmt() {
    Token keyword = previous();
    std::optional<Expr> value;
    if (!check(TokenType::SEMICOLON)) value = parseExpression();
    consume(TokenType::SEMICOLON, "expected ';' after return value");
    return makeStmt(ReturnStmt{ keyword, std::move(value) });
}

Stmt Parser::parseExprStmt() {
    Expr expr = parseExpression();
    consume(TokenType::SEMICOLON, "expected ';' after expression");
    return makeStmt(ExprStmt{ std::move(expr) });
}

// ============================================================
// Expression parsers
// ============================================================

Expr Parser::parseExpression() {
    // Variable declaration: typeName IDENTIFIER ( = expr )?
    if (isTypeName() && peekNext().type == TokenType::IDENTIFIER) {
        Token typeName = advance();
        Token name     = advance();
        std::unique_ptr<Expr> initializer = nullptr;
        if (match({ TokenType::EQUAL })) initializer = box(parseExpression());
        return makeExpr(VarDeclExpr{ typeName, name, std::move(initializer) });
    }
    return parseAssignment();
}

Expr Parser::parseAssignment() {
    // Assignment: IDENTIFIER ( = | += | -= | ... ) assignment
    if (check(TokenType::IDENTIFIER)) {
        TokenType next = peekNext().type;
        bool isAssignOp =
            next == TokenType::EQUAL           ||
            next == TokenType::PLUS_EQUAL      || next == TokenType::MINUS_EQUAL      ||
            next == TokenType::STAR_EQUAL      || next == TokenType::SLASH_EQUAL      ||
            next == TokenType::PERCENT_EQUAL   || next == TokenType::CARET_EQUAL      ||
            next == TokenType::AMPERSAND_EQUAL || next == TokenType::PIPE_EQUAL;

        if (isAssignOp) {
            Token name = advance();
            Token op   = advance();
            Expr value = parseAssignment();    // right-associative
            if (op.type == TokenType::EQUAL)
                return makeExpr(AssignExpr{ name, box(std::move(value)) });
            else
                return makeExpr(CompoundAssignExpr{ name, op, box(std::move(value)) });
        }
    }
    return parseLogicalOr();
}

Expr Parser::parseLogicalOr() {
    Expr left = parseLogicalAnd();
    while (match({ TokenType::OR })) {
        Token op = previous();
        left = makeExpr(BinaryExpr{ box(std::move(left)), op, box(parseLogicalAnd()) });
    }
    return left;
}

Expr Parser::parseLogicalAnd() {
    Expr left = parseBitwiseOr();
    while (match({ TokenType::AND })) {
        Token op = previous();
        left = makeExpr(BinaryExpr{ box(std::move(left)), op, box(parseBitwiseOr()) });
    }
    return left;
}

Expr Parser::parseBitwiseOr() {
    Expr left = parseBitwiseXor();
    while (match({ TokenType::PIPE })) {
        Token op = previous();
        left = makeExpr(BinaryExpr{ box(std::move(left)), op, box(parseBitwiseXor()) });
    }
    return left;
}

Expr Parser::parseBitwiseXor() {
    Expr left = parseBitwiseAnd();
    while (match({ TokenType::CARET })) {
        Token op = previous();
        left = makeExpr(BinaryExpr{ box(std::move(left)), op, box(parseBitwiseAnd()) });
    }
    return left;
}

Expr Parser::parseBitwiseAnd() {
    Expr left = parseEquality();
    while (match({ TokenType::AMPERSAND })) {
        Token op = previous();
        left = makeExpr(BinaryExpr{ box(std::move(left)), op, box(parseEquality()) });
    }
    return left;
}

Expr Parser::parseEquality() {
    Expr left = parseComparison();
    while (match({ TokenType::EQUAL_EQUAL, TokenType::BANG_EQUAL })) {
        Token op = previous();
        left = makeExpr(BinaryExpr{ box(std::move(left)), op, box(parseComparison()) });
    }
    return left;
}

Expr Parser::parseComparison() {
    Expr left = parseShift();
    while (match({ TokenType::LESS, TokenType::LESS_EQUAL,
                   TokenType::GREATER, TokenType::GREATER_EQUAL })) {
        Token op = previous();
        left = makeExpr(BinaryExpr{ box(std::move(left)), op, box(parseShift()) });
    }
    return left;
}

Expr Parser::parseShift() {
    Expr left = parseAddSub();
    while (match({ TokenType::SHIFT_LEFT, TokenType::SHIFT_RIGHT })) {
        Token op = previous();
        left = makeExpr(BinaryExpr{ box(std::move(left)), op, box(parseAddSub()) });
    }
    return left;
}

Expr Parser::parseAddSub() {
    Expr left = parseMulDiv();
    while (match({ TokenType::PLUS, TokenType::MINUS })) {
        Token op = previous();
        left = makeExpr(BinaryExpr{ box(std::move(left)), op, box(parseMulDiv()) });
    }
    return left;
}

Expr Parser::parseMulDiv() {
    Expr left = parseUnary();
    while (match({ TokenType::STAR, TokenType::SLASH, TokenType::PERCENT })) {
        Token op = previous();
        left = makeExpr(BinaryExpr{ box(std::move(left)), op, box(parseUnary()) });
    }
    return left;
}

Expr Parser::parseUnary() {
    if (match({ TokenType::BANG, TokenType::MINUS, TokenType::TILDE,
                TokenType::INCREMENT, TokenType::DECREMENT })) {
        Token op = previous();
        return makeExpr(UnaryExpr{ op, box(parseUnary()) });
    }
    return parsePostfix();
}

Expr Parser::parsePostfix() {
    Expr expr = parsePrimary();
    while (match({ TokenType::INCREMENT, TokenType::DECREMENT })) {
        Token op = previous();
        expr = makeExpr(PostfixExpr{ box(std::move(expr)), op });
    }
    return expr;
}

Expr Parser::parsePrimary() {
    if (match({ TokenType::TRUE, TokenType::FALSE,
                TokenType::NUMBER, TokenType::STRING, TokenType::CHAR })) {
        return makeExpr(LiteralExpr{ previous() });
    }

    if (match({ TokenType::IDENTIFIER })) {
        Token name = previous();
        // Function call: IDENTIFIER ( args )
        if (match({ TokenType::LEFT_PAREN })) {
            std::vector<std::unique_ptr<Expr>> args;
            if (!check(TokenType::RIGHT_PAREN)) {
                do {
                    args.push_back(box(parseExpression()));
                } while (match({ TokenType::COMMA }));
            }
            consume(TokenType::RIGHT_PAREN, "expected ')' after arguments");
            return makeExpr(CallExpr{ name, std::move(args) });
        }
        return makeExpr(IdentifierExpr{ name });
    }

    // Grouping — no AST node, just return the inner expression directly
    if (match({ TokenType::LEFT_PAREN })) {
        Expr inner = parseExpression();
        consume(TokenType::RIGHT_PAREN, "expected ')' after expression");
        return inner;
    }

    throw error(peek(), "expected expression");
}
