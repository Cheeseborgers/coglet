#include "ir/cog_ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(const char *message)
{
    fprintf(stderr, "cog-ir-core: %s\n", message);
    return 1;
}

static int dump_contains(const CogIrModule *module, const char *needle)
{
    FILE *file = tmpfile();
    if (!file)
        return 0;

    cog_ir_dump(file, module);
    if (fflush(file) != 0 || fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }

    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }

    char *text = malloc((size_t)size + 1);
    if (!text) {
        fclose(file);
        return 0;
    }

    size_t read = fread(text, 1, (size_t)size, file);
    text[read] = '\0';
    int found = strstr(text, needle) != NULL;
    free(text);
    fclose(file);
    return found;
}

static CogIrInstruction instruction(CogIrOp op, CogIrTypeId result_type, SourceSpan span)
{
    CogIrInstruction value;
    memset(&value, 0, sizeof(value));
    value.op = op;
    value.result = COG_IR_VALUE_INVALID;
    value.result_type = result_type;
    value.span = span;
    return value;
}

static CogIrTerminator ret_value(CogIrValueId value, SourceSpan span)
{
    CogIrTerminator term;
    memset(&term, 0, sizeof(term));
    term.kind = COG_IR_TERMINATOR_RET;
    term.span = span;
    term.as.ret.has_value = 1;
    term.as.ret.value = value;
    return term;
}

static CogIrTerminator ret_void(SourceSpan span)
{
    CogIrTerminator term;
    memset(&term, 0, sizeof(term));
    term.kind = COG_IR_TERMINATOR_RET;
    term.span = span;
    term.as.ret.has_value = 0;
    term.as.ret.value = COG_IR_VALUE_INVALID;
    return term;
}

