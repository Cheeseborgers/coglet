#include "ir/cog_ir_lower.h"

#include <stdio.h>
#include <string.h>

static int fail(const char *message)
{
    fprintf(stderr, "CogIR metadata lowering check failed: %s\n", message);
    return 1;
}

static StringView decl_name(const Node *node)
{
    if (!node) return string_view_empty();
    switch (node->type) {
        case NODE_VAR_DECL: return node->as.var_decl.name;
        case NODE_FUNC_DECL: return node->as.func_decl.name;
        case NODE_FUNC_PARAM_DECL: return node->as.param_decl.name;
        case NODE_STRUCT_DECL: return node->as.struct_decl.name;
        case NODE_STRUCT_FIELD_DECL: return node->as.struct_field_decl.name;
        case NODE_ENUM_DECL: return node->as.enum_decl.name;
        case NODE_ENUM_MEMBER: return node->as.enum_member.name;
        case NODE_CONST_DECL: return node->as.const_decl.name;
        default: return string_view_empty();
    }
}

static int sv_equals_cstr(StringView value, const char *text)
{
    size_t length = strlen(text);
    return value.length == length && memcmp(value.data, text, length) == 0;
}

static const CogIrLowerDeclBinding *find_binding(CogIrLowerContext *lower, const char *name)
{
    SemanticContext *sem = (SemanticContext *)&lower->frontend->sem;
    for (SemDeclId id = 0; id < lower->decl_binding_count; ++id) {
        SemDeclInfo *info = semantic_get_decl_info_by_id(sem, id);
        if (info && sv_equals_cstr(decl_name(info->node), name))
            return cog_ir_lower_get_decl_binding(lower, id);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc != 2) return fail("expected one Coglet input path");

    TargetConfig target_config = target_config_host();

    CompileResult frontend;
    if (compile_parse_and_check(argv[1], &target_config, &frontend) != COMPILE_STATUS_OK)
        return fail("frontend rejected metadata fixture");

    CogIrModule module;
    cog_ir_module_init(&module, &frontend.target);
    Arena *diag_arena = arena_create(16384);
    DiagnosticList diagnostics;
    diagnostic_list_init(&diagnostics, diag_arena);

    CogIrLowerContext lower;
    if (!cog_ir_lower_context_init(&lower, &frontend, &module, &diagnostics) ||
        !cog_ir_lower_prepare_metadata(&lower)) {
        diagnostic_print_all(stderr, &module.sources, &diagnostics);
        return fail("metadata preparation failed");
    }

    if (module.sources.count != 1) return fail("source table was not copied into CogIR");

    const CogIrLowerDeclBinding *cnode = find_binding(&lower, "CNode");
    const CogIrType *cnode_type = cnode && cnode->kind == COG_IR_LOWER_DECL_TYPE
        ? cog_ir_get_type(&module, cnode->as.nominal_type) : NULL;
    if (!cnode_type || cnode_type->kind != COG_IR_TYPE_STRUCT ||
        !cnode_type->as.aggregate.is_complete || !cnode_type->as.aggregate.is_repr_c ||
        cnode_type->as.aggregate.field_count != 3)
        return fail("CNode aggregate metadata was lowered incorrectly");

    const CogIrLowerDeclBinding *next = find_binding(&lower, "next");
    if (!next || next->kind != COG_IR_LOWER_DECL_FIELD ||
        next->as.field.owner_type != cnode->as.nominal_type || next->as.field.index != 1)
        return fail("field declaration identity was not preserved");

    const CogIrLowerDeclBinding *foreign = find_binding(&lower, "ForeignHandle");
    const CogIrType *foreign_type = foreign ? cog_ir_get_type(&module, foreign->type) : NULL;
    if (!foreign_type || !foreign_type->as.aggregate.is_incomplete || !foreign_type->as.aggregate.is_repr_c)
        return fail("incomplete repr(c) aggregate metadata was not preserved");

    const CogIrLowerDeclBinding *mode = find_binding(&lower, "Mode");
    const CogIrType *mode_type = mode ? cog_ir_get_type(&module, mode->type) : NULL;
    if (!mode_type || mode_type->kind != COG_IR_TYPE_ENUM || !mode_type->as.enumeration.is_repr_c ||
        mode_type->as.enumeration.member_count != 3 || mode_type->as.enumeration.backing_abi_type == COG_IR_ABI_TYPE_INVALID)
        return fail("repr(c) enum metadata was not lowered correctly");

    const CogIrLowerDeclBinding *eight = find_binding(&lower, "Eight");
    const CogIrConstant *eight_c = eight && eight->kind == COG_IR_LOWER_DECL_ENUM_MEMBER
        ? cog_ir_get_constant(&module, eight->as.enum_member.constant) : NULL;
    if (!eight_c || eight->as.enum_member.index != 2 || eight_c->type != mode->type || eight_c->as.integer_bits != 8)
        return fail("enum member identity/value was not lowered correctly");

    const CogIrLowerDeclBinding *count = find_binding(&lower, "COUNT");
    const CogIrConstant *count_c = count ? cog_ir_get_constant(&module, count->as.constant) : NULL;
    if (!count_c || count_c->kind != COG_IR_CONST_INTEGER || count_c->as.integer_bits != 42)
        return fail("integer constant did not lower correctly");

    const CogIrLowerDeclBinding *neg = find_binding(&lower, "NEG");
    const CogIrConstant *neg_c = neg ? cog_ir_get_constant(&module, neg->as.constant) : NULL;
    if (!neg_c || neg_c->kind != COG_IR_CONST_INTEGER || neg_c->as.integer_bits != UINT64_C(0xfffe))
        return fail("negative fixed-width integer bits were not preserved");

    const CogIrLowerDeclBinding *ratio = find_binding(&lower, "RATIO");
    const CogIrConstant *ratio_c = ratio ? cog_ir_get_constant(&module, ratio->as.constant) : NULL;
    if (!ratio_c || ratio_c->kind != COG_IR_CONST_FLOAT32 || ratio_c->as.float32_bits != UINT32_C(0x3fa00000))
        return fail("f32 IEEE bits were not preserved");

    const CogIrLowerDeclBinding *enabled = find_binding(&lower, "ENABLED");
    const CogIrConstant *enabled_c = enabled ? cog_ir_get_constant(&module, enabled->as.constant) : NULL;
    if (!enabled_c || enabled_c->kind != COG_IR_CONST_BOOL || !enabled_c->as.boolean)
        return fail("bool constant did not lower correctly");

    const CogIrLowerDeclBinding *none = find_binding(&lower, "NONE");
    const CogIrConstant *none_c = none ? cog_ir_get_constant(&module, none->as.constant) : NULL;
    if (!none_c || none_c->kind != COG_IR_CONST_NULL)
        return fail("typed null constant did not lower correctly");

    const CogIrLowerDeclBinding *counter = find_binding(&lower, "counter");
    const CogIrGlobal *counter_g = counter ? cog_ir_get_global(&module, counter->as.global) : NULL;
    const CogIrConstant *counter_init = counter_g ? cog_ir_get_constant(&module, counter_g->static_initializer) : NULL;
    if (!counter_init || counter_init->kind != COG_IR_CONST_ZERO)
        return fail("source global was not zero-initialized in static data");

    const CogIrLowerDeclBinding *c_flag = find_binding(&lower, "c_flag");
    const CogIrGlobal *c_flag_g = c_flag ? cog_ir_get_global(&module, c_flag->as.global) : NULL;
    const CogIrAbiType *c_flag_abi = c_flag_g && c_flag_g->abi_type != COG_IR_ABI_TYPE_INVALID
        ? cog_ir_get_abi_type(&module, c_flag_g->abi_type) : NULL;
    if (!c_flag_abi || c_flag_abi->kind != COG_IR_ABI_TYPE_C_SCALAR ||
        c_flag_abi->c_scalar_kind != COG_IR_C_SCALAR_BOOL)
        return fail("exact c_bool global object spelling was not frozen into CogIR");

    const CogIrLowerDeclBinding *callback = find_binding(&lower, "callback");
    const CogIrType *callback_type = callback ? cog_ir_get_type(&module, callback->type) : NULL;
    if (!callback_type || callback_type->kind != COG_IR_TYPE_FUNCTION || callback_type->as.function.abi != COG_IR_ABI_C)
        return fail("cfn global type was not lowered correctly");

    const CogIrLowerDeclBinding *values = find_binding(&lower, "values");
    const CogIrType *values_type = values ? cog_ir_get_type(&module, values->type) : NULL;
    if (!values_type || values_type->kind != COG_IR_TYPE_ARRAY || values_type->as.array.length != 3)
        return fail("array type was not lowered correctly");

    const CogIrLowerDeclBinding *native = find_binding(&lower, "native_probe_decl");
    const CogIrFunction *native_fn = native ? cog_ir_get_function(&module, native->as.function) : NULL;
    if (!native_fn || native_fn->kind != COG_IR_FUNCTION_DECLARATION || native_fn->linkage != COG_IR_LINKAGE_EXTERNAL ||
        native_fn->abi.abi != COG_IR_ABI_C || !sv_equals_cstr(native_fn->abi.external_symbol, "native_probe") ||
        native_fn->abi.parameter_count != 3 || native_fn->abi.return_abi_type == COG_IR_ABI_TYPE_INVALID)
        return fail("extern(c) function metadata was not lowered correctly");
    const CogIrAbiType *amount_abi = cog_ir_get_abi_type(&module, native_fn->abi.parameter_abi_types[2]);
    if (!amount_abi || amount_abi->kind != COG_IR_ABI_TYPE_C_SCALAR || amount_abi->c_scalar_kind != COG_IR_C_SCALAR_LONG)
        return fail("exact c_long ABI spelling was lost");

    const CogIrLowerDeclBinding *helper = find_binding(&lower, "helper");
    const CogIrFunction *helper_fn = helper ? cog_ir_get_function(&module, helper->as.function) : NULL;
    if (!helper_fn || helper_fn->kind != COG_IR_FUNCTION_DECLARATION ||
        helper_fn->linkage != COG_IR_LINKAGE_INTERNAL)
        return fail("Coglet function was not predeclared with stable identity");

    const CogIrLowerDeclBinding *x = find_binding(&lower, "x");
    const CogIrLowerDeclBinding *local = find_binding(&lower, "local");
    if (!x || x->kind != COG_IR_LOWER_DECL_PARAMETER_PENDING || !local || local->kind != COG_IR_LOWER_DECL_LOCAL_PENDING)
        return fail("local/parameter declaration identities were not reserved");

    if (diagnostics.count != 0 || !cog_ir_verify(&module, &diagnostics))
        return fail("metadata-only module did not verify");

    cog_ir_lower_context_destroy(&lower);
    compile_result_destroy(&frontend);

    DiagnosticList after;
    diagnostic_list_init(&after, diag_arena);
    if (!cog_ir_verify(&module, &after) || after.count != 0)
        return fail("CogIR metadata retained frontend lifetime dependencies");
    const SourceFile *copied = source_manager_get(&module.sources, 0);
    if (!copied || !strstr(copied->source, "ForeignHandle::struct"))
        return fail("IR-owned source text did not survive frontend destruction");

    cog_ir_module_destroy(&module);
    arena_destroy(diag_arena);
    puts("CogIR metadata lowering verification passed");
    return 0;
}
