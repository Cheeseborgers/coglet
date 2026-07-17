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
     * Borrowed from the caller. This string must remain valid until
     * compile_result_destroy() is called.
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
 * Reads, parses, and semantically checks one source file.
 *
 * This function:
 *   - initializes *out
 *   - owns and reports parser diagnostics
 *   - runs semantic analysis
 *   - prints the semantic error summary
 *
 * Individual semantic diagnostics are printed by semantic_check().
 *
 * After this function returns, *out may always be passed to
 * compile_result_destroy(), regardless of the returned status.
 *
 * On COMPILE_STATUS_OK:
 *   program and sem contain successful frontend results.
 *
 * On COMPILE_STATUS_SEMANTIC_ERROR:
 *   program is valid and sem may contain partial semantic information.
 *
 * On COMPILE_STATUS_PARSE_ERROR:
 *   parser and program may contain partial parser results.
 */
CompileStatus compile_parse_and_check(const char *filename, CompileResult *out);

/*
 * Releases the source and both arenas.
 *
 * All pointers into the AST, parser diagnostics, and semantic context
 * become invalid after this call.
 */
void compile_result_destroy(CompileResult *result);

#endif