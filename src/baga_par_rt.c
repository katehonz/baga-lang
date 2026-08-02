/* baga_par_rt.c — shared !Par runtime for LLVM backend (lli -load) */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>


typedef struct { void **data; int64_t len; int64_t cap; } baga_Vec;
static void par_vec_grow(baga_Vec *v) {
    if (v->len == v->cap) { v->cap *= 2; v->data = realloc(v->data, (size_t)v->cap * sizeof(void *)); }
}
static baga_Vec *par_vec_new(void) {
    baga_Vec *v = malloc(sizeof(baga_Vec));
    v->cap = 8; v->len = 0; v->data = malloc(8 * sizeof(void *)); return v;
}
static void par_vec_push_i64(baga_Vec *v, int64_t x) { par_vec_grow(v); v->data[v->len++] = (void *)(intptr_t)x; }
static int64_t par_vec_get_i64(baga_Vec *v, int64_t i) { return (int64_t)(intptr_t)v->data[i]; }
static void par_vec_set_i64(baga_Vec *v, int64_t i, int64_t x) { v->data[i] = (void *)(intptr_t)x; }
static int64_t par_vec_len(baga_Vec *v) { return v->len; }

int64_t baga_cell2(int64_t a, int64_t b) {
    int64_t *p = (int64_t *)malloc(2 * sizeof(int64_t));
    if (!p) { fprintf(stderr, "baga: cell2: oom\n"); exit(1); }
    p[0] = a; p[1] = b; return (int64_t)(intptr_t)p;
}
int64_t baga_cell2_0(int64_t h) { return ((int64_t *)(intptr_t)h)[0]; }
int64_t baga_cell2_1(int64_t h) { return ((int64_t *)(intptr_t)h)[1]; }
typedef int64_t (*baga_par_fn)(int64_t);
typedef struct {
    baga_par_fn fn; int64_t arg; int64_t result; pthread_t th;
    int joined; int detached;
} baga_JoinHandle;
static void *baga_par_trampoline(void *p) {
    baga_JoinHandle *h = (baga_JoinHandle *)p;
    h->result = h->fn(h->arg);
    /* 0=joinable, 1=detach requested, 2=finished (joinable) */
    int old = __sync_lock_test_and_set(&h->detached, 2);
    if (old == 1) free(h); /* parent already detached — we free */
    return NULL;
}
int64_t baga_go(baga_par_fn fn, int64_t arg) {
    baga_JoinHandle *h = (baga_JoinHandle *)calloc(1, sizeof(baga_JoinHandle));
    if (!h) { fprintf(stderr, "baga: go: out of memory\n"); exit(1); }
    h->fn = fn; h->arg = arg; h->joined = 0; h->detached = 0;
    if (pthread_create(&h->th, NULL, baga_par_trampoline, h) != 0) {
        fprintf(stderr, "baga: go: pthread_create failed\n"); exit(1);
    }
    return (int64_t)(intptr_t)h;
}
int64_t baga_go_bg(baga_par_fn fn, int64_t arg) {
    baga_JoinHandle *h = (baga_JoinHandle *)calloc(1, sizeof(baga_JoinHandle));
    if (!h) { fprintf(stderr, "baga: go_bg: out of memory\n"); exit(1); }
    h->fn = fn; h->arg = arg; h->joined = 0; h->detached = 1;
    pthread_attr_t attr; pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&h->th, &attr, baga_par_trampoline, h) != 0) {
        fprintf(stderr, "baga: go_bg: pthread_create failed\n"); exit(1);
    }
    pthread_attr_destroy(&attr);
    return 0;
}
int64_t baga_join(int64_t handle) {
    baga_JoinHandle *h = (baga_JoinHandle *)(intptr_t)handle;
    if (!h) return 0;
    if (h->detached == 1) {
        fprintf(stderr, "baga: join: handle detached\n"); exit(1);
    }
    if (!h->joined) { pthread_join(h->th, NULL); h->joined = 1; }
    int64_t r = h->result; free(h); return r;
}
int64_t baga_detach(int64_t handle) {
    baga_JoinHandle *h = (baga_JoinHandle *)(intptr_t)handle;
    if (!h || h->joined) return -1;
    int old = __sync_lock_test_and_set(&h->detached, 1);
    if (old == 2) { free(h); return 0; } /* already finished */
    if (old == 1) return 0; /* double detach */
    pthread_detach(h->th);
    return 0;
}
typedef struct {
    int64_t *buf; int64_t cap, len, head; int closed;
    pthread_mutex_t mu; pthread_cond_t not_empty, not_full;
} baga_Chan;
int64_t baga_chan_new(int64_t cap) {
    if (cap < 1) cap = 1; /* M1: min buffer 1 (rendezvous = M2) */
    baga_Chan *c = (baga_Chan *)calloc(1, sizeof(baga_Chan));
    if (!c) { fprintf(stderr, "baga: chan_new: oom\n"); exit(1); }
    c->cap = cap; c->buf = (int64_t *)malloc((size_t)cap * sizeof(int64_t));
    if (!c->buf) { fprintf(stderr, "baga: chan_new: oom\n"); exit(1); }
    pthread_mutex_init(&c->mu, NULL);
    pthread_cond_init(&c->not_empty, NULL);
    pthread_cond_init(&c->not_full, NULL);
    return (int64_t)(intptr_t)c;
}
int64_t baga_chan_send(int64_t ch, int64_t v) {
    baga_Chan *c = (baga_Chan *)(intptr_t)ch;
    if (!c) return -1;
    pthread_mutex_lock(&c->mu);
    while (c->len == c->cap && !c->closed) pthread_cond_wait(&c->not_full, &c->mu);
    if (c->closed) { pthread_mutex_unlock(&c->mu); return -1; }
    int64_t i = (c->head + c->len) % c->cap;
    c->buf[i] = v; c->len++;
    pthread_cond_signal(&c->not_empty);
    pthread_mutex_unlock(&c->mu);
    return 0;
}
int64_t baga_chan_recv(int64_t ch) {
    baga_Chan *c = (baga_Chan *)(intptr_t)ch;
    if (!c) return 0;
    pthread_mutex_lock(&c->mu);
    while (c->len == 0 && !c->closed) pthread_cond_wait(&c->not_empty, &c->mu);
    if (c->len == 0) { /* closed + empty */ pthread_mutex_unlock(&c->mu); return 0; }
    int64_t v = c->buf[c->head];
    c->head = (c->head + 1) % c->cap; c->len--;
    pthread_cond_signal(&c->not_full);
    pthread_mutex_unlock(&c->mu);
    return v;
}
int64_t baga_chan_recv2(int64_t ch) {
    baga_Chan *c = (baga_Chan *)(intptr_t)ch;
    if (!c) return baga_cell2(0, 0);
    pthread_mutex_lock(&c->mu);
    while (c->len == 0 && !c->closed) pthread_cond_wait(&c->not_empty, &c->mu);
    if (c->len == 0) { pthread_mutex_unlock(&c->mu); return baga_cell2(0, 0); }
    int64_t v = c->buf[c->head];
    c->head = (c->head + 1) % c->cap; c->len--;
    pthread_cond_signal(&c->not_full);
    pthread_mutex_unlock(&c->mu);
    return baga_cell2(1, v);
}
int64_t baga_chan_try_recv(int64_t ch) {
    baga_Chan *c = (baga_Chan *)(intptr_t)ch;
    if (!c) return baga_cell2(2, 0);
    pthread_mutex_lock(&c->mu);
    if (c->len == 0) {
        int st = c->closed ? 2 : 0;
        pthread_mutex_unlock(&c->mu);
        return baga_cell2(st, 0);
    }
    int64_t v = c->buf[c->head];
    c->head = (c->head + 1) % c->cap; c->len--;
    pthread_cond_signal(&c->not_full);
    pthread_mutex_unlock(&c->mu);
    return baga_cell2(1, v);
}
int64_t baga_chan_recv_timeout(int64_t ch, int64_t ms) {
    baga_Chan *c = (baga_Chan *)(intptr_t)ch;
    if (!c) return baga_cell2(2, 0);
    if (ms < 0) ms = 0;
    struct timespec abs; clock_gettime(CLOCK_REALTIME, &abs);
    abs.tv_sec += ms / 1000;
    abs.tv_nsec += (ms % 1000) * 1000000L;
    if (abs.tv_nsec >= 1000000000L) { abs.tv_sec++; abs.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&c->mu);
    while (c->len == 0 && !c->closed) {
        int rc = pthread_cond_timedwait(&c->not_empty, &c->mu, &abs);
        if (rc == ETIMEDOUT) { pthread_mutex_unlock(&c->mu); return baga_cell2(0, 0); }
    }
    if (c->len == 0) { pthread_mutex_unlock(&c->mu); return baga_cell2(2, 0); }
    int64_t v = c->buf[c->head];
    c->head = (c->head + 1) % c->cap; c->len--;
    pthread_cond_signal(&c->not_full);
    pthread_mutex_unlock(&c->mu);
    return baga_cell2(1, v);
}
int64_t baga_sleep_ms(int64_t ms) {
    if (ms <= 0) return 0;
    struct timespec ts; ts.tv_sec = ms / 1000; ts.tv_nsec = (ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
    return 0;
}
int64_t baga_chan_select2(int64_t c0, int64_t c1) {
    baga_Chan *a = (baga_Chan *)(intptr_t)c0;
    baga_Chan *b = (baga_Chan *)(intptr_t)c1;
    if (!a && !b) return baga_cell2(3, 0);
    int64_t la = 0, lb = 0; int ca = 1, cb = 1;
    if (a) { pthread_mutex_lock(&a->mu); la = a->len; ca = a->closed; pthread_mutex_unlock(&a->mu); }
    if (b) { pthread_mutex_lock(&b->mu); lb = b->len; cb = b->closed; pthread_mutex_unlock(&b->mu); }
    if (la == 0 && lb == 0) {
        if ((!a || ca) && (!b || cb)) return baga_cell2(3, 0);
        return baga_cell2(2, 0);
    }
    int prefer0 = (la >= lb); /* take from fuller first (less likely to starve) */
    if (prefer0 && la > 0) {
        int64_t pr = baga_chan_try_recv(c0);
        if (baga_cell2_0(pr) == 1) return baga_cell2(0, baga_cell2_1(pr));
    }
    if (lb > 0) {
        int64_t pr = baga_chan_try_recv(c1);
        if (baga_cell2_0(pr) == 1) return baga_cell2(1, baga_cell2_1(pr));
    }
    if (la > 0) {
        int64_t pr = baga_chan_try_recv(c0);
        if (baga_cell2_0(pr) == 1) return baga_cell2(0, baga_cell2_1(pr));
    }
    return baga_cell2(2, 0);
}
int64_t baga_chan_select2_wait(int64_t c0, int64_t c1) {
    int flip = 0;
    for (;;) {
        int64_t r = baga_chan_select2(c0, c1);
        int64_t w = baga_cell2_0(r);
        if (w != 2) return r;
        baga_Chan *a = (baga_Chan *)(intptr_t)c0;
        baga_Chan *b = (baga_Chan *)(intptr_t)c1;
        baga_Chan *wa = NULL, *wb = NULL;
        if (a && b) {
            if ((uintptr_t)a < (uintptr_t)b) { wa = a; wb = b; }
            else { wa = b; wb = a; }
        } else { wa = a ? a : b; }
        baga_Chan *wait = (flip && wb) ? wb : wa;
        flip = !flip;
        if (!wait) return baga_cell2(3, 0);
        struct timespec abs; clock_gettime(CLOCK_REALTIME, &abs);
        abs.tv_nsec += 5000000L; /* 5ms */
        if (abs.tv_nsec >= 1000000000L) { abs.tv_sec++; abs.tv_nsec -= 1000000000L; }
        pthread_mutex_lock(&wait->mu);
        if (wait->len == 0 && !wait->closed)
            pthread_cond_timedwait(&wait->not_empty, &wait->mu, &abs);
        pthread_mutex_unlock(&wait->mu);
    }
}
int64_t baga_chan_select2_timeout(int64_t c0, int64_t c1, int64_t ms) {
    if (ms < 0) ms = 0;
    struct timespec deadline; clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += ms / 1000;
    deadline.tv_nsec += (ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
    int flip = 0;
    for (;;) {
        int64_t r = baga_chan_select2(c0, c1);
        int64_t w = baga_cell2_0(r);
        if (w != 2) return r;
        struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))
            return baga_cell2(2, 0);
        baga_Chan *a = (baga_Chan *)(intptr_t)c0;
        baga_Chan *b = (baga_Chan *)(intptr_t)c1;
        baga_Chan *wa = NULL, *wb = NULL;
        if (a && b) {
            if ((uintptr_t)a < (uintptr_t)b) { wa = a; wb = b; }
            else { wa = b; wb = a; }
        } else { wa = a ? a : b; }
        baga_Chan *wait = (flip && wb) ? wb : wa;
        flip = !flip;
        if (!wait) return baga_cell2(3, 0);
        pthread_mutex_lock(&wait->mu);
        if (wait->len == 0 && !wait->closed)
            pthread_cond_timedwait(&wait->not_empty, &wait->mu, &deadline);
        pthread_mutex_unlock(&wait->mu);
    }
}
int64_t baga_chan_close(int64_t ch) {
    baga_Chan *c = (baga_Chan *)(intptr_t)ch;
    if (!c) return -1;
    pthread_mutex_lock(&c->mu);
    c->closed = 1;
    pthread_cond_broadcast(&c->not_empty);
    pthread_cond_broadcast(&c->not_full);
    pthread_mutex_unlock(&c->mu);
    return 0;
}
int64_t baga_chan_len(int64_t ch) {
    baga_Chan *c = (baga_Chan *)(intptr_t)ch;
    if (!c) return 0;
    pthread_mutex_lock(&c->mu);
    int64_t n = c->len;
    pthread_mutex_unlock(&c->mu);
    return n;
}
int64_t baga_mutex_new(void) {
    pthread_mutex_t *m = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    if (!m) { fprintf(stderr, "baga: mutex_new: oom\n"); exit(1); }
    pthread_mutex_init(m, NULL);
    return (int64_t)(intptr_t)m;
}
int64_t baga_mutex_lock(int64_t h) {
    pthread_mutex_t *m = (pthread_mutex_t *)(intptr_t)h;
    if (!m) return -1;
    return (int64_t)pthread_mutex_lock(m);
}
int64_t baga_mutex_unlock(int64_t h) {
    pthread_mutex_t *m = (pthread_mutex_t *)(intptr_t)h;
    if (!m) return -1;
    return (int64_t)pthread_mutex_unlock(m);
}
typedef struct {
    baga_par_fn fn; baga_Vec *in; int64_t jobs; int64_t results;
} baga_PoolCtx;
static int64_t baga_pool_worker(int64_t ctx_h) {
    baga_PoolCtx *ctx = (baga_PoolCtx *)(intptr_t)ctx_h;
    for (;;) {
        int64_t pr = baga_chan_recv2(ctx->jobs);
        if (baga_cell2_0(pr) == 0) break; /* closed + empty */
        int64_t idx = baga_cell2_1(pr);
        int64_t arg = par_vec_get_i64(ctx->in, idx);
        int64_t r = ctx->fn(arg);
        baga_chan_send(ctx->results, baga_cell2(idx, r));
    }
    return 0;
}
baga_Vec *baga_pool_map(baga_par_fn fn, baga_Vec *in, int64_t nw) {
    int64_t n = par_vec_len(in);
    baga_Vec *out = par_vec_new();
    if (n <= 0) return out;
    for (int64_t i = 0; i < n; i++) par_vec_push_i64(out, 0);
    if (nw < 1) nw = 1;
    if (nw > n) nw = n;
    int64_t jobs = baga_chan_new(n);
    int64_t results = baga_chan_new(n);
    baga_PoolCtx *ctx = (baga_PoolCtx *)calloc(1, sizeof(baga_PoolCtx));
    if (!ctx) { fprintf(stderr, "baga: pool_map: oom\n"); exit(1); }
    ctx->fn = fn; ctx->in = in; ctx->jobs = jobs; ctx->results = results;
    int64_t *hs = (int64_t *)malloc((size_t)nw * sizeof(int64_t));
    if (!hs) { fprintf(stderr, "baga: pool_map: oom\n"); exit(1); }
    for (int64_t w = 0; w < nw; w++)
        hs[w] = baga_go(baga_pool_worker, (int64_t)(intptr_t)ctx);
    for (int64_t i = 0; i < n; i++) baga_chan_send(jobs, i);
    baga_chan_close(jobs);
    for (int64_t i = 0; i < n; i++) {
        int64_t pair = baga_chan_recv(results);
        int64_t idx = baga_cell2_0(pair);
        int64_t r = baga_cell2_1(pair);
        if (idx >= 0 && idx < n) par_vec_set_i64(out, idx, r);
    }
    for (int64_t w = 0; w < nw; w++) baga_join(hs[w]);
    free(hs); free(ctx);
    return out;
}
