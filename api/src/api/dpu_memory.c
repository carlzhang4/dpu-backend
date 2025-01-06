/* Copyright 2020 UPMEM. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <dpu_transfer_matrix.h>
#include <dpu.h>
#include <dpu_api_verbose.h>
#include <dpu_types.h>
#include <dpu_error.h>
#include <dpu_log_utils.h>
#include <dpu_rank.h>
#include <dpu_management.h>
#include <dpu_program.h>
#include <dpu_memory.h>
#include <dpu_internals.h>
#include <dpu_attributes.h>
#include <dpu_thread_job.h>
#include <pthread.h>


#include <stdint.h>
#include <sys/time.h>

#include<pidcomm.h>
#define T uint32_t   //! PIDCOMM



#define IRAM_MASK (0x80000000u)
#define MRAM_MASK (0x08000000u)

#define IRAM_ALIGN (3u)
#define WRAM_ALIGN (2u)

#define ALIGN_MASK(align) (~((1u << (align)) - 1u))
#define IRAM_ALIGN_MASK ALIGN_MASK(IRAM_ALIGN)
#define WRAM_ALIGN_MASK ALIGN_MASK(WRAM_ALIGN)

#define MEMORY_SWITCH(address, iram_statement, mram_statement, wram_statement)                                                   \
    do {                                                                                                                         \
        if (((address)&IRAM_MASK) == IRAM_MASK) {                                                                                \
            iram_statement;                                                                                                      \
        } else if (((address)&MRAM_MASK) == MRAM_MASK) {                                                                         \
            mram_statement;                                                                                                      \
        } else {                                                                                                                 \
            wram_statement;                                                                                                      \
        }                                                                                                                        \
    } while (0)

#define UPDATE_MRAM_COPY_PARAMETERS(address, length)                                                                             \
    do {                                                                                                                         \
        (address) &= ~MRAM_MASK;                                                                                                 \
    } while (0)

#define UPDATE_WRAM_COPY_PARAMETERS(address, length)                                                                             \
    do {                                                                                                                         \
        (address) >>= WRAM_ALIGN;                                                                                                \
        (length) >>= WRAM_ALIGN;                                                                                                 \
    } while (0)

#define UPDATE_IRAM_COPY_PARAMETERS(address, length)                                                                             \
    do {                                                                                                                         \
        (address) = ((address) & ~IRAM_MASK) >> IRAM_ALIGN;                                                                      \
        (length) >>= IRAM_ALIGN;                                                                                                 \
    } while (0)

#define MEMORY_SWITCH_AND_UPDATE_COPY_PARAMETERS(address, length, iram_statement, mram_statement, wram_statement)                \
    do {                                                                                                                         \
        MEMORY_SWITCH((address), iram_statement; UPDATE_IRAM_COPY_PARAMETERS((address), (length)), mram_statement;               \
                      UPDATE_MRAM_COPY_PARAMETERS((address), (length));                                                          \
                      , wram_statement;                                                                                          \
                      UPDATE_WRAM_COPY_PARAMETERS((address), (length)));                                                         \
    } while (0)

#define CHECK_SYMBOL(symbol, symbol_offset, length)                                                                              \
    do {                                                                                                                         \
        dpu_mem_max_addr_t _address = (symbol).address + (symbol_offset);                                                        \
        if (((symbol_offset) + (length)) > (symbol).size) {                                                                      \
            LOG_FN(WARNING, "invalid symbol access (offset:%u + length:%u > size:%u)", symbol_offset, length, (symbol).size);    \
            return DPU_ERR_INVALID_SYMBOL_ACCESS;                                                                                \
        }                                                                                                                        \
        MEMORY_SWITCH(                                                                                                           \
            _address,                                                                                                            \
            if ((_address & ~IRAM_ALIGN_MASK) != 0) {                                                                            \
                LOG_FN(WARNING, "invalid iram access (offset:0x%x, address:0x%x)", symbol_offset, (symbol).address);             \
                return DPU_ERR_INVALID_IRAM_ACCESS;                                                                              \
            } if (((length) & ~IRAM_ALIGN_MASK) != 0) {                                                                          \
                LOG_FN(WARNING, "invalid iram access (length:0x%x)", length);                                                    \
                return DPU_ERR_INVALID_IRAM_ACCESS;                                                                              \
            },                                                                                                                   \
            /* All alignment are allowed */,                                                                                     \
            if ((_address & ~WRAM_ALIGN_MASK) != 0) {                                                                            \
                LOG_FN(WARNING, "invalid wram access (offset:0x%x, address:0x%x)", symbol_offset, (symbol).address);             \
                return DPU_ERR_INVALID_WRAM_ACCESS;                                                                              \
            } if (((length) & ~WRAM_ALIGN_MASK) != 0) {                                                                          \
                LOG_FN(WARNING, "invalid wram access (length:0x%x)", length);                                                    \
                return DPU_ERR_INVALID_WRAM_ACCESS;                                                                              \
            });                                                                                                                  \
    } while (0)

#define DPU_BROADCAST_SET_JOB_TYPE(job_type, address, length)                                                                    \
    do {                                                                                                                         \
        MEMORY_SWITCH_AND_UPDATE_COPY_PARAMETERS(address,                                                                        \
            length,                                                                                                              \
            (job_type) = DPU_THREAD_JOB_COPY_IRAM_TO_RANK,                                                                       \
            (job_type) = DPU_THREAD_JOB_COPY_MRAM_TO_RANK,                                                                       \
            (job_type) = DPU_THREAD_JOB_COPY_WRAM_TO_RANK);                                                                      \
    } while (0)

#define DPU_COPY_MATRIX_SET_JOB_TYPE(job_type, xfer, address, length)                                                            \
    do {                                                                                                                         \
        switch ((xfer)) {                                                                                                        \
            case DPU_XFER_TO_DPU:                                                                                                \
                MEMORY_SWITCH_AND_UPDATE_COPY_PARAMETERS(address,                                                                \
                    length,                                                                                                      \
                    (job_type) = DPU_THREAD_JOB_COPY_IRAM_TO_MATRIX,                                                             \
                    (job_type) = DPU_THREAD_JOB_COPY_MRAM_TO_MATRIX,                                                             \
                    (job_type) = DPU_THREAD_JOB_COPY_WRAM_TO_MATRIX);                                                            \
                break;                                                                                                           \
            case DPU_XFER_FROM_DPU:                                                                                              \
                MEMORY_SWITCH_AND_UPDATE_COPY_PARAMETERS(address,                                                                \
                    length,                                                                                                      \
                    (job_type) = DPU_THREAD_JOB_COPY_IRAM_FROM_MATRIX,                                                           \
                    (job_type) = DPU_THREAD_JOB_COPY_MRAM_FROM_MATRIX,                                                           \
                    (job_type) = DPU_THREAD_JOB_COPY_WRAM_FROM_MATRIX);                                                          \
                break;                                                                                                           \
            default:                                                                                                             \
                return DPU_ERR_INVALID_MEMORY_TRANSFER;                                                                          \
        }                                                                                                                        \
    } while (0)

#define SYNCHRONOUS_FLAGS(flags) ((DPU_XFER_ASYNC & flags) == 0)

static dpu_error_t
dpu_copy_symbol_dpu(struct dpu_t *dpu,
    struct dpu_symbol_t symbol,
    uint32_t symbol_offset,
    void *buffer,
    size_t length,
    dpu_xfer_t xfer,
    dpu_xfer_flags_t flags)
{
    if (!dpu->enabled) {
        return DPU_ERR_DPU_DISABLED;
    }
    CHECK_SYMBOL(symbol, symbol_offset, length);

    dpu_mem_max_addr_t address = symbol.address + symbol_offset;
    struct dpu_rank_t *rank = dpu_get_rank(dpu);
    dpu_error_t status = DPU_OK;

    enum dpu_thread_job_type job_type;
    DPU_COPY_MATRIX_SET_JOB_TYPE(job_type, xfer, address, length);

    uint32_t nr_jobs_per_rank;
    struct dpu_thread_job_sync sync;
    DPU_THREAD_JOB_GET_JOBS(&rank, 1, nr_jobs_per_rank, jobs, &sync, SYNCHRONOUS_FLAGS(flags), status);

    struct dpu_rank_t *rrank __attribute__((unused));
    struct dpu_thread_job *job;
    DPU_THREAD_JOB_SET_JOBS(&rank, rrank, 1, jobs, job, &sync, SYNCHRONOUS_FLAGS(flags), {
        job->type = job_type;
        dpu_transfer_matrix_clear_all(rank, &job->matrix);
        dpu_transfer_matrix_add_dpu(dpu, &job->matrix, buffer);
        job->matrix.offset = address;
        job->matrix.size = length;
    });

    status = dpu_thread_job_do_jobs(&rank, 1, nr_jobs_per_rank, jobs, SYNCHRONOUS_FLAGS(flags), &sync);

    return status;
}

static dpu_error_t
dpu_broadcast_to_symbol_for_ranks(struct dpu_rank_t **ranks,
    uint32_t nr_ranks,
    struct dpu_symbol_t symbol,
    uint32_t symbol_offset,
    const void *src,
    size_t length,
    dpu_xfer_flags_t flags)
{
    CHECK_SYMBOL(symbol, symbol_offset, length);

    dpu_error_t status = DPU_OK;
    dpu_mem_max_addr_t address = symbol.address + symbol_offset;

    enum dpu_thread_job_type job_type;
    DPU_BROADCAST_SET_JOB_TYPE(job_type, address, length);

    uint32_t nr_jobs_per_rank;
    struct dpu_thread_job_sync sync;
    DPU_THREAD_JOB_GET_JOBS(ranks, nr_ranks, nr_jobs_per_rank, jobs, &sync, SYNCHRONOUS_FLAGS(flags), status);

    struct dpu_rank_t *rank __attribute__((unused));
    struct dpu_thread_job *job;
    DPU_THREAD_JOB_SET_JOBS(ranks, rank, nr_ranks, jobs, job, &sync, SYNCHRONOUS_FLAGS(flags), {
        job->type = job_type;
        job->address = address;
        job->length = length;
        job->buffer = src;
    });

    status = dpu_thread_job_do_jobs(ranks, nr_ranks, nr_jobs_per_rank, jobs, SYNCHRONOUS_FLAGS(flags), &sync);

    return status;
}

static const char *
dpu_transfer_to_string(dpu_xfer_t transfer)
{
    switch (transfer) {
        case DPU_XFER_TO_DPU:
            return "HOST_TO_DPU";
        case DPU_XFER_FROM_DPU:
            return "DPU_TO_HOST";
        default:
            return "UNKNOWN";
    }
}

