#ifndef COGLET_COG_IR_H
#define COGLET_COG_IR_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "diagnostic.h"
#include "source.h"
#include "target_info.h"
#include "utils/arena.h"
#include "utils/string_view.h"

typedef uint32_t CogIrTypeId;
typedef uint32_t CogIrAbiTypeId;
typedef uint32_t CogIrConstId;
typedef uint32_t CogIrGlobalId;
typedef uint32_t CogIrFunctionId;
typedef uint32_t CogIrBlockId;
typedef uint32_t CogIrValueId;
typedef uint32_t CogIrSlotId;

#define COG_IR_ID_INVALID UINT32_MAX
#define COG_IR_TYPE_INVALID ((CogIrTypeId)COG_IR_ID_INVALID)
#define COG_IR_ABI_TYPE_INVALID ((CogIrAbiTypeId)COG_IR_ID_INVALID)
#define COG_IR_CONST_INVALID ((CogIrConstId)COG_IR_ID_INVALID)
#define COG_IR_GLOBAL_INVALID ((CogIrGlobalId)COG_IR_ID_INVALID)
#define COG_IR_FUNCTION_INVALID ((CogIrFunctionId)COG_IR_ID_INVALID)
#define COG_IR_BLOCK_INVALID ((CogIrBlockId)COG_IR_ID_INVALID)
#define COG_IR_VALUE_INVALID ((CogIrValueId)COG_IR_ID_INVALID)
#define COG_IR_SLOT_INVALID ((CogIrSlotId)COG_IR_ID_INVALID)

typedef enum CogIrTypeKind {
    COG_IR_TYPE_VOID,
    COG_IR_TYPE_BOOL,
    COG_IR_TYPE_INTEGER,
    COG_IR_TYPE_FLOAT,
    COG_IR_TYPE_POINTER,
    COG_IR_TYPE_OPAQUE_POINTER,
    COG_IR_TYPE_ARRAY,
    COG_IR_TYPE_STRUCT,
    COG_IR_TYPE_UNION,
    COG_IR_TYPE_ENUM,
    COG_IR_TYPE_FUNCTION,
} CogIrTypeKind;

typedef enum CogIrAbiRepresentation {
    COG_IR_ABI_COGLET,
    COG_IR_ABI_C,
} CogIrAbiRepresentation;

typedef enum CogIrCallingConvention {
    COG_IR_CALL_DEFAULT,
    COG_IR_CALL_CDECL,
    COG_IR_CALL_STDCALL,
    COG_IR_CALL_SYSV64,
    COG_IR_CALL_WIN64,
} CogIrCallingConvention;

typedef enum CogIrLinkage {
    COG_IR_LINKAGE_INTERNAL,
    COG_IR_LINKAGE_EXTERNAL,
} CogIrLinkage;

typedef enum CogIrFunctionKind {
    COG_IR_FUNCTION_DECLARATION,
    COG_IR_FUNCTION_DEFINITION,
} CogIrFunctionKind;

typedef enum CogIrCScalarKind {
    COG_IR_C_SCALAR_NONE,
    COG_IR_C_SCALAR_CHAR,
    COG_IR_C_SCALAR_SCHAR,
    COG_IR_C_SCALAR_UCHAR,
    COG_IR_C_SCALAR_SHORT,
    COG_IR_C_SCALAR_USHORT,
    COG_IR_C_SCALAR_INT,
    COG_IR_C_SCALAR_UINT,
    COG_IR_C_SCALAR_LONG,
    COG_IR_C_SCALAR_ULONG,
    COG_IR_C_SCALAR_LONGLONG,
    COG_IR_C_SCALAR_ULONGLONG,
    COG_IR_C_SCALAR_SIZE,
    COG_IR_C_SCALAR_BOOL,
    COG_IR_C_SCALAR_FLOAT,
    COG_IR_C_SCALAR_DOUBLE,
} CogIrCScalarKind;

