#include <stdarg.h>
#include <stddef.h>

int coglet_backend_link_probe(void)
{
    return 23;
}

int coglet_backend_integer_alias_probe(
    signed char schar_value,
    unsigned char uchar_value,
    short short_value,
    unsigned short ushort_value,
    long long_value,
    unsigned long ulong_value,
    long long longlong_value,
    unsigned long long ulonglong_value
)
{
    if (schar_value != -1 ||
        uchar_value != 2 ||
        short_value != -3 ||
        ushort_value != 4 ||
        long_value != -5 ||
        ulong_value != 6 ||
        longlong_value != -7 ||
        ulonglong_value != 8) {
        return 30;
    }

    return 29;
}

int coglet_backend_scalar_alias_probe(
    _Bool flag,
    float single_value,
    double double_value
)
{
    if (!flag || single_value != 1.25f || double_value != -2.5)
        return 32;

    return 31;
}

typedef struct CogletBackendProbePair {
    int left;
    double weight;
} CogletBackendProbePair;

CogletBackendProbePair coglet_backend_make_pair(int left, double weight)
{
    CogletBackendProbePair pair;
    pair.left = left;
    pair.weight = weight;
    return pair;
}

int coglet_backend_struct_probe(CogletBackendProbePair pair)
{
    if (pair.left != 17 || pair.weight != 2.5)
        return 38;

    return 37;
}

typedef struct CogletBackendNestedPoint {
    int x;
    double y;
} CogletBackendNestedPoint;

typedef struct CogletBackendNestedPacket {
    CogletBackendNestedPoint point;
    unsigned short tag;
} CogletBackendNestedPacket;

CogletBackendNestedPacket coglet_backend_make_nested_packet(
    int x,
    double y,
    unsigned short tag
)
{
    CogletBackendNestedPacket packet;
    packet.point.x = x;
    packet.point.y = y;
    packet.tag = tag;
    return packet;
}

int coglet_backend_nested_struct_probe(CogletBackendNestedPacket packet)
{
    if (packet.point.x != 19 ||
        packet.point.y != 3.5 ||
        packet.tag != 23) {
        return 42;
    }

    return 41;
}

typedef struct CogletBackendArrayPoint {
    int x;
    double y;
} CogletBackendArrayPoint;

typedef struct CogletBackendArrayPacket {
    char name[4];
    CogletBackendArrayPoint points[2];
    int values[3];
} CogletBackendArrayPacket;

CogletBackendArrayPacket coglet_backend_make_array_packet(void)
{
    CogletBackendArrayPacket packet = {
        {'c', 'o', 'g', '\0'},
        {{11, 1.5}, {13, 2.5}},
        {17, 19, 23}
    };
    return packet;
}

int coglet_backend_array_struct_probe(CogletBackendArrayPacket packet)
{
    if (packet.name[0] != 'c' ||
        packet.name[1] != 'o' ||
        packet.name[2] != 'g' ||
        packet.name[3] != '\0' ||
        packet.points[0].x != 11 ||
        packet.points[0].y != 1.5 ||
        packet.points[1].x != 13 ||
        packet.points[1].y != 2.5 ||
        packet.values[0] != 17 ||
        packet.values[1] != 19 ||
        packet.values[2] != 23) {
        return 46;
    }

    return 45;
}


typedef enum CogletBackendMode {
    COGLET_BACKEND_MODE_IDLE = -1,
    COGLET_BACKEND_MODE_RUNNING = 3
} CogletBackendMode;

typedef struct CogletBackendEnumPacket {
    CogletBackendMode current;
    CogletBackendMode history[2];
} CogletBackendEnumPacket;

CogletBackendEnumPacket coglet_backend_make_enum_packet(void)
{
    CogletBackendEnumPacket packet = {
        COGLET_BACKEND_MODE_RUNNING,
        {COGLET_BACKEND_MODE_IDLE, COGLET_BACKEND_MODE_RUNNING}
    };
    return packet;
}

