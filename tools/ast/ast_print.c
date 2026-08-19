#include "ast_print.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

#include "types.h"
#include "utils/utils.h"


static void print_escaped_string_view_inline(StringView view)
{
    putchar('"');

    for (size_t i = 0; i < view.length; i++) {
        unsigned char c = (unsigned char)view.data[i];

        switch (c) {
            case '\n': printf("\\n"); break;
            case '\t': printf("\\t"); break;
            case '\r': printf("\\r"); break;
            case '\\': printf("\\\\"); break;
            case '"': printf("\\\""); break;
            case '\0': printf("\\0"); break;
            default: putchar((int)c); break;
        }
    }

    putchar('"');
}

static const char *c_calling_convention_name(CCallingConvention convention)
{
    switch (convention) {
        case C_CALL_DEFAULT: return NULL;
        case C_CALL_CDECL: return "cdecl";
        case C_CALL_STDCALL: return "stdcall";
        case C_CALL_SYSV64: return "sysv64";
        case C_CALL_WIN64: return "win64";
    }

    return NULL;
}

static const char *cast_kind_name(CastKind kind)
{
    switch (kind) {
        case CAST_CHECKED:
            return "cast";

        case CAST_TRUNCATING:
            return "truncate";

        case CAST_REINTERPRET:
            return "reinterpret";
    }

    assert(0 && "unhandled CastKind");
    return "<invalid-cast>";
}

static const char *token_type_str(TokenType type)
{
    switch (type) {
        // Arithmetic
        case TOK_PLUS:
            return "+";

        case TOK_MINUS:
            return "-";

        case TOK_STAR:
            return "*";

        case TOK_SLASH:
            return "/";

        case TOK_PERCENT:
            return "%";

        case TOK_PLUS_PLUS:
            return "++";

        case TOK_MINUS_MINUS:
            return "--";

        // Assignment
        case TOK_EQUAL:
            return "=";

        case TOK_PLUS_EQUAL:
            return "+=";

        case TOK_MINUS_EQUAL:
            return "-=";

        case TOK_STAR_EQUAL:
            return "*=";

        case TOK_SLASH_EQUAL:
            return "/=";

        case TOK_PERCENT_EQUAL:
            return "%=";

        case TOK_AND_EQUAL:
            return "&=";

        case TOK_OR_EQUAL:
            return "|=";

        case TOK_XOR_EQUAL:
            return "^=";

        case TOK_SHIFT_LEFT_EQUAL:
            return "<<=";

        case TOK_SHIFT_RIGHT_EQUAL:
            return ">>=";

        // Equality and comparison
        case TOK_EQUAL_EQUAL:
            return "==";

        case TOK_BANG:
            return "!";

        case TOK_BANG_EQUAL:
            return "!=";

        case TOK_LESS:
            return "<";

        case TOK_LESS_EQUAL:
            return "<=";

        case TOK_GREATER:
            return ">";

        case TOK_GREATER_EQUAL:
            return ">=";

        // Logical, bitwise, and shifts
        case TOK_AND:
            return "&";

        case TOK_AND_AND:
            return "&&";

        case TOK_OR:
            return "|";

        case TOK_OR_OR:
            return "||";

        case TOK_XOR:
            return "^";

        case TOK_TILDE:
            return "~";

        case TOK_SHIFT_LEFT:
            return "<<";

        case TOK_SHIFT_RIGHT:
            return ">>";

        // Punctuation
        case TOK_DOT:
            return ".";

        case TOK_ELLIPSIS:
            return "...";

        case TOK_LBRACKET:
            return "[";

        case TOK_RBRACKET:
            return "]";

        case TOK_LPAREN:
            return "(";

        case TOK_RPAREN:
            return ")";

        case TOK_LBRACE:
            return "{";

        case TOK_RBRACE:
            return "}";

        case TOK_SEMICOLON:
            return ";";

        case TOK_COLON:
            return ":";

        case TOK_COMMA:
            return ",";

        case TOK_ARROW:
            return "->";

        case TOK_COLON_COLON:
            return "::";

        case TOK_COLON_EQUAL:
            return ":=";

        case TOK_HASH:
            return "#";

        // Literals
        case TOK_IDENT:
            return "IDENT";

        case TOK_NUMBER_INT:
            return "INT";

        case TOK_NUMBER_FLOAT:
            return "FLOAT";

        case TOK_STRING:
            return "STRING";

        case TOK_CHAR:
            return "CHAR";

        case TOK_TRUE:
            return "TRUE";

        case TOK_FALSE:
            return "FALSE";

        case TOK_NULL:
            return "NULL";

        // Keywords
        case TOK_IF:
            return "IF";

        case TOK_ELSE:
            return "ELSE";

        case TOK_WHILE:
            return "WHILE";

        case TOK_FOR:
            return "FOR";

        case TOK_IN:
            return "IN";

        case TOK_RETURN:
            return "RETURN";

        case TOK_DEFER:
            return "DEFER";

        case TOK_DISCARD:
            return "DISCARD";

        case TOK_STATIC_ASSERT:
            return "STATIC_ASSERT";

        case TOK_VOID:
            return "VOID";

        case TOK_STRUCT:
            return "STRUCT";

        case TOK_RESOURCE:
            return "RESOURCE";

        case TOK_UNION:
            return "UNION";

        case TOK_BREAK:
            return "BREAK";

        case TOK_CONTINUE:
            return "CONTINUE";

        case TOK_ENUM:
            return "ENUM";

        case TOK_SWITCH:
            return "SWITCH";

        case TOK_CASE:
            return "CASE";

        case TOK_DEFAULT:
            return "DEFAULT";

        case TOK_CAST:
            return "CAST";

        case TOK_TRUNCATE:
            return "TRUNCATE";

        case TOK_REINTERPRET:
            return "REINTERPRET";

        case TOK_MOVE:
            return "MOVE";

        case TOK_READONLY:
            return "READONLY";

        case TOK_VOLATILE:
            return "VOLATILE";

        case TOK_OPAQUE:
            return "OPAQUE";

        case TOK_CFN:
            return "CFN";

        case TOK_SIZE_OF:
            return "SIZE_OF";

        case TOK_ALIGN_OF:
            return "ALIGN_OF";

        case TOK_ASM:
            return "ASM";

        // Types
        case TOK_BOOL:
            return "BOOL";

        case TOK_S8:
            return "S8";

        case TOK_S16:
            return "S16";

        case TOK_S32:
            return "S32";

        case TOK_S64:
            return "S64";

        case TOK_U8:
            return "U8";

        case TOK_U16:
            return "U16";

        case TOK_U32:
            return "U32";

        case TOK_U64:
            return "U64";

        case TOK_ISIZE:
            return "ISIZE";

        case TOK_USIZE:
            return "USIZE";

        case TOK_F32:
            return "F32";

        case TOK_F64:
            return "F64";

        case TOK_INT_KW:
            return "INT KW";

        case TOK_UINT_KW:
            return "UINT KW";

        // Special
        case TOK_EOF:
            return "EOF";

        case TOK_ERROR:
            return "ERROR";
    }

    return "?";
}

