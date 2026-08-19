#include "compiler_driver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

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
        &out->target_config,
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


static int module_name_is_std_namespace(StringView module_name)
{
    if (module_name.length < 3 ||
        memcmp(module_name.data, "std", 3) != 0)
        return 0;

    return module_name.length == 3 || module_name.data[3] == '.';
}

typedef struct ModuleSourceSet {
    const char **items;
    size_t count;
} ModuleSourceSet;

static int compare_c_strings(const void *a, const void *b)
{
    const char *const *left = a;
    const char *const *right = b;
    return strcmp(*left, *right);
}

/* Discover either a traditional single-file module (foo.cog) or a package
 * directory (foo files ending in .cog). Package members are loaded in lexical order so the
 * compiler's behaviour and diagnostics are deterministic. */
static ModuleSourceSet find_module_sources(
    CompileResult *out,
    const CompileOptions *options,
    const CompileInputFile *importer,
    StringView module_name
) {
    ArenaMarker marker = arena_mark(out->scratch);
    const char *roots[2];
    size_t root_count = 0;
    roots[root_count++] = source_directory(out->scratch, importer->filename);

    if (options->stdlib_root && module_name_is_std_namespace(module_name))
        roots[root_count++] = options->stdlib_root;

    /* Preserve the existing precedence: importer directory, -I roots, stdlib. */
    for (size_t r = 0; r < 1; ++r) {
        char *single = module_candidate_path(out->scratch, roots[r], module_name);
        struct stat info;
        if (stat(single, &info) == 0 && S_ISREG(info.st_mode)) {
            const char **items = arena_alloc(out->arena, sizeof(*items));
            items[0] = arena_strdup_len(out->arena, single, strlen(single));
            arena_reset_to(out->scratch, marker);
            return (ModuleSourceSet){ items, 1 };
        }
    }
    for (size_t search = 0; search < options->module_search_dir_count; ++search) {
        char *single = module_candidate_path(out->scratch, options->module_search_dirs[search], module_name);
        struct stat info;
        if (stat(single, &info) == 0 && S_ISREG(info.st_mode)) {
            const char **items = arena_alloc(out->arena, sizeof(*items));
            items[0] = arena_strdup_len(out->arena, single, strlen(single));
            arena_reset_to(out->scratch, marker);
            return (ModuleSourceSet){ items, 1 };
        }
    }
    for (size_t r = 1; r < root_count; ++r) {
        char *single = module_candidate_path(out->scratch, roots[r], module_name);
        struct stat info;
        if (stat(single, &info) == 0 && S_ISREG(info.st_mode)) {
            const char **items = arena_alloc(out->arena, sizeof(*items));
            items[0] = arena_strdup_len(out->arena, single, strlen(single));
            arena_reset_to(out->scratch, marker);
            return (ModuleSourceSet){ items, 1 };
        }
    }

    /* Search package directories only when there is no single-file module. */
    for (size_t root_index = 0; root_index < root_count + options->module_search_dir_count; ++root_index) {
        const char *root = root_index < root_count ? roots[root_index]
            : options->module_search_dirs[root_index - root_count];
        size_t root_len = strlen(root);
        size_t path_len = root_len + 1 + module_name.length + 1;
        char *dir_path = arena_alloc(out->scratch, path_len);
        size_t at = 0;
        memcpy(dir_path + at, root, root_len); at += root_len;
        if (at && dir_path[at - 1] != '/' && dir_path[at - 1] != '\\') dir_path[at++] = '/';
        for (size_t i = 0; i < module_name.length; ++i) dir_path[at++] = module_name.data[i] == '.' ? '/' : module_name.data[i];
        dir_path[at] = '\0';

        DIR *dir = opendir(dir_path);
        if (!dir) continue;

        size_t capacity = 8, count = 0;
        const char **items = arena_alloc(out->scratch, capacity * sizeof(*items));
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            size_t name_len = strlen(entry->d_name);
            if (name_len < 5 || strcmp(entry->d_name + name_len - 4, ".cog") != 0)
                continue;
            size_t full_len = strlen(dir_path) + 1 + name_len + 1;
            char *full = arena_alloc(out->scratch, full_len);
            snprintf(full, full_len, "%s/%s", dir_path, entry->d_name);
            struct stat file_info;
            if (stat(full, &file_info) != 0 || !S_ISREG(file_info.st_mode)) continue;
            if (count == capacity) {
                capacity *= 2;
                const char **grown = arena_alloc(out->scratch, capacity * sizeof(*grown));
                memcpy(grown, items, count * sizeof(*grown));
                items = grown;
            }
            items[count++] = full;
        }
        closedir(dir);
        if (count) {
            qsort(items, count, sizeof(*items), compare_c_strings);
            const char **owned = arena_alloc(out->arena, count * sizeof(*owned));
            for (size_t i = 0; i < count; ++i)
                owned[i] = arena_strdup_len(out->arena, items[i], strlen(items[i]));
            arena_reset_to(out->scratch, marker);
            return (ModuleSourceSet){ owned, count };
        }
    }
    arena_reset_to(out->scratch, marker);
    return (ModuleSourceSet){ NULL, 0 };
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

            ModuleSourceSet candidates = find_module_sources(
                out, options, file, module_name
            );
            if (candidates.count == 0)
                continue;

            for (size_t source_index = 0; source_index < candidates.count; ++source_index) {
                const char *candidate = candidates.items[source_index];
                if (input_list_contains_file(inputs, candidate))
                    continue;
                if (!load_and_parse_input(out, inputs, candidate))
                    return 0;
                if (out->parser.had_error)
                    return 1;
            }
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

