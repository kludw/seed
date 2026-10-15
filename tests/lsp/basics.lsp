# Hover, definition, completion, symbols and formatting on a correct
# program (proj/main.sd) that uses a package of two files (proj/shop).
# Lines 6, 20 and 21 have non-ASCII text before the names asked about, so
# the characters are UTF-16 units: "😀" is 2, "é" is 1.
open proj/main.sd

# A local, then the method it calls, in another package's file.
hover proj/main.sd 19 10
definition proj/main.sd 19 10
definition proj/main.sd 19 14
hover proj/main.sd 19 14
# shop.Item (a type of the package's other file), a field in a struct
# literal, shop.Size.Small and its parts.
definition proj/main.sd 19 23
definition proj/main.sd 19 41
definition proj/main.sd 19 70
definition proj/main.sd 19 64
definition proj/main.sd 19 59
# cart.owner after "😀 é " (UTF-16 character 35 is byte 39).
hover proj/main.sd 20 35
definition proj/main.sd 20 35
# A standard library function with its doc comment, the package itself.
hover proj/main.sd 21 9
hover proj/main.sd 21 5
# A constant whose value has non-ASCII text, used after it.
hover proj/main.sd 21 17
definition proj/main.sd 21 17
# A function of the file, a method call, a built-in method.
definition proj/main.sd 21 80
hover proj/main.sd 21 52
hover proj/main.sd 13 34
# An enum pattern: the type, the case, the binding and its use.
hover proj/main.sd 12 15
hover proj/main.sd 12 20
hover proj/main.sd 12 30
definition proj/main.sd 13 28
# A parameter and its use, a qualified parameter type, the import path,
# the package clause.
definition proj/main.sd 9 12
hover proj/main.sd 8 17
definition proj/main.sd 3 9
hover proj/main.sd 0 9
# Nothing there: a keyword, a string, a blank line.
hover proj/main.sd 22 5
definition proj/main.sd 21 28
hover proj/main.sd 16 0

# Completion: a value's fields and methods (cart.o), a package's pub
# names (shop.n), an enum's cases (shop.Size.S), names in scope.
completion proj/main.sd 20 35
completion proj/main.sd 18 27
completion proj/main.sd 19 69
completion proj/main.sd 21 17

symbols proj/main.sd
format proj/main.sd

# Not supported: an error response, and the server carries on.
request textDocument/codeAction {"textDocument":{"uri":"file:///nowhere.sd"},"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":0}},"context":{"diagnostics":[]}}
hover proj/main.sd 19 10
