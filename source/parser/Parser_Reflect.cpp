//
// Created by Vladimir Arsenijevic on 28.5.2026.
//

#include "Parser.h"
#include <iostream>


// ============================================================
// Compile-time reflection expansion
// ============================================================

// Reduce a type token's lexeme to its base class name: drop a trailing '?' (nullable), a leading
// "ref:" (borrow), and a trailing '&' (owning reference).
static std::string reflectBaseClassName(std::string s) {
    if (!s.empty() && s.back() == '?') s.pop_back();
    if (s.rfind("ref:", 0) == 0)       s = s.substr(4);
    if (!s.empty() && s.back() == '&') s.pop_back();
    return s;
}

// Substitute the inline-for binding in one field's copy of the captured body tokens:
//   loopVar.name        -> a STRING literal holding the field name
//   @field(obj, name)   -> (obj).<field>   (name = `loopVar.name` or a string literal)
// Bare `loopVar` or `loopVar.<other>` is an error (Phase 1 exposes only `.name`).
std::vector<Token> Parser::substituteInlineForBody(const std::vector<Token>& body,
                                                   const std::string& loopVar,
                                                   const std::string& memberName,
                                                   bool overVariants,
                                                   const std::string& enumName,
                                                   const InlineAnnCtx& ann) {
    const std::string& fieldName = memberName;   // alias for the field-mode paths below
    std::vector<Token> out;
    size_t i = 0;
    while (i < body.size()) {
        const Token& t = body[i];

        // @field(obj, name) -> (obj).<field>
        if (!overVariants && t.type == TokenType::AT
            && i + 2 < body.size()
            && body[i + 1].type == TokenType::IDENTIFIER && body[i + 1].lexeme == "field"
            && body[i + 2].type == TokenType::LEFT_PAREN) {
            size_t j = i + 3;
            int  depth  = 1;
            bool inName = false;
            std::vector<Token> objToks, nameToks;
            for (; j < body.size(); ++j) {
                const Token& u = body[j];
                if      (u.type == TokenType::LEFT_PAREN)  depth++;
                else if (u.type == TokenType::RIGHT_PAREN) { depth--; if (depth == 0) break; }
                if (depth == 1 && u.type == TokenType::COMMA && !inName) { inName = true; continue; }
                (inName ? nameToks : objToks).push_back(u);
            }
            if (j >= body.size())
                throw error(body[i + 1], "unbalanced '@field(...)' in an 'inline for' body");
            bool nameIsThisBinding =
                nameToks.size() == 3 && nameToks[0].type == TokenType::IDENTIFIER
                && nameToks[0].lexeme == loopVar && nameToks[1].type == TokenType::DOT
                && nameToks[2].type == TokenType::IDENTIFIER && nameToks[2].lexeme == "name";
            bool nameIsString = nameToks.size() == 1 && nameToks[0].type == TokenType::STRING;
            if (!nameIsThisBinding && !nameIsString) {
                // The name refers to a DIFFERENT binding (a nested `inline for`) — leave the whole
                // @field(...) verbatim for that loop's own expansion pass; only substitute this
                // binding's f-references within the object sub-expression.
                std::vector<Token> objSub = substituteInlineForBody(objToks, loopVar, memberName, overVariants, enumName, ann);
                int line = t.line;
                out.push_back(t);                 // @
                out.push_back(body[i + 1]);        // field
                out.push_back(body[i + 2]);        // (
                for (const Token& o : objSub) out.push_back(o);
                out.push_back(Token{ TokenType::COMMA, ",", line });
                for (const Token& nm : nameToks)  out.push_back(nm);
                out.push_back(Token{ TokenType::RIGHT_PAREN, ")", line });
                i = j + 1;
                continue;
            }
            std::string accessName = nameIsThisBinding ? fieldName : nameToks[0].lexeme;
            std::vector<Token> objSub = substituteInlineForBody(objToks, loopVar, memberName, overVariants, enumName, ann);
            int line = t.line;
            out.push_back(Token{ TokenType::LEFT_PAREN,  "(", line });
            for (const Token& o : objSub) out.push_back(o);
            out.push_back(Token{ TokenType::RIGHT_PAREN, ")", line });
            out.push_back(Token{ TokenType::DOT,         ".", line });
            out.push_back(Token{ TokenType::IDENTIFIER,  accessName, line });
            i = j + 1;
            continue;
        }

        if (t.type == TokenType::IDENTIFIER && t.lexeme == loopVar) {
            // `v.has(Ann)` -> a bool literal: does the current member carry annotation `Ann`?
            if (i + 5 < body.size() && body[i + 1].type == TokenType::DOT
                && body[i + 2].type == TokenType::IDENTIFIER && body[i + 2].lexeme == "has"
                && body[i + 3].type == TokenType::LEFT_PAREN
                && body[i + 4].type == TokenType::IDENTIFIER
                && body[i + 5].type == TokenType::RIGHT_PAREN) {
                const std::string& annName = body[i + 4].lexeme;
                if (!ann.annTypes.count(annName))
                    throw error(body[i + 4], "unknown annotation '" + annName + "' in '"
                                + loopVar + ".has(" + annName + ")'");
                bool present = ann.memberAnns.count(annName) > 0;
                out.push_back(Token{ present ? TokenType::TRUE : TokenType::FALSE,
                                     present ? "true" : "false", t.line });
                i += 6;
                continue;
            }
            // `v.get(Ann).field` -> the const value of that annotation field (spliced arg tokens).
            if (i + 7 < body.size() && body[i + 1].type == TokenType::DOT
                && body[i + 2].type == TokenType::IDENTIFIER && body[i + 2].lexeme == "get"
                && body[i + 3].type == TokenType::LEFT_PAREN
                && body[i + 4].type == TokenType::IDENTIFIER
                && body[i + 5].type == TokenType::RIGHT_PAREN
                && body[i + 6].type == TokenType::DOT
                && body[i + 7].type == TokenType::IDENTIFIER) {
                const std::string& annName = body[i + 4].lexeme;
                const std::string& fld     = body[i + 7].lexeme;
                if (!ann.annTypes.count(annName))
                    throw error(body[i + 4], "unknown annotation '" + annName + "' in '" + loopVar + ".get'");
                auto fIt = ann.annFields.find(annName);
                if (fIt == ann.annFields.end())
                    throw error(body[i + 4], "'" + annName + "' is not an annotation type");
                const std::vector<std::string>& fnames = fIt->second;
                size_t idx = 0; bool found = false;
                for (; idx < fnames.size(); ++idx) if (fnames[idx] == fld) { found = true; break; }
                if (!found)
                    throw error(body[i + 7], "annotation '" + annName + "' has no field '" + fld + "'");
                // Present on this member → splice the real arg tokens. Absent → splice a default
                // placeholder of the field's type: only reachable in a false-`f.has`-guarded dead
                // branch (comptime-if pruning is deferred), so the value is never actually used.
                auto argIt = ann.memberArgs.find(annName);
                if (argIt != ann.memberArgs.end() && idx < argIt->second.size()) {
                    for (const Token& tk : argIt->second[idx]) out.push_back(tk);
                } else {
                    auto dIt = ann.annFieldDefaults.find(annName);
                    Token def = (dIt != ann.annFieldDefaults.end() && idx < dIt->second.size())
                                ? dIt->second[idx] : Token{ TokenType::NUMBER, "0", t.line };
                    out.push_back(def);
                }
                i += 8;
                continue;
            }
            // `v.name` -> the member-name string (both fields and variants). `name` is reserved for
            // the reflection name even if a variant type happens to declare a field called `name`.
            if (i + 2 < body.size() && body[i + 1].type == TokenType::DOT
                && body[i + 2].type == TokenType::IDENTIFIER && body[i + 2].lexeme == "name") {
                out.push_back(Token{ TokenType::STRING, memberName, t.line });
                i += 3;
                continue;
            }
            // For @variants the binding IS the variant singleton `Enum::member` — a real value, so
            // bare `v` and any `v.method()`/`v.field` are valid (the trailing `.member` access stays
            // and applies to the substituted singleton).
            if (overVariants) {
                out.push_back(Token{ TokenType::IDENTIFIER,   enumName,   t.line });
                out.push_back(Token{ TokenType::COLON_COLON,  "::",       t.line });
                out.push_back(Token{ TokenType::IDENTIFIER,   memberName, t.line });
                ++i;
                continue;
            }
            // For @fields the binding has no standalone value (a field needs an object — use
            // `@field(obj, v.name)`); only `v.name` is meaningful.
            if (i + 2 < body.size() && body[i + 1].type == TokenType::DOT
                && body[i + 2].type == TokenType::IDENTIFIER)
                throw error(body[i + 2], "only '" + loopVar + ".name' is supported in 'inline for' (got '."
                            + body[i + 2].lexeme + "')");
            throw error(t, "reflection binding '" + loopVar + "' can only be used as '"
                        + loopVar + ".name' or inside '@field(...)'");
        }

        out.push_back(t);
        ++i;
    }
    return out;
}

