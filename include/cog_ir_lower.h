#ifndef COGLET_COG_IR_LOWER_H
#define COGLET_COG_IR_LOWER_H

#include <stddef.h>

#include "cog_ir.h"
#include "compiler_driver.h"

typedef enum CogIrLowerDeclKind {
    COG_IR_LOWER_DECL_NONE,
    COG_IR_LOWER_DECL_TYPE,
    COG_IR_LOWER_DECL_GLOBAL,
    COG_IR_LOWER_DECL_FUNCTION,
    COG_IR_LOWER_DECL_CONSTANT,
    COG_IR_LOWER_DECL_FIELD,
    COG_IR_LOWER_DECL_ENUM_MEMBER,
    COG_IR_LOWER_DECL_LOCAL_PENDING,
    COG_IR_LOWER_DECL_PARAMETER_PENDING,
} CogIrLowerDeclKind;

typedef struct CogIrLowerDeclBinding {
    SemDeclId semantic_id;
    CogIrLowerDeclKind kind;
    CogIrTypeId type;

    union {
        CogIrTypeId nominal_type;
        CogIrGlobalId global;
        CogIrFunctionId function;
        CogIrConstId constant;

        struct {
            CogIrTypeId owner_type;
            uint32_t index;
        } field;

        struct {
            CogIrTypeId owner_type;
            uint32_t index;
            CogIrConstId constant;
        } enum_member;
    } as;
} CogIrLowerDeclBinding;

typedef struct CogIrLowerTypeMapEntry {
    const Type *semantic_type;
    CogIrTypeId ir_type;
} CogIrLowerTypeMapEntry;

typedef struct CogIrLowerAbiTypeMapEntry {
    const SemAbiType *semantic_type;
    CogIrAbiTypeId ir_type;
} CogIrLowerAbiTypeMapEntry;

typedef struct CogIrLowerContext {
    const CompileResult *frontend;
    CogIrModule *module;
    DiagnosticList *diagnostics;

    CogIrLowerTypeMapEntry *type_map;
    size_t type_map_count;
    size_t type_map_capacity;

    CogIrLowerAbiTypeMapEntry *abi_type_map;
    size_t abi_type_map_count;
    size_t abi_type_map_capacity;

    CogIrLowerDeclBinding *decl_bindings;
    size_t decl_binding_count;

    int failed;
} CogIrLowerContext;

int cog_ir_lower_context_init(
    CogIrLowerContext *ctx,
    const CompileResult *frontend,
    CogIrModule *module,
    DiagnosticList *diagnostics
);
void cog_ir_lower_context_destroy(CogIrLowerContext *ctx);

int cog_ir_lower_prepare_metadata(CogIrLowerContext *ctx);

CogIrTypeId cog_ir_lower_type(CogIrLowerContext *ctx, const Type *type);
CogIrAbiTypeId cog_ir_lower_abi_type(CogIrLowerContext *ctx, const SemAbiType *type);
CogIrConstId cog_ir_lower_const_value(CogIrLowerContext *ctx, const ConstValue *value);

const CogIrLowerDeclBinding *cog_ir_lower_get_decl_binding(
    const CogIrLowerContext *ctx,
    SemDeclId id
);

#endif
