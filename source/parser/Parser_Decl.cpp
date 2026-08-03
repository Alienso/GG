//
// Created by Vladimir Arsenijevic on 28.5.2026.
//

#include "Parser.h"
#include <iostream>


// After the `fn` keyword: a free function `[private] name(params) [-> RetType [alias]] { body }`.
// No arrow ⇒ void return. An alias names the result (required for object-value returns).
// `private` (optional, before the name) makes the function file-local — a warning, not an
// error, if called cross-file, mirroring private fields/methods.
Stmt Parser::parseFnDeclaration() {
    bool isPublic = !match({ TokenType::PRIVATE });
    Token name = consume(TokenType::IDENTIFIER, "expected function name after 'fn'");
    std::vector<ParamDecl> params = parseParamList();
    bool        hasAlias = false;
    std::string alias;
    Token retType = parseReturnSuffix(hasAlias, alias);
    consume(TokenType::LEFT_BRACE, "expected '{' before function body");
    bool saved = insideFunction;
    insideFunction = true;
    BlockStmt body = parseBlockBody();
    insideFunction = saved;
    return makeStmt(FunctionDeclStmt{ retType, name, std::move(params), std::move(body),
                                      hasAlias, alias, isPublic, filename });
}

Stmt Parser::parseExternFuncDecl(const Token& keyword) {
    Token name = consume(TokenType::IDENTIFIER, "expected function name after 'extern'");
    std::vector<ParamDecl> params = parseParamList(/*allowDefaults=*/false);
    bool        hasAlias = false;
    std::string alias;
    Token returnType = parseReturnSuffix(hasAlias, alias);
    if (hasAlias) throw error(name, "extern functions cannot declare a return alias");
    consume(TokenType::SEMICOLON, "expected ';' after extern declaration");
    return makeStmt(ExternFuncDeclStmt{ keyword, returnType, name, std::move(params) });
}

// `(param, ...)` — the parenthesised parameter list.
std::vector<ParamDecl> Parser::parseParamList(bool allowDefaults) {
    consume(TokenType::LEFT_PAREN, "expected '(' after function name");
    std::vector<ParamDecl> params;
    if (!check(TokenType::RIGHT_PAREN)) {
        do { params.push_back(parseParam()); } while (match({ TokenType::COMMA }));
    }
    consume(TokenType::RIGHT_PAREN, "expected ')' after parameters");

    // A variadic pack parameter must be the last one (v1: at most one pack, in final position).
    for (size_t i = 0; i < params.size(); ++i)
        if (params[i].isVariadic && i + 1 != params.size())
            throw error(params[i].name, "a variadic pack parameter must be the last parameter");

    // A default may sit on ANY parameter, in any position — named arguments can fill the later
    // ones while a defaulted earlier param falls back (a purely positional call still only omits
    // trailing defaults, since positional binding is left-to-right). `extern` takes no defaults.
    if (!allowDefaults)
        for (const ParamDecl& p : params)
            if (p.defaultValue != nullptr)
                throw error(p.name, "default parameter values are not allowed on 'extern' declarations");

    // Seed the next block scope (the function/lambda body) with these params, for capture analysis.
    // Cleared first so a stale seed from a bodyless trait signature can't leak into a later body.
    pendingScopeSeed_.clear();
    for (const ParamDecl& p : params) pendingScopeSeed_.emplace_back(p.name.lexeme, p.typeName);
    return params;
}

// See the header note. Recognises `name: expr` named arguments (unambiguous — no expression form
// begins with `IDENTIFIER :`), enforcing that positional args precede all named ones.
std::vector<std::unique_ptr<Expr>> Parser::parseCallArgs(std::vector<Token>& names,
                                                         TokenType close, bool allowNames,
                                                         std::vector<bool>* spreads) {
    std::vector<std::unique_ptr<Expr>> args;
    if (!check(close)) {
        bool sawNamed = false;
        do {
            if (allowNames && check(TokenType::IDENTIFIER) && peekNext().type == TokenType::COLON) {
                Token n = advance();            // the parameter name
                advance();                      // consume ':'
                sawNamed = true;
                names.push_back(n);
            } else {
                if (sawNamed)
                    throw error(peek(), "a positional argument cannot follow a named argument");
                names.push_back(Token{ TokenType::IDENTIFIER, "", peek().line });  // positional
            }
            args.push_back(box(parseExpression()));
            bool spread = match({ TokenType::ELLIPSIS });   // `xs...` — spread a pack into the call
            if (spreads) spreads->push_back(spread);
            else if (spread) throw error(previous(), "'...' spread is only valid as an argument to a "
                                                     "variadic function");
        } while (match({ TokenType::COMMA }));
    }
    consume(close, close == TokenType::RIGHT_BRACE ? "expected '}' after arguments"
                                                   : "expected ')' after arguments");
    return args;
}

