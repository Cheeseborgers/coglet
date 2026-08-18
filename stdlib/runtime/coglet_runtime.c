#include <inttypes.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(COGLET_RUNTIME_MATH)
#include <math.h>
#endif

/*
 * Coglet runtime ABI, v0.
 *
 * These symbols are implementation details used by runtime-facing standard
 * modules. User code should import std.io/std.math/std.mem rather than declaring them directly.
 * The ABI intentionally uses only ISO C scalar/pointer types so the same source
 * compiles for Linux and Windows on both x86-64 and AArch64 native toolchains.
 */

void coglet_rt_io_write(const uint8_t *data, uint64_t length)
{
    if (length == 0)
        return;

    (void)fwrite(data, 1, (size_t)length, stdout);
}

void coglet_rt_io_newline(void)
{
    fputc('\n', stdout);
}

void coglet_rt_io_flush(void)
{
    fflush(stdout);
}

void coglet_rt_io_print_bool(_Bool value)
{
    fputs(value ? "true" : "false", stdout);
}

void coglet_rt_io_print_s8(int8_t value)
{
    fprintf(stdout, "%" PRId8, value);
}

void coglet_rt_io_print_s16(int16_t value)
{
    fprintf(stdout, "%" PRId16, value);
}

void coglet_rt_io_print_s32(int32_t value)
{
    fprintf(stdout, "%" PRId32, value);
}

void coglet_rt_io_print_s64(int64_t value)
{
    fprintf(stdout, "%" PRId64, value);
}

void coglet_rt_io_print_u8(uint8_t value)
{
    fprintf(stdout, "%" PRIu8, value);
}

void coglet_rt_io_print_u16(uint16_t value)
{
    fprintf(stdout, "%" PRIu16, value);
}

void coglet_rt_io_print_u32(uint32_t value)
{
    fprintf(stdout, "%" PRIu32, value);
}

void coglet_rt_io_print_u64(uint64_t value)
{
    fprintf(stdout, "%" PRIu64, value);
}

void coglet_rt_io_print_f32(float value)
{
    fprintf(stdout, "%.9g", (double)value);
}

void coglet_rt_io_print_f64(double value)
{
    fprintf(stdout, "%.17g", value);
}

#if defined(COGLET_RUNTIME_MATH)

float coglet_rt_math_sqrt_f32(float value)
{
    return sqrtf(value);
}

double coglet_rt_math_sqrt_f64(double value)
{
    return sqrt(value);
}

float coglet_rt_math_sin_f32(float value)
{
    return sinf(value);
}

double coglet_rt_math_sin_f64(double value)
{
    return sin(value);
}

float coglet_rt_math_cos_f32(float value)
{
    return cosf(value);
}

double coglet_rt_math_cos_f64(double value)
{
    return cos(value);
}

float coglet_rt_math_tan_f32(float value)
{
    return tanf(value);
}

double coglet_rt_math_tan_f64(double value)
{
    return tan(value);
}

float coglet_rt_math_asin_f32(float value)
{
    return asinf(value);
}

double coglet_rt_math_asin_f64(double value)
{
    return asin(value);
}

float coglet_rt_math_acos_f32(float value)
{
    return acosf(value);
}

double coglet_rt_math_acos_f64(double value)
{
    return acos(value);
}

float coglet_rt_math_atan_f32(float value)
{
    return atanf(value);
}

double coglet_rt_math_atan_f64(double value)
{
    return atan(value);
}

float coglet_rt_math_atan2_f32(float y, float x)
{
    return atan2f(y, x);
}

double coglet_rt_math_atan2_f64(double y, double x)
{
    return atan2(y, x);
}

float coglet_rt_math_floor_f32(float value)
{
    return floorf(value);
}

double coglet_rt_math_floor_f64(double value)
{
    return floor(value);
}

float coglet_rt_math_ceil_f32(float value)
{
    return ceilf(value);
}

double coglet_rt_math_ceil_f64(double value)
{
    return ceil(value);
}

float coglet_rt_math_round_f32(float value)
{
    return roundf(value);
}

double coglet_rt_math_round_f64(double value)
{
    return round(value);
}

float coglet_rt_math_trunc_f32(float value)
{
    return truncf(value);
}