void Parser::expandReflection(Program& program) {
    // 1. className -> ordered instance-field names; enumName -> ordered variant names; plus the
    //    annotation types and per-member annotation names (for `f.has` / validation).
    ReflectRegistry reg;
    for (Stmt& d : program.declarations)
        if (d.node)
            if (const auto* an = std::get_if<AnnotationDeclStmt>(d.node.get())) {
                reg.annotationTypes.insert(an->name.lexeme);
                std::vector<std::string> fnames;
                std::vector<Token>       defaults;
                for (const FieldDecl& f : an->fields) {
                    fnames.push_back(f.name.lexeme);
                    // Type-appropriate default placeholder for `f.get` on an absent (dead-guarded) member.
                    switch (f.typeName.type) {
                        case TokenType::STR:  defaults.push_back(Token{ TokenType::STRING, "", f.name.line }); break;
                        case TokenType::BOOL: defaults.push_back(Token{ TokenType::FALSE, "false", f.name.line }); break;
                        // A CHAR literal with an empty lexeme lowers to code point 0; a numeric `0` here
                        // would be adopted into `char` and warn ("does not fit in 'char'") in the dead
                        // guarded branch that reaches this placeholder.
                        case TokenType::CHAR: defaults.push_back(Token{ TokenType::CHAR, "", f.name.line }); break;
                        default:              defaults.push_back(Token{ TokenType::NUMBER, "0", f.name.line }); break;
                    }
                }
                reg.annotationFields[an->name.lexeme] = std::move(fnames);
                reg.annotationFieldDefaults[an->name.lexeme] = std::move(defaults);
            }
    auto noteAnns = [&](const std::string& type, const std::string& member,
                        const std::deque<AnnotationApp>& apps) {
        for (const AnnotationApp& a : apps) {
            std::string key = type + "#" + member;
            reg.memberAnnotations[key].insert(a.name.lexeme);
            // emplace (copy-CONSTRUCT) — Token has no copy-assignment, so `map[k] = …` won't compile.
            reg.memberAnnotationArgs[key].emplace(a.name.lexeme, a.argTokens);   // positional arg tokens
        }
    };
    for (Stmt& d : program.declarations) {
        if (!d.node) continue;
        if (const auto* c = std::get_if<ClassDeclStmt>(d.node.get())) {
            std::vector<std::string> names;
            for (const FieldDecl& f : c->fields)
                if (!f.isStatic) { names.push_back(f.name.lexeme); noteAnns(c->name.lexeme, f.name.lexeme, f.annotations); }
            reg.fields[c->name.lexeme] = std::move(names);
        } else if (const auto* e = std::get_if<EnumDeclStmt>(d.node.get())) {
            std::vector<std::string> vs;
            for (const EnumVariant& v : e->variants) { vs.push_back(v.name.lexeme); noteAnns(e->name.lexeme, v.name.lexeme, v.annotations); }
            reg.variants[e->name.lexeme] = std::move(vs);
        }
    }
    // 2. expand inline-for everywhere.
    for (Stmt& d : program.declarations)
        expandReflectionInStmt(d, reg);
}

