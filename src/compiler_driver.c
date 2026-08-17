#include "compiler_driver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "parser_diag.h"
#include "utils/utils.h"

typedef struct CompileInputFile {
    const char *filename;
    SourceFileId source_id;
    Node *program;

    unsigned long long device_id;
    unsigned long long inode_id;
    int has_file_identity;

    struct CompileInputFile *next;
} CompileInputFile;

typedef struct CompileInputList {
    CompileInputFile *first;
    CompileInputFile *last;
} CompileInputList;

static void report_parser_errors(const CompileResult *result) {

    parser_print_diagnostics(&result->parser);

    fprintf(
        stderr,
        "%zu parser error%s generated.\n",
        result->parser.diagnostic_count,
        result->parser.diagnostic_count == 1 ? "" : "s"
    );
}

static void report_semantic_error_summary(const CompileResult *result) {

    diagnostic_print_all(stderr, &result->sources, &result->sem.diagnostics);

    fprintf(
        stderr,
        "%d semantic error%s generated.\n",
        result->sem.error_count,
        result->sem.error_count == 1 ? "" : "s"
    );
}

static void append_parser_diagnostics(Parser *aggregate, const Parser *parsed)
{
    if (!aggregate || !parsed)
        return;

    aggregate->had_error = aggregate->had_error || parsed->had_error;
    aggregate->diagnostic_count += parsed->diagnostic_count;
    aggregate->diagnostics.count += parsed->diagnostics.count;

    if (!parsed->diagnostics.first)
        return;

    if (aggregate->diagnostics.last) {
        aggregate->diagnostics.last->next = parsed->diagnostics.first;
    } else {
        aggregate->diagnostics.first = parsed->diagnostics.first;
    }

    aggregate->diagnostics.last = parsed->diagnostics.last;
}

static int string_view_equal(StringView left, StringView right)
{
    return left.length == right.length &&
           (left.length == 0 || memcmp(left.data, right.data, left.length) == 0);
}

static StringView file_program_module_name(const Node *program)
{
    if (!program || program->type != NODE_PROGRAM)
        return string_view_empty();

    for (int i = 0; i < program->as.program.statements.count; ++i) {
        const Node *stmt = program->as.program.statements.items[i];
        if (stmt->type == NODE_MODULE_DECL)
            return stmt->as.module_decl.name;
    }

    return string_view_empty();
}

static int input_list_declares_module(
    const CompileInputList *inputs,
    StringView module_name
) {
    for (const CompileInputFile *file = inputs->first; file; file = file->next) {
        if (string_view_equal(file_program_module_name(file->program), module_name))
            return 1;
    }

    return 0;
}

static int file_identity(
    const char *path,
    unsigned long long *device_id,
    unsigned long long *inode_id
) {
    struct stat info;
    if (!path || stat(path, &info) != 0)
        return 0;

    if (device_id)
        *device_id = (unsigned long long)info.st_dev;
    if (inode_id)
        *inode_id = (unsigned long long)info.st_ino;
    return 1;
}

static int input_list_contains_file(
    const CompileInputList *inputs,
    const char *path
) {
    unsigned long long device_id = 0;
    unsigned long long inode_id = 0;
    int has_identity = file_identity(path, &device_id, &inode_id);

    for (const CompileInputFile *file = inputs->first; file; file = file->next) {
        if (strcmp(file->filename, path) == 0)
            return 1;

        if (has_identity && file->has_file_identity &&
            file->device_id == device_id && file->inode_id == inode_id) {
            return 1;
        }
    }

    return 0;
}

static int compile_result_append_source_buffer(
    CompileResult *out,
    char *source
) {
    char **grown = realloc(
        out->source_buffers,
        (out->source_buffer_count + 1) * sizeof(*grown)
    );
    if (!grown)
        return 0;

    out->source_buffers = grown;
    out->source_buffers[out->source_buffer_count++] = source;
    if (out->source_buffer_count == 1)
        out->source = source;
    return 1;
}

static CompileInputFile *load_and_parse_input(
    CompileResult *out,
    CompileInputList *inputs,
    const char *filename
) {
    char *source = read_file(filename);
    if (!source) {
        fprintf(stderr, "error: could not read '%s'\n", filename);
        return NULL;
    }

    if (!compile_result_append_source_buffer(out, source)) {
        free(source);
        fprintf(stderr, "error: could not grow source buffer table\n");
        return NULL;
    }

    const char *owned_filename = arena_strdup_len(
        out->arena,
        filename,
        strlen(filename)
    );
    SourceFileId source_id = source_manager_add(
        &out->sources,
        owned_filename,
        source
    );

    Parser parser;
    parser_init_with_source(
        &parser,
        &out->sources,
        source_id,
        out->arena,
        out->scratch
    );

    Node *file_program = parse_program(&parser);
    append_parser_diagnostics(&out->parser, &parser);

    CompileInputFile *file = arena_new(out->arena, CompileInputFile);
    memset(file, 0, sizeof(*file));
    file->filename = owned_filename;
    file->source_id = source_id;
    file->program = file_program;
    file->has_file_identity = file_identity(
        filename,
        &file->device_id,
        &file->inode_id
    );

    if (inputs->last) {
        inputs->last->next = file;
    } else {
        inputs->first = file;
    }
    inputs->last = file;

    return file;
}