static dpu_error_t
dpu_get_common_program(struct dpu_set_t *dpu_set, struct dpu_program_t **program)
{
    struct dpu_program_t *the_program = NULL;

    switch (dpu_set->kind) {
        case DPU_SET_RANKS:
            for (uint32_t each_rank = 0; each_rank < dpu_set->list.nr_ranks; ++each_rank) {
                struct dpu_rank_t *rank = dpu_set->list.ranks[each_rank];
                uint8_t nr_cis = rank->description->hw.topology.nr_of_control_interfaces;
                uint8_t nr_dpus_per_ci = rank->description->hw.topology.nr_of_dpus_per_control_interface;

                for (int each_ci = 0; each_ci < nr_cis; ++each_ci) {
                    for (int each_dpu = 0; each_dpu < nr_dpus_per_ci; ++each_dpu) {
                        struct dpu_t *dpu = DPU_GET_UNSAFE(rank, each_ci, each_dpu);

                        if (!dpu_is_enabled(dpu)) {
                            continue;
                        }

                        struct dpu_program_t *dpu_program = dpu_get_program(dpu);

                        if (the_program == NULL) {
                            the_program = dpu_program;
                        }

                        if (the_program != dpu_program) {
                            return DPU_ERR_DIFFERENT_DPU_PROGRAMS;
                        }
                    }
                }
            }
            break;
        case DPU_SET_DPU:
            the_program = dpu_get_program(dpu_set->dpu);
            break;
        default:
            return DPU_ERR_INTERNAL;
    }
    if (the_program == NULL) {
        return DPU_ERR_NO_PROGRAM_LOADED;
    }

    *program = the_program;
    return DPU_OK;
}

__API_SYMBOL__ dpu_error_t
dpu_get_symbol(struct dpu_program_t *program, const char *symbol_name, struct dpu_symbol_t *symbol)
{
    LOG_FN(DEBUG, "\"%s\"", symbol_name);

    dpu_error_t status = DPU_OK;

    uint32_t nr_symbols = program->symbols->nr_symbols;

    for (uint32_t each_symbol = 0; each_symbol < nr_symbols; ++each_symbol) {
        dpu_elf_symbol_t *elf_symbol = program->symbols->map + each_symbol;
        if (strcmp(symbol_name, elf_symbol->name) == 0) {
            symbol->address = elf_symbol->value;
            symbol->size = elf_symbol->size;
            goto end;
        }
    }

    status = DPU_ERR_UNKNOWN_SYMBOL;

end:
    return status;
}

__API_SYMBOL__ dpu_error_t
dpu_broadcast_to(struct dpu_set_t dpu_set,
    const char *symbol_name,
    uint32_t symbol_offset,
    const void *src,
    size_t length,
    dpu_xfer_flags_t flags)
{
    LOG_FN(DEBUG, "%s, %d, %zd, 0x%x", symbol_name, symbol_offset, length, flags);
    dpu_error_t status;
    struct dpu_program_t *program;
    struct dpu_symbol_t symbol;

    if ((status = dpu_get_common_program(&dpu_set, &program)) != DPU_OK) {
        return status;
    }

    if ((status = dpu_get_symbol(program, symbol_name, &symbol)) != DPU_OK) {
        return status;
    }

    return dpu_broadcast_to_symbol(dpu_set, symbol, symbol_offset, src, length, flags);
}

__API_SYMBOL__ dpu_error_t
dpu_broadcast_to_symbol(struct dpu_set_t dpu_set,
    struct dpu_symbol_t symbol,
    uint32_t symbol_offset,
    const void *src,
    size_t length,
    dpu_xfer_flags_t flags)
{
    LOG_FN(DEBUG, "0x%08x, %d, %d, %zd, 0x%x", symbol.address, symbol.size, symbol_offset, length, flags);
    dpu_error_t status = DPU_OK;

    switch (dpu_set.kind) {
        case DPU_SET_RANKS:
            status = dpu_broadcast_to_symbol_for_ranks(
                dpu_set.list.ranks, dpu_set.list.nr_ranks, symbol, symbol_offset, src, length, flags);
            break;
        case DPU_SET_DPU:
            status = dpu_copy_symbol_dpu(dpu_set.dpu, symbol, symbol_offset, (void *)src, length, DPU_XFER_TO_DPU, flags);
            break;
        default:
            return DPU_ERR_INTERNAL;
    }

    return status;
}

__API_SYMBOL__ dpu_error_t
dpu_copy_to(struct dpu_set_t dpu_set, const char *symbol_name, uint32_t symbol_offset, const void *src, size_t length)
{
    LOG_FN(DEBUG, "\"%s\", %d, %p, %zd)", symbol_name, symbol_offset, src, length);

    dpu_error_t status;
    struct dpu_program_t *program;
    struct dpu_symbol_t symbol;

    if ((status = dpu_get_common_program(&dpu_set, &program)) != DPU_OK) {
        return status;
    }

    if ((status = dpu_get_symbol(program, symbol_name, &symbol)) != DPU_OK) {
        return status;
    }

    return dpu_broadcast_to_symbol(dpu_set, symbol, symbol_offset, src, length, DPU_XFER_DEFAULT);
}

__API_SYMBOL__ dpu_error_t
dpu_copy_from(struct dpu_set_t dpu_set, const char *symbol_name, uint32_t symbol_offset, void *dst, size_t length)
{
    LOG_FN(DEBUG, "\"%s\", %d, %p, %zd)", symbol_name, symbol_offset, dst, length);

    if (dpu_set.kind != DPU_SET_DPU) {
        return dpu_set.kind == DPU_SET_RANKS ? DPU_ERR_INVALID_DPU_SET : DPU_ERR_INTERNAL;
    }

    struct dpu_t *dpu = dpu_set.dpu;
    dpu_error_t status = DPU_OK;

    struct dpu_symbol_t symbol;
    struct dpu_program_t *program;

    if ((program = dpu_get_program(dpu)) == NULL) {
        return DPU_ERR_NO_PROGRAM_LOADED;
    }

    if ((status = dpu_get_symbol(program, symbol_name, &symbol)) != DPU_OK) {
        return status;
    }

    return dpu_copy_symbol_dpu(dpu_set.dpu, symbol, symbol_offset, dst, length, DPU_XFER_FROM_DPU, DPU_XFER_DEFAULT);
}

__API_SYMBOL__ dpu_error_t
dpu_copy_to_symbol(struct dpu_set_t dpu_set, struct dpu_symbol_t symbol, uint32_t symbol_offset, const void *src, size_t length)
{
    LOG_FN(DEBUG, "0x%08x, %d, %d, %p, %zd)", symbol.address, symbol.size, symbol_offset, src, length);

    return dpu_broadcast_to_symbol(dpu_set, symbol, symbol_offset, src, length, DPU_XFER_DEFAULT);
}

__API_SYMBOL__ dpu_error_t
dpu_copy_from_symbol(struct dpu_set_t dpu_set, struct dpu_symbol_t symbol, uint32_t symbol_offset, void *dst, size_t length)
{
    LOG_FN(DEBUG, "0x%08x, %d, %d, %p, %zd)", symbol.address, symbol.size, symbol_offset, dst, length);

    if (dpu_set.kind != DPU_SET_DPU) {
        return dpu_set.kind == DPU_SET_RANKS ? DPU_ERR_INVALID_DPU_SET : DPU_ERR_INTERNAL;
    }

    return dpu_copy_symbol_dpu(dpu_set.dpu, symbol, symbol_offset, dst, length, DPU_XFER_FROM_DPU, DPU_XFER_DEFAULT);
}

struct dpu_transfer_matrix *
dpu_get_transfer_matrix(struct dpu_rank_t *rank)
{
    if (rank->api.callback_tid_set && rank->api.callback_tid == pthread_self()) {
        return &rank->api.callback_matrix;
    } else {
        return &rank->api.matrix;
    }
}

struct dpu_rank_t *
dpu_get_as_rank_t(struct dpu_set_t set)
{
    return set.list.ranks[0];
}

static inline void
reset_sg_buffer_pool(struct dpu_set_t rank, struct sg_xfer_buffer *sg_buffer_pool)
{
    struct dpu_set_t dpu;
    DPU_FOREACH (rank, dpu) {
        uint32_t dpu_transfer_matrix_index = get_transfer_matrix_index(dpu.dpu->rank, dpu.dpu->slice_id, dpu.dpu->dpu_id);
        // set the number of block to 0
        sg_buffer_pool[dpu_transfer_matrix_index].nr_blocks = 0;
    }
}

struct sg_xfer_callback_args {
    /** get_block information */
    get_block_t *get_block_info;
    /** direction of the transfer */
    dpu_xfer_t xfer;
    /** the DPU symbol address where the transfer starts */
    dpu_mem_max_addr_t symbol_addr;
    /** length the number of bytes to copy */
    size_t length;
    /** flags options of the transfer */
    dpu_sg_xfer_flags_t flags;
    /** reference counter used to free the callback arguments*/
    uint32_t freeing_refcpt;
};

static pthread_mutex_t freeing_refcpt_mutex = PTHREAD_MUTEX_INITIALIZER;

