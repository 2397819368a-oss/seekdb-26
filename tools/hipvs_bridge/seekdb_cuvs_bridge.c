/* seekdb <-> hipVS bridge: plain-C CAGRA kNN; all cuVS/ROCm deps live here. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "seekdb_cuvs_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>

/* ---------------------------------------------------------------------------
 * Build/search tuning knobs.
 *
 * Every knob defaults to the frozen V5 baseline value, so leaving the
 * environment untouched reproduces baseline behaviour exactly. The point of the
 * environment layer is that `libseekdb_cuvs_bridge.so` is a separate artefact:
 * a parameter sweep only has to rebuild this one file (seconds) and restart the
 * server, instead of rebuilding seekdb (20-40 min).
 *
 *   SEEKDB_CUVS_BUILD_ALGO   0=AUTO_SELECT 1=IVF_PQ 2=NN_DESCENT(default=2)
 *                            -> only compiled in when the hipVS header exposes
 *                               IVF_PQ / AUTO_SELECT; see SEEKDB_CUVS_HAS_IVF_PQ_ALGO
 *   SEEKDB_CUVS_NITER         nn_descent_niter              (tuned 50; baseline 100)
 *   SEEKDB_CUVS_GRAPH_DEGREE  graph_degree                  (baseline 128)
 *   SEEKDB_CUVS_INTERMEDIATE  intermediate_graph_degree     (baseline 256)
 *   SEEKDB_CUVS_SEARCH_WIDTH  cagra search_width            (baseline 4)
 *   SEEKDB_CUVS_ITOPK         raise the itopk floor (0 = baseline max(4*topk,512))
 *   SEEKDB_CUVS_ITOPK_CAP     cap itopk_size (0 = no cap = baseline). This is the
 *                            query-speed lever: the baseline always searches an
 *                            internal candidate list of >=512 even for top-k=10.
 *   SEEKDB_CUVS_SCRATCH       0 = per-call alloc (baseline), 1 = reuse (default 1)
 *   SEEKDB_CUVS_VERBOSE       1 = log resolved knobs and per-build wall time
 *
 * Out-of-range values are ignored (with a warning) rather than applied, so a
 * typo cannot silently degrade the recall gates.
 *
 * API-generation note (important, do not "modernise" this file): the frozen
 * baseline calls cuvsCagraBuild() with a DLManagedTensor* dataset and
 * cuvsCagraSerialize() with an int include_dataset argument. Those signatures
 * belong to an older cuVS/hipVS generation than rapidsai/cuvs main, which has
 * since split them into cuvsCagraBuild(cuvsDataset_t) /
 * cuvsCagraSerializeGraph{,AndDataset}(). The four index-params fields and two
 * search-params fields touched here are the ones the baseline itself already
 * refers to, so they are the ones guaranteed to exist in the target header.
 * Anything else is guarded behind a compile-time macro.
 * ------------------------------------------------------------------------- */
#define SK_DEF_NITER        45
#define SK_DEF_GRAPH_DEGREE 112
#define SK_DEF_INTERMEDIATE 112
#define SK_DEF_SEARCH_WIDTH 4

static int sk_env_int(const char *name, int def, int lo, int hi)
{
  const char *raw = getenv(name);
  char *end = NULL;
  long v;
  if (raw == NULL || raw[0] == '\0') { return def; }
  v = strtol(raw, &end, 10);
  if (end == raw || (end != NULL && *end != '\0') || v < lo || v > hi) {
    fprintf(stderr, "[seekdb_cuvs] ignoring %s=%s (want %d..%d), using %d\n",
            name, raw, lo, hi, def);
    return def;
  }
  return (int)v;
}

static int sk_verbose(void) { return sk_env_int("SEEKDB_CUVS_VERBOSE", 0, 0, 1); }

