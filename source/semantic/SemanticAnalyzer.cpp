//
// Created by Vladimir Arsenijevic on 01.6.2026.
//

#include "SemanticAnalyzer.h"
#include <iostream>
#include <functional>

namespace {
    // Count trailing parameters that carry a default value. The parser guarantees defaults form a
    // contiguous trailing run, so this equals the number of arguments that may be omitted at a call.
    template <typename Params>
    size_t countTrailingDefaults(const Params& params) {
        size_t n = 0;
        for (const ParamDecl& p : params) if (p.defaultValue) ++n;
        return n;
    }
}

// ============================================================
// Public entry point
// ============================================================

SemanticResult SemanticAnalyzer::analyze(const Program& program,
                                          const std::string& filenameParam,
                                          const CompilerOptions& options) {
    symbolTable       = SymbolTable{};
    typeMap.clear();
    hadError          = false;
    filename          = filenameParam;
    currentReturnType = std::nullopt;
    loopDepth         = 0;
    currentClassName  = "";
    currentClassIsEnum = false;
    inEnumConstructor  = false;
    classRegistry.clear();
    enumRegistry.clear();
    functionRegistry.clear();
    traitRegistry.clear();
    implementedTraits.clear();
    currentSelfType_ = "";
    currentReturnSlotName_ = "";
    resolvedCallee.clear();
    expectedType_ = std::nullopt;
    allowRawPtr_      = options.allowRawPtr;

    symbolTable.enterScope();   // global scope

    collectClasses(program);    // pass 0: build class registry
    checkValueFieldCycles(program); // pass 0a: reject infinite-size value-object embedding cycles
    collectTraits(program);     // pass 0b: register trait contracts
    collectImpls(program);      // pass 0c: attach impl methods to their class + check conformance
    checkGenericBounds(program);// pass 0d: verify generic trait-bound obligations
    collectFunctions(program);  // pass 1: hoist function signatures

    for (const Stmt& stmt : program.declarations)
        analyzeStmt(stmt);      // pass 2: full analysis

    checkGenericBodies(program);  // pass 3: check bounded generic bodies against their bounds

    symbolTable.exitScope();

    std::unordered_set<std::string> eqImpls;
    for (const auto& [type, traits] : implementedTraits)
        if (traits.count("Eq")) eqImpls.insert(type);

    return SemanticResult{hadError, std::move(typeMap), classRegistry, enumRegistry,
                          std::move(resolvedCallee), std::move(addressIdentityCmp_),
                          std::move(structuralValueCmp_), std::move(eqImpls),
                          std::move(callableCalls_), std::move(braceInitClass_) };
}

// ============================================================
// Pass 0 — collect class declarations into classRegistry_
// ============================================================

