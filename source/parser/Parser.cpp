//
// Created by Vladimir Arsenijevic on 28.5.2026.
//

#include "Parser.h"
#include <iostream>

Parser::Parser(std::unordered_set<std::string> initialClassNames)
    : classNames_(std::move(initialClassNames)) {}

Program Parser::parse(const std::vector<Token>& inputTokens) {
    tokens  = std::vector<Token>(inputTokens);
    current = 0;
    // Do NOT clear classNames_ here — pre-registered names from imports must survive.
    // Pre-pass: also register class names defined in THIS file's token stream.
    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (tokens[i].type == TokenType::CLASS && tokens[i + 1].type == TokenType::IDENTIFIER)
            classNames_.insert(tokens[i + 1].lexeme);
    }

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
    std::cerr << "[line " << token.line << "] Error: " << msg << '\n';
    return ParseError("[line " + std::to_string(token.line) + "] Error: " + msg);
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
            case TokenType::BREAK:
            case TokenType::CONTINUE:
            case TokenType::IMPORT:
            case TokenType::EXTERN:
            case TokenType::I8:
            case TokenType::I16:
            case TokenType::I32:
            case TokenType::I64:
            case TokenType::U8:
            case TokenType::U16:
            case TokenType::U32:
            case TokenType::U64:
            case TokenType::F32:
            case TokenType::F64:
            case TokenType::BOOL:
            case TokenType::CHAR_TYPE:
            case TokenType::VOID:
            case TokenType::PTR:
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
        case TokenType::I8:
        case TokenType::I16:
        case TokenType::I32:
        case TokenType::I64:
        case TokenType::U8:
        case TokenType::U16:
        case TokenType::U32:
        case TokenType::U64:
        case TokenType::F32:
        case TokenType::F64:
        case TokenType::BOOL:
        case TokenType::CHAR_TYPE:
        case TokenType::VOID:
        case TokenType::PTR:
            return true;
        case TokenType::IDENTIFIER:
            return classNames_.count(peek().lexeme) > 0;
        default:
            return false;
    }
}

// ============================================================
// Statement parsers
// ============================================================

Stmt Parser::parseDeclaration() {
    // extern returnType name( params );
    if (match({ TokenType::IMPORT })) {
        Token keyword = previous();
        return parseImportStmt(keyword);
    }

    if (match({ TokenType::EXTERN })) {
        Token keyword = previous();
        return parseExternFuncDecl(keyword);
    }

    if (match({ TokenType::CLASS })) {
        return parseClassDecl();
    }

    // Function decl: typeName IDENTIFIER ( ...
    // Exception: if we are inside a function body and the return type is a class
    // name (an IDENTIFIER in classNames_), then "ClassName varName(args)" is a
    // constructor call (VarDecl with ctor initializer), not a function decl.
    // Keyword-typed patterns like "void inner() { ... }" are always function decls.
    if (isTypeName()
        && peekNext().type == TokenType::IDENTIFIER
        && current + 2 < tokens.size()
        && tokens[current + 2].type == TokenType::LEFT_PAREN
        && !(insideFunction_
             && peek().type == TokenType::IDENTIFIER
             && classNames_.count(peek().lexeme))) {
        Token returnType = advance();
        Token name       = advance();
        return parseFunctionDecl(returnType, name);
    }
    return parseStatement();
}

Stmt Parser::parseExternFuncDecl(const Token& keyword) {
    if (!isTypeName())
        throw error(peek(), "expected return type after 'extern'");
    Token returnType = advance();
    Token name       = consume(TokenType::IDENTIFIER, "expected function name after return type");

    consume(TokenType::LEFT_PAREN, "expected '(' after function name");
    std::vector<ParamDecl> params;
    if (!check(TokenType::RIGHT_PAREN)) {
        do {
            if (!isTypeName()) throw error(peek(), "expected parameter type");
            Token paramType = advance();
            Token paramName = consume(TokenType::IDENTIFIER, "expected parameter name");
            params.push_back(ParamDecl{ paramType, paramName });
        } while (match({ TokenType::COMMA }));
    }
    consume(TokenType::RIGHT_PAREN, "expected ')' after parameters");
    consume(TokenType::SEMICOLON,   "expected ';' after extern declaration");

    return makeStmt(ExternFuncDeclStmt{ keyword, returnType, name, std::move(params) });
}