static inline void
free_cb_args(struct sg_xfer_callback_args *args)
{
    pthread_mutex_lock(&freeing_refcpt_mutex);
    args->freeing_refcpt--;
    if (!args->freeing_refcpt) {
        free(args->get_block_info->args);
        free(args->get_block_info);
        free(args);
    }
    pthread_mutex_unlock(&freeing_refcpt_mutex);
}
__PERF_PROFILING_SYMBOL__ dpu_error_t
sg_xfer_rank_handler(struct dpu_set_t rank, __attribute((unused)) uint32_t rank_id, void *cb_args)
{
    struct dpu_set_t dpu;
    struct sg_block_info binfo;
    dpu_error_t status;
    struct dpu_transfer_matrix transfer_matrix;

    // retreive arguments of the rank
    struct sg_xfer_callback_args *args = (struct sg_xfer_callback_args *)(cb_args);

    uint32_t dpu_rank_offset = dpu_get_as_rank_t(rank)->dpu_offset;
    struct sg_xfer_buffer *sg_buffer_pool = dpu_get_as_rank_t(rank)->api.sg_buffer_pool;

    dpu_xfer_t xfer = args->xfer;

    // clearing the transfer matrix before using it
    dpu_transfer_matrix_clear_all(dpu_get_as_rank_t(rank), &transfer_matrix);

    mram_addr_t mram_byte_offset = args->symbol_addr;
    UPDATE_MRAM_COPY_PARAMETERS(mram_byte_offset, length);

    transfer_matrix.type = DPU_SG_XFER_MATRIX;
    transfer_matrix.size = args->length;
    transfer_matrix.offset = mram_byte_offset;

    uint32_t block_index = 0;
    uint32_t rank_dpu_index;

    bool check_length = ((args->flags & DPU_SG_XFER_DISABLE_LENGTH_CHECK) == 0);

    if (!dpu_get_as_rank_t(rank)->api.sg_xfer_enabled)
        return DPU_ERR_SG_NOT_ACTIVATED;

    // add each block of each DPU
    DPU_FOREACH (rank, dpu, rank_dpu_index) {

        uint32_t dpu_transfer_matrix_index = get_transfer_matrix_index(dpu.dpu->rank, dpu.dpu->slice_id, dpu.dpu->dpu_id);
        uint32_t dpu_index = rank_dpu_index + dpu_rank_offset;
        uint32_t dpu_sg_buffer_pool_max_nr_blocks = sg_buffer_pool[dpu_transfer_matrix_index].max_nr_blocks;
        uint32_t dpu_nr_bytes_to_transfer = 0;

        while (1) {

            // call the get_block function
            bool block_exists = args->get_block_info->f(&binfo, dpu_index, block_index, args->get_block_info->args);

            if (block_exists) {
                // the number of blocks exceed the capacity of the sg buffer pool
                if (block_index >= dpu_sg_buffer_pool_max_nr_blocks)
                    return DPU_ERR_SG_TOO_MANY_BLOCKS;
                // if DPU_SG_XFER_DISABLE_LENGTH_CHECK is provided, sending more bytes than "length" is authorized
                if (binfo.length && (check_length && dpu_nr_bytes_to_transfer >= args->length))
                    return DPU_ERR_SG_LENGTH_MISMATCH;
            }
            // no more block to add for this DPU
            else {
                // the precedent block was the last one, so we can check the number of bytes to be send for this DPU
                // if DPU_SG_XFER_DISABLE_LENGTH_CHECK is not provided, the number of bytes must be equal to length
                if (check_length && dpu_nr_bytes_to_transfer != args->length)
                    return DPU_ERR_SG_LENGTH_MISMATCH;

                block_index = 0;
                break;
            }

            // add this block if not empty
            if (binfo.length) {
                dpu_nr_bytes_to_transfer += binfo.length;
                switch ((args->xfer)) {
                    case DPU_XFER_TO_DPU:
                    case DPU_XFER_FROM_DPU:

                        // if this is the first time we add a new block for this DPU
                        // initialize the current DPU sg buffer pool pointer
                        if (transfer_matrix.sg_ptr[dpu_transfer_matrix_index] == NULL)
                            transfer_matrix.sg_ptr[dpu_transfer_matrix_index] = &sg_buffer_pool[dpu_transfer_matrix_index];

                        // add the new block to the transfer matrix
                        if ((status = dpu_transfer_matrix_add_dpu_block(dpu.dpu, &transfer_matrix, binfo.addr, binfo.length))
                            != DPU_OK)
                            return status;
                        break;
                    default:
                        return DPU_ERR_INVALID_MEMORY_TRANSFER;
                }
            }

            block_index++;
        }
    }

    // launch the transfer
    switch (xfer) {
        case DPU_XFER_TO_DPU:
            if ((status = dpu_copy_to_mrams(dpu_get_as_rank_t(rank), &transfer_matrix)) != DPU_OK)
                return status;
            break;
        case DPU_XFER_FROM_DPU:
            if ((status = dpu_copy_from_mrams(dpu_get_as_rank_t(rank), &transfer_matrix)) != DPU_OK)
                return status;
            break;
        default:
            return DPU_ERR_INVALID_MEMORY_TRANSFER;
    }

    // reset the scatter gather buffer pool
    reset_sg_buffer_pool(rank, sg_buffer_pool);

    // freeing the callback arguments
    free_cb_args(args);

    return DPU_OK;
}

__API_SYMBOL__ dpu_error_t
dpu_push_sg_xfer_symbol(struct dpu_set_t dpu_set,
    dpu_xfer_t xfer,
    struct dpu_symbol_t symbol,
    uint32_t symbol_offset,
    size_t length,
    get_block_t *get_block_info,
    dpu_sg_xfer_flags_t flags)
{
    LOG_FN(DEBUG,
        "%s, 0x%08x, %d, %d, %zd, 0x%x",
        dpu_transfer_to_string(xfer),
        symbol.address,
        symbol.size,
        symbol_offset,
        length,
        flags);

    dpu_error_t status;

    bool is_async = ((flags & DPU_SG_XFER_ASYNC) != 0);

    switch (dpu_set.kind) {
        case DPU_SET_RANKS: {
            break;
        }
        case DPU_SET_DPU: {
            fprintf(stderr, "scattered xfer to one DPU is not supported \n");
            return DPU_ERR_INTERNAL;
        }
        default:
            return DPU_ERR_INTERNAL;
    }

    dpu_mem_max_addr_t symbol_addr = (symbol).address + (symbol_offset);

    // check if the symbol is a MRAM symbol
    {
        if (!(((symbol_addr)&MRAM_MASK) == MRAM_MASK))
            return DPU_ERR_SG_NOT_MRAM_SYMBOL;
    }

    // alloc callback arguments and initialize ref counter
    // the arguments are freed at the end of the last executed callback
    struct sg_xfer_callback_args *sg_xfer_cb_args = malloc(sizeof(struct sg_xfer_callback_args));
    dpu_get_nr_ranks(dpu_set, &sg_xfer_cb_args->freeing_refcpt);
    sg_xfer_cb_args->get_block_info = malloc(sizeof(struct get_block_t));
    sg_xfer_cb_args->get_block_info->f = get_block_info->f;
    sg_xfer_cb_args->get_block_info->args = malloc(get_block_info->args_size);
    memcpy(sg_xfer_cb_args->get_block_info->args, get_block_info->args, get_block_info->args_size);
    sg_xfer_cb_args->get_block_info->args_size = get_block_info->args_size;
    sg_xfer_cb_args->xfer = xfer;
    sg_xfer_cb_args->symbol_addr = symbol_addr;
    sg_xfer_cb_args->length = length;
    sg_xfer_cb_args->flags = flags;

    if ((status
            = dpu_callback(dpu_set, sg_xfer_rank_handler, sg_xfer_cb_args, is_async ? DPU_CALLBACK_ASYNC : DPU_CALLBACK_DEFAULT))
        != DPU_OK)
        return status;

    return DPU_OK;
}

__API_SYMBOL__ dpu_error_t
dpu_push_sg_xfer(struct dpu_set_t dpu_set,
    dpu_xfer_t xfer,
    const char *symbol_name,
    uint32_t symbol_offset,
    size_t length,
    get_block_t *get_block_info,
    dpu_sg_xfer_flags_t flags)
{
    LOG_FN(DEBUG, "%s, %s, %d, %zd, 0x%x", dpu_transfer_to_string(xfer), symbol_name, symbol_offset, length, flags);

    dpu_error_t status;

    struct dpu_program_t *program;
    struct dpu_symbol_t symbol;

    if ((status = dpu_get_common_program(&dpu_set, &program)) != DPU_OK) {
        return status;
    }

    if ((status = dpu_get_symbol(program, symbol_name, &symbol)) != DPU_OK) {
        return status;
    }

    return dpu_push_sg_xfer_symbol(dpu_set, xfer, symbol, symbol_offset, length, get_block_info, flags);
}

__API_SYMBOL__ dpu_error_t
dpu_prepare_xfer(struct dpu_set_t dpu_set, void *buffer)
{
    LOG_FN(DEBUG, "%p", buffer);

    switch (dpu_set.kind) {
        case DPU_SET_RANKS:
            for (uint32_t each_rank = 0; each_rank < dpu_set.list.nr_ranks; ++each_rank) {
                struct dpu_rank_t *rank = dpu_set.list.ranks[each_rank];
                uint8_t nr_cis = rank->description->hw.topology.nr_of_control_interfaces;
                uint8_t nr_dpus_per_ci = rank->description->hw.topology.nr_of_dpus_per_control_interface;

                for (uint8_t each_ci = 0; each_ci < nr_cis; ++each_ci) {
                    for (uint8_t each_dpu = 0; each_dpu < nr_dpus_per_ci; ++each_dpu) {
                        struct dpu_t *dpu = DPU_GET_UNSAFE(rank, each_ci, each_dpu);

                        if (!dpu_is_enabled(dpu)) {
                            continue;
                        }

                        dpu_transfer_matrix_add_dpu(dpu, dpu_get_transfer_matrix(rank), buffer);
                    }
                }
            }

            break;
        case DPU_SET_DPU: {
            struct dpu_t *dpu = dpu_set.dpu;

            if (!dpu_is_enabled(dpu)) {
                return DPU_ERR_DPU_DISABLED;
            }

            dpu_transfer_matrix_add_dpu(dpu, dpu_get_transfer_matrix(dpu_get_rank(dpu)), buffer);

            break;
        }
        default:
            return DPU_ERR_INTERNAL;
    }

    return DPU_OK;
}

__API_SYMBOL__ dpu_error_t
dpu_push_xfer(struct dpu_set_t dpu_set,
    dpu_xfer_t xfer,
    const char *symbol_name,
    uint32_t symbol_offset,
    size_t length,
    dpu_xfer_flags_t flags)
{
    LOG_FN(DEBUG, "%s, %s, %d, %zd, 0x%x", dpu_transfer_to_string(xfer), symbol_name, symbol_offset, length, flags);

    dpu_error_t status;
    struct dpu_program_t *program;
    struct dpu_symbol_t symbol;

    if ((status = dpu_get_common_program(&dpu_set, &program)) != DPU_OK) {
        return status;
    }

    if ((status = dpu_get_symbol(program, symbol_name, &symbol)) != DPU_OK) {
        return status;
    }

    return dpu_push_xfer_symbol(dpu_set, xfer, symbol, symbol_offset, length, flags);
}

