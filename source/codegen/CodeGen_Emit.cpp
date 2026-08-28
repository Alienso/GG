#include "CodeGen.h"

// ---- Bounds check helpers ----

void CodeGen::ensureAbortDeclared() {
    static const std::string abortDecl = "declare void @abort()";
    for (const auto& declaration : module.declares)
        if (declaration == abortDecl) return;
    module.declares.push_back(abortDecl);
}

// Emit, into the current (trap) block, a `fputs("GG runtime error: <what> at <file:line>\n", stderr)`
// so a runtime panic explains itself instead of aborting silently. The message is a compile-time
// constant (no runtime formatting); the line comes from `currentStmtLine_`, the file from the debug
// source path when available. stderr is obtained via `__acrt_iob_func(2)` — the MinGW/UCRT accessor
// (GG's target triple is fixed to x86_64-w64-windows-gnu).
void CodeGen::emitPanicMessage(const std::string& what) {
    // Build the human-readable message.
    std::string loc;
    if (currentStmtLine_ > 0) {
        std::string file;
        if (!dbgSourceFile_.empty()) {
            auto pos = dbgSourceFile_.find_last_of("/\\");
            file = (pos == std::string::npos) ? dbgSourceFile_ : dbgSourceFile_.substr(pos + 1);
        }
        // With a known source file (from `--debug`): "at file.gg:42"; otherwise just "at line 42".
        loc = file.empty() ? (" at line " + std::to_string(currentStmtLine_))
                            : (" at " + file + ":" + std::to_string(currentStmtLine_));
    }
    std::string msg = "GG runtime error: " + what + loc + "\n";

    // Encode the message as an LLVM c-string constant, escaping non-identifier bytes as \HH and
    // counting the exact byte length (message + trailing NUL).
    std::string encoded;
    for (unsigned char c : msg) {
        if (c == '\\' || c == '"' || c < 0x20 || c >= 0x7F) {
            char buf[5];
            std::snprintf(buf, sizeof(buf), "\\%02X", c);
            encoded += buf;
        } else {
            encoded += static_cast<char>(c);
        }
    }
    encoded += "\\00";
    size_t byteLen = msg.size() + 1;   // + NUL

    std::string g = "@.panic." + std::to_string(stringCounter++);
    module.globals.push_back(g + " = private unnamed_addr constant [" + std::to_string(byteLen)
                             + " x i8] c\"" + encoded + "\", align 1");

    // Declare `fputs` once; obtain the stderr FILE* from the platform-neutral `@gg_stderr` helper
    // (defined per-target by emitStdioHelpers). `panicUsesStderr_` forces that define even when the
    // stdlib isn't imported.
    const std::string fputsDecl = "declare i32 @fputs(ptr, ptr)";
    bool haveFputs = false;
    for (const auto& d : module.declares) if (d == fputsDecl) { haveFputs = true; break; }
    if (!haveFputs) module.declares.push_back(fputsDecl);
    panicUsesStderr_ = true;

    std::string strm = freshTemp();
    emit("%" + strm + " = call ptr @gg_stderr()");
    std::string ptr = freshTemp();
    emit("%" + ptr + " = getelementptr inbounds [" + std::to_string(byteLen)
         + " x i8], ptr " + g + ", i32 0, i32 0");
    std::string ret = freshTemp();
    emit("%" + ret + " = call i32 @fputs(ptr %" + ptr + ", ptr %" + strm + ")");
}

// Emit `@gg_stdout`/`@gg_stderr` definitions for whichever are referenced (their `declare` present,
// or the panic path forced stderr). One target switch: Windows/UCRT gets `__acrt_iob_func(fd)`;
// glibc/musl load the external `@stdout`/`@stderr` FILE* globals. Removing the matching `declare`
// (from the stdlib `extern`) avoids an LLVM declare+define redefinition error.
void CodeGen::emitStdioHelpers() {
    auto wanted = [&](const std::string& name) {
        std::string decl = "declare ptr @" + name + "()";
        for (auto it = module.declares.begin(); it != module.declares.end(); ++it)
            if (*it == decl) { module.declares.erase(it); return true; }
        return false;
    };
    bool wantOut = wanted("gg_stdout");
    bool wantErr = wanted("gg_stderr") || panicUsesStderr_;
    if (!wantOut && !wantErr) return;

    if (targetWindows_) {
        const std::string iob = "declare ptr @__acrt_iob_func(i32)";
        bool have = false;
        for (const auto& d : module.declares) if (d == iob) { have = true; break; }
        if (!have) module.declares.push_back(iob);
        if (wantOut) module.runtime.push_back(
            "define ptr @gg_stdout() {\nentry:\n  %h = call ptr @__acrt_iob_func(i32 1)\n  ret ptr %h\n}\n");
        if (wantErr) module.runtime.push_back(
            "define ptr @gg_stderr() {\nentry:\n  %h = call ptr @__acrt_iob_func(i32 2)\n  ret ptr %h\n}\n");
    } else {
        // glibc/musl: `stdout`/`stderr` are external globals holding the FILE* — load through them.
        if (wantOut) {
            module.globals.push_back("@stdout = external global ptr");
            module.runtime.push_back(
                "define ptr @gg_stdout() {\nentry:\n  %h = load ptr, ptr @stdout\n  ret ptr %h\n}\n");
        }
        if (wantErr) {
            module.globals.push_back("@stderr = external global ptr");
            module.runtime.push_back(
                "define ptr @gg_stderr() {\nentry:\n  %h = load ptr, ptr @stderr\n  ret ptr %h\n}\n");
        }
    }
}

