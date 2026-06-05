//
// Compiler-wide configuration flags.
// Passed from main() through GG into the code-generation pass.
//

#ifndef GG_COMPILEROPTIONS_H
#define GG_COMPILEROPTIONS_H

struct CompilerOptions {
    bool boundsCheck = true;   // emit runtime array bounds checks; disabled by --no-bounds-check
};

#endif //GG_COMPILEROPTIONS_H