double coglet_rt_math_trunc_f64(double value)
{
    return trunc(value);
}

float coglet_rt_math_fmod_f32(float x, float y)
{
    return fmodf(x, y);
}

double coglet_rt_math_fmod_f64(double x, double y)
{
    return fmod(x, y);
}

#endif

/*
 * Explicit aligned heap storage used by std.mem.
 *
 * The allocator stores the original malloc pointer and allocation size in a
 * private header immediately before the aligned user pointer. This keeps the
 * runtime portable across the initial Linux/Windows x86-64/AArch64 matrix and
 * avoids platform-specific aligned-free/realloc pairing rules.
 */
typedef struct CogletRuntimeAllocationHeader {
    void *base;
    size_t size;
} CogletRuntimeAllocationHeader;

static void coglet_rt_mem_fail(void)
{
    fputs("Coglet runtime: out of memory\n", stderr);
    abort();
}

static size_t coglet_rt_mem_checked_size(uint64_t value)
{
    if (value > (uint64_t)SIZE_MAX)
        coglet_rt_mem_fail();
    return (size_t)value;
}

static size_t coglet_rt_mem_checked_alignment(uint64_t value)
{
    if (value == 0 || (value & (value - 1)) != 0 || value > (uint64_t)SIZE_MAX)
        coglet_rt_mem_fail();

    size_t alignment = (size_t)value;
    if (alignment < sizeof(void *))
        alignment = sizeof(void *);
    return alignment;
}

static void *coglet_rt_mem_allocate_impl(size_t size, size_t alignment)
{
    if (size == 0)
        return NULL;

    size_t header_size = sizeof(CogletRuntimeAllocationHeader);
    if (size > SIZE_MAX - header_size ||
        size + header_size > SIZE_MAX - (alignment - 1)) {
        coglet_rt_mem_fail();
    }

    size_t total = size + header_size + alignment - 1;
    unsigned char *base = (unsigned char *)malloc(total);
    if (!base)
        coglet_rt_mem_fail();

    uintptr_t first = (uintptr_t)(base + header_size);
    uintptr_t aligned = (first + (uintptr_t)(alignment - 1)) &
                        ~(uintptr_t)(alignment - 1);
    CogletRuntimeAllocationHeader *header =
        (CogletRuntimeAllocationHeader *)(aligned - header_size);
    header->base = base;
    header->size = size;
    return (void *)aligned;
}

void *coglet_rt_mem_alloc(uint64_t size, uint64_t alignment)
{
    return coglet_rt_mem_allocate_impl(
        coglet_rt_mem_checked_size(size),
        coglet_rt_mem_checked_alignment(alignment)
    );
}

void coglet_rt_mem_free(void *pointer)
{
    if (!pointer)
        return;

    CogletRuntimeAllocationHeader *header =
        (CogletRuntimeAllocationHeader *)((unsigned char *)pointer -
                                           sizeof(CogletRuntimeAllocationHeader));
    free(header->base);
}

void *coglet_rt_mem_resize(void *pointer, uint64_t size, uint64_t alignment)
{
    size_t new_size = coglet_rt_mem_checked_size(size);
    size_t checked_alignment = coglet_rt_mem_checked_alignment(alignment);

    if (!pointer)
        return coglet_rt_mem_allocate_impl(new_size, checked_alignment);

    if (new_size == 0) {
        coglet_rt_mem_free(pointer);
        return NULL;
    }

    CogletRuntimeAllocationHeader *old_header =
        (CogletRuntimeAllocationHeader *)((unsigned char *)pointer -
                                           sizeof(CogletRuntimeAllocationHeader));
    size_t old_size = old_header->size;
    void *resized = coglet_rt_mem_allocate_impl(new_size, checked_alignment);
    memcpy(resized, pointer, old_size < new_size ? old_size : new_size);
    coglet_rt_mem_free(pointer);
    return resized;
}


/* Generic allocator ABI used by std.mem. */
typedef void *(*CogletRuntimeAllocFn)(void *, size_t, size_t);
typedef void *(*CogletRuntimeResizeFn)(void *, void *, size_t, size_t, size_t);
typedef void (*CogletRuntimeFreeFn)(void *, void *, size_t, size_t);