void CodeGen::emitBoundsCheck(const std::string& indexValue, size_t arraySize) {
    emitBoundsCheckValue(indexValue, std::to_string(arraySize));
}

void CodeGen::emitBoundsCheckValue(const std::string& indexValue, const std::string& lengthValue) {
    std::string okLabel  = freshLabel("bounds.ok");
    std::string oobLabel = freshLabel("bounds.oob");

    std::string cmp = freshTemp();
    // icmp ult catches negative indices too: negative i64 values are huge unsigned numbers
    emit("%" + cmp + " = icmp ult i64 " + indexValue + ", " + lengthValue);
    emitCondBr("%" + cmp, okLabel, oobLabel);

    switchBlock(oobLabel);
    emitPanicMessage("index out of bounds");
    emit("call void @abort()");
    emit("unreachable");
    currentBasicBlock->terminated = true;

    switchBlock(okLabel);
}

// ---- Overflow-check helpers (gated by overflowChecks_) ----

// Branch on an i1 "overflow / out-of-range happened" condition to an abort()+unreachable block;
// execution continues in a fresh block when the condition is false. Mirrors emitBoundsCheck.
void CodeGen::emitOverflowTrap(const std::string& badCond, const std::string& what) {
    ensureAbortDeclared();
    std::string okLabel  = freshLabel("ovf.ok");
    std::string badLabel = freshLabel("ovf.bad");
    emitCondBr(badCond, badLabel, okLabel);

    switchBlock(badLabel);
    emitPanicMessage(what);
    emit("call void @abort()");
    emit("unreachable");
    currentBasicBlock->terminated = true;

    switchBlock(okLabel);
}

// Integer +/-/* via the LLVM checked-arithmetic intrinsic, trapping on overflow. The intrinsic
// returns { iN result, i1 overflow }; the ALU produces the overflow bit for free, so the only added
// cost is the (well-predicted, not-taken) branch. Signed vs. unsigned picks the s*/u* variant.
std::string CodeGen::emitCheckedArith(TokenType op, const Type& type,
                                      const std::string& lhs, const std::string& rhs) {
    const char* name = nullptr;
    bool uns = isUnsignedInt(type.kind);
    switch (op) {
        case TokenType::PLUS:  name = uns ? "uadd" : "sadd"; break;
        case TokenType::MINUS: name = uns ? "usub" : "ssub"; break;
        case TokenType::STAR:  name = uns ? "umul" : "smul"; break;
        default: return "";   // caller guarantees +/-/*
    }
    std::string ir  = irTypeName(type);                        // iN
    std::string fn  = std::string("llvm.") + name + ".with.overflow." + ir;
    std::string decl = "declare {" + ir + ", i1} @" + fn + "(" + ir + ", " + ir + ")";
    bool have = false;
    for (const auto& d : module.declares) if (d == decl) { have = true; break; }
    if (!have) module.declares.push_back(decl);

    std::string agg = freshTemp();
    emit("%" + agg + " = call {" + ir + ", i1} @" + fn + "(" + ir + " " + lhs + ", " + ir + " " + rhs + ")");
    std::string res = freshTemp();
    emit("%" + res + " = extractvalue {" + ir + ", i1} %" + agg + ", 0");
    std::string ovf = freshTemp();
    emit("%" + ovf + " = extractvalue {" + ir + ", i1} %" + agg + ", 1");
    emitOverflowTrap("%" + ovf);
    return "%" + res;
}

// ============================================================
// Reference-counting runtime (emitted once, when `new` is used)
// ============================================================