Stmt Parser::parseImportStmt(const Token& keyword) {
    Token path = consume(TokenType::STRING, "expected file path string after 'import'");
    consume(TokenType::SEMICOLON, "expected ';' after import path");
    return makeStmt(ImportStmt{ keyword, path });
}

Stmt Parser::parseFunctionDecl(const Token& returnType, const Token& name) {
    consume(TokenType::LEFT_PAREN, "expected '(' after function name");

    std::vector<ParamDecl> params;
    if (!check(TokenType::RIGHT_PAREN)) {
        do {
            if (!isTypeName()) throw error(peek(), "expected parameter type");
            Token paramType = advance();
            Token paramName = consume(TokenType::IDENTIFIER, "expected parameter name");
            params.push_back(ParamDecl{ paramType, paramName });
        } while (match({ TokenType::COMMA }));
    }
    consume(TokenType::RIGHT_PAREN, "expected ')' after parameters");
    consume(TokenType::LEFT_BRACE,  "expected '{' before function body");

    bool savedInsideFunction = insideFunction_;
    insideFunction_ = true;
    BlockStmt body = parseBlockBody();
    insideFunction_ = savedInsideFunction;
    return makeStmt(FunctionDeclStmt{ returnType, name, std::move(params), std::move(body) });
}

Stmt Parser::parseClassDecl() {
    Token name = consume(TokenType::IDENTIFIER, "expected class name after 'class'");
    consume(TokenType::LEFT_BRACE, "expected '{' after class name");

    std::vector<FieldDecl> fields;
    std::deque<MethodDecl> methods;

    while (!check(TokenType::RIGHT_BRACE) && !isAtEnd()) {
        // Each member starts with public/private
        bool isPublic = true;
        if (match({ TokenType::PUBLIC })) {
            isPublic = true;
        } else if (match({ TokenType::PRIVATE })) {
            isPublic = false;
        } else {
            throw error(peek(), "expected 'public' or 'private' before class member");
        }

        // Destructor: ~ClassName()
        if (check(TokenType::TILDE)
            && current + 1 < tokens.size()
            && tokens[current + 1].type == TokenType::IDENTIFIER
            && tokens[current + 1].lexeme == name.lexeme
            && current + 2 < tokens.size()
            && tokens[current + 2].type == TokenType::LEFT_PAREN) {

            advance();                          // consume '~'
            Token dtorName = advance();         // consume ClassName
            consume(TokenType::LEFT_PAREN,  "expected '(' after destructor name");
            consume(TokenType::RIGHT_PAREN, "expected ')' — destructor takes no parameters");
            consume(TokenType::LEFT_BRACE,  "expected '{' before destructor body");
            bool savedDtor = insideFunction_;
            insideFunction_ = true;
            BlockStmt dtorBody = parseBlockBody();
            insideFunction_ = savedDtor;

            methods.push_back(MethodDecl{
                isPublic, /*isConstructor=*/false, /*isDestructor=*/true,
                dtorName, dtorName, {}, std::move(dtorBody)
            });
            continue;
        }

        // Constructor: IDENTIFIER (== class name) followed by '('
        if (check(TokenType::IDENTIFIER) && peek().lexeme == name.lexeme
            && current + 1 < tokens.size() && tokens[current + 1].type == TokenType::LEFT_PAREN) {
            Token ctorName = advance();  // consume class name
            consume(TokenType::LEFT_PAREN, "expected '(' after constructor name");

            std::vector<ParamDecl> params;
            if (!check(TokenType::RIGHT_PAREN)) {
                do {
                    if (!isTypeName()) throw error(peek(), "expected parameter type");
                    Token paramType = advance();
                    Token paramName = consume(TokenType::IDENTIFIER, "expected parameter name");
                    params.push_back(ParamDecl{ paramType, paramName });
                } while (match({ TokenType::COMMA }));
            }
            consume(TokenType::RIGHT_PAREN, "expected ')' after constructor parameters");
            consume(TokenType::LEFT_BRACE,  "expected '{' before constructor body");
            bool savedIF1 = insideFunction_;
            insideFunction_ = true;
            BlockStmt body = parseBlockBody();
            insideFunction_ = savedIF1;

            methods.push_back(MethodDecl{
                isPublic, /*isConstructor=*/true, /*isDestructor=*/false,
                ctorName,   // returnType token = class name token (no actual return type)
                ctorName,   // name token
                std::move(params), std::move(body)
            });
            continue;
        }

        // Regular method or field: typeName IDENTIFIER
        if (!isTypeName()) throw error(peek(), "expected type name for class member");
        Token memberType = advance();
        Token memberName = consume(TokenType::IDENTIFIER, "expected member name");

        if (check(TokenType::LEFT_PAREN)) {
            // Method
            consume(TokenType::LEFT_PAREN, "");
            std::vector<ParamDecl> params;
            if (!check(TokenType::RIGHT_PAREN)) {
                do {
                    if (!isTypeName()) throw error(peek(), "expected parameter type");
                    Token paramType = advance();
                    Token paramName = consume(TokenType::IDENTIFIER, "expected parameter name");
                    params.push_back(ParamDecl{ paramType, paramName });
                } while (match({ TokenType::COMMA }));
            }
            consume(TokenType::RIGHT_PAREN, "expected ')' after method parameters");
            consume(TokenType::LEFT_BRACE,  "expected '{' before method body");
            bool savedIF2 = insideFunction_;
            insideFunction_ = true;
            BlockStmt body = parseBlockBody();
            insideFunction_ = savedIF2;

            methods.push_back(MethodDecl{
                isPublic, /*isConstructor=*/false, /*isDestructor=*/false,
                memberType, memberName,
                std::move(params), std::move(body)
            });
        } else {
            // Field
            consume(TokenType::SEMICOLON, "expected ';' after field declaration");
            fields.push_back(FieldDecl{ isPublic, memberType, memberName });
        }
    }

    consume(TokenType::RIGHT_BRACE, "expected '}' after class body");
    return makeStmt(ClassDeclStmt{ name, std::move(fields), std::move(methods) });
}

