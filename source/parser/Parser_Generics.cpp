//
// Created by Vladimir Arsenijevic on 28.5.2026.
//

#include "Parser.h"
#include "Parser_internal.h"
#include <iostream>


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
        // Enum names are also type names (no generic enums for now).
        if (toks[i].type == TokenType::ENUM && toks[i + 1].type == TokenType::IDENTIFIER)
            classNames.insert(toks[i + 1].lexeme);
    }
    // Register generic function template names so call sites are recognised regardless of
    // declaration order. Every generic function is `fn name < ... > ( ... )` — the name
    // follows `fn` directly and is immediately followed by '<'.
    for (size_t i = 0; i + 3 < toks.size(); ++i) {
        if (toks[i].type != TokenType::FN) continue;
        size_t nameIdx = i + 1;
        if (toks[nameIdx].type != TokenType::IDENTIFIER || toks[nameIdx + 1].type != TokenType::LESS)
            continue;
        size_t j = nameIdx + 2; int depth = 1;
        while (j < toks.size() && depth > 0) {
            if (toks[j].type == TokenType::LESS)             depth++;
            else if (toks[j].type == TokenType::GREATER)     depth--;
            else if (toks[j].type == TokenType::SHIFT_RIGHT) depth -= 2;
            j++;
        }
        if (j < toks.size() && toks[j].type == TokenType::LEFT_PAREN)
            gen_->funcNames.insert(toks[nameIdx].lexeme);
    }
}

void Parser::monomorphize(Program& program, const std::string& filenameStr) {
    filename = filenameStr;   // label any monomorphization parse error with the source file
    runMonomorphization(program);
}

// ============================================================
// Generics (monomorphization)
// ============================================================

bool Parser::tryCaptureFunctionTemplate() {
    // Unified generic function: `fn name < params > ( args ) [mut] [-> RetType [alias]] { body }`.
    // The name follows `fn` directly and precedes the '<' type-parameter list.
    if (peek().type != TokenType::FN) return false;
    size_t s = current + 1;   // first token after `fn`
    if (s + 1 >= tokens.size()
        || tokens[s].type != TokenType::IDENTIFIER
        || tokens[s + 1].type != TokenType::LESS)
        return false;
    size_t nameIdx = s;

    // Collect type-parameter names (and optional `: Trait + Trait` bounds) between
    // '<' and '>'; verify the decl continues with '('.
    std::vector<std::string>              typeParams;
    std::vector<std::vector<std::string>> bounds;
    size_t j = scanTypeParamList(s + 2, typeParams, bounds);
    if (j == 0 || tokens[j].type != TokenType::GREATER) return false;
    size_t afterGt = j + 1;
    if (afterGt >= tokens.size() || tokens[afterGt].type != TokenType::LEFT_PAREN) return false;
    if (typeParams.empty()) return false;

    // Capture with the `<...>` list stripped so the monomorphized re-parse sees an ordinary
    // declaration: `fn`, the name, the parameter list, then everything up to the body
    // (`[mut] [-> RetType [alias]]`, whose return type may reference a type parameter).
    std::vector<Token> captured;
    captured.push_back(tokens[current]);   // `fn`
    captured.push_back(tokens[nameIdx]);   // function name

    size_t k = afterGt;
    int parenDepth = 0;
    do {
        if (tokens[k].type == TokenType::LEFT_PAREN)  parenDepth++;
        else if (tokens[k].type == TokenType::RIGHT_PAREN) parenDepth--;
        captured.push_back(tokens[k]);
        ++k;
    } while (k < tokens.size() && parenDepth > 0);

    // Capture any `mut` and/or `-> RetType alias` between the ')' and the body.
    while (k < tokens.size() && tokens[k].type != TokenType::LEFT_BRACE) {
        captured.push_back(tokens[k]);
        ++k;
    }

    if (k >= tokens.size() || tokens[k].type != TokenType::LEFT_BRACE) return false;
    int braceDepth = 0;
    do {
        if (tokens[k].type == TokenType::LEFT_BRACE)  braceDepth++;
        else if (tokens[k].type == TokenType::RIGHT_BRACE) braceDepth--;
        captured.push_back(tokens[k]);
        ++k;
    } while (k < tokens.size() && braceDepth > 0);

    const std::string& name = tokens[nameIdx].lexeme;
    gen_->templates[name] = GenericTemplate{ std::move(typeParams), std::move(bounds), std::move(captured) };
    gen_->funcNames.insert(name);
    current = k;   // advance past the captured declaration
    return true;
}

