#include "backends/llvm/backend_llvm.h"
#include "diagnostic.h"
#include "utils/arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(const char *message)
{
    fprintf(stderr, "llvm-stage4-trap: %s\n", message);
    return 1;
}

static int file_contains(const char *path, const char *needle)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return 0;
    if (fseek(file, 0, SEEK_END) != 0) {
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

int main(int argc, char **argv)
{
    if (argc != 2)
        return fail("expected output LLVM IR path");

    TargetInfo target = target_info_host();
    CogIrModule module;
    cog_ir_module_init(&module, &target);

    CogIrTypeId i32 = cog_ir_type_integer(&module, 32, 1);
    CogIrTypeId entry_type = cog_ir_type_function(
        &module,
        i32,
        NULL,
        0,
        COG_IR_ABI_COGLET,
        COG_IR_CALL_DEFAULT,
        0
    );
    CogIrFunctionId entry = cog_ir_add_function(
        &module,
        string_view_from_cstr("trap_entry"),
        source_span_invalid(),
        entry_type,
        COG_IR_FUNCTION_DEFINITION,
        COG_IR_LINKAGE_INTERNAL,
        0,
        NULL
    );
    CogIrBlockId block = cog_ir_add_block(
        &module,
        entry,
        string_view_from_cstr("entry"),
        source_span_invalid()
    );
    CogIrTerminator trap;
    memset(&trap, 0, sizeof(trap));
    trap.kind = COG_IR_TERMINATOR_TRAP;
    trap.span = source_span_invalid();
    trap.as.trap.reason = COG_IR_TRAP_EXPLICIT;

    if (i32 == COG_IR_TYPE_INVALID || entry_type == COG_IR_TYPE_INVALID ||
        entry == COG_IR_FUNCTION_INVALID || block == COG_IR_BLOCK_INVALID ||
        !cog_ir_set_terminator(&module, entry, block, &trap) ||
        !cog_ir_set_entry_function(&module, entry)) {
        cog_ir_module_destroy(&module);
        return fail("could not construct trap CogIR module");
    }

    Arena *arena = arena_create(4096);
    if (!arena) {
        cog_ir_module_destroy(&module);
        return fail("could not allocate diagnostics arena");
    }
    DiagnosticList diagnostics;
    diagnostic_list_init(&diagnostics, arena);
    if (!cog_ir_verify(&module, &diagnostics)) {
        diagnostic_print_all(stderr, &module.sources, &diagnostics);
        arena_destroy(arena);
        cog_ir_module_destroy(&module);
        return fail("constructed trap CogIR did not verify");
    }

    cog_ir_module_freeze(&module);
    LlvmBackendOptions backend_options = {
        .optimization_level = COG_OPTIMIZATION_LEVEL_0,
    };
    LlvmBackendStatus status = llvm_backend_emit_ir_file(
        argv[1],
        &module,
        &backend_options
    );
    int ok = status == LLVM_BACKEND_STATUS_OK &&
             file_contains(argv[1], "call void @llvm.trap()") &&
             file_contains(argv[1], "unreachable");

    arena_destroy(arena);
    cog_ir_module_destroy(&module);
    if (!ok)
        return fail("LLVM trap terminator did not lower to llvm.trap + unreachable");
    return 0;
}