void CodeGen::emitRefcountRuntime() {
    // Ensure malloc/free are declared, deduplicating against extern-imported decls.
    const std::string mallocDecl = "declare ptr @malloc(i64)";
    const std::string freeDecl   = "declare void @free(ptr)";
    bool haveMalloc = false, haveFree = false;
    for (const auto& d : module.declares) {
        if (d == mallocDecl) haveMalloc = true;
        if (d == freeDecl)   haveFree = true;
    }
    if (!haveMalloc) module.declares.push_back(mallocDecl);
    if (!haveFree)   module.declares.push_back(freeDecl);

    // Intrusive header layout: [ i64 refcount ][ object body ].
    // gg_alloc returns a pointer to the body; the count lives at body-8.
    module.runtime.push_back(
        "define ptr @gg_alloc(i64 %size) {\n"
        "entry:\n"
        "  %total = add i64 %size, 8\n"
        "  %raw = call ptr @malloc(i64 %total)\n"
        "  store i64 1, ptr %raw\n"
        "  %body = getelementptr i8, ptr %raw, i64 8\n"
        "  ret ptr %body\n"
        "}\n");

    // retain/release are null-safe so uninitialised/null reference slots are harmless.
    module.runtime.push_back(
        "define void @gg_retain(ptr %obj) {\n"
        "entry:\n"
        "  %isnull = icmp eq ptr %obj, null\n"
        "  br i1 %isnull, label %done, label %inc\n"
        "inc:\n"
        "  %hdr = getelementptr i8, ptr %obj, i64 -8\n"
        "  %cnt = load i64, ptr %hdr\n"
        "  %n = add i64 %cnt, 1\n"
        "  store i64 %n, ptr %hdr\n"
        "  br label %done\n"
        "done:\n"
        "  ret void\n"
        "}\n");

    module.runtime.push_back(
        "define void @gg_release(ptr %obj, ptr %dtor) {\n"
        "entry:\n"
        "  %isnull = icmp eq ptr %obj, null\n"
        "  br i1 %isnull, label %done, label %dec\n"
        "dec:\n"
        "  %hdr = getelementptr i8, ptr %obj, i64 -8\n"
        "  %cnt = load i64, ptr %hdr\n"
        "  %n = sub i64 %cnt, 1\n"
        "  store i64 %n, ptr %hdr\n"
        "  %zero = icmp eq i64 %n, 0\n"
        "  br i1 %zero, label %dealloc, label %done\n"
        "dealloc:\n"
        "  %hasdtor = icmp ne ptr %dtor, null\n"
        "  br i1 %hasdtor, label %calldtor, label %freeit\n"
        "calldtor:\n"
        "  call void %dtor(ptr %obj)\n"
        "  br label %freeit\n"
        "freeit:\n"
        "  call void @free(ptr %hdr)\n"
        "  br label %done\n"
        "done:\n"
        "  ret void\n"
        "}\n");

    // ---- Atomic refcount runtime for Shared<T> ----
    // Same intrusive header, but the count is mutated with atomic RMW ops so the count is correct
    // when a Shared<T> is owned from more than one thread. Retain is a relaxed (monotonic) increment;
    // release is a `release`-ordered decrement with an `acquire` fence on the count-reaches-0 path
    // before the dtor/free (the standard Arc pattern — see docs/concurrency.md §7). The BODY is not
    // synchronised — atomic counts protect the count, not the object's mutable fields (that's the
    // Mutex/RwLock exclusion axis, Phase 2). Only emitted when a Shared<T> is actually lowered.
    if (sharedUsed_) {
        module.runtime.push_back(
            "define void @gg_retain_shared(ptr %obj) {\n"
            "entry:\n"
            "  %isnull = icmp eq ptr %obj, null\n"
            "  br i1 %isnull, label %done, label %inc\n"
            "inc:\n"
            "  %hdr = getelementptr i8, ptr %obj, i64 -8\n"
            "  %old = atomicrmw add ptr %hdr, i64 1 monotonic\n"
            "  br label %done\n"
            "done:\n"
            "  ret void\n"
            "}\n");

        module.runtime.push_back(
            "define void @gg_release_shared(ptr %obj, ptr %dtor) {\n"
            "entry:\n"
            "  %isnull = icmp eq ptr %obj, null\n"
            "  br i1 %isnull, label %done, label %dec\n"
            "dec:\n"
            "  %hdr = getelementptr i8, ptr %obj, i64 -8\n"
            "  %old = atomicrmw sub ptr %hdr, i64 1 release\n"
            "  %zero = icmp eq i64 %old, 1\n"       // old==1 ⇒ new==0 ⇒ last reference
            "  br i1 %zero, label %dealloc, label %done\n"
            "dealloc:\n"
            "  fence acquire\n"                      // synchronise with other threads' releases
            "  %hasdtor = icmp ne ptr %dtor, null\n"
            "  br i1 %hasdtor, label %calldtor, label %freeit\n"
            "calldtor:\n"
            "  call void %dtor(ptr %obj)\n"
            "  br label %freeit\n"
            "freeit:\n"
            "  call void @free(ptr %hdr)\n"
            "  br label %done\n"
            "done:\n"
            "  ret void\n"
            "}\n");
    }
}

