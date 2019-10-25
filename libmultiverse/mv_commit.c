#include "mv_assert.h"
#include "mv_string.h"
#include "multiverse.h"
#include "mv_commit.h"
#include "arch.h"
#include "platform.h"

// wait-free patching needs realloc
#include <stdlib.h>
#include "patch.h"

#define _GNU_SOURCE
#include <unistd.h>
#include <sys/syscall.h>

/* TODO encapsulate all this stuff in mv_info */
extern struct mv_info_fn *__start___multiverse_fn_ptr;
extern struct mv_info_fn *__stop___multiverse_fn_ptr;


static mv_value_t multiverse_var_read(struct mv_info_var *var) {
    if (var->variable_width == sizeof(unsigned char)) {
        return *(unsigned char *)var->variable_location;
    } else if (var->variable_width == sizeof(unsigned short)) {
        return *(unsigned short *)var->variable_location;
    } else if (var->variable_width == sizeof(unsigned int)) {
        return *(unsigned int *)var->variable_location;
    }
    MV_ASSERT(0 && "Invalid width of multiverse variable. This should not happen");
    return 0;
}

typedef struct {
    unsigned int cache_size;
    void        *unprotected[10];
;
} mv_transaction_ctx_t;

static mv_transaction_ctx_t mv_transaction_start(void) {
    return (mv_transaction_ctx_t){ .cache_size = 10 };
}

static void mv_transaction_end(mv_transaction_ctx_t *ctx) {
    unsigned i = 0;
    for (i = 0; i < ctx->cache_size; i++) {
        if (ctx->unprotected[i] != NULL) {
            multiverse_os_protect(ctx->unprotected[i]);
        }
    }
    multiverse_os_clear_caches();
}



static void multiverse_transaction_unprotect(mv_transaction_ctx_t *ctx, void *addr) {
    void *page = multiverse_os_addr_to_page(addr);
    // The unprotected_pages implements a LRU cache, where element 0 is
    // the hottest one.
    unsigned i;
    for (i = 0; i < ctx->cache_size; i++) {
        if (ctx->unprotected[i] == page) {
            // If we have a cache hit, we sort the current element to
            // the front. Therefore we push all elements before the
            // hitting one, one element back.
            unsigned j;
            for (j = i; j > 0; j--) {
                ctx->unprotected[j] = ctx->unprotected[j-1];
            }
            ctx->unprotected[0] = page;
            return;
        }
    }

    multiverse_os_unprotect(page);

    // Insert into the cache.
    if (ctx->unprotected[ctx->cache_size - 1] != NULL) {
        // If the cache is full, we push out the coldest element from the cache
        multiverse_os_protect(ctx->unprotected[ctx->cache_size - 1]);
    }
    // Push everything one elment back.
    memmove(&(ctx->unprotected[1]), &(ctx->unprotected[0]),
            ctx->cache_size - 1);
    ctx->unprotected[0] = page;

}


static int
multiverse_select_mvfn(mv_transaction_ctx_t *ctx,
                       struct mv_info_fn *fn,
                       struct mv_info_mvfn *mvfn) {
    struct mv_patchpoint *pp;

    if (mvfn == fn->active_mvfn) return 0;

    // array of patches
    void *patches = NULL;
    size_t patches_size = 0;

    for (pp = fn->patchpoints_head; pp != NULL; pp = pp->next) {
        void *from, *to;
        unsigned char *location = pp->location;

        // TODO: arch function is_patchpoint_valid??
        if (pp->type == PP_TYPE_INVALID) continue;
        if (!location) continue; // TODO: when does this happen??

        multiverse_arch_patchpoint_size(pp, &from, &to);

        // This is not needed for wait-free patching
        // (the whole transaction_ctx thing)

        size_t data_len = to - from;
        size_t patch_len = sizeof(struct patch) + data_len;
        size_t curr_offset = patches_size;
        patches_size += patch_len;
        patches = realloc(patches, patches_size);
        struct patch *curr = (struct patch*)(patches + curr_offset);
        curr->pos = pp->location;
        curr->len = data_len;

        if (mvfn == NULL) {
            multiverse_arch_patchpoint_revert(pp, curr->data);
        } else {
            multiverse_arch_patchpoint_apply(fn, mvfn, pp, curr->data);
        }
    }

    // print patch array
    for (struct patch *curr = (struct patch*)patches;
         (void*)curr < patches + patches_size;
         curr = (void*)curr + sizeof(struct patch) + curr->len) {
        multiverse_os_print("PATCH: %p, len: %d\n", curr->pos, curr->len);
    }
    // TODO: invoke syscall
    syscall(1000, patches, patches_size);

    fn->active_mvfn = mvfn;

    return 1; // We changed this function
}