// Build a ClassInfo (fields + methods + optional destructor) shared by classes and
// enums. `ownerName` is used in error messages; `allowDestructor` is false for enums.
ClassInfo SemanticAnalyzer::buildClassInfo(const std::string& ownerName,
                                           const std::deque<FieldDecl>& fields,
                                           const std::deque<MethodDecl>& methods,
                                           bool allowDestructor) {
    ClassInfo info;
    int fieldIndex = 0;
    for (const FieldDecl& fd : fields) {
        const std::string& lex = fd.typeName.lexeme;
        Type fieldType;
        Type synth = decodeSynthesizedType(fd.typeName);
        if (!isError(synth)) {
            // Reference field (Class&) or typed-pointer field (ptr<T>).
            fieldType = synth;
        } else if (fd.typeName.type == TokenType::IDENTIFIER) {
            // Bare type name: an enum-value field, or a value-object field (embedding).
            if (declaredEnumNames_.count(lex)) {
                fieldType = makeEnumType(lex);
            } else if (declaredClassNames_.count(lex)) {
                // Value-object field: the sub-object is embedded contiguously in this struct
                // (deep-copied on clone, destroyed recursively). Not allowed inside an enum
                // (singletons are pre-main-initialised globals — out of scope for now).
                if (!allowDestructor) {
                    error(fd.name, "enums cannot have value-object fields; declare it as a reference '"
                          + lex + "& " + fd.name.lexeme + "'");
                    fieldType = Type{TypeKind::Error};
                } else {
                    fieldType = makeObjectType(lex);
                }
            } else {
                error(fd.name, "unknown type '" + lex + "' for field '" + fd.name.lexeme + "'");
                fieldType = Type{TypeKind::Error};
            }
        } else {
            fieldType = typeFromToken(fd.typeName.type);  // primitive / ptr
        }
        // A `ref T` borrow may not be stored in a field — a borrow must not outlive its source, and
        // a field could. Use an owning reference (`Class&`) or a value.
        if (fieldType.kind == TypeKind::Reference && fieldType.borrow) {
            error(fd.name, "a field cannot be a borrow ('ref " + fieldType.className
                  + "'); use an owning reference '" + fieldType.className + "& " + fd.name.lexeme
                  + "' or a value");
            fieldType = Type{TypeKind::Error};
        }
        // Static fields are class-level storage (a global), not part of the struct
        // layout — they get no struct index and live in a separate registry.
        // (allowDestructor is false only for enums, which don't support statics yet.)
        if (fd.isStatic) {
            if (!allowDestructor) {
                error(fd.name, "enums cannot declare static fields");
                continue;
            }
            if (info.staticFields.count(fd.name.lexeme) || info.fields.count(fd.name.lexeme)) {
                error(fd.name, "duplicate field '" + fd.name.lexeme
                      + "' in '" + ownerName + "'");
                continue;
            }
            info.staticFields.emplace(fd.name.lexeme,
                ClassInfo::StaticField{fd.isPublic, fieldType, fd.name,
                                       /*hasInit=*/fd.initializer != nullptr});
            continue;
        }
        info.fieldOrder.push_back(fd.name.lexeme);
        // emplace constructs in-place, avoiding copy/move-assignment of Token
        info.fields.emplace(fd.name.lexeme,
            ClassInfo::Field{fd.isPublic, fd.isMut, fieldType, fieldIndex++, fd.name});
    }
    // REFERENCE field names — escape analysis treats `field = param` as an escape only for these
    // (storing into a value-object field is a deep copy, not a reference escape).
    std::unordered_set<std::string> refFieldNames;
    for (const auto& [n, f] : info.fields)       if (f.type.kind == TypeKind::Reference) refFieldNames.insert(n);
    for (const auto& [n, f] : info.staticFields) if (f.type.kind == TypeKind::Reference) refFieldNames.insert(n);
    for (const MethodDecl& md : methods) {
        if (md.isDestructor) {
            if (!allowDestructor) {
                error(md.name, "enums cannot declare a destructor");
                continue;
            }
            // Validate: no params, at most one destructor per class.
            if (!md.params.empty()) {
                error(md.name, "destructor '" + ownerName + "::~" + md.name.lexeme
                      + "' must take no parameters");
                continue;
            }
            if (info.destructor.has_value()) {
                error(md.name, "class '" + ownerName
                      + "' already has a destructor (duplicate ~" + md.name.lexeme + ")");
                continue;
            }
            info.destructor.emplace(ClassInfo::Method{
                md.isPublic, /*isStatic=*/false, /*isMut=*/false, Type{TypeKind::Void},
                std::vector<Type>{}, std::vector<bool>{}, /*numDefaults=*/0, md.name
            });
            continue;
        }
        Type returnType = md.isConstructor
            ? Type{TypeKind::Void}
            : resolveTypeToken(md.returnType);
        std::vector<Type> paramTypes;
        std::vector<bool> paramMut;
        for (const ParamDecl& p : md.params) {
            paramTypes.push_back(resolveTypeToken(p.typeName));
            paramMut.push_back(p.isMut);
        }
        // Overload set per name. Enums may not overload (single ctor / fixed method set).
        std::vector<ClassInfo::Method>& set = info.methods[md.name.lexeme];
        const bool isEnum = !allowDestructor;
        if (isEnum && !set.empty()) {
            error(md.name, "enum '" + ownerName + "' cannot overload '" + md.name.lexeme
                  + "' — overloading is not supported for enums");
            continue;
        }
        bool redef = false;
        for (const ClassInfo::Method& e : set)
            if (e.paramTypes == paramTypes && e.returnType == returnType) { redef = true; break; }
        if (redef) {
            error(md.name, "'" + ownerName + "::" + md.name.lexeme
                  + "' is already defined with the same signature");
            continue;
        }
        set.push_back(ClassInfo::Method{md.isPublic, md.isStatic, md.isMut, returnType,
                                        std::move(paramTypes), std::move(paramMut),
                                        countTrailingDefaults(md.params), md.name});
        bool te = false;
        computeParamEscapes(md.params, md.body, /*computeThis=*/!md.isStatic, refFieldNames,
                            set.back().paramEscapes, te);
        set.back().thisEscapes = te;
    }
    return info;
}

