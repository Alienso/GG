//
// Created by Vladimir Arsenijevic on 4.6.2026.
//

#ifndef GG_CODE_GEN_H
#define GG_CODE_GEN_H

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "IR.h"
#include "../parser/Ast.h"
#include "../semantic/SemanticAnalyzer.h"
#include "../CompilerOptions.h"

class CodeGen {
public:
    IRModule generate(const Program& program, const SemanticResult& semanticResult, const CompilerOptions& options = {});

private:
    // ---- Per-class info (populated in generate()) ----
    struct CGClassInfo {
        std::string                             irTypeName;      // "%Point"
        std::vector<std::pair<std::string,Type>> fields;        // ordered: (name, type)
        bool                                    hasDestructor = false;  // user-written ~Class()
        bool                                    needsDtor     = false;  // user dtor OR has reference fields
    };
    std::unordered_map<std::string, CGClassInfo>      cgClasses_;

    // ---- Scope-exit cleanup tracking ----
    // One entry per object/reference that needs cleanup when its scope ends.
    //   isReference == false : a value object living in `allocaPtr`
    //                          → call <className>_dtor(allocaPtr)
    //   isReference == true  : a reference variable whose slot `allocaPtr` holds a
    //                          heap pointer → load it and gg_release(ptr, dtor)
    struct DtorEntry {
        std::string allocaPtr;
        std::string className;
        bool        isReference = false;
    };
    std::vector<std::vector<DtorEntry>> dtorScopes_;

    // Set true the first time `new` is lowered; triggers emission of the refcount runtime.
    bool usesRefcount_ = false;

    // Classes that need a generated @Class_clone helper (deep copy: memberwise
    // copy with retain at reference-field boundaries). Populated on demand.
    std::unordered_set<std::string> clonesNeeded_;

    // Reference values produced with a +1 count (from `new` or a reference-returning
    // call) that are not yet owned by anyone. Released at the end of the enclosing
    // full expression unless a consumer claims ownership first.
    struct TempRelease { std::string ptr; std::string className; };
    std::vector<TempRelease> pendingTemps_;

    // ---- Module-level state ----
    IRModule           module;
    const ExprTypeMap* typeMap        = nullptr;
    int                stringCounter  = 0;
    std::string        currentClassName_;  // set while generating a method

    // ---- Per-function state (reset in genFunction) ----
    IRFunction*  currentFunction    = nullptr;
    BasicBlock*  currentBasicBlock  = nullptr;
    int          tempCounter        = 0;
    int          labelCounter       = 0;
    Type         currentReturnType  = Type{TypeKind::Void};

    // funcName → ordered list of declared parameter types (populated in generate())
    std::unordered_map<std::string, std::vector<Type>> funcParamTypes;

    bool boundsCheck = true;   // from CompilerOptions; false disables runtime checks

    // varName → alloca pointer register name, e.g. "x" → "%x.addr"
    std::unordered_map<std::string, std::string> allocaMap;

    // varName → declared Type (needed to emit correct load/store sizes)
    std::unordered_map<std::string, Type>        varTypeMap;

    // All alloca pointer names ever created in the current function.
    // Persists across scope save/restore so re-used variable names (e.g.
    // two for-loops both declaring 'i') always get distinct LLVM names.
    std::unordered_set<std::string>              usedAllocaNames;

    // Innermost loop exit label (for break) and re-entry label (for continue).
    // Pushed on loop entry, popped on loop exit — supports arbitrary nesting.
    std::vector<std::string> breakLabelStack;
    std::vector<std::string> continueLabelStack;

    // ---- Function / extern / class codegen ----
    void genFunction(const FunctionDeclStmt& function);
    void genExternDecl(const ExternFuncDeclStmt& ext);
    void genClassDecl(const ClassDeclStmt& classDecl);
    void genMethod(const std::string& className, const MethodDecl& method);
    // Generate @Class_dtor: runs the user destructor body (if any), then releases
    // each reference field in reverse declaration order. Emitted for any class that
    // has a user destructor or any reference field.
    void genDestructor(const ClassDeclStmt& classDecl);
    // Generate @Class_clone(ptr %dest, ptr %src): memberwise deep copy — copy
    // primitive fields, copy+retain reference fields, releasing dest's old targets.
    void genCloneFunction(const std::string& className);

    // ---- Statement codegen ----
    void genStmt(const Stmt& stmt);
    void genBlock(const BlockStmt& blockStmt);
    void genIf(const IfStmt& ifStmt);
    void genWhile(const WhileStmt& whileStmt);
    void genFor(const ForStmt& forStmt);
    void genReturn(const ReturnStmt& returnStmt);
    void genBreak(const BreakStmt& breakStmt);
    void genContinue(const ContinueStmt& continueStmt);

