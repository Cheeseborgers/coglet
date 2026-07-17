#ifndef COGLET_SEMANTIC_INFO_VERIFIER_H
#define COGLET_SEMANTIC_INFO_VERIFIER_H

#include <stdio.h>

#include "ast.h"
#include "semantic_anal.h"

typedef struct SemanticInfoVerification {
    int expression_count;
    int mutation_count;
    int table_entry_count;
    int error_count;
} SemanticInfoVerification;

int semantic_info_verify_program(
    SemanticContext *sem,
    Node *program,
    FILE *diagnostics,
    SemanticInfoVerification *verification
);

void semantic_info_dump_program(
    SemanticContext *sem,
    Node *program,
    FILE *output
);

#endif