typedef struct CogletRuntimeAllocator {
    void *state;
    CogletRuntimeAllocFn allocate;
    CogletRuntimeResizeFn resize;
    CogletRuntimeFreeFn free;
} CogletRuntimeAllocator;

void *coglet_rt_mem_heap_alloc(void *state, size_t size, size_t alignment)
{
    (void)state;
    return coglet_rt_mem_allocate_impl(size, coglet_rt_mem_checked_alignment((uint64_t)alignment));
}

void *coglet_rt_mem_heap_resize(
    void *state,
    void *pointer,
    size_t old_size,
    size_t new_size,
    size_t alignment
) {
    (void)state;
    (void)old_size;
    return coglet_rt_mem_resize(pointer, (uint64_t)new_size, (uint64_t)alignment);
}

void coglet_rt_mem_heap_free(void *state, void *pointer, size_t size, size_t alignment)
{
    (void)state;
    (void)size;
    (void)alignment;
    coglet_rt_mem_free(pointer);
}

typedef struct CogletRuntimeArenaBlock {
    struct CogletRuntimeArenaBlock *next;
    size_t allocation_size;
    size_t capacity;
    size_t used;
} CogletRuntimeArenaBlock;

typedef struct CogletRuntimeArena {
    CogletRuntimeAllocator parent;
    size_t block_size;
    CogletRuntimeArenaBlock *head;
    CogletRuntimeArenaBlock *current;
} CogletRuntimeArena;

static int coglet_rt_mem_allocator_valid(CogletRuntimeAllocator allocator)
{
    return allocator.allocate && allocator.resize && allocator.free;
}

static uintptr_t coglet_rt_align_up_uintptr(uintptr_t value, size_t alignment)
{
    return (value + (uintptr_t)(alignment - 1)) & ~(uintptr_t)(alignment - 1);
}

static CogletRuntimeArenaBlock *coglet_rt_mem_arena_new_block(
    CogletRuntimeArena *arena,
    size_t minimum_payload,
    size_t requested_alignment
) {
    size_t payload = arena->block_size;
    size_t needed = minimum_payload;
    if (requested_alignment > 1) {
        if (needed > SIZE_MAX - (requested_alignment - 1))
            coglet_rt_mem_fail();
        needed += requested_alignment - 1;
    }
    if (payload < needed)
        payload = needed;
    if (payload > SIZE_MAX - sizeof(CogletRuntimeArenaBlock))
        coglet_rt_mem_fail();

    size_t total = sizeof(CogletRuntimeArenaBlock) + payload;
    CogletRuntimeArenaBlock *block = (CogletRuntimeArenaBlock *)arena->parent.allocate(
        arena->parent.state,
        total,
        sizeof(void *)
    );
    if (!block)
        coglet_rt_mem_fail();
    block->next = NULL;
    block->allocation_size = total;
    block->capacity = payload;
    block->used = 0;
    return block;
}

void *coglet_rt_mem_arena_create(CogletRuntimeAllocator parent, size_t block_size)
{
    if (!coglet_rt_mem_allocator_valid(parent))
        coglet_rt_mem_fail();
    if (block_size == 0)
        block_size = 65536;

    CogletRuntimeArena *arena = (CogletRuntimeArena *)parent.allocate(
        parent.state,
        sizeof(CogletRuntimeArena),
        sizeof(void *)
    );
    if (!arena)
        coglet_rt_mem_fail();
    arena->parent = parent;
    arena->block_size = block_size;
    arena->head = NULL;
    arena->current = NULL;
    return arena;
}

void *coglet_rt_mem_arena_alloc(void *state, size_t size, size_t alignment)
{
    CogletRuntimeArena *arena = (CogletRuntimeArena *)state;
    if (!arena || size == 0)
        return NULL;
    alignment = coglet_rt_mem_checked_alignment((uint64_t)alignment);

    CogletRuntimeArenaBlock *block = arena->current ? arena->current : arena->head;
    while (block) {
        uintptr_t base = (uintptr_t)(block + 1);
        uintptr_t current = base + (uintptr_t)block->used;
        uintptr_t aligned = coglet_rt_align_up_uintptr(current, alignment);
        if (aligned >= base && size <= block->capacity &&
            aligned - base <= block->capacity - size) {
            block->used = (size_t)(aligned - base) + size;
            arena->current = block;
            return (void *)aligned;
        }
        block = block->next;
    }

    block = coglet_rt_mem_arena_new_block(arena, size, alignment);
    if (!arena->head) {
        arena->head = block;
    } else {
        CogletRuntimeArenaBlock *tail = arena->head;
        while (tail->next)
            tail = tail->next;
        tail->next = block;
    }
    arena->current = block;

    uintptr_t base = (uintptr_t)(block + 1);
    uintptr_t aligned = coglet_rt_align_up_uintptr(base, alignment);
    block->used = (size_t)(aligned - base) + size;
    return (void *)aligned;
}

