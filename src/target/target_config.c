#include "target/target_config.h"

#include <string.h>

int target_parse_config(const char *value, TargetConfig *target)
{
    if (strcmp(value, "x86_64-linux") == 0) {
        target->arch = TARGET_ARCH_X86_64;
        target->os = TARGET_OS_LINUX;
        target->abi = TARGET_ABI_SYSV;
        return 1;
    }

    if (strcmp(value, "aarch64-linux") == 0) {
        target->arch = TARGET_ARCH_AARCH64;
        target->os = TARGET_OS_LINUX;
        target->abi = TARGET_ABI_SYSV;
        return 1;
    }

    if (strcmp(value, "x86_64-windows") == 0) {
        target->arch = TARGET_ARCH_X86_64;
        target->os = TARGET_OS_WINDOWS;
        target->abi = TARGET_ABI_WINDOWS;
        return 1;
    }

    if (strcmp(value, "aarch64-windows") == 0) {
        target->arch = TARGET_ARCH_AARCH64;
        target->os = TARGET_OS_WINDOWS;
        target->abi = TARGET_ABI_WINDOWS;
        return 1;
    }

    return 0;
}

TargetConfig target_config_host(void)
{
    TargetConfig target;

#if defined(__aarch64__) || defined(_M_ARM64)
    target.arch = TARGET_ARCH_AARCH64;
#else
    target.arch = TARGET_ARCH_X86_64;
#endif

#if defined(_WIN32)
    target.os = TARGET_OS_WINDOWS;
    target.abi = TARGET_ABI_WINDOWS;
#else
    target.os = TARGET_OS_LINUX;
    target.abi = TARGET_ABI_SYSV;
#endif

    return target;
}