static void print_type(Type *t)
{
    if (!t) {
        printf("<inferred>");
        return;
    }

    switch (t->kind) {
        case TYPE_VOID: printf("void"); break;
        case TYPE_BOOL: printf("bool"); break;
        case TYPE_NULL: printf("null"); break;

        case TYPE_S8:   printf("s8");   break;
        case TYPE_S16:  printf("s16");  break;
        case TYPE_S32:  printf("s32");  break;
        case TYPE_S64:  printf("s64");  break;
        case TYPE_U8:   printf("u8");   break;
        case TYPE_U16:  printf("u16");  break;
        case TYPE_U32:  printf("u32");  break;
        case TYPE_U64:  printf("u64");  break;
        case TYPE_F32:  printf("f32");  break;
        case TYPE_F64:  printf("f64");  break;

        case TYPE_OPAQUE_POINTER:
            switch (t->pointer_access) {
                case POINTER_ACCESS_MUTABLE:
                    break;

                case POINTER_ACCESS_READONLY:
                    printf("readonly ");
                    break;
            }
            if (t->pointer_is_volatile)
                printf("volatile ");
            printf("opaque*");
            break;

        case TYPE_POINTER:
            switch (t->pointer_access) {
            case POINTER_ACCESS_MUTABLE:
                    break;

            case POINTER_ACCESS_READONLY:
                    printf("readonly ");
                    break;
            }

            if (t->pointer_is_volatile)
                printf("volatile ");

            print_type(t->element);
            printf("*");
            break;

        case TYPE_ARRAY:
            print_type(t->element);
            printf("[%d]", t->array_size);
            break;

        case TYPE_SLICE:
            if (t->pointer_access == POINTER_ACCESS_READONLY)
                printf("readonly ");
            print_type(t->element);
            printf("[]");
            break;

        case TYPE_NAMED:
            if (t->named_module.length != 0) {
                print_string_view(t->named_module);
                printf(".");
            }
            print_string_view(t->named_name);
            if (t->type_argument_count > 0) {
                printf("::<");
                for (int i = 0; i < t->type_argument_count; i++) {
                    if (i > 0) printf(", ");
                    print_type(t->type_arguments[i]);
                }
                printf(">");
            }
            break;

        case TYPE_STRUCT:
            printf("struct ");
            print_string_view(t->struct_name);
            break;

        case TYPE_ENUM:
            printf("enum ");
            print_string_view(t->enum_name);
            break;

        case TYPE_FUNCTION:
            if (t->function_abi == FUNCTION_ABI_C) {
                printf("cfn(");
                const char *call = c_calling_convention_name(t->function_call_conv);
                if (call)
                    printf("call=%s%s", call, t->parameter_count > 0 ? ", " : "");
            } else {
                printf("fn(");
            }

            for (int i = 0; i < t->parameter_count; i++) {
                if (i > 0) printf(", ");
                print_type(t->parameters[i]);
            }

            if (t->function_is_variadic) {
                if (t->parameter_count > 0) printf(", ");
                printf("...");
            }

            printf(") -> ");
            print_type(t->return_type);
            break;

        default:
            printf("<unknown-type>");
            break;
    }
}