std::optional<Token> Parser::readCallSigType(size_t& k) const {
    if (k >= tokens.size()) return std::nullopt;
    const Token& base = tokens[k];
    bool isType = false;
    switch (base.type) {
        case TokenType::I8:  case TokenType::I16: case TokenType::I32: case TokenType::I64:
        case TokenType::U8:  case TokenType::U16: case TokenType::U32: case TokenType::U64:
        case TokenType::F32: case TokenType::F64: case TokenType::BOOL:
        case TokenType::CHAR_TYPE: case TokenType::VOID: case TokenType::PTR: case TokenType::SELF:
            isType = true; break;
        case TokenType::IDENTIFIER:
            isType = classNames.count(base.lexeme) > 0 || gen_->classNames.count(base.lexeme) > 0;
            break;
        default: break;
    }
    if (!isType) return std::nullopt;
    size_t j = k + 1;
    if (j < tokens.size() && tokens[j].type == TokenType::AMPERSAND) {  // `T&` reference
        k = j + 1;
        return Token{ TokenType::IDENTIFIER, base.lexeme + "&", base.line };
    }
    k = j;
    return base;   // copy-construct (Token is copyable)
}

std::optional<std::string> Parser::scanCallBound(size_t& k) {
    k += 2;   // past `Call` `(`
    std::vector<Token> paramTypes;
    while (k < tokens.size() && tokens[k].type != TokenType::RIGHT_PAREN) {
        std::optional<Token> t = readCallSigType(k);
        if (!t) return std::nullopt;
        paramTypes.push_back(*t);
        if (k < tokens.size() && tokens[k].type == TokenType::COMMA) ++k;
    }
    if (k >= tokens.size() || tokens[k].type != TokenType::RIGHT_PAREN) return std::nullopt;
    int line = tokens[k].line;
    ++k;   // past `)`
    if (k < tokens.size() && tokens[k].type == TokenType::ARROW) {
        ++k;
        std::optional<Token> r = readCallSigType(k);
        if (!r) return std::nullopt;
        return canonicalCallTrait(paramTypes, *r);
    }
    return canonicalCallTrait(paramTypes, Token{ TokenType::VOID, "void", line });
}

// Resolve a Call-signature type token to a Type for mangling: a synthesized "Class&"/"ptr<…>",
// a primitive/void keyword, or a class-name identifier. Mirrors the semantic/codegen resolvers.
static Type callSigTokenType(const Token& t) {
    Type d = decodeSynthesizedType(t);            // "Class&", "ptr<Elem>"
    if (d.kind != TypeKind::Error) return d;
    Type p = typeFromToken(t.type);               // primitives / void / opaque ptr
    if (p.kind != TypeKind::Error) return p;
    if (t.type == TokenType::IDENTIFIER || t.type == TokenType::SELF)
        return makeObjectType(t.lexeme);          // class (or already-mangled) name
    return Type{TypeKind::Error};
}