void *coglet_rt_mem_arena_resize(
    void *state,
    void *pointer,
    size_t old_size,
    size_t new_size,
    size_t alignment
) {
    if (!pointer)
        return coglet_rt_mem_arena_alloc(state, new_size, alignment);
    if (new_size == 0)
        return NULL;
    void *resized = coglet_rt_mem_arena_alloc(state, new_size, alignment);
    memcpy(resized, pointer, old_size < new_size ? old_size : new_size);
    return resized;
}

void coglet_rt_mem_arena_free(
    void *state,
    void *pointer,
    size_t size,
    size_t alignment
) {
    (void)state;
    (void)pointer;
    (void)size;
    (void)alignment;
    /* Individual arena frees are intentionally no-ops. */
}

void coglet_rt_mem_arena_reset(void *state)
{
    CogletRuntimeArena *arena = (CogletRuntimeArena *)state;
    if (!arena)
        return;
    for (CogletRuntimeArenaBlock *block = arena->head; block; block = block->next)
        block->used = 0;
    arena->current = arena->head;
}

void coglet_rt_mem_arena_destroy(void *state)
{
    CogletRuntimeArena *arena = (CogletRuntimeArena *)state;
    if (!arena)
        return;
    CogletRuntimeAllocator parent = arena->parent;
    CogletRuntimeArenaBlock *block = arena->head;
    while (block) {
        CogletRuntimeArenaBlock *next = block->next;
        parent.free(parent.state, block, block->allocation_size, sizeof(void *));
        block = next;
    }
    parent.free(parent.state, arena, sizeof(CogletRuntimeArena), sizeof(void *));
}


/* Arena checkpoints used by std.mem.Scratch. */
void coglet_rt_mem_arena_mark(void *state, void **out_block, size_t *out_used)
{
    CogletRuntimeArena *arena = (CogletRuntimeArena *)state;
    if (out_block)
        *out_block = NULL;
    if (out_used)
        *out_used = 0;
    if (!arena)
        return;

    CogletRuntimeArenaBlock *block = arena->current ? arena->current : arena->head;
    if (block) {
        if (out_block)
            *out_block = block;
        if (out_used)
            *out_used = block->used;
    }
}

void coglet_rt_mem_arena_rewind(void *state, void *mark_block, size_t mark_used)
{
    CogletRuntimeArena *arena = (CogletRuntimeArena *)state;
    if (!arena)
        return;

    if (!mark_block) {
        coglet_rt_mem_arena_reset(state);
        return;
    }

    CogletRuntimeArenaBlock *target = (CogletRuntimeArenaBlock *)mark_block;
    CogletRuntimeArenaBlock *block = arena->head;
    while (block && block != target)
        block = block->next;
    if (!block || mark_used > block->capacity)
        coglet_rt_mem_fail();

    block->used = mark_used;
    for (CogletRuntimeArenaBlock *later = block->next; later; later = later->next)
        later->used = 0;
    arena->current = block;
}

/* Caller-owned fixed-buffer arena used by std.mem.FixedArena. */
typedef struct CogletRuntimeFixedArena {
    unsigned char *data;
    size_t capacity;
    size_t used;
} CogletRuntimeFixedArena;

static size_t coglet_rt_mem_fixed_arena_alignment(void)
{
    struct AlignmentProbe {
        char prefix;
        CogletRuntimeFixedArena value;
    };
    return offsetof(struct AlignmentProbe, value);
}

