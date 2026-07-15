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

#endif