std::string Parser::canonicalCallTrait(const std::vector<Token>& paramTypeTokens, const Token& retType) {
    std::vector<Type> params;
    params.reserve(paramTypeTokens.size());
    for (const Token& t : paramTypeTokens) params.push_back(callSigTokenType(t));
    Type ret = callSigTokenType(retType);
    std::string name = mangleOverload("Call", params, ret);   // Call$P…$ret$R
    // Remember the signature's tokens so an untyped lambda argument can infer from this bound.
    gen_->callTraitSigs.emplace(name, std::make_pair(paramTypeTokens, retType));
    if (gen_->emittedCallTraits.insert(name).second) {
        // Generate `trait <name> { fn call(P a0, …) -> R; }` (bodyless required method).
        std::vector<ParamDecl> callParams;
        for (size_t i = 0; i < paramTypeTokens.size(); ++i)
            callParams.push_back(ParamDecl{ paramTypeTokens[i],
                Token{ TokenType::IDENTIFIER, "a" + std::to_string(i), retType.line }, false, nullptr });
        std::deque<MethodDecl> methods;
        methods.push_back(MethodDecl{
            /*isPublic=*/true, /*isConstructor=*/false, /*isDestructor=*/false, /*isStatic=*/false,
            /*isMut=*/false, /*hasBody=*/false, retType,
            Token{ TokenType::IDENTIFIER, "call", retType.line },
            std::move(callParams), BlockStmt{}, /*hasReturnSlot=*/false, "" });
        pendingCallTraits_.push_back(makeStmt(TraitDeclStmt{
            Token{ TokenType::IDENTIFIER, name, retType.line }, std::move(methods) }));
    }
    return name;
}

// Scan a `<T, U: Trait + Trait2, ...>` type-parameter list starting at token index
// `from` (the first token after '<'). Fills `typeParams` and a parallel `bounds`
// (bounds[i] = trait names on typeParams[i], empty if unbounded). Returns the index
// of the closing '>' token, or 0 if the list is malformed / unterminated.
// A `Call(P…)->R` bound is canonicalized to a mangled `Call$…` trait name.
size_t Parser::scanTypeParamList(size_t from, std::vector<std::string>& typeParams,
                                 std::vector<std::vector<std::string>>& bounds) {
    bool   expectParam = true;
    int    depth = 1;
    size_t j = from;
    for (; j < tokens.size() && depth > 0; ++j) {
        TokenType tt = tokens[j].type;
        if (tt == TokenType::LESS)             { depth++; continue; }
        if (tt == TokenType::GREATER)          { depth--; if (depth == 0) break; continue; }
        if (tt == TokenType::SHIFT_RIGHT)      { depth -= 2; if (depth <= 0) return 0; continue; }
        if (depth != 1) continue;
        if (tt == TokenType::COMMA)            { expectParam = true; }
        else if (tt == TokenType::IDENTIFIER) {
            if (expectParam) { typeParams.push_back(tokens[j].lexeme); bounds.push_back({}); expectParam = false; }
            // `Call(P…)->R` bound → canonical `Call$…` trait name.
            else if (tokens[j].lexeme == "Call" && !bounds.empty()
                     && j + 1 < tokens.size() && tokens[j + 1].type == TokenType::LEFT_PAREN) {
                size_t k = j;
                std::optional<std::string> canon = scanCallBound(k);
                if (!canon) return 0;
                bounds.back().push_back(*canon);
                j = k - 1;   // the loop's ++j resumes after the signature
            }
            else if (!bounds.empty()) bounds.back().push_back(tokens[j].lexeme);
        }
        // COLON / PLUS are separators inside a bound list — ignored.
    }
    if (j >= tokens.size() || tokens[j].type != TokenType::GREATER) return 0;
    return j;
}

bool Parser::tryCaptureClassTemplate() {
    if (!(peek().type == TokenType::CLASS
          && peekNext().type == TokenType::IDENTIFIER
          && current + 2 < tokens.size() && tokens[current + 2].type == TokenType::LESS))
        return false;

    std::vector<std::string>              typeParams;
    std::vector<std::vector<std::string>> bounds;
    size_t j = scanTypeParamList(current + 3, typeParams, bounds);
    if (j == 0 || tokens[j].type != TokenType::GREATER) return false;
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
    gen_->templates[name] = GenericTemplate{ std::move(typeParams), std::move(bounds), std::move(captured) };
    gen_->classNames.insert(name);
    current = k;
    return true;
}

