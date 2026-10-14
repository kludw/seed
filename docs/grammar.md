# Seed grammar

This is part of the language spec ([SPEC.md](../SPEC.md)). It uses EBNF: `{ x }` means zero or more, `[ x ]` means optional, `|` separates choices, and quoted text is a literal token.

## Lexical structure

- Source files are UTF-8 and use the `.sd` extension.
- Comments start with `//` and run to the end of the line. There are no block comments.
- Identifiers: an ASCII letter (`a`–`z`, `A`–`Z`) or `_`, followed by ASCII letters, digits or `_`. `_` alone is the discard name. Outside strings, byte literals and comments, a source file is ASCII: any other character is an error (reported once, as the whole UTF-8 character).
- Keywords:
  `package import pub fn type struct enum interface implements const if else for switch case default break continue return defer try null true false mut test assert select spawn group chan map`
- Built-in type names (`int float bool byte string error`) are not keywords but predeclared identifiers that refer to the built-in types. Like every name, they can't be declared again: a variable, parameter, field, function, type or constant can't be called `int` or `string`.
- Literals are described in [SPEC.md](../SPEC.md#literals). A number never starts with `0` followed by a digit: `017` and `00` are errors (only `0` itself, `0.5` and the like, and `0x...` start with `0`).
- Operators and punctuation:
  `+ - * / % & | ^ << >> == != < <= > >= && || ! = ++ -- ( ) [ ] { } , ; : . ?`

### Newlines

A newline ends a statement. As in Go, the lexer turns a newline into a statement terminator (written `NL` below) when the last token on the line is one of:

- an identifier or a literal (including `true`, `false`, `null`)
- `return`, `break`, `continue`
- `)`, `]`, `}`, `?`, `++`, `--`

Blank lines and comment-only lines produce nothing. Because of this rule, a line continues exactly when its last token is not one of these, for example an operator, `=`, `.`, a comma or an opening bracket. So an expression that continues on the next line must break after an operator or a comma, never before it:

```
int total = first +
    second

[]int xs = []int{
    1,
    2,
}
```

## Files

```
File        = "package" Ident NL { Import NL } { TopDecl NL } .
Import      = "import" String .
TopDecl     = [ "pub" ] ( FnDecl | TypeDecl | ConstDecl ) | TestDecl .
TestDecl    = "test" String Block .
```

## Declarations

```
FnDecl      = "fn" [ Receiver ] Ident "(" [ Params ] ")" [ Result ] [ Block ] .   // no Block: native
Receiver    = "(" Type Ident ")" [ "mut" ] .
Params      = Param { "," Param } .
Param       = Type Ident .
Result      = Type | "(" Type { "," Type } ")" .

TypeDecl    = "type" Ident ( StructType | EnumType | InterfaceType ) .
Implements  = "implements" TypeName { "," TypeName } .
StructType  = "struct" [ Implements ] "{" NL { Field NL } "}" .
Field       = [ "pub" ] Type Ident .
EnumType    = "enum" [ Implements ] "{" NL { Ident [ "(" Params ")" ] NL } "}" .   // no () for a case without data
InterfaceType = "interface" "{" NL { MethodSig NL } "}" .
MethodSig   = [ "mut" ] Ident "(" [ Params ] ")" [ Result ] .

ConstDecl   = "const" Type Ident "=" Expr .
```

Example of a method declaration: `fn (User u) mut tag(string t) { ... }`.

A function without a block is **native** (`pub fn read(string path) (string, error)`): its body is written in C. Only packages of the standard library may declare native functions; the compiler rejects a missing body anywhere else.

`pub` before a top-level declaration or a struct field makes it usable from other packages. A `TestDecl` is only allowed in a package's `tests/` directory.

The imported packages are written `import "path"`, and the code names a package by the last element of its path (`import "shop/money"` gives `money.Amount`). A `TypeName` with a dot (`money.Amount`) names a type of an imported package.

## Types

```
Type        = BaseType [ "?" ] .
BaseType    = TypeName
            | "[" "]" BaseType
            | "map" "[" Type "]" BaseType
            | "chan" BaseType
            | "fn" "(" [ Type { "," Type } ] ")" [ Result ]
            | "(" Type ")" .
TypeName    = Ident [ "." Ident ] .
```

`?` applies to the whole type on its left: `[]string?` is a nullable list of strings. A list of nullable strings needs parentheses: `[](string?)`, and so does a nullable function type: `(fn(int) int?)?`. `T??` and `error?` are not valid types, and neither are a nullable map value or channel element (`map[K](V?)`, `chan (T?)`).

## Statements

```
Block       = "{" NL { Stmt NL } "}" .
Stmt        = VarDecl | ConstDecl | Assign | IncDec | ExprStmt
            | If | For | Switch | Select | Return | Defer | Spawn
            | "break" | "continue" | Assert .

VarDecl     = DeclTarget { "," DeclTarget } "=" Expr .     // at least one typed target
DeclTarget  = Type Ident | LValue | "_" .
Assign      = LValue { "," LValue } "=" Expr .
LValue      = Ident { "." Ident | "[" Expr "]" } | "_" .
IncDec      = LValue ( "++" | "--" ) .                      // not on a map entry m[k]
ExprStmt    = Expr .                                      // must be a call or try

If          = "if" "(" Expr ")" Block [ "else" ( If | Block ) ] .
For         = "for" "(" [ ForInit ] ";" [ Expr ] ";" [ ForPost ] ")" Block .
ForInit     = VarDecl | Assign .
ForPost     = Assign | IncDec .

Switch      = "switch" "(" Expr ")" "{" NL { CaseClause } "}" .
CaseClause  = ( "case" CaseLabel { "," CaseLabel } | "default" ) ":" NL { Stmt NL } .   // default only last
CaseLabel   = Expr | EnumPattern .
EnumPattern = TypeName "." Ident [ "(" Params ")" ] .

Select      = "select" "{" NL { SelectClause } "}" .
SelectClause = "case" ( VarDecl | Assign | Expr ) ":" NL { Stmt NL } .   // ch.send(v), ch.receive(), _ = ch.receive() or T? v = ch.receive()

Return      = "return" [ Expr { "," Expr } ] .
Defer       = "defer" [ "_" "=" ] Expr .                   // a call; _ = discards its error
Spawn       = "spawn" Expr .                               // a call, only inside a group block
Assert      = "assert" "(" Expr ")" .                      // only inside a test
```

A statement that starts with a type followed by a name (`User u = ...`, `[]int xs = ...`, `users.User u = ...`, `int? n = ...`) is a declaration. Anything else is an assignment or an expression statement.

## Expressions

Binary operators, from lowest to highest precedence:

| Level | Operators |
|---|---|
| 1 | `\|\|` |
| 2 | `&&` |
| 3 | `== != < <= > >=` |
| 4 | `+ - \| ^` |
| 5 | `* / % << >> &` |

All binary operators are left-associative. Comparison operators do not chain: `a < b < c` is a compile error.

```
Expr        = UnaryExpr | Expr BinaryOp Expr .
UnaryExpr   = PostfixExpr | ( "-" | "!" ) UnaryExpr | "try" UnaryExpr .
PostfixExpr = Primary { "." Ident | "[" Expr "]" | "(" [ Args ] ")" } .
Args        = Expr { "," Expr } .
Primary     = Literal | Ident | "(" Expr ")"
            | ListLit | MapLit | StructLit | ChanMake | Group .
ListLit     = "[" "]" BaseType "{" [ Expr { "," Expr } [ "," ] ] "}" .
MapLit      = "map" "[" Type "]" BaseType "{" [ Entry { "," Entry } [ "," ] ] "}" .
Entry       = Expr ":" Expr .
StructLit   = TypeName "{" [ FieldInit { "," FieldInit } [ "," ] ] "}" .
FieldInit   = Ident ":" Expr .
ChanMake    = "chan" BaseType "(" Expr ")" .
Group       = "group" Block .
```

- A conversion looks like a call to a type name: `int(x)`, `float(n)`, `byte(i)`, `string(bytes)`.
- An enum value is `Type.Case(args)`, or `Type.Case` for a case without data. In an `EnumPattern`, a case with data has a `Param` for every field (`_` as the name ignores one), and a case without data has no parentheses.
- Conditions in `if`, `for` and `switch` are always parenthesised, so `Name{` is always a struct literal.
- Blocks and expressions nest at most 256 levels deep.
- A `Group` is only used whole: as the value of an assignment (`error err = group {...}`, `err = ...`, `_ = ...`), of `return`, or after `try`.