void *coglet_rt_mem_fixed_arena_create(void *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0)
        coglet_rt_mem_fail();

    uintptr_t base = (uintptr_t)buffer;
    if (base > UINTPTR_MAX - buffer_size)
        coglet_rt_mem_fail();
    uintptr_t end = base + buffer_size;

    size_t state_alignment = coglet_rt_mem_fixed_arena_alignment();
    uintptr_t state_address = coglet_rt_align_up_uintptr(base, state_alignment);
    if (state_address > end || sizeof(CogletRuntimeFixedArena) > (size_t)(end - state_address))
        coglet_rt_mem_fail();

    uintptr_t data_address = state_address + sizeof(CogletRuntimeFixedArena);
    if (data_address > end)
        coglet_rt_mem_fail();

    CogletRuntimeFixedArena *arena = (CogletRuntimeFixedArena *)state_address;
    arena->data = (unsigned char *)data_address;
    arena->capacity = (size_t)(end - data_address);
    arena->used = 0;
    return arena;
}

void *coglet_rt_mem_fixed_arena_alloc(void *state, size_t size, size_t alignment)
{
    CogletRuntimeFixedArena *arena = (CogletRuntimeFixedArena *)state;
    if (!arena || size == 0)
        return NULL;
    alignment = coglet_rt_mem_checked_alignment((uint64_t)alignment);

    uintptr_t base = (uintptr_t)arena->data;
    uintptr_t current = base + arena->used;
    uintptr_t aligned = coglet_rt_align_up_uintptr(current, alignment);
    if (aligned < base || aligned - base > arena->capacity ||
        size > arena->capacity - (size_t)(aligned - base)) {
        coglet_rt_mem_fail();
    }

    arena->used = (size_t)(aligned - base) + size;
    return (void *)aligned;
}

void *coglet_rt_mem_fixed_arena_resize(
    void *state,
    void *pointer,
    size_t old_size,
    size_t new_size,
    size_t alignment
) {
    if (!pointer)
        return coglet_rt_mem_fixed_arena_alloc(state, new_size, alignment);
    if (new_size == 0)
        return NULL;

    void *resized = coglet_rt_mem_fixed_arena_alloc(state, new_size, alignment);
    memcpy(resized, pointer, old_size < new_size ? old_size : new_size);
    return resized;
}

void coglet_rt_mem_fixed_arena_free(
    void *state,
    void *pointer,
    size_t size,
    size_t alignment
) {
    (void)state;
    (void)pointer;
    (void)size;
    (void)alignment;
    /* Individual fixed-arena frees are intentionally no-ops. */
}

void coglet_rt_mem_fixed_arena_reset(void *state)
{
    CogletRuntimeFixedArena *arena = (CogletRuntimeFixedArena *)state;
    if (arena)
        arena->used = 0;
}

size_t coglet_rt_mem_fixed_arena_used(void *state)
{
    CogletRuntimeFixedArena *arena = (CogletRuntimeFixedArena *)state;
    return arena ? arena->used : 0;
}

size_t coglet_rt_mem_fixed_arena_remaining(void *state)
{
    CogletRuntimeFixedArena *arena = (CogletRuntimeFixedArena *)state;
    if (!arena || arena->used > arena->capacity)
        return 0;
    return arena->capacity - arena->used;
}

/* Debug allocator wrapper used by std.mem.DebugAllocator. */
#define COGLET_RT_DEBUG_GUARD_SIZE 16u
#define COGLET_RT_DEBUG_GUARD_BYTE 0xA5u
#define COGLET_RT_DEBUG_ALLOC_BYTE 0xCDu
#define COGLET_RT_DEBUG_FREE_BYTE  0xDDu

typedef struct CogletRuntimeDebugBlock {
    struct CogletRuntimeDebugBlock *next;
    void *user;
    size_t size;
    size_t alignment;
    size_t total_size;
    size_t parent_alignment;
    uint64_t id;
} CogletRuntimeDebugBlock;

typedef struct CogletRuntimeDebugAllocator {
    CogletRuntimeAllocator parent;
    CogletRuntimeDebugBlock *head;
    uint64_t next_id;
    uint64_t live_allocations;
    uint64_t live_bytes;
    uint64_t total_allocations;
    uint64_t total_bytes;
    uint64_t error_count;
} CogletRuntimeDebugAllocator;