int coglet_backend_enum_packet_probe(CogletBackendMode mode, CogletBackendEnumPacket packet)
{
    if (mode != COGLET_BACKEND_MODE_RUNNING ||
        packet.current != COGLET_BACKEND_MODE_RUNNING ||
        packet.history[0] != COGLET_BACKEND_MODE_IDLE ||
        packet.history[1] != COGLET_BACKEND_MODE_RUNNING) {
        return 50;
    }

    return 49;
}

typedef int (*CogletBackendUnaryCallback)(int);

static int coglet_backend_callback_add_two(int value)
{
    return value + 2;
}

CogletBackendUnaryCallback coglet_backend_return_callback(void)
{
    return coglet_backend_callback_add_two;
}

int coglet_backend_callback_probe(
    CogletBackendUnaryCallback callback,
    CogletBackendUnaryCallback missing,
    int value
)
{
    if (!callback || missing != 0)
        return 54;

    if (callback(value) != value)
        return 55;

    return 53;
}

typedef int (*CogletBackendVariadicCallback)(int);

int coglet_backend_varargs_probe(int marker, ...)
{
    va_list args;
    va_start(args, marker);

    int promoted_short = va_arg(args, int);
    int promoted_bool = va_arg(args, int);
    double promoted_float = va_arg(args, double);
    double direct_double = va_arg(args, double);
    const char *text = va_arg(args, const char *);
    CogletBackendVariadicCallback callback =
        va_arg(args, CogletBackendVariadicCallback);

    va_end(args);

    if (marker != 91 ||
        promoted_short != -3 ||
        promoted_bool != 1 ||
        promoted_float != 1.25 ||
        direct_double != -2.5 ||
        !text || text[0] != 'o' || text[1] != 'k' || text[2] != '\0' ||
        !callback || callback(57) != 57) {
        return 58;
    }

    return 57;
}

typedef struct CogletBackendIncompleteHandle {
    int marker;
} CogletBackendIncompleteHandle;

CogletBackendIncompleteHandle *coglet_backend_get_incomplete_handle(void)
{
    static CogletBackendIncompleteHandle handle = {67};
    return &handle;
}

int coglet_backend_incomplete_handle_probe(
    const CogletBackendIncompleteHandle *handle
)
{
    if (!handle || handle->marker != 67)
        return 68;

    return 67;
}

typedef struct CogletBackendUnionPoint {
    int x;
    double y;
} CogletBackendUnionPoint;

typedef union CogletBackendUnionValue {
    int integer;
    double real;
    CogletBackendUnionPoint point;
} CogletBackendUnionValue;

typedef struct CogletBackendUnionPacket {
    int tag;
    CogletBackendUnionValue value;
    CogletBackendUnionValue history[2];
} CogletBackendUnionPacket;

CogletBackendUnionPacket coglet_backend_make_union_packet(int value)
{
    CogletBackendUnionPacket packet;
    packet.tag = value;
    packet.value.point.x = value;
    packet.value.point.y = 2.5;
    packet.history[0].integer = value + 1;
    packet.history[1].real = 3.5;
    return packet;
}

int coglet_backend_union_packet_probe(CogletBackendUnionPacket packet)
{
    if (packet.tag != 71 ||
        packet.value.point.x != 71 ||
        packet.value.point.y != 2.5 ||
        packet.history[0].integer != 72 ||
        packet.history[1].real != 3.5) {
        return 72;
    }

    return 71;
}


#if defined(__GNUC__) || defined(__clang__)
typedef struct __attribute__((packed)) CogletBackendPackedLayout {
    unsigned char tag;
    unsigned int value;
} CogletBackendPackedLayout;

typedef struct __attribute__((aligned(16))) CogletBackendAlignedLayout {
    int value;
} CogletBackendAlignedLayout;

typedef struct __attribute__((packed, aligned(8))) CogletBackendPackedAlignedLayout {
    unsigned char tag;
    unsigned int value;
} CogletBackendPackedAlignedLayout;

CogletBackendPackedLayout coglet_backend_make_packed_layout(void)
{
    CogletBackendPackedLayout value = {7, 101};
    return value;
}

CogletBackendAlignedLayout coglet_backend_make_aligned_layout(void)
{
    CogletBackendAlignedLayout value = {103};
    return value;
}

