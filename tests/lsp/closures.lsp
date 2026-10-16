# Function literals (closures/main.sd): a variable captured by a literal
# is the variable around it for hover, definition, references, highlights
# and rename; the literal's parameters and the variables around it are
# completed inside its body; signature help inside a literal's body.
open closures/main.sd

# offset inside the sortBy literal: hover and definition (line 10), and a
# parameter of the literal.
hover closures/main.sd 13 23
definition closures/main.sd 13 23
hover closures/main.sd 13 17

# Every use of offset, in both literals, from its declaration and from a
# use inside a literal.
references closures/main.sd 10 8 decl
highlight closures/main.sd 17 19

# Completion at the start of a statement in the literal: its parameters,
# the locals before it and main's.
completion closures/main.sd 14 8

# Signature help: sortBy's, a function value holding a literal, and in no
# call at the start of a line of a literal's body.
signature closures/main.sd 12 15
signature closures/main.sd 19 22
signature closures/main.sd 14 8

# Renaming the captured variable renames its uses inside the literals.
rename closures/main.sd 10 8 delta