static size_t coglet_rt_mem_debug_block_alignment(void)
{
    struct AlignmentProbe {
        char prefix;
        CogletRuntimeDebugBlock value;
    };
    return offsetof(struct AlignmentProbe, value);
}

static size_t coglet_rt_mem_debug_state_alignment(void)
{
    struct AlignmentProbe {
        char prefix;
        CogletRuntimeDebugAllocator value;
    };
    return offsetof(struct AlignmentProbe, value);
}

static unsigned char *coglet_rt_mem_debug_front_guard(CogletRuntimeDebugBlock *block)
{
    return (unsigned char *)block->user - COGLET_RT_DEBUG_GUARD_SIZE;
}

static unsigned char *coglet_rt_mem_debug_back_guard(CogletRuntimeDebugBlock *block)
{
    return (unsigned char *)block->user + block->size;
}

static int coglet_rt_mem_debug_guard_intact(const unsigned char *guard)
{
    for (size_t i = 0; i < COGLET_RT_DEBUG_GUARD_SIZE; ++i) {
        if (guard[i] != COGLET_RT_DEBUG_GUARD_BYTE)
            return 0;
    }
    return 1;
}

static int coglet_rt_mem_debug_block_intact(CogletRuntimeDebugBlock *block)
{
    return coglet_rt_mem_debug_guard_intact(coglet_rt_mem_debug_front_guard(block)) &&
           coglet_rt_mem_debug_guard_intact(coglet_rt_mem_debug_back_guard(block));
}

static CogletRuntimeDebugBlock *coglet_rt_mem_debug_find(
    CogletRuntimeDebugAllocator *debug,
    void *pointer,
    CogletRuntimeDebugBlock **out_previous
) {
    CogletRuntimeDebugBlock *previous = NULL;
    CogletRuntimeDebugBlock *block = debug ? debug->head : NULL;
    while (block) {
        if (block->user == pointer) {
            if (out_previous)
                *out_previous = previous;
            return block;
        }
        previous = block;
        block = block->next;
    }
    if (out_previous)
        *out_previous = NULL;
    return NULL;
}

static void coglet_rt_mem_debug_report_corruption(
    CogletRuntimeDebugAllocator *debug,
    CogletRuntimeDebugBlock *block
) {
    int front_ok = coglet_rt_mem_debug_guard_intact(coglet_rt_mem_debug_front_guard(block));
    int back_ok = coglet_rt_mem_debug_guard_intact(coglet_rt_mem_debug_back_guard(block));
    if (front_ok && back_ok)
        return;

    ++debug->error_count;
    fprintf(
        stderr,
        "Coglet debug allocator: allocation #%" PRIu64 " guard corruption (%s%s), size=%zu, ptr=%p\n",
        block->id,
        front_ok ? "" : "underflow",
        (!front_ok && !back_ok) ? "+overflow" : (back_ok ? "" : "overflow"),
        block->size,
        block->user
    );
}

static void *coglet_rt_mem_debug_allocate_internal(
    CogletRuntimeDebugAllocator *debug,
    size_t size,
    size_t alignment
) {
    if (!debug || size == 0)
        return NULL;

    alignment = coglet_rt_mem_checked_alignment((uint64_t)alignment);
    size_t block_alignment = coglet_rt_mem_debug_block_alignment();
    if (block_alignment < sizeof(void *))
        block_alignment = sizeof(void *);

    size_t prefix = sizeof(CogletRuntimeDebugBlock) + COGLET_RT_DEBUG_GUARD_SIZE;
    if (prefix > SIZE_MAX - (alignment - 1) ||
        prefix + alignment - 1 > SIZE_MAX - size ||
        prefix + alignment - 1 + size > SIZE_MAX - COGLET_RT_DEBUG_GUARD_SIZE) {
        coglet_rt_mem_fail();
    }
    size_t total = prefix + alignment - 1 + size + COGLET_RT_DEBUG_GUARD_SIZE;

    unsigned char *raw = (unsigned char *)debug->parent.allocate(
        debug->parent.state,
        total,
        block_alignment
    );
    if (!raw)
        coglet_rt_mem_fail();

    CogletRuntimeDebugBlock *block = (CogletRuntimeDebugBlock *)raw;
    uintptr_t first_user = (uintptr_t)(raw + sizeof(CogletRuntimeDebugBlock) + COGLET_RT_DEBUG_GUARD_SIZE);
    uintptr_t user_address = coglet_rt_align_up_uintptr(first_user, alignment);
    unsigned char *user = (unsigned char *)user_address;

    block->next = debug->head;
    block->user = user;
    block->size = size;
    block->alignment = alignment;
    block->total_size = total;
    block->parent_alignment = block_alignment;
    block->id = debug->next_id++;
    debug->head = block;

    memset(user - COGLET_RT_DEBUG_GUARD_SIZE, COGLET_RT_DEBUG_GUARD_BYTE, COGLET_RT_DEBUG_GUARD_SIZE);
    memset(user, COGLET_RT_DEBUG_ALLOC_BYTE, size);
    memset(user + size, COGLET_RT_DEBUG_GUARD_BYTE, COGLET_RT_DEBUG_GUARD_SIZE);

    ++debug->live_allocations;
    debug->live_bytes += (uint64_t)size;
    ++debug->total_allocations;
    debug->total_bytes += (uint64_t)size;
    return user;
}

