#ifndef COGLET_OPTIMIZATION_H
#define COGLET_OPTIMIZATION_H

/*
 * Compiler-wide optimization intent. Backends map these levels to their own
 * optimization/code-generation policy; the level is deliberately not part of
 * CogIR because it does not change Coglet semantics or IR ownership.
 */
typedef enum CogOptimizationLevel {
    COG_OPTIMIZATION_LEVEL_0 = 0,
    COG_OPTIMIZATION_LEVEL_1,
    COG_OPTIMIZATION_LEVEL_2,
    COG_OPTIMIZATION_LEVEL_3,
} CogOptimizationLevel;

#endif