void SemanticAnalyzer::collectClasses(const Program& program) {
    // Pre-pass: record every class / enum name so a value-object field type may forward-reference
    // a type declared later in the file (buildClassInfo resolves field types against these sets).
    for (const Stmt& stmt : program.declarations) {
        if (std::holds_alternative<EnumDeclStmt>(*stmt.node))
            declaredEnumNames_.insert(std::get<EnumDeclStmt>(*stmt.node).name.lexeme);
        else if (std::holds_alternative<ClassDeclStmt>(*stmt.node))
            declaredClassNames_.insert(std::get<ClassDeclStmt>(*stmt.node).name.lexeme);
    }

    // Enums are registered first so that a class field/param of an enum type
    // resolves correctly, and vice-versa (both share classRegistry).
    for (const Stmt& stmt : program.declarations) {
        if (!std::holds_alternative<EnumDeclStmt>(*stmt.node)) continue;
        const auto& en = std::get<EnumDeclStmt>(*stmt.node);

        EnumInfo einfo{ {}, {}, en.name };
        for (const EnumVariant& v : en.variants) {
            if (!einfo.variantSet.insert(v.name.lexeme).second) {
                error(v.name, "duplicate enum variant '" + v.name.lexeme
                      + "' in enum '" + en.name.lexeme + "'");
                continue;
            }
            einfo.variantOrder.push_back(v.name.lexeme);
        }
        enumRegistry.emplace(en.name.lexeme, std::move(einfo));

        ClassInfo info = buildClassInfo(en.name.lexeme, en.fields, en.methods,
                                        /*allowDestructor=*/false);
        classRegistry.emplace(en.name.lexeme, std::move(info));
    }

    for (const Stmt& stmt : program.declarations) {
        if (!std::holds_alternative<ClassDeclStmt>(*stmt.node)) continue;
        const auto& cls = std::get<ClassDeclStmt>(*stmt.node);
        ClassInfo info = buildClassInfo(cls.name.lexeme, cls.fields, cls.methods,
                                        /*allowDestructor=*/true);
        classRegistry.emplace(cls.name.lexeme, std::move(info));
    }
}

void SemanticAnalyzer::checkValueFieldCycles(const Program& program) {
    // A value-object field embeds its type contiguously; a cycle of such embeddings
    // (`A{B b} B{A a}`, or direct `A{A a}`) is an infinite-size struct. Reference / ptr
    // fields do NOT create an edge — they are pointers and break the cycle. 3-colour DFS
    // over value-embedding edges; report the first back-edge and stop.
    enum Colour { White, Grey, Black };
    std::unordered_map<std::string, int> colour;

    std::function<bool(const std::string&)> dfs = [&](const std::string& name) -> bool {
        colour[name] = Grey;
        auto it = classRegistry.find(name);
        if (it != classRegistry.end()) {
            for (const std::string& fieldName : it->second.fieldOrder) {
                auto fit = it->second.fields.find(fieldName);
                if (fit == it->second.fields.end()) continue;
                const ClassInfo::Field& f = fit->second;
                if (f.type.kind != TypeKind::Object) continue;    // only value embedding
                const std::string& target = f.type.className;
                if (target == name || colour[target] == Grey) {
                    error(f.decl, "value field cycle: '" + name + "' embeds '" + target
                          + "' by value (directly or transitively); use a reference '"
                          + target + "& " + fieldName + "'");
                    colour[name] = Black;
                    return true;                                   // stop at the first cycle
                }
                if (colour[target] == White && dfs(target)) { colour[name] = Black; return true; }
            }
        }
        colour[name] = Black;
        return false;
    };

    // Iterate in declaration order for deterministic diagnostics.
    for (const Stmt& stmt : program.declarations) {
        if (!std::holds_alternative<ClassDeclStmt>(*stmt.node)) continue;
        const std::string& name = std::get<ClassDeclStmt>(*stmt.node).name.lexeme;
        if (colour[name] == White && dfs(name)) break;
    }
}

// ============================================================
// Traits & impls
// ============================================================

bool SemanticAnalyzer::isBuiltinTrait(const std::string& name) {
    static const std::unordered_set<std::string> builtins = {
        "Add", "Sub", "Mul", "Div", "Rem", "Eq", "Ord", "Neg", "Index"
    };
    return builtins.count(name) > 0;
}

