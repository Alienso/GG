//
// Created by Vladimir Arsenijevic on 28.5.2026.
//

#include "Parser.h"
#include "Parser_internal.h"   // isTypeKeyword


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

    // Range-for detection: `for (DECL : ITERABLE)`. A COLON at paren-depth 1 that is NOT a ternary
    // `?:` colon (no pending unmatched `?`) and comes before any `;` marks the range form. A `::`
    // (COLON_COLON) is a distinct token, so a `Class::x` in the header is never mistaken for it.
    {
        size_t i = current; int depth = 1, ternary = 0; bool range = false, decided = false;
        while (i < tokens.size() && depth > 0 && !decided) {
            TokenType t = tokens[i].type;
            if      (t == TokenType::LEFT_PAREN)  ++depth;
            else if (t == TokenType::RIGHT_PAREN) --depth;
            else if (depth == 1) {
                if      (t == TokenType::SEMICOLON) decided = true;                 // C-style
                else if (t == TokenType::QUESTION)  ++ternary;
                else if (t == TokenType::COLON) { if (ternary > 0) --ternary; else { range = true; decided = true; } }
            }
            ++i;
        }
        if (range) return parseForInStmt();
    }

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

// `for (DECL : ITERABLE) BODY` — the Java-style range loop, a parser-only desugar (no new AST /
// semantic / codegen). `(` is already consumed and the cursor sits at DECL. Lowers to:
//   { [mut var __forsrc_N = ITERABLE;]                 // only when ITERABLE is a temp-producer
//     mut var __forit_N = (<src>).iter();
//     while (__forit_N.hasNext()) { DECL = __forit_N.next(); BODY } }
// `next()` returns a `T*` borrow, so DECL binds it via the ordinary rules: a primitive `i32 x`
// loads the value, a borrow `Point* p` binds it directly. A bare object/enum loop variable (a value
// copy per element) is rejected — objects must be iterated by borrow (`Point*`).
// The `__forsrc` binding is introduced when ITERABLE is a call/`new` (a temporary): it keeps the
// collection alive for the whole loop (otherwise a reference-returning-call iterable would release
// the collection — running its dtor and freeing the buffer — at the `.iter()` statement boundary,
// BEFORE the loop, a use-after-free) and splits a value-returning-call chain into two steps that
// each type-check (`var s = call(); var it = s.iter();`). A plain place (local / field / `this` /
// index) is iterated directly — no binding, no copy.
Stmt Parser::parseForInStmt() {
    ParamDecl lv = parseParam();                       // [mut] <type> <name>
    const Token& lt = lv.typeName;
    const bool isBorrow = lt.lexeme.rfind("ref:", 0) == 0;   // `T*` → synthesized "ref:T"
    const bool isPrim   = isTypeKeyword(lt.type);            // i32/f64/bool/char/str/… (bare value ok)
    if (!isBorrow && !isPrim) {
        std::string base = lt.lexeme;
        if (!base.empty() && base.back() == '&') base.pop_back();   // `Point&` → suggest `Point*`
        throw error(lv.name, "iterate '" + base + "' by borrow: declare the loop variable as '"
                    + base + "* " + lv.name.lexeme + "' (a value copy per element is not allowed for "
                    "object/enum elements; only primitives may be iterated by value)");
    }
    consume(TokenType::COLON, "expected ':' between the loop variable and the iterable");
    Expr iterable = parseExpression();
    consume(TokenType::RIGHT_PAREN, "expected ')' after the for-in iterable");

    // Parse the body in a fresh scope that knows the loop variable (so generic inference / lambda
    // capture over it inside the body resolve, exactly as for a normally-declared local).
    const int line = lv.name.line;
    scopes_.emplace_back();
    recordLocal(lv.typeName, lv.name);
    Stmt body = parseStatement();
    scopes_.pop_back();

    const int idx = forInCounter_++;
    const std::string foritName = "__forit_" + std::to_string(idx);
    auto forit = [&] { return makeExpr(IdentifierExpr{ Token{ TokenType::IDENTIFIER, foritName, line } }); };
    auto call0 = [&](Expr recv, const char* m) {
        return makeExpr(MethodCallExpr{ box(std::move(recv)),
            Token{ TokenType::IDENTIFIER, m, line }, {}, {}, /*safe=*/false });
    };

    std::vector<std::unique_ptr<Stmt>> outer;

    // If ITERABLE is a temp-producer (a call / `new`), bind it to `__forsrc` so it lives for the whole
    // loop; a plain place (identifier / member / `this` / index) is iterated directly (no copy).
    Expr iterReceiver;
    const auto& in = *iterable.node;
    const bool iterableIsPlace = std::holds_alternative<IdentifierExpr>(in)
                              || std::holds_alternative<MemberAccessExpr>(in)
                              || std::holds_alternative<ThisExpr>(in)
                              || std::holds_alternative<IndexExpr>(in);
    if (iterableIsPlace) {
        iterReceiver = std::move(iterable);
    } else {
        const std::string srcName = "__forsrc_" + std::to_string(idx);
        Expr srcDecl = makeExpr(VarDeclExpr{ Token{ TokenType::VAR, "var", line },
            Token{ TokenType::IDENTIFIER, srcName, line }, box(std::move(iterable)),
            /*arraySize=*/0, /*isStatic=*/false, /*isMut=*/true });
        outer.push_back(box(makeStmt(ExprStmt{ std::move(srcDecl) })));
        iterReceiver = makeExpr(IdentifierExpr{ Token{ TokenType::IDENTIFIER, srcName, line } });
    }

    // mut var __forit = (<src>).iter();
    Expr foritInit = call0(std::move(iterReceiver), "iter");
    Expr foritDecl = makeExpr(VarDeclExpr{ Token{ TokenType::VAR, "var", line },
        Token{ TokenType::IDENTIFIER, foritName, line }, box(std::move(foritInit)),
        /*arraySize=*/0, /*isStatic=*/false, /*isMut=*/true });

    // DECL = __forit.next();
    Expr loopDecl = makeExpr(VarDeclExpr{ lv.typeName, lv.name, box(call0(forit(), "next")),
        /*arraySize=*/0, /*isStatic=*/false, lv.isMut });

    // while (__forit.hasNext()) { DECL = ...; BODY }
    std::vector<std::unique_ptr<Stmt>> whileBody;
    whileBody.push_back(box(makeStmt(ExprStmt{ std::move(loopDecl) })));
    whileBody.push_back(box(std::move(body)));
    Stmt whileStmt = makeStmt(WhileStmt{ call0(forit(), "hasNext"),
        box(makeStmt(BlockStmt{ std::move(whileBody) })) });

    outer.push_back(box(makeStmt(ExprStmt{ std::move(foritDecl) })));
    outer.push_back(box(std::move(whileStmt)));
    return makeStmt(BlockStmt{ std::move(outer) });
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

// ---- match ----
// `match <scrutinee> { pattern -> body … }`. The scrutinee has no surrounding parens (a full
// expression parsed up to the opening `{`), matching the user-facing form `match pair { … }`.
Stmt Parser::parseMatchStmt() {
    Token keyword  = previous();               // 'match'
    Expr  scrutinee = parseExpression();
    std::deque<MatchArm> arms = parseMatchArmBlock();
    return makeStmt(MatchStmt{ keyword, std::move(scrutinee), std::move(arms) });
}

Expr Parser::parseMatchExpr() {
    Token keyword  = previous();               // 'match' (consumed by the caller)
    Expr  scrutinee = parseExpression();
    std::deque<MatchArm> arms = parseMatchArmBlock();
    return makeExpr(MatchExpr{ keyword, box(std::move(scrutinee)), std::move(arms) });
}

std::deque<MatchArm> Parser::parseMatchArmBlock() {
    consume(TokenType::LEFT_BRACE, "expected '{' to open match body");
    std::deque<MatchArm> arms;
    while (!check(TokenType::RIGHT_BRACE) && !isAtEnd()) {
        parsingCaseLabel_ = true;              // a trailing `->` is the arm separator, not a lambda
        Pattern pat = parsePattern();
        parsingCaseLabel_ = false;
        Token arrow = consume(TokenType::ARROW, "expected '->' after match pattern");

        std::unique_ptr<Expr> valueExpr = nullptr;
        std::unique_ptr<Stmt> block     = nullptr;
        if (check(TokenType::LEFT_BRACE)) {
            block = box(parseBlock());
        } else {
            valueExpr = box(parseExpression());
            consume(TokenType::SEMICOLON, "expected ';' after match arm expression");
        }
        arms.push_back(MatchArm{ std::move(pat), std::move(valueExpr), std::move(block), arrow });
    }
    consume(TokenType::RIGHT_BRACE, "expected '}' to close match body");
    return arms;
}

// Recursive-descent pattern grammar. A pattern is one of: a tuple `(p0, p1, …)`, a struct
// `Class{ field: sub, field, … }`, a wildcard `_`, a binding `name`, or a literal (number / string /
// char / bool / null / enum-variant — compared via ==). Sub-patterns nest.
Pattern Parser::parsePattern() {
    auto mk = [](auto&& v) {
        return Pattern{ std::make_unique<Pattern::Variant>(std::forward<decltype(v)>(v)) };
    };

    // Tuple pattern: '(' sub (',' sub)+ ')'  (arity ≥ 2; '()' reserved for the empty-collection form).
    if (match({ TokenType::LEFT_PAREN })) {
        Token paren = previous();
        std::deque<std::unique_ptr<Pattern>> elems;
        if (!check(TokenType::RIGHT_PAREN)) {
            do { elems.push_back(std::make_unique<Pattern>(parsePattern())); }
            while (match({ TokenType::COMMA }));
        }
        consume(TokenType::RIGHT_PAREN, "expected ')' to close a tuple pattern");
        if (elems.size() < 2)
            throw error(paren, "a tuple pattern needs at least two elements; '()' is reserved and "
                               "'(p)' is just a parenthesized pattern");
        return mk(TuplePat{ std::move(elems), paren });
    }

    // Struct pattern: ClassName '{' (field (':' sub)? ) (',' …)* '}'. A bare `field` is shorthand
    // for `field: field` (bind the field's value to a same-named variable).
    if (check(TokenType::IDENTIFIER)
        && (classNames.count(peek().lexeme) || gen_->classNames.count(peek().lexeme))
        && peekNext().type == TokenType::LEFT_BRACE) {
        Token typeName = advance();
        consume(TokenType::LEFT_BRACE, "expected '{' in a struct pattern");
        std::deque<std::pair<Token, std::unique_ptr<Pattern>>> fields;
        if (!check(TokenType::RIGHT_BRACE)) {
            do {
                Token field = consume(TokenType::IDENTIFIER, "expected a field name in a struct pattern");
                std::unique_ptr<Pattern> sub;
                if (match({ TokenType::COLON }))
                    sub = std::make_unique<Pattern>(parsePattern());
                else
                    sub = std::make_unique<Pattern>(mk(BindingPat{ field }));   // shorthand
                fields.push_back({ field, std::move(sub) });
            } while (match({ TokenType::COMMA }));
        }
        Token brace = consume(TokenType::RIGHT_BRACE, "expected '}' to close a struct pattern");
        return mk(StructPat{ typeName, std::move(fields), brace });
    }

    // Identifier forms: wildcard `_`, an enum-variant / member literal (`Color::RED`, `E.V`), else a
    // binding. (Matching against an existing variable's *value* uses a literal, not a bare name.)
    if (check(TokenType::IDENTIFIER)) {
        if (peek().lexeme == "_") { Token t = advance(); return mk(WildcardPat{ t }); }
        if (peekNext().type == TokenType::COLON_COLON || peekNext().type == TokenType::DOT)
            return mk(LiteralPat{ box(parseUnary()) });      // scoped/member access → literal
        Token name = advance();
        return mk(BindingPat{ name });
    }

    // Literal pattern: a number / string / char / bool / null / negated literal, compared via ==.
    return mk(LiteralPat{ box(parseUnary()) });
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