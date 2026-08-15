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
