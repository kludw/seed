# Seed grammar

This is part of the language spec ([SPEC.md](../SPEC.md)). It uses EBNF: `{ x }` means zero or more, `[ x ]` means optional, `|` separates choices, and quoted text is a literal token.

## Lexical structure

- Source files are UTF-8 and use the `.sd` extension.
- Comments start with `//` and run to the end of the line. There are no block comments.
- Identifiers: a letter or `_`, followed by letters, digits or `_`. `_` alone is the discard name.
- Keywords:
  `package import pub fn type struct enum interface implements const if else for switch case default break continue return defer try null true false mut test assert select spawn group chan map`
- Built-in type names (`int float bool byte string error`) are ordinary identifiers that refer to built-in types.
- Literals are described in [SPEC.md](../SPEC.md#literals).
- Operators and punctuation:
  `+ - * / % & | ^ << >> == != < <= > >= && || ! = ++ -- ( ) [ ] { } , ; : . ?`

### Newlines

A newline ends a statement. As in Go, the lexer turns a newline into a statement terminator (written `NL` below) when the last token on the line is one of:

- an identifier or a literal (including `true`, `false`, `null`)
- `return`, `break`, `continue`
- `)`, `]`, `}`, `?`, `++`, `--`

Blank lines and comment-only lines produce nothing. Because of this rule, an expression that continues on the next line must break after an operator or a comma, never before it:

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
FnDecl      = "fn" [ Receiver ] Ident "(" [ Params ] ")" [ Result ] Block .
Receiver    = "(" Type Ident ")" [ "mut" ] .
Params      = Param { "," Param } .
Param       = Type Ident .
Result      = Type | "(" Type { "," Type } ")" .

TypeDecl    = "type" Ident ( StructType | EnumType | InterfaceType ) .
Implements  = "implements" TypeName { "," TypeName } .
StructType  = "struct" [ Implements ] "{" NL { Type Ident NL } "}" .
EnumType    = "enum" [ Implements ] "{" NL { Ident [ "(" Params ")" ] NL } "}" .
InterfaceType = "interface" "{" NL { MethodSig NL } "}" .
MethodSig   = [ "mut" ] Ident "(" [ Params ] ")" [ Result ] .

ConstDecl   = "const" Type Ident "=" Expr .
```

Example of a method declaration: `fn (User u) mut tag(string t) { ... }`.

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

`?` applies to the whole type on its left: `[]string?` is a nullable list of strings. A list of nullable strings needs parentheses: `[](string?)`. `T??` is not a valid type.

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
IncDec      = LValue ( "++" | "--" ) .
ExprStmt    = Expr .                                      // must be a call, try, or group

If          = "if" "(" Expr ")" Block [ "else" ( If | Block ) ] .
For         = "for" "(" [ ForInit ] ";" [ Expr ] ";" [ ForPost ] ")" Block .
ForInit     = VarDecl | Assign .
ForPost     = Assign | IncDec .

Switch      = "switch" "(" Expr ")" "{" NL { CaseClause } "}" .
CaseClause  = ( "case" CaseLabel { "," CaseLabel } | "default" ) ":" NL { Stmt NL } .
CaseLabel   = Expr | EnumPattern .
EnumPattern = TypeName "." Ident [ "(" Params ")" ] .

Select      = "select" "{" NL { SelectClause } "}" .
SelectClause = "case" ( VarDecl | Expr ) ":" NL { Stmt NL } .   // a send or receive call

Return      = "return" [ Expr { "," Expr } ] .
Defer       = "defer" Expr .                               // a call
Spawn       = "spawn" Expr .                               // a call, only inside a group
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
StructLit   = TypeName "{" [ Field { "," Field } [ "," ] ] "}" .
Field       = Ident ":" Expr .
ChanMake    = "chan" BaseType "(" Expr ")" .
Group       = "group" Block .
```

- A conversion looks like a call to a type name: `int(x)`, `float(n)`, `byte(i)`, `string(bytes)`.
- An enum value is `Type.Case(args)`, or `Type.Case` for a case without data.
- Conditions in `if`, `for` and `switch` are always parenthesised, so `Name{` is always a struct literal.