void *coglet_rt_mem_debug_create(CogletRuntimeAllocator parent)
{
    if (!coglet_rt_mem_allocator_valid(parent))
        coglet_rt_mem_fail();

    size_t state_alignment = coglet_rt_mem_debug_state_alignment();
    if (state_alignment < sizeof(void *))
        state_alignment = sizeof(void *);
    CogletRuntimeDebugAllocator *debug = (CogletRuntimeDebugAllocator *)parent.allocate(
        parent.state,
        sizeof(CogletRuntimeDebugAllocator),
        state_alignment
    );
    if (!debug)
        coglet_rt_mem_fail();

    debug->parent = parent;
    debug->head = NULL;
    debug->next_id = 1;
    debug->live_allocations = 0;
    debug->live_bytes = 0;
    debug->total_allocations = 0;
    debug->total_bytes = 0;
    debug->error_count = 0;
    return debug;
}

void *coglet_rt_mem_debug_alloc(void *state, size_t size, size_t alignment)
{
    return coglet_rt_mem_debug_allocate_internal((CogletRuntimeDebugAllocator *)state, size, alignment);
}

void coglet_rt_mem_debug_free(
    void *state,
    void *pointer,
    size_t size,
    size_t alignment
) {
    CogletRuntimeDebugAllocator *debug = (CogletRuntimeDebugAllocator *)state;
    if (!debug || !pointer)
        return;

    CogletRuntimeDebugBlock *previous = NULL;
    CogletRuntimeDebugBlock *block = coglet_rt_mem_debug_find(debug, pointer, &previous);
    if (!block) {
        ++debug->error_count;
        fprintf(stderr, "Coglet debug allocator: invalid or double free of %p\n", pointer);
        return;
    }

    if (size != block->size ||
        coglet_rt_mem_checked_alignment((uint64_t)alignment) != block->alignment) {
        ++debug->error_count;
        fprintf(
            stderr,
            "Coglet debug allocator: allocation #%" PRIu64 " freed with mismatched layout (expected %zu/%zu, got %zu/%zu)\n",
            block->id,
            block->size,
            block->alignment,
            size,
            alignment
        );
    }

    coglet_rt_mem_debug_report_corruption(debug, block);
    memset(block->user, COGLET_RT_DEBUG_FREE_BYTE, block->size);

    if (previous)
        previous->next = block->next;
    else
        debug->head = block->next;
    --debug->live_allocations;
    debug->live_bytes -= (uint64_t)block->size;

    debug->parent.free(
        debug->parent.state,
        block,
        block->total_size,
        block->parent_alignment
    );
}

