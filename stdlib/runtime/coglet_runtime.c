/*
 * Compatibility translation unit for consumers that still compile the v0
 * runtime as one source file. Coglet's driver links the required components
 * directly so pure std.io/std.math/std.mem programs do not compile unrelated
 * runtime code.
 */
#include "coglet_runtime_io.c"
#if defined(COGLET_RUNTIME_MATH)
#include "coglet_runtime_math.c"
#endif
#include "coglet_runtime_mem.c"
