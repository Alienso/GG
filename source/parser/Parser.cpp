//
// Created by Vladimir Arsenijevic on 28.5.2026.
//

#include "Parser.h"
#include <iostream>

static bool isTypeKeyword(TokenType t) {
    switch (t) {
        case TokenType::I8:  case TokenType::I16: case TokenType::I32: case TokenType::I64:
        case TokenType::U8:  case TokenType::U16: case TokenType::U32: case TokenType::U64:
        case TokenType::F32: case TokenType::F64:
        case TokenType::BOOL: case TokenType::CHAR_TYPE: case TokenType::VOID: case TokenType::PTR:
            return true;
        default:
            return false;
    }
}

// Mangle one type-argument token slice into an LLVM-safe name fragment.
static std::string argMangle(const std::vector<Token>& arg) {
    std::string s;
    for (const Token& t : arg) {
        if (t.type == TokenType::AMPERSAND) s += ".ref";   // Class&  ->  Class.ref
        else                                s += t.lexeme;
    }
    return s;
}

Parser::Parser(std::unordered_set<std::string> initialClassNames, GenericRegistry* sharedRegistry)
    : classNames(std::move(initialClassNames)) {
    if (sharedRegistry) gen_ = sharedRegistry;
}

void Parser::prescanTemplateNames(const std::vector<Token>& toks) {
    // Register class names defined in this token stream. A class followed by '<'
    // is a generic template (its name goes to the generic class-name set instead).
    for (size_t i = 0; i + 1 < toks.size(); ++i) {
        if (toks[i].type == TokenType::CLASS && toks[i + 1].type == TokenType::IDENTIFIER) {
            if (i + 2 < toks.size() && toks[i + 2].type == TokenType::LESS)
                gen_->classNames.insert(toks[i + 1].lexeme);
            else
                classNames.insert(toks[i + 1].lexeme);
        }
    }
    // Register generic function template names ( "<type|id> name < ... > (" )
    // so call sites are recognised regardless of declaration order.
    for (size_t i = 0; i + 2 < toks.size(); ++i) {
        bool startsType = toks[i].type == TokenType::IDENTIFIER || isTypeKeyword(toks[i].type);
        if (startsType && toks[i + 1].type == TokenType::IDENTIFIER
            && toks[i + 2].type == TokenType::LESS) {
            size_t j = i + 3; int depth = 1;
            while (j < toks.size() && depth > 0) {
                if (toks[j].type == TokenType::LESS)             depth++;
                else if (toks[j].type == TokenType::GREATER)     depth--;
                else if (toks[j].type == TokenType::SHIFT_RIGHT) depth -= 2;
                j++;
            }
            if (j < toks.size() && toks[j].type == TokenType::LEFT_PAREN)
                gen_->funcNames.insert(toks[i + 1].lexeme);
        }
    }
}

void Parser::monomorphize(Program& program) { runMonomorphization(program); }

Program Parser::parse(const std::vector<Token>& inputTokens, const std::string& filenameStr,
                      bool runMono) {
    tokens   = std::vector<Token>(inputTokens);
    current  = 0;
    filename = filenameStr;
    // Register this file's template/class names (idempotent if pre-seeded by the caller).
    prescanTemplateNames(tokens);

    Program program;
    while (!isAtEnd()) {
        if (tryCaptureClassTemplate())    continue;   // generic decls produce no AST node
        if (tryCaptureFunctionTemplate()) continue;
        program.declarations.push_back(parseDeclaration());
    }

    if (runMono) runMonomorphization(program);   // expand instantiations now (single-file path)
    return program;
}

// ============================================================
// Generics (monomorphization)
// ============================================================