typedef enum CogIrAbiTypeKind {
    COG_IR_ABI_TYPE_SEMANTIC,
    COG_IR_ABI_TYPE_C_SCALAR,
    COG_IR_ABI_TYPE_POINTER,
    COG_IR_ABI_TYPE_OPAQUE_POINTER,
    COG_IR_ABI_TYPE_ARRAY,
    COG_IR_ABI_TYPE_FUNCTION,
} CogIrAbiTypeKind;

typedef struct CogIrAggregateField {
    StringView debug_name;
    CogIrTypeId type;
    CogIrAbiTypeId abi_type;
    SourceSpan span;
} CogIrAggregateField;

typedef struct CogIrEnumMember {
    StringView debug_name;
    uint64_t bits;
    SourceSpan span;
} CogIrEnumMember;

typedef struct CogIrFunctionType {
    CogIrTypeId result_type;
    CogIrTypeId *parameter_types;
    size_t parameter_count;
    CogIrAbiRepresentation abi;
    CogIrCallingConvention calling_convention;
    int is_variadic;
} CogIrFunctionType;

typedef struct CogIrType {
    CogIrTypeId id;
    CogIrTypeKind kind;
    StringView debug_name;
    SourceSpan span;

    union {
        struct {
            unsigned bits;
            int is_signed;
        } integer;

        struct {
            unsigned bits;
        } floating;

        struct {
            CogIrTypeId pointee;
            int is_readonly;
            int is_volatile;
        } pointer;

        struct {
            int is_readonly;
            int is_volatile;
        } opaque_pointer;

        struct {
            CogIrTypeId element_type;
            size_t length;
        } array;

        struct {
            int is_complete;
            int is_incomplete;
            CogIrAggregateField *fields;
            size_t field_count;
            int is_repr_c;
            int is_packed;
            unsigned explicit_alignment;
        } aggregate;

        struct {
            CogIrTypeId backing_type;
            CogIrEnumMember *members;
            size_t member_count;
            int is_repr_c;
            CogIrAbiTypeId backing_abi_type;
        } enumeration;

        CogIrFunctionType function;
    } as;
} CogIrType;

typedef struct CogIrAbiType {
    CogIrAbiTypeId id;
    CogIrAbiTypeKind kind;
    CogIrTypeId runtime_type;
    CogIrCScalarKind c_scalar_kind;
    CogIrAbiTypeId element_type;
    CogIrAbiTypeId *parameter_types;
    size_t parameter_count;
    CogIrAbiTypeId return_type;
} CogIrAbiType;

typedef enum CogIrConstKind {
    COG_IR_CONST_ZERO,
    COG_IR_CONST_BOOL,
    COG_IR_CONST_INTEGER,
    COG_IR_CONST_FLOAT32,
    COG_IR_CONST_FLOAT64,
    COG_IR_CONST_NULL,
    COG_IR_CONST_ARRAY,
    COG_IR_CONST_STRUCT,
} CogIrConstKind;

typedef struct CogIrConstant {
    CogIrConstId id;
    CogIrConstKind kind;
    CogIrTypeId type;

    union {
        int boolean;
        uint64_t integer_bits;
        uint32_t float32_bits;
        uint64_t float64_bits;
        struct {
            CogIrConstId *elements;
            size_t element_count;
        } aggregate;
    } as;
} CogIrConstant;

typedef struct CogIrGlobal {
    CogIrGlobalId id;
    StringView debug_name;
    SourceSpan span;
    CogIrTypeId type;
    CogIrLinkage linkage;
    int is_compiler_generated;
    int is_readonly;
    CogIrConstId static_initializer;
} CogIrGlobal;

typedef enum CogIrValueKind {
    COG_IR_VALUE_FUNCTION_PARAMETER,
    COG_IR_VALUE_BLOCK_PARAMETER,
    COG_IR_VALUE_INSTRUCTION,
} CogIrValueKind;

typedef struct CogIrValue {
    CogIrValueId id;
    CogIrTypeId type;
    /* Optional exact native-C ABI spelling for values whose runtime type alone is insufficient. */
    CogIrAbiTypeId abi_type;
    CogIrValueKind kind;
    CogIrBlockId block;
    size_t ordinal;
    SourceSpan span;
} CogIrValue;

