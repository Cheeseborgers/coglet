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