const std::pair<const char*, const char*>* SemanticAnalyzer::operatorTraitFor(TokenType op) {
    switch (op) {
        case TokenType::PLUS:          { static const std::pair<const char*, const char*> t{"Add", "add"}; return &t; }
        case TokenType::MINUS:         { static const std::pair<const char*, const char*> t{"Sub", "sub"}; return &t; }
        case TokenType::STAR:          { static const std::pair<const char*, const char*> t{"Mul", "mul"}; return &t; }
        case TokenType::SLASH:         { static const std::pair<const char*, const char*> t{"Div", "div"}; return &t; }
        case TokenType::PERCENT:       { static const std::pair<const char*, const char*> t{"Rem", "rem"}; return &t; }
        case TokenType::EQUAL_EQUAL:
        case TokenType::BANG_EQUAL:    { static const std::pair<const char*, const char*> t{"Eq",  "eq"};  return &t; }
        case TokenType::LESS:
        case TokenType::LESS_EQUAL:
        case TokenType::GREATER:
        case TokenType::GREATER_EQUAL: { static const std::pair<const char*, const char*> t{"Ord", "cmp"}; return &t; }
        default: return nullptr;
    }
}

void SemanticAnalyzer::collectTraits(const Program& program) {
    for (const Stmt& stmt : program.declarations) {
        if (!std::holds_alternative<TraitDeclStmt>(*stmt.node)) continue;
        const auto& tr = std::get<TraitDeclStmt>(*stmt.node);
        if (isBuiltinTrait(tr.name.lexeme))
            error(tr.name, "'" + tr.name.lexeme + "' is a built-in operator trait and cannot be redeclared");
        else if (traitRegistry.count(tr.name.lexeme) || classRegistry.count(tr.name.lexeme))
            error(tr.name, "'" + tr.name.lexeme + "' is already declared");
        // v1: default method bodies are not supported (static dispatch would need per-impl
        // materialisation of the shared body — deferred).
        for (const MethodDecl& md : tr.methods)
            if (md.hasBody)
                error(md.name, "default trait method bodies are not yet supported; "
                      "declare a signature ending in ';'");
        traitRegistry[tr.name.lexeme] = &tr;
    }
}

void SemanticAnalyzer::collectImpls(const Program& program) {
    // Built-in operator trait → the conventional method name an impl must define.
    static const std::unordered_map<std::string, std::string> builtinMethod = {
        {"Add", "add"}, {"Sub", "sub"}, {"Mul", "mul"}, {"Div", "div"}, {"Rem", "rem"},
        {"Eq", "eq"}, {"Ord", "cmp"}, {"Neg", "neg"}, {"Index", "get"}
    };
    for (const Stmt& stmt : program.declarations) {
        if (!std::holds_alternative<ImplDeclStmt>(*stmt.node)) continue;
        const auto& impl = std::get<ImplDeclStmt>(*stmt.node);
        const std::string& type  = impl.typeName.lexeme;
        const std::string& trait = impl.traitName.lexeme;

        if (enumRegistry.count(type)) {
            error(impl.typeName, "cannot 'impl' a trait for enum '" + type + "'");
            continue;
        }
        auto clsIt = classRegistry.find(type);
        if (clsIt == classRegistry.end()) {
            error(impl.typeName, "unknown class '" + type + "' in 'impl'");
            continue;
        }
        const bool builtin = isBuiltinTrait(trait);
        const TraitDeclStmt* traitDecl = nullptr;
        if (!builtin) {
            auto tIt = traitRegistry.find(trait);
            if (tIt == traitRegistry.end()) {
                error(impl.traitName, "unknown trait '" + trait + "'");
                continue;
            }
            traitDecl = tIt->second;
        }

        // Register impl methods as methods on the target class (with Self → the type).
        currentSelfType_ = type;
        ClassInfo& info = clsIt->second;
        std::unordered_set<std::string> refFieldNames;
        for (const auto& [n, f] : info.fields)       if (f.type.kind == TypeKind::Reference) refFieldNames.insert(n);
        for (const auto& [n, f] : info.staticFields) if (f.type.kind == TypeKind::Reference) refFieldNames.insert(n);
        for (const MethodDecl& md : impl.methods) {
            Type ret = resolveTypeToken(md.returnType);
            std::vector<Type> paramTypes;
            std::vector<bool> paramMut;
            for (const ParamDecl& p : md.params) {
                paramTypes.push_back(resolveTypeToken(p.typeName));
                paramMut.push_back(p.isMut);
            }
            std::vector<ClassInfo::Method>& set = info.methods[md.name.lexeme];
            bool redef = false;
            for (const ClassInfo::Method& e : set)
                if (e.paramTypes == paramTypes && e.returnType == ret) { redef = true; break; }
            if (redef) {
                error(md.name, "'" + type + "::" + md.name.lexeme + "' is already defined with the same signature");
                continue;
            }
            set.push_back(ClassInfo::Method{md.isPublic, md.isStatic, md.isMut, ret,
                                            std::move(paramTypes), std::move(paramMut),
                                            countTrailingDefaults(md.params), md.name});
            bool te = false;
            computeParamEscapes(md.params, md.body, /*computeThis=*/!md.isStatic, refFieldNames,
                                set.back().paramEscapes, te);
            set.back().thisEscapes = te;
        }

        implementedTraits[type].insert(trait);

        // Conformance check.
        if (builtin) {
            const std::string& need = builtinMethod.at(trait);
            if (!info.methods.count(need))
                error(impl.traitName, "impl of built-in trait '" + trait + "' for '" + type
                      + "' must define method '" + need + "'");
        } else {
            for (const MethodDecl& req : traitDecl->methods) {
                std::vector<Type> reqParams;
                for (const ParamDecl& p : req.params) reqParams.push_back(resolveTypeToken(p.typeName));
                Type reqRet = resolveTypeToken(req.returnType);
                auto it = info.methods.find(req.name.lexeme);
                bool found = false;
                if (it != info.methods.end())
                    for (const ClassInfo::Method& c : it->second)
                        if (c.paramTypes == reqParams && c.returnType == reqRet) { found = true; break; }
                if (!found)
                    error(impl.typeName, "impl of trait '" + trait + "' for '" + type
                          + "' is missing required method '" + req.name.lexeme + "'");
            }
        }
        currentSelfType_ = "";
    }
}