void Parser::expandReflectionInBlock(BlockStmt& block, const ReflectRegistry& reg) {
    for (auto& s : block.body) if (s) expandReflectionInStmt(*s, reg);
}

void Parser::expandReflectionInStmt(Stmt& s, const ReflectRegistry& reg) {
    if (!s.node) return;

    if (std::holds_alternative<InlineForStmt>(*s.node)) {
        InlineForStmt inf = std::move(std::get<InlineForStmt>(*s.node));
        std::string cls = reflectBaseClassName(inf.targetType.lexeme);
        const auto& table = inf.overVariants ? reg.variants : reg.fields;
        auto it = table.find(cls);
        if (it == table.end())
            throw error(inf.targetType, std::string("'inline for' over '@")
                        + (inf.overVariants ? "variants(" : "fields(") + inf.targetType.lexeme
                        + ")': '" + cls + "' is not "
                        + (inf.overVariants ? "an enum type" : "a class type"));

        // Reject break/continue directly in the body (depth 1) — after expansion they would
        // silently bind to an enclosing runtime loop.
        {
            int depth = 0;
            for (const Token& t : inf.bodyTokens) {
                if      (t.type == TokenType::LEFT_BRACE)  depth++;
                else if (t.type == TokenType::RIGHT_BRACE) depth--;
                else if (depth == 1 && (t.type == TokenType::BREAK || t.type == TokenType::CONTINUE))
                    throw error(t, "'" + t.lexeme + "' is not allowed directly inside an 'inline for'");
            }
        }

        static const std::unordered_set<std::string> kNoAnns;
        static const std::unordered_map<std::string, std::vector<std::vector<Token>>> kNoArgs;
        std::vector<std::unique_ptr<Stmt>> perField;
        for (const std::string& fname : it->second) {
            std::string key = cls + "#" + fname;
            auto maIt  = reg.memberAnnotations.find(key);
            auto argIt = reg.memberAnnotationArgs.find(key);
            const std::unordered_set<std::string>& memberAnns =
                maIt != reg.memberAnnotations.end() ? maIt->second : kNoAnns;
            const std::unordered_map<std::string, std::vector<std::vector<Token>>>& memberArgs =
                argIt != reg.memberAnnotationArgs.end() ? argIt->second : kNoArgs;
            InlineAnnCtx annCtx{ reg.annotationTypes, memberAnns, memberArgs,
                                 reg.annotationFields, reg.annotationFieldDefaults };
            std::vector<Token> sub = substituteInlineForBody(inf.bodyTokens, inf.loopVar.lexeme,
                                                             fname, inf.overVariants, cls, annCtx);
            sub.push_back(Token{ TokenType::END_OF_FILE, "", inf.keyword.line });
            // Re-parse the substituted body as a block (save/restore the token cursor).
            std::vector<Token> savedTokens = std::move(tokens);
            size_t savedCurrent = current;
            tokens  = std::move(sub);
            current = 0;
            Stmt blockStmt = parseStatement();   // leading '{' -> parseBlock
            tokens  = std::move(savedTokens);
            current = savedCurrent;
            perField.push_back(std::make_unique<Stmt>(std::move(blockStmt)));
        }
        *s.node = BlockStmt{ std::move(perField) };
        expandReflectionInBlock(std::get<BlockStmt>(*s.node), reg);   // nested inline-for
        return;
    }

    Stmt::Variant& v = *s.node;
    if (auto* n = std::get_if<BlockStmt>(&v))        { expandReflectionInBlock(*n, reg); return; }
    if (auto* n = std::get_if<IfStmt>(&v))           {
        if (n->thenBranch) expandReflectionInStmt(*n->thenBranch, reg);
        if (n->elseBranch) expandReflectionInStmt(*n->elseBranch, reg);
        return;
    }
    if (auto* n = std::get_if<WhileStmt>(&v))        { if (n->body) expandReflectionInStmt(*n->body, reg); return; }
    if (auto* n = std::get_if<ForStmt>(&v))          {
        if (n->init) expandReflectionInStmt(*n->init, reg);
        if (n->body) expandReflectionInStmt(*n->body, reg);
        return;
    }
    if (auto* n = std::get_if<SwitchStmt>(&v))       { for (auto& arm : n->arms) if (arm.block) expandReflectionInStmt(*arm.block, reg); return; }
    if (auto* n = std::get_if<FunctionDeclStmt>(&v)) { expandReflectionInBlock(n->body, reg); return; }
    if (auto* n = std::get_if<ClassDeclStmt>(&v))    { for (auto& m : n->methods) expandReflectionInBlock(m.body, reg); return; }
    if (auto* n = std::get_if<EnumDeclStmt>(&v))     { for (auto& m : n->methods) expandReflectionInBlock(m.body, reg); return; }
    if (auto* n = std::get_if<ImplDeclStmt>(&v))     { for (auto& m : n->methods) expandReflectionInBlock(m.body, reg); return; }
    if (auto* n = std::get_if<TraitDeclStmt>(&v))    { for (auto& m : n->methods) if (m.hasBody) expandReflectionInBlock(m.body, reg); return; }
    // ExprStmt / Break / Continue / Return / Yield / Import / Extern: no sub-statements.
}

