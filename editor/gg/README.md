# GG syntax highlighting (TextMate grammar)

A TextMate grammar for `.gg` source, usable in **CLion** (and other JetBrains IDEs) and
**VS Code**. It highlights keywords, primitive types, `//` and `/* */` comments, string/char
literals, numeric literals, function/type declaration names, and the `->` / `::` operators.

## CLion / JetBrains IDEs

1. *Settings → Editor → TextMate Bundles → +*
2. Select this folder: `editor/gg`
3. Apply. `.gg` files now highlight automatically (JetBrains reads the VS Code-style
   `package.json` → `contributes.grammars`).

TextMate bundles give lexer-level highlighting only (no semantic analysis / error checking),
which is exactly what a non-native language needs.

## VS Code

Copy or symlink this folder into your extensions dir and reload:

- Windows: `%USERPROFILE%\.vscode\extensions\gg-language`
- macOS/Linux: `~/.vscode/extensions/gg-language`

## Files

- `package.json` — language + grammar registration (VS Code extension manifest; JetBrains reads it too)
- `language-configuration.json` — comment tokens, bracket pairs, auto-closing
- `syntaxes/gg.tmLanguage.json` — the grammar itself

The keyword and type lists mirror `source/lexer/Token.cpp`; update them together when the
lexer's keyword set changes.