__API_SYMBOL__ dpu_error_t
dpu_push_xfer_symbol(struct dpu_set_t dpu_set,
    dpu_xfer_t xfer,
    struct dpu_symbol_t symbol,
    uint32_t symbol_offset,
    size_t length,
    dpu_xfer_flags_t flags)
{
    LOG_FN(DEBUG,
        "%s, 0x%08x, %d, %d, %zd, 0x%x",
        dpu_transfer_to_string(xfer),
        symbol.address,
        symbol.size,
        symbol_offset,
        length,
        flags);

    dpu_error_t status = DPU_OK;
    dpu_mem_max_addr_t address = symbol.address + symbol_offset;

    switch (dpu_set.kind) {
        case DPU_SET_RANKS: {
            CHECK_SYMBOL(symbol, symbol_offset, length);

            uint32_t nr_ranks = dpu_set.list.nr_ranks;
            struct dpu_rank_t **ranks = dpu_set.list.ranks;

            enum dpu_thread_job_type job_type;
            DPU_COPY_MATRIX_SET_JOB_TYPE(job_type, xfer, address, length);

            uint32_t nr_jobs_per_rank;
            struct dpu_thread_job_sync sync;
            DPU_THREAD_JOB_GET_JOBS(ranks, nr_ranks, nr_jobs_per_rank, jobs, &sync, SYNCHRONOUS_FLAGS(flags), status);

            struct dpu_rank_t *rank;
            struct dpu_thread_job *job;
            DPU_THREAD_JOB_SET_JOBS(ranks, rank, nr_ranks, jobs, job, &sync, SYNCHRONOUS_FLAGS(flags), {
                dpu_transfer_matrix_copy(&job->matrix, dpu_get_transfer_matrix(rank));
                job->type = job_type;
                job->matrix.offset = address;
                job->matrix.size = length;
            });

            status = dpu_thread_job_do_jobs(ranks, nr_ranks, nr_jobs_per_rank, jobs, SYNCHRONOUS_FLAGS(flags), &sync);

        } break;
        case DPU_SET_DPU: {
            struct dpu_t *dpu = dpu_set.dpu;
            struct dpu_rank_t *rank = dpu_get_rank(dpu);
            uint8_t dpu_transfer_matrix_index = get_transfer_matrix_index(rank, dpu_get_slice_id(dpu), dpu_get_member_id(dpu));
            void *buffer = dpu_get_transfer_matrix(rank)->ptr[dpu_transfer_matrix_index];

            status = dpu_copy_symbol_dpu(dpu, symbol, symbol_offset, buffer, length, xfer, flags);

            break;
        }
        default:
            return DPU_ERR_INTERNAL;
    }

    if (flags != DPU_XFER_NO_RESET) {
        switch (dpu_set.kind) {
            case DPU_SET_RANKS:
                for (uint32_t each_rank = 0; each_rank < dpu_set.list.nr_ranks; ++each_rank) {
                    struct dpu_rank_t *rank = dpu_set.list.ranks[each_rank];
                    dpu_transfer_matrix_clear_all(rank, dpu_get_transfer_matrix(rank));
                }
                break;
            case DPU_SET_DPU: {
                struct dpu_t *dpu = dpu_set.dpu;
                dpu_transfer_matrix_clear_dpu(dpu, dpu_get_transfer_matrix(dpu_get_rank(dpu)));
                break;
            }
            default:
                return DPU_ERR_INTERNAL;
        }
    }

    return status;
}


//! PIDCOMM

/*PID-Comm*/
#ifndef DPU_BINARY_RELOCATE_CLOCKWISE
#define DPU_BINARY_RELOCATE_CLOCKWISE "../pidcomm_lib/bin/data_relocate_clockwise"
#endif
#ifndef DPU_BINARY_RELOCATE_CLOCKWISE_INT8
#define DPU_BINARY_RELOCATE_CLOCKWISE_INT8 "../pidcomm_lib/bin/data_relocate_clockwise_int8"
#endif
#ifndef DPU_BINARY_RELOCATE_CLOCKWISE_INT32
#define DPU_BINARY_RELOCATE_CLOCKWISE_INT32 "../pidcomm_lib/bin/data_relocate_clockwise_int32"
#endif

#ifndef DPU_BINARY_RELOCATE_REVERSE_CLOCKWISE
#define DPU_BINARY_RELOCATE_REVERSE_CLOCKWISE "../pidcomm_lib/bin/data_relocate_reverse_clockwise"
#endif
#ifndef DPU_BINARY_RELOCATE_REVERSE_CLOCKWISE_INT8
#define DPU_BINARY_RELOCATE_REVERSE_CLOCKWISE_INT8 "../pidcomm_lib/bin/data_relocate_reverse_clockwise_int8"
#endif
#ifndef DPU_BINARY_RELOCATE_REVERSE_CLOCKWISE_INT32
#define DPU_BINARY_RELOCATE_REVERSE_CLOCKWISE_INT32 "../pidcomm_lib/bin/data_relocate_reverse_clockwise_int32"
#endif

#ifndef DPU_BINARY_RELOCATE_COUNTERCLOCKWISE
#define DPU_BINARY_RELOCATE_COUNTERCLOCKWISE "../pidcomm_lib/bin/data_relocate_counterclockwise"
#endif
#ifndef DPU_BINARY_RELOCATE_COUNTERCLOCKWISE_INT8
#define DPU_BINARY_RELOCATE_COUNTERCLOCKWISE_INT8 "../pidcomm_lib/bin/data_relocate_counterclockwise_int8"
#endif
#ifndef DPU_BINARY_RELOCATE_COUNTERCLOCKWISE_INT32
#define DPU_BINARY_RELOCATE_COUNTERCLOCKWISE_INT32 "../pidcomm_lib/bin/data_relocate_counterclockwise_int32"
#endif

#ifndef DPU_BINARY_RELOCATE_INCREMENTAL_COUNTERCLOCKWISE
#define DPU_BINARY_RELOCATE_INCREMENTAL_COUNTERCLOCKWISE "../pidcomm_lib/bin/data_relocate_incremental_counterclockwise"
#endif
#ifndef DPU_BINARY_RELOCATE_INCREMENTAL_COUNTERCLOCKWISE_INT8
#define DPU_BINARY_RELOCATE_INCREMENTAL_COUNTERCLOCKWISE_INT8 "../pidcomm_lib/bin/data_relocate_incremental_counterclockwise_int8"
#endif
#ifndef DPU_BINARY_RELOCATE_INCREMENTAL_COUNTERCLOCKWISE_INT32
#define DPU_BINARY_RELOCATE_INCREMENTAL_COUNTERCLOCKWISE_INT32 "../pidcomm_lib/bin/data_relocate_incremental_counterclockwise_int32"
#endif

#ifndef DPU_BINARY_RELOCATE_MODIFIED_CLOCKWISE
#define DPU_BINARY_RELOCATE_MODIFIED_CLOCKWISE "../pidcomm_lib/bin/data_relocate_modified_clockwise"
#endif
#ifndef DPU_BINARY_RELOCATE_MODIFIED_CLOCKWISE_INT8
#define DPU_BINARY_RELOCATE_MODIFIED_CLOCKWISE_INT8 "../pidcomm_lib/bin/data_relocate_modified_clockwise_int8"
#endif
#ifndef DPU_BINARY_RELOCATE_MODIFIED_CLOCKWISE_INT32
#define DPU_BINARY_RELOCATE_MODIFIED_CLOCKWISE_INT32 "../pidcomm_lib/bin/data_relocate_modified_clockwise_int32"
#endif

#ifndef DPU_BINARY_RELOCATE_CLOCKWISE_SHORT
#define DPU_BINARY_RELOCATE_CLOCKWISE_SHORT "../pidcomm_lib/bin/data_relocate_clockwise_short"
#endif
#ifndef DPU_BINARY_RELOCATE_CLOCKWISE_SHORT_INT8
#define DPU_BINARY_RELOCATE_CLOCKWISE_SHORT_INT8 "../pidcomm_lib/bin/data_relocate_clockwise_short_int8"
#endif
#ifndef DPU_BINARY_RELOCATE_CLOCKWISE_SHORT_INT32
#define DPU_BINARY_RELOCATE_CLOCKWISE_SHORT_INT32 "../pidcomm_lib/bin/data_relocate_clockwise_short_int32"
#endif

#ifndef DPU_BINARY_RELOCATE_MODIFIED_REVERSE_CLOCKWISE
#define DPU_BINARY_RELOCATE_MODIFIED_REVERSE_CLOCKWISE "../pidcomm_lib/bin/data_relocate_modified_reverse_clockwise"
#endif
#ifndef DPU_BINARY_RELOCATE_MODIFIED_REVERSE_CLOCKWISE_INT8
#define DPU_BINARY_RELOCATE_MODIFIED_REVERSE_CLOCKWISE_INT8 "../pidcomm_lib/bin/data_relocate_modified_reverse_clockwise"
#endif
#ifndef DPU_BINARY_RELOCATE_MODIFIED_REVERSE_CLOCKWISE_INT32
#define DPU_BINARY_RELOCATE_MODIFIED_REVERSE_CLOCKWISE_INT32 "../pidcomm_lib/bin/data_relocate_modified_reverse_clockwise"
#endif

#ifndef DPU_BINARY_RELOCATE_REVERSE_CLOCKWISE_SHORT
#define DPU_BINARY_RELOCATE_REVERSE_CLOCKWISE_SHORT "../pidcomm_lib/bin/data_relocate_reverse_clockwise_short"
#endif
#ifndef DPU_BINARY_RELOCATE_REVERSE_CLOCKWISE_SHORT_INT8
#define DPU_BINARY_RELOCATE_REVERSE_CLOCKWISE_SHORT_INT8 "../pidcomm_lib/bin/data_relocate_reverse_clockwise_short_int8"
#endif
#ifndef DPU_BINARY_RELOCATE_REVERSE_CLOCKWISE_SHORT_INT32
#define DPU_BINARY_RELOCATE_REVERSE_CLOCKWISE_SHORT_INT32 "../pidcomm_lib/bin/data_relocate_reverse_clockwise_short_int32"
#endif


typedef struct {
    uint32_t start_offset;
    uint32_t target_offset;
    uint32_t total_data_size;
    uint32_t num_comm_dpu;
    uint32_t each_dpu;
    bool no_rotate;
    uint32_t num_row;
    uint32_t comm_type;
    uint32_t a_length;
    uint32_t num_comm_rg;
} dpu_arguments_comm_t;




__API_SYMBOL__
hypercube_manager* init_hypercube_manager(struct dpu_set_t dpu_set, uint32_t dimension, uint32_t* axis_len){
    hypercube_manager* manager = malloc(sizeof(hypercube_manager));

    manager->dpu_set = dpu_set;
    manager->dimension = dimension;
    manager->axis_len = axis_len;

    return manager;
}

