#include "source.h"

#include <assert.h>
#include <string.h>

void source_manager_init(SourceManager *manager, Arena *arena)
{
    assert(manager);
    assert(arena);

    manager->arena = arena;
    manager->first = NULL;
    manager->last = NULL;
    manager->count = 0;
    manager->next_id = 0;
}

SourceFileId source_manager_add(
    SourceManager *manager,
    const char *filename,
    const char *source
) {
    assert(manager);
    assert(manager->arena);
    assert(filename);
    assert(source);
    assert(manager->next_id != SOURCE_FILE_ID_INVALID);

    SourceFile *file = arena_new(manager->arena, SourceFile);

    file->id = manager->next_id++;
    file->filename = arena_strdup_len(manager->arena, filename, strlen(filename));
    file->source = source;
    file->length = strlen(source);
    file->next = NULL;

    if (manager->last) {
        manager->last->next = file;
    } else {
        manager->first = file;
    }

    manager->last = file;
    manager->count++;

    return file->id;
}

const SourceFile *source_manager_get(
    const SourceManager *manager,
    SourceFileId id
) {
    if (!manager || id == SOURCE_FILE_ID_INVALID)
        return NULL;

    for (const SourceFile *file = manager->first; file; file = file->next) {
        if (file->id == id)
            return file;
    }

    return NULL;
}

SourceSpan source_span_invalid(void)
{
    SourceSpan span;
    span.file_id = SOURCE_FILE_ID_INVALID;
    span.start_offset = 0;
    span.end_offset = 0;
    span.line = 0;
    span.column = 0;
    return span;
}

SourceSpan source_span_make(
    SourceFileId file_id,
    size_t start_offset,
    size_t end_offset,
    uint32_t line,
    uint32_t column
) {
    SourceSpan span;
    span.file_id = file_id;
    span.start_offset = start_offset;
    span.end_offset = end_offset < start_offset ? start_offset : end_offset;
    span.line = line;
    span.column = column;
    return span;
}

int source_span_is_valid(SourceSpan span)
{
    return span.file_id != SOURCE_FILE_ID_INVALID &&
           span.line > 0 &&
           span.column > 0;
}

SourceSpan source_span_join(SourceSpan first, SourceSpan second)
{
    if (!source_span_is_valid(first))
        return second;

    if (!source_span_is_valid(second))
        return first;

    if (first.file_id != second.file_id)
        return first;

    SourceSpan result = first;

    if (second.start_offset < result.start_offset) {
        result.start_offset = second.start_offset;
        result.line = second.line;
        result.column = second.column;
    }

    if (second.end_offset > result.end_offset)
        result.end_offset = second.end_offset;

    return result;
}