static char *module_candidate_path(
    Arena *arena,
    const char *directory,
    StringView module_name
) {
    size_t dir_len = directory ? strlen(directory) : 0;
    int needs_separator = dir_len != 0 &&
        directory[dir_len - 1] != '/' && directory[dir_len - 1] != '\\';
    size_t total = dir_len + (needs_separator ? 1 : 0) +
        module_name.length + sizeof(".cog");

    char *path = arena_alloc(arena, total);
    size_t at = 0;

    if (dir_len) {
        memcpy(path + at, directory, dir_len);
        at += dir_len;
    }
    if (needs_separator)
        path[at++] = '/';

    for (size_t i = 0; i < module_name.length; ++i) {
        char c = module_name.data[i];
        path[at++] = c == '.' ? '/' : c;
    }
    memcpy(path + at, ".cog", sizeof(".cog"));
    return path;
}

static char *source_directory(Arena *arena, const char *filename)
{
    const char *last_slash = strrchr(filename, '/');
    const char *last_backslash = strrchr(filename, '\\');
    const char *separator = last_slash;
    if (!separator || (last_backslash && last_backslash > separator))
        separator = last_backslash;

    if (!separator)
        return arena_strdup_len(arena, ".", 1);

    size_t length = (size_t)(separator - filename);
    if (length == 0)
        length = 1;

    return arena_strdup_len(arena, filename, length);
}

static const char *find_module_source(
    CompileResult *out,
    const CompileOptions *options,
    const CompileInputFile *importer,
    StringView module_name
) {
    ArenaMarker marker = arena_mark(out->scratch);

    char *directory = source_directory(out->scratch, importer->filename);
    char *candidate = module_candidate_path(
        out->scratch,
        directory,
        module_name
    );

    struct stat info;
    if (stat(candidate, &info) == 0) {
        const char *result = arena_strdup_len(
            out->arena,
            candidate,
            strlen(candidate)
        );
        arena_reset_to(out->scratch, marker);
        return result;
    }

    for (size_t i = 0; i < options->module_search_dir_count; ++i) {
        const char *search_dir = options->module_search_dirs[i];
        candidate = module_candidate_path(
            out->scratch,
            search_dir,
            module_name
        );
        if (stat(candidate, &info) == 0) {
            const char *result = arena_strdup_len(
                out->arena,
                candidate,
                strlen(candidate)
            );
            arena_reset_to(out->scratch, marker);
            return result;
        }
        arena_reset_to(out->scratch, marker);
        marker = arena_mark(out->scratch);
    }

    arena_reset_to(out->scratch, marker);
    return NULL;
}

static int discover_import_sources(
    CompileResult *out,
    CompileInputList *inputs,
    const CompileOptions *options
) {
    if (!options->discover_imports)
        return 1;

    for (CompileInputFile *file = inputs->first; file; file = file->next) {
        Node *program = file->program;
        if (!program || program->type != NODE_PROGRAM)
            continue;

        for (int i = 0; i < program->as.program.statements.count; ++i) {
            Node *stmt = program->as.program.statements.items[i];
            if (stmt->type != NODE_IMPORT_DECL)
                continue;

            StringView module_name = stmt->as.import_decl.name;
            if (input_list_declares_module(inputs, module_name))
                continue;

            const char *candidate = find_module_source(
                out,
                options,
                file,
                module_name
            );
            if (!candidate)
                continue;

            if (input_list_contains_file(inputs, candidate))
                continue;

            if (!load_and_parse_input(out, inputs, candidate))
                return 0;

            if (out->parser.had_error)
                return 1;
        }
    }

    return 1;
}

static Node *combine_input_programs(
    CompileResult *out,
    const CompileInputList *inputs
) {
    if (!inputs->first || !inputs->first->program)
        return NULL;

    Node *program = ast_new_program(out->arena, inputs->first->program->span);
    for (const CompileInputFile *file = inputs->first; file; file = file->next) {
        Node *file_program = file->program;
        if (!file_program)
            continue;

        for (int i = 0; i < file_program->as.program.statements.count; ++i) {
            nodelist_push(
                out->arena,
                &program->as.program.statements,
                file_program->as.program.statements.items[i]
            );
        }
    }

    return program;
}

CompileOptions compile_options_default(void)
{
    CompileOptions options;
    memset(&options, 0, sizeof(options));
    return options;
}

CompileStatus compile_parse_and_check(const char *filename, CompileResult *out) {
    const char *filenames[1] = { filename };
    return compile_parse_and_check_files(filenames, 1, out);
}

CompileStatus compile_parse_and_check_files(
    const char *const *filenames,
    size_t filename_count,
    CompileResult *out
) {
    CompileOptions options = compile_options_default();
    return compile_parse_and_check_files_with_options(
        filenames,
        filename_count,
        &options,
        out
    );
}