bool Parser::tryCaptureFunctionTemplate() {
    if (!((isTypeName() || peek().type == TokenType::IDENTIFIER)
          && peekNext().type == TokenType::IDENTIFIER
          && current + 2 < tokens.size() && tokens[current + 2].type == TokenType::LESS))
        return false;

    // Collect type-parameter names between '<' and '>'; verify the decl continues with '('.
    std::vector<std::string> typeParams;
    size_t j = current + 3; int depth = 1;
    while (j < tokens.size() && depth > 0) {
        if (tokens[j].type == TokenType::LESS)            { depth++; }
        else if (tokens[j].type == TokenType::GREATER)    { depth--; if (depth == 0) break; }
        else if (tokens[j].type == TokenType::SHIFT_RIGHT){ depth -= 2; if (depth <= 0) break; }
        else if (tokens[j].type == TokenType::IDENTIFIER && depth == 1) typeParams.push_back(tokens[j].lexeme);
        j++;
    }
    if (j >= tokens.size() || tokens[j].type != TokenType::GREATER) return false;
    size_t afterGt = j + 1;
    if (afterGt >= tokens.size() || tokens[afterGt].type != TokenType::LEFT_PAREN) return false;
    if (typeParams.empty()) return false;

    // Capture: retType, name, then the parameter list and body verbatim.
    std::vector<Token> captured;
    captured.push_back(tokens[current]);       // return type
    captured.push_back(tokens[current + 1]);   // function name

    size_t k = afterGt;
    int parenDepth = 0;
    do {
        if (tokens[k].type == TokenType::LEFT_PAREN)  parenDepth++;
        else if (tokens[k].type == TokenType::RIGHT_PAREN) parenDepth--;
        captured.push_back(tokens[k]);
        ++k;
    } while (k < tokens.size() && parenDepth > 0);

    if (k >= tokens.size() || tokens[k].type != TokenType::LEFT_BRACE) return false;
    int braceDepth = 0;
    do {
        if (tokens[k].type == TokenType::LEFT_BRACE)  braceDepth++;
        else if (tokens[k].type == TokenType::RIGHT_BRACE) braceDepth--;
        captured.push_back(tokens[k]);
        ++k;
    } while (k < tokens.size() && braceDepth > 0);

    const std::string& name = tokens[current + 1].lexeme;
    gen_->templates[name] = GenericTemplate{ std::move(typeParams), std::move(captured) };
    gen_->funcNames.insert(name);
    current = k;   // advance past the captured declaration
    return true;
}

bool Parser::tryCaptureClassTemplate() {
    if (!(peek().type == TokenType::CLASS
          && peekNext().type == TokenType::IDENTIFIER
          && current + 2 < tokens.size() && tokens[current + 2].type == TokenType::LESS))
        return false;

    std::vector<std::string> typeParams;
    size_t j = current + 3; int depth = 1;
    while (j < tokens.size() && depth > 0) {
        if (tokens[j].type == TokenType::LESS)             { depth++; }
        else if (tokens[j].type == TokenType::GREATER)     { depth--; if (depth == 0) break; }
        else if (tokens[j].type == TokenType::SHIFT_RIGHT) { depth -= 2; if (depth <= 0) break; }
        else if (tokens[j].type == TokenType::IDENTIFIER && depth == 1) typeParams.push_back(tokens[j].lexeme);
        j++;
    }
    if (j >= tokens.size() || tokens[j].type != TokenType::GREATER) return false;
    size_t afterGt = j + 1;
    if (afterGt >= tokens.size() || tokens[afterGt].type != TokenType::LEFT_BRACE) return false;
    if (typeParams.empty()) return false;

    std::vector<Token> captured;
    captured.push_back(tokens[current]);       // 'class'
    captured.push_back(tokens[current + 1]);   // class name

    size_t k = afterGt;
    int braceDepth = 0;
    do {
        if (tokens[k].type == TokenType::LEFT_BRACE)  braceDepth++;
        else if (tokens[k].type == TokenType::RIGHT_BRACE) braceDepth--;
        captured.push_back(tokens[k]);
        ++k;
    } while (k < tokens.size() && braceDepth > 0);

    const std::string& name = tokens[current + 1].lexeme;
    gen_->templates[name] = GenericTemplate{ std::move(typeParams), std::move(captured) };
    gen_->classNames.insert(name);
    current = k;
    return true;
}