typedef struct CogIrSlot {
    CogIrSlotId id;
    StringView debug_name;
    SourceSpan span;
    CogIrTypeId type;
    /* Optional exact native-C ABI spelling preserved across local storage/spills. */
    CogIrAbiTypeId abi_type;
} CogIrSlot;

typedef struct CogIrBlockParam {
    CogIrValueId value;
    CogIrTypeId type;
    StringView debug_name;
    SourceSpan span;
} CogIrBlockParam;

typedef enum CogIrOp {
    COG_IR_OP_CONST,
    COG_IR_OP_FUNCTION_REF,
    COG_IR_OP_LOCAL_ADDR,
    COG_IR_OP_GLOBAL_ADDR,
    COG_IR_OP_FIELD_ADDR,
    COG_IR_OP_ARRAY_ELEM_ADDR,
    COG_IR_OP_PTR_INDEX_ADDR,
    COG_IR_OP_LOAD,
    COG_IR_OP_STORE,

    COG_IR_OP_MAKE_STRUCT,
    COG_IR_OP_MAKE_ARRAY,
    COG_IR_OP_EXTRACT_FIELD,
    COG_IR_OP_EXTRACT_ELEMENT,

    COG_IR_OP_IADD_CHECKED,
    COG_IR_OP_ISUB_CHECKED,
    COG_IR_OP_IMUL_CHECKED,
    COG_IR_OP_IDIV_CHECKED,
    COG_IR_OP_IREM_CHECKED,
    COG_IR_OP_INEG_CHECKED,

    COG_IR_OP_IADD_WRAP,
    COG_IR_OP_ISUB_WRAP,
    COG_IR_OP_IMUL_WRAP,
    COG_IR_OP_INEG_WRAP,

    COG_IR_OP_BIT_AND,
    COG_IR_OP_BIT_OR,
    COG_IR_OP_BIT_XOR,
    COG_IR_OP_BIT_NOT,
    COG_IR_OP_SHL_CHECKED_COUNT,
    COG_IR_OP_SHR_SIGNED_CHECKED_COUNT,
    COG_IR_OP_SHR_UNSIGNED_CHECKED_COUNT,

    COG_IR_OP_FADD,
    COG_IR_OP_FSUB,
    COG_IR_OP_FMUL,
    COG_IR_OP_FDIV,
    COG_IR_OP_FNEG,

    COG_IR_OP_ICMP_EQ,
    COG_IR_OP_ICMP_NE,
    COG_IR_OP_ICMP_SLT,
    COG_IR_OP_ICMP_SLE,
    COG_IR_OP_ICMP_SGT,
    COG_IR_OP_ICMP_SGE,
    COG_IR_OP_ICMP_ULT,
    COG_IR_OP_ICMP_ULE,
    COG_IR_OP_ICMP_UGT,
    COG_IR_OP_ICMP_UGE,

    COG_IR_OP_FCMP_EQ,
    COG_IR_OP_FCMP_NE,
    COG_IR_OP_FCMP_LT,
    COG_IR_OP_FCMP_LE,
    COG_IR_OP_FCMP_GT,
    COG_IR_OP_FCMP_GE,

    COG_IR_OP_PTR_EQ,
    COG_IR_OP_PTR_NE,
    COG_IR_OP_BOOL_NOT,

    COG_IR_OP_CAST_CHECKED,
    COG_IR_OP_INT_TRUNCATE,
    COG_IR_OP_PTR_REINTERPRET,
    COG_IR_OP_PTR_QUALIFY,

    /* Target-C default argument promotion at a variadic call boundary. */
    COG_IR_OP_C_VARARG_PROMOTE,

    COG_IR_OP_CALL,
} CogIrOp;

