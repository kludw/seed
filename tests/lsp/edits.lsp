# Diagnostics while a document is edited (full-text didChange), and
# answers on broken code.
open proj/main.sd

# A type error after non-ASCII text: cart.ownr at UTF-16 character 34
# (byte 38).
change proj/main.sd edits/type_error.sd
hover proj/main.sd 20 35

# A syntax error: only the parser's error is reported; hover and
# completion still answer from the parts that parse; formatting makes no
# edits.
change proj/main.sd edits/syntax_error.sd
hover proj/main.sd 21 17
definition proj/main.sd 21 80
completion proj/main.sd 21 17
symbols proj/main.sd
format proj/main.sd

# Several syntax errors: one in a function's header, one in that
# function's body and one in another function, all reported; hover still
# answers in the body of the function with the broken header.
change proj/main.sd edits/syntax_errors.sd
hover proj/main.sd 19 14

# Not formatted: a warning, and formatting replaces the whole text.
change proj/main.sd edits/unformatted.sd
format proj/main.sd

# Completion right after "cart." with nothing typed after the dot.
change proj/main.sd edits/dot.sd
completion proj/main.sd 20 34

# A request whose work crashes gets a null result (the crash is logged to
# stderr) and the server carries on.
request seed/crash {"textDocument":{"uri":"file:///nowhere.sd"}}
change proj/main.sd proj/main.sd
hover proj/main.sd 19 14

# Closing clears the document's diagnostics.
close proj/main.sd