// Optional `-> RetType [alias]`. Returns the return type token — a synthesized `void`
// token when there is no arrow — and sets hasAlias/aliasName.
Token Parser::parseReturnSuffix(bool& hasAlias, std::string& aliasName) {
    hasAlias = false;
    if (match({ TokenType::ARROW })) {
        if (!isTypeName()) throwTypeExpected("a return type after '->'");
        Token retType = consumeType();
        if (check(TokenType::IDENTIFIER)) {         // optional return alias
            hasAlias  = true;
            aliasName = advance().lexeme;
        }
        return retType;
    }
    return Token{ TokenType::VOID, "void", previous().line };  // no arrow ⇒ void
}

ParamDecl Parser::parseParam() {
    bool isMut = match({ TokenType::MUT });
    if (!isTypeName()) throwTypeExpected("a parameter type");
    Token paramType = consumeType();
    // Variadic pack: `Ts... args` — the pack element type followed by `...`. The type is a generic
    // pack type-parameter; the pack materializes as a tuple at monomorphization.
    bool isVariadic = match({ TokenType::ELLIPSIS });
    Token paramName = consume(TokenType::IDENTIFIER, "expected parameter name");
    std::unique_ptr<Expr> defaultValue;
    if (match({ TokenType::EQUAL })) {                     // `= <expr>` default value
        if (isVariadic) throw error(paramName, "a variadic pack parameter cannot have a default value");
        defaultValue = std::make_unique<Expr>(parseExpression());
    }
    return ParamDecl{ paramType, paramName, isMut, isVariadic, std::move(defaultValue) };
}


Stmt Parser::parseClassDecl() {
    // `class <T> Name` is a common mistake — GG puts type parameters after the name.
    if (peek().type == TokenType::LESS)
        throw error(peek(), "type parameters go after the class name: write "
            "`class Name<T> { ... }`, not `class <T> Name`");
    Token name = consume(TokenType::IDENTIFIER, "expected class name after 'class'");
    consume(TokenType::LEFT_BRACE, "expected '{' after class name");

    std::deque<FieldDecl> fields;
    std::deque<MethodDecl> methods;
    parseMemberList(name, fields, methods, /*allowDestructor=*/true, /*isEnum=*/false);

    consume(TokenType::RIGHT_BRACE, "expected '}' after class body");
    return makeStmt(ClassDeclStmt{ name, std::move(fields), std::move(methods) });
}

// A run of `@Name` / `@Name(arg, …)` annotation prefixes in declaration position. Args are ordinary
// expressions (validated compile-time-constant in semantics). Positional disambiguation: this is
// only called where a declaration/member starts, so a leading '@' is never a reflection builtin.
std::deque<AnnotationApp> Parser::parseAnnotationPrefixes() {
    std::deque<AnnotationApp> anns;
    while (check(TokenType::AT)) {
        advance();   // '@'
        Token name = consume(TokenType::IDENTIFIER, "expected an annotation name after '@'");
        std::vector<std::unique_ptr<Expr>> args;
        std::vector<std::vector<Token>>    argTokens;
        if (match({ TokenType::LEFT_PAREN })) {
            if (!check(TokenType::RIGHT_PAREN))
                do {
                    size_t start = current;
                    args.push_back(box(parseExpression()));
                    argTokens.emplace_back(tokens.begin() + start, tokens.begin() + current);
                } while (match({ TokenType::COMMA }));
            consume(TokenType::RIGHT_PAREN, "expected ')' after annotation arguments");
        }
        anns.push_back(AnnotationApp{ name, std::move(args), std::move(argTokens) });
    }
    return anns;
}