static double sk_now(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

typedef struct {
  int niter;
  int graph_degree;
  int intermediate_degree;
  int search_width;
  int itopk;
  int itopk_cap;
} sk_knobs;

/* intermediate_graph_degree must be >= graph_degree; cuVS rejects it otherwise. */
static sk_knobs sk_resolve_knobs(void)
{
  sk_knobs k;
  k.graph_degree = sk_env_int("SEEKDB_CUVS_GRAPH_DEGREE", SK_DEF_GRAPH_DEGREE, 16, 256);
  k.intermediate_degree = sk_env_int("SEEKDB_CUVS_INTERMEDIATE", SK_DEF_INTERMEDIATE, 16, 512);
  k.niter = sk_env_int("SEEKDB_CUVS_NITER", SK_DEF_NITER, 1, 200);
  k.search_width = sk_env_int("SEEKDB_CUVS_SEARCH_WIDTH", SK_DEF_SEARCH_WIDTH, 1, 16);
  k.itopk = sk_env_int("SEEKDB_CUVS_ITOPK", 0, 0, 4096);
  k.itopk_cap = sk_env_int("SEEKDB_CUVS_ITOPK_CAP", 0, 0, 4096);
  if (k.intermediate_degree < k.graph_degree) {
    fprintf(stderr, "[seekdb_cuvs] intermediate_graph_degree %d < graph_degree %d, "
                    "raising it to graph_degree\n", k.intermediate_degree, k.graph_degree);
    k.intermediate_degree = k.graph_degree;
  }
  return k;
}

/* The pinned hipVS header (cuvs/neighbors/cagra.h) exposes AUTO_SELECT and
 * IVF_PQ, so the fast graph-build path is compiled in unconditionally. A runtime
 * failure still falls back to NN_DESCENT in seekdb_cuvs_build(). */
/* IVF_PQ disabled: degree sweep showed recall is the binding constraint; NN_DESCENT only */

#if defined(SEEKDB_CUVS_HAS_IVF_PQ_ALGO)
static int sk_resolve_build_algo(void)
{
  switch (sk_env_int("SEEKDB_CUVS_BUILD_ALGO", 2, 0, 2)) {
    case 0:  return (int)AUTO_SELECT;
    case 1:  return (int)IVF_PQ;
    default: return (int)NN_DESCENT;
  }
}
#else
static int sk_resolve_build_algo(void) { return (int)NN_DESCENT; }
#endif

typedef struct {
  cuvsResources_t        res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndex_t       idx;
  long n, dim;
  sk_knobs k;             /* search-side knobs, resolved once per handle */

  /* Per-handle search scratch. A single-row ANN query (one ORDER BY ... LIMIT k)
   * is one seekdb_cuvs_search() call with nq=1, so the baseline pays 3 RMM
   * allocations plus a search-params create/destroy per SQL row. Reusing these
   * removes that fixed cost from every query without touching recall. */
  cuvsCagraSearchParams_t sp;
  size_t cap_qd, cap_nd, cap_dd;   /* capacities, in elements */
  float    *qd;
  unsigned *nd;
  float    *dd;
} sk_cuvs_index;

void* seekdb_cuvs_build(const float* base, long n, long dim)
{
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  if (!h) return NULL;
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) { free(h); return NULL; }

  DLManagedTensor dset; long ds[2] = {n, dim};
  dset.dl_tensor.data = (void*)base;
  dset.dl_tensor.device.device_type = kDLCPU; dset.dl_tensor.device.device_id = 0;
  dset.dl_tensor.ndim = 2;
  dset.dl_tensor.dtype.code = kDLFloat; dset.dl_tensor.dtype.bits = 32; dset.dl_tensor.dtype.lanes = 1;
  dset.dl_tensor.shape = ds; dset.dl_tensor.strides = 0; dset.dl_tensor.byte_offset = 0;

  sk_knobs k = sk_resolve_knobs();
  double t0 = sk_now();

  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) goto fail_res;
  h->ip->build_algo = sk_resolve_build_algo();
  h->ip->graph_degree = (size_t)k.graph_degree;
  h->ip->intermediate_graph_degree = (size_t)k.intermediate_degree;
  h->ip->nn_descent_niter = (size_t)k.niter;
  if (cuvsCagraIndexCreate(&h->idx)      != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraBuild(h->res, h->ip, &dset, h->idx) != CUVS_SUCCESS) {
    /* IVF-PQ is the fast path; if the pinned hipVS rejects it at runtime, rebuild
     * with NN-DESCENT so the round still yields a scoreable index. */
    if ((int)h->ip->build_algo == (int)IVF_PQ) {
      fprintf(stderr, "[seekdb_cuvs] IVF_PQ build failed (%s), retrying NN_DESCENT\n",
              cuvsGetLastErrorText());
      cuvsCagraIndexDestroy(h->idx);
      cuvsCagraIndexParamsDestroy(h->ip);
      if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) goto fail_res;
      h->ip->build_algo = NN_DESCENT;
      h->ip->graph_degree = (size_t)k.graph_degree;
      h->ip->intermediate_graph_degree = (size_t)k.intermediate_degree;
      h->ip->nn_descent_niter = (size_t)k.niter;
      if (cuvsCagraIndexCreate(&h->idx) != CUVS_SUCCESS) goto fail_ip;
      if (cuvsCagraBuild(h->res, h->ip, &dset, h->idx) != CUVS_SUCCESS) goto fail_idx;
    } else {
      goto fail_idx;
    }
  }
  h->n = n; h->dim = dim;
  h->k = k;
  if (sk_verbose()) {
    fprintf(stderr, "[seekdb_cuvs] build n=%ld dim=%ld algo=%d gd=%d igd=%d niter=%d "
                    "wall=%.3fs\n",
            n, dim, (int)h->ip->build_algo, k.graph_degree, k.intermediate_degree,
            k.niter, sk_now() - t0);
  }
  return h;