static void print_node(Node *node)
{
    int wrap_export = node && node->is_exported;
    if (wrap_export)
        printf("(export ");

    switch (node->type)
    {
        case NODE_NUMBER:
            if (node->as.number.kind == NUMBER_LITERAL_FLOAT) {
                printf("%.17g", node->as.number.value.floating);
            } else {
                printf(
                    "%" PRIu64,
                    node->as.number.value.integer
                );
            }
            break;

        case NODE_IDENT:
            print_string_view(node->as.ident);
            break;

        case NODE_STRING:
            print_string_view_quoted(node->as.string_literal);
            break;

        case NODE_CHAR:
            print_string_view_single_quoted(node->as.char_literal);
            break;

        case NODE_BOOL:
            printf("%s", node->as.boolean.value ? "true" : "false");
            break;

        case NODE_NULL:
            printf("null");
            break;

        case NODE_CAST:
            printf("(%s ",cast_kind_name(node->as.cast_expr.kind));
            print_type(node->as.cast_expr.target_type);
            printf(" ");
            print_node(node->as.cast_expr.expression);
            printf(")");
            break;

        case NODE_UNARY:
            printf("(%s ", token_type_str(node->as.unary.op));
            print_node(node->as.unary.operand);
            printf(")");
            break;

        case NODE_BINARY:
            printf("(%s ", token_type_str(node->as.binary.op));
            print_node(node->as.binary.left);
            printf(" ");
            print_node(node->as.binary.right);
            printf(")");
            break;

        case NODE_INC_DEC:
            if (node->as.inc_dec.is_prefix) {
                printf("(%s ", token_type_str(node->as.inc_dec.op));
                print_node(node->as.inc_dec.target);
                printf(")");
            } else {
                printf("(post%s ", token_type_str(node->as.inc_dec.op));
                print_node(node->as.inc_dec.target);
                printf(")");
            }
            break;

        case NODE_ASSIGN:
            printf("(= ");
            print_node(node->as.assign.target);
            printf(" ");
            print_node(node->as.assign.value);
            printf(")");
            break;

        case NODE_COMPOUND_ASSIGN:
            printf("(%s ",
                token_type_str(node->as.compound_assign.op));

            print_node(node->as.compound_assign.target);

            printf(" ");

            print_node(node->as.compound_assign.value);

            printf(")");
            break;

        case NODE_IF_EXPR:
            printf("(if_expr ");
            print_node(node->as.if_expr.condition);
            printf(" ");
            print_node(node->as.if_expr.then_value);
            printf(" ");
            print_node(node->as.if_expr.else_value);
            printf(")");
            break;

        case NODE_IF:
            printf("(if ");
            print_node(node->as.if_stmt.condition);
            printf(" ");
            print_node(node->as.if_stmt.then_branch);

            if (node->as.if_stmt.else_branch) {
                printf(" ");
                print_node(node->as.if_stmt.else_branch);
            }

            printf(")");

            break;

        case NODE_EXPR_STMT:
            print_node(node->as.expr_stmt.expr);
            break;

        case NODE_ASM:
            printf("(asm%s %.*s)",
                node->as.asm_stmt.is_volatile ? " volatile" : "",
                (int)node->as.asm_stmt.template_text.length,
                node->as.asm_stmt.template_text.data);
            break;

        case NODE_STATIC_ASSERT:
            printf("(static_assert ");
            print_node(node->as.static_assert_stmt.condition);
            if (node->as.static_assert_stmt.message) {
                printf(" ");
                print_node(node->as.static_assert_stmt.message);
            }
            printf(")");
            break;

        case NODE_BLOCK:
            printf("block\n");
            for (int i = 0;
                 i < node->as.block.statements.count;
                 i++)
            {
                print_node(node->as.block.statements.items[i]);
            }

            break;

        case NODE_CALL:
            printf("(call");
            if (node->as.call.type_arguments.count > 0) {
                printf("::<");
                for (int i = 0; i < node->as.call.type_arguments.count; i++) {
                    if (i > 0) printf(", ");
                    print_type(node->as.call.type_arguments.items[i]);
                }
                printf(">");
            }
            printf(" ");
            print_node(node->as.call.callee);
            printf(" (");

            for (int i = 0;
                 i < node->as.call.arguments.count;
                 i++)
            {
                print_node(node->as.call.arguments.items[i]);

                if (i + 1 < node->as.call.arguments.count)
                    printf(" ");
            }

            printf("))");

            break;

        case NODE_FIELD:
            printf("(field ");
            print_string_view(node->as.field.name);
            printf(" ");
            print_node(node->as.field.object);
            printf(")");

            break;

        case NODE_INDEX:
            printf("(index ");
            print_node(node->as.index.object);
            printf(" ");
            print_node(node->as.index.index);
            printf(")");

            break;

        case NODE_TYPE_REF:
            printf("(type_ref ");
            print_type(node->as.type_ref.source_type);
            printf(")");
            break;

        case NODE_CONST_DECL:
            printf("(const_decl ");
            print_type(node->as.const_decl.const_type);
            print_string_view(node->as.const_decl.name);
            print_node(node->as.const_decl.value);
            printf(")");
            break;

        case NODE_ERROR:
            printf("<error>");
            break;

        case NODE_PROGRAM:

            for (int i = 0;
                 i < node->as.program.statements.count;
                 i++)
            {
                print_node(node->as.program.statements.items[i]);

                if (i + 1 < node->as.program.statements.count)
                    printf("\n");
            }

            break;

        case NODE_MODULE_DECL:
            printf("(module ");
            print_string_view(node->as.module_decl.name);
            printf(")");
            break;

        case NODE_IMPORT_DECL:
            printf("(import ");
            print_string_view(node->as.import_decl.name);
            if (node->as.import_decl.alias.length) {
                printf(" as ");
                print_string_view(node->as.import_decl.alias);
            }
            printf(")");
            break;

        case NODE_VAR_DECL:
            printf("(var_decl ");

            print_type(node->as.var_decl.var_type);

            printf(" ");

            print_string_view(node->as.var_decl.name);

            if (node->as.var_decl.initializer) {
                printf(" = ");
                print_node(node->as.var_decl.initializer);
            }

            printf(")");

            break;

        case NODE_VAR_DECL_GROUP:
            printf("(var_decl_group");
            for (int i = 0; i < node->as.var_decl_group.declarations.count; i++) {
                printf(" ");
                print_node(node->as.var_decl_group.declarations.items[i]);
            }
            printf(")");
            break;

        case NODE_FUNC_PARAM_DECL:
            printf("(param_decl ");

            print_type(node->as.param_decl.var_type);

            printf(" ");

            print_string_view(node->as.param_decl.name);


            printf(")");

            break;

        case NODE_STRUCT_FIELD_DECL:
            printf("(struct_field_decl ");
            print_type(node->as.struct_field_decl.var_type);

            printf(" ");

            print_string_view(node->as.struct_field_decl.name);

            printf(")");

            break;

        case NODE_STRUCT_INIT:
            printf("(struct_init ");
            if (node->as.struct_init.module_name.length != 0) {
                print_string_view(node->as.struct_init.module_name);
                printf(".");
            }
            print_string_view(node->as.struct_init.name);
            if (node->as.struct_init.type_arguments.count > 0) {
                printf("::<");
                for (int i = 0; i < node->as.struct_init.type_arguments.count; i++) {
                    if (i > 0) printf(", ");
                    print_type(node->as.struct_init.type_arguments.items[i]);
                }
                printf(">");
            }

            for (int i = 0;
                 i < node->as.struct_init.fields.count;
                 i++)
            {
                printf(" ");
                print_node(node->as.struct_init.fields.items[i]);
            }
            printf(")");
            break;


        case NODE_FIELD_INIT:
            printf("(field_init ");
            print_string_view(node->as.field_init.name);
            if (node->as.field_init.value) {
                printf(" ");
                print_node(node->as.field_init.value);
            }
            printf(")");
            break;

        case NODE_ENUM_DECL:
            printf("(enum ");
            if (node->as.enum_decl.is_repr_c)
                printf("#repr(c) ");
            printf("%.*s (",
                (int)node->as.enum_decl.name.length,
                node->as.enum_decl.name.data);


            if (node->as.enum_decl.backing_type) {
                print_type(node->as.enum_decl.backing_type);
            } else {
                printf("<default>");
            }

            printf(")");

            for (int i = 0; i < node->as.enum_decl.members.count; i++) {
                printf(" ");
                print_node(node->as.enum_decl.members.items[i]);
            }

            printf(")");
            break;

        case NODE_ENUM_MEMBER:
            printf("(member %.*s",
                (int)node->as.enum_member.name.length,
                node->as.enum_member.name.data);

            if (node->as.enum_member.value) {
                printf(" = ");
                print_node(node->as.enum_member.value);
            }

            printf(")");
            break;

        case NODE_RETURN:
            printf("(return");
            if (node->as.return_stmt.value) {
                printf(" ");
                print_node(node->as.return_stmt.value);
            }
            printf(")");
            break;

        case NODE_DEFER:
            printf("(defer ");
            print_node(node->as.defer_stmt.statement);
            printf(")");
            break;

        case NODE_SWITCH:
            printf("(switch ");
            print_node(node->as.switch_stmt.expression);

            for (int i = 0; i < node->as.switch_stmt.cases.count; i++) {
                printf(" ");
                print_node(node->as.switch_stmt.cases.items[i]);
            }

            printf(")");
            break;

        case NODE_SWITCH_CASE:
            if (node->as.switch_case.is_default) {
                printf("(default ");
                print_node(node->as.switch_case.body);
                printf(")");
            } else {
                printf("(case ");
                print_node(node->as.switch_case.value);
                printf(" ");
                print_node(node->as.switch_case.body);
                printf(")");
            }

            break;

        case NODE_WHILE:
            printf("(while ");
            print_node(node->as.while_stmt.condition);
            printf(" ");
            print_node(node->as.while_stmt.body);
            printf(")");
            break;

        case NODE_FOR:
            printf("(for ");
            if (node->as.for_stmt.condition)
                print_node(node->as.for_stmt.condition);
            printf(" ");

            if (node->as.for_stmt.post)
                print_node(node->as.for_stmt.post);
            printf(" ");
            print_node(node->as.for_stmt.body);
            printf(")");
            break;

        case NODE_BREAK:
            printf("(break)");
            break;

        case NODE_CONTINUE:
            printf("(continue)");
            break;

        case NODE_FUNC_DECL:
            if (node->as.func_decl.linkage == FUNCTION_LINKAGE_EXTERN_C) {
                printf("(extern-func c ");
                if (node->as.func_decl.is_discardable)
                    printf("#discardable ");

                if (!string_view_is_empty(node->as.func_decl.external_name)) {
                    printf("name=");
                    print_escaped_string_view_inline(node->as.func_decl.external_name);
                    printf(" ");
                }

                const char *call = c_calling_convention_name(node->as.func_decl.c_call_conv);
                if (call)
                    printf("call=%s ", call);
            } else {
                printf("(func ");
                if (node->as.func_decl.is_discardable)
                    printf("#discardable ");
                if (node->as.func_decl.is_repr_c) {
                    const char *call = c_calling_convention_name(node->as.func_decl.c_call_conv);
                    if (call)
                        printf("#repr(c, call=%s) ", call);
                    else
                        printf("#repr(c) ");
                }
            }

            print_string_view(node->as.func_decl.name);
            if (node->as.func_decl.type_parameters.count > 0) {
                printf("<");
                for (int i = 0; i < node->as.func_decl.type_parameters.count; i++) {
                    if (i > 0) printf(", ");
                    GenericTypeParameter parameter =
                        node->as.func_decl.type_parameters.items[i];
                    print_string_view(parameter.name);
                    if (!string_view_is_empty(parameter.constraint)) {
                        printf(": ");
                        print_string_view(parameter.constraint);
                    }
                }
                printf(">");
            }
            printf(" (");

            for (int i = 0;
                 i < node->as.func_decl.params.count;
                 i++)
            {
                print_node(node->as.func_decl.params.items[i]);

                if (i + 1 < node->as.func_decl.params.count ||
                    node->as.func_decl.is_variadic)
                    printf(" ");
            }

            if (node->as.func_decl.is_variadic)
                printf("...");

            printf(") -> ");
            print_type(node->as.func_decl.return_type);

            if (node->as.func_decl.body) {
                printf(" ");
                print_node(node->as.func_decl.body);
            }

            printf(")");
            break;

        case NODE_STRUCT_DECL:
            printf(node->as.struct_decl.is_union
                ? "(union "
                : node->as.struct_decl.is_resource
                    ? "(resource "
                    : "(struct ");
            if (node->as.struct_decl.is_repr_c) {
                printf("#repr(c");
                if (node->as.struct_decl.repr_c_packed)
                    printf(", packed");
                if (node->as.struct_decl.repr_c_align > 0)
                    printf(", align=%d", node->as.struct_decl.repr_c_align);
                printf(") ");
            }
            print_string_view(node->as.struct_decl.name);
            if (node->as.struct_decl.type_parameters.count > 0) {
                printf("<");
                for (int i = 0; i < node->as.struct_decl.type_parameters.count; i++) {
                    if (i > 0) printf(", ");
                    GenericTypeParameter parameter =
                        node->as.struct_decl.type_parameters.items[i];
                    print_string_view(parameter.name);
                    if (!string_view_is_empty(parameter.constraint)) {
                        printf(": ");
                        print_string_view(parameter.constraint);
                    }
                }
                printf(">");
            }
            if (node->as.struct_decl.is_incomplete)
                printf(" incomplete");
            printf(" ");

            for (int i = 0;
                 i < node->as.struct_decl.fields.count;
                 i++)
            {
                print_node(node->as.struct_decl.fields.items[i]);

                if (i + 1 < node->as.struct_decl.fields.count)
                    printf(" ");
            }

            for (int i = 0; i < node->as.struct_decl.methods.count; i++) {
                if (node->as.struct_decl.fields.count > 0 || i > 0)
                    printf(" ");
                print_node(node->as.struct_decl.methods.items[i]);
            }

            if (node->as.struct_decl.operators.count > 0) {
                if (node->as.struct_decl.fields.count > 0 ||
                    node->as.struct_decl.methods.count > 0)
                    printf(" ");
                printf("(operators");
                for (int i = 0; i < node->as.struct_decl.operators.count; i++) {
                    StructOperatorDecl mapping =
                        node->as.struct_decl.operators.items[i];
                    printf(" (");
                    if (mapping.is_unary)
                        printf("unary ");
                    printf("%s ", token_type_str(mapping.op));
                    print_string_view(mapping.method_name);
                    printf(")");
                }
                printf(")");
            }

            printf(")");

            break;

        case NODE_ARRAY_LITERAL:
            printf(node->as.array_literal.is_zero_initializer
                ? "zero_array_initializer\n"
                : "array_literal\n");
            for (int i = 0; i < node->as.array_literal.elements.count; i++) {
                print_node(node->as.array_literal.elements.items[i]);
            }

            break;

        default:
            UNREACHABLE("unknown ast node");
    }

    if (wrap_export)
        printf(")");
}

