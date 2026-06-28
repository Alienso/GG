//
// Created by Vladimir Arsenijevic on 4.6.2026.
//

#include "CodeGen.h"

// ============================================================
// Public entry point
// ============================================================

IRModule CodeGen::generate(const Program& program, const SemanticResult& semanticResult, const CompilerOptions& options) {
    module           = {};
    this->typeMap    = &semanticResult.typeMap;
    stringCounter    = 0;
    currentClassName_ = "";
    boundsCheck      = options.boundsCheck;
    usesRefcount_    = false;
    clonesNeeded_.clear();
    funcParamTypes.clear();
    cgClasses_.clear();
    cgEnumNames_.clear();

    // Build CGClassInfo for each class (field order + types for GEP).
    for (const auto& decl : program.declarations) {
        if (!decl.node) continue;
        if (!std::holds_alternative<ClassDeclStmt>(*decl.node)) continue;
        const auto& cls = std::get<ClassDeclStmt>(*decl.node);
        CGClassInfo cgi;
        cgi.irTypeName = "%" + cls.name.lexeme;
        bool hasRefField = false;
        for (const FieldDecl& fd : cls.fields) {
            Type fieldType  = decodeSynthesizedType(fd.typeName);
            if (fieldType.kind == TypeKind::Reference) {
                hasRefField = true;
            } else if (isError(fieldType)) {
                fieldType = typeFromToken(fd.typeName.type);
            }
            // TypedPtr fields decode to a plain `ptr` in the struct (no refcount).
            cgi.fields.emplace_back(fd.name.lexeme, fieldType);
        }
        // A class needs a destructor if it declares one or owns reference fields.
        for (const MethodDecl& md : cls.methods)
            if (md.isDestructor) { cgi.hasDestructor = true; break; }
        cgi.needsDtor = cgi.hasDestructor || hasRefField;
        cgClasses_[cls.name.lexeme] = std::move(cgi);
    }

    // Build CGClassInfo for each enum (reuses the class machinery for fields/GEP)
    // and record the enum name. Enum singletons are never destroyed, so no dtor.
    for (const auto& decl : program.declarations) {
        if (!decl.node) continue;
        if (!std::holds_alternative<EnumDeclStmt>(*decl.node)) continue;
        const auto& en = std::get<EnumDeclStmt>(*decl.node);
        cgEnumNames_.insert(en.name.lexeme);
        CGClassInfo cgi;
        cgi.irTypeName = "%" + en.name.lexeme;
        for (const FieldDecl& fd : en.fields) {
            Type fieldType = decodeSynthesizedType(fd.typeName);
            if (isError(fieldType)) fieldType = typeFromToken(fd.typeName.type);
            cgi.fields.emplace_back(fd.name.lexeme, fieldType);
        }
        cgClasses_[en.name.lexeme] = std::move(cgi);
    }

    // Build function parameter type table (used in genCall to cast arguments).
    for (const auto& decl : program.declarations) {
        if (!decl.node) continue;
        if (std::holds_alternative<FunctionDeclStmt>(*decl.node)) {
            const auto& function = std::get<FunctionDeclStmt>(*decl.node);
            std::vector<Type> paramTypes;
            for (const auto& param : function.params)
                paramTypes.push_back(resolveParamType(param));
            funcParamTypes[function.name.lexeme] = std::move(paramTypes);
        } else if (std::holds_alternative<ExternFuncDeclStmt>(*decl.node)) {
            const auto& externDecl = std::get<ExternFuncDeclStmt>(*decl.node);
            std::vector<Type> paramTypes;
            for (const auto& param : externDecl.params)
                paramTypes.push_back(typeFromToken(param.typeName.type));
            funcParamTypes[externDecl.name.lexeme] = std::move(paramTypes);
        } else if (std::holds_alternative<ClassDeclStmt>(*decl.node)) {
            const auto& cls = std::get<ClassDeclStmt>(*decl.node);
            for (const MethodDecl& md : cls.methods) {
                // Destructor is mangled as ClassName_dtor
                std::string mangledName = md.isDestructor
                    ? cls.name.lexeme + "_dtor"
                    : cls.name.lexeme + "_" + md.name.lexeme;
                std::vector<Type> paramTypes;
                for (const auto& param : md.params)
                    paramTypes.push_back(resolveParamType(param));
                funcParamTypes[mangledName] = std::move(paramTypes);
            }
        } else if (std::holds_alternative<EnumDeclStmt>(*decl.node)) {
            const auto& en = std::get<EnumDeclStmt>(*decl.node);
            for (const MethodDecl& md : en.methods) {
                std::string mangledName = en.name.lexeme + "_" + md.name.lexeme;
                std::vector<Type> paramTypes;
                for (const auto& param : md.params)
                    paramTypes.push_back(resolveParamType(param));
                funcParamTypes[mangledName] = std::move(paramTypes);
            }
        }
    }

    // Emit class + enum type declarations first, then regular declarations.
    for (const auto& decl : program.declarations) {
        if (!decl.node) continue;
        if (std::holds_alternative<ClassDeclStmt>(*decl.node))
            genClassDecl(std::get<ClassDeclStmt>(*decl.node));
        else if (std::holds_alternative<EnumDeclStmt>(*decl.node))
            genEnumDecl(std::get<EnumDeclStmt>(*decl.node));
    }

    for (const auto& decl : program.declarations) {
        if (!decl.node) continue;
        if (std::holds_alternative<FunctionDeclStmt>(*decl.node))
            genFunction(std::get<FunctionDeclStmt>(*decl.node));
        else if (std::holds_alternative<ExternFuncDeclStmt>(*decl.node))
            genExternDecl(std::get<ExternFuncDeclStmt>(*decl.node));
    }

    // Emit the pre-main enum-variant initializer (registered in @llvm.global_ctors).
    genEnumInit(program);

    // Emit any clone helpers requested during codegen (may set usesRefcount_).
    for (const auto& cn : clonesNeeded_) genCloneFunction(cn);

    // Emit the refcount runtime if any `new`/retain/release was lowered.
    if (usesRefcount_) emitRefcountRuntime();

    return std::move(module);
}
