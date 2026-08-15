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
