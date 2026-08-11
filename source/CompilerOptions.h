//
// Compiler-wide configuration flags.
// Passed from main() through GG into the code-generation pass.
//

#ifndef GG_COMPILEROPTIONS_H
#define GG_COMPILEROPTIONS_H

#include <string>

struct CompilerOptions {
    bool boundsCheck = true;   // emit runtime array bounds checks; disabled by --no-bounds-check
    bool allowRawPtr = false;  // allow ptr / ptr<T> in non-extern contexts; enabled by --unsafe-ptr
    bool debugInfo   = false;  // emit LLVM/DWARF debug metadata (line + variable info); enabled by --debug / -g
    bool overflowChecks = false; // trap on integer overflow (+/-/*) and out-of-range narrowing
                                 // conversions; enabled by --overflow-checks (Rust-style, opt-in)
    std::string sourceFile;    // main source path, used as the DWARF DIFile when debugInfo is on
    // LLVM target triple emitted into the .ll and used by codegen to pick platform-specific runtime
    // (e.g. how stdout/stderr are obtained). Defaults to the HOST so a native build "just works";
    // override with --target=<triple> to cross-compile.
#ifdef _WIN32
    std::string targetTriple = "x86_64-w64-windows-gnu";
#else
    std::string targetTriple = "x86_64-pc-linux-gnu";
#endif
};

#endif //GG_COMPILEROPTIONS_H
