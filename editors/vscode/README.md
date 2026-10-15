# Seed for VS Code

Syntax highlighting, editor settings and a client for the Seed language server (`seed lsp`) for `.sd` files.

It provides:

- a TextMate grammar (`syntaxes/seed.tmLanguage.json`): comments, keywords, built-in types, `true`/`false`/`null`, numbers (invalid forms such as `1e9` are marked), strings and byte literals with their escapes (unknown escapes are marked), function and method declarations, calls, type names, enum cases (`Shape.Circle`), `test "name"` blocks and operators, including the `?` of nullable types;
- language settings (`language-configuration.json`): `//` line comments (Seed has no block comments), bracket matching, auto-closing of brackets and quotes, and indentation that follows `seed fmt` (`case` and `default` at the level of their `switch`);
- a language client (`extension.js`) that starts `seed lsp` and gives VS Code what the server offers: the compiler's errors as you type (syntax and type errors, and a warning where the file is not formatted), **Format Document** (`seed fmt`), **Go to Definition**, hover with the declaration and its doc comment, completion (after `pkg.`, `value.` and `Enum.`, and the names in scope), signature help, **Find All References**, highlights of the name under the cursor, **Rename Symbol**, and the outline (document symbols). vscode-languageclient turns these on from the server's capabilities, so the client has no code of its own for them. The server is described in [compiler/README.md](../../compiler/README.md), "The language server".

## Install

The client needs the npm package `vscode-languageclient` (the grammar and the settings work without it). From the repository root:

```
cd editors/vscode && npm install && cd ../..
```

`node_modules/` is not committed (`.gitignore`); `package-lock.json` pins the versions. There is nothing to compile: the client is plain JavaScript.

Then copy or symlink this folder into your VS Code extensions directory, and run **Developer: Reload Window**:

```
ln -s "$PWD/editors/vscode" ~/.vscode/extensions/seed-lang.seed-lang-0.2.0
```

or, to copy it instead (rerun after updating the repository):

```
cp -R editors/vscode ~/.vscode/extensions/seed-lang.seed-lang-0.2.0
```

To package it as a `.vsix` instead: `npx @vscode/vsce package` in this folder, then **Extensions: Install from VSIX...**.

## Settings

| Setting | Default | What it does |
|---|---|---|
| `seed.path` | `seed` | The seed binary that runs `seed lsp`. `seed` works after `make install`; otherwise give the absolute path of `build/seed2`. |
| `seed.languageServer.enabled` | `true` | Start the language server (takes effect after a reload). |
| `seed.trace.server` | `off` | `messages` or `verbose` log the protocol in the Output panel, under "Seed language server". |

## Formatting

**Format Document** runs the formatter of `seed fmt` on the editor's text; with `"editor.formatOnSave": true` in the settings for `[seed]`, files are formatted when they are saved. A file with syntax errors is left as it is. Without the language server, run `seed fmt <file.sd>` yourself.