__API_SYMBOL__ dpu_error_t
all_to_all(struct dpu_set_t *comm_dpu_set, uint32_t src_start_offset, uint32_t dst_start_offset, uint32_t byte_length, uint32_t comm_type, uint32_t communication_buffer_offset, uint32_t dimension, uint32_t* axis_len, uint32_t* comm_axis)
{
    dpu_error_t status = DPU_OK;
    struct dpu_rank_t *rank_set = comm_dpu_set->list.ranks[0];
    status = (dpu_error_t)rank_set->handler_context->handler->all_to_all_rns(comm_dpu_set, src_start_offset, dst_start_offset, byte_length, comm_type, communication_buffer_offset, dimension, axis_len, comm_axis);
    return status;
}

__API_SYMBOL__ dpu_error_t
all_gather(struct dpu_set_t *comm_dpu_set, uint32_t src_start_offset, uint32_t dst_start_offset, uint32_t byte_length, uint32_t comm_type, uint32_t communication_buffer_offset, uint32_t dimension, uint32_t* axis_len, uint32_t* comm_axis)
{
    dpu_error_t status = DPU_OK;
    struct dpu_rank_t *rank_set = comm_dpu_set->list.ranks[0];
    status = rank_set->handler_context->handler->all_gather_rns(comm_dpu_set, src_start_offset, dst_start_offset, byte_length, comm_type, communication_buffer_offset, dimension, axis_len, comm_axis);
    return status;
}

__API_SYMBOL__ dpu_error_t
all_reduce(struct dpu_set_t *comm_dpu_set, uint32_t src_start_offset, uint32_t dst_start_offset, uint32_t byte_length, uint32_t comm_type, uint32_t communication_buffer_offset, uint32_t dimension, uint32_t* axis_len, uint32_t* comm_axis, uint32_t size, uint32_t reduce_type)
{
    dpu_error_t status = DPU_OK;
    struct dpu_rank_t *rank_set = comm_dpu_set->list.ranks[0];
    status = rank_set->handler_context->handler->all_reduce_rns(comm_dpu_set, src_start_offset, dst_start_offset, byte_length, comm_type, communication_buffer_offset, dimension, axis_len, comm_axis, size, reduce_type);
    return status;
}

__API_SYMBOL__ dpu_error_t
reduce_scatter(struct dpu_set_t *comm_dpu_set, uint32_t src_start_offset, uint32_t dst_start_offset, uint32_t byte_length, uint32_t comm_type, uint32_t communication_buffer_offset, uint32_t dimension, uint32_t* axis_len, uint32_t* comm_axis, uint32_t size)
{
    dpu_error_t status = DPU_OK;
    struct dpu_rank_t *rank_set = comm_dpu_set->list.ranks[0];
    status = rank_set->handler_context->handler->reduce_scatter_rns(comm_dpu_set, src_start_offset, dst_start_offset, byte_length, comm_type, communication_buffer_offset, dimension, axis_len, comm_axis, size);
    return status;
}

__API_SYMBOL__
void pidcomm_alltoall(hypercube_manager* manager, char* comm, uint32_t total_data_size, uint32_t start_offset,
                        uint32_t target_offset, uint32_t buffer_offset){

    struct dpu_set_t dpu_set = manager->dpu_set;
    uint32_t dimension = manager->dimension;
    uint32_t* axis_len = manager->axis_len;

    uint32_t* comm_axis = malloc(sizeof(uint32_t) * dimension);

    for(uint32_t dim=0; dim<dimension; dim++){
        comm_axis[dim] = (int)(*(comm+dim))-48;  //* ascii of '0' is 48
    }


    struct dpu_set_t dpu;
    uint32_t nr_dpus;
    DPU_ASSERT(dpu_get_nr_dpus(dpu_set, &nr_dpus));
    dpu_arguments_comm_t* dpu_argument = (dpu_arguments_comm_t*) malloc(sizeof(dpu_arguments_comm_t) * nr_dpus);
    uint32_t num_comm_dpu = 1;
    uint32_t comm_type;

    if(comm_axis[0] == 1){
        comm_type = 0;
    }
    else comm_type = 1;

    for(uint32_t dim=0; dim<dimension; dim++){
        if(comm_axis[dim]==1){
            num_comm_dpu *= axis_len[dim];
        }
    }

    T** result = (T**) calloc(nr_dpus, sizeof(T*));
    for(int i=0; i<(int)nr_dpus; i++)
        result[i] = (T*) calloc(total_data_size/sizeof(T), sizeof(T));
    int i;

    uint32_t num_comm_rg = 1;
    for(uint32_t dim=0, len = 1; dim<dimension&&len<8; len*=axis_len[dim], dim++){  //TODO changed from PID, mainly bug
        if(comm_axis[dim] == 1){
            if (axis_len[dim] <= (8/len)) num_comm_rg *= axis_len[dim];
            else num_comm_rg *= (8/len);
        }
        if(num_comm_rg >= 8) num_comm_rg = 8;
    }

    //relocate before kernel
    if(!comm_type){
    //if(0){
        //DPU_ASSERT(dpu_load(dpu_set, getenv("DPU_BINARY_RELOCATE_2"), NULL));
        DPU_ASSERT(dpu_load(dpu_set, DPU_BINARY_RELOCATE_CLOCKWISE, NULL));

        for(int i=0; i<(int)nr_dpus; i++){
            //printf("%d\n",i);
            dpu_argument[i].each_dpu = i;
            dpu_argument[i].start_offset = start_offset;
            dpu_argument[i].target_offset = start_offset;
            dpu_argument[i].total_data_size = total_data_size;
            dpu_argument[i].num_comm_dpu = num_comm_dpu;
            dpu_argument[i].no_rotate = 0;
            dpu_argument[i].comm_type = comm_type;
            dpu_argument[i].a_length = axis_len[0];

        }
        

        DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus){

            DPU_ASSERT(dpu_prepare_xfer(dpu, dpu_argument+i));     
        }
        DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, "DPU_INPUT_ARGUMENTS_RS1", 0, sizeof(dpu_arguments_comm_t), DPU_XFER_DEFAULT));

        // Run kernel on DPUs
        DPU_ASSERT(dpu_launch(dpu_set, DPU_SYNCHRONOUS));
    }
    i=0;
    // DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus){
    //     DPU_ASSERT(dpu_prepare_xfer(dpu, result[i]));
    // }
    //DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_FROM_DPU, DPU_MRAM_HEAP_POINTER_NAME, 0, 8, DPU_XFER_DEFAULT));  //! seems unused
    // DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_FROM_DPU, DPU_MRAM_HEAP_POINTER_NAME, start_offset , total_data_size, DPU_XFER_DEFAULT));
    // printf("***************************\n");
    // uint32_t data_num_per_dpu = total_data_size/sizeof(uint32_t);
    // for(uint32_t i=0; i<nr_dpus; i++){
    //     for(uint32_t j=0;j<data_num_per_dpu;j++){
    //             printf("%.4x ",result[i][j]);
    //             //fprintf(stderr,"%d ",result[i*data_num_per_dpu+j]);
    //     }
    //     printf("\n");
    // }
    all_to_all(&dpu_set, start_offset, start_offset, total_data_size/num_comm_dpu, comm_type, buffer_offset, dimension, axis_len, comm_axis);

    // i=0;
    // DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus){
    //     DPU_ASSERT(dpu_prepare_xfer(dpu, result[i]));
    // }
    // DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_FROM_DPU, DPU_MRAM_HEAP_POINTER_NAME, start_offset + buffer_offset, total_data_size, DPU_XFER_DEFAULT));
    // printf("***************************\n");
    // uint32_t data_num_per_dpu = total_data_size/sizeof(uint32_t);
    // for(uint32_t i=0; i<nr_dpus; i++){
    //     for(uint32_t j=0;j<data_num_per_dpu;j++){
    //             printf("%.8x ",result[i][j]);
    //             //fprintf(stderr,"%d ",result[i*data_num_per_dpu+j]);
    //     }
    //     printf("\n");
    // }

    // i=0;
    // DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus){
    //     DPU_ASSERT(dpu_prepare_xfer(dpu, result[i]));        
    // }
    // DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_FROM_DPU, DPU_MRAM_HEAP_POINTER_NAME, start_offset + buffer_offset, total_data_size, DPU_XFER_DEFAULT));


    //relocate after kernel
    if(axis_len[0]==2 && axis_len[1]==2 && ((comm_axis[0]==1 && comm_axis[1]==0 && comm_axis[2]==1) || (comm_axis[0]==0 && comm_axis[1]==1 && comm_axis[2]==0))){
        //DPU_ASSERT(dpu_load(dpu_set, getenv("DPU_ALLTOALL_22"), NULL));
        DPU_ASSERT(dpu_load(dpu_set, DPU_BINARY_RELOCATE_MODIFIED_CLOCKWISE, NULL));

        for(int i=0; i<(int)nr_dpus; i++){
            dpu_argument[i].each_dpu = i;
            dpu_argument[i].start_offset = start_offset + buffer_offset;
            dpu_argument[i].target_offset = target_offset;
            dpu_argument[i].total_data_size = total_data_size;
            dpu_argument[i].num_comm_dpu = num_comm_dpu;
            dpu_argument[i].no_rotate = 0;
            dpu_argument[i].comm_type = comm_type;
            dpu_argument[i].a_length = axis_len[0];
            dpu_argument[i].num_comm_rg = 2;
        }
        DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus){
            DPU_ASSERT(dpu_prepare_xfer(dpu, dpu_argument+i));
                
        }
        DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, "DPU_INPUT_ARGUMENTS_RS1", 0, sizeof(dpu_arguments_comm_t), DPU_XFER_DEFAULT));

        // Run kernel on DPUs
        DPU_ASSERT(dpu_launch(dpu_set, DPU_SYNCHRONOUS));
    }
    else if(!comm_type  || (axis_len[0]<8 && comm_axis[1]==1) || (axis_len[0]*axis_len[1]==4 && (comm_axis[1] == 1 || comm_axis[2] == 1)) ){
        //DPU_ASSERT(dpu_load(dpu_set, getenv("DPU_ALLTOALL_X_2"), NULL));
        DPU_ASSERT(dpu_load(dpu_set, DPU_BINARY_RELOCATE_REVERSE_CLOCKWISE, NULL));

        for(int i=0; i<(int)nr_dpus; i++){
            dpu_argument[i].each_dpu = i;
            dpu_argument[i].start_offset = start_offset + buffer_offset;
            dpu_argument[i].target_offset = target_offset;
            dpu_argument[i].total_data_size = total_data_size;
            dpu_argument[i].num_comm_dpu = num_comm_dpu;
            dpu_argument[i].no_rotate = 0;
            dpu_argument[i].comm_type = comm_type;
            dpu_argument[i].a_length = axis_len[0];
            dpu_argument[i].num_comm_rg = num_comm_rg;
        }
        DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus){
            DPU_ASSERT(dpu_prepare_xfer(dpu, dpu_argument+i));
                
        }
        DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, "DPU_INPUT_ARGUMENTS_RS1", 0, sizeof(dpu_arguments_comm_t), DPU_XFER_DEFAULT));

        // Run kernel on DPUs
        DPU_ASSERT(dpu_launch(dpu_set, DPU_SYNCHRONOUS));
    }
    else{
        //DPU_ASSERT(dpu_load(dpu_set, getenv("DPU_BINARY_RELOCATE_2"), NULL));
        DPU_ASSERT(dpu_load(dpu_set, DPU_BINARY_RELOCATE_CLOCKWISE, NULL));

        for(int i=0; i<(int)nr_dpus; i++){
            dpu_argument[i].each_dpu = i;
            dpu_argument[i].start_offset = start_offset+ buffer_offset;
            dpu_argument[i].target_offset = target_offset;
            dpu_argument[i].total_data_size = total_data_size;
            dpu_argument[i].num_comm_dpu = num_comm_dpu;
            dpu_argument[i].no_rotate = 1;
            dpu_argument[i].comm_type = comm_type;
            dpu_argument[i].a_length = axis_len[0];
        }

        

        DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus){
            DPU_ASSERT(dpu_prepare_xfer(dpu, dpu_argument+i));
                
        }
        DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, "DPU_INPUT_ARGUMENTS_RS1", 0, sizeof(dpu_arguments_comm_t), DPU_XFER_DEFAULT));

        DPU_ASSERT(dpu_launch(dpu_set, DPU_SYNCHRONOUS));
    }
}