CompileStatus compile_parse_and_check(const char *filename, const TargetConfig *target_config, CompileResult *out) {
    const char *filenames[1] = { filename };
    return compile_parse_and_check_files(filenames, 1, target_config, out);
}

CompileStatus compile_parse_and_check_files(
    const char *const *filenames,
    size_t filename_count,
    const TargetConfig *target_config,
    CompileResult *out
) {
    CompileOptions options = compile_options_default();
    return compile_parse_and_check_files_with_options(
        filenames,
        filename_count,
        &options,
        target_config,
        out
    );
}

CompileStatus compile_parse_and_check_files_with_options(
    const char *const *filenames,
    size_t filename_count,
    const CompileOptions *options,
    const TargetConfig *target_config,
    CompileResult *out
) {
    TargetConfig host_config;
    if (target_config)
        host_config = *target_config;
    else
        host_config = target_config_native();

    TargetInfo target_info;
    if (!target_info_from_config(&host_config, &target_info)) {
        fprintf(stderr, "error: unsupported target configuration\n");
        return COMPILE_STATUS_DRIVER_ERROR;
    }

    return compile_parse_and_check_files_for_target_with_options(
        filenames,
        filename_count,
        &target_info,
        &host_config,
        options,
        out
    );
}

CompileStatus compile_parse_and_check_for_target(
    const char *filename,
    const TargetInfo *target,
    const TargetConfig *target_config,
    CompileResult *out
) {
    const char *filenames[1] = { filename };
    return compile_parse_and_check_files_for_target(
        filenames, 1, target, target_config, out
    );
}

CompileStatus compile_parse_and_check_files_for_target(
    const char *const *filenames,
    size_t filename_count,
    const TargetInfo *target,
    const TargetConfig *target_config,
    CompileResult *out
) {
    CompileOptions options = compile_options_default();
    return compile_parse_and_check_files_for_target_with_options(
        filenames,
        filename_count,
        target,
        target_config,
        &options,
        out
    );
}

CompileStatus compile_parse_and_check_files_for_target_with_options(
    const char *const *filenames,
    size_t filename_count,
    const TargetInfo *target,
    const TargetConfig *target_config,
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

    if (!target_config) {
        fprintf(stderr, "error: no target configuration\n");
        return out->status;
    }

    char target_error[160];
    if (!target_info_validate(target, target_error, sizeof(target_error))) {
        fprintf(stderr, "error: invalid target description: %s\n", target_error);
        return out->status;
    }
    out->target = *target;
    out->target_config = *target_config;

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
    if (options->stdlib_root && options->stdlib_root[0] == '\0') {
        fprintf(stderr, "error: standard library root must be non-empty\n");
        return out->status;
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
