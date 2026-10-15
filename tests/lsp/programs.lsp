# A directory of single-file programs (like examples/): each file that
# declares main is a program of its own, so the two greet functions and
# mains do not clash, and each document gets only its own errors (two.sd
# has one: nul).
open programs/one.sd
open programs/two.sd
definition programs/one.sd 10 6
hover programs/two.sd 10 6
completion programs/two.sd 11 11