// ============================================================
// OS-thread runtime (emitted once, when a Thread is lowered)
// ============================================================
//
// Three platform-neutral helpers the Thread lowering calls:
//   @gg_thread_create(ptr %entry, ptr %arg) -> ptr    — start a thread running %entry(%arg), return a handle
//   @gg_thread_join(ptr %handle)                      — block until that thread finishes
//   @gg_thread_dispose(ptr %handle)                   — release the OS handle (called by `~Thread`)
// The thread entry is the per-closure trampoline `@__thread_entry$Class` (C ABI). Its signature is
// platform-specific: Windows CreateThread wants `DWORD (*)(LPVOID)` = i32(ptr); pthread wants
// `void* (*)(void*)` = ptr(ptr) — the trampoline is emitted to match (see CodeGen_Classes.cpp).
//
// Handle lifetime: on Windows a thread HANDLE from CreateThread must be CloseHandle'd exactly once,
// and WaitForSingleObject (join) does NOT close it — so `gg_thread_dispose` = CloseHandle, run from
// `~Thread` at scope exit. This is correct whether or not the thread was joined first (closing a
// running thread's handle just drops our reference; the OS reclaims the thread when it exits). On
// POSIX, pthread_join itself frees the thread's resources, so the common create+join path already
// leaks nothing and `gg_thread_dispose` is a no-op; a POSIX thread that is NEVER joined leaks until
// process exit (a documented Phase-1 limit — a real `detach()` is Phase-2, since detach-after-join
// is UB and telling them apart needs per-Thread join-state we don't yet track).
void CodeGen::emitThreadRuntime() {
    if (!threadsUsed_) return;
    // Remove the stdlib `extern gg_thread_create/join/dispose` declares — we DEFINE them here, and
    // LLVM forbids a declare + define of the same symbol (mirrors emitStdioHelpers' gg_stdout).
    for (auto it = module.declares.begin(); it != module.declares.end(); ) {
        if (it->find("@gg_thread_create(") != std::string::npos
            || it->find("@gg_thread_join(") != std::string::npos
            || it->find("@gg_thread_dispose(") != std::string::npos)
            it = module.declares.erase(it);
        else ++it;
    }
    if (targetWindows_) {
        // kernel32: HANDLE CreateThread(NULL,0,start,arg,0,NULL); DWORD WaitForSingleObject(h,ms);
        //           BOOL CloseHandle(h).
        module.declares.push_back("declare ptr @CreateThread(ptr, i64, ptr, ptr, i32, ptr)");
        module.declares.push_back("declare i32 @WaitForSingleObject(ptr, i32)");
        module.declares.push_back("declare i32 @CloseHandle(ptr)");
        module.runtime.push_back(
            "define ptr @gg_thread_create(ptr %fn, ptr %arg) {\n"
            "entry:\n"
            "  %h = call ptr @CreateThread(ptr null, i64 0, ptr %fn, ptr %arg, i32 0, ptr null)\n"
            "  ret ptr %h\n"
            "}\n");
        module.runtime.push_back(
            "define void @gg_thread_join(ptr %h) {\n"
            "entry:\n"
            "  %w = call i32 @WaitForSingleObject(ptr %h, i32 -1)\n"   // -1 = INFINITE
            "  ret void\n"
            "}\n");
        module.runtime.push_back(
            "define void @gg_thread_dispose(ptr %h) {\n"
            "entry:\n"
            "  %c = call i32 @CloseHandle(ptr %h)\n"
            "  ret void\n"
            "}\n");
    } else {
        // pthread: int pthread_create(pthread_t*, attr, start, arg); int pthread_join(pthread_t, void**).
        // pthread_t is pointer-sized; we store it in the ptr handle slot (int/ptr round-trip).
        module.declares.push_back("declare i32 @pthread_create(ptr, ptr, ptr, ptr)");
        module.declares.push_back("declare i32 @pthread_join(i64, ptr)");
        module.runtime.push_back(
            "define ptr @gg_thread_create(ptr %fn, ptr %arg) {\n"
            "entry:\n"
            "  %tid = alloca i64\n"
            "  %rc = call i32 @pthread_create(ptr %tid, ptr null, ptr %fn, ptr %arg)\n"
            "  %t = load i64, ptr %tid\n"
            "  %h = inttoptr i64 %t to ptr\n"
            "  ret ptr %h\n"
            "}\n");
        module.runtime.push_back(
            "define void @gg_thread_join(ptr %h) {\n"
            "entry:\n"
            "  %t = ptrtoint ptr %h to i64\n"
            "  %r = call i32 @pthread_join(i64 %t, ptr null)\n"
            "  ret void\n"
            "}\n");
        // No-op: pthread_join already frees the thread's resources, and pthread_detach after a join
        // is UB — so a POSIX Thread that is never joined leaks until process exit (Phase-1 limit).
        module.runtime.push_back(
            "define void @gg_thread_dispose(ptr %h) {\n"
            "entry:\n"
            "  ret void\n"
            "}\n");
    }
}

