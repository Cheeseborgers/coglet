#ifndef COGLET_STRING_DECODE_H
#define COGLET_STRING_DECODE_H

#include "utils/string_view.h"

typedef struct StringDecodeInfo {
    int ok;
    int decoded_length;
    size_t invalid_offset;
    char invalid_escape;
} StringDecodeInfo;

StringDecodeInfo string_analyze(StringView s);

/*
 * Decode a previously source-spelled Coglet string payload into dest.
 * dest must have room for at least s.length bytes. The returned metadata
 * matches string_analyze(); on success exactly decoded_length bytes are
 * written. No terminating NUL is appended.
 */
StringDecodeInfo string_decode_into(StringView s, char *dest);

#endif