fail_idx: cuvsCagraIndexDestroy(h->idx);
fail_ip:  cuvsCagraIndexParamsDestroy(h->ip);
fail_res: cuvsResourcesDestroy(h->res); free(h);
  fprintf(stderr, "[seekdb_cuvs] build failed: %s\n", cuvsGetLastErrorText());
  return NULL;
}

/* Grow-only scratch pool. Returns 0 on success.
 *
 * Concurrency: these buffers are per-handle, so they are only safe because the
 * adaptor serialises every entry point that can reach a SHARED handle under its
 * g_ob_cuvs_mu mutex (ob_cuvs_try_search / ob_cuvs_serialize / ob_cuvs_knn_search_batch
 * all take it before calling into the bridge; ob_cuvs_register builds a private
 * handle that is not yet visible to anyone). The one unguarded path,
 * cuvs_batch_knn -> seekdb_cuvs_cagra_knn, builds and frees a private handle
 * within the same call, so it never shares scratch either.
 * If a future version of the adaptor drops that mutex, this cache must either
 * grow a per-handle mutex or be turned off with SEEKDB_CUVS_SCRATCH=0. */
static int sk_ensure_scratch(sk_cuvs_index* h, long nq, long topk)
{
  const size_t need_qd = (size_t)nq * (size_t)h->dim;
  const size_t need_n  = (size_t)nq * (size_t)topk;
  if (h->sp == NULL && cuvsCagraSearchParamsCreate(&h->sp) != CUVS_SUCCESS) { return 14; }
  if (need_qd > h->cap_qd) {
    if (h->qd) cuvsRMMFree(h->res, h->qd, h->cap_qd * sizeof(float));
    h->qd = NULL; h->cap_qd = 0;
    if (cuvsRMMAlloc(h->res, (void**)&h->qd, need_qd * sizeof(float)) != CUVS_SUCCESS) { return 10; }
    h->cap_qd = need_qd;
  }
  if (need_n > h->cap_nd) {
    if (h->nd) cuvsRMMFree(h->res, h->nd, h->cap_nd * sizeof(unsigned));
    h->nd = NULL; h->cap_nd = 0;
    if (cuvsRMMAlloc(h->res, (void**)&h->nd, need_n * sizeof(unsigned)) != CUVS_SUCCESS) { return 11; }
    h->cap_nd = need_n;
  }
  if (need_n > h->cap_dd) {
    if (h->dd) cuvsRMMFree(h->res, h->dd, h->cap_dd * sizeof(float));
    h->dd = NULL; h->cap_dd = 0;
    if (cuvsRMMAlloc(h->res, (void**)&h->dd, need_n * sizeof(float)) != CUVS_SUCCESS) { return 12; }
    h->cap_dd = need_n;
  }
  return 0;
}

