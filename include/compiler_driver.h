#ifndef COGLET_COMPILER_DRIVER_H
#define COGLET_COMPILER_DRIVER_H

#include "ast.h"
#include "parser.h"
#include "semantic_anal.h"
#include "utils/arena.h"

typedef enum CompileStatus {
    COMPILE_STATUS_OK = 0,
    COMPILE_STATUS_SEMANTIC_ERROR = 1,

    /*
     * Parser and driver failures intentionally share exit status 2.
     * Separate names let callers distinguish their meaning when needed.
     */
    COMPILE_STATUS_PARSE_ERROR = 2,
    COMPILE_STATUS_DRIVER_ERROR = 2,
} CompileStatus;

typedef struct CompileResult {
    CompileStatus status;

    /*
    * Borrowed from the caller and retained by parser/lexer state.
    */
    const char *filename;

    /*
     * Owned source buffer returned by read_file().
     */
    char *source;

    /*
     * Both arenas are owned by this result.
     *
     * AST, parser diagnostics, semantic types, symbols, scopes, and
     * semantic expression information are backed by these arenas.
     */
    Arena *arena;
    Arena *scratch;

    Parser parser;
    Node *program;
    SemanticContext sem;
} CompileResult;

/*
 * Initializes out and runs the source-file parse/check pipeline.
 *
 * Ownership after return:
 *   - out->filename is borrowed
 *   - out->source is owned by out
 *   - out->arena and out->scratch are owned by out
 *   - out->program, parser diagnostics, and semantic data remain valid until
 *     compile_result_destroy(out)
 *
 * The result may be destroyed after any returned status.
 *
 * On COMPILE_STATUS_OK:
 *   - program and semantic state are available for later compiler phases
 *
 * On COMPILE_STATUS_SEMANTIC_ERROR:
 *   - program is available
 *   - semantic state may be partial and is intended only for diagnostics or
 *     debugging tools
 *
 * On COMPILE_STATUS_PARSE_ERROR:
 *   - parser state and a partial program may be available
 *   - semantic analysis has not run
 *
 * Diagnostics:
 *   - parser diagnostics are printed here after parsing
 *   - semantic_check() prints individual semantic diagnostics
 *   - this function prints the semantic error summary
 */
CompileStatus compile_parse_and_check(const char *filename, CompileResult *out);

/*
 * Releases all resources owned by result.
 *
 * Safe to call after any normal return from compile_parse_and_check().
 * After this call, all AST, parser, and semantic pointers from result are
 * invalid.
 */
void compile_result_destroy(CompileResult *result);

#endif