size_t Parser::typeSpanAt(size_t from) const {
    if (from >= tokens.size()) return 0;
    const Token& t = tokens[from];
    bool isType = isTypeKeyword(t.type)
               || (t.type == TokenType::IDENTIFIER
                   && (classNames.count(t.lexeme) || gen_->classNames.count(t.lexeme)));
    if (!isType) return 0;

    size_t i = from + 1;
    // generic argument list: Name<...>  or  typed pointer: ptr<...>
    if (((t.type == TokenType::IDENTIFIER && gen_->classNames.count(t.lexeme))
         || t.type == TokenType::PTR)
        && i < tokens.size() && tokens[i].type == TokenType::LESS) {
        int depth = 1; ++i;
        while (i < tokens.size() && depth > 0) {
            if (tokens[i].type == TokenType::LESS)             depth++;
            else if (tokens[i].type == TokenType::GREATER)     depth--;
            else if (tokens[i].type == TokenType::SHIFT_RIGHT) depth -= 2;
            ++i;
        }
    }
    // reference suffix
    if (i < tokens.size() && tokens[i].type == TokenType::AMPERSAND) ++i;
    return i - from;
}

std::vector<std::vector<Token>> Parser::parseTypeArgList() {
    consume(TokenType::LESS, "expected '<'");
    std::vector<std::vector<Token>> args;
    if (!check(TokenType::GREATER) && !check(TokenType::SHIFT_RIGHT)) {
        do { args.push_back(parseOneTypeArg()); } while (match({ TokenType::COMMA }));
    }
    consumeCloseAngle();
    return args;
}

// Parse a single type argument. A nested generic instantiation (Name<...>) is
// collapsed into one mangled token (and its instantiation recorded); a trailing
// '&' is kept as a separate token so argMangle can render it as ".ref".
std::vector<Token> Parser::parseOneTypeArg() {
    std::vector<Token> cur;
    Token base = advance();
    if (base.type == TokenType::IDENTIFIER && gen_->classNames.count(base.lexeme)
        && check(TokenType::LESS)) {
        std::vector<std::vector<Token>> nested = parseTypeArgList();
        std::string mangled = mangleInstantiation(base.lexeme, nested);
        recordInstantiation(base.lexeme, mangled, std::move(nested));
        classNames.insert(mangled);
        cur.push_back(Token{ TokenType::IDENTIFIER, mangled, base.line });
    } else {
        cur.push_back(base);
    }
    // A trailing '&' belongs to THIS argument only when no outer close-angles are
    // pending: a '>>' that closed this arg also closes an enclosing level, so any
    // '&' following it applies to the outer type, not this argument.
    if (pendingCloseAngles_ == 0 && check(TokenType::AMPERSAND))
        cur.push_back(advance());
    return cur;
}

// Consume one closing '>'. A '>>' (SHIFT_RIGHT) closes two levels: consume it once
// and leave a virtual '>' pending for the enclosing list (the classic C++ fix).
void Parser::consumeCloseAngle() {
    if (pendingCloseAngles_ > 0) { --pendingCloseAngles_; return; }
    if (check(TokenType::GREATER))     { advance(); return; }
    if (check(TokenType::SHIFT_RIGHT)) { advance(); pendingCloseAngles_ = 1; return; }
    throw error(peek(), "expected '>' to close type arguments");
}

std::string Parser::mangleInstantiation(const std::string& base,
                                        const std::vector<std::vector<Token>>& args) const {
    std::string m = base;
    for (const auto& a : args) m += "$" + argMangle(a);
    return m;
}

void Parser::recordInstantiation(const std::string& templateName, const std::string& mangled,
                                 std::vector<std::vector<Token>> args) {
    if (gen_->instantiated.count(mangled)) return;
    gen_->instantiated.insert(mangled);
    gen_->worklist.push_back(GenericInstantiation{ templateName, mangled, std::move(args) });
}