// ============================================================
// Pass 0d — verify generic trait-bound obligations
// ============================================================
// Each obligation (recorded by the parser at monomorphization) says a concrete type
// argument must implement a named trait. Static dispatch: the check is that the type
// appears in `implementedTraits` for that trait (both user and built-in impls populate
// it). Primitives and non-implementing classes produce a clear instantiation-site error.
// Render a bound name for diagnostics: a canonical `Call$p…$ret$R` → `Call(p…) -> R`
// (with `.ref` shown as `&`); any other trait name is returned unchanged.
static std::string prettyBound(const std::string& b) {
    if (b.rfind("Call$", 0) != 0) return b;
    std::vector<std::string> parts;
    for (size_t i = 0; i <= b.size(); ) {
        size_t d = b.find('$', i);
        if (d == std::string::npos) { parts.push_back(b.substr(i)); break; }
        parts.push_back(b.substr(i, d - i));
        i = d + 1;
    }
    size_t retIdx = 0;
    for (size_t i = 1; i < parts.size(); ++i) if (parts[i] == "ret") retIdx = i;
    if (retIdx == 0 || retIdx + 1 >= parts.size()) return b;   // malformed — leave as-is
    auto unref = [](std::string s) {
        size_t p = s.rfind(".ref");
        if (p != std::string::npos && p == s.size() - 4) s = s.substr(0, p) + "&";
        return s;
    };
    std::string out = "Call(";
    for (size_t i = 1; i < retIdx; ++i) out += (i > 1 ? ", " : "") + unref(parts[i]);
    out += ") -> " + unref(parts[retIdx + 1]);
    return out;
}

void SemanticAnalyzer::checkGenericBounds(const Program& program) {
    for (const GenericBoundCheck& bc : program.genericBoundChecks) {
        Token where{TokenType::IDENTIFIER, bc.typeName, bc.line};
        if (!isBuiltinTrait(bc.traitName) && !traitRegistry.count(bc.traitName)) {
            error(where, "unknown trait '" + prettyBound(bc.traitName) + "' in bound for '" + bc.context + "'");
            continue;
        }
        auto it = implementedTraits.find(bc.typeName);
        bool ok = it != implementedTraits.end() && it->second.count(bc.traitName);
        if (!ok)
            error(where, "type '" + bc.typeName + "' does not satisfy bound '" + prettyBound(bc.traitName)
                  + "' required by '" + bc.context + "'");
    }
}

// ============================================================
// Pass 3 — check bounded generic bodies against their bounds
// ============================================================

const std::vector<std::string>* SemanticAnalyzer::typeParamBoundsOf(const Type& t) const {
    if (currentTypeParamBounds_.empty()) return nullptr;
    // A bare `T` (TypeParam), a `T&` (Reference), or `T` re-materialised as Object all carry the
    // parameter name in className; the active param map decides whether that name is a parameter.
    if (t.kind == TypeKind::TypeParam || t.kind == TypeKind::Reference || t.kind == TypeKind::Object) {
        auto it = currentTypeParamBounds_.find(t.className);
        if (it != currentTypeParamBounds_.end()) return &it->second;
    }
    return nullptr;
}

