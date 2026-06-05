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
    funcParamTypes.clear();
    cgClasses_.clear();

    // Build CGClassInfo for each class (field order + types for GEP).
    for (const auto& decl : program.declarations) {
        if (!decl.node) continue;
        if (!std::holds_alternative<ClassDeclStmt>(*decl.node)) continue;
        const auto& cls = std::get<ClassDeclStmt>(*decl.node);
        CGClassInfo cgi;
        cgi.irTypeName = "%" + cls.name.lexeme;
        for (const FieldDecl& fd : cls.fields) {
            Type fieldType = typeFromToken(fd.typeName.type);
            cgi.fields.emplace_back(fd.name.lexeme, fieldType);
        }
        // Check whether this class declares a destructor
        for (const MethodDecl& md : cls.methods)
            if (md.isDestructor) { cgi.hasDestructor = true; break; }
        cgClasses_[cls.name.lexeme] = std::move(cgi);
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
        }
    }

    // Emit class type declarations first, then regular declarations.
    for (const auto& decl : program.declarations) {
        if (!decl.node) continue;
        if (std::holds_alternative<ClassDeclStmt>(*decl.node))
            genClassDecl(std::get<ClassDeclStmt>(*decl.node));
    }

    for (const auto& decl : program.declarations) {
        if (!decl.node) continue;
        if (std::holds_alternative<FunctionDeclStmt>(*decl.node))
            genFunction(std::get<FunctionDeclStmt>(*decl.node));
        else if (std::holds_alternative<ExternFuncDeclStmt>(*decl.node))
            genExternDecl(std::get<ExternFuncDeclStmt>(*decl.node));
    }
    return std::move(module);
}