typedef struct CogIrInstruction {
    CogIrOp op;
    CogIrValueId result;
    CogIrTypeId result_type;
    SourceSpan span;

    union {
        struct { CogIrConstId constant; } constant;
        struct { CogIrFunctionId function; } function_ref;
        struct { CogIrSlotId slot; } local_addr;
        struct { CogIrGlobalId global; } global_addr;
        struct { CogIrValueId base; uint32_t field_index; } field_addr;
        struct { CogIrValueId base; CogIrValueId index; } index_addr;
        struct { CogIrValueId address; int is_volatile; } load;
        struct { CogIrValueId address; CogIrValueId value; int is_volatile; } store;
        struct { CogIrValueId lhs; CogIrValueId rhs; } binary;
        struct { CogIrValueId operand; } unary;
        struct { CogIrValueId operand; CogIrTypeId target_type; } conversion;
        struct { CogIrValueId aggregate; uint32_t index; } extract;
        struct { CogIrValueId *values; size_t value_count; } aggregate;
        struct { CogIrValueId callee; CogIrValueId *arguments; size_t argument_count; } call;
    } as;
} CogIrInstruction;

typedef struct CogIrBranchEdge {
    CogIrBlockId target;
    CogIrValueId *arguments;
    size_t argument_count;
} CogIrBranchEdge;

typedef struct CogIrSwitchCase {
    CogIrConstId key;
    CogIrBranchEdge edge;
} CogIrSwitchCase;

typedef enum CogIrTrapReason {
    COG_IR_TRAP_EXPLICIT,
    COG_IR_TRAP_ARITHMETIC_OVERFLOW,
    COG_IR_TRAP_DIVISION_BY_ZERO,
    COG_IR_TRAP_INVALID_SHIFT,
    COG_IR_TRAP_INVALID_CAST,
} CogIrTrapReason;

typedef enum CogIrTerminatorKind {
    COG_IR_TERMINATOR_NONE,
    COG_IR_TERMINATOR_BR,
    COG_IR_TERMINATOR_COND_BR,
    COG_IR_TERMINATOR_SWITCH,
    COG_IR_TERMINATOR_RET,
    COG_IR_TERMINATOR_TRAP,
    COG_IR_TERMINATOR_UNREACHABLE,
} CogIrTerminatorKind;

typedef struct CogIrTerminator {
    CogIrTerminatorKind kind;
    SourceSpan span;

    union {
        struct { CogIrBranchEdge edge; } branch;
        struct {
            CogIrValueId condition;
            CogIrBranchEdge if_true;
            CogIrBranchEdge if_false;
        } cond_branch;
        struct {
            CogIrValueId value;
            CogIrSwitchCase *cases;
            size_t case_count;
            CogIrBranchEdge default_edge;
        } switch_term;
        struct {
            int has_value;
            CogIrValueId value;
        } ret;
        struct { CogIrTrapReason reason; } trap;
    } as;
} CogIrTerminator;

typedef struct CogIrBlock {
    CogIrBlockId id;
    StringView debug_name;
    SourceSpan span;

    CogIrBlockParam *parameters;
    size_t parameter_count;
    size_t parameter_capacity;

    CogIrInstruction *instructions;
    size_t instruction_count;
    size_t instruction_capacity;

    CogIrTerminator terminator;
} CogIrBlock;

typedef struct CogIrFunctionAbi {
    CogIrAbiRepresentation abi;
    CogIrCallingConvention calling_convention;
    int is_variadic;
    StringView external_symbol;
    CogIrAbiTypeId return_abi_type;
    CogIrAbiTypeId *parameter_abi_types;
    size_t parameter_count;
} CogIrFunctionAbi;

typedef struct CogIrFunction {
    CogIrFunctionId id;
    StringView debug_name;
    SourceSpan span;
    CogIrTypeId type;
    CogIrFunctionKind kind;
    CogIrLinkage linkage;
    int is_compiler_generated;
    CogIrCScalarKind source_return_c_scalar_kind;
    CogIrFunctionAbi abi;

    CogIrValue *values;
    size_t value_count;
    size_t value_capacity;

    CogIrValueId *parameters;
    size_t parameter_count;

    CogIrSlot *slots;
    size_t slot_count;
    size_t slot_capacity;

    CogIrBlock *blocks;
    size_t block_count;
    size_t block_capacity;
    CogIrBlockId entry_block;
} CogIrFunction;

