//
// Created by Vladimir Arsenijevic on 28.5.2026.
//

#include "Parser.h"
#include <iostream>


bool Parser::isLambdaAhead() const {
    if (parsingCaseLabel_) return false;   // inside a case label, `( … ) ->` is not a lambda
    if (!check(TokenType::LEFT_PAREN)) return false;
    int depth = 0;
    size_t k = current;
    for (; k < tokens.size(); ++k) {
        if (tokens[k].type == TokenType::LEFT_PAREN)       depth++;
        else if (tokens[k].type == TokenType::RIGHT_PAREN) { if (--depth == 0) { ++k; break; } }
    }
    return k < tokens.size() && tokens[k].type == TokenType::ARROW;
}

std::vector<std::pair<Token, std::optional<Token>>> Parser::parseLambdaParamList() {
    consume(TokenType::LEFT_PAREN, "expected '(' in lambda parameters");
    std::vector<std::pair<Token, std::optional<Token>>> params;
    if (!check(TokenType::RIGHT_PAREN)) {
        do {
            // Typed `Type name` vs untyped `name` (type inferred from the expected signature).
            size_t span = typeSpanAt(current);
            if (span > 0 && current + span < tokens.size()
                && tokens[current + span].type == TokenType::IDENTIFIER) {
                Token type = consumeType();
                Token name = consume(TokenType::IDENTIFIER, "expected parameter name");
                params.emplace_back(name, std::optional<Token>(type));
            } else {
                Token name = consume(TokenType::IDENTIFIER, "expected lambda parameter name");
                params.emplace_back(name, std::nullopt);
            }
        } while (match({ TokenType::COMMA }));
    }
    consume(TokenType::RIGHT_PAREN, "expected ')' after lambda parameters");
    return params;
}

Expr Parser::parseLambda() {
    int line = peek().line;
    return finishLambda(parseLambdaParamList(), line);
}

