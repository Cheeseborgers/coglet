#ifndef COGLET_TARGET_H
#define COGLET_TARGET_H

typedef enum {
    TARGET_ARCH_X86_64,
    TARGET_ARCH_AARCH64,
} TargetArch;

typedef enum {
    TARGET_OS_LINUX,
    TARGET_OS_WINDOWS,
} TargetOs;

typedef enum {
    TARGET_ABI_SYSV,
    TARGET_ABI_WINDOWS,
} TargetAbi;

typedef struct {
    TargetArch arch;
    TargetOs os;
    TargetAbi abi;
} TargetConfig;

int target_parse_config(const char *value, TargetConfig *target);

TargetConfig target_config_host(void);

#endif