bool SemanticAnalyzer::builtinBoundMethod(const std::string& trait, const std::string& method,
                                          size_t argc, const std::string& paramName, Type& out) const {
    if (argc == 1 && ((trait == "Add" && method == "add") || (trait == "Sub" && method == "sub")
        || (trait == "Mul" && method == "mul") || (trait == "Div" && method == "div")
        || (trait == "Rem" && method == "rem"))) { out = makeTypeParam(paramName); return true; }
    if (trait == "Neg" && method == "neg" && argc == 0) { out = makeTypeParam(paramName); return true; }
    if (trait == "Ord" && method == "cmp" && argc == 1) { out = Type{TypeKind::I32};  return true; }
    if (trait == "Eq"  && method == "eq"  && argc == 1) { out = Type{TypeKind::Bool}; return true; }
    return false;
}

bool SemanticAnalyzer::resolveBoundMethod(const std::vector<std::string>& bounds,
                                          const std::string& paramName, const std::string& name,
                                          size_t argc, Type& out) {
    for (const std::string& b : bounds) {
        // User trait: its declared method signatures (Self → the parameter).
        auto tit = traitRegistry.find(b);
        if (tit != traitRegistry.end()) {
            for (const MethodDecl& md : tit->second->methods) {
                if (md.name.lexeme == name && md.params.size() == argc) {
                    std::string savedSelf = currentSelfType_;
                    currentSelfType_ = paramName;             // Self resolves to the type parameter
                    out = resolveTypeToken(md.returnType);
                    currentSelfType_ = savedSelf;
                    return true;
                }
            }
        }
        // Built-in operator trait: its conventional method (add/sub/cmp/eq/neg/…).
        if (builtinBoundMethod(b, name, argc, paramName, out)) return true;
    }
    return false;
}

void SemanticAnalyzer::checkGenericBodies(const Program& program) {
    for (const GenericTemplateDecl& gtd : program.genericTemplates) {
        if (!gtd.decl.node) continue;

        // Bind each type parameter to its bounds (empty ⇒ unbounded / permissive).
        currentTypeParamBounds_.clear();
        bool anyBounded = false;
        for (size_t i = 0; i < gtd.typeParams.size(); ++i) {
            std::vector<std::string> b = (i < gtd.bounds.size()) ? gtd.bounds[i]
                                                                 : std::vector<std::string>{};
            if (!b.empty()) anyBounded = true;
            currentTypeParamBounds_.emplace(gtd.typeParams[i], std::move(b));
        }
        if (!anyBounded) { currentTypeParamBounds_.clear(); continue; }  // nothing to enforce

        if (std::holds_alternative<FunctionDeclStmt>(*gtd.decl.node)) {
            analyzeFunctionDecl(std::get<FunctionDeclStmt>(*gtd.decl.node));
        } else if (std::holds_alternative<ClassDeclStmt>(*gtd.decl.node)) {
            const auto& cls = std::get<ClassDeclStmt>(*gtd.decl.node);
            const std::string& name = cls.name.lexeme;
            // Temporarily register the template class (with its type params known as type names)
            // so field / method / `this` lookups resolve while its method bodies are analysed.
            for (const auto& [tp, _] : currentTypeParamBounds_) declaredClassNames_.insert(tp);
            bool hadClass = classRegistry.count(name) > 0;
            if (!hadClass)
                classRegistry.emplace(name, buildClassInfo(name, cls.fields, cls.methods, true));
            analyzeClassDecl(cls);
            if (!hadClass) classRegistry.erase(name);
            for (const auto& [tp, _] : currentTypeParamBounds_) declaredClassNames_.erase(tp);
        }
        currentTypeParamBounds_.clear();
    }
}

// ============================================================
// Pass 1 — collect top-level function signatures
// ============================================================

