# Spec conformance audit

An audit of `SPEC.md`, `docs/grammar.md`, `docs/builtins.md` and
`docs/formatting.md` against the compiler at `seed-v2` (`1cd8f36`). Every
normative statement (each "must", "is", "cannot", each table row) was checked
for a test that pins it down. The existing suites were searched first
(`tests/lang`, `tests/core`, `tests/full`, their `errors/`, `tests/fmt`,
`tests/testcmd`, `lib/*/tests`, `docs/guide/examples`); a test was added in
`tests/spec/` only where a rule wasn't covered.

Run the new tests with `tests/run.sh "build/seed2 c" tests/spec` (part of
`make test-full`; 211 tests since the resolution) and `tests/test_cmd.sh
build/seed2` (the `spec/testcmd/` projects and the toolchain checks). The
five bugs were fixed and their tests moved from `spec/pending/errors/` to
`spec/errors/`; every finding's outcome is under [Resolution](#resolution).

## Summary

| | Count |
|---|---|
| Normative statements reviewed (the "rules" counts in the tables below) | 394 (SPEC.md 220, grammar.md 54, builtins.md 80, formatting.md 40) |
| Already covered by an existing test | 272 |
| Newly covered | 117 |
| Not observable by a test (SPEC "Memory") | 5 |
| New test programs (`spec/*.sd`, `spec/NAME/`) | 44 |
| New compile-error tests (`spec/errors/`) | 157 (14 of them `fmt_*`) |
| New `seed test` projects (`spec/testcmd/`) | 3 |
| New checks in `tests/test_cmd.sh` | 10 |
| Implementation bugs (tests were in `spec/pending/`; all fixed) | 5 |
| Spec problems | 19 |

Names below: `lang/x` is `tests/lang/x.sd`, `full/errors/x` is
`tests/full/errors/x.sd` and so on; **bold** names are new
(`tests/spec/...`); `test_cmd` is a check in `tests/test_cmd.sh`.

## Coverage

### SPEC.md