typedef struct CogIrModule {
    Arena *arena;
    TargetInfo target;
    SourceManager sources;

    CogIrType *types;
    size_t type_count;
    size_t type_capacity;

    CogIrAbiType *abi_types;
    size_t abi_type_count;
    size_t abi_type_capacity;

    CogIrConstant *constants;
    size_t constant_count;
    size_t constant_capacity;

    CogIrGlobal *globals;
    size_t global_count;
    size_t global_capacity;

    CogIrFunction *functions;
    size_t function_count;
    size_t function_capacity;

    /* Optional source-language executable entry declaration. */
    CogIrFunctionId entry_function;
    CogIrFunctionId init_function;
    int is_frozen;
} CogIrModule;

/* Module/source ownership. */
void cog_ir_module_init(CogIrModule *module, const TargetInfo *target);
void cog_ir_module_destroy(CogIrModule *module);
int cog_ir_module_add_source(CogIrModule *module, const char *filename, const char *source, SourceFileId *out_id);
int cog_ir_module_copy_sources(CogIrModule *module, const SourceManager *sources);
void cog_ir_module_freeze(CogIrModule *module);
int cog_ir_module_is_frozen(const CogIrModule *module);

/* Runtime type builder. Structural types are interned; nominal types are not. */
CogIrTypeId cog_ir_type_void(CogIrModule *module);
CogIrTypeId cog_ir_type_bool(CogIrModule *module);
CogIrTypeId cog_ir_type_integer(CogIrModule *module, unsigned bits, int is_signed);
CogIrTypeId cog_ir_type_float(CogIrModule *module, unsigned bits);
CogIrTypeId cog_ir_type_pointer(CogIrModule *module, CogIrTypeId pointee, int is_readonly, int is_volatile);
CogIrTypeId cog_ir_type_opaque_pointer(CogIrModule *module, int is_readonly, int is_volatile);
CogIrTypeId cog_ir_type_array(CogIrModule *module, CogIrTypeId element_type, size_t length);
CogIrTypeId cog_ir_type_function(
    CogIrModule *module,
    CogIrTypeId result_type,
    const CogIrTypeId *parameter_types,
    size_t parameter_count,
    CogIrAbiRepresentation abi,
    CogIrCallingConvention calling_convention,
    int is_variadic
);
CogIrTypeId cog_ir_declare_nominal_type(
    CogIrModule *module,
    CogIrTypeKind kind,
    StringView debug_name,
    SourceSpan span
);
int cog_ir_mark_incomplete_aggregate_type(CogIrModule *module, CogIrTypeId type);
int cog_ir_define_aggregate_type(
    CogIrModule *module,
    CogIrTypeId type,
    const CogIrAggregateField *fields,
    size_t field_count,
    int is_repr_c,
    int is_packed,
    unsigned explicit_alignment
);
int cog_ir_define_enum_type(
    CogIrModule *module,
    CogIrTypeId type,
    CogIrTypeId backing_type,
    const CogIrEnumMember *members,
    size_t member_count,
    int is_repr_c,
    CogIrAbiTypeId backing_abi_type
);

/* ABI-spelling builder. */
CogIrAbiTypeId cog_ir_abi_type_semantic(CogIrModule *module, CogIrTypeId runtime_type);
CogIrAbiTypeId cog_ir_abi_type_c_scalar(CogIrModule *module, CogIrTypeId runtime_type, CogIrCScalarKind scalar);
CogIrAbiTypeId cog_ir_abi_type_pointer(CogIrModule *module, CogIrTypeId runtime_type, CogIrAbiTypeId element_type);
CogIrAbiTypeId cog_ir_abi_type_opaque_pointer(CogIrModule *module, CogIrTypeId runtime_type);
CogIrAbiTypeId cog_ir_abi_type_array(CogIrModule *module, CogIrTypeId runtime_type, CogIrAbiTypeId element_type);
CogIrAbiTypeId cog_ir_abi_type_function(
    CogIrModule *module,
    CogIrTypeId runtime_type,
    CogIrAbiTypeId return_type,
    const CogIrAbiTypeId *parameter_types,
    size_t parameter_count
);

