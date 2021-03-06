// -*- mode: c; tab-width: 4; indent-tabs-mode: nil; st-rulers: [132] -*-
// vim: ts=4 sw=4 ft=c et

#ifndef H2O_NIF_SLICE_H
#define H2O_NIF_SLICE_H

#include "globals.h"

/* Types */

typedef struct h2o_nif_slice_s h2o_nif_slice_t;
typedef size_t h2o_nif_slice_map_t(ErlNifEnv *env, h2o_nif_slice_t *slice, size_t offset, size_t length);
typedef ERL_NIF_TERM h2o_nif_slice_reduce_t(ErlNifEnv *env, h2o_nif_slice_t *slice);

struct h2o_nif_slice_s {
    const char fun_name[256];
    size_t max_per_slice;
    int badarg;
    int flags;
    h2o_mem_pool_t pool;
    struct {
        size_t length;
        size_t offset;
        ErlNifBinary binary;
    } in;
    struct {
        size_t length;
        size_t offset;
        ErlNifBinary binary;
    } out;
    h2o_nif_slice_map_t *map;
    h2o_nif_slice_reduce_t *reduce;
    // void *data;
};

typedef struct h2o_nif_slicelist_s h2o_nif_slicelist_t;
typedef ERL_NIF_TERM h2o_nif_slicelist_reduce_t(ErlNifEnv *env, h2o_nif_slicelist_t *slicelist, ERL_NIF_TERM list,
                                                h2o_linklist_t *node);

struct h2o_nif_slicelist_s {
    const char fun_name[256];
    size_t max_per_slice;
    size_t length;
    size_t offset;
    size_t count;
    int badarg;
    h2o_linklist_t list;
    h2o_linklist_t *node;
    h2o_nif_slicelist_reduce_t *reduce;
    void *data;
};

/* Variables */

extern ErlNifResourceType *h2o_nif_slice_resource_type;
extern ErlNifResourceType *h2o_nif_slicelist_resource_type;
extern ErlNifResourceType *h2o_nif_trap_resource_type;

/* NIF Functions */

extern int h2o_nif_slice_load(ErlNifEnv *env, h2o_nif_data_t *nif_data);
extern int h2o_nif_slice_upgrade(ErlNifEnv *env, void **priv_data, void **old_priv_data, ERL_NIF_TERM load_info);
extern void h2o_nif_slice_unload(ErlNifEnv *env, h2o_nif_data_t *nif_data);

/* Slice Functions */

extern int __h2o_nif_slice_create(size_t size, const char *fun_name, h2o_nif_slice_map_t *map, h2o_nif_slice_reduce_t *reduce,
                                  h2o_nif_slice_t **slicep);
extern void h2o_nif_slice_dtor(ErlNifEnv *env, void *obj);
extern ERL_NIF_TERM h2o_nif_slice_schedule(ErlNifEnv *env, h2o_nif_slice_t *slice);
static int h2o_nif_slice_create(const char *fun_name, h2o_nif_slice_map_t *map, h2o_nif_slice_reduce_t *reduce,
                                h2o_nif_slice_t **slicep);
static void h2o_nif_slice_release(h2o_nif_slice_t *slice);

inline int
h2o_nif_slice_create(const char *fun_name, h2o_nif_slice_map_t *map, h2o_nif_slice_reduce_t *reduce, h2o_nif_slice_t **slicep)
{
    return __h2o_nif_slice_create(sizeof(h2o_nif_slice_t), fun_name, map, reduce, slicep);
}

inline void
h2o_nif_slice_release(h2o_nif_slice_t *slice)
{
    (void)enif_release_resource((void *)slice);
}

/* Slicelist Functions */

extern int __h2o_nif_slicelist_create(size_t size, const char *fun_name, h2o_nif_slicelist_reduce_t *reduce,
                                      h2o_nif_slicelist_t **slicelistp);
extern void h2o_nif_slicelist_dtor(ErlNifEnv *env, void *obj);
extern ERL_NIF_TERM h2o_nif_slicelist_schedule(ErlNifEnv *env, h2o_nif_slicelist_t *slicelist);
static int h2o_nif_slicelist_create(const char *fun_name, h2o_nif_slicelist_reduce_t *reduce, h2o_nif_slicelist_t **slicelistp);
static void h2o_nif_slicelist_release(h2o_nif_slicelist_t *slicelist);

inline int
h2o_nif_slicelist_create(const char *fun_name, h2o_nif_slicelist_reduce_t *reduce, h2o_nif_slicelist_t **slicelistp)
{
    return __h2o_nif_slicelist_create(sizeof(h2o_nif_slicelist_t), fun_name, reduce, slicelistp);
}

inline void
h2o_nif_slicelist_release(h2o_nif_slicelist_t *slicelist)
{
    (void)enif_release_resource((void *)slicelist);
}

#endif