__API_SYMBOL__
void pidcomm_allgather(hypercube_manager* manager, char* comm, uint32_t total_data_size, uint32_t start_offset,
                        uint32_t target_offset, uint32_t buffer_offset){

    struct dpu_set_t dpu_set = manager->dpu_set;
    uint32_t dimension = manager->dimension;
    uint32_t* axis_len = manager->axis_len;

    uint32_t* comm_axis = malloc(sizeof(uint32_t) * dimension);

    for(uint32_t dim=0; dim<dimension; dim++){
        comm_axis[dim] = (int)(*(comm+dim))-48;
    }


    struct dpu_set_t dpu;
    uint32_t nr_dpus;
    DPU_ASSERT(dpu_get_nr_dpus(dpu_set, &nr_dpus));
    dpu_arguments_comm_t* dpu_argument = (dpu_arguments_comm_t*) malloc(sizeof(dpu_arguments_comm_t) * nr_dpus);
    uint32_t num_comm_dpu = 1;
    uint32_t comm_type;

    if(comm_axis[0] == 1){
        comm_type = 0;
    }
    else comm_type = 1;

    

    for(uint32_t dim=0; dim<dimension; dim++){
        if(comm_axis[dim]==1){
            num_comm_dpu *= axis_len[dim];
        }
    }

    T** result = (T**) calloc(nr_dpus, sizeof(T*));
    for(int i=0; i<nr_dpus; i++)
        result[i] = (T*) calloc(8/sizeof(T), sizeof(T));
    int i;

    uint32_t num_comm_rg = 1;
    for(uint32_t dim=0, len = 1; dim<dimension, len<8; len*=axis_len[dim], dim++){
        if(comm_axis[dim] == 1){
            if (axis_len[dim] <= (8/len)) num_comm_rg *= axis_len[dim];
            else num_comm_rg *= (8/len);
        }
        if(num_comm_rg >= 8) num_comm_rg = 8;
    }
    //relocate before kernel

    i=0;
    DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus){
        DPU_ASSERT(dpu_prepare_xfer(dpu, result[i]));
    }
    DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_FROM_DPU, DPU_MRAM_HEAP_POINTER_NAME, 0, 8, DPU_XFER_DEFAULT));

    all_gather(&dpu_set, start_offset, start_offset, total_data_size/num_comm_dpu, comm_type, buffer_offset, dimension, axis_len, comm_axis);

    i=0;
    DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus){
        DPU_ASSERT(dpu_prepare_xfer(dpu, result[i]));
    }
    DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_FROM_DPU, DPU_MRAM_HEAP_POINTER_NAME, 0, 8, DPU_XFER_DEFAULT));

    //relocate after kernel
    if(axis_len[0]==2 && axis_len[1]==2 && ((comm_axis[0]==1 && comm_axis[1]==0 && comm_axis[2]==1) || (comm_axis[0]==0 && comm_axis[1]==1 && comm_axis[2]==0))){
        // DPU_ASSERT(dpu_load(dpu_set, getenv("DPU_ALLTOALL_22"), NULL));
        DPU_ASSERT(dpu_load(dpu_set, DPU_BINARY_RELOCATE_MODIFIED_CLOCKWISE, NULL));

        for(int i=0; i<nr_dpus; i++){
            dpu_argument[i].each_dpu = i;
            dpu_argument[i].start_offset = start_offset + buffer_offset;
            dpu_argument[i].target_offset = target_offset;
            dpu_argument[i].total_data_size = total_data_size;
            dpu_argument[i].num_comm_dpu = num_comm_dpu;
            dpu_argument[i].no_rotate = 0;
            dpu_argument[i].comm_type = comm_type;
            dpu_argument[i].a_length = axis_len[0];
            dpu_argument[i].num_comm_rg = 2;
        }
        DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus){
            DPU_ASSERT(dpu_prepare_xfer(dpu, dpu_argument+i));
                
        }
        DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, "DPU_INPUT_ARGUMENTS_RS1", 0, sizeof(dpu_arguments_comm_t), DPU_XFER_DEFAULT));

        // Run kernel on DPUs
        DPU_ASSERT(dpu_launch(dpu_set, DPU_SYNCHRONOUS));
    }
    else if(!comm_type  || (axis_len[0]<8 && comm_axis[1]==1) || (axis_len[0]*axis_len[1]==4 && (comm_axis[1] == 1 || comm_axis[2] == 1))){
        // DPU_ASSERT(dpu_load(dpu_set, getenv("DPU_ALLTOALL_X_2"), NULL));
        DPU_ASSERT(dpu_load(dpu_set, DPU_BINARY_RELOCATE_REVERSE_CLOCKWISE, NULL));

        for(int i=0; i<nr_dpus; i++){
            dpu_argument[i].each_dpu = i;
            dpu_argument[i].start_offset = start_offset + buffer_offset;
            dpu_argument[i].target_offset = target_offset;
            dpu_argument[i].total_data_size = total_data_size;
            dpu_argument[i].num_comm_dpu = num_comm_dpu;
            dpu_argument[i].no_rotate = 0;
            dpu_argument[i].comm_type = comm_type;
            dpu_argument[i].a_length = axis_len[0];
            dpu_argument[i].num_comm_rg = num_comm_rg;
        }
        DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus){
            DPU_ASSERT(dpu_prepare_xfer(dpu, dpu_argument+i));
        }
        DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, "DPU_INPUT_ARGUMENTS_RS1", 0, sizeof(dpu_arguments_comm_t), DPU_XFER_DEFAULT));

        // Run kernel on DPUs
        DPU_ASSERT(dpu_launch(dpu_set, DPU_SYNCHRONOUS));
    }
    else{
        // DPU_ASSERT(dpu_load(dpu_set, getenv("DPU_BINARY_RELOCATE_2"), NULL));
        DPU_ASSERT(dpu_load(dpu_set, DPU_BINARY_RELOCATE_CLOCKWISE, NULL));

        for(int i=0; i<nr_dpus; i++){
            dpu_argument[i].each_dpu = i;
            dpu_argument[i].start_offset = start_offset + buffer_offset;
            dpu_argument[i].target_offset = target_offset;
            dpu_argument[i].total_data_size = total_data_size;
            dpu_argument[i].num_comm_dpu = num_comm_dpu;
            dpu_argument[i].no_rotate = 1;
            dpu_argument[i].comm_type = comm_type;
            dpu_argument[i].a_length = axis_len[0];
        }

        DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus){
            DPU_ASSERT(dpu_prepare_xfer(dpu, dpu_argument+i));
        }
        DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, "DPU_INPUT_ARGUMENTS_RS1", 0, sizeof(dpu_arguments_comm_t), DPU_XFER_DEFAULT));

        DPU_ASSERT(dpu_launch(dpu_set, DPU_SYNCHRONOUS));
    }

    i=0;
    DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus){
        DPU_ASSERT(dpu_prepare_xfer(dpu, result[i]));
    }
    DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_FROM_DPU, DPU_MRAM_HEAP_POINTER_NAME, 0, 8, DPU_XFER_DEFAULT));
}

