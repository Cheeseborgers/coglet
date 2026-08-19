#include "target/target_config.h"

#include <string.h>

int target_parse_config(const char *value, TargetConfig *config)
{
    if (strcmp(value, "x86_64-linux") == 0) {
        config->arch = TARGET_ARCH_X86_64;
        config->os = TARGET_OS_LINUX;
        config->abi = TARGET_ABI_SYSV;
        return 1;
    }

    if (strcmp(value, "aarch64-linux") == 0) {
        config->arch = TARGET_ARCH_AARCH64;
        config->os = TARGET_OS_LINUX;
        config->abi = TARGET_ABI_SYSV;
        return 1;
    }

    if (strcmp(value, "x86_64-windows") == 0) {
        config->arch = TARGET_ARCH_X86_64;
        config->os = TARGET_OS_WINDOWS;
        config->abi = TARGET_ABI_WINDOWS;
        return 1;
    }

    if (strcmp(value, "aarch64-windows") == 0) {
        config->arch = TARGET_ARCH_AARCH64;
        config->os = TARGET_OS_WINDOWS;
        config->abi = TARGET_ABI_WINDOWS;
        return 1;
    }

    return 0;
}

TargetConfig target_config_native(void)
{
    TargetConfig config;

#if defined(__aarch64__) || defined(_M_ARM64)
    config.arch = TARGET_ARCH_AARCH64;
#else
    config.arch = TARGET_ARCH_X86_64;
#endif

#if defined(_WIN32)
    config.os = TARGET_OS_WINDOWS;
    config.abi = TARGET_ABI_WINDOWS;
#else
    config.os = TARGET_OS_LINUX;
    config.abi = TARGET_ABI_SYSV;
#endif

    return config;
}
