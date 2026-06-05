//
// Created by Vladimir Arsenijevic on 4.6.2026.
//

#include "CodeGen.h"
#include "../semantic/Type.h"

#include <cassert>
#include <cstdint>
#include <sstream>

// ============================================================
// Public entry point
// ============================================================

IRModule CodeGen::generate(const Program& program, const ExprTypeMap& inputTypeMap, const CompilerOptions& options) {
    module        = {};
    this->typeMap = &inputTypeMap;
    stringCounter = 0;
    boundsCheck   = options.boundsCheck;
    funcParamTypes.clear();

    // Build function parameter type table (used in genCall to cast arguments).
    for (const auto& decl : program.declarations) {
        if (!decl.node) continue;
        if (std::holds_alternative<FunctionDeclStmt>(*decl.node)) {
            const auto& function = std::get<FunctionDeclStmt>(*decl.node);
            std::vector<Type> paramTypes;
            for (const auto& param : function.params)
                paramTypes.push_back(typeFromToken(param.typeName.type));
            funcParamTypes[function.name.lexeme] = std::move(paramTypes);
        } else if (std::holds_alternative<ExternFuncDeclStmt>(*decl.node)) {
            const auto& externDecl = std::get<ExternFuncDeclStmt>(*decl.node);
            std::vector<Type> paramTypes;
            for (const auto& param : externDecl.params)
                paramTypes.push_back(typeFromToken(param.typeName.type));
            funcParamTypes[externDecl.name.lexeme] = std::move(paramTypes);
        }
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

// ============================================================
// Function codegen
// ============================================================

void CodeGen::genFunction(const FunctionDeclStmt& function) {
    // Reset per-function state
    tempCounter         = 0;
    labelCounter        = 0;
    allocaMap.clear();
    varTypeMap.clear();
    usedAllocaNames.clear();
    breakLabelStack.clear();
    continueLabelStack.clear();

    // Return type
    currentReturnType        = typeFromToken(function.returnType.type);
    std::string returnIrType = irTypeName(currentReturnType);

    // Build parameter list string
    std::string parameterString;
    bool first = true;
    for (const auto& param : function.params) {
        if (!first) parameterString += ", ";
        first = false;
        parameterString += irTypeName(typeFromToken(param.typeName.type));
        parameterString += " %";
        parameterString += param.name.lexeme;
    }

    IRFunction irFunction;
    irFunction.signature = "define " + returnIrType + " @" + function.name.lexeme + "(" + parameterString + ")";

    module.functions.push_back(std::move(irFunction));
    currentFunction = &module.functions.back();

    // Create the entry basic block
    currentFunction->blocks.push_back(BasicBlock{"entry", {}, false});
    currentBasicBlock = &currentFunction->blocks.back();

    // Spill each parameter into an alloca so it can be reassigned.
    for (const auto& param : function.params) {
        Type        paramType  = typeFromToken(param.typeName.type);
        std::string irType     = irTypeName(paramType);
        std::string ptrName    = "%" + param.name.lexeme + ".addr";
        emitAlloca(ptrName, irType);
        usedAllocaNames.insert(ptrName);
        allocaMap[param.name.lexeme]  = ptrName;
        varTypeMap[param.name.lexeme] = paramType;
        emitStore(irType, "%" + param.name.lexeme, ptrName);
    }

    // Codegen body statements
    for (const auto& stmtPtr : function.body.body) {
        if (stmtPtr) genStmt(*stmtPtr);
    }

    // Ensure each block is terminated
    if (currentBasicBlock && !currentBasicBlock->terminated) {
        if (returnIrType == "void") {
            emit("ret void");
        } else {
            // Semantic pass should have caught missing return; emit unreachable
            emit("unreachable");
        }
        currentBasicBlock->terminated = true;
    }

    currentFunction   = nullptr;
    currentBasicBlock = nullptr;
}

void CodeGen::genExternDecl(const ExternFuncDeclStmt& externDecl) {
    std::string returnIrType = irTypeName(typeFromToken(externDecl.returnType.type));

    std::string parameterString;
    bool first = true;
    for (const auto& param : externDecl.params) {
        if (!first) parameterString += ", ";
        first = false;
        parameterString += irTypeName(typeFromToken(param.typeName.type));
    }

    module.declares.push_back(
        "declare " + returnIrType + " @" + externDecl.name.lexeme + "(" + parameterString + ")");
}

// ============================================================
// Statement codegen
// ============================================================

void CodeGen::genStmt(const Stmt& stmt) {
    std::visit(overloaded{
        [&](const ExprStmt& exprStmt)      { genExpr(exprStmt.expression); },
        [&](const BlockStmt& blockStmt)    { genBlock(blockStmt); },
        [&](const IfStmt& ifStmt)          { genIf(ifStmt); },
        [&](const WhileStmt& whileStmt)    { genWhile(whileStmt); },
        [&](const ForStmt& forStmt)        { genFor(forStmt); },
        [&](const ReturnStmt& returnStmt)    { genReturn(returnStmt); },
        [&](const BreakStmt& breakStmt)     { genBreak(breakStmt); },
        [&](const ContinueStmt& continueStmt) { genContinue(continueStmt); },
        [&](const FunctionDeclStmt&)         { /* nested functions not supported */ },
        [&](const ExternFuncDeclStmt&)       { /* handled at module level in generate() */ },
        [&](const ImportStmt&)               { /* resolved before codegen pass */ },
    }, *stmt.node);
}

void CodeGen::genBlock(const BlockStmt& blockStmt) {
    // Snapshot current scope so inner declarations / shadows are undone on exit.
    auto savedAllocas = allocaMap;
    auto savedTypes   = varTypeMap;

    for (const auto& stmtPtr : blockStmt.body) {
        if (stmtPtr) genStmt(*stmtPtr);
    }

    // Restore: removes names added in this block and restores any shadowed names.
    allocaMap  = std::move(savedAllocas);
    varTypeMap = std::move(savedTypes);
}

void CodeGen::genIf(const IfStmt& ifStmt) {
    int labelIndex          = ++labelCounter;
    std::string thenLabel   = "if.then."  + std::to_string(labelIndex);
    std::string elseLabel   = "if.else."  + std::to_string(labelIndex);
    std::string mergeLabel  = "if.merge." + std::to_string(labelIndex);

    Type        conditionType  = exprType(ifStmt.condition);
    std::string conditionValue = genExpr(ifStmt.condition);
    std::string conditionBool  = emitToBool(conditionValue, conditionType);

    if (ifStmt.elseBranch)
        emitCondBr(conditionBool, thenLabel, elseLabel);
    else
        emitCondBr(conditionBool, thenLabel, mergeLabel);

    // Then block
    switchBlock(thenLabel);
    genStmt(*ifStmt.thenBranch);
    if (!currentBasicBlock->terminated) emitBr(mergeLabel);

    // Else block
    if (ifStmt.elseBranch) {
        switchBlock(elseLabel);
        genStmt(*ifStmt.elseBranch);
        if (!currentBasicBlock->terminated) emitBr(mergeLabel);
    }

    // Merge block
    switchBlock(mergeLabel);
}

void CodeGen::genWhile(const WhileStmt& whileStmt) {
    int labelIndex          = ++labelCounter;
    std::string condLabel   = "while.cond."  + std::to_string(labelIndex);
    std::string bodyLabel   = "while.body."  + std::to_string(labelIndex);
    std::string mergeLabel  = "while.merge." + std::to_string(labelIndex);

    emitBr(condLabel);

    switchBlock(condLabel);
    Type        conditionType  = exprType(whileStmt.condition);
    std::string conditionValue = genExpr(whileStmt.condition);
    std::string conditionBool  = emitToBool(conditionValue, conditionType);
    emitCondBr(conditionBool, bodyLabel, mergeLabel);

    switchBlock(bodyLabel);
    breakLabelStack.push_back(mergeLabel);
    continueLabelStack.push_back(condLabel);
    genStmt(*whileStmt.body);
    breakLabelStack.pop_back();
    continueLabelStack.pop_back();
    if (!currentBasicBlock->terminated) emitBr(condLabel);

    switchBlock(mergeLabel);
}

void CodeGen::genFor(const ForStmt& forStmt) {
    // The for-init variable belongs to the for scope (not the enclosing scope).
    auto savedAllocas = allocaMap;
    auto savedTypes   = varTypeMap;

    int labelIndex          = ++labelCounter;
    std::string condLabel   = "for.cond."  + std::to_string(labelIndex);
    std::string bodyLabel   = "for.body."  + std::to_string(labelIndex);
    std::string incLabel    = "for.inc."   + std::to_string(labelIndex);
    std::string mergeLabel  = "for.merge." + std::to_string(labelIndex);

    if (forStmt.init) genStmt(*forStmt.init);

    emitBr(condLabel);

    switchBlock(condLabel);
    if (forStmt.condition.has_value()) {
        Type        conditionType  = exprType(*forStmt.condition);
        std::string conditionValue = genExpr(*forStmt.condition);
        std::string conditionBool  = emitToBool(conditionValue, conditionType);
        emitCondBr(conditionBool, bodyLabel, mergeLabel);
    } else {
        emitBr(bodyLabel);  // for(;;)
    }

    switchBlock(bodyLabel);
    breakLabelStack.push_back(mergeLabel);
    continueLabelStack.push_back(incLabel);
    genStmt(*forStmt.body);
    breakLabelStack.pop_back();
    continueLabelStack.pop_back();
    if (!currentBasicBlock->terminated) emitBr(incLabel);

    switchBlock(incLabel);
    if (forStmt.increment.has_value()) genExpr(*forStmt.increment);
    emitBr(condLabel);

    switchBlock(mergeLabel);

    // Restore scope — for-init variable goes out of scope
    allocaMap  = std::move(savedAllocas);
    varTypeMap = std::move(savedTypes);
}

void CodeGen::genReturn(const ReturnStmt& returnStmt) {
    if (returnStmt.value.has_value()) {
        Type        returnValueType = exprType(*returnStmt.value);
        std::string value           = genExpr(*returnStmt.value);
        // Cast the value to the declared return type if needed
        value = emitCast(value, returnValueType, currentReturnType);
        emit("ret " + irTypeName(currentReturnType) + " " + value);
    } else {
        emit("ret void");
    }
    if (currentBasicBlock) currentBasicBlock->terminated = true;
}

void CodeGen::genBreak(const BreakStmt&) {
    if (!breakLabelStack.empty())
        emitBr(breakLabelStack.back());
}

void CodeGen::genContinue(const ContinueStmt&) {
    if (!continueLabelStack.empty())
        emitBr(continueLabelStack.back());
}

// ============================================================
// Expression codegen
// ============================================================

std::string CodeGen::genExpr(const Expr& expr) {
    Type resolvedType = exprType(expr);
    return std::visit(overloaded{
        [&](const LiteralExpr& literal)              -> std::string { return genLiteral(literal, resolvedType); },
        [&](const IdentifierExpr& identifier)        -> std::string { return genIdentifier(identifier); },
        [&](const UnaryExpr& unary)                  -> std::string { return genUnary(unary, resolvedType); },
        [&](const BinaryExpr& binary)                -> std::string { return genBinary(binary, resolvedType); },
        [&](const AssignExpr& assign)                -> std::string { return genAssign(assign); },
        [&](const CompoundAssignExpr& compoundAssign)-> std::string { return genCompoundAssign(compoundAssign); },
        [&](const PostfixExpr& postfix)              -> std::string { return genPostfix(postfix); },
        [&](const CallExpr& call)                    -> std::string { return genCall(call, resolvedType); },
        [&](const VarDeclExpr& varDecl)              -> std::string { return genVarDecl(varDecl); },
        [&](const IndexExpr& indexExpr)              -> std::string { return genIndex(indexExpr); },
        [&](const IndexAssignExpr& indexAssign)      -> std::string { return genIndexAssign(indexAssign); },
    }, *expr.node);
}

// ---- Literal ----

std::string CodeGen::genLiteral(const LiteralExpr& literal, Type resolvedType) {
    const std::string& lexeme = literal.token.lexeme;

    switch (literal.token.type) {
        case TokenType::NUMBER: {
            if (lexeme.find('.') != std::string::npos) {
                // Float literal — ensure at least one digit after '.'
                std::string value = lexeme;
                if (!value.empty() && value.back() == '.') value += '0';
                return value;
            }
            // Integer literal
            return lexeme;
        }
        case TokenType::TRUE:  return "1";
        case TokenType::FALSE: return "0";

        case TokenType::CHAR: {
            // The lexer stores the char lexeme WITHOUT the surrounding single quotes.
            // char is a 32-bit Unicode code point (u32).
            if (lexeme.empty()) return "0";

            // ---- Escape sequences ----
            if (lexeme[0] == '\\' && lexeme.size() >= 2) {
                switch (lexeme[1]) {
                    case 'n':  return "10";
                    case 't':  return "9";
                    case 'r':  return "13";
                    case '\\': return "92";
                    case '\'': return "39";
                    case '"':  return "34";
                    case '0':  return "0";
                    default:   return std::to_string(static_cast<uint32_t>(
                                    static_cast<unsigned char>(lexeme[1])));
                }
            }

            // ---- UTF-8 → Unicode code point decoding ----
            const auto* bytes = reinterpret_cast<const unsigned char*>(lexeme.data());
            const size_t  len = lexeme.size();
            uint32_t cp = 0;

            if (len >= 1 && (bytes[0] & 0x80) == 0x00) {
                // 1-byte: 0xxxxxxx
                cp = bytes[0];
            } else if (len >= 2 && (bytes[0] & 0xE0) == 0xC0) {
                // 2-byte: 110xxxxx 10xxxxxx
                cp = static_cast<uint32_t>((bytes[0] & 0x1F) << 6)
                   | static_cast<uint32_t>( bytes[1] & 0x3F);
            } else if (len >= 3 && (bytes[0] & 0xF0) == 0xE0) {
                // 3-byte: 1110xxxx 10xxxxxx 10xxxxxx
                cp = static_cast<uint32_t>((bytes[0] & 0x0F) << 12)
                   | static_cast<uint32_t>((bytes[1] & 0x3F) <<  6)
                   | static_cast<uint32_t>( bytes[2] & 0x3F);
            } else if (len >= 4 && (bytes[0] & 0xF8) == 0xF0) {
                // 4-byte: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
                cp = static_cast<uint32_t>((bytes[0] & 0x07) << 18)
                   | static_cast<uint32_t>((bytes[1] & 0x3F) << 12)
                   | static_cast<uint32_t>((bytes[2] & 0x3F) <<  6)
                   | static_cast<uint32_t>( bytes[3] & 0x3F);
            } else {
                cp = bytes[0];  // invalid UTF-8 — use first byte as-is
            }

            return std::to_string(cp);
        }

        case TokenType::STRING: {
            // The lexer stores the string lexeme WITHOUT the surrounding double quotes.
            // Convert GG escape sequences to LLVM hex escape sequences.
            std::string content;
            int byteCount = 0;
            for (size_t i = 0; i < lexeme.size(); ++i) {
                if (lexeme[i] == '\\' && i + 1 < lexeme.size()) {
                    char escaped = lexeme[++i];
                    switch (escaped) {
                        case 'n':  content += "\\0A"; break;
                        case 't':  content += "\\09"; break;
                        case '\\': content += "\\5C"; break;
                        case '"':  content += "\\22"; break;
                        case '0':  content += "\\00"; break;
                        default:   content += lexeme[i]; break;
                    }
                } else {
                    content += lexeme[i];
                }
                ++byteCount;
            }
            int totalBytes = byteCount + 1;  // +1 for null terminator

            std::string globalName = "@.str." + std::to_string(stringCounter++);
            module.globals.push_back(
                globalName + " = private unnamed_addr constant ["
                + std::to_string(totalBytes) + " x i8] c\""
                + content + "\\00\", align 1");

            std::string tempName = freshTemp();
            emit("%" + tempName + " = getelementptr inbounds ["
                + std::to_string(totalBytes) + " x i8], ptr "
                + globalName + ", i32 0, i32 0");
            return "%" + tempName;
        }

        default:
            (void)resolvedType;
            return "0";
    }
}

// ---- Identifier ----

std::string CodeGen::genIdentifier(const IdentifierExpr& identifier) {
    auto allocaIt  = allocaMap.find(identifier.name.lexeme);
    if (allocaIt == allocaMap.end()) return "0";  // undefined — semantic pass should catch this

    auto varTypeIt = varTypeMap.find(identifier.name.lexeme);
    if (varTypeIt == varTypeMap.end()) return "0";

    std::string irType  = irTypeName(varTypeIt->second);
    std::string ptrName = allocaIt->second;
    return emitLoad(irType, ptrName);
}

// ---- Unary ----

std::string CodeGen::genUnary(const UnaryExpr& unary, Type resolvedType) {
    switch (unary.operatorToken.type) {
        case TokenType::MINUS: {
            std::string value      = genExpr(*unary.operand);
            Type        operandType = exprType(*unary.operand);
            std::string irType     = irTypeName(operandType);
            std::string tempName   = freshTemp();
            if (isFloat(operandType.kind))
                emit("%" + tempName + " = fneg " + irType + " " + value);
            else
                emit("%" + tempName + " = sub " + irType + " 0, " + value);
            return "%" + tempName;
        }
        case TokenType::BANG: {
            std::string value      = genExpr(*unary.operand);
            Type        operandType = exprType(*unary.operand);
            std::string boolValue  = emitToBool(value, operandType);
            std::string tempName   = freshTemp();
            emit("%" + tempName + " = xor i1 " + boolValue + ", true");
            return "%" + tempName;
        }
        case TokenType::TILDE: {
            std::string value      = genExpr(*unary.operand);
            Type        operandType = exprType(*unary.operand);
            std::string irType     = irTypeName(operandType);
            std::string tempName   = freshTemp();
            emit("%" + tempName + " = xor " + irType + " " + value + ", -1");
            return "%" + tempName;
        }
        case TokenType::INCREMENT:
        case TokenType::DECREMENT: {
            // Prefix ++/-- : load, modify, store, return new value
            const auto& id       = std::get<IdentifierExpr>(*unary.operand->node);
            auto        varTypeIt = varTypeMap.find(id.name.lexeme);
            auto        allocaIt  = allocaMap.find(id.name.lexeme);
            if (varTypeIt == varTypeMap.end() || allocaIt == allocaMap.end()) return "0";
            Type        variableType = varTypeIt->second;
            std::string irType       = irTypeName(variableType);
            std::string ptrName      = allocaIt->second;
            std::string oldValue     = emitLoad(irType, ptrName);
            std::string tempName     = freshTemp();
            std::string one          = isFloat(variableType.kind) ? "1.0" : "1";
            std::string instruction  = (unary.operatorToken.type == TokenType::INCREMENT) ? "add" : "sub";
            if (isFloat(variableType.kind))
                instruction = (unary.operatorToken.type == TokenType::INCREMENT) ? "fadd" : "fsub";
            emit("%" + tempName + " = " + instruction + " " + irType + " " + oldValue + ", " + one);
            emitStore(irType, "%" + tempName, ptrName);
            return "%" + tempName;
        }
        default:
            (void)resolvedType;
            return genExpr(*unary.operand);
    }
}

// ---- Binary ----

std::string CodeGen::genBinary(const BinaryExpr& binary, Type resolvedType) {
    TokenType operatorType = binary.operatorToken.type;

    // Short-circuit logical ops
    if (operatorType == TokenType::AND || operatorType == TokenType::OR) {
        // Eager evaluation — emit and i1 / or i1
        Type        leftType  = exprType(*binary.left);
        Type        rightType = exprType(*binary.right);
        std::string leftValue  = genExpr(*binary.left);
        std::string rightValue = genExpr(*binary.right);
        std::string leftBool   = emitToBool(leftValue,  leftType);
        std::string rightBool  = emitToBool(rightValue, rightType);
        std::string tempName   = freshTemp();
        std::string instruction = (operatorType == TokenType::AND) ? "and" : "or";
        emit("%" + tempName + " = " + instruction + " i1 " + leftBool + ", " + rightBool);
        return "%" + tempName;
    }

    // Comparison ops — result is Bool (i1)
    bool isComparison = (operatorType == TokenType::EQUAL_EQUAL || operatorType == TokenType::BANG_EQUAL ||
                         operatorType == TokenType::LESS         || operatorType == TokenType::LESS_EQUAL  ||
                         operatorType == TokenType::GREATER      || operatorType == TokenType::GREATER_EQUAL);

    Type leftType  = exprType(*binary.left);
    Type rightType = exprType(*binary.right);

    // Determine the arithmetic type for the operation
    Type operandType = isComparison
                     ? commonArithmeticType(leftType, rightType)
                     : resolvedType;

    std::string leftValue  = genExpr(*binary.left);
    std::string rightValue = genExpr(*binary.right);

    // Cast operands to the common type
    leftValue  = emitCast(leftValue,  leftType,  operandType);
    rightValue = emitCast(rightValue, rightType, operandType);

    std::string irType   = irTypeName(operandType);
    std::string tempName = freshTemp();

    if (isComparison) {
        std::string comparisonInstruction = cmpInstr(operatorType, operandType);
        emit("%" + tempName + " = " + comparisonInstruction + " " + irType + " " + leftValue + ", " + rightValue);
    } else {
        std::string arithmeticInstruction = arithInstr(operatorType, operandType);
        emit("%" + tempName + " = " + arithmeticInstruction + " " + irType + " " + leftValue + ", " + rightValue);
    }
    return "%" + tempName;
}

// ---- Assign ----

std::string CodeGen::genAssign(const AssignExpr& assign) {
    auto varTypeIt = varTypeMap.find(assign.name.lexeme);
    auto allocaIt  = allocaMap.find(assign.name.lexeme);
    if (varTypeIt == varTypeMap.end() || allocaIt == allocaMap.end()) return "0";

    Type        lhsType = varTypeIt->second;
    std::string irType  = irTypeName(lhsType);
    std::string ptrName = allocaIt->second;

    Type        rhsType = exprType(*assign.value);
    std::string value   = genExpr(*assign.value);
    value = emitCast(value, rhsType, lhsType);

    emitStore(irType, value, ptrName);
    return value;
}

// ---- CompoundAssign ----

std::string CodeGen::genCompoundAssign(const CompoundAssignExpr& compoundAssign) {
    auto varTypeIt = varTypeMap.find(compoundAssign.name.lexeme);
    auto allocaIt  = allocaMap.find(compoundAssign.name.lexeme);
    if (varTypeIt == varTypeMap.end() || allocaIt == allocaMap.end()) return "0";

    Type        lhsType      = varTypeIt->second;
    std::string irType       = irTypeName(lhsType);
    std::string ptrName      = allocaIt->second;

    // Load current value
    std::string currentValue = emitLoad(irType, ptrName);

    // Evaluate RHS
    Type        rhsType    = exprType(*compoundAssign.value);
    std::string rightValue = genExpr(*compoundAssign.value);
    rightValue = emitCast(rightValue, rhsType, lhsType);

    // Apply the base operation
    TokenType   baseOperatorType        = compoundBaseOp(compoundAssign.operatorToken.type);
    std::string arithmeticInstruction   = arithInstr(baseOperatorType, lhsType);
    std::string tempName                = freshTemp();
    emit("%" + tempName + " = " + arithmeticInstruction + " " + irType + " " + currentValue + ", " + rightValue);

    emitStore(irType, "%" + tempName, ptrName);
    return "%" + tempName;
}

// ---- Postfix ----

std::string CodeGen::genPostfix(const PostfixExpr& postfix) {
    const auto& id       = std::get<IdentifierExpr>(*postfix.operand->node);
    auto        varTypeIt = varTypeMap.find(id.name.lexeme);
    auto        allocaIt  = allocaMap.find(id.name.lexeme);
    if (varTypeIt == varTypeMap.end() || allocaIt == allocaMap.end()) return "0";

    Type        variableType = varTypeIt->second;
    std::string irType       = irTypeName(variableType);
    std::string ptrName      = allocaIt->second;

    // Load old value (this is the result of the postfix expression)
    std::string oldValue = emitLoad(irType, ptrName);

    // Compute new value
    std::string tempName    = freshTemp();
    std::string one         = isFloat(variableType.kind) ? "1.0" : "1";
    std::string instruction = (postfix.operatorToken.type == TokenType::INCREMENT) ? "add" : "sub";
    if (isFloat(variableType.kind))
        instruction = (postfix.operatorToken.type == TokenType::INCREMENT) ? "fadd" : "fsub";
    emit("%" + tempName + " = " + instruction + " " + irType + " " + oldValue + ", " + one);
    emitStore(irType, "%" + tempName, ptrName);

    return oldValue;  // return OLD value
}

// ---- Call ----

std::string CodeGen::genCall(const CallExpr& call, Type resolvedType) {
    std::string returnIrType = irTypeName(resolvedType);

    // Look up declared parameter types so we can cast each argument correctly.
    auto funcLookup = funcParamTypes.find(call.callee.lexeme);
    const std::vector<Type>* declaredParamTypes =
        funcLookup != funcParamTypes.end() ? &funcLookup->second : nullptr;

    std::string argumentString;
    bool   first      = true;
    size_t paramIndex = 0;
    for (const auto& arg : call.args) {
        if (!first) argumentString += ", ";
        first = false;
        Type        argType = exprType(*arg);
        std::string value   = genExpr(*arg);

        // Cast to the declared parameter type when the IR types differ.
        if (declaredParamTypes && paramIndex < declaredParamTypes->size()) {
            Type paramType = (*declaredParamTypes)[paramIndex];
            value   = emitCast(value, argType, paramType);
            argType = paramType;
        }
        ++paramIndex;

        argumentString += irTypeName(argType) + " " + value;
    }

    if (returnIrType == "void") {
        emit("call void @" + call.callee.lexeme + "(" + argumentString + ")");
        return "";  // void call has no result
    }

    std::string tempName = freshTemp();
    emit("%" + tempName + " = call " + returnIrType + " @" + call.callee.lexeme + "(" + argumentString + ")");
    return "%" + tempName;
}

// ---- VarDecl ----

std::string CodeGen::genVarDecl(const VarDeclExpr& varDecl) {
    // ---- Array declaration ----
    if (varDecl.arraySize > 0) {
        TypeKind elementKind = typeFromToken(varDecl.typeName.type).kind;
        Type     arrayType   = makeArrayType(elementKind, varDecl.arraySize);
        std::string arrayIrType = irTypeName(arrayType);
        std::string name        = varDecl.name.lexeme;

        std::string ptrName = "%" + name + ".addr";
        if (usedAllocaNames.count(ptrName)) {
            int suffix = 1;
            while (usedAllocaNames.count(ptrName + "." + std::to_string(suffix)))
                ++suffix;
            ptrName += "." + std::to_string(suffix);
        }
        usedAllocaNames.insert(ptrName);

        emitAlloca(ptrName, arrayIrType);
        allocaMap[name]  = ptrName;
        varTypeMap[name] = arrayType;

        // Zero-initialise the entire array in one store
        emit("store " + arrayIrType + " zeroinitializer, ptr " + ptrName);

        return ptrName;
    }

    // ---- Scalar declaration (existing logic) ----
    Type        declaredType = typeFromToken(varDecl.typeName.type);
    std::string irType       = irTypeName(declaredType);
    std::string name         = varDecl.name.lexeme;

    // Build a unique alloca pointer name.
    // We consult usedAllocaNames (which persists across scope save/restore) so
    // that two variables with the same name in sibling scopes — e.g. two for-loops
    // both declaring 'i' — always get distinct LLVM value names within the function.
    std::string ptrName = "%" + name + ".addr";
    if (usedAllocaNames.count(ptrName)) {
        int suffix = 1;
        while (usedAllocaNames.count(ptrName + "." + std::to_string(suffix)))
            ++suffix;
        ptrName += "." + std::to_string(suffix);
    }
    usedAllocaNames.insert(ptrName);

    emitAlloca(ptrName, irType);
    allocaMap[name]  = ptrName;
    varTypeMap[name] = declaredType;

    if (varDecl.initializer) {
        Type        initializerType = exprType(*varDecl.initializer);
        std::string value           = genExpr(*varDecl.initializer);
        value = emitCast(value, initializerType, declaredType);
        emitStore(irType, value, ptrName);
    }

    return ptrName;
}

// ---- Index (array read) ----

std::string CodeGen::genIndex(const IndexExpr& indexExpr) {
    const std::string& name = indexExpr.name.lexeme;
    auto varTypeIt = varTypeMap.find(name);
    auto allocaIt  = allocaMap.find(name);
    if (varTypeIt == varTypeMap.end() || allocaIt == allocaMap.end()) return "0";

    Type        arrayType     = varTypeIt->second;
    Type        elementType{arrayType.elementKind};
    std::string elementIrType = irTypeName(elementType);
    std::string arrayIrType   = irTypeName(arrayType);
    std::string ptrName       = allocaIt->second;

    // Evaluate and widen the index to i64 for GEP (icmp ult also needs i64)
    Type        indexType  = exprType(*indexExpr.index);
    std::string indexValue = genExpr(*indexExpr.index);
    indexValue = emitCast(indexValue, indexType, Type{TypeKind::I64});

    if (boundsCheck) {
        ensureAbortDeclared();
        emitBoundsCheck(indexValue, arrayType.arraySize);
    }

    std::string elemPtr = freshTemp();
    emit("%" + elemPtr + " = getelementptr " + arrayIrType + ", ptr " + ptrName
         + ", i32 0, i64 " + indexValue);

    return emitLoad(elementIrType, "%" + elemPtr);
}

// ---- IndexAssign (array write) ----

std::string CodeGen::genIndexAssign(const IndexAssignExpr& indexAssign) {
    const std::string& name = indexAssign.name.lexeme;
    auto varTypeIt = varTypeMap.find(name);
    auto allocaIt  = allocaMap.find(name);
    if (varTypeIt == varTypeMap.end() || allocaIt == allocaMap.end()) return "0";

    Type        arrayType     = varTypeIt->second;
    Type        elementType{arrayType.elementKind};
    std::string elementIrType = irTypeName(elementType);
    std::string arrayIrType   = irTypeName(arrayType);
    std::string ptrName       = allocaIt->second;

    // Index (widen to i64)
    Type        indexType  = exprType(*indexAssign.index);
    std::string indexValue = genExpr(*indexAssign.index);
    indexValue = emitCast(indexValue, indexType, Type{TypeKind::I64});

    if (boundsCheck) {
        ensureAbortDeclared();
        emitBoundsCheck(indexValue, arrayType.arraySize);
    }

    std::string elemPtr = freshTemp();
    emit("%" + elemPtr + " = getelementptr " + arrayIrType + ", ptr " + ptrName
         + ", i32 0, i64 " + indexValue);

    // Value (cast to element type)
    Type        valueType = exprType(*indexAssign.value);
    std::string value     = genExpr(*indexAssign.value);
    value = emitCast(value, valueType, elementType);

    emitStore(elementIrType, value, "%" + elemPtr);
    return value;  // assignment expression returns the stored value
}

// ---- Bounds check helpers ----

void CodeGen::ensureAbortDeclared() {
    static const std::string abortDecl = "declare void @abort()";
    for (const auto& declaration : module.declares)
        if (declaration == abortDecl) return;
    module.declares.push_back(abortDecl);
}

void CodeGen::emitBoundsCheck(const std::string& indexValue, size_t arraySize) {
    std::string sizeStr  = std::to_string(arraySize);
    std::string okLabel  = freshLabel("bounds.ok");
    std::string oobLabel = freshLabel("bounds.oob");

    std::string cmp = freshTemp();
    // icmp ult catches negative indices too: negative i64 values are huge unsigned numbers
    emit("%" + cmp + " = icmp ult i64 " + indexValue + ", " + sizeStr);
    emitCondBr("%" + cmp, okLabel, oobLabel);

    switchBlock(oobLabel);
    emit("call void @abort()");
    emit("unreachable");
    currentBasicBlock->terminated = true;

    switchBlock(okLabel);
}

// ============================================================
// Low-level emit helpers
// ============================================================

void CodeGen::emit(const std::string& instruction) {
    if (currentBasicBlock && !currentBasicBlock->terminated)
        currentBasicBlock->instructions.push_back("  " + instruction);
}

void CodeGen::emitAlloca(const std::string& ptrName, const std::string& irType) {
    if (currentFunction)
        currentFunction->allocas.push_back("  " + ptrName + " = alloca " + irType);
}

void CodeGen::emitStore(const std::string& irType, const std::string& value, const std::string& ptr) {
    emit("store " + irType + " " + value + ", ptr " + ptr);
}

std::string CodeGen::emitLoad(const std::string& irType, const std::string& ptr) {
    std::string tempName = freshTemp();
    emit("%" + tempName + " = load " + irType + ", ptr " + ptr);
    return "%" + tempName;
}

void CodeGen::emitBr(const std::string& label) {
    if (currentBasicBlock && !currentBasicBlock->terminated) {
        emit("br label %" + label);
        currentBasicBlock->terminated = true;
    }
}

void CodeGen::emitCondBr(const std::string& cond,
                          const std::string& trueLabel,
                          const std::string& falseLabel) {
    if (currentBasicBlock && !currentBasicBlock->terminated) {
        emit("br i1 " + cond + ", label %" + trueLabel + ", label %" + falseLabel);
        currentBasicBlock->terminated = true;
    }
}

void CodeGen::switchBlock(const std::string& label) {
    if (currentFunction) {
        currentFunction->blocks.push_back(BasicBlock{label, {}, false});
        currentBasicBlock = &currentFunction->blocks.back();
    }
}

// ============================================================
// Value / type helpers
// ============================================================

std::string CodeGen::freshTemp() {
    return "t" + std::to_string(tempCounter++);
}

std::string CodeGen::freshLabel(const std::string& hint) {
    return hint + "." + std::to_string(++labelCounter);
}

Type CodeGen::exprType(const Expr& expression) const {
    if (!expression.node || !typeMap) return Type{TypeKind::Error};
    auto it = typeMap->find(expression.node.get());
    if (it == typeMap->end()) return Type{TypeKind::Error};
    return it->second;
}

std::string CodeGen::emitCast(const std::string& value, Type from, Type to) {
    if (from == to) return value;
    if (isError(from) || isError(to)) return value;

    // If both types map to the same LLVM IR type (e.g. string ↔ ptr, i32 ↔ u32,
    // char ↔ u32) no cast instruction is needed — bits are already identical.
    if (irTypeName(from) == irTypeName(to)) return value;

    auto getBitWidth = [](TypeKind kind) -> int {
        switch (kind) {
            case TypeKind::I8:
            case TypeKind::U8:
                return 8;
            case TypeKind::I16:
            case TypeKind::U16:
                return 16;
            case TypeKind::I32:
            case TypeKind::U32:
            case TypeKind::Char:
                return 32;
            case TypeKind::I64:
            case TypeKind::U64:
                return 64;
            case TypeKind::F32:
                return 32;
            case TypeKind::F64:
                return 64;
            case TypeKind::Bool:
                return 1;
            default:
                return 32;
        }
    };

    std::string fromIrType = irTypeName(from);
    std::string toIrType   = irTypeName(to);
    std::string instruction;

    if (isInteger(from.kind) && isInteger(to.kind)) {
        int fromBits = getBitWidth(from.kind);
        int toBits   = getBitWidth(to.kind);
        if (toBits > fromBits)
            instruction = isUnsignedInt(from.kind) ? "zext" : "sext";
        else if (toBits < fromBits)
            instruction = "trunc";
        else
            return value;  // Same IR bit-width — just reinterpret; no instruction needed
    } else if (from.kind == TypeKind::Bool && isInteger(to.kind)) {
        instruction = "zext";
    } else if (isInteger(from.kind) && to.kind == TypeKind::Bool) {
        // Convert to i1 via icmp ne
        return emitToBool(value, from);
    } else if (isFloat(from.kind) && isFloat(to.kind)) {
        int fromBits = getBitWidth(from.kind);
        int toBits   = getBitWidth(to.kind);
        instruction  = (toBits > fromBits) ? "fpext" : "fptrunc";
    } else if (isInteger(from.kind) && isFloat(to.kind)) {
        instruction = isSignedInt(from.kind) ? "sitofp" : "uitofp";
    } else if (isFloat(from.kind) && isInteger(to.kind)) {
        instruction = isSignedInt(to.kind) ? "fptosi" : "fptoui";
    } else {
        return value;  // no known cast
    }

    std::string tempName = freshTemp();
    emit("%" + tempName + " = " + instruction + " " + fromIrType + " " + value + " to " + toIrType);
    return "%" + tempName;
}

std::string CodeGen::emitToBool(const std::string& value, Type valueType) {
    if (valueType.kind == TypeKind::Bool) return value;

    std::string irType   = irTypeName(valueType);
    std::string tempName = freshTemp();
    if (isFloat(valueType.kind))
        emit("%" + tempName + " = fcmp une " + irType + " " + value + ", 0.0");
    else
        emit("%" + tempName + " = icmp ne " + irType + " " + value + ", 0");
    return "%" + tempName;
}

// ============================================================
// Arithmetic / comparison instruction selection
// ============================================================

std::string CodeGen::arithInstr(TokenType operatorType, Type type) {
    bool isFloat  = ::isFloat(type.kind);
    bool isSigned = isSignedInt(type.kind);

    switch (operatorType) {
        case TokenType::PLUS:        return isFloat ? "fadd" : "add";
        case TokenType::MINUS:       return isFloat ? "fsub" : "sub";
        case TokenType::STAR:        return isFloat ? "fmul" : "mul";
        case TokenType::SLASH:       return isFloat ? "fdiv" : (isSigned ? "sdiv" : "udiv");
        case TokenType::PERCENT:     return isFloat ? "frem" : (isSigned ? "srem" : "urem");
        case TokenType::AMPERSAND:   return "and";
        case TokenType::PIPE:        return "or";
        case TokenType::CARET:       return "xor";
        case TokenType::SHIFT_LEFT:  return "shl";
        case TokenType::SHIFT_RIGHT: return isSigned ? "ashr" : "lshr";
        default:                     return "add";  // fallback
    }
}

std::string CodeGen::cmpInstr(TokenType operatorType, Type type) {
    bool isFloat  = ::isFloat(type.kind);
    bool isSigned = isSignedInt(type.kind) || type.kind == TypeKind::Bool;

    if (isFloat) {
        // ordered comparisons (quiet NaN → false)
        switch (operatorType) {
            case TokenType::EQUAL_EQUAL:   return "fcmp oeq";
            case TokenType::BANG_EQUAL:    return "fcmp one";
            case TokenType::LESS:          return "fcmp olt";
            case TokenType::LESS_EQUAL:    return "fcmp ole";
            case TokenType::GREATER:       return "fcmp ogt";
            case TokenType::GREATER_EQUAL: return "fcmp oge";
            default:                       return "fcmp oeq";
        }
    } else {
        switch (operatorType) {
            case TokenType::EQUAL_EQUAL:   return "icmp eq";
            case TokenType::BANG_EQUAL:    return "icmp ne";
            case TokenType::LESS:          return isSigned ? "icmp slt" : "icmp ult";
            case TokenType::LESS_EQUAL:    return isSigned ? "icmp sle" : "icmp ule";
            case TokenType::GREATER:       return isSigned ? "icmp sgt" : "icmp ugt";
            case TokenType::GREATER_EQUAL: return isSigned ? "icmp sge" : "icmp uge";
            default:                       return "icmp eq";
        }
    }
}

TokenType CodeGen::compoundBaseOp(TokenType operatorType) {
    switch (operatorType) {
        case TokenType::PLUS_EQUAL:      return TokenType::PLUS;
        case TokenType::MINUS_EQUAL:     return TokenType::MINUS;
        case TokenType::STAR_EQUAL:      return TokenType::STAR;
        case TokenType::SLASH_EQUAL:     return TokenType::SLASH;
        case TokenType::PERCENT_EQUAL:   return TokenType::PERCENT;
        case TokenType::AMPERSAND_EQUAL: return TokenType::AMPERSAND;
        case TokenType::PIPE_EQUAL:      return TokenType::PIPE;
        case TokenType::CARET_EQUAL:     return TokenType::CARET;
        default:                         return TokenType::PLUS;
    }
}