bool Parser::tryCaptureImplTemplate() {
    // Generic impl: `impl<T…> Trait for Class<T…> { … }`.  Only fires on `impl <` — a plain
    // `impl Trait for Type { … }` still goes through parseImplDecl unchanged.
    if (!(peek().type == TokenType::IMPL
          && current + 1 < tokens.size() && tokens[current + 1].type == TokenType::LESS))
        return false;

    std::vector<std::string>              typeParams;
    std::vector<std::vector<std::string>> bounds;
    size_t j = scanTypeParamList(current + 2, typeParams, bounds);   // j = closing '>'
    if (j == 0 || tokens[j].type != TokenType::GREATER || typeParams.empty()) return false;

    auto isTypeParam = [&](const std::string& s) {
        for (const auto& p : typeParams) if (p == s) return true;
        return false;
    };
    auto at = [&](size_t idx) -> const Token& {
        return tokens[idx < tokens.size() ? idx : tokens.size() - 1];
    };

    // Header: `<Trait> for <Class> < args >`.
    size_t p = j + 1;
    if (at(p).type != TokenType::IDENTIFIER)
        throw error(at(p), "expected a trait name after 'impl<…>'");
    if (at(p + 1).type != TokenType::FOR)
        throw error(at(p + 1), "expected 'for' in 'impl<…> <Trait> for <Type>'");
    size_t classIdx = p + 2;
    if (at(classIdx).type != TokenType::IDENTIFIER)
        throw error(at(classIdx), "expected a class name after 'for'");
    std::string targetClass = tokens[classIdx].lexeme;
    if (at(classIdx + 1).type != TokenType::LESS)
        throw error(at(classIdx + 1), "a generic impl must target a generic type: write "
            "`impl<T> " + tokens[p].lexeme + " for " + targetClass + "<T>`");

    // Target type arguments — each must be exactly one of the impl's own type parameters.
    std::vector<std::string> targetParamAtPos;
    size_t q = classIdx + 2;
    while (q < tokens.size() && tokens[q].type != TokenType::GREATER) {
        if (tokens[q].type == TokenType::COMMA) { ++q; continue; }
        bool ok = tokens[q].type == TokenType::IDENTIFIER && isTypeParam(tokens[q].lexeme)
               && (tokens[q + 1 < tokens.size() ? q + 1 : q].type == TokenType::COMMA
                   || tokens[q + 1 < tokens.size() ? q + 1 : q].type == TokenType::GREATER);
        if (!ok)
            throw error(at(q), "a generic impl target must name each type parameter directly, e.g. "
                "`impl<T> " + tokens[p].lexeme + " for " + targetClass
                + "<T>` — nested or concrete type arguments are not supported here");
        targetParamAtPos.push_back(tokens[q].lexeme);
        ++q;
    }
    if (q >= tokens.size() || tokens[q].type != TokenType::GREATER)
        throw error(at(q), "expected '>' to close the impl target type");
    size_t braceIdx = q + 1;
    if (at(braceIdx).type != TokenType::LEFT_BRACE)
        throw error(at(braceIdx), "expected '{' after the impl header");

    // Capture `impl <Trait> for <Class<…>> { … }` WITHOUT the `impl<…>` header, so a
    // re-parse after substitution is an ordinary impl.
    std::vector<Token> captured;
    captured.push_back(tokens[current]);                     // 'impl'
    for (size_t idx = p; idx <= q; ++idx) captured.push_back(tokens[idx]);   // Trait for Class < … >

    size_t k = braceIdx;
    int braceDepth = 0;
    do {
        if (tokens[k].type == TokenType::LEFT_BRACE)       braceDepth++;
        else if (tokens[k].type == TokenType::RIGHT_BRACE) braceDepth--;
        captured.push_back(tokens[k]);
        ++k;
    } while (k < tokens.size() && braceDepth > 0);

    gen_->implTemplates.push_back(GenericImplTemplate{
        std::move(typeParams), std::move(bounds), std::move(targetClass),
        std::move(targetParamAtPos), std::move(captured), tokens[p].lexeme });
    current = k;   // past the closing '}'
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
    // reference suffix: `&` (owning heap reference) or `*` (non-owning borrow)
    if (i < tokens.size()
        && (tokens[i].type == TokenType::AMPERSAND || tokens[i].type == TokenType::STAR)) ++i;
    // nullable suffix: `T?`
    if (i < tokens.size() && tokens[i].type == TokenType::QUESTION) ++i;
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

std::string Parser::genericArgBaseName(const Token& t) {
    std::string s = t.lexeme;
    if (!s.empty() && s.back() == '?') s.pop_back();          // nullable suffix
    if (s.rfind("ref:", 0) == 0) s = s.substr(4);             // `T*` borrow → "ref:T"
    else if (!s.empty() && s.back() == '&') s.pop_back();     // `T&` owning reference
    return s;
}

bool Parser::inferGenericTypeArgs(const std::string& fnName,
                                  const std::vector<std::unique_ptr<Expr>>& args,
                                  std::vector<std::vector<Token>>& out) {
    auto it = gen_->templates.find(fnName);
    if (it == gen_->templates.end() || it->second.typeParams.empty()) return false;
    const GenericTemplate& tmpl = it->second;
    std::unordered_set<std::string> tparams(tmpl.typeParams.begin(), tmpl.typeParams.end());

    // 1. Scan the template's parameter list (raw tokens; params are not pre-parsed) to find, for
    //    each parameter position, whether its type is a bare type parameter (`T` / `T&` / `T*` /
    //    `T?`). Record paramPos → type-param name for those.
    const std::vector<Token>& toks = tmpl.tokens;
    size_t lp = 0;
    while (lp < toks.size() && toks[lp].type != TokenType::LEFT_PAREN) ++lp;
    if (lp == toks.size()) return false;

    std::unordered_map<size_t, std::string> paramTypeParam;   // paramPos → type-param name
    size_t depth = 1, i = lp + 1, paramPos = 0, groupStart = i;
    auto analyzeGroup = [&](size_t s, size_t e) {
        if (s >= e) return;
        size_t j = s;
        if (toks[j].lexeme == "mut") ++j;                      // optional `mut`
        if (j >= e || toks[j].type != TokenType::IDENTIFIER) return;   // primitives are keywords, skip
        if (!tparams.count(toks[j].lexeme)) return;            // not a bare type parameter
        const std::string tp = toks[j].lexeme;
        ++j;
        while (j < e && (toks[j].type == TokenType::AMPERSAND || toks[j].type == TokenType::STAR
                         || toks[j].type == TokenType::QUESTION)) ++j;   // trailing sigils
        if (j < e && toks[j].type == TokenType::IDENTIFIER)    // then the param name → a match
            paramTypeParam.emplace(paramPos, tp);
    };
    for (; i < toks.size() && depth > 0; ++i) {
        TokenType tt = toks[i].type;
        if (tt == TokenType::LEFT_PAREN || tt == TokenType::LESS) ++depth;
        else if (tt == TokenType::GREATER) { if (depth > 1) --depth; }
        else if (tt == TokenType::RIGHT_PAREN) {
            if (--depth == 0) { analyzeGroup(groupStart, i); break; }
        } else if (tt == TokenType::COMMA && depth == 1) {
            analyzeGroup(groupStart, i);
            groupStart = i + 1;
            ++paramPos;
        }
    }

    // 2. For each type parameter (in order), read the type off its matched positional argument.
    for (const std::string& tp : tmpl.typeParams) {
        size_t pos = SIZE_MAX;
        for (const auto& [p, name] : paramTypeParam) if (name == tp) { pos = std::min(pos, p); }
        if (pos == SIZE_MAX || pos >= args.size() || !args[pos]) return false;
        const Expr* n = args[pos].get();
        std::string base;
        // A binding's recorded type token whose kind is a usable base name. A `var`-typed local
        // records the `var` SENTINEL (its real type is only deduced later, in semantics, after
        // monomorphization), so it is NOT inferable here — treat it as un-inferable so the call
        // gets the clean "specify explicitly" error rather than a bogus `f$var` instantiation.
        auto baseFrom = [](const Token& t) -> std::string {
            return t.type == TokenType::VAR ? std::string{} : genericArgBaseName(t);
        };
        if (const auto* id = std::get_if<IdentifierExpr>(n->node.get())) {
            const Token* found = nullptr;
            for (size_t k = scopes_.size(); k-- > 0 && !found; ) {   // innermost binding wins (shadowing)
                auto sit = scopes_[k].find(id->name.lexeme);
                if (sit != scopes_[k].end()) found = &sit->second;
            }
            if (!found) {                                            // else an enclosing instance field
                auto fit = classFieldScope_.find(id->name.lexeme);
                if (fit != classFieldScope_.end()) found = &fit->second;
            }
            if (found) base = baseFrom(*found);                      // empty for a `var` binding → un-inferable
        } else if (const auto* c = std::get_if<CallExpr>(n->node.get())) {
            if (classNames.count(c->callee.lexeme) || gen_->classNames.count(c->callee.lexeme))
                base = c->callee.lexeme;                       // a `Class(...)` constructor call
        } else if (const auto* ne = std::get_if<NewExpr>(n->node.get())) {
            base = ne->className.lexeme;                       // `new Class(...)`
        }
        if (base.empty()) return false;                        // un-inferable argument
        out.push_back(std::vector<Token>{ Token{ TokenType::IDENTIFIER, base, 0 } });
    }
    return true;
}

void Parser::runMonomorphization(Program& program) {
    // Every queued instantiation's mangled name is a concrete class name during
    // re-parse (the per-file parsers that recorded them are gone, so seed here).
    for (const auto& mangled : gen_->instantiated) classNames.insert(mangled);
    for (const auto& lname : gen_->lambdaClassNames) classNames.insert(lname);  // generated lambda classes

    // Impl specialization — a CONCRETE `impl Trait for Class<i32>` overrides the BLANKET
    // `impl<T> Trait for Class<T>` for that exact type ("most specific wins", the only tier GG can
    // express since partial specialization isn't a thing here). Collect every explicit concrete
    // impl's `Trait@MangledClass` key (the user-written impls are already parsed into the program;
    // their `Class<i32>` target was mangled to `Class$i32` by consumeType) so that below we skip
    // instantiating a blanket impl for a class a concrete impl already covers — otherwise both would
    // emit `Class$i32::method` and collide as a duplicate.
    std::unordered_set<std::string> concreteImplKeys;
    for (const Stmt& s : program.declarations)
        if (const auto* im = std::get_if<ImplDeclStmt>(s.node.get())) {
            concreteImplKeys.insert(im->traitName.lexeme + "@" + im->typeName.lexeme);
            // A concrete `impl Call for X` canonicalizes its trait to the signature-mangled
            // `Call$…$ret$…`, whereas a blanket `impl<T> Call for X<T>` stores the RAW `Call` in its
            // template. Register a raw-`Call@class` alias so the concrete still suppresses the
            // blanket for that class (any concrete `call` on a class collides with a blanket `call`).
            if (im->traitName.lexeme.rfind("Call$", 0) == 0)
                concreteImplKeys.insert("Call@" + im->typeName.lexeme);
        }

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

        // Record trait-bound obligations: each bounded type parameter's concrete
        // argument must implement the named trait(s). Verified in the semantic pass
        // (static dispatch — the type param itself never reaches semantics/codegen).
        for (size_t i = 0; i < tmpl.typeParams.size() && i < inst.args.size(); ++i) {
            if (i >= tmpl.bounds.size() || tmpl.bounds[i].empty() || inst.args[i].empty()) continue;
            const Token& argHead = inst.args[i].front();   // base type token ('&' is separate)
            for (const std::string& tr : tmpl.bounds[i])
                program.genericBoundChecks.push_back(
                    GenericBoundCheck{ argHead.lexeme, tr, inst.mangledName, argHead.line });
        }

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

        // A generic CLASS instantiation also instantiates every generic `impl … for Class<…>`
        // with the same type arguments, so e.g. `Array<i32>` gets its `Index` impl.
        if (gen_->classNames.count(inst.templateName)) {
            for (size_t ti = 0; ti < gen_->implTemplates.size(); ++ti) {
                const GenericImplTemplate& impl = gen_->implTemplates[ti];
                if (impl.targetClass != inst.templateName) continue;
                if (impl.targetParamAtPos.size() != inst.args.size()) continue;  // arity mismatch

                // Specialization: a concrete `impl Trait for Class$args` overrides this blanket one.
                if (concreteImplKeys.count(impl.traitName + "@" + inst.mangledName)) continue;

                std::string key = std::to_string(ti) + "@" + inst.mangledName;
                if (!gen_->implInstantiated.insert(key).second) continue;        // already done

                // impl type-parameter name → the class's concrete argument tokens (positional).
                std::unordered_map<std::string, std::vector<Token>> isub;
                for (size_t pos = 0; pos < impl.targetParamAtPos.size(); ++pos)
                    isub.emplace(impl.targetParamAtPos[pos], inst.args[pos]);

                std::vector<Token> iout;
                for (const Token& t : impl.tokens) {
                    if (t.type == TokenType::IDENTIFIER) {
                        auto sit = isub.find(t.lexeme);
                        if (sit != isub.end()) { for (const Token& a : sit->second) iout.push_back(a); continue; }
                    }
                    iout.push_back(t);
                }
                iout.push_back(Token{ TokenType::END_OF_FILE, "", 0 });

                // Re-parse the concrete impl (its `Class<args>` target mangles to the same
                // `Class$args` and attaches to that class; may enqueue further instantiations).
                tokens  = std::move(iout);
                current = 0;
                program.declarations.push_back(parseDeclaration());
            }
        }
    }

    // Surface bounded generic templates for definition-time body checking (semantic pass
    // checkGenericBodies). Re-parse each template's ORIGINAL body (no type substitution) with
    // its type-parameter names registered as types, using a throwaway parser with its OWN
    // registry so the parse cannot perturb this monomorphization or enqueue real instantiations.
    for (const auto& [tmplName, tmpl] : gen_->templates) {
        bool hasBound = false;
        for (const auto& b : tmpl.bounds) if (!b.empty()) { hasBound = true; break; }
        if (!hasBound) continue;   // only bounded templates are checked

        std::unordered_set<std::string> scratchClasses = classNames;
        for (const std::string& tp : tmpl.typeParams) scratchClasses.insert(tp);

        std::vector<Token> toks(tmpl.tokens);
        toks.push_back(Token{ TokenType::END_OF_FILE, "", 0 });

        try {
            Parser scratch(scratchClasses, /*sharedRegistry=*/nullptr);
            Program bodyProg = scratch.parse(toks, filename, /*runMonomorphization=*/false);
            if (!bodyProg.declarations.empty() && bodyProg.declarations.front().node)
                program.genericTemplates.push_back(GenericTemplateDecl{
                    std::move(bodyProg.declarations.front()), tmpl.typeParams, tmpl.bounds });
        } catch (const CompileError&) {
            // Body could not be re-parsed for checking — skip silently. Monomorphized
            // instantiations are still analyzed normally; only a definition-time diagnostic is lost.
        }
    }

    // Compile-time reflection: now that every class (incl. monomorphized ones) is in the program,
    // expand every `inline for (v in @fields(T))` into ordinary statements.
    expandReflection(program);
}