// ============================================================
// OS mutex runtime — the stdlib Mutex<T> lock primitives
// ============================================================
//
// Four platform-neutral helpers the stdlib `Mutex<T>` (std.sync) ctor/dtor/`.with` lowering call:
//   @gg_mutex_create() -> ptr        — allocate + init an OS mutex, return an opaque handle
//   @gg_mutex_destroy(ptr handle)    — destroy + free it
//   @gg_mutex_lock(ptr handle)       — acquire (exclusive; blocks)
//   @gg_mutex_unlock(ptr handle)     — release
// Gated by declare-presence (like emitStdioHelpers' gg_stdout): if the program imported Mutex, its
// `extern gg_mutex_*` declares are present — we remove them and emit the definitions (LLVM forbids a
// declare + define of the same symbol). Windows uses SRWLOCK held exclusively (in kernel32, already
// linked via -lkernel32); POSIX uses pthread_mutex. Non-reentrant on both.
void CodeGen::emitMutexRuntime() {
    bool useMutex = false, useRw = false;
    for (auto it = module.declares.begin(); it != module.declares.end(); ) {
        if (it->find("@gg_mutex_create(") != std::string::npos
            || it->find("@gg_mutex_destroy(") != std::string::npos
            || it->find("@gg_mutex_lock(") != std::string::npos
            || it->find("@gg_mutex_unlock(") != std::string::npos) {
            useMutex = true;
            it = module.declares.erase(it);
        } else if (it->find("@gg_rwlock_") != std::string::npos) {
            useRw = true;
            it = module.declares.erase(it);
        } else ++it;
    }
    if (useRw) emitRwLockRuntime();
    if (!useMutex) return;

    // malloc/free hold the lock storage; dedup against extern-imported / refcount-runtime decls.
    const std::string mallocDecl = "declare ptr @malloc(i64)";
    const std::string freeDecl   = "declare void @free(ptr)";
    bool haveMalloc = false, haveFree = false;
    for (const auto& d : module.declares) {
        if (d == mallocDecl) haveMalloc = true;
        if (d == freeDecl)   haveFree = true;
    }
    if (!haveMalloc) module.declares.push_back(mallocDecl);
    if (!haveFree)   module.declares.push_back(freeDecl);

    if (targetWindows_) {
        // SRWLOCK is one pointer-sized word; needs no explicit destroy (just free the storage).
        module.declares.push_back("declare void @InitializeSRWLock(ptr)");
        module.declares.push_back("declare void @AcquireSRWLockExclusive(ptr)");
        module.declares.push_back("declare void @ReleaseSRWLockExclusive(ptr)");
        module.runtime.push_back(
            "define ptr @gg_mutex_create() {\n"
            "entry:\n"
            "  %h = call ptr @malloc(i64 8)\n"
            "  call void @InitializeSRWLock(ptr %h)\n"
            "  ret ptr %h\n"
            "}\n");
        module.runtime.push_back(
            "define void @gg_mutex_destroy(ptr %h) {\n"
            "entry:\n  call void @free(ptr %h)\n  ret void\n}\n");
        module.runtime.push_back(
            "define void @gg_mutex_lock(ptr %h) {\n"
            "entry:\n  call void @AcquireSRWLockExclusive(ptr %h)\n  ret void\n}\n");
        module.runtime.push_back(
            "define void @gg_mutex_unlock(ptr %h) {\n"
            "entry:\n  call void @ReleaseSRWLockExclusive(ptr %h)\n  ret void\n}\n");
    } else {
        // pthread_mutex_t is opaque and platform-sized (≤ 40 bytes on glibc/musl); over-allocate 64.
        module.declares.push_back("declare i32 @pthread_mutex_init(ptr, ptr)");
        module.declares.push_back("declare i32 @pthread_mutex_destroy(ptr)");
        module.declares.push_back("declare i32 @pthread_mutex_lock(ptr)");
        module.declares.push_back("declare i32 @pthread_mutex_unlock(ptr)");
        module.runtime.push_back(
            "define ptr @gg_mutex_create() {\n"
            "entry:\n"
            "  %h = call ptr @malloc(i64 64)\n"
            "  %r = call i32 @pthread_mutex_init(ptr %h, ptr null)\n"
            "  ret ptr %h\n"
            "}\n");
        module.runtime.push_back(
            "define void @gg_mutex_destroy(ptr %h) {\n"
            "entry:\n"
            "  %r = call i32 @pthread_mutex_destroy(ptr %h)\n"
            "  call void @free(ptr %h)\n  ret void\n}\n");
        module.runtime.push_back(
            "define void @gg_mutex_lock(ptr %h) {\n"
            "entry:\n  %r = call i32 @pthread_mutex_lock(ptr %h)\n  ret void\n}\n");
        module.runtime.push_back(
            "define void @gg_mutex_unlock(ptr %h) {\n"
            "entry:\n  %r = call i32 @pthread_mutex_unlock(ptr %h)\n  ret void\n}\n");
    }
}