CogletBackendPackedAlignedLayout coglet_backend_make_packed_aligned_layout(void)
{
    CogletBackendPackedAlignedLayout value = {11, 107};
    return value;
}

int coglet_backend_layout_probe(
    CogletBackendPackedLayout packed,
    CogletBackendAlignedLayout aligned,
    CogletBackendPackedAlignedLayout both
)
{
    if (sizeof(CogletBackendPackedLayout) != 5 ||
        __alignof__(CogletBackendPackedLayout) != 1 ||
        offsetof(CogletBackendPackedLayout, value) != 1 ||
        sizeof(CogletBackendAlignedLayout) != 16 ||
        __alignof__(CogletBackendAlignedLayout) != 16 ||
        sizeof(CogletBackendPackedAlignedLayout) != 8 ||
        __alignof__(CogletBackendPackedAlignedLayout) != 8 ||
        offsetof(CogletBackendPackedAlignedLayout, value) != 1 ||
        packed.tag != 7 || packed.value != 101 ||
        aligned.value != 103 ||
        both.tag != 11 || both.value != 107) {
        return 74;
    }

    return 73;
}

#endif

#if (defined(__GNUC__) || defined(__clang__)) && \
    (defined(__x86_64__) || defined(__amd64__))
typedef int (__attribute__((ms_abi)) *CogletBackendWin64Callback)(int);

int __attribute__((ms_abi)) coglet_backend_win64_call_probe(
    CogletBackendWin64Callback callback,
    int value
)
{
    if (!callback || callback(value) != value)
        return 80;

    return 79;
}
#endif

static volatile int coglet_backend_volatile_rw_value = 0;
static const volatile int coglet_backend_volatile_ro_value = 89;

volatile int *coglet_backend_get_volatile_rw(void)
{
    return &coglet_backend_volatile_rw_value;
}

const volatile int *coglet_backend_get_volatile_ro(void)
{
    return &coglet_backend_volatile_ro_value;
}

int coglet_backend_volatile_probe(
    volatile int *rw,
    const volatile int *ro
)
{
    if (!rw || !ro)
        return 90;

    *rw = 89;

    if (*rw != 89 || *ro != 89)
        return 90;

    return 89;
}

int coglet_backend_wrapping_probe(
    signed char signed_add,
    signed char signed_sub,
    signed char signed_mul,
    signed char signed_neg,
    unsigned char unsigned_add,
    unsigned char unsigned_sub,
    unsigned char unsigned_mul,
    unsigned char unsigned_neg,
    long long signed_wide_add,
    long long signed_wide_neg
)
{
    if (signed_add != -128 ||
        signed_sub != 127 ||
        signed_mul != -128 ||
        signed_neg != -128 ||
        unsigned_add != 0 ||
        unsigned_sub != 255 ||
        unsigned_mul != 144 ||
        unsigned_neg != 255 ||
        signed_wide_add != (-9223372036854775807LL - 1LL) ||
        signed_wide_neg != (-9223372036854775807LL - 1LL)) {
        return 94;
    }

    return 93;
}

int coglet_backend_checked_arithmetic_probe(
    int signed_add,
    int signed_sub,
    int signed_mul,
    int signed_div,
    int signed_rem,
    int signed_neg,
    unsigned int unsigned_add,
    unsigned int unsigned_sub,
    unsigned int unsigned_mul,
    unsigned int unsigned_div,
    unsigned int unsigned_rem
)
{
    if (signed_add != 42 ||
        signed_sub != -42 ||
        signed_mul != -42 ||
        signed_div != -42 ||
        signed_rem != -1 ||
        signed_neg != -42 ||
        unsigned_add != 42U ||
        unsigned_sub != 38U ||
        unsigned_mul != 42U ||
        unsigned_div != 42U ||
        unsigned_rem != 1U) {
        return 100;
    }

    return 95;
}


typedef signed char (*CogletBackendSmallCallback)(signed char);

signed char coglet_backend_return_schar(void)
{
    return -7;
}

_Bool coglet_backend_return_bool(void)
{
    return 1;
}

int coglet_backend_small_callback_probe(CogletBackendSmallCallback callback)
{
    if (!callback || callback(-9) != -9)
        return 84;
    return 83;
}
