# GG Language — VS Code extension

Syntax highlighting and basic editing support for the **GG** programming language
(`.gg` files): keywords, primitive types, user types (classes / enums / traits),
functions, generics, strings, chars, numbers, comments, and operators — plus
bracket matching, auto-closing pairs, comment toggling, and folding.

This is a **TextMate grammar** (regex-based), so type-vs-variable classification
follows GG's naming convention: **Capitalized** identifiers are highlighted as
types; **lowercase** identifiers followed by `(` are highlighted as functions.
Precise semantic highlighting would require a language server (see below).

## Layout

```
editors/vscode/
├── package.json                 # extension manifest + .gg association
├── language-configuration.json  # brackets, comments, auto-close, folding
├── syntaxes/gg.tmLanguage.json  # the TextMate grammar
└── README.md
```

## Try it (no install)

1. Open the `editors/vscode/` folder in VS Code.
2. Press **F5** ("Run Extension") — this launches an *Extension Development Host*
   window with the extension loaded.
3. In that window, open any `.gg` file (e.g. from `e2e/` or `samples/`) and you'll
   see highlighting. Changes to the grammar reload on **Ctrl+R** in the host window.

Tip: to inspect which scope a token gets, run **Developer: Inspect Editor Tokens
and Scopes** from the Command Palette.

## Install locally

```powershell
npm install -g @vscode/vsce
cd editors/vscode
vsce package                 # produces gg-lang-0.1.0.vsix
code --install-extension gg-lang-0.1.0.vsix
```

Or symlink/copy the folder into your VS Code extensions dir:
`%USERPROFILE%\.vscode\extensions\gg-lang-0.1.0\` and reload VS Code.

## What's covered

| Element | Examples |
|---------|----------|
| Control keywords | `if else while for return break continue import` |
| Declaration keywords | `class enum trait impl extern` |
| Modifiers | `static mut private` |
| Operator keywords | `new as sizeof` |
| Primitive types | `i8..i64 u8..u64 f32 f64 bool char void ptr` |
| User types | Capitalized names + `Self` (classes, enums, traits) |
| Functions | `maxOf<T>(...)`, `obj.get()` |
| Literals | `"strings"` (with escapes), `'c'`, numbers, `true`/`false` |
| Comments | `// line` and `/* block */` |
| Operators | arithmetic, bitwise, comparison, `::`, `->`, `&` |

## Beyond highlighting (optional, future)

For semantic features — distinguishing type names from variables precisely,
go-to-definition, hover, inline errors — implement a **Language Server** (LSP)
that reuses the compiler's existing lexer / parser / semantic analyzer. The
grammar here is standalone and needs no build step.