// OS reader/writer-lock runtime for the stdlib RwLock<T> (std.sync). Six helpers
// (create/destroy/rdlock/rdunlock/wrlock/wrunlock). Windows uses SRWLOCK, which is a native
// reader/writer lock but needs a MODE-MATCHED release (Shared vs Exclusive) — hence distinct
// rd/wr unlocks; POSIX uses pthread_rwlock, whose single unlock serves both modes.
void CodeGen::emitRwLockRuntime() {
    const std::string mallocDecl = "declare ptr @malloc(i64)";
    const std::string freeDecl   = "declare void @free(ptr)";
    bool haveMalloc = false, haveFree = false;
    for (const auto& d : module.declares) {
        if (d == mallocDecl) haveMalloc = true;
        if (d == freeDecl)   haveFree = true;
    }
    if (!haveMalloc) module.declares.push_back(mallocDecl);
    if (!haveFree)   module.declares.push_back(freeDecl);

    if (targetWindows_) {
        module.declares.push_back("declare void @InitializeSRWLock(ptr)");
        module.declares.push_back("declare void @AcquireSRWLockShared(ptr)");
        module.declares.push_back("declare void @ReleaseSRWLockShared(ptr)");
        module.declares.push_back("declare void @AcquireSRWLockExclusive(ptr)");
        module.declares.push_back("declare void @ReleaseSRWLockExclusive(ptr)");
        module.runtime.push_back(
            "define ptr @gg_rwlock_create() {\n"
            "entry:\n"
            "  %h = call ptr @malloc(i64 8)\n"
            "  call void @InitializeSRWLock(ptr %h)\n"
            "  ret ptr %h\n"
            "}\n");
        module.runtime.push_back(
            "define void @gg_rwlock_destroy(ptr %h) {\nentry:\n  call void @free(ptr %h)\n  ret void\n}\n");
        module.runtime.push_back(
            "define void @gg_rwlock_rdlock(ptr %h) {\nentry:\n  call void @AcquireSRWLockShared(ptr %h)\n  ret void\n}\n");
        module.runtime.push_back(
            "define void @gg_rwlock_rdunlock(ptr %h) {\nentry:\n  call void @ReleaseSRWLockShared(ptr %h)\n  ret void\n}\n");
        module.runtime.push_back(
            "define void @gg_rwlock_wrlock(ptr %h) {\nentry:\n  call void @AcquireSRWLockExclusive(ptr %h)\n  ret void\n}\n");
        module.runtime.push_back(
            "define void @gg_rwlock_wrunlock(ptr %h) {\nentry:\n  call void @ReleaseSRWLockExclusive(ptr %h)\n  ret void\n}\n");
    } else {
        module.declares.push_back("declare i32 @pthread_rwlock_init(ptr, ptr)");
        module.declares.push_back("declare i32 @pthread_rwlock_destroy(ptr)");
        module.declares.push_back("declare i32 @pthread_rwlock_rdlock(ptr)");
        module.declares.push_back("declare i32 @pthread_rwlock_wrlock(ptr)");
        module.declares.push_back("declare i32 @pthread_rwlock_unlock(ptr)");
        module.runtime.push_back(
            "define ptr @gg_rwlock_create() {\n"
            "entry:\n"
            "  %h = call ptr @malloc(i64 64)\n"
            "  %r = call i32 @pthread_rwlock_init(ptr %h, ptr null)\n"
            "  ret ptr %h\n"
            "}\n");
        module.runtime.push_back(
            "define void @gg_rwlock_destroy(ptr %h) {\nentry:\n  %r = call i32 @pthread_rwlock_destroy(ptr %h)\n  call void @free(ptr %h)\n  ret void\n}\n");
        module.runtime.push_back(
            "define void @gg_rwlock_rdlock(ptr %h) {\nentry:\n  %r = call i32 @pthread_rwlock_rdlock(ptr %h)\n  ret void\n}\n");
        module.runtime.push_back(
            "define void @gg_rwlock_rdunlock(ptr %h) {\nentry:\n  %r = call i32 @pthread_rwlock_unlock(ptr %h)\n  ret void\n}\n");
        module.runtime.push_back(
            "define void @gg_rwlock_wrlock(ptr %h) {\nentry:\n  %r = call i32 @pthread_rwlock_wrlock(ptr %h)\n  ret void\n}\n");
        module.runtime.push_back(
            "define void @gg_rwlock_wrunlock(ptr %h) {\nentry:\n  %r = call i32 @pthread_rwlock_unlock(ptr %h)\n  ret void\n}\n");
    }
}

// The per-closure C-ABI trampoline the OS thread runs: it invokes the closure's `call()` then
// releases the (heap, sole-owned) closure. Return type matches the platform's thread-entry ABI
// (Windows `DWORD(*)(LPVOID)` = i32; pthread `void*(*)(void*)` = ptr). Deduped by closure class.
void CodeGen::emitThreadTrampoline(const std::string& cn) {
    if (!threadTrampolines_.insert(cn).second) return;   // already emitted for this closure class
    threadsUsed_ = true;
    usesRefcount_ = true;
    auto cgIt = cgClasses_.find(cn);
    std::string dtorArg = (cgIt != cgClasses_.end() && cgIt->second.needsDtor)
                        ? ("@" + cn + "_dtor") : "null";
    std::string retTy  = targetWindows_ ? "i32"       : "ptr";
    std::string retVal = targetWindows_ ? "ret i32 0" : "ret ptr null";
    module.runtime.push_back(
        "define " + retTy + " @__thread_entry$" + cn + "(ptr %arg) {\n"
        "entry:\n"
        "  call void @" + cn + "_call(ptr %arg)\n"          // run the closure's Call impl
        "  call void @gg_release(ptr %arg, ptr " + dtorArg + ")\n"   // free the heap closure (sole owner)
        "  " + retVal + "\n"
        "}\n");
}

// ============================================================
// Low-level emit helpers
// ============================================================

