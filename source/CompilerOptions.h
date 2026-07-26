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
};

#endif //GG_COMPILEROPTIONS_H