Stmt Parser::parseStatement() {
    if (check(TokenType::LEFT_BRACE))   return parseBlock();
    if (match({ TokenType::IF     }))   return parseIfStmt();
    if (match({ TokenType::WHILE  }))   return parseWhileStmt();
    if (match({ TokenType::FOR    }))   return parseForStmt();
    if (match({ TokenType::RETURN }))   return parseReturnStmt();
    if (match({ TokenType::BREAK }))    return parseBreakStmt();
    if (match({ TokenType::CONTINUE })) return parseContinueStmt();
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

Stmt Parser::parseExprStmt() {
    Expr expression = parseExpression();
    consume(TokenType::SEMICOLON, "expected ';' after expression");
    return makeStmt(ExprStmt{ std::move(expression) });
}

// ============================================================
// Expression parsers
// ============================================================

Expr Parser::parseExpression() {
    // Array declaration: typeName [ NUMBER ] IDENTIFIER ( = expr )?
    if (isTypeName() && peekNext().type == TokenType::LEFT_BRACKET) {
        Token  typeName  = advance();
        consume(TokenType::LEFT_BRACKET, "expected '[' after type name");
        Token  sizeToken = consume(TokenType::NUMBER, "expected integer array size");
        size_t arraySize = std::stoull(sizeToken.lexeme);
        consume(TokenType::RIGHT_BRACKET, "expected ']' after array size");
        Token  name      = consume(TokenType::IDENTIFIER, "expected variable name after array type");
        std::unique_ptr<Expr> initializer = nullptr;
        if (match({ TokenType::EQUAL })) initializer = box(parseExpression());
        return makeExpr(VarDeclExpr{ typeName, name, std::move(initializer), arraySize });
    }

    // Scalar variable declaration: typeName IDENTIFIER ( = expr | (args) )?
    if (isTypeName() && peekNext().type == TokenType::IDENTIFIER) {
        Token typeName = advance();
        Token name     = advance();
        std::unique_ptr<Expr> initializer = nullptr;
        if (match({ TokenType::EQUAL })) {
            initializer = box(parseExpression());
        } else if (check(TokenType::LEFT_PAREN)
                   && classNames_.count(typeName.lexeme) > 0) {
            // Constructor call syntax: ClassName varName(args)
            advance();  // consume '('
            std::vector<std::unique_ptr<Expr>> args;
            if (!check(TokenType::RIGHT_PAREN)) {
                do { args.push_back(box(parseExpression())); } while (match({ TokenType::COMMA }));
            }
            consume(TokenType::RIGHT_PAREN, "expected ')' after constructor arguments");
            // Store as a CallExpr whose callee lexeme == class name — semantic pass detects this
            initializer = box(makeExpr(CallExpr{ typeName, std::move(args) }));
        }
        return makeExpr(VarDeclExpr{ typeName, name, std::move(initializer) });
    }
    return parseAssignment();
}

Expr Parser::parseAssignment() {
    // Assignment: IDENTIFIER ( = | += | -= | ... ) assignment
    if (check(TokenType::IDENTIFIER)) {
        TokenType nextTokenType = peekNext().type;
        bool isAssignmentOperator =
            nextTokenType == TokenType::EQUAL           ||
            nextTokenType == TokenType::PLUS_EQUAL      || nextTokenType == TokenType::MINUS_EQUAL      ||
            nextTokenType == TokenType::STAR_EQUAL      || nextTokenType == TokenType::SLASH_EQUAL      ||
            nextTokenType == TokenType::PERCENT_EQUAL   || nextTokenType == TokenType::CARET_EQUAL      ||
            nextTokenType == TokenType::AMPERSAND_EQUAL || nextTokenType == TokenType::PIPE_EQUAL;

        if (isAssignmentOperator) {
            Token name          = advance();
            Token operatorToken = advance();
            Expr value = parseAssignment();    // right-associative
            if (operatorToken.type == TokenType::EQUAL)
                return makeExpr(AssignExpr{ name, box(std::move(value)) });
            else
                return makeExpr(CompoundAssignExpr{ name, operatorToken, box(std::move(value)) });
        }
    }
    Expr expression = parseLogicalOr();

    // Indexed assignment: arr[i] = expr (detected after parsing the LHS)
    if (expression.node && std::holds_alternative<IndexExpr>(*expression.node)
        && check(TokenType::EQUAL)) {
        advance();  // consume =
        auto indexNode = std::move(std::get<IndexExpr>(*expression.node));
        Expr value = parseAssignment();  // right-associative
        return makeExpr(IndexAssignExpr{
            indexNode.name,
            std::move(indexNode.index),
            box(std::move(value))
        });
    }

    // Member assignment: obj.field = expr (detected after parsing obj.field LHS)
    if (expression.node && std::holds_alternative<MemberAccessExpr>(*expression.node)
        && check(TokenType::EQUAL)) {
        advance();  // consume =
        auto& ma = std::get<MemberAccessExpr>(*expression.node);
        Expr value = parseAssignment();  // right-associative
        return makeExpr(MemberAssignExpr{
            std::move(ma.object), ma.field, box(std::move(value))
        });
    }

    return expression;
}

Expr Parser::parseLogicalOr() {
    Expr left = parseLogicalAnd();
    while (match({ TokenType::OR })) {
        Token operatorToken = previous();
        left = makeExpr(BinaryExpr{ box(std::move(left)), operatorToken, box(parseLogicalAnd()) });
    }
    return left;
}

Expr Parser::parseLogicalAnd() {
    Expr left = parseBitwiseOr();
    while (match({ TokenType::AND })) {
        Token operatorToken = previous();
        left = makeExpr(BinaryExpr{ box(std::move(left)), operatorToken, box(parseBitwiseOr()) });
    }
    return left;
}

Expr Parser::parseBitwiseOr() {
    Expr left = parseBitwiseXor();
    while (match({ TokenType::PIPE })) {
        Token operatorToken = previous();
        left = makeExpr(BinaryExpr{ box(std::move(left)), operatorToken, box(parseBitwiseXor()) });
    }
    return left;
}

Expr Parser::parseBitwiseXor() {
    Expr left = parseBitwiseAnd();
    while (match({ TokenType::CARET })) {
        Token operatorToken = previous();
        left = makeExpr(BinaryExpr{ box(std::move(left)), operatorToken, box(parseBitwiseAnd()) });
    }
    return left;
}

Expr Parser::parseBitwiseAnd() {
    Expr left = parseEquality();
    while (match({ TokenType::AMPERSAND })) {
        Token operatorToken = previous();
        left = makeExpr(BinaryExpr{ box(std::move(left)), operatorToken, box(parseEquality()) });
    }
    return left;
}

Expr Parser::parseEquality() {
    Expr left = parseComparison();
    while (match({ TokenType::EQUAL_EQUAL, TokenType::BANG_EQUAL })) {
        Token operatorToken = previous();
        left = makeExpr(BinaryExpr{ box(std::move(left)), operatorToken, box(parseComparison()) });
    }
    return left;
}

Expr Parser::parseComparison() {
    Expr left = parseShift();
    while (match({ TokenType::LESS, TokenType::LESS_EQUAL,
                   TokenType::GREATER, TokenType::GREATER_EQUAL })) {
        Token operatorToken = previous();
        left = makeExpr(BinaryExpr{ box(std::move(left)), operatorToken, box(parseShift()) });
    }
    return left;
}

Expr Parser::parseShift() {
    Expr left = parseAddSub();
    while (match({ TokenType::SHIFT_LEFT, TokenType::SHIFT_RIGHT })) {
        Token operatorToken = previous();
        left = makeExpr(BinaryExpr{ box(std::move(left)), operatorToken, box(parseAddSub()) });
    }
    return left;
}

Expr Parser::parseAddSub() {
    Expr left = parseMulDiv();
    while (match({ TokenType::PLUS, TokenType::MINUS })) {
        Token operatorToken = previous();
        left = makeExpr(BinaryExpr{ box(std::move(left)), operatorToken, box(parseMulDiv()) });
    }
    return left;
}

Expr Parser::parseMulDiv() {
    Expr left = parseUnary();
    while (match({ TokenType::STAR, TokenType::SLASH, TokenType::PERCENT })) {
        Token operatorToken = previous();
        left = makeExpr(BinaryExpr{ box(std::move(left)), operatorToken, box(parseUnary()) });
    }
    return left;
}

Expr Parser::parseUnary() {
    if (match({ TokenType::BANG, TokenType::MINUS, TokenType::TILDE,
                TokenType::INCREMENT, TokenType::DECREMENT })) {
        Token operatorToken = previous();
        return makeExpr(UnaryExpr{ operatorToken, box(parseUnary()) });
    }
    return parsePostfix();
}

Expr Parser::parsePostfix() {
    Expr expression = parsePrimary();
    for (;;) {
        // Subscript access: identifier[index]
        if (check(TokenType::LEFT_BRACKET)
            && expression.node
            && std::holds_alternative<IdentifierExpr>(*expression.node)) {
            advance();  // consume [
            Expr indexExpr = parseExpression();
            consume(TokenType::RIGHT_BRACKET, "expected ']' after array index");
            Token arrayName = std::get<IdentifierExpr>(*expression.node).name;
            return makeExpr(IndexExpr{ arrayName, box(std::move(indexExpr)) });
        }
        // Postfix ++ and --
        if (match({ TokenType::INCREMENT, TokenType::DECREMENT })) {
            expression = makeExpr(PostfixExpr{ box(std::move(expression)), previous() });
            continue;
        }
        // Member access / method call: expr.member or expr.method(args)
        if (check(TokenType::DOT)) {
            advance();  // consume '.'
            Token member = consume(TokenType::IDENTIFIER, "expected member name after '.'");
            if (check(TokenType::LEFT_PAREN)) {
                advance();  // consume '('
                std::vector<std::unique_ptr<Expr>> args;
                if (!check(TokenType::RIGHT_PAREN)) {
                    do {
                        args.push_back(box(parseExpression()));
                    } while (match({ TokenType::COMMA }));
                }
                consume(TokenType::RIGHT_PAREN, "expected ')' after method arguments");
                expression = makeExpr(MethodCallExpr{
                    box(std::move(expression)), member, std::move(args)
                });
            } else {
                expression = makeExpr(MemberAccessExpr{
                    box(std::move(expression)), member
                });
            }
            continue;
        }
        break;
    }
    return expression;
}

Expr Parser::parsePrimary() {
    if (match({ TokenType::THIS })) {
        return makeExpr(ThisExpr{ previous() });
    }

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
