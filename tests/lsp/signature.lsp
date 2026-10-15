# Signature help: the signature of the call the cursor is in, with the
# parameter it is at (activeParameter, from the commas before it).
open proj/main.sd

# A function of another package, a method whose argument is a struct
# literal (the cursor inside the literal), a function of the file around
# an enum case's constructor, and the constructor.
signature proj/main.sd 18 35
signature proj/main.sd 19 40
signature proj/main.sd 21 87
signature proj/main.sd 21 102

# More calls (edits/calls.sd): the second parameter, a call after
# non-ASCII text (UTF-16 character 32 is byte 35), the standard library
# function around it (in its string, and after the inner call), a
# built-in method and a function value.
change proj/main.sd edits/calls.sd
signature proj/main.sd 12 22
signature proj/main.sd 13 32
signature proj/main.sd 13 18
signature proj/main.sd 13 37
signature proj/main.sd 14 33
signature proj/main.sd 16 23
# In no call: a statement, a function's parameters.
signature proj/main.sd 18 4
signature proj/main.sd 6 14

# Calls being typed: after a comma and right after the '('
# (edits/call_typed.sd); after the next statement, which the parser takes
# as the call's argument, the cursor is in no call any more.
change proj/main.sd edits/call_typed.sd
signature proj/main.sd 12 23
signature proj/main.sd 12 20
signature proj/main.sd 14 4
# A method of a local, typed inside a struct literal argument
# (edits/call_method.sd).
change proj/main.sd edits/call_method.sd
signature proj/main.sd 19 40
