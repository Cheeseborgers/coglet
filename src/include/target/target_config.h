#ifndef COGLET_TARGET_CONFIG_H
#define COGLET_TARGET_CONFIG_H

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

/* Parses a target triple-like name such as `x86_64-linux`. */
int target_parse_config(const char *value, TargetConfig *config);

/* Returns the target represented by the compiler's native host platform. */
TargetConfig target_config_native(void);

#endif
