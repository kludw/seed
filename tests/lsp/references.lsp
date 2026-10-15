# References and document highlights in the project (proj/main.sd and
# its package shop of two files, with tests). Positions are UTF-16 units:
# on line 20, cart comes after "😀 é " (UTF-16 character 29, byte 33).
open proj/main.sd

# A local: its declaration, its uses (one after non-ASCII text), with and
# without the declaration; the highlights in the document, asked from the
# use after the emoji.
references proj/main.sd 18 15 decl
references proj/main.sd 18 15
highlight proj/main.sd 20 31

# A method of the other package: its declaration, the call in main.sd and
# the call in the package's tests.
references proj/main.sd 19 14 decl
# A pub field: its declaration, a struct literal of the package that sets
# it (not newCart's parameter of the same name) and the use in main.sd.
references proj/main.sd 20 35 decl
# A type: the field of the other file of its package, the parameter, the
# patterns of the switch, the qualified uses and the tests.
references proj/shop/item.sd 10 10 decl
# An enum case, in a pattern, in an expression, in the tests.
references proj/shop/item.sd 11 5 decl
# A constant, and a function of the file.
references proj/main.sd 6 15 decl
references proj/main.sd 8 5 decl
# A private field, from inside its package: the package and its tests only.
open proj/shop/cart.sd
references proj/shop/cart.sd 7 13 decl
highlight proj/shop/cart.sd 20 11
# A receiver, a parameter and an enum pattern's binding: their function only.
highlight proj/shop/cart.sd 16 13
references proj/shop/cart.sd 16 29 decl
references proj/main.sd 12 30 decl

# A standard library function: its uses in the workspace (not in the
# standard library, which is not in it).
references proj/main.sd 21 9
# Nothing to find: a built-in method, a package name, a keyword, a string.
references proj/main.sd 13 34 decl
references proj/main.sd 21 5 decl
highlight proj/main.sd 22 5
references proj/main.sd 21 28 decl

# Broken code: the references still come from what parses.
change proj/main.sd edits/syntax_errors.sd
references proj/main.sd 18 15 decl
highlight proj/main.sd 19 9
