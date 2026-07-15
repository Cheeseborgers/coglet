#include "string_decode.h"

StringDecodeInfo string_analyze(StringView s)
{
    StringDecodeInfo info;
    info.ok             = 1;
    info.decoded_length = 0;
    info.invalid_offset = -1;
    info.invalid_escape = 0;

    for (size_t i = 0; i < s.length; i++) {
        if (s.data[i] != '\\') {
            info.decoded_length++;
            continue;
        }

        i++;

        if (i >= s.length) {
            info.ok = 0;
            info.invalid_offset = i - 1;
            info.invalid_escape = 0;
            return info;
        }

        switch (s.data[i]) {
            case 'n':
            case 't':
            case 'r':
            case '\\':
            case '"':
            case '0':
                info.decoded_length++;
                break;

            default:
                info.ok = 0;
                info.invalid_offset = i;
                info.invalid_escape = s.data[i];
                return info;
        }
    }

    return info;
}