__API_SYMBOL__
void pidcomm_all_reduce(hypercube_manager* manager, char* comm, uint32_t total_data_size, uint32_t start_offset, uint32_t target_offset, \
                    uint32_t buffer_offset, uint32_t size, uint32_t reduce_type){

    struct dpu_set_t dpu_set = manager->dpu_set;
    uint32_t dimension = manager->dimension;
    uint32_t* axis_len = manager->axis_len;

    uint32_t* comm_axis = malloc(sizeof(uint32_t) * dimension);

    for(uint32_t dim=0; dim<dimension; dim++){
        comm_axis[dim] = (int)(*(comm+dim))-48;
    }

    struct dpu_set_t dpu;
    uint32_t nr_dpus;
    DPU_ASSERT(dpu_get_nr_dpus(dpu_set, &nr_dpus));
    dpu_arguments_comm_t* dpu_argument = (dpu_arguments_comm_t*) malloc(sizeof(dpu_arguments_comm_t) * nr_dpus);
    uint32_t num_comm_dpu = 1;
    uint32_t comm_type;

    if(comm_axis[0] == 1){
        comm_type = 0;
    }
    else comm_type = 1;

    for(uint32_t dim=0; dim<dimension; dim++){
        if(comm_axis[dim]==1){
            num_comm_dpu *= axis_len[dim];
        }
    }

    T** result = (T**) calloc(nr_dpus, sizeof(T*));
    for(int i=0; i<nr_dpus; i++)
        result[i] = (T*) calloc(8/sizeof(T), sizeof(T));
    int i;

    uint32_t num_comm_rg = 1;
    for(uint32_t dim=0, len = 1; dim<dimension, len<8; len*=axis_len[dim], dim++){
        if(comm_axis[dim] == 1){
            if (axis_len[dim] <= (8/len)) num_comm_rg *= axis_len[dim];
            else num_comm_rg *= (8/len);
        }
        if(num_comm_rg >= 8) num_comm_rg = 8;
    }

    //relocate before kernel
    if(axis_len[0]==2 && axis_len[1]==2 && ( ((comm_axis[0]==0) && (comm_axis[1]==1) && (comm_axis[2]==0)) || ((comm_axis[0]==1) && (comm_axis[1]==0) && (comm_axis[2]==1)))){
        if(size==1) DPU_ASSERT(dpu_load(dpu_set, DPU_BINARY_RELOCATE_MODIFIED_REVERSE_CLOCKWISE_INT8, NULL));
        else DPU_ASSERT(dpu_load(dpu_set, DPU_BINARY_RELOCATE_MODIFIED_REVERSE_CLOCKWISE_INT32, NULL));

        for(int i=0; i<nr_dpus; i++){
            dpu_argument[i].each_dpu = i;
            dpu_argument[i].start_offset = start_offset;
            dpu_argument[i].target_offset = start_offset;
            dpu_argument[i].total_data_size = total_data_size;
            dpu_argument[i].num_comm_dpu = num_comm_dpu;
            dpu_argument[i].no_rotate = 0;
            dpu_argument[i].comm_type = comm_type;
            dpu_argument[i].a_length = 2;
            dpu_argument[i].num_comm_rg = 2;
        }

        DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus){
            DPU_ASSERT(dpu_prepare_xfer(dpu, dpu_argument+i));
        }
        DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, "DPU_INPUT_ARGUMENTS_RS1", 0, sizeof(dpu_arguments_comm_t), DPU_XFER_DEFAULT));

        // Run kernel on DPUs
        DPU_ASSERT(dpu_launch(dpu_set, DPU_SYNCHRONOUS));
    }

    else if(axis_len[0] < 8 && ((num_comm_rg < 8) && (num_comm_rg > 1) )){
        // if(size==1) DPU_ASSERT(dpu_load(dpu_set, getenv("DPU_RS_24_INT8"), NULL));
        // else DPU_ASSERT(dpu_load(dpu_set, getenv("DPU_RS_24_INT32"), NULL));
        if(size==1) DPU_ASSERT(dpu_load(dpu_set, DPU_BINARY_RELOCATE_CLOCKWISE_SHORT_INT8, NULL));
        else DPU_ASSERT(dpu_load(dpu_set, DPU_BINARY_RELOCATE_CLOCKWISE_SHORT_INT32, NULL));

        for(int i=0; i<nr_dpus; i++){
            dpu_argument[i].each_dpu = i;
            dpu_argument[i].start_offset = start_offset;
            dpu_argument[i].target_offset = start_offset;
            dpu_argument[i].total_data_size = total_data_size;
            dpu_argument[i].num_comm_dpu = num_comm_dpu;
            dpu_argument[i].no_rotate = 0;
            dpu_argument[i].comm_type = comm_type;
            dpu_argument[i].a_length = axis_len[0];
            dpu_argument[i].num_comm_rg = num_comm_rg;
        }
            
        DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus){
            DPU_ASSERT(dpu_prepare_xfer(dpu, dpu_argument+i));
        }
        DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, "DPU_INPUT_ARGUMENTS_RS1", 0, sizeof(dpu_arguments_comm_t), DPU_XFER_DEFAULT));

        // Run kernel on DPUs
        DPU_ASSERT(dpu_launch(dpu_set, DPU_SYNCHRONOUS));

    }
    else if(!comm_type){

        // if(size==1) DPU_ASSERT(dpu_load(dpu_set, getenv("DPU_BINARY_RELOCATE_2_INT8"), NULL));
        // else DPU_ASSERT(dpu_load(dpu_set, getenv("DPU_BINARY_RELOCATE_2_INT32"), NULL));
        if(size==1) DPU_ASSERT(dpu_load(dpu_set, DPU_BINARY_RELOCATE_CLOCKWISE_INT8, NULL));
        else DPU_ASSERT(dpu_load(dpu_set, DPU_BINARY_RELOCATE_CLOCKWISE_INT32, NULL));
        // if(size==1) DPU_ASSERT(dpu_load(dpu_set, getenv("DPU1_INT8"), NULL));
        // else DPU_ASSERT(dpu_load(dpu_set, getenv("DPU1_INT32"), NULL));

        for(int i=0; i<nr_dpus; i++){
            dpu_argument[i].each_dpu = i;
            dpu_argument[i].start_offset = start_offset;
            dpu_argument[i].target_offset = start_offset;
            dpu_argument[i].total_data_size = total_data_size;
            dpu_argument[i].num_comm_dpu = num_comm_dpu;
            dpu_argument[i].no_rotate = 0;
            dpu_argument[i].comm_type = comm_type;
            dpu_argument[i].a_length = axis_len[0];
            dpu_argument[i].num_comm_rg = num_comm_rg;
        }
            
        DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus){
            DPU_ASSERT(dpu_prepare_xfer(dpu, dpu_argument+i));
        }
        DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, "DPU_INPUT_ARGUMENTS_RS1", 0, sizeof(dpu_arguments_comm_t), DPU_XFER_DEFAULT));

        // Run kernel on DPUs
        DPU_ASSERT(dpu_launch(dpu_set, DPU_SYNCHRONOUS));
    }

    i=0;
    DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus){
        DPU_ASSERT(dpu_prepare_xfer(dpu, result[i]));
    }
    DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_FROM_DPU, DPU_MRAM_HEAP_POINTER_NAME, 0, 8, DPU_XFER_DEFAULT));


    //kernel function of All-Reduce
    all_reduce(&dpu_set, start_offset, start_offset, total_data_size/num_comm_dpu, comm_type, buffer_offset, dimension, axis_len, comm_axis, size, reduce_type);
  

    i=0;
    DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus){
        DPU_ASSERT(dpu_prepare_xfer(dpu, result[i]));
    }
    DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_FROM_DPU, DPU_MRAM_HEAP_POINTER_NAME, 0, 8, DPU_XFER_DEFAULT));


    //relocate before kernel
    if(axis_len[0]==2 && axis_len[1]==2 && ((comm_axis[0]==1 && comm_axis[1]==0 && comm_axis[2]==1) || (comm_axis[0]==0 && comm_axis[1]==1 && comm_axis[2]==0))){
        if(size==1) DPU_ASSERT(dpu_load(dpu_set, DPU_BINARY_RELOCATE_MODIFIED_CLOCKWISE_INT8, NULL));
        else DPU_ASSERT(dpu_load(dpu_set, DPU_BINARY_RELOCATE_MODIFIED_CLOCKWISE_INT32, NULL));

        for(int i=0; i<nr_dpus; i++){
            dpu_argument[i].each_dpu = i;
            dpu_argument[i].start_offset = start_offset + buffer_offset;
            dpu_argument[i].target_offset = target_offset;
            dpu_argument[i].total_data_size = total_data_size;
            dpu_argument[i].num_comm_dpu = num_comm_dpu;
            dpu_argument[i].no_rotate = 0;
            dpu_argument[i].comm_type = comm_type;
            dpu_argument[i].a_length = axis_len[0];
            dpu_argument[i].num_comm_rg = 2;
        }
        DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus){
            DPU_ASSERT(dpu_prepare_xfer(dpu, dpu_argument+i));
                
        }
        DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, "DPU_INPUT_ARGUMENTS_RS1", 0, sizeof(dpu_arguments_comm_t), DPU_XFER_DEFAULT));

        // Run kernel on DPUs
        DPU_ASSERT(dpu_launch(dpu_set, DPU_SYNCHRONOUS));
    }

    else if(axis_len[0] < 8 && ((num_comm_rg < 8) && (num_comm_rg > 1) )){
        if(size==1) DPU_ASSERT(dpu_load(dpu_set, DPU_BINARY_RELOCATE_REVERSE_CLOCKWISE_SHORT_INT8, NULL));
        else DPU_ASSERT(dpu_load(dpu_set, DPU_BINARY_RELOCATE_REVERSE_CLOCKWISE_SHORT_INT32, NULL));

        for(int i=0; i<nr_dpus; i++){
            dpu_argument[i].each_dpu = i;
            dpu_argument[i].start_offset = start_offset+buffer_offset;
            dpu_argument[i].target_offset = target_offset;
            dpu_argument[i].total_data_size = total_data_size;
            dpu_argument[i].num_comm_dpu = num_comm_dpu;
            dpu_argument[i].no_rotate = 0;
            dpu_argument[i].comm_type = comm_type;
            dpu_argument[i].a_length = axis_len[0];
            dpu_argument[i].num_comm_rg = num_comm_rg;
        }
            
        DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus){
            DPU_ASSERT(dpu_prepare_xfer(dpu, dpu_argument+i));
        }
        DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, "DPU_INPUT_ARGUMENTS_RS1", 0, sizeof(dpu_arguments_comm_t), DPU_XFER_DEFAULT));

        // Run kernel on DPUs
        DPU_ASSERT(dpu_launch(dpu_set, DPU_SYNCHRONOUS));

    }
    else if(!comm_type){

        if(size==1) DPU_ASSERT(dpu_load(dpu_set, DPU_BINARY_RELOCATE_COUNTERCLOCKWISE_INT8, NULL));
        else DPU_ASSERT(dpu_load(dpu_set, DPU_BINARY_RELOCATE_COUNTERCLOCKWISE_INT32, NULL));

        for(int i=0; i<nr_dpus; i++){
            dpu_argument[i].each_dpu = i;
            dpu_argument[i].start_offset = start_offset + buffer_offset;
            dpu_argument[i].target_offset = target_offset;
            dpu_argument[i].total_data_size = total_data_size;
            dpu_argument[i].num_comm_dpu = num_comm_dpu;
            dpu_argument[i].no_rotate = 0;
            dpu_argument[i].comm_type = comm_type;
            dpu_argument[i].a_length = axis_len[0];
            dpu_argument[i].num_comm_rg = num_comm_rg;
        }
            
        DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus){
            DPU_ASSERT(dpu_prepare_xfer(dpu, dpu_argument+i));
        }
        DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, "DPU_INPUT_ARGUMENTS_RS1", 0, sizeof(dpu_arguments_comm_t), DPU_XFER_DEFAULT));

        // Run kernel on DPUs
        DPU_ASSERT(dpu_launch(dpu_set, DPU_SYNCHRONOUS));
    }
    else{
        // if(size==1) DPU_ASSERT(dpu_load(dpu_set, getenv("DPU_BINARY_AR_2_Y_INT8"), NULL));
        // else DPU_ASSERT(dpu_load(dpu_set, getenv("DPU_BINARY_AR_2_Y_INT32"), NULL));
        if(size==1) DPU_ASSERT(dpu_load(dpu_set, DPU_BINARY_RELOCATE_INCREMENTAL_COUNTERCLOCKWISE_INT8, NULL));
        else DPU_ASSERT(dpu_load(dpu_set, DPU_BINARY_RELOCATE_INCREMENTAL_COUNTERCLOCKWISE_INT32, NULL));

        for(int i=0; i<nr_dpus; i++){
            dpu_argument[i].each_dpu = i;
            dpu_argument[i].start_offset = start_offset + buffer_offset;
            dpu_argument[i].target_offset = target_offset;
            dpu_argument[i].total_data_size = total_data_size;
            dpu_argument[i].num_comm_dpu = num_comm_dpu;
            dpu_argument[i].no_rotate = 0;
            dpu_argument[i].comm_type = comm_type;
            dpu_argument[i].a_length = axis_len[0];
            dpu_argument[i].num_comm_rg = num_comm_rg;
        }

        DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus){
            DPU_ASSERT(dpu_prepare_xfer(dpu, dpu_argument+i));
        }
        DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, "DPU_INPUT_ARGUMENTS_RS1", 0, sizeof(dpu_arguments_comm_t), DPU_XFER_DEFAULT));

        // Run kernel on DPUs
        DPU_ASSERT(dpu_launch(dpu_set, DPU_SYNCHRONOUS));
    }
    i=0;
    DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus){
        DPU_ASSERT(dpu_prepare_xfer(dpu, result[i]));
    }
    DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_FROM_DPU, DPU_MRAM_HEAP_POINTER_NAME, 0, 8, DPU_XFER_DEFAULT));
}