void ast_print(Node *node)
{
    print_node(node);
    printf("\n");
}

static void indent(int n)
{
    for (int i = 0; i < n; i++)
        printf("  ");
}

static void print_node_pretty(Node *node, int depth)
{
    switch (node->type)
    {
        case NODE_NUMBER:
            indent(depth);
            if (node->as.number.kind == NUMBER_LITERAL_FLOAT) {
                printf("%.17g\n", node->as.number.value.floating);
            } else {
                printf(
                    "%" PRIu64 "\n",
                    node->as.number.value.integer
                );
            }
            break;

        case NODE_IDENT:
            indent(depth);
            print_string_view_ln(node->as.ident);
            break;

        case NODE_STRING:
            indent(depth);
            print_string_view_quoted(node->as.string_literal);
            break;

        case NODE_CHAR:
            indent(depth);
            print_string_view_single_quoted(node->as.char_literal);
            break;

        case NODE_BOOL:
            indent(depth);
            printf("%s\n", node->as.boolean.value ? "true" : "false");
            break;

        case NODE_NULL:
            indent(depth);
            printf("null\n");
            break;

        case NODE_CAST:
            indent(depth);
            printf("%s\n",cast_kind_name(node->as.cast_expr.kind));
            indent(depth + 1);
            printf("target type:\n");

            indent(depth + 2);
            print_type(node->as.cast_expr.target_type);
            printf("\n");

            indent(depth + 1);
            printf("expression:\n");

            print_node_pretty(node->as.cast_expr.expression,depth + 2);

            break;

        case NODE_UNARY:
            indent(depth);
            printf("(%s)\n", token_type_str(node->as.unary.op));
            print_node_pretty(node->as.unary.operand, depth + 1);
            break;

        case NODE_BINARY:
            indent(depth);
            printf("(%s)\n", token_type_str(node->as.binary.op));
            print_node_pretty(node->as.binary.left, depth + 1);
            print_node_pretty(node->as.binary.right, depth + 1);
            break;

        case NODE_INC_DEC:
            indent(depth);

            if (node->as.inc_dec.is_prefix)
                printf("%s prefix\n", token_type_str(node->as.inc_dec.op));
            else
                printf("%s postfix\n", token_type_str(node->as.inc_dec.op));

            print_node_pretty(node->as.inc_dec.target, depth + 1);
            break;

        case NODE_ASSIGN:
            indent(depth);
            printf("(=)\n");
            print_node_pretty(node->as.assign.target, depth + 1);
            print_node_pretty(node->as.assign.value, depth + 1);
            break;

        case NODE_COMPOUND_ASSIGN:
            indent(depth);
            printf("compound_assign %s\n",
                token_type_str(node->as.compound_assign.op));

            indent(depth + 1);
            printf("target:\n");

            print_node_pretty(
                node->as.compound_assign.target,
                depth + 2
            );

            indent(depth + 1);
            printf("value:\n");

            print_node_pretty(
                node->as.compound_assign.value,
                depth + 2
            );

            break;

        case NODE_IF:
            indent(depth);
            printf("if\n");

            indent(depth + 1);
            printf("condition:\n");
            print_node_pretty(node->as.if_stmt.condition, depth + 2);

            indent(depth + 1);
            printf("then:\n");
            print_node_pretty(node->as.if_stmt.then_branch, depth + 2);

            if (node->as.if_stmt.else_branch) {
                indent(depth + 1);
                printf("else:\n");
                print_node_pretty(node->as.if_stmt.else_branch, depth + 2);
            }
            break;

        case NODE_CALL:
            indent(depth);
            printf("call");
            if (node->as.call.type_arguments.count > 0) {
                printf("::<");
                for (int i = 0; i < node->as.call.type_arguments.count; i++) {
                    if (i > 0) printf(", ");
                    print_type(node->as.call.type_arguments.items[i]);
                }
                printf(">");
            }
            printf("\n");
            print_node_pretty(node->as.call.callee, depth + 1);
            for (int i = 0; i < node->as.call.arguments.count; i++)
                print_node_pretty(node->as.call.arguments.items[i], depth + 1);
            break;

        case NODE_FIELD:
            indent(depth);
            printf("field .");
            print_string_view_ln(node->as.field.name);

            print_node_pretty(
                node->as.field.object,
                depth + 1
            );

            break;

        case NODE_INDEX:
            indent(depth);
            printf("index\n");

            print_node_pretty(node->as.index.object, depth + 1);
            print_node_pretty(node->as.index.index, depth + 1);
            break;

        case NODE_TYPE_REF:
            indent(depth);
            printf("type_ref ");
            print_type(node->as.type_ref.source_type);
            printf("\n");
            break;

        case NODE_EXPR_STMT:
            print_node_pretty(node->as.expr_stmt.expr, depth);
            break;

        case NODE_ASM:
            indent(depth);
            printf("asm%s \"%.*s\"\n",
                node->as.asm_stmt.is_volatile ? " volatile" : "",
                (int)node->as.asm_stmt.template_text.length,
                node->as.asm_stmt.template_text.data);
            indent(depth + 1); printf("output: %.*s\n",
                (int)node->as.asm_stmt.output_constraint.length,
                node->as.asm_stmt.output_constraint.data);
            print_node_pretty(node->as.asm_stmt.output, depth + 2);
            if (node->as.asm_stmt.input) {
                indent(depth + 1); printf("input: %.*s\n",
                    (int)node->as.asm_stmt.input_constraint.length,
                    node->as.asm_stmt.input_constraint.data);
                print_node_pretty(node->as.asm_stmt.input, depth + 2);
            }
            break;

        case NODE_STATIC_ASSERT:
            indent(depth);
            printf("static_assert\n");
            indent(depth + 1);
            printf("condition:\n");
            print_node_pretty(node->as.static_assert_stmt.condition, depth + 2);
            if (node->as.static_assert_stmt.message) {
                indent(depth + 1);
                printf("message:\n");
                print_node_pretty(node->as.static_assert_stmt.message, depth + 2);
            }
            break;

        case NODE_BLOCK:
            indent(depth);
            printf("block\n");

            for (int i = 0; i < node->as.block.statements.count; i++) {
                print_node_pretty(node->as.block.statements.items[i], depth + 1);
            }
            break;

        case NODE_ERROR:
            indent(depth);
            printf("<error>\n");
            break;

        case NODE_PROGRAM:
            printf("program\n");
            for (int i = 0; i < node->as.program.statements.count; i++)
                print_node_pretty(node->as.program.statements.items[i], depth + 1);
            break;

        case NODE_MODULE_DECL:
            indent(depth);
            printf("module ");
            print_string_view_ln(node->as.module_decl.name);
            break;

        case NODE_IMPORT_DECL:
            indent(depth);
            printf("import ");
            print_string_view(node->as.import_decl.name);
            if (node->as.import_decl.alias.length) {
                printf(" as ");
                print_string_view(node->as.import_decl.alias);
            }
            printf("\n");
            break;

        case NODE_VAR_DECL:
            indent(depth);

            if (node->is_exported)
                printf("export ");
            printf("var_decl ");
            print_string_view(node->as.var_decl.name);
            printf(": ");

            print_type(node->as.var_decl.var_type);
            printf("\n");

            if (node->as.var_decl.initializer)
            {
                indent(depth + 1);
                printf("init:\n");

                print_node_pretty(
                    node->as.var_decl.initializer,
                    depth + 2
                );
            }

            break;

        case NODE_VAR_DECL_GROUP:
            indent(depth);
            if (node->is_exported)
                printf("export ");
            printf("var_decl_group\n");
            for (int i = 0; i < node->as.var_decl_group.declarations.count; i++)
                print_node_pretty(node->as.var_decl_group.declarations.items[i], depth + 1);
            break;

        case NODE_FUNC_PARAM_DECL:
            indent(depth);

            printf("param_decl ");
            print_string_view(node->as.param_decl.name);
            printf(": ");

            print_type(node->as.param_decl.var_type);
            printf("\n");
            break;

        case NODE_STRUCT_FIELD_DECL:
            indent(depth);

            printf("struct_field_decl ");
            print_string_view(node->as.struct_field_decl.name);
            printf(": ");

            print_type(node->as.struct_field_decl.var_type);
            printf("\n");

            break;


        case NODE_STRUCT_INIT:
            indent(depth);

            printf("struct_init ");
            if (node->as.struct_init.module_name.length != 0) {
                print_string_view(node->as.struct_init.module_name);
                printf(".");
            }
            print_string_view(node->as.struct_init.name);
            if (node->as.struct_init.type_arguments.count > 0) {
                printf("::<");
                for (int i = 0; i < node->as.struct_init.type_arguments.count; i++) {
                    if (i > 0) printf(", ");
                    print_type(node->as.struct_init.type_arguments.items[i]);
                }
                printf(">");
            }
            printf("\n");

            for (int i = 0; i < node->as.struct_init.fields.count; i++)
            {
                print_node_pretty(
                    node->as.struct_init.fields.items[i],
                    depth + 1
                );
            }

            break;

        case NODE_ENUM_DECL:
            indent(depth);

            if (node->is_exported)
                printf("export ");
            if (node->as.enum_decl.is_repr_c)
                printf("#repr(c) ");
            printf(
                "enum_decl %.*s\n",
                (int)node->as.enum_decl.name.length,
                node->as.enum_decl.name.data
            );

            indent(depth + 1);
            printf("Backing type\n");

            indent(depth + 2);

            if (node->as.enum_decl.backing_type) {
                print_type(node->as.enum_decl.backing_type);
            } else {
                printf("<default>");
            }

            printf("\n");
            indent(depth + 1);
            printf("Members\n");

            for (int i = 0;
                 i < node->as.enum_decl.members.count;
                 i++) {

                print_node_pretty(
                    node->as.enum_decl.members.items[i],
                    depth + 2
                );
                 }

            break;

        case NODE_ENUM_MEMBER:
            indent(depth);

            printf(
                "EnumMember %.*s\n",
                (int)node->as.enum_member.name.length,
                node->as.enum_member.name.data
            );

            if (node->as.enum_member.value) {
                indent(depth + 1);
                printf("Explicit value\n");

                print_node_pretty(
                    node->as.enum_member.value,
                    depth + 2
                );
            }

            break;

        case NODE_FIELD_INIT:
            indent(depth);

            printf("field_init ");
            print_string_view_ln(node->as.field_init.name);

            if (node->as.field_init.value)
            {
                print_node_pretty(
                    node->as.field_init.value,
                    depth + 1
                );
            }

            break;

        case NODE_CONST_DECL:
            indent(depth);

            if (node->is_exported)
                printf("export ");
            printf("const_decl ");
            print_string_view(node->as.const_decl.name);
            printf(": ");
            print_type(node->as.const_decl.const_type);
            printf("\n");

            indent(depth + 1);
            printf("value:\n");

            print_node_pretty(
                node->as.const_decl.value,
                depth + 2
            );

            break;

        case NODE_RETURN:
            indent(depth);
            printf("return\n");
            if (node->as.return_stmt.value)
                print_node_pretty(node->as.return_stmt.value, depth + 1);
            break;

        case NODE_DEFER:
            indent(depth);
            printf("defer\n");
            print_node_pretty(node->as.defer_stmt.statement, depth + 1);
            break;

        case NODE_SWITCH:
            indent(depth);
            printf("switch\n");

            indent(depth + 1);
            printf("expression:\n");

            print_node_pretty(
                node->as.switch_stmt.expression,
                depth + 2
            );

            indent(depth + 1);
            printf("cases:\n");

            for (int i = 0; i < node->as.switch_stmt.cases.count; i++) {
                print_node_pretty(
                    node->as.switch_stmt.cases.items[i],
                    depth + 2
                );
            }

            break;

        case NODE_SWITCH_CASE:
            indent(depth);

            if (node->as.switch_case.is_default) {
                printf("default\n");
            } else {
                printf("case\n");

                indent(depth + 1);
                printf("value:\n");

                print_node_pretty(
                    node->as.switch_case.value,
                    depth + 2
                );
            }

            indent(depth + 1);
            printf("body:\n");

            print_node_pretty(
                node->as.switch_case.body,
                depth + 2
            );

            break;

        case NODE_WHILE:
            indent(depth);
            printf("while\n");

            indent(depth + 1);
            printf("condition:\n");
            print_node_pretty(node->as.while_stmt.condition, depth + 2);

            indent(depth + 1);
            printf("body:\n");
            print_node_pretty(node->as.while_stmt.body, depth + 2);
            break;

        case NODE_FOR:
            indent(depth);
            printf("for\n");

            if (node->as.for_stmt.condition) {
                indent(depth + 1);
                printf("condition:\n");
                print_node_pretty(node->as.for_stmt.condition, depth + 2);
            }

            if (node->as.for_stmt.post) {
                indent(depth + 1);
                printf("post:\n");
                print_node_pretty(node->as.for_stmt.post, depth + 2);
            }

            indent(depth + 1);
            printf("body:\n");
            print_node_pretty(node->as.for_stmt.body, depth + 2);
            break;

        case NODE_BREAK:
            indent(depth);
            printf("break\n");
            break;

        case NODE_CONTINUE:
            indent(depth);
            printf("continue\n");
            break;


        case NODE_FUNC_DECL:
            indent(depth);

            if (node->is_exported)
                printf("export ");
            if (node->as.func_decl.is_discardable)
                printf("#discardable ");
            if (node->as.func_decl.linkage == FUNCTION_LINKAGE_EXTERN_C) {
                const char *call = c_calling_convention_name(node->as.func_decl.c_call_conv);
                if (string_view_is_empty(node->as.func_decl.external_name) && !call) {
                    printf("extern(c) func ");
                } else {
                    printf("extern(c");
                    if (!string_view_is_empty(node->as.func_decl.external_name)) {
                        printf(", name=");
                        print_escaped_string_view_inline(node->as.func_decl.external_name);
                    }
                    if (call)
                        printf(", call=%s", call);
                    printf(") func ");
                }
            } else {
                if (node->as.func_decl.is_repr_c) {
                    const char *call = c_calling_convention_name(node->as.func_decl.c_call_conv);
                    if (call)
                        printf("#repr(c, call=%s) ", call);
                    else
                        printf("#repr(c) ");
                }
                printf("func ");
            }

            print_string_view(node->as.func_decl.name);
            if (node->as.func_decl.type_parameters.count > 0) {
                printf("<");
                for (int i = 0; i < node->as.func_decl.type_parameters.count; i++) {
                    if (i > 0) printf(", ");
                    GenericTypeParameter parameter =
                        node->as.func_decl.type_parameters.items[i];
                    print_string_view(parameter.name);
                    if (!string_view_is_empty(parameter.constraint)) {
                        printf(": ");
                        print_string_view(parameter.constraint);
                    }
                }
                printf(">");
            }
            printf(" -> ");

            print_type(node->as.func_decl.return_type);

            printf("\n");


            if (node->as.func_decl.params.count > 0)
            {
                indent(depth + 1);
                printf("params:\n");


                for (int i = 0; i < node->as.func_decl.params.count; i++)
                {
                    print_node_pretty(
                        node->as.func_decl.params.items[i],
                        depth + 2
                    );
                }
            }

            if (node->as.func_decl.is_variadic) {
                indent(depth + 1);
                printf("variadic: c\n");
            }

            if (node->as.func_decl.body) {
                indent(depth + 1);
                printf("body:\n");

                print_node_pretty(
                    node->as.func_decl.body,
                    depth + 2
                );
            }

            break;

        case NODE_STRUCT_DECL:
            indent(depth);

            if (node->is_exported)
                printf("export ");
            if (node->as.struct_decl.is_repr_c) {
                printf("#repr(c");
                if (node->as.struct_decl.repr_c_packed)
                    printf(", packed");
                if (node->as.struct_decl.repr_c_align > 0)
                    printf(", align=%d", node->as.struct_decl.repr_c_align);
                printf(") ");
            }
            printf(node->as.struct_decl.is_union
                ? "union "
                : node->as.struct_decl.is_resource
                    ? "resource "
                    : "struct ");
            print_string_view(node->as.struct_decl.name);
            if (node->as.struct_decl.type_parameters.count > 0) {
                printf("<");
                for (int i = 0; i < node->as.struct_decl.type_parameters.count; i++) {
                    if (i > 0) printf(", ");
                    GenericTypeParameter parameter =
                        node->as.struct_decl.type_parameters.items[i];
                    print_string_view(parameter.name);
                    if (!string_view_is_empty(parameter.constraint)) {
                        printf(": ");
                        print_string_view(parameter.constraint);
                    }
                }
                printf(">");
            }
            if (node->as.struct_decl.is_incomplete)
                printf(" incomplete");
            printf("\n");


            for (int i = 0; i < node->as.struct_decl.fields.count; i++)
            {
                print_node_pretty(
                    node->as.struct_decl.fields.items[i],
                    depth + 1
                );
            }


            for (int i = 0; i < node->as.struct_decl.methods.count; i++)
            {
                print_node_pretty(
                    node->as.struct_decl.methods.items[i],
                    depth + 1
                );
            }

            if (node->as.struct_decl.operators.count > 0) {
                indent(depth + 1);
                printf("operators:\n");
                for (int i = 0; i < node->as.struct_decl.operators.count; i++) {
                    StructOperatorDecl mapping =
                        node->as.struct_decl.operators.items[i];
                    indent(depth + 2);
                    if (mapping.is_unary)
                        printf("unary ");
                    printf("%s -> ", token_type_str(mapping.op));
                    print_string_view(mapping.method_name);
                    printf("\n");
                }
            }

            break;

        case NODE_ARRAY_LITERAL:
            indent(depth);
            printf(node->as.array_literal.is_zero_initializer
                ? "zero_array_initializer\n"
                : "array_literal\n");

            for (int i = 0; i < node->as.array_literal.elements.count; i++) {
                print_node_pretty(node->as.array_literal.elements.items[i], depth + 1);
            }

            break;

        default:
            indent(depth);
            printf("<UNKNOWN NODE TYPE: %d>\n", node->type);
            break;
    }
}

void ast_pretty_print(Node *node)
{
    print_node_pretty(node, 0);
    printf("\n");
}