| Section (rules) | Existing tests | New tests |
|---|---|---|
| Syntax basics (7): `//` comments only; no `;`; continuation lines; braces required and on the same line; conditions in parentheses; formatting is part of the language; naming | full/errors/unformatted_code, core/errors/statement_newline, lang/errors/syntax_condition_without_parens, full/errors/naming_* (6), fmt/continuation | **errors/syntax_block_comment**, **errors/syntax_semicolon**, **errors/syntax_operator_starts_line**, **newlines_continuation**, **errors/syntax_braces_required**, **errors/syntax_brace_same_line**, **errors/syntax_switch_without_parens**, **errors/syntax_for_without_parens**, **errors/naming_field_snake_case**, **errors/naming_parameter_pascal_case**, **errors/naming_method_pascal_case**, **errors/naming_interface_lowercase**, **errors/naming_enum_snake_case** |
| Literals (18): int decimal/hex of any size, no octal/binary/`_`, unary `-`; float needs `.`; byte is one byte, string escapes; bad escapes; no raw/multi-line strings; bool; null; list/map/struct/chan literals, map key types, struct literal by name | lang/literals_int, full/bigint_arith, full/int_no_overflow, lang/float_arith, lang/byte_literals, full/errors/byte_bad_escape, lang/string_escapes, core/strings_escapes, lang/list_basic, lang/map_basic, lang/map_key_types, lang/struct_basic, full/errors/struct_literal_missing_field, full/chan_values | **errors/literal_octal**, **errors/literal_binary**, **errors/literal_underscore**, **errors/literal_float_exponent_only**, **errors/literal_byte_two_bytes**, **errors/literal_byte_multibyte_char**, **errors/literal_string_bad_escape**, **errors/literal_string_bad_hex_escape**, **errors/literal_string_multiline**, **errors/literal_raw_string**, **errors/literal_list_without_type**, **errors/literal_map_float_key**, **errors/literal_map_struct_key**, **errors/literal_map_nullable_key**, **errors/literal_struct_positional**, **errors/literal_struct_duplicate_field** |
| Types (16): the type table; explicit conversions only; `?` applies to the whole type, `T??`; no user generics; no struct cycles by value | full/bigint_*, lang/conversions, lang/crash_byte_range, lang/crash_float_to_int, lang/errors/type_int_to_float, core/errors/no_implicit_conversion, core/narrowing, core/equality, full/errors/double_nullable, core/errors/recursive_struct, full/chars_utf8 | **errors/conv_int_of_string**, **errors/conv_string_of_int**, **errors/conv_bool_of_int**, **errors/conv_implicit_int_to_byte**, **errors/conv_implicit_byte_to_string**, **errors/types_error_nullable**, **errors/types_generic_struct**, **errors/types_struct_contains_itself**, **types_cycle_breakers**, **fn_type_nullable** |
| Lists (14) | lang/list_* (6), full/sort_by, lang/crash_index, lang/crash_negative_index, lang/crash_list_remove, core/mut_paths | **errors/list_sort_bool**, **errors/list_join_int**, **errors/list_append**, **list_insert_out_of_range_crash**, **list_slice_out_of_range_crash**, **list_write_out_of_range_crash** |
| Maps (7) | lang/map_basic, lang/map_order_delete, core/map_order, core/equality, lang/errors/nullable_map_lookup | |
| Converting to text (3) | lang/byte_ops, lang/float_format | **text_conversion**, **errors/text_list_string**, **errors/text_map_string**, **errors/text_struct_string** |
| Printing (3) | lang/hello, lang/stderr_print | **errors/print_int_argument**, **errors/print_two_arguments** |
| Variables and constants (9): type first, no `var`/`:=`; every declaration has a value; shadowing (locals, parameters, package-level names, imports; hint `=`); several results (declare/assign/`_`); `const` rules; no package-level variables | lang/errors/syntax_decl_without_value, core/errors/shadow, full/errors/shadowing, full/errors/shadowing_function, full/errors/param_receiver_same_name, lang/multiple_returns, core/try_discard, lang/const, full/const_values, full/consts_counted, full/errors/const_call, full/errors/const_local_variable, full/errors/const_chan | **errors/decl_var_keyword**, **errors/decl_short_assign**, **errors/shadow_parameter**, **errors/shadow_package_constant**, **errors/shadow_imported_package**, **errors/shadow_parameter_named_like_function**, **errors/shadow_hint_assign**, **scope_sibling_blocks**, **decl_mixed_targets**, **errors/const_method_call**, **errors/const_try**, **errors/const_local_channel**, **errors/package_level_variable** |
| Operators (16): the operator table; `==` semantics; integer division/`%`; division by zero; same types on both sides; no byte arithmetic; no compound assignment; `++` is a statement; no `?:`, no unary `^`; precedence; no chained comparisons | lang/int_arith, lang/float_arith, lang/int_bitwise, lang/byte_ops, full/int_ops_big, core/equality, lang/struct_equality, full/interface_values, lang/crash_div_zero, lang/crash_mod_zero, full/float_div_zero, lang/float_special, lang/errors/type_int_plus_float, lang/errors/type_string_plus_int, lang/errors/syntax_compound_assign, lang/precedence, core/errors/chain_compare*, full/errors/chan_compare, full/errors/compare_functions | **operators_equality_errors_interfaces**, **op_shift_semantics**, **op_shift_negative_crash**, **errors/op_float_modulo**, **errors/op_bitwise_bool**, **errors/op_bitwise_float**, **errors/op_ordering_bool**, **errors/op_ordering_list**, **errors/op_logic_int**, **errors/op_not_int**, **errors/op_negate_string**, **errors/op_negate_byte**, **errors/op_byte_arithmetic**, **errors/op_byte_increment**, **errors/op_unary_caret**, **errors/op_ternary**, **errors/op_increment_expression**, **errors/op_compound_minus**, **errors/op_compare_struct_with_channel**, **errors/op_compare_list_of_functions** |
| Null (8): plain `T` never null; narrowing of variables and fields, not of elements, lookups or call results; `&&`, `\|\|`, `!`; after an `if` that leaves (`return`, `break`, `continue`, `os.exit`, `os.crash`); `assert`; assignment of a non-null value; where narrowing ends | lang/nullable_narrowing, lang/nullable_fields, lang/nullable_loops, core/narrowing, full/narrow_assign, full/loop_post_narrowing, lang/errors/nullable_*, core/errors/narrow_*, core/errors/null_to_plain, full/errors/narrow_*, full/errors/nullable_element_hint, full/errors/loop_post_narrowing_killed, testcmd/pass (assert narrows) | **null_narrowing_not**, **null_narrowing_after_crash**, **null_narrowing_else_early_exit**, **null_narrowing_or_early_exit**, **null_narrowing_kept**, **errors/null_plain_param**, **errors/null_narrowing_list_element**, **errors/null_narrowing_call_result**, **errors/null_narrowing_not_else**, **errors/null_narrowing_or_wrong_side**, **errors/null_narrowing_prefix_assign**, **errors/null_narrowing_prefix_mut**, **testcmd/rules** |
| Values, not pointers (4) | lang/value_semantics_*, core/value_copies, core/value_enums, core/errors/assign_param, full/chan_reply_struct | |
| Functions and methods (9) | lang/multiple_returns, lang/methods, lang/mut_methods, core/mut_paths, full/errors/duplicate_function, full/errors/receiver_not_mut, full/errors/pkg_foreign_method, lang/errors/mut_method_on_param, lang/errors/builtin_mut_on_param, lang/errors/mut_on_param_field, core/errors/mut_param*, core/errors/mut_temporary | **errors/fn_naked_return**, **errors/fn_named_results**, **errors/fn_variadic**, **errors/fn_overloading**, **errors/fn_default_argument**, **errors/method_on_builtin_type**, **errors/method_on_interface**, **errors/method_plain_receiver_mut_call** |
| Function values (5) | full/fn_values, full/fn_values_more, full/fn_nullable_field, full/errors/compare_functions | **errors/fn_anonymous**, **fn_type_nullable**, **errors/fn_value_nullable_needs_parens**, **fn_field_called_directly**, **errors/struct_field_and_method_same_name**, **errors/fn_value_nullable_hint** |
| Structs, enums, interfaces (8) | full/errors/struct_literal_missing_field, core/errors/missing_struct_field, full/errors/pkg_private_field, lang/enum_*, full/enum_patterns, full/errors/enum_pattern_*, full/errors/switch_enum_missing_case, full/interface_*, full/errors/implements_*, full/errors/missing_implements, full/errors/interface_mut_on_param | **errors/enum_case_without_type**, **errors/enum_pattern_without_type**, **errors/enum_value_empty_parens**, **errors/enum_pattern_wrong_type**, **errors/interface_value_not_implementing**, **errors/interface_mut_on_call_result**, **implements_several**, **errors/enum_case_empty_parens_decl** |
| Control flow (10) | lang/for_loops, lang/break_continue, lang/switch_values, full/switch_loop_control, full/select_cases, full/errors/switch_*, core/errors/enum_default, core/errors/value_switch_default, full/defer_*, full/errors/defer_*, lang/os_exit | **errors/for_while_form**, **errors/for_post_call**, **errors/op_increment_expression**, **errors/switch_string_no_default**, **errors/switch_bool_no_default**, **errors/switch_byte_no_default**, **errors/switch_struct_value**, **errors/enum_pattern_untyped_binding**, **errors/break_outside_loop**, **errors/continue_in_switch_outside_loop**, **errors/goto_label**, **defer_kinds**, **os_exit_low_bits**, **errors/defer_builtin_send**, **errors/defer_not_call**, **errors/switch_default_not_last** |
| Errors (8): `error` interface; null error; `(..., error)`; every result handled; error guards; `try`; `return null, err`; `_` | lang/errors_basic, full/interface_custom_error, full/error_*, full/errors/guard_*, full/errors/unchecked_error_*, full/errors/ignored_error, full/errors/unused_return_value, lang/errors/error_*, lang/try_*, core/errors/try_no_error_result, full/return_null_without_error, full/errors/return_null_*, lang/discard, lang/errors/discard_plain_type, core/errors/discard_plain, full/group_crash, full/os_crash | **error_guard_or_early_exit**, **error_guard_nullable_field_target**, **errors/error_guard_element_target**, **try_precedence**, **errors/try_in_function_without_error**, **errors/error_return_value_ignored**, **errors/unused_builtin_result**, **errors/error_return_null_beside_null**, **errors/error_discard_plain_beside**, **testcmd/rules** (guard lifted by `assert`) |
| Concurrency (8) | full/group_*, full/chan_deadlock, full/group_deadlock, full/group_cancel_receiver, full/os_stop_points, full/fs_file_stop, full/spawn_kinds, full/chan_copy_semantics, full/errors/spawn_outside_group | **io_stdin_stop_point** (+ test_cmd), **errors/spawn_in_function_called_by_group** |
| Groups (4) | full/group_forms, full/group_nested, full/errors/group_*, full/group_error | **group_block_not_stopped**, **errors/group_statement_alone**, **errors/group_in_condition**, **errors/group_continue_outer_loop** |
| Spawn (3) | full/spawn_kinds, full/group_spawn_loop, full/errors/spawn_* | **errors/spawn_builtin_method** |
| Channels (7) | full/chan_*, full/errors/chan_*, full/errors/const_chan, full/group_basic (channel parameters) | **chan_negative_buffer_crash**, **chan_close_twice_crash**, **select_send_closed_crash**, **errors/chan_nullable_elem_list**, **errors/chan_receive_error_is_error** |
| Select (6) | full/select_*, full/errors/select_* | **select_random_choice**, **select_evaluation_order**, **select_receive_error**, **errors/select_default**, **errors/select_receive_plain_type**, **time_after_value**, **errors/select_receive_error_unhandled** |
| Packages (8) | full/pkg_basic, full/errors/pkg_* (17), core/errors/import_unknown, core/errors/missing_import, full/errors/unused_import | **pkg_tests_dir_ignored/**, **errors/pkg_import_per_file/**, **errors/pkg_mixed_package_names/**, **errors/pkg_import_path_uppercase/**, **errors/pkg_private_const/**, **errors/pkg_private_field_write/**, **errors/pkg_name_same_as_import**, **errors/pkg_core_not_imported** |
| Native functions (1) | full/errors/pkg_native | |
| Program entry (4) | core/errors/main_signature, lang/main_args, lang/main_error, lang/crash_*, lang/os_exit | **program_args** (+ test_cmd) |
| Tests (7) | testcmd/pass, testcmd/fail, testcmd/broken, full/errors/pkg_test_outside, full/errors/assert_outside_test, lib/*/tests | **testcmd/rules**, **testcmd/test_return**, **testcmd/test_assert_in_group**, **pkg_tests_dir_ignored/**, test_cmd (`seed test` without a directory) |
| Memory (5) | not observable by a program; `make test-leaks`, full/values_refcount | |
| Compiler (16) | the error tests above; core/errors/unknown_field ("did you mean") | **unused_package_level_allowed**, **errors/unused_pattern_binding**, **errors/unreachable_after_infinite_loop**, **errors/unreachable_after_os_crash**, **control_infinite_loop_no_return** |
| Toolchain (6) | tests/test_cmd.sh (run, build of a directory, test, fmt error, symbolic link, cache, install) | test_cmd: `seed run -- ...`, `seed build file.sd` with and without `-o`, `seed ast`, `seed fmt --check` output and exit codes |

### docs/grammar.md

| Section (rules) | Existing tests | New tests |
|---|---|---|
| Lexical structure (6): UTF-8 `.sd`; comments; identifiers and `_`; keywords; built-in type names; operators | full/errors/builtin_type_name, full/errors/builtin_name_function | **errors/identifier_starts_with_digit**, **errors/identifier_non_ascii_letter**, **errors/discard_read**, **errors/keyword_select_as_name**, **errors/keyword_group_as_name**, **errors/keyword_test_as_name**, **errors/keyword_map_as_name**, **errors/identifier_non_ascii_message** |
| Newlines (2) | core/errors/statement_newline, fmt/continuation | **newlines_continuation**, **errors/syntax_operator_starts_line** |
| Files (4) | full/errors/pkg_test_outside, full/errors/pkg_native | **errors/import_after_declaration**, **errors/package_line_missing**, **errors/import_two_on_one_line** |
| Declarations (9) | fmt/decls, full/pkg_basic, full/interface_enum, lang/const | (see SPEC sections) |
| Types (3) | full/errors/double_nullable, core/narrowing, full/fn_nullable_field | **fn_type_nullable** |
| Statements (13) | lang/*, full/select_cases, full/defer_more | **errors/expr_statement_not_call**, **decl_mixed_targets**, **errors/for_post_call** |
| Expressions (17): precedence levels, left associativity, no chaining, productions, conversions, enum values, `Name{` in conditions, `group` used whole | lang/precedence, core/errors/chain_compare*, lang/conversions, full/errors/group_in_expression | **try_precedence**, **struct_literal_in_condition**, **errors/group_in_condition** |

### docs/builtins.md

| Section (rules) | Existing tests | New tests |
|---|---|---|
| Runtime failure behaviour (4) | lang/main_error, lang/crash_*, full/defer_crash, full/group_crash_message, lang/os_exit, full/chan_deadlock | **os_exit_low_bits** |
| int, float, bool, byte (8) | lang/conversions, lang/float_format, lang/float_special, lang/crash_byte_range, lang/crash_float_to_int, lang/byte_ops, lang/bool_ops | **float_to_int_nan_crash**, **text_conversion** |
| string (18) | lang/string_*, lang/crash_string_*, lang/crash_split_empty, lang/crash_replace_empty, full/chars_utf8, full/core_error_messages | **string_ascii_only** |
| Lists (13) | lang/list_*, full/sort_by | **list_insert_out_of_range_crash**, **list_slice_out_of_range_crash**, **list_write_out_of_range_crash**, **errors/list_sort_bool**, **errors/list_join_int** |
| Maps (6) | lang/map_*, core/map_order | |
| error, channels, stop points (6) | full/chan_*, full/group_cancel_receiver | **chan_negative_buffer_crash**, **chan_close_twice_crash** |
| Core packages (25): errors, fmt, os, fs, time, error message form | full/core_error_messages, lib/errors/tests, lang/stderr_print, lang/os_*, full/os_crash, lang/fs_basic, lang/fs_errors, full/select_timeout | **errors_wrap_null_crash**, **os_exit_low_bits**, **os_run_empty_argv_crash**, **time_constants**, **time_after_value** |

### docs/formatting.md

| Section (rules) | Existing tests | New tests |
|---|---|---|
| The command (4) | test_cmd (unformatted file names `seed fmt`) | test_cmd: `--check` output and exit codes, syntax errors not rewritten |
| What the formatter changes (2): line endings, final newline, trailing whitespace, tabs | fmt/edge (tabs) | **errors/fmt_crlf_line_endings**, **errors/fmt_extra_final_newline**, **errors/fmt_no_final_newline**, **errors/fmt_trailing_whitespace**, **errors/fmt_tab_indentation** |
| Indentation, statements and blocks, switch and select (7) | fmt/blocks, fmt/spacing | |
| Declarations (2) | fmt/decls, fmt/toplevel | **errors/fmt_blank_line_between_imports** |
| Blank lines (5) | fmt/toplevel, fmt/comments, fmt/blocks | **errors/fmt_blank_line_in_continuation**, **errors/fmt_blank_line_after_case**, **errors/fmt_blank_line_before_brace**, **errors/fmt_two_blank_lines_between_decls**, **errors/fmt_no_blank_between_decls** |
| Spaces (10) | fmt/spacing | **errors/fmt_space_before_paren_call**, **errors/fmt_result_type_no_space** |
| Continuation lines, lists (5) | fmt/continuation, fmt/lists, fmt/edge | |
| Comments (5) | fmt/comments, fmt/edge | **errors/fmt_comment_trailing_spaces** |

## Implementation bugs

The spec is right in each case. The tests were in `tests/spec/pending/errors/`
and are now in `tests/spec/errors/`, where they pass (see [Resolution](#resolution)).

1. **`switch_default_not_last`**: a value `switch` with `default` before
   another `case` compiles.
   - Expected: an error at line 11 ("A `switch` over an `int`, `byte`, `string` or `bool` must end with `default`", and the
     compiler's own message is "must end with a default case").
   - Actual: no error. The `default` is accepted anywhere, and the program works as if it were last.
   - Repro: `switch (x) { case 1: ... default: ... case 2: ... }` in a function over `int x`.

2. **`enum_case_empty_parens_decl`**: an enum case declared with empty parentheses (`Eof()`) compiles.
   - Expected: an error. A case without data has no parentheses, and in the grammar `EnumType = ... Ident [ "(" Params ")" ]` with `Params` needs at least one `Param`.
   - Actual: `Eof()` is accepted as a case without data, used as `Token.Eof`, and `seed fmt` keeps the `()`.
     (`Token.Eof()` as a value is rejected correctly, see `spec/errors/enum_value_empty_parens`.)
   - Repro: `type Token enum {` / `Eof()` / `Num(int n)` / `}`.

3. **`select_receive_error_unhandled`**: in a `select` on a `chan error`, `case errs.receive():` drops the error without any message.
   - Expected: the "not handled" error that the statement `errs.receive()` gets ("Every error must be handled", and `receive()` on a `chan error` returns `error`; see `spec/errors/chan_receive_error_is_error`). `_ = errs.receive()` would discard it explicitly.
   - Actual: it compiles, and the received error is dropped.
   - Repro: `chan error errs = chan error(1)` / `errs.send(null)` / `select {` / `case errs.receive():` / `return null` / `}`.

4. **`fn_value_nullable_hint`**: the hint for `fn(int) int? f = null` suggests a type that doesn't exist.
   - Expected: a hint naming a valid type, `(fn(int) int?)?`. The spec says a nullable function type needs parentheses, and that `T??` is not a type.
   - Actual: `hint: declare it as fn(int) int?? if it may be missing`.
   - Repro: `fn(int) int? f = null`.

5. **`identifier_non_ascii_message`**: a non-ASCII character in the source is reported once per UTF-8 byte, as a broken character.
   - Expected: one error per character that shows the character ("every error message shows the code it's about"). With `expect-errors: 2`, that is one error per line, for example `unexpected character 'é'`.
   - Actual: `unexpected character '\xc3'` and `unexpected character '\xa9'` (the raw bytes, which show as `�`), four errors for two `é`.
   - Repro: `int café = 1`.

Other observations, not pinned by a test because the right wording is the
compiler's choice:

- `0b101`, `0o17`, `1_000` and `2x` all get `invalid number literal (a float needs a '.', for example 1.0e9)`. That explanation is about floats, which none of them are.
- A map key of the wrong type is reported twice, once for the type and once for the literal (`spec/errors/literal_map_float_key`). A local named like an imported package also reports the import as unused (`spec/errors/shadow_imported_package`).

## Spec problems

In each case the implementation is sensible, but the spec is unclear, wrong
or silent. The tests pin what the implementation does.

1. **File reads as stop points.** SPEC "Concurrency" says "File I/O (`fs`) is not a stop point", and stdlib.md says the same. But `full/fs_file_stop` checks that reading an `fs.File` (a FIFO) *is* a stop point, and stdlib.md's "Handles" note says waiting for a handle's operation lock is one too.
   Proposed: "Reads of an `fs.File` that can wait (a FIFO, a device) are stop points like stdin; operations on regular files don't block for long, so they are not."
2. **Comparing nullable values.** The Operators table says `==` works on every type. But `int? n` then `n == 2` is rejected: "can only be compared with null" (`core/errors/compare_nullable`, used in `spec/fn_type_nullable`). Nullable values inside lists and structs do compare.
   Proposed, in "Null": "A `T?` can be compared with `null`; to compare it with a value, narrow it first (`n != null && n == 2`). Nullable elements and fields of lists, maps, structs and enums compare as part of their container."
3. **Map values cannot be nullable.** `map[string](string?)` is rejected (`full/errors/map_nullable_value`), but neither SPEC nor builtins.md says so. Channels have the same rule, and the spec does state it for them.
   Proposed, in "Maps": "Values cannot be nullable (`map[K](V?)` is rejected): `m[k]` already uses null for a missing key."
4. **Fields and elements beside an error.** "Variables and constants" says a target without a type can be "a field or element", and the error guards cover variables "declared or assigned beside an error". But `b.s, err = f()` and `xs[0], err = f()` are rejected for a plain `T` (`full/errors/guard_field_target`, `spec/errors/error_guard_element_target`). A `T?` field is fine (`spec/error_guard_nullable_field_target`).
   Proposed, in "Errors": "Only a local variable can receive a plain `T` beside an error; a field or element cannot, because it would hold null until the check (assign to a local, check the error, then store it)."
5. **Mut methods on the narrowed value itself.** "Narrowing ends ... when a `mut` method is called on it". But the compiler keeps `xs` narrowed after `xs.push(1)`, and `c` after `c.bump()` (`spec/null_narrowing_kept`). That is right: inside a `mut` method the receiver is a plain `T`, so the method can't make it null.
   Proposed: "...or when a `mut` method is called on a part of the path above it (`u.reset()` ends the narrowing of `u.email`; `xs.push(x)` or `c.bump()` on the narrowed `xs` or `c` itself does not)."
6. **Select on a `chan error`.** SPEC "Select" lists `T? v = ch.receive()` as the receive case with a variable. For a `chan error` the form is `error e = errs.receive()`, which works (`spec/select_receive_error`) but isn't listed.
   Proposed: "...or `T? v = ch.receive()` (`error e = ch.receive()` for a `chan error`). On a `chan error`, `case ch.receive():` must be written `case _ = ch.receive():`" (see bug 3).
7. **Grammar productions miss forms the spec allows.**
   - `Defer = "defer" Expr` doesn't allow `defer _ = f.close()`, which SPEC requires.
   - `SelectClause = "case" ( VarDecl | Expr )` doesn't allow `_ = ch.receive()`, because `VarDecl` needs a typed target and this is an `Assign`.
   - Proposed: `Defer = "defer" [ "_" "=" ] Expr .` and `SelectClause = "case" ( VarDecl | Assign | Expr ) ":" ...`.
8. **The continuation rule.** "Syntax basics" says "A line that continues must end with an operator, a comma or an opening bracket". grammar.md's token rule is wider: `int x =` / `5` and `xs.` / `len()` both continue (`spec/newlines_continuation`).
   Proposed: "A line continues when its last token can't end a statement (an operator, `=`, `.`, a comma, an opening bracket; see grammar.md)."
9. **Leading zeros.** "No octal" leaves `017` open. It is the decimal 17, while Go and C read it as 15.
   Proposed: "A leading `0` has no meaning: `017` is 17."
10. **Identifiers are ASCII.** grammar.md says "a letter", but `café` is rejected (`spec/errors/identifier_non_ascii_letter`).
    Proposed: "an ASCII letter (`a`–`z`, `A`–`Z`) or `_`, followed by ASCII letters, digits or `_`."
11. **Built-in type names.** grammar.md calls `int float bool byte string error` "ordinary identifiers", but declaring one as a name is an error (`full/errors/builtin_type_name`, `full/errors/builtin_name_function`). The shadowing rule's list (locals, parameters, package-level names, imported packages) doesn't include them either.
    Proposed: "...are predeclared identifiers that refer to built-in types; like every name, they can't be declared again."
12. **`error?`.** It is rejected ("error is already nullable"), but only `T??` is mentioned (`spec/errors/types_error_nullable`).
    Proposed, in "Errors": "`error?` is not a type: `error` is already nullable."
13. **Crash conditions for list methods.** builtins.md gives them for strings ("Crashes unless `0 <= a <= b <= len`"). For lists it gives only `0 <= i <= len` for `insert`, and nothing for `slice`. The implementation crashes for all of these: `insert index out of range`, `slice bounds out of range`, `index out of range` for `xs[i] = v` (`spec/list_*_crash`).
    Proposed: "`xs.slice(a, b)`: crashes unless `0 <= a <= b <= len`. `xs.insert(i, x)`: crashes unless `0 <= i <= len`. `xs[i] = v`: crashes when out of range (it never grows the list)."
14. **Shifts.** Several things aren't specified:
    - A negative shift count crashes (`crash: negative shift count`).
    - `byte` shifts keep the low 8 bits.
    - `>>` floors on negative ints.
    - The count has the operand's type (`b << byte(1)`).
    - See `spec/op_shift_*`.
    - Proposed, in "Operators": "`x << n` and `x >> n` take `n` of the same type as `x`; a negative `n` crashes. On `int`, `>>` rounds toward negative infinity; on `byte`, the result keeps the low 8 bits."
15. **A project package with a standard library name.** SPEC says it "can't be imported (the standard library wins)". The parenthesis suggests the import silently gets the standard library, but it is an error at the import (`full/errors/pkg_stdlib_name`).
    Proposed: "...is an error at the import; rename the project's package."
16. **Unstated limits.**
    - `m[k]++` is rejected because the key may be missing (`full/errors/map_entry_incdec`), although `m[k]` is an `LValue` in `IncDec`.
    - Nesting deeper than 256 levels is an error (`full/errors/nested_too_deeply`).
    - Proposed: "`m[k]++` and `m[k]--` are not allowed (the entry may be missing); write `m[k] = v`." Also list the nesting limit under "Compiler".
17. **A function field and a method of the same name.** "(when the struct has no method of that name)" doesn't say what happens when there is one. The method wins for `j.run()`, and `j.run` without a call is still the field (`spec/fn_field_and_method_same_name`).
    Proposed: "If the struct also has a method of that name, `op.apply(...)` calls the method; the field can still be read as a value (`fn(int, int) int f = op.apply`)."
18. **Which types a `switch` takes.** SPEC says what a `switch` over an enum or over `int`, `byte`, `string` or `bool` must do, but not that other types (`float`, structs, lists) can't be switched on. The compiler says "switch works on enums, int, byte, string and bool" (`spec/errors/switch_struct_value`).
    Proposed: "A `switch` is over an enum, `int`, `byte`, `string` or `bool`."
19. **How a failed `assert` shows bytes.** "the value of each side" for bytes prints numbers: `b == 'b' (97 == 98)` (`spec/testcmd/rules`). That matches `byte.string()`, but a reader might expect `'a'`.
    Proposed: "...the value of each side (as `.string()` gives it: a byte as its number)".

## Resolution

The decisions were made by the project manager; DECISIONS.md records them.
All tests listed above pass; `tests/spec/pending/` no longer exists.

### Implementation bugs

| # | Test | Resolution |
|---|---|---|
| 1 | `errors/switch_default_not_last` | Fixed: a `case` after `default` in a value switch is an error ("default must be the last case of a switch"). |
| 2 | `errors/enum_case_empty_parens_decl` | Fixed: `Eof()` in an enum declaration is a syntax error with the hint `write Eof`; the AST no longer records the parentheses, so the formatter never sees them. |
| 3 | `errors/select_receive_error_unhandled` | Fixed: `case errs.receive():` on a `chan error` is an unhandled error; the hint gives `case error e = ...:` and `case _ = ...:`. |
| 4 | `errors/fn_value_nullable_hint` | Fixed: the hint names `(fn(int) int?)?` (also in the hint for a value beside a discarded error). |
| 5 | `errors/identifier_non_ascii_message` | Fixed: a non-ASCII character is reported once, as the whole UTF-8 character; a byte that isn't valid UTF-8 (or a control character) is reported once as `0xHH` (new: `errors/source_invalid_utf8_byte`). |

### Spec problems

| # | Resolution |
|---|---|
| 1 | Spec fixed (SPEC "Concurrency", stdlib.md, builtins.md, guide, README): reading an `fs.File` and waiting for a handle's operation lock are stop points; the other file operations are not. |
| 2 | Spec fixed ("Null", Operators table): a `T?` compares only with `null`; narrow first. |
| 3 | Spec fixed ("Maps", builtins.md, grammar.md): map values can't be nullable. |
| 4 | Spec fixed ("Errors", "Variables and constants"): only a local variable receives a plain `T` beside an error. |
| 5 | Spec fixed ("Null", guide): a `mut` method ends the narrowing of the paths under its receiver, not of the receiver itself. |
| 6 | Spec fixed ("Select", builtins.md): `error e = ch.receive()` and `_ = ch.receive()` for a `chan error` (with bug 3). |
| 7 | grammar.md fixed: `Defer = "defer" [ "_" "=" ] Expr` and `SelectClause = "case" ( VarDecl \| Assign \| Expr ) ...`. |
| 8 | SPEC's summary aligned with grammar.md's token rule. |
| 9 | **Implementation changed:** a number that starts with `0` followed by a digit (`017`, `00`, also `01.5`) is an error with the hint `write 17`. Tests: `errors/literal_leading_zero`, `errors/literal_leading_zeros_only`, `errors/literal_float_leading_zero`, `literal_zero_forms`. |
| 10 | Spec fixed (grammar.md, SPEC "Syntax basics"): identifiers are ASCII. |
| 11 | grammar.md fixed: built-in type names are predeclared identifiers that can't be declared again. |
| 12 | Spec fixed ("Types", "Errors", grammar.md): `error?` is not a type. |
| 13 | Spec fixed (builtins.md, SPEC "Lists"): crash conditions of `xs[i] = v`, `insert`, `remove` and `slice`. |
| 14 | Spec fixed ("Operators", builtins.md): shift count type, negative count crashes, `>>` floors on `int`, `byte` keeps the low 8 bits. |
| 15 | Spec fixed ("Packages"): such an import is an error; rename the package. |
| 16 | Spec fixed ("Maps", builtins.md, grammar.md, "Compiler"): `m[k]++` is rejected; nesting is limited to 256 levels. |
| 17 | **Implementation changed:** a struct can't have a field and a method with the same name (compile error at the method). `spec/fn_field_and_method_same_name` became `errors/struct_field_and_method_same_name`; `full/pkg_basic`'s `Counter.name` field was renamed `label`. |
| 18 | Spec fixed ("Control flow"): a `switch` is over an enum, `int`, `byte`, `string` or `bool`. |
| 19 | Spec fixed ("Tests"): the sides of a failed `assert` are shown as `.string()` gives them, a byte as its number. |
