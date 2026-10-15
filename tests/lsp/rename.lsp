# Rename and prepareRename in the project (proj/main.sd and its package
# shop of two files, with tests): the edits cover every file that names
# the symbol, unsaved open documents included, with UTF-16 ranges.
open proj/main.sd

# prepareRename: the range of the name and the name; nothing on a
# keyword or a string; refused on a package and a standard library
# function.
prepareRename proj/main.sd 20 31
prepareRename proj/main.sd 22 5
prepareRename proj/main.sd 21 28
prepareRename proj/main.sd 21 5
prepareRename proj/main.sd 21 9

# A local, used after non-ASCII text.
rename proj/main.sd 18 15 basket
# A method of the other package: main.sd, cart.sd and the package's tests.
rename proj/main.sd 19 14 put
# A type: both files of the package, main.sd and the tests.
rename proj/shop/item.sd 3 10 Product
# An enum case, a pub field, a constant of main.sd, a function.
rename proj/shop/item.sd 11 5 Tiny
rename proj/main.sd 20 35 holder
rename proj/main.sd 6 15 welcome
rename proj/main.sd 8 5 sizeName

# An unsaved edit of the other file is what is renamed: in
# item_renamed.sd the field price is called cost.
open proj/shop/item.sd
change proj/shop/item.sd edits/item_renamed.sd
rename proj/shop/item.sd 6 14 amount
change proj/shop/item.sd proj/shop/item.sd
close proj/shop/item.sd

# Refused: a keyword, a built-in type, a name that breaks the naming
# rules (with the name it should have), a name already taken (the
# compiler's error), a standard library function, a built-in method, a
# package, nothing at all.
rename proj/main.sd 18 15 for
rename proj/main.sd 18 15 string
rename proj/main.sd 18 15 my_cart
rename proj/main.sd 18 15 Basket
rename proj/shop/item.sd 3 10 product
rename proj/main.sd 18 15 2cart
rename proj/main.sd 18 15 label
rename proj/main.sd 8 5 main
rename proj/shop/item.sd 10 10 Item
rename proj/main.sd 21 9 say
rename proj/main.sd 13 34 text
rename proj/main.sd 21 5 out
rename proj/main.sd 22 5 x

# Refused while the program has syntax errors (its references could be
# incomplete).
change proj/main.sd edits/syntax_errors.sd
rename proj/main.sd 18 15 basket
