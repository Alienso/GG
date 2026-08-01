//
// Created by Vladimir Arsenijevic on 28.5.2026.
//

#include "Parser.h"


Stmt Parser::parseIfStmt() {
    consume(TokenType::LEFT_PAREN, "expected '(' after 'if'");
    Expr condition = parseExpression();
    consume(TokenType::RIGHT_PAREN, "expected ')' after if condition");

    Stmt thenBranch = parseStatement();   // block or single statement

    std::unique_ptr<Stmt> elseBranch = nullptr;
    if (match({ TokenType::ELSE }))
        elseBranch = box(parseStatement());

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

Stmt Parser::parseBreakStmt() {
    Token keyword = previous();
    consume(TokenType::SEMICOLON, "expected ';' after 'break'");
    return makeStmt(BreakStmt{ keyword });
}

Stmt Parser::parseContinueStmt() {
    Token keyword = previous();
    consume(TokenType::SEMICOLON, "expected ';' after 'continue'");
    return makeStmt(ContinueStmt{ keyword });
}

Stmt Parser::parseSwitchStmt() {
    Token keyword = previous();   // 'switch'
    consume(TokenType::LEFT_PAREN, "expected '(' after 'switch'");
    Expr scrutinee = parseExpression();
    consume(TokenType::RIGHT_PAREN, "expected ')' after switch scrutinee");
    std::deque<SwitchArm> arms = parseSwitchArmBlock();
    return makeStmt(SwitchStmt{ keyword, std::move(scrutinee), std::move(arms) });
}

Expr Parser::parseSwitchExpr() {
    Token keyword = previous();   // 'switch' (consumed by the caller)
    consume(TokenType::LEFT_PAREN, "expected '(' after 'switch'");
    Expr scrutinee = parseExpression();
    consume(TokenType::RIGHT_PAREN, "expected ')' after switch scrutinee");
    std::deque<SwitchArm> arms = parseSwitchArmBlock();
    return makeExpr(SwitchExpr{ keyword, box(std::move(scrutinee)), std::move(arms) });
}

// Shared by the statement and expression forms. Parses `{ (case ... | default) -> ... }`.
std::deque<SwitchArm> Parser::parseSwitchArmBlock() {
    consume(TokenType::LEFT_BRACE, "expected '{' to open switch body");
    std::deque<SwitchArm> arms;
    bool sawDefault = false;
    while (!check(TokenType::RIGHT_BRACE) && !isAtEnd()) {
        std::vector<std::unique_ptr<Expr>> labels;
        bool isDefault = false;
        if (match({ TokenType::DEFAULT })) {
            if (sawDefault) throw error(previous(), "a switch may have at most one 'default' arm");
            sawDefault = true;
            isDefault  = true;
        } else {
            consume(TokenType::CASE, "expected 'case' or 'default' in switch body");
            parsingCaseLabel_ = true;   // a trailing `->` here is the arm separator, not a lambda
            labels.push_back(box(parseExpression()));
            while (match({ TokenType::COMMA }))
                labels.push_back(box(parseExpression()));
            parsingCaseLabel_ = false;
        }
        Token arrow = consume(TokenType::ARROW, "expected '->' after switch case label");

        std::unique_ptr<Expr> valueExpr = nullptr;
        std::unique_ptr<Stmt> block     = nullptr;
        if (check(TokenType::LEFT_BRACE)) {
            block = box(parseBlock());
        } else {
            valueExpr = box(parseExpression());
            consume(TokenType::SEMICOLON, "expected ';' after switch arm expression");
        }
        arms.push_back(SwitchArm{ std::move(labels), isDefault,
                                  std::move(valueExpr), std::move(block), arrow });
    }
    consume(TokenType::RIGHT_BRACE, "expected '}' to close switch body");
    return arms;
}

Stmt Parser::parseYieldStmt() {
    Token keyword = previous();   // 'yield'
    Expr value = parseExpression();
    consume(TokenType::SEMICOLON, "expected ';' after yield value");
    return makeStmt(YieldStmt{ keyword, std::move(value) });
}

Stmt Parser::parseExprStmt() {
    Expr expression = parseExpression();
    consume(TokenType::SEMICOLON, "expected ';' after expression");
    return makeStmt(ExprStmt{ std::move(expression) });
}