__API_SYMBOL__
void pidcomm_reduce_scatter(hypercube_manager* manager, char* comm, uint32_t total_data_size, uint32_t start_offset,
                        uint32_t target_offset, uint32_t buffer_offset, uint32_t size){

    struct dpu_set_t dpu_set = manager->dpu_set;
    uint32_t dimension = manager->dimension;
    uint32_t* axis_len = manager->axis_len;

    uint32_t* comm_axis = malloc(sizeof(uint32_t) * dimension);

    for(uint32_t dim=0; dim<dimension; dim++){
        comm_axis[dim] = (int)(*(comm+dim))-48;
    }

    struct dpu_set_t dpu;
    uint32_t nr_dpus;
    DPU_ASSERT(dpu_get_nr_dpus(dpu_set, &nr_dpus));
    dpu_arguments_comm_t* dpu_argument = (dpu_arguments_comm_t*) malloc(sizeof(dpu_arguments_comm_t) * nr_dpus);
    uint32_t num_comm_dpu = 1;
    uint32_t comm_type;

    if(comm_axis[0] == 1){
        comm_type = 0;
    }
    else comm_type = 1;

    for(uint32_t dim=0; dim<dimension; dim++){
        if(comm_axis[dim]==1){
            num_comm_dpu *= axis_len[dim];
        }
    }

    uint32_t num_comm_rg = 1;
    for(uint32_t dim=0, len = 1; dim<dimension, len<8; len*=axis_len[dim], dim++){
        if(comm_axis[dim] == 1){
            if (axis_len[dim] <= (8/len)) num_comm_rg *= axis_len[dim];
            else num_comm_rg *= (8/len);
        }
        if(num_comm_rg >= 8) num_comm_rg = 8;
    }

    T** result = (T**) calloc(nr_dpus, sizeof(T*));
    for(int i=0; i<nr_dpus; i++)
        result[i] = (T*) calloc(8/sizeof(T), sizeof(T));
    int i;

    if(axis_len[0]==2 && axis_len[1]==2 && ( ((comm_axis[0]==0) && (comm_axis[1]==1) && (comm_axis[2]==0)) || ((comm_axis[0]==1) && (comm_axis[1]==0) && (comm_axis[2]==1)))){
        // if(size==1) DPU_ASSERT(dpu_load(dpu_set, getenv("DPU_RS_22_INT8"), NULL));
        // else DPU_ASSERT(dpu_load(dpu_set, getenv("DPU_RS_22_INT32"), NULL));
        if(size==1) DPU_ASSERT(dpu_load(dpu_set, DPU_BINARY_RELOCATE_MODIFIED_REVERSE_CLOCKWISE_INT8, NULL));
        else DPU_ASSERT(dpu_load(dpu_set, DPU_BINARY_RELOCATE_MODIFIED_REVERSE_CLOCKWISE_INT32, NULL));

        for(int i=0; i<nr_dpus; i++){
            dpu_argument[i].each_dpu = i;
            dpu_argument[i].start_offset = start_offset;
            dpu_argument[i].target_offset = start_offset + buffer_offset;
            dpu_argument[i].total_data_size = total_data_size;
            dpu_argument[i].num_comm_dpu = num_comm_dpu;
            dpu_argument[i].no_rotate = 0;
            dpu_argument[i].comm_type = comm_type;
            dpu_argument[i].a_length = 2;
            dpu_argument[i].num_comm_rg = 2;
        }

        DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus){
            DPU_ASSERT(dpu_prepare_xfer(dpu, dpu_argument+i));
        }
        DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, "DPU_INPUT_ARGUMENTS_RS1", 0, sizeof(dpu_arguments_comm_t), DPU_XFER_DEFAULT));

        // Run kernel on DPUs
        DPU_ASSERT(dpu_launch(dpu_set, DPU_SYNCHRONOUS));
    }

    else if(axis_len[0] < 8 && ((num_comm_rg < 8) && (num_comm_rg > 1) )){
        if(size==1) DPU_ASSERT(dpu_load(dpu_set, DPU_BINARY_RELOCATE_CLOCKWISE_SHORT_INT8, NULL));
        else DPU_ASSERT(dpu_load(dpu_set, DPU_BINARY_RELOCATE_CLOCKWISE_SHORT_INT32, NULL));

        for(int i=0; i<nr_dpus; i++){
            dpu_argument[i].each_dpu = i;
            dpu_argument[i].start_offset = start_offset;
            dpu_argument[i].target_offset = start_offset + buffer_offset;
            dpu_argument[i].total_data_size = total_data_size;
            dpu_argument[i].num_comm_dpu = num_comm_dpu;
            dpu_argument[i].no_rotate = 0;
            dpu_argument[i].comm_type = comm_type;
            dpu_argument[i].a_length = axis_len[0];
            dpu_argument[i].num_comm_rg = num_comm_rg;
        }
            
        DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus){
            DPU_ASSERT(dpu_prepare_xfer(dpu, dpu_argument+i));
        }
        DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, "DPU_INPUT_ARGUMENTS_RS1", 0, sizeof(dpu_arguments_comm_t), DPU_XFER_DEFAULT));

        // Run kernel on DPUs
        DPU_ASSERT(dpu_launch(dpu_set, DPU_SYNCHRONOUS));
    }
    //relocate before kernel
    else if(!comm_type){

        // if(size==1) DPU_ASSERT(dpu_load(dpu_set, getenv("DPU_BINARY_RELOCATE_2_INT8"), NULL));
        // else DPU_ASSERT(dpu_load(dpu_set, getenv("DPU_BINARY_RELOCATE_2_INT32"), NULL));
        if(size==1) DPU_ASSERT(dpu_load(dpu_set, DPU_BINARY_RELOCATE_CLOCKWISE_INT8, NULL));
        else DPU_ASSERT(dpu_load(dpu_set, DPU_BINARY_RELOCATE_CLOCKWISE_INT32, NULL));

        for(int i=0; i<nr_dpus; i++){
            dpu_argument[i].each_dpu = i;
            dpu_argument[i].start_offset = start_offset;
            dpu_argument[i].target_offset = start_offset + buffer_offset;
            dpu_argument[i].total_data_size = total_data_size;
            dpu_argument[i].num_comm_dpu = num_comm_dpu;
            dpu_argument[i].no_rotate = 0;
            dpu_argument[i].comm_type = comm_type;
            dpu_argument[i].a_length = axis_len[0];
            dpu_argument[i].num_comm_rg = num_comm_rg;
        }
            
        DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus){
            DPU_ASSERT(dpu_prepare_xfer(dpu, dpu_argument+i));
        }
        DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, "DPU_INPUT_ARGUMENTS_RS1", 0, sizeof(dpu_arguments_comm_t), DPU_XFER_DEFAULT));

        // Run kernel on DPUs
        DPU_ASSERT(dpu_launch(dpu_set, DPU_SYNCHRONOUS));
    }
    else{
        // if(size==1) DPU_ASSERT(dpu_load(dpu_set, getenv("DPU_BINARY_RELOCATE_2_INT8"), NULL));
        // else DPU_ASSERT(dpu_load(dpu_set, getenv("DPU_BINARY_RELOCATE_2_INT32"), NULL));
        if(size==1) DPU_ASSERT(dpu_load(dpu_set, DPU_BINARY_RELOCATE_CLOCKWISE_INT8, NULL));
        else DPU_ASSERT(dpu_load(dpu_set, DPU_BINARY_RELOCATE_CLOCKWISE_INT32, NULL));

        for(int i=0; i<nr_dpus; i++){
            dpu_argument[i].start_offset = start_offset;
            dpu_argument[i].target_offset = start_offset + buffer_offset;
            dpu_argument[i].total_data_size = total_data_size;
            dpu_argument[i].num_comm_dpu = num_comm_dpu;
            dpu_argument[i].no_rotate = 1;
            dpu_argument[i].comm_type = comm_type;
            dpu_argument[i].a_length = axis_len[0];
        }

        DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus){
            dpu_argument[i].each_dpu = i;
            DPU_ASSERT(dpu_prepare_xfer(dpu, dpu_argument+i));
        }
        DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, "DPU_INPUT_ARGUMENTS_RS1", 0, sizeof(dpu_arguments_comm_t), DPU_XFER_DEFAULT));

        // Run kernel on DPUs
        DPU_ASSERT(dpu_launch(dpu_set, DPU_SYNCHRONOUS));
    }

    i=0;
    DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus){
        DPU_ASSERT(dpu_prepare_xfer(dpu, result[i]));
    }
    DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_FROM_DPU, DPU_MRAM_HEAP_POINTER_NAME, start_offset + buffer_offset, 8, DPU_XFER_DEFAULT));

    reduce_scatter(&dpu_set, start_offset, target_offset, total_data_size/num_comm_dpu, comm_type, buffer_offset, dimension, axis_len, comm_axis, size);
    

    i=0;
    DPU_FOREACH_ENTANGLED_GROUP(dpu_set, dpu, i, nr_dpus){
        DPU_ASSERT(dpu_prepare_xfer(dpu, result[i]));
    }
    DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_FROM_DPU, DPU_MRAM_HEAP_POINTER_NAME, 0, 8, DPU_XFER_DEFAULT));

}