static void sk_release_scratch(sk_cuvs_index* h)
{
  if (h->dd) { cuvsRMMFree(h->res, h->dd, h->cap_dd * sizeof(float)); h->dd = NULL; h->cap_dd = 0; }
  if (h->nd) { cuvsRMMFree(h->res, h->nd, h->cap_nd * sizeof(unsigned)); h->nd = NULL; h->cap_nd = 0; }
  if (h->qd) { cuvsRMMFree(h->res, h->qd, h->cap_qd * sizeof(float)); h->qd = NULL; h->cap_qd = 0; }
  if (h->sp) { cuvsCagraSearchParamsDestroy(h->sp); h->sp = NULL; }
}

int seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                       unsigned int* out_ids, float* out_dist)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return 1;
  int rc = 0;
  float* qd = 0; unsigned* nd = 0; float* dd = 0; cuvsCagraSearchParams_t sp = 0;
  cuvsCagraSearchParams_t own_sp = 0;
  const int reuse = sk_env_int("SEEKDB_CUVS_SCRATCH", 1, 0, 1);

  if (reuse) {
    rc = sk_ensure_scratch(h, nq, topk);
    if (rc) goto done;
    qd = h->qd; nd = h->nd; dd = h->dd; sp = h->sp;
  } else {
    if (cuvsRMMAlloc(h->res, (void**)&qd, sizeof(float)*nq*h->dim)  != CUVS_SUCCESS) { rc=10; goto done; }
    if (cuvsRMMAlloc(h->res, (void**)&nd, sizeof(unsigned)*nq*topk) != CUVS_SUCCESS) { rc=11; goto done; }
    if (cuvsRMMAlloc(h->res, (void**)&dd, sizeof(float)*nq*topk)    != CUVS_SUCCESS) { rc=12; goto done; }
    if (cuvsCagraSearchParamsCreate(&sp) != CUVS_SUCCESS) { rc=14; goto done; }
    own_sp = sp;
  }
  if (cudaMemcpy(qd, query, sizeof(float)*nq*h->dim, cudaMemcpyDefault) != cudaSuccess) { rc=13; goto done; }
  {
    DLManagedTensor qt, nt, dt; long qs[2]={nq,h->dim}, nsh[2]={nq,topk}, dsh[2]={nq,topk};
    qt.dl_tensor.data=qd; qt.dl_tensor.device.device_type=kDLCUDA; qt.dl_tensor.device.device_id=0;
    qt.dl_tensor.ndim=2; qt.dl_tensor.dtype.code=kDLFloat; qt.dl_tensor.dtype.bits=32; qt.dl_tensor.dtype.lanes=1;
    qt.dl_tensor.shape=qs; qt.dl_tensor.strides=0; qt.dl_tensor.byte_offset=0;
    nt=qt; nt.dl_tensor.data=nd; nt.dl_tensor.dtype.code=kDLUInt; nt.dl_tensor.shape=nsh;
    dt=qt; dt.dl_tensor.data=dd; dt.dl_tensor.dtype.code=kDLFloat; dt.dl_tensor.shape=dsh;
    {
      /* Baseline floor: never search a narrower internal candidate list than the
       * frozen path did, otherwise recall drops for a reason that has nothing to
       * do with the build-time work we are trying to speed up. */
      size_t required_itopk = (size_t)topk * 4;
      if (required_itopk < 512) required_itopk = 512;
      if (h->k.itopk > 0 && (size_t)h->k.itopk > required_itopk) {
        required_itopk = (size_t)h->k.itopk;
      }
      /* Raise-only, exactly like the baseline: this must not lower whatever
       * cuvsCagraSearchParamsCreate() chose by default, or the "no env vars"
       * build would no longer be bit-identical to the frozen baseline. */
      if (sp->itopk_size < required_itopk) sp->itopk_size = required_itopk;
      /* Opt-in ceiling. The baseline searches >=512 internal candidates even for
       * top-k=10; that is the single biggest per-query cost and capping it is
       * the query-speed lever, at the price of recall. Off by default. */
      if (h->k.itopk_cap > 0 && sp->itopk_size > (size_t)h->k.itopk_cap) {
        sp->itopk_size = (size_t)h->k.itopk_cap;
      }
      sp->search_width = (size_t)h->k.search_width;
    }
    cuvsFilter f; f.type=NO_FILTER; f.addr=(uintptr_t)0;
    if (cuvsCagraSearch(h->res, sp, h->idx, &qt, &nt, &dt, f) != CUVS_SUCCESS) { rc=15; goto done; }
    if (cudaMemcpy(out_ids, nd, sizeof(unsigned)*nq*topk, cudaMemcpyDefault) != cudaSuccess) { rc=16; goto done; }
    if (out_dist) cudaMemcpy(out_dist, dd, sizeof(float)*nq*topk, cudaMemcpyDefault);
  }