int main(void)
{
    TargetInfo target = target_info_host();
    CogIrModule module;
    cog_ir_module_init(&module, &target);

    const char *source0 =
        "counter: s32 = 1;\n"
        "bump::(x: s32) -> s32 { return x + 1; }\n";
    const char *source1 = "foreign declaration\n";

    SourceFileId file0 = SOURCE_FILE_ID_INVALID;
    SourceFileId file1 = SOURCE_FILE_ID_INVALID;
    if (!cog_ir_module_add_source(&module, "main.cog", source0, &file0) ||
        !cog_ir_module_add_source(&module, "ffi.cog", source1, &file1) ||
        file0 == file1)
        return fail("multi-file source ownership failed");

    SourceSpan global_span = source_span_make(file0, 0, 18, 1, 1);
    SourceSpan function_span = source_span_make(file0, 19, strlen(source0), 2, 1);

    CogIrTypeId void_type = cog_ir_type_void(&module);
    CogIrTypeId bool_type = cog_ir_type_bool(&module);
    CogIrTypeId s32_type = cog_ir_type_integer(&module, 32, 1);
    CogIrTypeId s32_again = cog_ir_type_integer(&module, 32, 1);
    CogIrTypeId u32_type = cog_ir_type_integer(&module, 32, 0);
    CogIrTypeId ptr_s32 = cog_ir_type_pointer(&module, s32_type, 0, 0);

    if (void_type == COG_IR_TYPE_INVALID || bool_type == COG_IR_TYPE_INVALID ||
        s32_type == COG_IR_TYPE_INVALID || s32_type != s32_again ||
        u32_type == s32_type || ptr_s32 == COG_IR_TYPE_INVALID)
        return fail("structural type interning failed");

    CogIrTypeId node_a = cog_ir_declare_nominal_type(
        &module, COG_IR_TYPE_STRUCT, string_view_from_cstr("Node"), function_span);
    CogIrTypeId node_b = cog_ir_declare_nominal_type(
        &module, COG_IR_TYPE_STRUCT, string_view_from_cstr("Node"), function_span);
    if (node_a == COG_IR_TYPE_INVALID || node_b == COG_IR_TYPE_INVALID || node_a == node_b)
        return fail("nominal type identity failed");

    CogIrTypeId ptr_node = cog_ir_type_pointer(&module, node_a, 0, 0);
    CogIrAggregateField node_fields[2];
    memset(node_fields, 0, sizeof(node_fields));
    node_fields[0].debug_name = string_view_from_cstr("value");
    node_fields[0].type = s32_type;
    node_fields[0].abi_type = COG_IR_ABI_TYPE_INVALID;
    node_fields[0].span = function_span;
    node_fields[1].debug_name = string_view_from_cstr("next");
    node_fields[1].type = ptr_node;
    node_fields[1].abi_type = COG_IR_ABI_TYPE_INVALID;
    node_fields[1].span = function_span;
    if (!cog_ir_define_aggregate_type(&module, node_a, node_fields, 2, 0, 0, 0))
        return fail("recursive aggregate definition failed");

    if (!cog_ir_mark_incomplete_aggregate_type(&module, node_b))
        return fail("incomplete repr(c) aggregate state failed");

    CogIrAbiTypeId c_int = cog_ir_abi_type_c_scalar(&module, s32_type, COG_IR_C_SCALAR_INT);
    if (c_int == COG_IR_ABI_TYPE_INVALID ||
        c_int != cog_ir_abi_type_c_scalar(&module, s32_type, COG_IR_C_SCALAR_INT))
        return fail("ABI type interning failed");

    CogIrConstId zero_s32 = cog_ir_const_zero(&module, s32_type);
    CogIrConstId one_s32 = cog_ir_const_integer(&module, s32_type, 1);
    if (zero_s32 == COG_IR_CONST_INVALID || one_s32 == COG_IR_CONST_INVALID)
        return fail("constant builder failed");

    CogIrGlobalId counter = cog_ir_add_global(
        &module,
        string_view_from_cstr("counter"),
        global_span,
        s32_type,
        COG_IR_ABI_TYPE_INVALID,
        COG_IR_LINKAGE_INTERNAL,
        0,
        0,
        zero_s32
    );
    if (counter == COG_IR_GLOBAL_INVALID)
        return fail("global builder failed");

    CogIrTypeId bump_params[] = { s32_type };
    CogIrTypeId bump_type = cog_ir_type_function(
        &module, s32_type, bump_params, 1,
        COG_IR_ABI_COGLET, COG_IR_CALL_DEFAULT, 0);
    CogIrFunctionId bump = cog_ir_add_function(
        &module,
        string_view_from_cstr("bump"),
        function_span,
        bump_type,
        COG_IR_FUNCTION_DEFINITION,
        COG_IR_LINKAGE_INTERNAL,
        0,
        NULL
    );
    if (bump == COG_IR_FUNCTION_INVALID)
        return fail("function builder failed");

    const CogIrFunction *bump_fn = cog_ir_get_function(&module, bump);
    if (!bump_fn || bump_fn->parameter_count != 1)
        return fail("function parameter values were not created");
    CogIrValueId x = bump_fn->parameters[0];

    CogIrSlotId x_slot = cog_ir_add_slot(
        &module, bump, COG_IR_SLOT_SOURCE_PARAMETER, 0,
        string_view_from_cstr("x"), function_span, s32_type);
    CogIrBlockId entry = cog_ir_add_block(
        &module, bump, string_view_from_cstr("entry"), function_span);
    if (x_slot == COG_IR_SLOT_INVALID || entry == COG_IR_BLOCK_INVALID)
        return fail("slot/block builder failed");

    CogIrInstruction op = instruction(COG_IR_OP_LOCAL_ADDR, ptr_s32, function_span);
    op.as.local_addr.slot = x_slot;
    CogIrValueId x_addr = COG_IR_VALUE_INVALID;
    if (!cog_ir_emit(&module, bump, entry, &op, &x_addr)) return fail("local_addr emission failed");

    op = instruction(COG_IR_OP_STORE, COG_IR_TYPE_INVALID, function_span);
    op.as.store.address = x_addr;
    op.as.store.value = x;
    op.as.store.is_volatile = 0;
    if (!cog_ir_emit(&module, bump, entry, &op, NULL)) return fail("store emission failed");

    op = instruction(COG_IR_OP_CONST, s32_type, function_span);
    op.as.constant.constant = one_s32;
    CogIrValueId one = COG_IR_VALUE_INVALID;
    if (!cog_ir_emit(&module, bump, entry, &op, &one)) return fail("const emission failed");

    op = instruction(COG_IR_OP_LOAD, s32_type, function_span);
    op.as.load.address = x_addr;
    op.as.load.is_volatile = 0;
    CogIrValueId loaded = COG_IR_VALUE_INVALID;
    if (!cog_ir_emit(&module, bump, entry, &op, &loaded)) return fail("load emission failed");

    op = instruction(COG_IR_OP_IADD_CHECKED, s32_type, function_span);
    op.as.binary.lhs = loaded;
    op.as.binary.rhs = one;
    CogIrValueId added = COG_IR_VALUE_INVALID;
    if (!cog_ir_emit(&module, bump, entry, &op, &added)) return fail("add emission failed");

    CogIrTerminator term = ret_value(added, function_span);
    if (x_addr == COG_IR_VALUE_INVALID || one == COG_IR_VALUE_INVALID ||
        loaded == COG_IR_VALUE_INVALID || added == COG_IR_VALUE_INVALID ||
        !cog_ir_set_terminator(&module, bump, entry, &term))
        return fail("instruction/terminator builder failed");

    CogIrTypeId main_type = cog_ir_type_function(
        &module, s32_type, NULL, 0,
        COG_IR_ABI_COGLET, COG_IR_CALL_DEFAULT, 0);
    CogIrFunctionId main_function = cog_ir_add_function(
        &module, string_view_from_cstr("main"), function_span, main_type,
        COG_IR_FUNCTION_DEFINITION, COG_IR_LINKAGE_INTERNAL, 0, NULL);
    CogIrBlockId main_entry = cog_ir_add_block(
        &module, main_function, string_view_from_cstr("entry"), function_span);
    op = instruction(COG_IR_OP_CONST, s32_type, function_span);
    op.as.constant.constant = one_s32;
    CogIrValueId main_result = COG_IR_VALUE_INVALID;
    if (!cog_ir_emit(&module, main_function, main_entry, &op, &main_result))
        return fail("entry result emission failed");
    term = ret_value(main_result, function_span);
    if (main_function == COG_IR_FUNCTION_INVALID || main_entry == COG_IR_BLOCK_INVALID ||
        main_result == COG_IR_VALUE_INVALID ||
        !cog_ir_set_terminator(&module, main_function, main_entry, &term) ||
        !cog_ir_set_entry_function(&module, main_function))
        return fail("entry function builder failed");

    CogIrTypeId init_type = cog_ir_type_function(
        &module, void_type, NULL, 0,
        COG_IR_ABI_COGLET, COG_IR_CALL_DEFAULT, 0);
    CogIrFunctionId init = cog_ir_add_function(
        &module,
        string_view_from_cstr("<module-init>"),
        source_span_invalid(),
        init_type,
        COG_IR_FUNCTION_DEFINITION,
        COG_IR_LINKAGE_INTERNAL,
        1,
        NULL
    );
    CogIrBlockId init_entry = cog_ir_add_block(
        &module, init, string_view_from_cstr("entry"), source_span_invalid());

    op = instruction(COG_IR_OP_GLOBAL_ADDR, ptr_s32, global_span);
    op.as.global_addr.global = counter;
    CogIrValueId counter_addr = COG_IR_VALUE_INVALID;
    if (!cog_ir_emit(&module, init, init_entry, &op, &counter_addr)) return fail("global_addr emission failed");

    op = instruction(COG_IR_OP_CONST, s32_type, global_span);
    op.as.constant.constant = one_s32;
    CogIrValueId init_one = COG_IR_VALUE_INVALID;
    if (!cog_ir_emit(&module, init, init_entry, &op, &init_one)) return fail("init const emission failed");

    op = instruction(COG_IR_OP_STORE, COG_IR_TYPE_INVALID, global_span);
    op.as.store.address = counter_addr;
    op.as.store.value = init_one;
    op.as.store.is_volatile = 0;
    if (!cog_ir_emit(&module, init, init_entry, &op, NULL)) return fail("init store emission failed");

    term = ret_void(source_span_invalid());
    if (init == COG_IR_FUNCTION_INVALID || init_entry == COG_IR_BLOCK_INVALID ||
        counter_addr == COG_IR_VALUE_INVALID || init_one == COG_IR_VALUE_INVALID ||
        !cog_ir_set_terminator(&module, init, init_entry, &term) ||
        !cog_ir_set_init_function(&module, init))
        return fail("module initializer builder failed");

    CogIrFunctionId late_defined = cog_ir_add_function(
        &module, string_view_from_cstr("late_defined"), function_span, init_type,
        COG_IR_FUNCTION_DECLARATION, COG_IR_LINKAGE_INTERNAL, 0, NULL);
    if (late_defined == COG_IR_FUNCTION_INVALID ||
        !cog_ir_begin_function_definition(&module, late_defined))
        return fail("function predeclaration upgrade failed");
    CogIrBlockId late_entry = cog_ir_add_block(
        &module, late_defined, string_view_from_cstr("entry"), function_span);
    term = ret_void(function_span);
    if (late_entry == COG_IR_BLOCK_INVALID ||
        !cog_ir_set_terminator(&module, late_defined, late_entry, &term))
        return fail("upgraded function body construction failed");

    Arena *diag_arena = arena_create(4096);
    DiagnosticList diagnostics;
    diagnostic_list_init(&diagnostics, diag_arena);

    cog_ir_module_freeze(&module);
    if (!cog_ir_verify(&module, &diagnostics) || diagnostics.count != 0)
        return fail("valid manually-built CogIR module did not verify");

    if (cog_ir_type_integer(&module, 64, 1) != COG_IR_TYPE_INVALID)
        return fail("frozen module still accepted builder mutation");

    if (!dump_contains(&module, "global @g0 \"counter\" : s32") ||
        !dump_contains(&module, "iadd.checked") ||
        !dump_contains(&module, "entry @f1") ||
        !dump_contains(&module, "init @f2") ||
        !dump_contains(&module, "type %t"))
        return fail("deterministic CogIR dump is missing expected entities");

    arena_destroy(diag_arena);
    cog_ir_module_destroy(&module);

    /* Negative verifier regression: declared nominal type + unterminated body. */
    CogIrModule bad;
    cog_ir_module_init(&bad, &target);
    CogIrTypeId bad_void = cog_ir_type_void(&bad);
    (void)cog_ir_declare_nominal_type(
        &bad, COG_IR_TYPE_STRUCT, string_view_from_cstr("Forgotten"), source_span_invalid());
    CogIrTypeId bad_fn_type = cog_ir_type_function(
        &bad, bad_void, NULL, 0, COG_IR_ABI_COGLET, COG_IR_CALL_DEFAULT, 0);
    CogIrFunctionId bad_fn = cog_ir_add_function(
        &bad, string_view_from_cstr("broken"), source_span_invalid(), bad_fn_type,
        COG_IR_FUNCTION_DEFINITION, COG_IR_LINKAGE_INTERNAL, 0, NULL);
    (void)cog_ir_add_block(&bad, bad_fn, string_view_from_cstr("entry"), source_span_invalid());
    cog_ir_module_freeze(&bad);

    diag_arena = arena_create(4096);
    diagnostic_list_init(&diagnostics, diag_arena);
    if (cog_ir_verify(&bad, &diagnostics) || diagnostics.count < 2)
        return fail("verifier did not reject unfinished nominal type/CFG");

    arena_destroy(diag_arena);
    cog_ir_module_destroy(&bad);

    /* Negative verifier regression: the resolved executable entry has a
     * backend-neutral Coglet () -> s32 contract. */
    CogIrModule bad_entry;
    cog_ir_module_init(&bad_entry, &target);
    CogIrTypeId bad_entry_s32 = cog_ir_type_integer(&bad_entry, 32, 1);
    CogIrTypeId bad_entry_params[] = { bad_entry_s32 };
    CogIrTypeId bad_entry_type = cog_ir_type_function(
        &bad_entry, bad_entry_s32, bad_entry_params, 1,
        COG_IR_ABI_COGLET, COG_IR_CALL_DEFAULT, 0);
    CogIrFunctionId bad_entry_fn = cog_ir_add_function(
        &bad_entry, string_view_from_cstr("main"), source_span_invalid(), bad_entry_type,
        COG_IR_FUNCTION_DEFINITION, COG_IR_LINKAGE_INTERNAL, 0, NULL);
    CogIrBlockId bad_entry_block = cog_ir_add_block(
        &bad_entry, bad_entry_fn, string_view_from_cstr("entry"), source_span_invalid());
    const CogIrFunction *bad_entry_function = cog_ir_get_function(&bad_entry, bad_entry_fn);
    if (!bad_entry_function || bad_entry_function->parameter_count != 1)
        return fail("negative entry parameter creation failed");
    term = ret_value(bad_entry_function->parameters[0], source_span_invalid());
    if (bad_entry_block == COG_IR_BLOCK_INVALID ||
        !cog_ir_set_terminator(&bad_entry, bad_entry_fn, bad_entry_block, &term) ||
        !cog_ir_set_entry_function(&bad_entry, bad_entry_fn))
        return fail("negative entry construction failed");
    cog_ir_module_freeze(&bad_entry);

    diag_arena = arena_create(4096);
    diagnostic_list_init(&diagnostics, diag_arena);
    if (cog_ir_verify(&bad_entry, &diagnostics) || diagnostics.count == 0)
        return fail("verifier did not reject non-() -> s32 module entry");

    arena_destroy(diag_arena);
    cog_ir_module_destroy(&bad_entry);

    /* Negative verifier regression: C variadic tails must already carry the
     * target default argument promotions before the call instruction. */
    CogIrModule bad_vararg;
    cog_ir_module_init(&bad_vararg, &target);
    CogIrTypeId vararg_void = cog_ir_type_void(&bad_vararg);
    CogIrTypeId vararg_bool = cog_ir_type_bool(&bad_vararg);
    CogIrTypeId vararg_int = cog_ir_type_integer(
        &bad_vararg, target.c_int_bits, 1);
    CogIrTypeId vararg_params[] = { vararg_int };
    CogIrTypeId variadic_type = cog_ir_type_function(
        &bad_vararg, vararg_void, vararg_params, 1,
        COG_IR_ABI_C, COG_IR_CALL_DEFAULT, 1);
    CogIrAbiTypeId vararg_void_abi = cog_ir_abi_type_semantic(&bad_vararg, vararg_void);
    CogIrAbiTypeId vararg_int_abi = cog_ir_abi_type_c_scalar(
        &bad_vararg, vararg_int, COG_IR_C_SCALAR_INT);
    CogIrAbiTypeId variadic_param_abis[] = { vararg_int_abi };
    CogIrFunctionAbi variadic_abi = {
        .abi = COG_IR_ABI_C,
        .calling_convention = COG_IR_CALL_DEFAULT,
        .is_variadic = 1,
        .return_abi_type = vararg_void_abi,
        .parameter_abi_types = variadic_param_abis,
        .parameter_count = 1,
    };
    CogIrFunctionId variadic = cog_ir_add_function(
        &bad_vararg, string_view_from_cstr("variadic"), source_span_invalid(),
        variadic_type, COG_IR_FUNCTION_DECLARATION, COG_IR_LINKAGE_EXTERNAL, 0, &variadic_abi);
    CogIrTypeId caller_type = cog_ir_type_function(
        &bad_vararg, vararg_void, NULL, 0,
        COG_IR_ABI_COGLET, COG_IR_CALL_DEFAULT, 0);
    CogIrFunctionId caller = cog_ir_add_function(
        &bad_vararg, string_view_from_cstr("caller"), source_span_invalid(),
        caller_type, COG_IR_FUNCTION_DEFINITION, COG_IR_LINKAGE_INTERNAL, 0, NULL);
    CogIrBlockId caller_entry = cog_ir_add_block(
        &bad_vararg, caller, string_view_from_cstr("entry"), source_span_invalid());

    op = instruction(COG_IR_OP_FUNCTION_REF, variadic_type, source_span_invalid());
    op.as.function_ref.function = variadic;
    CogIrValueId variadic_ref = COG_IR_VALUE_INVALID;
    if (!cog_ir_emit(&bad_vararg, caller, caller_entry, &op, &variadic_ref))
        return fail("negative variadic function_ref emission failed");
    CogIrAbiTypeId variadic_callback_abi = cog_ir_abi_type_function(
        &bad_vararg, variadic_type, vararg_void_abi, variadic_param_abis, 1);
    if (variadic_callback_abi == COG_IR_ABI_TYPE_INVALID ||
        !cog_ir_set_value_abi_type(&bad_vararg, caller, variadic_ref, variadic_callback_abi))
        return fail("negative variadic callback ABI annotation failed");

    CogIrConstId marker_const = cog_ir_const_integer(&bad_vararg, vararg_int, 1);
    op = instruction(COG_IR_OP_CONST, vararg_int, source_span_invalid());
    op.as.constant.constant = marker_const;
    CogIrValueId marker_value = COG_IR_VALUE_INVALID;
    if (!cog_ir_emit(&bad_vararg, caller, caller_entry, &op, &marker_value))
        return fail("negative variadic marker emission failed");

    CogIrConstId bool_const = cog_ir_const_bool(&bad_vararg, vararg_bool, 1);
    op = instruction(COG_IR_OP_CONST, vararg_bool, source_span_invalid());
    op.as.constant.constant = bool_const;
    CogIrValueId bool_value = COG_IR_VALUE_INVALID;
    if (!cog_ir_emit(&bad_vararg, caller, caller_entry, &op, &bool_value))
        return fail("negative variadic bool emission failed");

    CogIrValueId call_args[] = { marker_value, bool_value };
    op = instruction(COG_IR_OP_CALL, COG_IR_TYPE_INVALID, source_span_invalid());
    op.as.call.callee = variadic_ref;
    op.as.call.arguments = call_args;
    op.as.call.argument_count = 2;
    if (!cog_ir_emit(&bad_vararg, caller, caller_entry, &op, NULL))
        return fail("negative variadic call emission failed");

    term = ret_void(source_span_invalid());
    if (!cog_ir_set_terminator(&bad_vararg, caller, caller_entry, &term))
        return fail("negative variadic terminator emission failed");

    cog_ir_module_freeze(&bad_vararg);
    diag_arena = arena_create(4096);
    diagnostic_list_init(&diagnostics, diag_arena);
    if (cog_ir_verify(&bad_vararg, &diagnostics) || diagnostics.count == 0)
        return fail("verifier accepted an unpromoted C variadic argument");

    arena_destroy(diag_arena);
    cog_ir_module_destroy(&bad_vararg);

    puts("CogIR core builder/verifier/dump passed");
    return 0;
}
