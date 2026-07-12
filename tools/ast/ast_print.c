#include "ast_print.h"

#include <stdio.h>
#include <stdlib.h>

#include "../../include/types.h"
#include "../../include/utils/utils.h"

static const char *token_type_str(TokenType t)
{
    switch (t) {
        case TOK_PLUS:    return "+";
        case TOK_MINUS:   return "-";
        case TOK_PLUS_PLUS:   return "++";
        case TOK_MINUS_MINUS: return "--";
        case TOK_STAR:    return "*";
        case TOK_SLASH:   return "/";
        case TOK_PERCENT: return "%";

        case TOK_EQUAL:       return "=";
        case TOK_EQUAL_EQUAL: return "==";
        case TOK_BANG:        return "!";
        case TOK_BANG_EQUAL:  return "!=";

        case TOK_LESS:          return "<";
        case TOK_LESS_EQUAL:    return "<=";
        case TOK_GREATER:       return ">";
        case TOK_GREATER_EQUAL: return ">=";

        case TOK_AND_AND: return "&&";
        case TOK_OR_OR: return "||";

        case TOK_DOT: return ".";
        case TOK_LBRACKET: return "[";
        case TOK_RBRACKET: return "]";
        case TOK_LPAREN: return "(";
        case TOK_RPAREN: return ")";

        case TOK_IDENT: return "IDENT";
        case TOK_NUMBER_INT: return "INT";
        case TOK_NUMBER_FLOAT: return "FLOAT";

        default: return "?";
    }
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
        case TYPE_I8:   printf("i8");   break;
        case TYPE_I16:  printf("i16");  break;
        case TYPE_I32:  printf("i32");  break;
        case TYPE_I64:  printf("i64");  break;
        case TYPE_U8:   printf("u8");   break;
        case TYPE_U16:  printf("u16");  break;
        case TYPE_U32:  printf("u32");  break;
        case TYPE_U64:  printf("u64");  break;
        case TYPE_F32:  printf("f32");  break;
        case TYPE_F64:  printf("f64");  break;

        case TYPE_POINTER:
            print_type(t->element);
            printf("*");
            break;

        case TYPE_ARRAY:
            print_type(t->element);

            if (t->array_size >= 0)
                printf("[%d]", t->array_size);
            else
                printf("[]");

            break;

        case TYPE_NAMED:
            print_string_view(t->named_name);
            break;

        case TYPE_STRUCT:
            printf("struct ");
            print_string_view(t->struct_name);
            break;

        case TYPE_ENUM:
            printf("enum ");
            print_string_view(t->enum_name);
            break;

        default:
            printf("<unknown-type>");
            break;
    }
}

static void print_node(Node *node)
{
    switch (node->type)
    {
        case NODE_NUMBER:
            if (node->as.number.is_float)
                printf("%g", node->as.number.value);
            else
                printf("%lld", (long long)node->as.number.value);
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
            printf("(call ");
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


        case NODE_FUNC_PARAM_DECL:
            printf("(param_decl ");

            print_type(node->as.param_decl.var_type);

            printf(" ");

            print_string_view(node->as.param_decl.name);


            if (node->as.param_decl.default_value) {
                printf(" = ");
                print_node(node->as.param_decl.default_value);
            }

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
            print_string_view(node->as.struct_init.name);

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
            printf("(enum %.*s (",
                (int)node->as.enum_decl.name.length,
                node->as.enum_decl.name.data);

            print_type(node->as.enum_decl.backing_type);

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
            printf("(func ");
            print_string_view(node->as.func_decl.name);
            printf(" (");

            for (int i = 0;
                 i < node->as.func_decl.params.count;
                 i++)
            {
                print_node(node->as.func_decl.params.items[i]);

                if (i + 1 < node->as.func_decl.params.count)
                    printf(" ");
            }

            printf(") -> ");
            print_type(node->as.func_decl.return_type);
            printf(" ");
            print_node(node->as.func_decl.body);
            printf(")");
            break;

        case NODE_STRUCT_DECL:
            printf("(struct ");
            print_string_view(node->as.struct_decl.name);
            printf(" ");

            for (int i = 0;
                 i < node->as.struct_decl.fields.count;
                 i++)
            {
                print_node(node->as.struct_decl.fields.items[i]);

                if (i + 1 < node->as.struct_decl.fields.count)
                    printf(" ");
            }

            printf(")");

            break;

        default:
            UNREACHABLE("unknown ast node");
    }
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
            if (node->as.number.is_float)
                printf("%g\n", node->as.number.value);
            else
                printf("%lld\n", (long long)node->as.number.value);
            break;

        case NODE_IDENT:
            indent(depth);
            print_string_view_ln(node->as.ident);
            break;

        case NODE_STRING:
            indent(depth);
            print_string_view_ln(node->as.string_literal);
            break;

        case NODE_CHAR:
            indent(depth);
            print_string_view_ln(node->as.char_literal);
            break;

        case NODE_BOOL:
            indent(depth);
            printf("%s\n", node->as.boolean.value ? "true" : "false");
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
            printf("call\n");
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

        case NODE_EXPR_STMT:
            print_node_pretty(node->as.expr_stmt.expr, depth);
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

        case NODE_VAR_DECL:
            indent(depth);

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

        case NODE_FUNC_PARAM_DECL:
            indent(depth);

            printf("param_decl ");
            print_string_view(node->as.param_decl.name);
            printf(": ");

            print_type(node->as.param_decl.var_type);
            printf("\n");

            if (node->as.param_decl.default_value)
            {
                indent(depth + 1);
                printf("default:\n");

                print_node_pretty(
                    node->as.param_decl.default_value,
                    depth + 2
                );
            }

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
            print_string_view_ln(node->as.struct_init.name);

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

            printf(
                "enum_decl %.*s\n",
                (int)node->as.enum_decl.name.length,
                node->as.enum_decl.name.data
            );

            indent(depth + 1);
            printf("Backing type\n");

            indent(depth + 2);
            print_type(node->as.enum_decl.backing_type);
            printf("\n");

            /*
             * Use your existing type-printing helper here.
             */
            // TODO: FIX THIS and above

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

            printf("func ");
            print_string_view(node->as.func_decl.name);
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

            indent(depth + 1);
            printf("body:\n");

            print_node_pretty(
                node->as.func_decl.body,
                depth + 2
            );

            break;

        case NODE_STRUCT_DECL:
            indent(depth);

            printf("struct ");
            print_string_view_ln(node->as.struct_decl.name);


            for (int i = 0; i < node->as.struct_decl.fields.count; i++)
            {
                print_node_pretty(
                    node->as.struct_decl.fields.items[i],
                    depth + 1
                );
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