// `annotation Name { <const fields> }` — a compile-time metadata type. Reuses the class member
// parser for its fields; methods are rejected (an annotation carries data only).
Stmt Parser::parseAnnotationDecl() {
    Token name = consume(TokenType::IDENTIFIER, "expected annotation name after 'annotation'");
    consume(TokenType::LEFT_BRACE, "expected '{' after annotation name");
    std::deque<FieldDecl>  fields;
    std::deque<MethodDecl> methods;
    parseMemberList(name, fields, methods, /*allowDestructor=*/false, /*isEnum=*/false);
    if (!methods.empty())
        throw error(name, "an annotation cannot declare methods — only const data fields");
    for (const FieldDecl& f : fields)
        if (f.isStatic)
            throw error(f.name, "an annotation field cannot be 'static'");
    consume(TokenType::RIGHT_BRACE, "expected '}' after annotation body");
    return makeStmt(AnnotationDeclStmt{ name, std::move(fields) });
}

// One method inside a trait or impl body, `fn`-prefixed and unified:
// `fn name(params) [mut] [-> RetType [alias]] (';' | '{ body }')`. In a trait,
// `bodyOptional` is true and a `;` yields a required (bodyless) method.
MethodDecl Parser::parseTraitMethod(bool bodyOptional) {
    consume(TokenType::FN, "expected 'fn' before method");
    // Unified: `fn name(params) [mut] [-> RetType [alias]] (';' | '{ }')`.
    Token mname = consume(TokenType::IDENTIFIER, "expected method name after 'fn'");
    std::vector<ParamDecl> params = parseParamList();
    bool methodMut = match({ TokenType::MUT });
    bool        hasAlias = false;
    std::string alias;
    Token retType = parseReturnSuffix(hasAlias, alias);
    bool      hasBody = true;
    BlockStmt body;
    parseTraitMethodBody(bodyOptional, hasBody, body);
    return MethodDecl{
        /*isPublic=*/true, /*isConstructor=*/false, /*isDestructor=*/false, /*isStatic=*/false,
        /*isMut=*/methodMut, hasBody, retType, mname, std::move(params), std::move(body),
        hasAlias, alias
    };
}

// Parse the tail of a trait/impl method: a `;` (bodyless, only when bodyOptional) or a
// `{ body }`. Sets hasBody/body accordingly.
void Parser::parseTraitMethodBody(bool bodyOptional, bool& hasBody, BlockStmt& body) {
    if (bodyOptional && match({ TokenType::SEMICOLON })) {
        hasBody = false;   // required (bodyless) trait method
        return;
    }
    consume(TokenType::LEFT_BRACE, bodyOptional ? "expected '{' or ';'" : "expected '{' before method body");
    bool saved = insideFunction;
    insideFunction = true;
    body = parseBlockBody();
    insideFunction = saved;
}

Stmt Parser::parseTraitDecl() {
    Token name = consume(TokenType::IDENTIFIER, "expected trait name after 'trait'");
    consume(TokenType::LEFT_BRACE, "expected '{' after trait name");
    std::deque<MethodDecl> methods;
    while (!check(TokenType::RIGHT_BRACE) && !isAtEnd())
        methods.push_back(parseTraitMethod(/*bodyOptional=*/true));
    consume(TokenType::RIGHT_BRACE, "expected '}' after trait body");
    return makeStmt(TraitDeclStmt{ name, std::move(methods) });
}

Stmt Parser::parseImplDecl() {
    Token traitName = consume(TokenType::IDENTIFIER, "expected trait name after 'impl'");
    consume(TokenType::FOR, "expected 'for' in 'impl <Trait> for <Type>'");
    if (!isTypeName()) throw error(peek(), "expected a type name after 'for'");
    Token typeName = consumeType();
    consume(TokenType::LEFT_BRACE, "expected '{' after impl header");
    std::deque<MethodDecl> methods;
    inImplBlock_ = true;   // enriches a "not a known type" error with the generic-impl note
    while (!check(TokenType::RIGHT_BRACE) && !isAtEnd())
        methods.push_back(parseTraitMethod(/*bodyOptional=*/false));
    inImplBlock_ = false;
    consume(TokenType::RIGHT_BRACE, "expected '}' after impl body");

    // `impl Call for X` is sugar: canonicalize the trait to the signature-mangled `Call$…`
    // derived from the `call` method, so it matches `Call(P…)->R` bounds with the same shape.
    if (traitName.lexeme == "Call") {
        const MethodDecl* callMethod = nullptr;
        for (const MethodDecl& m : methods)
            if (m.name.lexeme == "call") { callMethod = &m; break; }
        if (!callMethod) throw error(traitName, "'impl Call' must define a 'call' method");
        std::vector<Token> paramTypes;
        for (const ParamDecl& p : callMethod->params) paramTypes.push_back(p.typeName);
        std::string canon = canonicalCallTrait(paramTypes, callMethod->returnType);
        return makeStmt(ImplDeclStmt{ Token{ TokenType::IDENTIFIER, canon, traitName.line },
                                      typeName, std::move(methods) });
    }
    return makeStmt(ImplDeclStmt{ traitName, typeName, std::move(methods) });
}