Expr Parser::finishLambda(std::vector<std::pair<Token, std::optional<Token>>> params, int line) {
    if (capturing_) throw error(peek(), "nested lambdas are not supported");
    consume(TokenType::ARROW, "expected '->' in lambda");
    // Optional explicit return type; otherwise inferred from the expected signature (else void).
    std::optional<Token> explicitRet;
    if (isTypeName()) explicitRet.emplace(consumeType());

    // Resolve parameter types: explicit where written, otherwise from the callee's `Call(…)` bound.
    bool anyUntyped = false;
    for (const auto& p : params) if (!p.second) { anyUntyped = true; break; }
    if (anyUntyped && !expectedLambdaSig_)
        throw error(params.empty() ? peek() : params.front().first,
                    "cannot infer this lambda's parameter types here; annotate them "
                    "(e.g. (i32 x) -> …) or pass the lambda to a Call-bounded function");
    if ((anyUntyped || !explicitRet) && expectedLambdaSig_
        && expectedLambdaSig_->first.size() != params.size())
        throw error(params.empty() ? peek() : params.front().first,
                    "lambda has " + std::to_string(params.size())
                    + " parameter(s) but the expected signature has "
                    + std::to_string(expectedLambdaSig_->first.size()));

    std::vector<ParamDecl> methodParams;
    for (size_t i = 0; i < params.size(); ++i) {
        Token ptype = params[i].second ? *params[i].second : expectedLambdaSig_->first[i];
        methodParams.push_back(ParamDecl{ ptype, params[i].first, /*isMut=*/false, nullptr });
    }
    Token retType = explicitRet ? *explicitRet
                  : (expectedLambdaSig_ ? expectedLambdaSig_->second
                                        : Token{ TokenType::VOID, "void", line });

    consume(TokenType::LEFT_BRACE, "expected '{' before lambda body");

    // Seed the body scope with the lambda's parameters, then capture free variables by value.
    pendingScopeSeed_.clear();
    for (const ParamDecl& p : methodParams) pendingScopeSeed_.emplace_back(p.name.lexeme, p.typeName);
    capturing_   = true;
    captureBase_ = scopes_.size();   // the body scope parseBlockBody pushes is the lambda's own
    captures_.clear();
    BlockStmt body = parseBlockBody();
    std::vector<std::pair<std::string, Token>> caps = std::move(captures_);
    capturing_ = false;

    std::string lname = "__lambda_" + std::to_string(gen_->lambdaCounter++);
    classNames.insert(lname);
    gen_->lambdaClassNames.insert(lname);   // shared, so monomorphization's re-parse recognizes it
    Token lnameTok{ TokenType::IDENTIFIER, lname, line };

    // class __lambda_N { <capture fields>; __lambda_N(<caps>) { this.cap = cap; … } }
    std::deque<FieldDecl> fields;
    for (const auto& [cn, ct] : caps)
        fields.push_back(FieldDecl{ /*isPublic=*/true, /*isStatic=*/false, /*isMut=*/false,
                                    ct, Token{ TokenType::IDENTIFIER, cn, line }, nullptr });

    std::deque<MethodDecl> classMethods;
    if (!caps.empty()) {
        std::vector<ParamDecl> ctorParams;
        std::vector<std::unique_ptr<Stmt>> ctorBody;
        for (const auto& [cn, ct] : caps) {
            ctorParams.push_back(ParamDecl{ ct, Token{ TokenType::IDENTIFIER, cn, line }, false, nullptr });
            // this.cap = cap;
            ctorBody.push_back(std::make_unique<Stmt>(makeStmt(ExprStmt{ makeExpr(MemberAssignExpr{
                box(makeExpr(ThisExpr{ Token{ TokenType::THIS, "this", line } })),
                Token{ TokenType::IDENTIFIER, cn, line },
                box(makeExpr(IdentifierExpr{ Token{ TokenType::IDENTIFIER, cn, line } })) }) })));
        }
        classMethods.push_back(MethodDecl{
            /*isPublic=*/true, /*isConstructor=*/true, /*isDestructor=*/false, /*isStatic=*/false,
            /*isMut=*/false, /*hasBody=*/true, lnameTok /*ctor ret = class name*/,
            Token{ TokenType::IDENTIFIER, lname, line }, std::move(ctorParams),
            BlockStmt{ std::move(ctorBody) }, /*hasReturnSlot=*/false, "" });
    }
    pendingLambdaDecls_.push_back(makeStmt(ClassDeclStmt{ lnameTok, std::move(fields), std::move(classMethods) }));

    // impl Call for __lambda_N { fn call(<params>) -> <ret> { <body> } }  (canonicalized trait)
    std::vector<Token> paramTypes;
    for (const ParamDecl& p : methodParams) paramTypes.push_back(p.typeName);
    std::string canon = canonicalCallTrait(paramTypes, retType);
    std::deque<MethodDecl> implMethods;
    implMethods.push_back(MethodDecl{
        /*isPublic=*/true, /*isConstructor=*/false, /*isDestructor=*/false, /*isStatic=*/false,
        /*isMut=*/false, /*hasBody=*/true, retType,
        Token{ TokenType::IDENTIFIER, "call", line },
        std::move(methodParams), std::move(body), /*hasReturnSlot=*/false, "" });
    pendingLambdaDecls_.push_back(makeStmt(ImplDeclStmt{
        Token{ TokenType::IDENTIFIER, canon, line }, lnameTok, std::move(implMethods) }));

    // Replace the lambda with `__lambda_N(capturedVars…)` — a stack value object.
    std::vector<std::unique_ptr<Expr>> ctorArgs;
    for (const auto& [cn, ct] : caps)
        ctorArgs.push_back(box(makeExpr(IdentifierExpr{ Token{ TokenType::IDENTIFIER, cn, line } })));
    return makeExpr(CallExpr{ lnameTok, std::move(ctorArgs) });
}

// ============================================================
// Expression parsers
// ============================================================

