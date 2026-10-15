# A package of several files, checked as a package (in test mode, as seed
# test checks it, since it is not package main), and its tests.
open proj/shop/cart.sd

# Item, its field price and newCart's parameter, from the package's other
# file.
definition proj/shop/cart.sd 16 25
hover proj/shop/cart.sd 17 15
definition proj/shop/cart.sd 28 33
definition proj/shop/cart.sd 12 27
# A receiver and its use, a local in a loop.
hover proj/shop/cart.sd 16 13
definition proj/shop/cart.sd 20 8
completion proj/shop/cart.sd 28 20
# The private field items is visible inside the package.
completion proj/shop/cart.sd 20 10

# An unsaved edit of the other file (price renamed to cost) is what
# cart.sd is checked against: both documents get their diagnostics again.
open proj/shop/item.sd
change proj/shop/item.sd edits/item_renamed.sd
hover proj/shop/cart.sd 17 15
change proj/shop/item.sd proj/shop/item.sd
close proj/shop/item.sd

# A test file: the package and its tests are checked together.
open proj/shop/tests/cart_test.sd
hover proj/shop/tests/cart_test.sd 3 16
definition proj/shop/tests/cart_test.sd 4 14
completion proj/shop/tests/cart_test.sd 5 13
symbols proj/shop/tests/cart_test.sd
symbols proj/shop/cart.sd