Stmt Parser::parseEnumDecl() {
    Token name = consume(TokenType::IDENTIFIER, "expected enum name after 'enum'");
    classNames.insert(name.lexeme);   // recognise the enum name as a type name
    consume(TokenType::LEFT_BRACE, "expected '{' after enum name");

    std::deque<EnumVariant> variants;
    // Variant list: [@Name…] IDENTIFIER ( '(' args ')' )?  comma-separated, terminated by ';' or '}'.
    if (check(TokenType::IDENTIFIER) || check(TokenType::AT)) {
        do {
            std::deque<AnnotationApp> vanns;
            if (check(TokenType::AT)) vanns = parseAnnotationPrefixes();
            Token vname = consume(TokenType::IDENTIFIER, "expected enum variant name");
            std::vector<std::unique_ptr<Expr>> args;
            if (match({ TokenType::LEFT_PAREN })) {
                if (!check(TokenType::RIGHT_PAREN)) {
                    do { args.push_back(box(parseExpression())); } while (match({ TokenType::COMMA }));
                }
                consume(TokenType::RIGHT_PAREN, "expected ')' after enum variant arguments");
            }
            variants.push_back(EnumVariant{ vname, std::move(args) });
            variants.back().annotations = std::move(vanns);
        } while (match({ TokenType::COMMA }));
    }
    // Optional ';' separates the variant list from the body (fields / constructor / methods).
    (void)match({ TokenType::SEMICOLON });

    std::deque<FieldDecl> fields;
    std::deque<MethodDecl> methods;
    // Parse a destructor if present so the semantic analyser can reject it with a
    // clear "enums cannot declare a destructor" diagnostic (rather than a generic
    // parse error). allowDestructor=true here; the rejection happens in semantics.
    parseMemberList(name, fields, methods, /*allowDestructor=*/true, /*isEnum=*/true);

    consume(TokenType::RIGHT_BRACE, "expected '}' after enum body");
    return makeStmt(EnumDeclStmt{ name, std::move(variants), std::move(fields), std::move(methods) });
}