void SemanticAnalyzer::collectFunctions(const Program& program) {
    for (const Stmt& stmt : program.declarations) {
        if (std::holds_alternative<FunctionDeclStmt>(*stmt.node)) {
            const auto& function = std::get<FunctionDeclStmt>(*stmt.node);
            const std::string& name = function.name.lexeme;

            std::vector<Type> paramTypes;
            std::vector<bool> paramMut;
            for (const ParamDecl& param : function.params) {
                paramTypes.push_back(resolveTypeToken(param.typeName));
                paramMut.push_back(param.isMut);
            }
            Type returnType = resolveTypeToken(function.returnType);

            std::vector<FunctionOverload>& set = functionRegistry[name];
            if (name == "main" && !set.empty()) {
                error(function.name, "'main' cannot be overloaded");
            } else if (!set.empty() && set.front().isExtern) {
                error(function.name, "cannot overload extern function '" + name + "'");
            } else {
                bool redef = false;
                for (const FunctionOverload& e : set)
                    if (e.paramTypes == paramTypes && e.returnType == returnType) { redef = true; break; }
                if (redef)
                    error(function.name, "function '" + name + "' is already defined with the same signature");
                else {
                    set.push_back(FunctionOverload{returnType, paramTypes, paramMut,
                                                   countTrailingDefaults(function.params),
                                                   /*isExtern=*/false, function.name});
                    bool te = false;
                    static const std::unordered_set<std::string> noFields;
                    computeParamEscapes(function.params, function.body, /*computeThis=*/false,
                                        noFields, set.back().paramEscapes, te);
                }
            }

            // Symbol-table marker (first declaration wins) so a local can shadow the name
            // and bare use as a value still reports "cannot use function as a value".
            symbolTable.declare(name, Symbol{Symbol::Kind::Function, returnType, function.name, {}});
        }
        else if (std::holds_alternative<ExternFuncDeclStmt>(*stmt.node)) {
            const auto& externDecl = std::get<ExternFuncDeclStmt>(*stmt.node);
            const std::string& name = externDecl.name.lexeme;

            std::vector<Type> paramTypes;
            for (const ParamDecl& param : externDecl.params)
                paramTypes.push_back(typeFromToken(param.typeName.type));
            Type returnType = typeFromToken(externDecl.returnType.type);

            std::vector<FunctionOverload>& set = functionRegistry[name];
            if (!set.empty()) {
                error(externDecl.name, "extern '" + name + "' cannot be overloaded or redefined");
            } else {
                set.push_back(FunctionOverload{returnType, paramTypes, std::vector<bool>{},
                                               /*numDefaults=*/0, /*isExtern=*/true, externDecl.name});
                symbolTable.declare(name, Symbol{Symbol::Kind::Function, returnType, externDecl.name, {}});
            }
        }
    }
}

// ============================================================
// Helpers
// ============================================================

void SemanticAnalyzer::analyzeBreak(const BreakStmt& breakStmt) {
    if (loopDepth == 0)
        error(breakStmt.keyword, "'break' used outside of a loop");
}

void SemanticAnalyzer::analyzeContinue(const ContinueStmt& continueStmt) {
    if (loopDepth == 0)
        error(continueStmt.keyword, "'continue' used outside of a loop");
}

void SemanticAnalyzer::enterScope() { symbolTable.enterScope(); }
void SemanticAnalyzer::exitScope()  { symbolTable.exitScope(); }

const Symbol* SemanticAnalyzer::lookupSymbol(const Token& nameToken) {
    const Symbol* sym = symbolTable.lookup(nameToken.lexeme);
    if (!sym) error(nameToken, "use of undeclared identifier '" + nameToken.lexeme + "'");
    return sym;
}

void SemanticAnalyzer::error(const Token& token, const std::string& message) {
    std::string prefix = filename.empty() ? "" : filename + ":";
    std::string formatted = prefix + std::to_string(token.line) + ": error: " + message;
    throw CompileError(formatted);
}

void SemanticAnalyzer::warn(const Token& token, const std::string& message) {
    std::string prefix = filename.empty() ? "" : filename + ":";
    std::cerr << prefix << token.line << ": Warning: " << message << '\n';
}

void SemanticAnalyzer::checkRawPtrAllowed(const Token& typeToken, const Token& site) {
    if (allowRawPtr_) return;
    Type t = resolveTypeToken(typeToken);
    if (t.kind == TypeKind::Ptr || t.kind == TypeKind::TypedPtr) {
        error(site, "'" + typeName(t) + "' is a raw pointer type and requires --unsafe-ptr "
              "(raw pointers are for stdlib/internal use only)");
    }
}

