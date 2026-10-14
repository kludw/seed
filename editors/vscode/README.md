# Seed for VS Code

Syntax highlighting and basic editor settings for Seed (`.sd` files). It has no code of its own and no language server: no completion, go-to-definition or diagnostics.

It provides:

- a TextMate grammar (`syntaxes/seed.tmLanguage.json`): comments, keywords, built-in types, `true`/`false`/`null`, numbers (invalid forms such as `1e9` are marked), strings and byte literals with their escapes (unknown escapes are marked), function and method declarations, calls, type names, enum cases (`Shape.Circle`), `test "name"` blocks and operators, including the `?` of nullable types;
- language settings (`language-configuration.json`): `//` line comments (Seed has no block comments), bracket matching, auto-closing of brackets and quotes, and indentation that follows `seed fmt` (`case` and `default` at the level of their `switch`).

## Install

Copy or symlink this folder into your VS Code extensions directory, then run **Developer: Reload Window**. From the repository root:

```
ln -s "$PWD/editors/vscode" ~/.vscode/extensions/seed-lang.seed-lang-0.1.0
```

or, to copy it instead (rerun after updating the repository):

```
cp -R editors/vscode ~/.vscode/extensions/seed-lang.seed-lang-0.1.0
```

## Formatting

The extension does not format code. Run `seed fmt <file.sd>` yourself, from a VS Code task or from an extension that runs a command on save.