    // ---- Expression codegen — return SSA value string ("%t3", "42", …) ----
    std::string genExpr(const Expr& expr);
    std::string genLiteral(const LiteralExpr& literal, const Type& resolvedType);
    std::string genIdentifier(const IdentifierExpr& identifier);
    std::string genUnary(const UnaryExpr& unary, const Type& resolvedType);
    std::string genBinary(const BinaryExpr& binary, const Type& resolvedType);
    std::string genAssign(const AssignExpr& assign);
    std::string genCompoundAssign(const CompoundAssignExpr& compoundAssign);
    std::string genPostfix(const PostfixExpr& postfix);
    std::string genCall(const CallExpr& call, const Type& resolvedType);
    std::string genVarDecl(const VarDeclExpr& varDecl);
    std::string genIndex(const IndexExpr& indexExpr);
    std::string genIndexAssign(const IndexAssignExpr& indexAssign);
    std::string genThis(const ThisExpr& thisExpr);
    std::string genMemberAccess(const MemberAccessExpr& memberAccess);
    std::string genMemberAssign(const MemberAssignExpr& memberAssign);
    std::string genMethodCall(const MethodCallExpr& methodCall, const Type& resolvedType);
    std::string genCast(const CastExpr& castExpr, const Type& toType);
    std::string genNew(const NewExpr& newExpr, const Type& resolvedType);
    std::string genSizeof(const SizeofExpr& sizeofExpr);

    // ---- Destructor helpers ----
    // Emit destructor / release calls for all entries in one scope (reverse order).
    void emitDtorsForScope(const std::vector<DtorEntry>& scope);

    // Emit the refcount runtime (gg_alloc/gg_retain/gg_release) + malloc/free decls.
    void emitRefcountRuntime();

    // ---- Shared codegen helpers ----
    // Returns a unique alloca pointer name for `varName` (e.g. "%x.addr" or "%x.addr.1")
    // and registers it in usedAllocaNames.
    std::string freshAllocaName(const std::string& varName);

    // Spill a parameter list into alloca slots; populates allocaMap / varTypeMap.
    void spillParamsToAllocas(const std::vector<ParamDecl>& params);

    // Emit the body of a function (open dtor scope, gen stmts, ensure termination, close scope).
    void emitFunctionBody(const BlockStmt& body, const std::string& returnIrType);

    // Build a comma-separated LLVM argument string for a call, casting each arg to
    // the declared param type when provided.
    std::string buildArgString(const std::vector<std::unique_ptr<Expr>>& args,
                               const std::vector<Type>* declaredParamTypes);

    // Emit a GEP for field `fieldName` on `objPtr` of class `className`.
    // Returns {gepRegister, fieldType}; returns {"0", Error} on lookup failure.
    std::pair<std::string, Type> resolveFieldGEP(const std::string& objPtr,
                                                  const std::string& className,
                                                  const std::string& fieldName);

    // ---- Bounds check helpers ----
    void ensureAbortDeclared();
    void emitBoundsCheck(const std::string& indexValue, size_t arraySize);

    // ---- Low-level emit helpers ----
    void        emit(const std::string& instruction);
    void        emitAlloca(const std::string& ptrName, const std::string& irType);
    void        emitStore(const std::string& irType,
                          const std::string& value,
                          const std::string& ptr);
    std::string emitLoad(const std::string& irType, const std::string& ptr);
    void        emitBr(const std::string& label);
    void        emitCondBr(const std::string& cond,
                           const std::string& trueLabel,
                           const std::string& falseLabel);
    void        switchBlock(const std::string& label);

    // ---- Value / type helpers ----
    std::string freshTemp();
    std::string freshLabel(const std::string& hint);

    // Resolve a ParamDecl type token — handles IDENTIFIER tokens naming a class.
    Type        resolveParamType(const ParamDecl& param) const;

    // Resolve a return-type token. Decodes "Class&" → Reference; otherwise primitives/void.
    Type        resolveReturnType(const Token& typeToken) const;

    // ---- Reference-return / temporary lifetime helpers ----
    // True if `e` yields a +1 reference (a `new` or a reference-returning call).
    [[nodiscard]] bool producesPlusOne(const Expr& e) const;
    // A consumer takes ownership of the +1 temporary `ptr` (drop its pending release).
    void claimTemp(const std::string& ptr);
    // Release every still-pending +1 reference temporary (end of a full expression).
    void flushTempReleases();

    // IR type used to pass `type` as a function argument / parameter.
    // Objects are passed by reference (ptr); all other types by value.
    static std::string paramIrType(const Type& type);

    // Look up the resolved type for an expression via the side-table.
    // Returns TypeKind::Error if not found (should not happen after semantic pass).
    Type        exprType(const Expr& expression) const;

    // Insert a cast instruction if from != to; return (possibly unchanged) value reg.
    std::string emitCast(const std::string& value, const Type& from, const Type& to);

    // Ensure result is i1 (suitable for conditional branches).
    std::string emitToBool(const std::string& value, const Type& valueType);

    // Arithmetic instruction name (add/fadd/sub/…) for a binary op on type t.
    static std::string arithInstr(TokenType operatorType, const Type& type);

    // Comparison instruction fragment (icmp slt / fcmp olt / …).
    static std::string cmpInstr(TokenType operatorType, const Type& type);

    // Compound-assign base op (PLUS_EQUAL → PLUS, etc.).
    static TokenType compoundBaseOp(TokenType operatorType);
};


#endif //GG_CODE_GEN_H