Type SemanticAnalyzer::resolveTypeToken(const Token& typeToken) const {
    // Parser-synthesized types: "<Class>&" (Reference) and "ptr<Elem>" (TypedPtr).
    Type synth = decodeSynthesizedType(typeToken);
    if (!isError(synth)) {
        // `Self&` inside a trait/impl → a reference to the implementing type.
        if (synth.className == "Self") synth.className = currentSelfType_;
        return synth;
    }
    // Bare `Self` → the implementing type (object); during a generic body-check where Self is a
    // type parameter, it is the abstract parameter itself.
    if (typeToken.type == TokenType::SELF) {
        if (currentSelfType_.empty()) return Type{TypeKind::Error};
        if (currentTypeParamBounds_.count(currentSelfType_)) return makeTypeParam(currentSelfType_);
        return makeObjectType(currentSelfType_);
    }
    // A bare generic type-parameter name (only while checking a bounded generic body).
    if (typeToken.type == TokenType::IDENTIFIER && currentTypeParamBounds_.count(typeToken.lexeme))
        return makeTypeParam(typeToken.lexeme);
    if (typeToken.type == TokenType::IDENTIFIER && enumRegistry.count(typeToken.lexeme))
        return makeEnumType(typeToken.lexeme);
    if (typeToken.type == TokenType::IDENTIFIER && classRegistry.count(typeToken.lexeme))
        return makeObjectType(typeToken.lexeme);
    return typeFromToken(typeToken.type);
}

void SemanticAnalyzer::recordType(const Expr& expr, const Type& type) {
    typeMap[expr.node.get()] = type;
}

void SemanticAnalyzer::checkCast(const Type& from, const Type& to,
                                  const Token& site, const std::string& context) {
    if (isError(from) || isError(to)) return;
    CastResult castResult = canImplicitlyCast(from, to);
    std::string contextString = context.empty() ? "" : " in " + context;
    if (castResult == CastResult::None) {
        error(site, "cannot implicitly convert " + typeName(from)
              + " to " + typeName(to) + contextString);
    } else if (castResult == CastResult::Warn) {
        warn(site, "implicit conversion from " + typeName(from)
             + " to " + typeName(to) + " may lose data" + contextString);
    }
}

void SemanticAnalyzer::checkArgCast(const Type& from, const Type& to,
                                     const Token& site, const std::string& context) {
    if (isError(from) || isError(to)) return;
    CastResult castResult = canPassArgument(from, to);
    std::string contextString = context.empty() ? "" : " in " + context;
    if (castResult == CastResult::None) {
        error(site, "cannot implicitly convert " + typeName(from)
              + " to " + typeName(to) + contextString);
    } else if (castResult == CastResult::Warn) {
        warn(site, "implicit conversion from " + typeName(from)
             + " to " + typeName(to) + " may lose data" + contextString);
    }
    // Silent (incl. a value-object → reference borrow) → no diagnostic.
}

const ClassInfo* SemanticAnalyzer::lookupObjectClass(Type objectType, const Token& site) {
    // Value objects (Point), heap references (Point&) and enum values all carry a
    // className and support member/method access.
    if (objectType.kind != TypeKind::Object && objectType.kind != TypeKind::Reference
        && objectType.kind != TypeKind::Enum) {
        error(site, "member access on non-class type '" + typeName(objectType) + "'");
        return nullptr;
    }
    auto it = classRegistry.find(objectType.className);
    if (it == classRegistry.end()) {
        error(site, "unknown class '" + objectType.className + "'");
        return nullptr;
    }
    return &it->second;
}

void SemanticAnalyzer::analyzeCallArgs(
    const std::vector<std::unique_ptr<Expr>>& args,
    const std::vector<Type>& paramTypes,
    const Token& callee,
    const std::string& context,
    const std::vector<bool>& paramMut)
{
    for (size_t i = 0; i < args.size(); ++i) {
        Type argType = analyzeExpr(*args[i]);
        checkArgCast(argType, paramTypes[i], callee,
                     "argument " + std::to_string(i + 1) + " of " + context);
        // Passing a read-only reference into a `mut` reference parameter is a const→mut
        // coercion — warn unless the argument is an explicit cast.
        if (i < paramMut.size() && paramMut[i])
            warnConstToMut(callee, *args[i], paramTypes[i]);
    }
}

void SemanticAnalyzer::checkConstantIndexBounds(
    const Expr& indexExpr, size_t arraySize)
{
    if (!indexExpr.node) return;
    if (!std::holds_alternative<LiteralExpr>(*indexExpr.node)) return;
    const auto& lit = std::get<LiteralExpr>(*indexExpr.node);
    if (lit.token.type != TokenType::NUMBER) return;
    if (lit.token.lexeme.find('.') != std::string::npos) return;
    long long idx = 0;
    try {
        idx = std::stoll(lit.token.lexeme);
    } catch (const std::exception&) {
        return;  // non-integer or overflow — skip bounds check
    }
    if (idx < 0 || static_cast<size_t>(idx) >= arraySize)
        error(lit.token, "array index " + lit.token.lexeme
              + " is out of bounds for array of size " + std::to_string(arraySize));
}