static int __multiverse_commit_fn(mv_transaction_ctx_t *ctx, struct mv_info_fn *fn) {
    int ret;
    if (fn->n_mv_functions != -1) {
        // A normal multiverse function
        struct mv_info_mvfn *best_mvfn = NULL;
        int f;
        for (f = 0; f < fn->n_mv_functions; f++) {
            struct mv_info_mvfn * mvfn = &fn->mv_functions[f];
            unsigned good = 1;
            unsigned a;
            for (a = 0; a < mvfn->n_assignments; a++) {
                struct mv_info_assignment * assign = &mvfn->assignments[a];
                // If the assignment of this mvfn depends on an unbound
                // variable. The mvfn is unsuitable currently.
                if (!assign->variable.info->flag_bound) {
                    good = 0;
                } else {
                    // Variable is bound
                    mv_value_t cur = multiverse_var_read(assign->variable.info);
                    if (cur > assign->upper_bound || cur < assign->lower_bound)
                        good = 0;
                }
            }
            if (good) {
                // Here we possibly override an already valid mvfn
                best_mvfn = mvfn;
            }
        }
        ret = multiverse_select_mvfn(ctx, fn, best_mvfn);
    } else {
        // A multiversed function pointer
        // Set the "artificial" mvfn according to the function pointer

        void *old_body = (fn->active_mvfn) ? fn->active_mvfn->function_body : NULL;
        void *new_body = *((void**)fn->function_body);

        if (fn->mv_functions == NULL) {
            fn->mv_functions = multiverse_os_malloc(sizeof(struct mv_info_mvfn));
            fn->mv_functions->n_assignments = 0;
            fn->mv_functions->assignments = NULL;
        }
        fn->mv_functions->function_body = new_body;

        // TODO: This is quite hacky and could be done much nicer.
        //       This would require changing multiverse_select_mvfn to detect
        //       the mvfn changing also for function_pointers.
        if (fn->active_mvfn == NULL || old_body != new_body) {
            // Reset active_mvfn if necessary so that multiverse_select_mvfn does its magic
            fn->active_mvfn = NULL;
            // For normal mvfns decoding the body is done in multiverse_init
            // Function pointers use a single mvfn that is used for the currently
            // assigned function
            multiverse_arch_decode_mvfn_body(fn->mv_functions);
        }
        ret = multiverse_select_mvfn(ctx, fn, fn->mv_functions);
    }
    return ret;
}

int multiverse_commit_info_fn(struct mv_info_fn *fn) {
    mv_transaction_ctx_t ctx = mv_transaction_start();
    int ret = __multiverse_commit_fn(&ctx, fn);
    mv_transaction_end(&ctx);

    return ret;
}


int multiverse_commit_fn(void *function_body) {
    struct mv_info_fn *fn = multiverse_info_fn(function_body);
    if (!fn) return -1;

    return multiverse_commit_info_fn(fn);
}

int multiverse_commit_info_refs(struct mv_info_var *var) {
    int ret = 0;
    struct mv_info_fn_ref *fref;
    mv_transaction_ctx_t ctx = mv_transaction_start();

    for (fref = var->functions_head; fref != NULL; fref = fref->next) {
        int r = __multiverse_commit_fn(&ctx, fref->fn);
        if (r < 0) {
            ret = -1;
            break;
        }
        ret += r;
    }

    mv_transaction_end(&ctx);

    return ret;
}

int multiverse_commit_refs(void *variable_location) {
    struct mv_info_var *var = multiverse_info_var(variable_location);
    if (!var) return -1;

    return multiverse_commit_info_refs(var);
}

int multiverse_commit() {
    int ret = 0;
    mv_transaction_ctx_t ctx = mv_transaction_start();
    struct mv_info_fn *fn;

    for (fn = __start___multiverse_fn_ptr; fn < __stop___multiverse_fn_ptr; fn++) {
        int r = __multiverse_commit_fn(&ctx, fn);
        if (r < 0) {
            ret = -1;
            break; // FIXME: get a valid state after this
        }
        ret += r;
    }

    mv_transaction_end(&ctx);

    return ret;
}

int multiverse_revert_info_fn(struct mv_info_fn *fn) {
    mv_transaction_ctx_t ctx = mv_transaction_start();
    int ret;

    ret = multiverse_select_mvfn(&ctx,  fn, NULL);

    mv_transaction_end(&ctx);
    return ret;
}


int multiverse_revert_fn(void *function_body) {
    struct mv_info_fn *fn = multiverse_info_fn(function_body);
    if (!fn) return -1;

    return multiverse_revert_info_fn(fn);
}

int multiverse_revert_info_refs(struct mv_info_var *var) {
    int ret = 0;
    struct mv_info_fn_ref *fref;
    mv_transaction_ctx_t ctx = mv_transaction_start();

    for (fref = var->functions_head; fref != NULL; fref = fref->next) {
        int r = multiverse_select_mvfn(&ctx, fref->fn, NULL);
        if (r < 0) {
            ret = -1;
            break;
        }
        ret += r;
    }

    mv_transaction_end(&ctx);

    return ret;
}

int multiverse_revert_refs(void *variable_location) {
    struct mv_info_var *var = multiverse_info_var(variable_location);
    if (!var) return -1;
    return multiverse_revert_info_refs(var);
}


int multiverse_revert() {
    int ret = 0;
    mv_transaction_ctx_t ctx = mv_transaction_start();
    struct mv_info_fn *fn;

    for (fn = __start___multiverse_fn_ptr; fn < __stop___multiverse_fn_ptr; fn++) {
        int r = multiverse_select_mvfn(&ctx, fn, NULL);
        if (r < 0) {
            r = -1;
            break;
        }
        ret += r;
    }

    mv_transaction_end(&ctx);

    return ret;
}

int multiverse_is_committed(void *function_body) {
    struct mv_info_fn *fn = multiverse_info_fn(function_body);
    return fn->active_mvfn != NULL;
}

int multiverse_bind(void *var_location, int state) {
    struct mv_info_var *var = multiverse_info_var(var_location);
    if (!var) return -1;

    if (state >= 0) {
        if (!var->flag_tracked) return -1;
        var->flag_bound = (state != 0);
    }
    return var->flag_bound;
}