Expr Parser::parseExpression() {
    // Leading declaration modifiers: `static` (C-style static local) and `mut`
    // (reassignable binding). Either order is accepted; both unambiguously
    // introduce a VarDeclExpr.
    bool isStatic = false;
    bool isMut    = false;
    for (;;) {
        if (!isStatic && match({ TokenType::STATIC })) { isStatic = true; continue; }
        if (!isMut    && match({ TokenType::MUT    })) { isMut    = true; continue; }
        break;
    }

    // Inferred local: `var name = expr;`  (const by default; `mut var` / `var mut` for mutable).
    // The type is left as a `var` sentinel token and deduced from the initializer by the semantic
    // analyzer, which records the synthesized type token for codegen. An initializer is required —
    // there is nothing to infer from otherwise.
    if (match({ TokenType::VAR })) {
        Token varTok = previous();
        if (!isMut) isMut = match({ TokenType::MUT });   // accept `var mut` as well as `mut var`
        Token name = consume(TokenType::IDENTIFIER, "expected variable name after 'var'");
        consume(TokenType::EQUAL, "an inferred 'var' declaration requires an initializer");
        std::unique_ptr<Expr> initializer = box(parseExpression());
        recordLocal(varTok, name);
        return makeExpr(VarDeclExpr{ varTok, name, std::move(initializer),
                                     /*arraySize=*/0, isStatic, isMut });
    }

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
        recordLocal(typeName, name);
        return makeExpr(VarDeclExpr{ typeName, name, std::move(initializer), arraySize, isStatic, isMut });
    }

    // Variable declaration of any type form: <type> IDENTIFIER ...
    //   where <type> is  Base | Base& | Vec<args> | Vec<args>&
    size_t declSpan = typeSpanAt(current);
    if (declSpan > 0 && current + declSpan < tokens.size()
        && tokens[current + declSpan].type == TokenType::IDENTIFIER) {
        Token typeName = consumeType();   // consumes <args> and/or trailing '&'
        Token name     = advance();
        std::unique_ptr<Expr> initializer = nullptr;
        if (match({ TokenType::EQUAL })) {
            initializer = box(parseExpression());
        } else if ((check(TokenType::LEFT_PAREN) || check(TokenType::LEFT_BRACE))
                   && classNames.count(typeName.lexeme) > 0) {
            // Constructor call syntax: ClassName varName(args)  or  ClassName varName{args}.
            // Braces are an alternate delimiter for the same positional constructor call.
            bool      brace = check(TokenType::LEFT_BRACE);
            TokenType close = brace ? TokenType::RIGHT_BRACE : TokenType::RIGHT_PAREN;
            advance();  // consume '(' or '{'
            // Braces stay positional; `ClassName v(...)` accepts named arguments.
            std::vector<Token> argNames;
            std::vector<std::unique_ptr<Expr>> args = parseCallArgs(argNames, close, /*allowNames=*/!brace);
            // Store as a CallExpr whose callee lexeme == class name — semantic pass detects this
            initializer = box(makeExpr(CallExpr{ typeName, std::move(args), std::move(argNames) }));
        }
        recordLocal(typeName, name);
        return makeExpr(VarDeclExpr{ typeName, name, std::move(initializer), /*arraySize=*/0, isStatic, isMut });
    }

    if (isStatic || isMut)
        throw error(peek(), "expected a variable declaration after '"
                    + std::string(isStatic ? "static" : "mut") + "'");
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
    Expr expression = parseElvis();

    // Indexed assignment: arr[i] = expr (detected after parsing the LHS)
    if (expression.node && std::holds_alternative<IndexExpr>(*expression.node)
        && check(TokenType::EQUAL)) {
        advance();  // consume =
        auto indexNode = std::move(std::get<IndexExpr>(*expression.node));
        Expr value = parseAssignment();  // right-associative
        return makeExpr(IndexAssignExpr{
            std::move(indexNode.object),
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

    // Store through a reference-valued expression: `<expr> = value` where <expr> is not a plain
    // name/index/member (e.g. a call returning `ref T` — `v.at(i) = x`). Semantic analysis verifies
    // the target evaluates to a reference/borrow and stores the value into the referent.
    if (expression.node && check(TokenType::EQUAL)) {
        Token op = advance();  // consume =
        Expr value = parseAssignment();  // right-associative
        return makeExpr(RefStoreExpr{ box(std::move(expression)), op, box(std::move(value)) });
    }

    return expression;
}

// Elvis `a ?: b` — sits just below assignment, above logical-or; right-associative.
Expr Parser::parseElvis() {
    Expr left = parseLogicalOr();
    if (match({ TokenType::QUESTION_COLON })) {
        Token op = previous();
        Expr right = parseElvis();   // right-associative: a ?: b ?: c == a ?: (b ?: c)
        return makeExpr(ElvisExpr{ box(std::move(left)), op, box(std::move(right)) });
    }
    return left;
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
    Expr left = parseCast();
    while (match({ TokenType::STAR, TokenType::SLASH, TokenType::PERCENT })) {
        Token operatorToken = previous();
        left = makeExpr(BinaryExpr{ box(std::move(left)), operatorToken, box(parseCast()) });
    }
    return left;
}

Expr Parser::parseCast() {
    Expr expr = parseUnary();
    while (check(TokenType::AS)) {
        advance();  // consume 'as'
        bool isMut = match({ TokenType::MUT });   // `expr as mut T` — mutable reference view
        if (!isTypeName()) throw error(peek(), "expected type name after 'as'");
        Token targetType = consumeType();          // supports Class& / ptr<T> / generics
        expr = makeExpr(CastExpr{ box(std::move(expr)), targetType, isMut });
    }
    return expr;
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
        // Subscript access on any expression: expr[index]  (arrays, ptr<T>, this.field, …)
        if (check(TokenType::LEFT_BRACKET)) {
            advance();  // consume [
            Expr indexExpr = parseExpression();
            consume(TokenType::RIGHT_BRACKET, "expected ']' after index");
            expression = makeExpr(IndexExpr{ box(std::move(expression)), box(std::move(indexExpr)) });
            continue;
        }
        // Postfix ++ and --
        if (match({ TokenType::INCREMENT, TokenType::DECREMENT })) {
            expression = makeExpr(PostfixExpr{ box(std::move(expression)), previous() });
            continue;
        }
        // Non-null assertion: `x!!` — unwrap a nullable, aborting if null. Recognised as two
        // consecutive `!` AFTER an expression (prefix `!!x` is parsed by parseUnary as double-NOT
        // and never reaches here).
        if (check(TokenType::BANG) && peekNext().type == TokenType::BANG) {
            Token op = advance();   // first '!'
            advance();              // second '!'
            expression = makeExpr(UnwrapExpr{ box(std::move(expression)), op });
            continue;
        }
        // Member access / call via `.`, `?.` (safe), or `::` (static scope resolution).
        // Lowered to the same nodes; `?.` sets the `safe` flag (null-propagating).
        if (check(TokenType::DOT) || check(TokenType::QUESTION_DOT) || check(TokenType::COLON_COLON)) {
            bool safe = check(TokenType::QUESTION_DOT);
            advance();  // consume '.', '?.' or '::'
            Token member = consume(TokenType::IDENTIFIER, "expected member name after '.'");
            if (check(TokenType::LEFT_PAREN)) {
                advance();  // consume '('
                std::vector<Token> argNames;
                std::vector<std::unique_ptr<Expr>> args =
                    parseCallArgs(argNames, TokenType::RIGHT_PAREN, /*allowNames=*/true);
                expression = makeExpr(MethodCallExpr{
                    box(std::move(expression)), member, std::move(args), std::move(argNames), safe
                });
            } else {
                expression = makeExpr(MemberAccessExpr{
                    box(std::move(expression)), member, safe
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

    // The null literal — typed against the expected `T?` in the semantic pass.
    if (match({ TokenType::NULL_LITERAL })) {
        return makeExpr(NullLiteralExpr{ previous() });
    }

    // Switch expression: `switch (x) { case ... -> v; default -> v; }`
    if (match({ TokenType::SWITCH })) {
        return parseSwitchExpr();
    }

    // Lambda literal: `( … ) -> …` (distinguished from a grouped expression by the trailing `->`).
    if (isLambdaAhead()) {
        return parseLambda();
    }

    // Heap allocation operator: new ClassName(args)  /  new Vec<args>(args)
    if (match({ TokenType::NEW })) {
        Token keyword = previous();
        if (!check(TokenType::IDENTIFIER))
            throw error(peek(), "expected class name after 'new'");
        Token rawName  = advance();
        std::string clsLex = rawName.lexeme;
        if (gen_->classNames.count(rawName.lexeme) && check(TokenType::LESS)) {
            std::vector<std::vector<Token>> typeArgs = parseTypeArgList();
            std::string mangled = mangleInstantiation(rawName.lexeme, typeArgs);
            recordInstantiation(rawName.lexeme, mangled, std::move(typeArgs));
            classNames.insert(mangled);
            clsLex = mangled;
        } else if (classNames.count(rawName.lexeme) == 0) {
            throw error(rawName, "expected class name after 'new'");
        }
        Token className = Token{ TokenType::IDENTIFIER, clsLex, rawName.line };  // construct, not assign
        // `new Point(args)` or `new Point{args}` — braces are an alternate delimiter.
        bool      brace = check(TokenType::LEFT_BRACE);
        if (!brace) consume(TokenType::LEFT_PAREN, "expected '(' or '{' after class name in 'new' expression");
        else        advance();  // consume '{'
        TokenType close = brace ? TokenType::RIGHT_BRACE : TokenType::RIGHT_PAREN;
        // Braces stay positional; parenthesised `new Point(...)` accepts named arguments.
        std::vector<Token> argNames;
        std::vector<std::unique_ptr<Expr>> args = parseCallArgs(argNames, close, /*allowNames=*/!brace);
        return makeExpr(NewExpr{ keyword, className, std::move(args), std::move(argNames) });
    }

    if (match({ TokenType::TRUE, TokenType::FALSE,
                TokenType::NUMBER, TokenType::STRING, TokenType::CHAR })) {
        return makeExpr(LiteralExpr{ previous() });
    }

    // sizeof(Type) — size in bytes of a type, as u64.
    if (match({ TokenType::SIZEOF })) {
        Token keyword = previous();
        consume(TokenType::LEFT_PAREN, "expected '(' after 'sizeof'");
        if (!isTypeName()) throw error(peek(), "expected a type name in 'sizeof'");
        Token typeName = consumeType();
        consume(TokenType::RIGHT_PAREN, "expected ')' after 'sizeof' type");
        return makeExpr(SizeofExpr{ keyword, typeName });
    }

    // Compile-time reflection builtin: @name(args).
    if (match({ TokenType::AT })) return parseReflectExpr();

    if (match({ TokenType::IDENTIFIER })) {
        Token name = previous();

        // Bare single-parameter untyped lambda: `x -> …` (the `()` is optional for one parameter).
        // Not inside a switch case label, where `x ->` is `case x -> body`, not a lambda.
        if (check(TokenType::ARROW) && !parsingCaseLabel_) {
            std::vector<std::pair<Token, std::optional<Token>>> p;
            p.emplace_back(name, std::nullopt);
            return finishLambda(std::move(p), name.line);
        }

        // Generic function call: name<typeArgs>(args)  →  mangled concrete call.
        if (gen_->funcNames.count(name.lexeme) && check(TokenType::LESS)) {
            std::vector<std::vector<Token>> typeArgs = parseTypeArgList();
            std::string mangled = mangleInstantiation(name.lexeme, typeArgs);
            recordInstantiation(name.lexeme, mangled, std::move(typeArgs));
            consume(TokenType::LEFT_PAREN, "expected '(' after generic type arguments");
            std::vector<Token> genNames;
            std::vector<std::unique_ptr<Expr>> genArgs =
                parseCallArgs(genNames, TokenType::RIGHT_PAREN, /*allowNames=*/true);
            return makeExpr(CallExpr{ Token{ TokenType::IDENTIFIER, mangled, name.line },
                                      std::move(genArgs), std::move(genNames) });
        }

        // Constructor call via braces: ClassName{ args } — an alternate delimiter for
        // ClassName(args). Gated on a known class name (a bare `x {` is otherwise not an
        // expression form in GG).
        if (check(TokenType::LEFT_BRACE)
            && (classNames.count(name.lexeme) || gen_->classNames.count(name.lexeme))) {
            advance();  // consume '{'
            std::vector<std::unique_ptr<Expr>> args;
            if (!check(TokenType::RIGHT_BRACE)) {
                do { args.push_back(box(parseExpression())); } while (match({ TokenType::COMMA }));
            }
            consume(TokenType::RIGHT_BRACE, "expected '}' after constructor arguments");
            return makeExpr(CallExpr{ name, std::move(args) });
        }

        // Function call: IDENTIFIER ( args )
        if (match({ TokenType::LEFT_PAREN })) {
            // If this is a single-`Call`-bounded-parameter generic, expose that bound's signature so
            // an untyped lambda argument can infer its parameter/return types while being parsed.
            const std::pair<std::vector<Token>, Token>* savedSig = expectedLambdaSig_;
            if (gen_->funcNames.count(name.lexeme)) {
                auto tit = gen_->templates.find(name.lexeme);
                if (tit != gen_->templates.end() && tit->second.typeParams.size() == 1
                    && !tit->second.bounds.empty()) {
                    for (const std::string& b : tit->second.bounds[0]) {
                        if (b.rfind("Call$", 0) != 0) continue;
                        auto sit = gen_->callTraitSigs.find(b);
                        if (sit != gen_->callTraitSigs.end()) expectedLambdaSig_ = &sit->second;
                        break;
                    }
                }
            }
            std::vector<Token> argNames;
            std::vector<std::unique_ptr<Expr>> args =
                parseCallArgs(argNames, TokenType::RIGHT_PAREN, /*allowNames=*/true);
            expectedLambdaSig_ = savedSig;   // restore (handles nested calls)

            // Lambda-literal inference: a generic function with exactly one `Call`-bounded type
            // parameter, called without explicit `<…>`, and a lambda-literal argument → infer that
            // type parameter as the lambda's generated class (whose type is otherwise unspellable).
            if (gen_->funcNames.count(name.lexeme)) {
                auto tit = gen_->templates.find(name.lexeme);
                if (tit != gen_->templates.end() && tit->second.typeParams.size() == 1
                    && !tit->second.bounds.empty()) {
                    bool callBound = false;
                    for (const std::string& b : tit->second.bounds[0])
                        if (b.rfind("Call$", 0) == 0) { callBound = true; break; }
                    std::string lam;
                    if (callBound)
                        for (const auto& a : args) {
                            if (const auto* c = std::get_if<CallExpr>(a->node.get()))
                                if (c->callee.lexeme.rfind("__lambda_", 0) == 0) { lam = c->callee.lexeme; break; }
                        }
                    if (!lam.empty()) {
                        std::vector<std::vector<Token>> typeArgs = { { Token{ TokenType::IDENTIFIER, lam, name.line } } };
                        std::string mangled = mangleInstantiation(name.lexeme, typeArgs);
                        recordInstantiation(name.lexeme, mangled, std::move(typeArgs));
                        return makeExpr(CallExpr{ Token{ TokenType::IDENTIFIER, mangled, name.line },
                                                  std::move(args), std::move(argNames) });
                    }
                }
            }

            // Generic type-argument DEDUCTION: a generic function called without explicit `<…>`
            // (and not the lambda case above) infers its type parameters from the argument types,
            // so `f(x)` behaves like `f<T>(x)`. Only attempted for a purely positional call.
            if (gen_->funcNames.count(name.lexeme)) {
                bool positional = true;
                for (const Token& an : argNames) if (!an.lexeme.empty()) { positional = false; break; }
                std::vector<std::vector<Token>> inferred;
                if (positional && inferGenericTypeArgs(name.lexeme, args, inferred)) {
                    std::string mangled = mangleInstantiation(name.lexeme, inferred);
                    recordInstantiation(name.lexeme, mangled, std::move(inferred));
                    return makeExpr(CallExpr{ Token{ TokenType::IDENTIFIER, mangled, name.line },
                                              std::move(args), std::move(argNames) });
                }
                // A generic function with no explicit `<…>` whose type arguments we could not deduce:
                // a clear error beats the confusing "unknown function" a bare template name causes
                // later (only mangled instantiations are ever defined).
                throw error(name, "cannot infer type argument(s) for generic function '" + name.lexeme
                            + "'; specify them explicitly, e.g. " + name.lexeme + "<Type>(...)");
            }
            return makeExpr(CallExpr{ name, std::move(args), std::move(argNames) });
        }

        // Lambda capture: a bare name inside a lambda body that resolves to an enclosing
        // local/parameter (a scope below the lambda's own) is captured by value.
        if (capturing_) {
            bool inLambda = false;
            for (size_t i = captureBase_; i < scopes_.size(); ++i)
                if (scopes_[i].count(name.lexeme)) { inLambda = true; break; }
            if (!inLambda) {
                const Token* capType = nullptr;
                for (size_t i = captureBase_; i-- > 0; ) {          // enclosing local / parameter
                    auto it = scopes_[i].find(name.lexeme);
                    if (it != scopes_[i].end()) { capType = &it->second; break; }
                }
                if (!capType) {                                     // enclosing instance field
                    auto fit = classFieldScope_.find(name.lexeme);
                    if (fit != classFieldScope_.end()) capType = &fit->second;
                }
                if (capType) {
                    bool dup = false;
                    for (const auto& c : captures_) if (c.first == name.lexeme) { dup = true; break; }
                    if (!dup) captures_.emplace_back(name.lexeme, *capType);
                }
            }
        }
        return makeExpr(IdentifierExpr{ name });
    }

    // Grouping — no AST node, just return the inner expression directly
    if (match({ TokenType::LEFT_PAREN })) {
        Expr inner = parseExpression();
        consume(TokenType::RIGHT_PAREN, "expected ')' after expression");
        return inner;
    }

    // Untyped brace initializer `{ args }` — the class is deduced from the expected type at the use
    // site (e.g. the inner `{0,0}` in `Line l{ {0,0}, {1,1} }`). Typed `Point{args}` is handled
    // above as a constructor call.
    if (check(TokenType::LEFT_BRACE)) {
        Token brace = advance();  // consume '{'
        std::vector<std::unique_ptr<Expr>> args;
        if (!check(TokenType::RIGHT_BRACE)) {
            do { args.push_back(box(parseExpression())); } while (match({ TokenType::COMMA }));
        }
        consume(TokenType::RIGHT_BRACE, "expected '}' after brace-initializer arguments");
        return makeExpr(BraceInitExpr{ std::move(args), brace });
    }

    // A bare type keyword (i32, f64, ptr, …) where a value is expected means a type name has
    // leaked into expression position. The two common causes: (1) a type was passed as a value —
    // e.g. `sizeOf(i32)` (an ordinary call) instead of the `sizeof(i32)` keyword, or any
    // `f(i32)` that passes a type as an argument; (2) `Name<i32>` where `Name` isn't a declared
    // generic, so it parses as the comparison `Name < i32 > …` and the type keyword ends up here.
    if (isTypeName())
        throw error(peek(), "expected a value here, but found the type '" + peek().lexeme
            + "'.\n  A type can't be used where a value is expected. Two common causes:\n"
            "    (1) a type was passed as a value - e.g. a call `f(" + peek().lexeme
            + ")`; if you meant its byte size, use the `sizeof(" + peek().lexeme
            + ")` keyword (lowercase);\n"
            "    (2) `Name<" + peek().lexeme
            + ">` where `Name` is not a declared generic (`class Name<T>`).");

    throw error(peek(), "expected expression");
}