done:
  if (own_sp) cuvsCagraSearchParamsDestroy(own_sp);
  if (!reuse) {
    if (dd) cuvsRMMFree(h->res, dd, sizeof(float)*nq*topk);
    if (nd) cuvsRMMFree(h->res, nd, sizeof(unsigned)*nq*topk);
    if (qd) cuvsRMMFree(h->res, qd, sizeof(float)*nq*h->dim);
  }
  if (rc) fprintf(stderr, "[seekdb_cuvs] search rc=%d: %s\n", rc, cuvsGetLastErrorText());
  return rc;
}

void seekdb_cuvs_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return;
  sk_release_scratch(h);
  cuvsCagraIndexDestroy(h->idx);
  cuvsCagraIndexParamsDestroy(h->ip);
  cuvsResourcesDestroy(h->res);
  free(h);
}

int seekdb_cuvs_serialize(void* handle, const char* filename)
{
  sk_cuvs_index* index = (sk_cuvs_index*)handle;
  if (!index || !filename) return 1;
  /* include_dataset=false: the adaptor persists the vectors itself, so embedding
   * a second copy of the dataset here wrote ~128MB per 250K index twice. The
   * load path rebuilds from the persisted vectors if the graph alone cannot be
   * re-attached (see ob_cuvs_deserialize in ob_vsag_adaptor.cpp). */
  if (cuvsCagraSerialize(index->res, filename, index->idx, false) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] serialize failed: %s\n", cuvsGetLastErrorText());
    return 1;
  }
  return 0;
}

void* seekdb_cuvs_deserialize(const char* filename, long n, long dim)
{
  if (!filename || n <= 0 || dim <= 0) return NULL;
  sk_cuvs_index* index = (sk_cuvs_index*)calloc(1, sizeof(*index));
  if (!index) return NULL;
  if (cuvsResourcesCreate(&index->res) != CUVS_SUCCESS) goto fail;
  if (cuvsCagraIndexParamsCreate(&index->ip) != CUVS_SUCCESS) goto fail_res;
  if (cuvsCagraIndexCreate(&index->idx) != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraDeserialize(index->res, filename, index->idx) != CUVS_SUCCESS) goto fail_idx;
  {
    int64_t actual_n = 0;
    int64_t actual_dim = 0;
    if (cuvsCagraIndexGetSize(index->idx, &actual_n) != CUVS_SUCCESS ||
        cuvsCagraIndexGetDims(index->idx, &actual_dim) != CUVS_SUCCESS ||
        actual_n != n || actual_dim != dim) {
      fprintf(stderr, "[seekdb_cuvs] snapshot shape mismatch: %ld x %ld, expected %ld x %ld\n",
              (long)actual_n, (long)actual_dim, n, dim);
      goto fail_idx;
    }
  }
  index->n = n;
  index->dim = dim;
  index->k = sk_resolve_knobs();
  return index;
fail_idx:
  cuvsCagraIndexDestroy(index->idx);
fail_ip:
  cuvsCagraIndexParamsDestroy(index->ip);
fail_res:
  cuvsResourcesDestroy(index->res);
fail:
  fprintf(stderr, "[seekdb_cuvs] deserialize failed: %s\n", cuvsGetLastErrorText());
  free(index);
  return NULL;
}

int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                          const float* query, long nq, long topk,
                          unsigned int* out_ids)
{
  void* h = seekdb_cuvs_build(base, n, dim);
  if (!h) return 10;
  int rc = seekdb_cuvs_search(h, query, nq, topk, out_ids, NULL);
  seekdb_cuvs_free(h);
  return rc;
}