void Parser::runMonomorphization(Program& program) {
    // Every queued instantiation's mangled name is a concrete class name during
    // re-parse (the per-file parsers that recorded them are gone, so seed here).
    for (const auto& mangled : gen_->instantiated) classNames.insert(mangled);

    while (!gen_->worklist.empty()) {
        GenericInstantiation inst = std::move(gen_->worklist.back());
        gen_->worklist.pop_back();

        auto it = gen_->templates.find(inst.templateName);
        if (it == gen_->templates.end())
            throw error(tokens.empty() ? Token{TokenType::END_OF_FILE, "", 0} : tokens.back(),
                        "no generic template named '" + inst.templateName + "'");
        const GenericTemplate& tmpl = it->second;

        // Map each type parameter to its argument token slice (emplace = copy-construct;
        // Token is not copy-assignable due to its const members).
        std::unordered_map<std::string, std::vector<Token>> sub;
        for (size_t i = 0; i < tmpl.typeParams.size() && i < inst.args.size(); ++i)
            sub.emplace(tmpl.typeParams[i], inst.args[i]);

        // Substitute. Rename the declaration name and any constructor/destructor name
        // (a token == templateName that is NOT followed by '<'); self-references like
        // "Name<...>" are left for re-parse to mangle. Replace type-parameter tokens.
        std::vector<Token> out;
        for (size_t idx = 0; idx < tmpl.tokens.size(); ++idx) {
            const Token& t = tmpl.tokens[idx];
            if (t.type == TokenType::IDENTIFIER && t.lexeme == inst.templateName
                && (idx + 1 >= tmpl.tokens.size() || tmpl.tokens[idx + 1].type != TokenType::LESS)) {
                out.push_back(Token{ TokenType::IDENTIFIER, inst.mangledName, t.line });
                continue;
            }
            if (t.type == TokenType::IDENTIFIER) {
                auto sit = sub.find(t.lexeme);
                if (sit != sub.end()) {
                    for (const Token& a : sit->second) out.push_back(a);
                    continue;
                }
            }
            out.push_back(t);
        }
        out.push_back(Token{ TokenType::END_OF_FILE, "", 0 });

        // Re-parse the concrete declaration (may enqueue further instantiations).
        tokens  = std::move(out);
        current = 0;
        program.declarations.push_back(parseDeclaration());
    }
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
    std::string prefix = filename.empty() ? "" : filename + ":";
    std::string formatted = prefix + std::to_string(token.line) + ": error: " + msg;
    std::cerr << formatted << '\n';
    return ParseError(formatted);
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
            return classNames.count(peek().lexeme) > 0 || gen_->classNames.count(peek().lexeme) > 0;
        default:
            return false;
    }
}