// `inline for (v in @fields(T)) { … }` — a compile-time unroll. The body is captured verbatim as a
// token slice (NOT parsed) so the reflection-expansion pass can re-parse it once per field with the
// binding `v` substituted. Distinct from the (future) runtime `for … in`.
Stmt Parser::parseInlineForStmt() {
    Token keyword = previous();   // 'inline'
    consume(TokenType::FOR,        "expected 'for' after 'inline'");
    consume(TokenType::LEFT_PAREN, "expected '(' after 'inline for'");
    Token loopVar = consume(TokenType::IDENTIFIER, "expected a loop-variable name in 'inline for'");
    consume(TokenType::IN, "expected 'in' after the 'inline for' variable");
    consume(TokenType::AT, "the 'inline for' source must be '@fields(T)' or '@variants(E)'");
    Token which = consume(TokenType::IDENTIFIER, "expected 'fields' or 'variants' after '@'");
    bool overVariants = which.lexeme == "variants";
    if (which.lexeme != "fields" && !overVariants)
        throw error(which, "'inline for' can only iterate '@fields(T)' or '@variants(E)' (got '@"
                    + which.lexeme + "')");
    consume(TokenType::LEFT_PAREN, "expected '(' after '@" + which.lexeme + "'");
    if (!isTypeName()) throw error(peek(), "expected a type name in '@" + which.lexeme + "(...)'");
    Token targetType = consumeType();
    consume(TokenType::RIGHT_PAREN, "expected ')' after the '@" + which.lexeme + "' type");
    consume(TokenType::RIGHT_PAREN, "expected ')' after the 'inline for' clause");

    // Slurp the `{ … }` body as tokens (brace-balanced, braces included). Counting token *types*
    // (not characters) means a '{' inside a string/char literal is a STRING/CHAR token, not a
    // LEFT_BRACE, so it never miscounts.
    if (!check(TokenType::LEFT_BRACE))
        throw error(peek(), "expected '{' to open the 'inline for' body");
    std::vector<Token> bodyTokens;
    int depth = 0;
    do {
        const Token& t = advance();
        if      (t.type == TokenType::LEFT_BRACE)  depth++;
        else if (t.type == TokenType::RIGHT_BRACE) depth--;
        bodyTokens.push_back(t);
    } while (depth > 0 && !isAtEnd());

    return makeStmt(InlineForStmt{ keyword, loopVar, targetType, std::move(bodyTokens), overVariants });
}