void CodeGen::emit(const std::string& instruction) {
    if (currentBasicBlock && !currentBasicBlock->terminated)
        currentBasicBlock->instructions.push_back("  " + instruction + currentDbgLoc_);
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

Type CodeGen::resolveParamType(const ParamDecl& param) const {
    // Nullable `T?` (synthesized IDENTIFIER ending in '?'): strip it and resolve the inner type.
    // Nullability does not change the IR for reference-like types (all `ptr`), so this only needs
    // to reach the inner type; the flag rides along for exprType consistency.
    if (param.typeName.type == TokenType::IDENTIFIER && !param.typeName.lexeme.empty()
        && param.typeName.lexeme.back() == '?') {
        std::string base = param.typeName.lexeme.substr(0, param.typeName.lexeme.size()-1);
        TypeKind prim = typeKindFromName(base);
        if (prim != TypeKind::Error) return makeNullable(Type{prim});   // nullable primitive
        Token bt{TokenType::IDENTIFIER, base, param.typeName.line};
        return makeNullable(resolveReturnType(bt));                     // ref/enum inner
    }
    // Parser-synthesized types: "<Class>&" (Reference) and "ptr<Elem>" (TypedPtr).
    Type synth = decodeSynthesizedType(param.typeName);
    if (!isError(synth)) {
        if (synth.className == "Self") synth.className = currentClassName_;   // `Self&` in an impl
        return synth;
    }
    if (param.typeName.type == TokenType::SELF)
        return makeObjectType(currentClassName_);
    if (param.typeName.type == TokenType::IDENTIFIER && cgClasses_.count(param.typeName.lexeme))
        // A bare value-object parameter (`fn f(Obj o)`) is sugar for a non-owning class borrow:
        // GG objects are address-backed (there is no by-value struct-param ABI), so `Obj o` lowers to
        // a `ptr` borrow exactly like `Obj*`. The caller's object value / owning ref coerces to the
        // borrow (Silent), member/method access works through it, and `field = o` into a value field
        // clones. Matches the semantic side (resolveTypeToken callers), which also maps it to a borrow.
        return makeBorrowType(param.typeName.lexeme);
    return typeFromToken(param.typeName.type);
}

Type CodeGen::resolveReturnType(const Token& typeToken) const {
    // Nullable `T?`: strip the '?' and resolve the inner type (IR is unchanged — all `ptr`).
    if (typeToken.type == TokenType::IDENTIFIER && !typeToken.lexeme.empty()
        && typeToken.lexeme.back() == '?') {
        std::string base = typeToken.lexeme.substr(0, typeToken.lexeme.size()-1);
        TypeKind prim = typeKindFromName(base);
        if (prim != TypeKind::Error) return makeNullable(Type{prim});   // nullable primitive
        Token bt{TokenType::IDENTIFIER, base, typeToken.line};
        return makeNullable(resolveReturnType(bt));
    }
    // "Class&" → reference return (lowers to ptr); "ptr<Elem>" → typed pointer.
    // Bare class returns (by value) are unsupported and fall through to typeFromToken.
    Type synth = decodeSynthesizedType(typeToken);
    if (!isError(synth)) {
        if (synth.className == "Self") synth.className = currentClassName_;   // `Self&` in an impl
        return synth;
    }
    if (typeToken.type == TokenType::SELF)
        return makeObjectType(currentClassName_);
    // Enum return-by-value: an enum value is a pointer to a singleton, so a bare
    // enum return type lowers to `ptr` (unlike bare class-by-value, which is rejected).
    if (typeToken.type == TokenType::IDENTIFIER && cgEnumNames_.count(typeToken.lexeme))
        return makeEnumType(typeToken.lexeme);
    return typeFromToken(typeToken.type);
}

// ============================================================
// Reference-return / temporary lifetime helpers
// ============================================================

bool CodeGen::producesPlusOne(const Expr& e) const {
    if (!e.node) return false;
    if (std::holds_alternative<NewExpr>(*e.node)) return true;   // `new` → +1
    if ((std::holds_alternative<CallExpr>(*e.node)
         || std::holds_alternative<MethodCallExpr>(*e.node)
         || std::holds_alternative<SwitchExpr>(*e.node))
        && exprType(e).kind == TypeKind::Reference)
        return true;                          // reference-returning call / switch expr → +1
    return false;
}

void CodeGen::claimTemp(const std::string& ptr) {
    // Remove the claimed +1 temporary from the pending-release list so it is NOT released at the
    // full-expression boundary — ownership passes to the consumer (a binding / return / field).
    // It is usually the most recently registered temp, but a nested `new` or ref-returning call
    // in the constructor arguments pushes further temps *after* it (e.g. `new Vec(new Node())`
    // leaves pending = [Vec, Node] with Node at back), so search rather than only checking back().
    // Any such inner temps stay pending and are correctly released (their +1 was consumed by the
    // outer object's field retain).
    for (size_t i = pendingTemps_.size(); i-- > 0; ) {
        if (pendingTemps_[i].ptr == ptr) {
            pendingTemps_.erase(pendingTemps_.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

void CodeGen::flushTempReleases() {
    for (const auto& t : pendingTemps_) {
        auto it = cgClasses_.find(t.className);
        std::string dtorArg = (it != cgClasses_.end() && it->second.needsDtor)
                            ? ("@" + t.className + "_dtor") : "null";
        emit(std::string("call void @") + releaseFn(t.shared) + "(ptr " + t.ptr + ", ptr " + dtorArg + ")");
    }
    pendingTemps_.clear();
}

std::string CodeGen::emitCast(const std::string& value, const Type& from, const Type& to,
                              bool checked) {
    if (from == to) return value;
    if (isError(from) || isError(to)) return value;

    // A value object borrowed as a reference of the same class (argument position): objects
    // are manipulated by address, so `value` is already a `ptr` to the body — no instruction.
    if (from.kind == TypeKind::Object && to.kind == TypeKind::Reference
        && from.className == to.className)
        return value;

    // A `ref <primitive>` used where its primitive value is expected: load through the borrow
    // (lvalue-to-rvalue). `value` is the referent pointer.
    if (isPrimitiveBorrow(from) && !to.borrow && from.elementKind == to.kind)
        return emitLoad(irTypeName(to), value);

    // ---- Nullable-primitive tag+payload (`{ i1, iN }`) ----
    auto isOptPrim = [](const Type& t) {
        return t.isNullable && (isNumeric(t.kind) || t.kind == TypeKind::Bool || t.kind == TypeKind::Char);
    };
    // `null` → optional primitive: the empty value { present=false, 0 }.
    if (isOptPrim(to) && from.kind == TypeKind::Null)
        return "zeroinitializer";
    // Wrap `iN → iN?`: build { present=true, value } (coercing the payload's numeric type first).
    if (isOptPrim(to) && !from.isNullable) {
        std::string structIr  = irTypeName(to);
        std::string payloadIr = irTypeName(Type{to.kind});
        std::string payload   = emitCast(value, from, Type{to.kind});   // numeric coercion to payload
        std::string t0 = freshTemp(), t1 = freshTemp();
        emit("%" + t0 + " = insertvalue " + structIr + " poison, i1 true, 0");
        emit("%" + t1 + " = insertvalue " + structIr + " %" + t0 + ", " + payloadIr + " " + payload + ", 1");
        return "%" + t1;
    }
    // Unwrap `iN? → iN` (used after a null/tag check by `!!` and smart-casts): extract the payload.
    if (isOptPrim(from) && !to.isNullable && from.kind == to.kind) {
        std::string t = freshTemp();
        emit("%" + t + " = extractvalue " + irTypeName(from) + " " + value + ", 1");
        return "%" + t;
    }
    // Nullable → nullable with a different payload type/width (`i64? → i32?`): unpack the tag and
    // payload, convert the payload through the scalar rules, then re-wrap under the SAME present-tag.
    // A plain trunc/zext/sext on the whole `{ i1, iN }` struct is not a valid LLVM cast.
    if (isOptPrim(from) && isOptPrim(to) && from.kind != to.kind) {
        std::string fromIr = irTypeName(from);
        std::string toIr   = irTypeName(to);
        std::string tag = freshTemp(), pay = freshTemp();
        emit("%" + tag + " = extractvalue " + fromIr + " " + value + ", 0");
        emit("%" + pay + " = extractvalue " + fromIr + " " + value + ", 1");
        std::string newPay = emitCast("%" + pay, Type{from.kind}, Type{to.kind}, checked);
        std::string toPayIr = irTypeName(Type{to.kind});
        std::string t0 = freshTemp(), t1 = freshTemp();
        emit("%" + t0 + " = insertvalue " + toIr + " poison, i1 %" + tag + ", 0");
        emit("%" + t1 + " = insertvalue " + toIr + " %" + t0 + ", " + toPayIr + " " + newPay + ", 1");
        return "%" + t1;
    }

    // `str` → `ptr` decay: extract the data pointer (field 0) from the { ptr, i64 } view.
    if (from.kind == TypeKind::Str && to.kind == TypeKind::Ptr) {
        std::string t = freshTemp();
        emit("%" + t + " = extractvalue { ptr, i64 } " + value + ", 0");
        return "%" + t;
    }

    // If both types map to the same LLVM IR type (e.g. i32 ↔ u32,
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

        // Overflow-check: verify the value fits in `to` before a lossy narrowing / sign change.
        // Widening (toBits > fromBits) of a matching signedness can never lose data → no check.
        if (overflowChecks_ && checked) {
            if (toBits < fromBits) {
                // Narrowing: truncate, extend back (per `to`'s signedness), compare to the original.
                // Any difference means the value didn't fit the narrower type.
                std::string tv = freshTemp();
                emit("%" + tv + " = trunc " + fromIrType + " " + value + " to " + toIrType);
                std::string back = freshTemp();
                const char* ext = isUnsignedInt(to.kind) ? "zext" : "sext";
                emit("%" + back + " = " + std::string(ext) + " " + toIrType + " %" + tv + " to " + fromIrType);
                std::string bad = freshTemp();
                emit("%" + bad + " = icmp ne " + fromIrType + " " + value + ", %" + back);
                emitOverflowTrap("%" + bad, "value out of range in narrowing conversion");
                return "%" + tv;
            }
            if (toBits == fromBits && isSignedInt(from.kind) != isSignedInt(to.kind)) {
                // Same width, signedness flip: the sign bit set is exactly the out-of-range case
                // (a negative signed value, or an unsigned value above the signed max).
                std::string bad = freshTemp();
                emit("%" + bad + " = icmp slt " + fromIrType + " " + value + ", 0");
                emitOverflowTrap("%" + bad, "value out of range in narrowing conversion");
                return value;   // same IR bits — reinterpret only
            }
        }

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

std::string CodeGen::emitToBool(const std::string& value, const Type& valueType) {
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

std::string CodeGen::arithInstr(TokenType operatorType, const Type& type) {
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

std::string CodeGen::cmpInstr(TokenType operatorType, const Type& type) {
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