void Parser::parseMemberList(const Token& name,
                             std::deque<FieldDecl>& fields,
                             std::deque<MethodDecl>& methods,
                             bool allowDestructor,
                             bool isEnum) {
    classFieldScope_.clear();   // fresh per class (no nested classes); populated as fields are parsed
    while (!check(TokenType::RIGHT_BRACE) && !isAtEnd()) {
        // A member may be prefixed with `@Name(...)` annotations; attach them to whatever member
        // this iteration parses (each branch ends by pushing exactly one field/method).
        std::deque<AnnotationApp> memberAnns;
        if (check(TokenType::AT)) memberAnns = parseAnnotationPrefixes();

        // ---- Method: `fn [static] [private] ...` (the `fn` leads; modifiers follow) ----
        if (match({ TokenType::FN })) {
            bool mPublic = true;
            bool mStatic = false;
            for (;;) {
                if (match({ TokenType::PRIVATE })) { mPublic = false; continue; }
                if (match({ TokenType::STATIC  })) { mStatic = true;  continue; }
                break;
            }
            // Unified method: `fn name(params) [mut] [-> RetType [alias]] { }`.
            Token mname = consume(TokenType::IDENTIFIER, "expected method name after 'fn'");
            std::vector<ParamDecl> params = parseParamList();
            bool methodMut = match({ TokenType::MUT });
            if (methodMut && mStatic)
                throw error(mname, "static methods cannot be 'mut' (there is no implicit 'this')");
            if (methodMut && isEnum)
                throw error(mname, "enum methods cannot be 'mut' — enums are immutable");
            bool        hasAlias = false;
            std::string alias;
            Token retType = parseReturnSuffix(hasAlias, alias);
            consume(TokenType::LEFT_BRACE, "expected '{' before method body");
            bool savedIF = insideFunction;
            insideFunction = true;
            BlockStmt body = parseBlockBody();
            insideFunction = savedIF;
            methods.push_back(MethodDecl{
                mPublic, /*isConstructor=*/false, /*isDestructor=*/false, mStatic,
                /*isMut=*/methodMut, /*hasBody=*/true, retType, mname,
                std::move(params), std::move(body), hasAlias, alias
            });
            methods.back().annotations = std::move(memberAnns);
            continue;
        }

        // ---- Non-method members: fields, constructors, destructors ----
        // Public by default; 'private' opts out. `static` marks a class-level field; `mut`
        // marks a reassignable field. All three may appear in any order.
        bool isPublic = true;
        bool isStatic = false;
        bool isMut    = false;
        for (;;) {
            if (match({ TokenType::PRIVATE })) { isPublic = false; continue; }
            if (match({ TokenType::STATIC  })) { isStatic = true;  continue; }
            if (match({ TokenType::MUT     })) { isMut    = true;  continue; }
            break;
        }
        if (isMut && isEnum)
            throw error(previous(), "enum fields are always immutable; 'mut' is not allowed");

        // Destructor: ~ClassName()
        if (allowDestructor
            && check(TokenType::TILDE)
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
            bool savedDtor = insideFunction;
            insideFunction = true;
            BlockStmt dtorBody = parseBlockBody();
            insideFunction = savedDtor;

            methods.push_back(MethodDecl{
                isPublic, /*isConstructor=*/false, /*isDestructor=*/true, /*isStatic=*/false,
                /*isMut=*/false, /*hasBody=*/true, dtorName, dtorName, {}, std::move(dtorBody)
            });
            methods.back().annotations = std::move(memberAnns);
            continue;
        }

        // Constructor: IDENTIFIER (== class name) followed by '('
        if (check(TokenType::IDENTIFIER) && peek().lexeme == name.lexeme
            && current + 1 < tokens.size() && tokens[current + 1].type == TokenType::LEFT_PAREN) {
            Token ctorName = advance();  // consume class name
            std::vector<ParamDecl> params = parseParamList();   // '(' … ')' + default values
            consume(TokenType::LEFT_BRACE,  "expected '{' before constructor body");
            bool savedIF1 = insideFunction;
            insideFunction = true;
            BlockStmt body = parseBlockBody();
            insideFunction = savedIF1;

            methods.push_back(MethodDecl{
                isPublic, /*isConstructor=*/true, /*isDestructor=*/false, /*isStatic=*/false,
                /*isMut=*/false, /*hasBody=*/true,
                ctorName,   // returnType token = class name token (no actual return type)
                ctorName,   // name token
                std::move(params), std::move(body)
            });
            methods.back().annotations = std::move(memberAnns);
            continue;
        }

        // Field: `[modifiers] Type[&] name [= const];`. A '(' here means a method was
        // written without the required `fn` keyword.
        if (!isTypeName()) throw error(peek(), "expected 'fn', a field, or a constructor");
        Token memberType = consumeType();
        Token memberName = consume(TokenType::IDENTIFIER, "expected member name");

        if (check(TokenType::LEFT_PAREN))
            throw error(memberName, "methods must be declared with 'fn' (e.g. 'fn "
                        + memberType.lexeme + " " + memberName.lexeme + "(...)')");

        // Field — a static field may carry a constant initializer.
        std::unique_ptr<Expr> initializer;
        if (isStatic && match({ TokenType::EQUAL })) {
            initializer = box(parseExpression());
        }
        consume(TokenType::SEMICOLON, "expected ';' after field declaration");
        // Make instance fields capturable by a lambda in a later method body.
        if (!isStatic) classFieldScope_.emplace(memberName.lexeme, memberType);
        fields.push_back(FieldDecl{
            isPublic, isStatic, isMut, memberType, memberName, std::move(initializer)
        });
        fields.back().annotations = std::move(memberAnns);
    }
}