// `@name(args)` — a compile-time reflection builtin. Scalar queries (@typeName/@fieldCount/
// @hasField) survive to semantic+codegen (folded like `sizeof`); @compileError errors in semantics;
// @field / @fields are only valid inside `inline for` (rewritten there) and are errors here.
Expr Parser::parseReflectExpr() {
    Token at = previous();   // '@'
    Token name = consume(TokenType::IDENTIFIER, "expected a reflection builtin name after '@'");
    const std::string& n = name.lexeme;

    if (n == "field")
        throw error(name, "'@field' is only valid inside an 'inline for' body");
    if (n == "fields")
        throw error(name, "'@fields' is only valid as the source of an 'inline for'");
    if (n == "variants")
        throw error(name, "'@variants' is only valid as the source of an 'inline for'");

    consume(TokenType::LEFT_PAREN, "expected '(' after '@" + n + "'");
    ReflectKind kind;
    std::vector<Token> typeArgs;
    std::vector<std::unique_ptr<Expr>> valueArgs;

    // Single-type-argument builtins.
    static const std::unordered_map<std::string, ReflectKind> oneTypeArg = {
        { "typeName",    ReflectKind::TypeName    }, { "fieldCount",  ReflectKind::FieldCount },
        { "variantCount",ReflectKind::VariantCount}, { "alignOf",     ReflectKind::AlignOf    },
        { "isInteger",   ReflectKind::IsInteger   }, { "isFloat",     ReflectKind::IsFloat    },
        { "isClass",     ReflectKind::IsClass     }, { "isEnum",      ReflectKind::IsEnum     },
        { "isPrimitive", ReflectKind::IsPrimitive },
    };
    if (auto it = oneTypeArg.find(n); it != oneTypeArg.end()) {
        kind = it->second;
        if (!isTypeName()) throw error(peek(), "expected a type name in '@" + n + "'");
        typeArgs.push_back(consumeType());
    } else if (n == "hasField" || n == "offsetOf") {
        kind = (n == "hasField") ? ReflectKind::HasField : ReflectKind::OffsetOf;
        if (!isTypeName()) throw error(peek(), "expected a type name in '@" + n + "'");
        typeArgs.push_back(consumeType());
        consume(TokenType::COMMA, "expected ',' then a field-name string in '@" + n + "'");
        valueArgs.push_back(box(parseExpression()));
    } else if (n == "implements") {
        kind = ReflectKind::Implements;
        if (!isTypeName()) throw error(peek(), "expected a type name in '@implements'");
        typeArgs.push_back(consumeType());
        consume(TokenType::COMMA, "expected ',' then a trait name in '@implements(T, Trait)'");
        typeArgs.push_back(consume(TokenType::IDENTIFIER, "expected a trait name in '@implements'"));
    } else if (n == "hasAnnotation") {
        kind = ReflectKind::HasAnnotation;
        if (!isTypeName()) throw error(peek(), "expected a type name in '@hasAnnotation'");
        typeArgs.push_back(consumeType());
        consume(TokenType::COMMA, "expected ',' then an annotation name in '@hasAnnotation(T, Ann)'");
        typeArgs.push_back(consume(TokenType::IDENTIFIER, "expected an annotation name in '@hasAnnotation'"));
    } else if (n == "compileError") {
        kind = ReflectKind::CompileError;
        valueArgs.push_back(box(parseExpression()));
    } else {
        throw error(name, "unknown reflection builtin '@" + n + "'");
    }

    consume(TokenType::RIGHT_PAREN, "expected ')' after '@" + n + "' arguments");
    return makeExpr(ReflectExpr{ at, kind, std::move(typeArgs), std::move(valueArgs) });
}