#ifndef COGLET_SOURCE_H
#define COGLET_SOURCE_H

#include <stddef.h>
#include <stdint.h>

#include "utils/arena.h"

/*
 * Stable identity for a source file within one compilation/source manager.
 * IDs are deliberately independent of filenames so two distinct source files
 * may have the same display name if a future embedding API needs that.
 */
typedef uint32_t SourceFileId;

#define SOURCE_FILE_ID_INVALID UINT32_MAX

typedef struct SourceSpan {
    SourceFileId file_id;

    /* Byte offsets into the registered source buffer: [start_offset, end_offset). */
    size_t start_offset;
    size_t end_offset;

    /* Cached one-based position of start_offset for diagnostics. */
    uint32_t line;
    uint32_t column;
} SourceSpan;

typedef struct SourceFile {
    SourceFileId id;
    const char *filename;
    const char *source;
    size_t length;

    struct SourceFile *next;
} SourceFile;

typedef struct SourceManager {
    Arena *arena;
    SourceFile *first;
    SourceFile *last;
    size_t count;
    SourceFileId next_id;
} SourceManager;

void source_manager_init(SourceManager *manager, Arena *arena);

/*
 * Registers a source buffer and returns its stable ID.
 *
 * The filename is copied into manager->arena. The source buffer is borrowed and
 * must remain alive for as long as spans referring to it are used.
 */
SourceFileId source_manager_add(
    SourceManager *manager,
    const char *filename,
    const char *source
);

const SourceFile *source_manager_get(
    const SourceManager *manager,
    SourceFileId id
);

SourceSpan source_span_invalid(void);

SourceSpan source_span_make(
    SourceFileId file_id,
    size_t start_offset,
    size_t end_offset,
    uint32_t line,
    uint32_t column
);

int source_span_is_valid(SourceSpan span);

/*
 * Returns one span beginning at the earlier start and ending at the later end.
 * Both spans must belong to the same file. If either span is invalid, the other
 * span is returned. Spans from different files are never merged.
 */
SourceSpan source_span_join(SourceSpan first, SourceSpan second);

#endif