CompileStatus compile_parse_and_check_files_with_options(
    const char *const *filenames,
    size_t filename_count,
    const CompileOptions *options,
    CompileResult *out
) {
    TargetInfo target = target_info_host();
    return compile_parse_and_check_files_for_target_with_options(
        filenames,
        filename_count,
        &target,
        options,
        out
    );
}

CompileStatus compile_parse_and_check_for_target(
    const char *filename,
    const TargetInfo *target,
    CompileResult *out
) {
    const char *filenames[1] = { filename };
    return compile_parse_and_check_files_for_target(filenames, 1, target, out);
}

CompileStatus compile_parse_and_check_files_for_target(
    const char *const *filenames,
    size_t filename_count,
    const TargetInfo *target,
    CompileResult *out
) {
    CompileOptions options = compile_options_default();
    return compile_parse_and_check_files_for_target_with_options(
        filenames,
        filename_count,
        target,
        &options,
        out
    );
}

CompileStatus compile_parse_and_check_files_for_target_with_options(
    const char *const *filenames,
    size_t filename_count,
    const TargetInfo *target,
    const CompileOptions *options,
    CompileResult *out
) {
    if (!out) return COMPILE_STATUS_DRIVER_ERROR;

    memset(out, 0, sizeof(*out));
    out->status = COMPILE_STATUS_DRIVER_ERROR;

    CompileOptions default_options = compile_options_default();
    if (!options)
        options = &default_options;

    if (options->module_search_dir_count != 0 && !options->module_search_dirs) {
        fprintf(stderr, "error: module search path count requires module search paths\n");
        return out->status;
    }

    if (!target) {
        fprintf(stderr, "error: no target description\n");
        return out->status;
    }

    char target_error[160];
    if (!target_info_validate(target, target_error, sizeof(target_error))) {
        fprintf(stderr, "error: invalid target description: %s\n", target_error);
        return out->status;
    }
    out->target = *target;

    if (!filenames || filename_count == 0 || !filenames[0]) {
        fprintf(stderr, "error: no input file\n");
        return out->status;
    }

    for (size_t i = 0; i < options->module_search_dir_count; ++i) {
        if (!options->module_search_dirs[i] || options->module_search_dirs[i][0] == '\0') {
            fprintf(stderr, "error: module search paths must be non-empty\n");
            return out->status;
        }
    }

    out->filename = filenames[0];

    out->arena = arena_create(MB(2));
    out->scratch = arena_create(MB(1));
    if (!out->arena || !out->scratch) {
        fprintf(stderr, "error: could not allocate compiler arenas\n");
        return out->status;
    }

    source_manager_init(&out->sources, out->arena);

    memset(&out->parser, 0, sizeof(out->parser));
    out->parser.arena = out->arena;
    out->parser.scratch = out->scratch;
    out->parser.sources = &out->sources;
    out->parser.source_id = SOURCE_FILE_ID_INVALID;
    diagnostic_list_init(&out->parser.diagnostics, out->arena);

    CompileInputList inputs;
    memset(&inputs, 0, sizeof(inputs));

    for (size_t i = 0; i < filename_count; ++i) {
        if (!filenames[i]) {
            fprintf(stderr, "error: null input filename\n");
            return out->status;
        }

        CompileInputFile *file = load_and_parse_input(out, &inputs, filenames[i]);
        if (!file)
            return out->status;

        if (i == 0)
            out->primary_source_id = file->source_id;
    }

    if (out->parser.had_error) {
        out->program = combine_input_programs(out, &inputs);
        report_parser_errors(out);
        out->status = COMPILE_STATUS_PARSE_ERROR;
        return out->status;
    }

    if (!discover_import_sources(out, &inputs, options))
        return out->status;

    out->program = combine_input_programs(out, &inputs);

    if (out->parser.had_error) {
        report_parser_errors(out);
        out->status = COMPILE_STATUS_PARSE_ERROR;
        return out->status;
    }

    out->sem.arena = out->arena;
    semantic_check(out->program, &out->sem, &out->target, &out->sources);

    if (out->sem.had_error) {
        report_semantic_error_summary(out);
        out->status = COMPILE_STATUS_SEMANTIC_ERROR;
        return out->status;
    }

    out->status = COMPILE_STATUS_OK;
    return out->status;
}

void compile_result_destroy(CompileResult *result)
{
    if (!result) return;

    if (result->scratch) arena_destroy(result->scratch);
    if (result->arena)   arena_destroy(result->arena);

    if (result->source_buffers) {
        for (size_t i = 0; i < result->source_buffer_count; ++i)
            free(result->source_buffers[i]);
        free(result->source_buffers);
    } else {
        free(result->source);
    }

    memset(result, 0, sizeof(*result));
}

int status_to_exit_code(CompileStatus status) {
    switch (status) {
        case COMPILE_STATUS_OK:
            return 0;

        case COMPILE_STATUS_SEMANTIC_ERROR:
            return 1;

        case COMPILE_STATUS_PARSE_ERROR:
        case COMPILE_STATUS_DRIVER_ERROR:
            return 2;
    }

    return 2;
}
