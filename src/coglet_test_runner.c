#include <stdio.h>

typedef struct {
    const char *name;
    const char *source;
    int expect_error;
} TestCase;

int run_case(const TestCase *tc);

static const TestCase tests[] = {

    // ---------- positive ----------
    { "var_decl_and_arith",  "x : i32; x = 1 + 2 * 3;", 0 },
    { "compound_assign",     "i := 0; i += 1;", 0 },
    { "two_param_func",      "foo :: (a:i32, b:i32) { }", 0 },
    { "chained_assign",      "a := 0; b := 0; c := 0; a = b = c;", 0 },

    // exercises the pre-pass -- neither of these worked before it existed
    { "forward_call",        "main_fn :: () { helper(); } helper :: () { }", 0 },
    { "mutual_structs",      "A :: struct { b: B*; } B :: struct { a: A*; }", 0 },

    // ---------- negative ----------
    { "break_outside_loop",    "break;", 1 },
    { "continue_outside_loop", "continue;", 1 },
    { "return_outside_func",   "return 1;", 1 },
    { "duplicate_var",         "x : i32; x : i32;", 1 },
    { "duplicate_func",        "f :: () { } f :: () { }", 1 },
    { "duplicate_struct",      "S :: struct { } S :: struct { }", 1 },
    { "duplicate_field",       "S :: struct { x: i32; x: i32; }", 1 },
    { "undefined_ident",       "x = y;", 1 },
    { "unknown_struct_type",   "p : Nope;", 1 },
    { "wrong_arg_count",       "f :: (a:i32) { } f(1, 2);", 1 },
    { "wrong_arg_type",        "f :: (a:i32) { } f(3.14);", 1 },
    { "bad_pointer_init",      "p : i32* = 5;", 1 },      // only literal 0 is a null constant
    { "non_bool_if_cond",      "x : i32; if (x) { }", 1 },
    { "non_bool_while_cond",   "x : i32; while (x) { }", 1 },
    { "unknown_field",         "S :: struct { x: i32; } s : S; y := s.nope;", 1 },
    { "field_on_nonstruct",    "x : i32; y := x.nope;", 1 },
    { "assign_to_call",        "f :: () { } f() = 1;", 1 },   // not an lvalue
};

int main(void) {
    int total = sizeof(tests) / sizeof(tests[0]);
    int passed = 0;
    for (int i = 0; i < total; i++) passed += run_case(&tests[i]);
    printf("\n%d / %d test cases behaved as expected\n", passed, total);
    return passed == total ? 0 : 1;
}