void *coglet_rt_mem_debug_resize(
    void *state,
    void *pointer,
    size_t old_size,
    size_t new_size,
    size_t alignment
) {
    CogletRuntimeDebugAllocator *debug = (CogletRuntimeDebugAllocator *)state;
    if (!pointer)
        return coglet_rt_mem_debug_allocate_internal(debug, new_size, alignment);
    if (new_size == 0) {
        coglet_rt_mem_debug_free(state, pointer, old_size, alignment);
        return NULL;
    }

    CogletRuntimeDebugBlock *block = coglet_rt_mem_debug_find(debug, pointer, NULL);
    if (!block) {
        ++debug->error_count;
        fprintf(stderr, "Coglet debug allocator: resize of invalid or freed pointer %p\n", pointer);
        return NULL;
    }

    if (old_size != block->size) {
        ++debug->error_count;
        fprintf(
            stderr,
            "Coglet debug allocator: allocation #%" PRIu64 " resized with wrong old size (expected %zu, got %zu)\n",
            block->id,
            block->size,
            old_size
        );
    }
    void *resized = coglet_rt_mem_debug_allocate_internal(debug, new_size, alignment);
    if (!resized)
        return NULL;
    memcpy(resized, pointer, block->size < new_size ? block->size : new_size);
    coglet_rt_mem_debug_free(state, pointer, block->size, block->alignment);
    return resized;
}

_Bool coglet_rt_mem_debug_check(void *state)
{
    CogletRuntimeDebugAllocator *debug = (CogletRuntimeDebugAllocator *)state;
    if (!debug)
        return 1;
    for (CogletRuntimeDebugBlock *block = debug->head; block; block = block->next) {
        if (!coglet_rt_mem_debug_block_intact(block))
            return 0;
    }
    return 1;
}

uint64_t coglet_rt_mem_debug_live_allocations(void *state)
{
    CogletRuntimeDebugAllocator *debug = (CogletRuntimeDebugAllocator *)state;
    return debug ? debug->live_allocations : 0;
}

uint64_t coglet_rt_mem_debug_live_bytes(void *state)
{
    CogletRuntimeDebugAllocator *debug = (CogletRuntimeDebugAllocator *)state;
    return debug ? debug->live_bytes : 0;
}

uint64_t coglet_rt_mem_debug_total_allocations(void *state)
{
    CogletRuntimeDebugAllocator *debug = (CogletRuntimeDebugAllocator *)state;
    return debug ? debug->total_allocations : 0;
}

uint64_t coglet_rt_mem_debug_total_bytes(void *state)
{
    CogletRuntimeDebugAllocator *debug = (CogletRuntimeDebugAllocator *)state;
    return debug ? debug->total_bytes : 0;
}

uint64_t coglet_rt_mem_debug_error_count(void *state)
{
    CogletRuntimeDebugAllocator *debug = (CogletRuntimeDebugAllocator *)state;
    return debug ? debug->error_count : 0;
}

uint64_t coglet_rt_mem_debug_report_leaks(void *state)
{
    CogletRuntimeDebugAllocator *debug = (CogletRuntimeDebugAllocator *)state;
    if (!debug)
        return 0;

    if (debug->live_allocations == 0)
        return 0;

    fprintf(
        stderr,
        "Coglet debug allocator: %" PRIu64 " live allocation(s), %" PRIu64 " byte(s)\n",
        debug->live_allocations,
        debug->live_bytes
    );
    for (CogletRuntimeDebugBlock *block = debug->head; block; block = block->next) {
        fprintf(
            stderr,
            "  leak #%" PRIu64 ": size=%zu align=%zu ptr=%p\n",
            block->id,
            block->size,
            block->alignment,
            block->user
        );
    }
    return debug->live_allocations;
}

void coglet_rt_mem_debug_destroy(void *state)
{
    CogletRuntimeDebugAllocator *debug = (CogletRuntimeDebugAllocator *)state;
    if (!debug)
        return;

    (void)coglet_rt_mem_debug_report_leaks(state);

    CogletRuntimeDebugBlock *block = debug->head;
    while (block) {
        CogletRuntimeDebugBlock *next = block->next;
        coglet_rt_mem_debug_report_corruption(debug, block);
        memset(block->user, COGLET_RT_DEBUG_FREE_BYTE, block->size);
        debug->parent.free(
            debug->parent.state,
            block,
            block->total_size,
            block->parent_alignment
        );
        block = next;
    }

    CogletRuntimeAllocator parent = debug->parent;
    size_t state_alignment = coglet_rt_mem_debug_state_alignment();
    if (state_alignment < sizeof(void *))
        state_alignment = sizeof(void *);
    parent.free(parent.state, debug, sizeof(CogletRuntimeDebugAllocator), state_alignment);
}