/* Constants. */
CogIrConstId cog_ir_const_zero(CogIrModule *module, CogIrTypeId type);
CogIrConstId cog_ir_const_bool(CogIrModule *module, CogIrTypeId type, int value);
CogIrConstId cog_ir_const_integer(CogIrModule *module, CogIrTypeId type, uint64_t bits);
CogIrConstId cog_ir_const_float32(CogIrModule *module, CogIrTypeId type, uint32_t bits);
CogIrConstId cog_ir_const_float64(CogIrModule *module, CogIrTypeId type, uint64_t bits);
CogIrConstId cog_ir_const_null(CogIrModule *module, CogIrTypeId type);
CogIrConstId cog_ir_const_array(CogIrModule *module, CogIrTypeId type, const CogIrConstId *elements, size_t element_count);
CogIrConstId cog_ir_const_struct(CogIrModule *module, CogIrTypeId type, const CogIrConstId *fields, size_t field_count);

/* Globals/functions/CFG. */
CogIrGlobalId cog_ir_add_global(
    CogIrModule *module,
    StringView debug_name,
    SourceSpan span,
    CogIrTypeId type,
    CogIrLinkage linkage,
    int is_compiler_generated,
    int is_readonly,
    CogIrConstId static_initializer
);
CogIrFunctionId cog_ir_add_function(
    CogIrModule *module,
    StringView debug_name,
    SourceSpan span,
    CogIrTypeId function_type,
    CogIrFunctionKind kind,
    CogIrLinkage linkage,
    int is_compiler_generated,
    CogIrCScalarKind source_return_c_scalar_kind,
    const CogIrFunctionAbi *abi
);
/* Upgrade a predeclared internal function to a definition before adding CFG. */
int cog_ir_begin_function_definition(CogIrModule *module, CogIrFunctionId function);

int cog_ir_set_entry_function(CogIrModule *module, CogIrFunctionId function);
int cog_ir_set_init_function(CogIrModule *module, CogIrFunctionId function);
CogIrSlotId cog_ir_add_slot(CogIrModule *module, CogIrFunctionId function, StringView debug_name, SourceSpan span, CogIrTypeId type);
int cog_ir_set_slot_abi_type(CogIrModule *module, CogIrFunctionId function, CogIrSlotId slot, CogIrAbiTypeId abi_type);
int cog_ir_set_value_abi_type(CogIrModule *module, CogIrFunctionId function, CogIrValueId value, CogIrAbiTypeId abi_type);
CogIrBlockId cog_ir_add_block(CogIrModule *module, CogIrFunctionId function, StringView debug_name, SourceSpan span);
CogIrValueId cog_ir_add_block_parameter(CogIrModule *module, CogIrFunctionId function, CogIrBlockId block, CogIrTypeId type, StringView debug_name, SourceSpan span);
int cog_ir_emit(CogIrModule *module, CogIrFunctionId function, CogIrBlockId block, const CogIrInstruction *instruction, CogIrValueId *out_result);
int cog_ir_set_terminator(CogIrModule *module, CogIrFunctionId function, CogIrBlockId block, const CogIrTerminator *terminator);

/* Table lookup helpers. */
const CogIrType *cog_ir_get_type(const CogIrModule *module, CogIrTypeId id);
const CogIrAbiType *cog_ir_get_abi_type(const CogIrModule *module, CogIrAbiTypeId id);
const CogIrConstant *cog_ir_get_constant(const CogIrModule *module, CogIrConstId id);
const CogIrGlobal *cog_ir_get_global(const CogIrModule *module, CogIrGlobalId id);
const CogIrFunction *cog_ir_get_function(const CogIrModule *module, CogIrFunctionId id);
const CogIrBlock *cog_ir_get_block(const CogIrFunction *function, CogIrBlockId id);
const CogIrValue *cog_ir_get_value(const CogIrFunction *function, CogIrValueId id);
const CogIrSlot *cog_ir_get_slot(const CogIrFunction *function, CogIrSlotId id);

/* Debugging/validation. */
int cog_ir_verify(const CogIrModule *module, DiagnosticList *diagnostics);
void cog_ir_dump(FILE *stream, const CogIrModule *module);

const char *cog_ir_type_kind_name(CogIrTypeKind kind);
const char *cog_ir_op_name(CogIrOp op);

#endif