Token Parser::consumeType() {
    Token base = advance();  // caller has verified isTypeName()
    std::string lexeme = base.lexeme;
    int         line   = base.line;
    bool        classLike = base.type == TokenType::IDENTIFIER;  // class / mangled instantiation

    // Typed raw pointer: ptr<T> -> synthesized "ptr<elem>" token (internal type).
    if (base.type == TokenType::PTR && check(TokenType::LESS)) {
        std::vector<std::vector<Token>> args = parseTypeArgList();
        std::string elem = args.empty() ? "" : argMangle(args[0]);
        return Token{ TokenType::IDENTIFIER, "ptr<" + elem + ">", line };
    }

    // Generic class instantiation: Name<args> -> mangled concrete class name.
    if (base.type == TokenType::IDENTIFIER && gen_->classNames.count(base.lexeme)
        && check(TokenType::LESS)) {
        std::vector<std::vector<Token>> args = parseTypeArgList();
        std::string mangled = mangleInstantiation(base.lexeme, args);
        recordInstantiation(base.lexeme, mangled, std::move(args));
        classNames.insert(mangled);   // the instantiation is now a concrete class name
        lexeme    = mangled;
        classLike = true;
    }

    if (check(TokenType::AMPERSAND)) {
        if (!(classLike && classNames.count(lexeme) > 0))
            throw error(peek(), "'&' reference type is only allowed on class types, not '" + lexeme + "'");
        advance();  // consume '&'
        // Synthesize a single reference-type token; resolvers decode the trailing '&'.
        return Token{ TokenType::IDENTIFIER, lexeme + "&", line };
    }

    if (lexeme != base.lexeme)  // a generic instantiation was mangled
        return Token{ TokenType::IDENTIFIER, lexeme, line };
    return base;
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
    size_t funcSpan = typeSpanAt(current);
    if (funcSpan > 0
        && current + funcSpan + 1 < tokens.size()
        && tokens[current + funcSpan].type == TokenType::IDENTIFIER
        && tokens[current + funcSpan + 1].type == TokenType::LEFT_PAREN
        && !(insideFunction
             && peek().type == TokenType::IDENTIFIER
             && (classNames.count(peek().lexeme) || gen_->classNames.count(peek().lexeme)))) {
        Token returnType = consumeType();
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
            Token paramType = consumeType();
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
            Token paramType = consumeType();
            Token paramName = consume(TokenType::IDENTIFIER, "expected parameter name");
            params.push_back(ParamDecl{ paramType, paramName });
        } while (match({ TokenType::COMMA }));
    }
    consume(TokenType::RIGHT_PAREN, "expected ')' after parameters");
    consume(TokenType::LEFT_BRACE,  "expected '{' before function body");

    bool savedInsideFunction = insideFunction;
    insideFunction = true;
    BlockStmt body = parseBlockBody();
    insideFunction = savedInsideFunction;
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
            bool savedDtor = insideFunction;
            insideFunction = true;
            BlockStmt dtorBody = parseBlockBody();
            insideFunction = savedDtor;

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
                    Token paramType = consumeType();
                    Token paramName = consume(TokenType::IDENTIFIER, "expected parameter name");
                    params.push_back(ParamDecl{ paramType, paramName });
                } while (match({ TokenType::COMMA }));
            }
            consume(TokenType::RIGHT_PAREN, "expected ')' after constructor parameters");
            consume(TokenType::LEFT_BRACE,  "expected '{' before constructor body");
            bool savedIF1 = insideFunction;
            insideFunction = true;
            BlockStmt body = parseBlockBody();
            insideFunction = savedIF1;

            methods.push_back(MethodDecl{
                isPublic, /*isConstructor=*/true, /*isDestructor=*/false,
                ctorName,   // returnType token = class name token (no actual return type)
                ctorName,   // name token
                std::move(params), std::move(body)
            });
            continue;
        }

        // Regular method or field: typeName[&] IDENTIFIER
        if (!isTypeName()) throw error(peek(), "expected type name for class member");
        Token memberType = consumeType();
        Token memberName = consume(TokenType::IDENTIFIER, "expected member name");

        if (check(TokenType::LEFT_PAREN)) {
            // Method
            consume(TokenType::LEFT_PAREN, "");
            std::vector<ParamDecl> params;
            if (!check(TokenType::RIGHT_PAREN)) {
                do {
                    if (!isTypeName()) throw error(peek(), "expected parameter type");
                    Token paramType = consumeType();
                    Token paramName = consume(TokenType::IDENTIFIER, "expected parameter name");
                    params.push_back(ParamDecl{ paramType, paramName });
                } while (match({ TokenType::COMMA }));
            }
            consume(TokenType::RIGHT_PAREN, "expected ')' after method parameters");
            consume(TokenType::LEFT_BRACE,  "expected '{' before method body");
            bool savedIF2 = insideFunction;
            insideFunction = true;
            BlockStmt body = parseBlockBody();
            insideFunction = savedIF2;

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
        body.push_back(std::make_unique<Stmt>(parseDeclaration()));
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
        } else if (check(TokenType::LEFT_PAREN)
                   && classNames.count(typeName.lexeme) > 0) {
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
        if (!isTypeName()) throw error(peek(), "expected type name after 'as'");
        Token targetType = advance();
        expr = makeExpr(CastExpr{ box(std::move(expr)), targetType });
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
        consume(TokenType::LEFT_PAREN, "expected '(' after class name in 'new' expression");
        std::vector<std::unique_ptr<Expr>> args;
        if (!check(TokenType::RIGHT_PAREN)) {
            do { args.push_back(box(parseExpression())); } while (match({ TokenType::COMMA }));
        }
        consume(TokenType::RIGHT_PAREN, "expected ')' after constructor arguments");
        return makeExpr(NewExpr{ keyword, className, std::move(args) });
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

    if (match({ TokenType::IDENTIFIER })) {
        Token name = previous();

        // Generic function call: name<typeArgs>(args)  →  mangled concrete call.
        if (gen_->funcNames.count(name.lexeme) && check(TokenType::LESS)) {
            std::vector<std::vector<Token>> typeArgs = parseTypeArgList();
            std::string mangled = mangleInstantiation(name.lexeme, typeArgs);
            recordInstantiation(name.lexeme, mangled, std::move(typeArgs));
            consume(TokenType::LEFT_PAREN, "expected '(' after generic type arguments");
            std::vector<std::unique_ptr<Expr>> genArgs;
            if (!check(TokenType::RIGHT_PAREN)) {
                do { genArgs.push_back(box(parseExpression())); } while (match({ TokenType::COMMA }));
            }
            consume(TokenType::RIGHT_PAREN, "expected ')' after arguments");
            return makeExpr(CallExpr{ Token{ TokenType::IDENTIFIER, mangled, name.line }, std::move(genArgs) });
        }

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
