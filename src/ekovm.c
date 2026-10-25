/* EKO CODE VM – see ekovm.h and docs/VM.md for the mapping to Woody.exe addresses. */
#include "ekovm.h"
#include "crtrand.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>

const char *eko_opcode_names[64] = {
    "NOP", "HANG", "END", "PUSH", "PUSHSTR", "PUSHVAR", "STOREVAR", "ADD", "SUB", "MUL",
    "DIV", "NEG", "TOBOOL", "EQ", "NE", "GT", "GE", "LT", "LE", "OR",
    "AND", "NOT", "JMP", "JF", "DELAY", "SKIP1", "DURING", "PUSHTIME", "SEND", "VOL_FLAG5",
    "VOL_STATE", "VOL_FLAG4", "VOL_FLAG3", "VOL_COUNT", "FOREACH", "PUSHGLOBAL", "STOREGLOBAL", "VOL_HAS", "VOL_HASNOT", "COL_FLAG5",
    "COL_FLAG4", "COL_FLAG6", "COL_FLAG3", "JMPPOP", "VOL_SEQ", "VOL_ACTOR_F2", "VOL_ACTOR_F1", "INVALID", "VOL_FLAG6", "VOL_FLAG2",
    "VOL_ALL_F1", "COL_B3_BIT0", "COL_B2_BIT0", "COL_ALL_F4", "COL_ACTOR_F2", "COL_ACTOR_F4", "COL_ACTOR_F1", "CUT", "MSGTEST", "MSGCLEAR",
    "VOL_PAIR", "DELAYPOP", "RANDOM", "?63"
};

/* the C runtime rand() of Woody.exe (crtrand.h): here because both the game and the ekorun harness link this file */
static uint32_t g_holdrand = 1;                                         /* ptd+0x14, 1 at thread start; the exe never calls srand */
uint32_t crt_rand(void) { g_holdrand = g_holdrand * 214013u + 2531011u; return (g_holdrand >> 16) & 0x7fff; }   /* 0x49a7bd */
void crt_srand(uint32_t seed) { g_holdrand = seed; }

static void warn(EkoVM *vm, const char *s) { if (vm->on_warn) vm->on_warn(vm, s, vm->user); }

/* ------------------------------------------------------------------ loading */
/* every count and offset of the header is checked against the file before it is used (a damaged or truncated code file
 * fails to load instead of reading outside the buffer); the shipped 28 pass all of these */
static int span_ok(size_t n, uint64_t off, uint64_t len) { return off <= n && len <= n - off; }
/* count [n][ids] watcher lists from word q on; the pointer to list i goes to (char *)base + i * stride */
static int lists_ok(const uint32_t *w, size_t n, uint64_t q, uint32_t count, void *base, size_t stride)
{
    for (uint32_t i = 0; i < count; i++) {
        if (q >= n || !span_ok(n, q + 1, w[q])) return 0;
        const uint32_t *l = w + q; memcpy((char *)base + (size_t)i * stride, &l, sizeof l); q += 1 + (uint64_t)w[q];
    }
    return 1;
}
int eko_load(EkoVM *vm, const void *data, size_t size)
{
    memset(vm, 0, sizeof *vm);
    if (size < 16 * 4 || memcmp(data, "EKO CODE", 8) != 0) return -1;
    vm->nwords = size / 4;
    vm->w = (uint32_t *)malloc(vm->nwords * 4); if (!vm->w) return -1;
    memcpy(vm->w, data, vm->nwords * 4);
    uint32_t *w = vm->w; const size_t n = vm->nwords;
    vm->compiler_version = w[vm->nwords - 1];
    if (vm->compiler_version != 6) warn(vm, "Conflit: compiler version != 6");
    vm->nobj = w[2];
    if (!span_ok(n, w[3], vm->nobj)) goto bad;
    vm->objs = w + w[3];                       /* 0x5d0538 */
    vm->base = w[3] + vm->nobj;                /* 0x5d0534 */
    /* variables: 8-byte entries then [count][ids] lists (0x5d0540) */
    vm->nvars = w[4];
    uint32_t p = w[5];
    if (!span_ok(n, p, (uint64_t)vm->nvars * 2)) goto bad;
    vm->varval = (int32_t *)calloc((size_t)vm->nvars + 1, sizeof(int32_t));
    vm->varwatch = (const uint32_t **)calloc((size_t)vm->nvars + 1, sizeof(uint32_t *));
    if (!vm->varval || !vm->varwatch || !lists_ok(w, n, (uint64_t)p + (uint64_t)vm->nvars * 2, vm->nvars, (void *)vm->varwatch, sizeof *vm->varwatch)) goto bad;
    /* volumes: 16-byte entries then lists (0x5d0544) */
    vm->nvol = w[6]; p = w[7];
    if (!span_ok(n, p, (uint64_t)vm->nvol * 4)) goto bad;
    vm->vol = (EkoVolume *)calloc((size_t)vm->nvol + 1, sizeof(EkoVolume));
    if (!vm->vol || !lists_ok(w, n, (uint64_t)p + (uint64_t)vm->nvol * 4, vm->nvol, (char *)vm->vol + offsetof(EkoVolume, watch), sizeof(EkoVolume))) goto bad;
    /* strings (0x5ce2b0) */
    vm->nstr = w[8]; p = w[9];
    if (!span_ok(n, p, vm->nstr)) goto bad;
    vm->str = (const char **)calloc((size_t)vm->nstr + 1, sizeof(char *)); if (!vm->str) goto bad;
    { const char *s = (const char *)(w + p + vm->nstr), *end = (const char *)(w + n);
      for (uint32_t i = 0; i < vm->nstr; i++) { const char *z = s < end ? memchr(s, 0, (size_t)(end - s)) : NULL; if (!z) goto bad; vm->str[i] = s; s = z + 1; } }
    /* collisions: 12-byte entries then lists (0x5d0548) */
    vm->ncol = w[10]; p = w[11];
    if (!span_ok(n, p, (uint64_t)vm->ncol * 3)) goto bad;
    vm->col = (EkoCollision *)calloc((size_t)vm->ncol + 1, sizeof(EkoCollision));
    if (!vm->col || !lists_ok(w, n, (uint64_t)p + (uint64_t)vm->ncol * 3, vm->ncol, (char *)vm->col + offsetof(EkoCollision, watch), sizeof(EkoCollision))) goto bad;

    vm->stamp = (uint32_t *)calloc((size_t)vm->nobj + 1, 4);
    vm->msgmask = (uint32_t *)calloc((size_t)vm->nobj + 1, 4);
    vm->changed_vol = (uint32_t *)calloc((size_t)vm->nvol + 1, 4);
    vm->changed_col = (uint32_t *)calloc((size_t)vm->ncol + 1, 4);
    if (!vm->stamp || !vm->msgmask || !vm->changed_vol || !vm->changed_col) goto bad;
    for (int i = 0; i < EKO_MAX_ACTORS - 1; i++) vm->actor_pool[i].next = &vm->actor_pool[i + 1];
    vm->actor_free = vm->actor_pool;
    vm->wake_cur = vm->wake_a; vm->wake_run = vm->wake_b;
    vm->frame = 1;
    return 0;
bad:
    eko_free(vm); return -1;
}

void eko_free(EkoVM *vm)
{
    free(vm->w); free(vm->varval); free((void *)vm->varwatch); free(vm->vol); free((void *)vm->str);
    free(vm->col); free(vm->stamp); free(vm->msgmask); free(vm->changed_vol); free(vm->changed_col);
    memset(vm, 0, sizeof *vm);
}

/* ------------------------------------------------------------------ helpers */
uint32_t eko_obj_of(const EkoVM *vm, uint32_t pc)   /* 0x444060 */
{
    for (uint32_t i = 0; i + 1 < vm->nobj; i++)
        if (vm->objs[i] <= pc && vm->objs[i + 1] > pc) return i;
    return vm->nobj ? vm->nobj - 1 : 0;
}

static void wake(EkoVM *vm, uint32_t obj)            /* 0x442210 */
{
    if (vm->nwake_cur <= 1000) vm->wake_cur[vm->nwake_cur++] = obj;
}

static void wake_list(EkoVM *vm, const uint32_t *lst)
{
    for (uint32_t i = 0; i < lst[0]; i++) wake(vm, lst[1 + i]);
}

void eko_set_var(EkoVM *vm, uint32_t var, int32_t value)   /* 0x443ca0 */
{
    var &= 0xffffff;
    if (var >= vm->nvars) return;
    wake_list(vm, vm->varwatch[var]);
    vm->varval[var] = value;
}

static void vol_changed(EkoVM *vm, uint32_t v)       /* 0x443d20 */
{
    if (v >= vm->nvol || vm->vol[v].stamp == vm->frame) return;
    vm->vol[v].stamp = vm->frame;
    vm->changed_vol[vm->nchanged_vol++] = v;
    wake_list(vm, vm->vol[v].watch);
}
static void col_changed(EkoVM *vm, uint32_t c)       /* 0x443db0 */
{
    if (c >= vm->ncol || vm->col[c].stamp == vm->frame) return;
    vm->col[c].stamp = vm->frame;
    vm->changed_col[vm->nchanged_col++] = c;
    wake_list(vm, vm->col[c].watch);
}

static void list_add(EkoVM *vm, EkoActorEntry **head, uint32_t actor, uint32_t flags)  /* 0x4447e0 */
{
    EkoActorEntry *e = vm->actor_free;
    if (!e) { warn(vm, "actor entry pool exhausted"); return; }
    vm->actor_free = e->next;
    e->actor = actor; e->flags = flags; e->next = *head; *head = e;
}
static int list_remove_flagged(EkoVM *vm, EkoActorEntry **head, uint32_t mask)   /* 0x444800 */
{
    int n = 0;
    while (*head) {
        EkoActorEntry *e = *head;
        if (e->flags & mask) { *head = e->next; e->next = vm->actor_free; vm->actor_free = e; n++; }
        else head = &e->next;
    }
    return n;
}
static EkoActorEntry *list_find(EkoActorEntry *e, uint32_t actor)
{
    for (; e; e = e->next) if (e->actor == actor) return e;
    return NULL;
}

/* ------------------------------------------------------------------ volumes (engine -> VM) */
#define VOL(v) (&vm->vol[(v) & 0xffffff])
#define COL(c) (&vm->col[(c) & 0xffffff])
#define CHK_VOL(v) if (((v) & 0xffffff) >= vm->nvol) return
#define CHK_COL(c) if (((c) & 0xffffff) >= vm->ncol) return

void eko_vol_enter(EkoVM *vm, uint32_t v, uint32_t actor)          /* 0x441da0 -> 0x4443f0 */
{
    CHK_VOL(v); EkoVolume *o = VOL(v); actor &= 0xffffff;
    if (o->count == 0) o->flags |= 0x40;
    o->flags |= 6; o->count++;
    list_add(vm, &o->list, actor, 6);
    o->time = vm->frame;
    vol_changed(vm, v & 0xffffff);
}
void eko_vol_in(EkoVM *vm, uint32_t v, uint32_t actor)             /* 0x441e10 -> 0x444430 */
{
    CHK_VOL(v); EkoVolume *o = VOL(v); actor &= 0xffffff;
    o->flags |= 4; o->time = vm->frame;
    EkoActorEntry *e = list_find(o->list, actor);
    if (e) e->flags |= 4; else warn(vm, "world_volume::MessageIn sans Enter prealable");
    vol_changed(vm, v & 0xffffff);
}
void eko_vol_leave(EkoVM *vm, uint32_t v, uint32_t actor)          /* 0x441e70 -> 0x444470 */
{
    CHK_VOL(v); EkoVolume *o = VOL(v); actor &= 0xffffff;
    o->flags |= 1;
    EkoActorEntry *e = list_find(o->list, actor);
    if (e) e->flags |= 1; else warn(vm, "world_volume::MessageLeave sans Enter prealable");
    vol_changed(vm, v & 0xffffff);
}
void eko_vol_perso_enter(EkoVM *vm, uint32_t v, uint32_t actor)    /* 0x441f00 -> 0x4444e0 */
{
    CHK_VOL(v); EkoVolume *o = VOL(v); actor &= 0xffffff; vm->last_perso_vol = actor;
    if (o->count == 0) o->flags |= 0x40;
    o->flags |= 0x36; o->count++;
    list_add(vm, &o->list, actor, 6);
    o->time = vm->frame;
    vol_changed(vm, v & 0xffffff);
}
void eko_vol_perso_in(EkoVM *vm, uint32_t v, uint32_t actor)       /* 0x441f40 -> 0x444530 */
{
    CHK_VOL(v); EkoVolume *o = VOL(v); vm->last_perso_vol = actor & 0xffffff;
    o->flags |= 0x24; o->time = vm->frame;
    vol_changed(vm, v & 0xffffff);
}
void eko_vol_perso_leave(EkoVM *vm, uint32_t v, uint32_t actor)    /* 0x441f80 -> 0x444550 */
{
    CHK_VOL(v); EkoVolume *o = VOL(v); actor &= 0xffffff; vm->last_perso_vol = actor;
    o->flags |= 9;
    EkoActorEntry *e = list_find(o->list, actor);
    if (e) e->flags |= 1; else warn(vm, "world_volume::MessagePersoLeave sans Enter prealable");
    vol_changed(vm, v & 0xffffff);
}
int eko_vol_has_actor_f1(EkoVM *vm, uint32_t v, uint32_t actor)    /* 0x443e20 -> 0x4444b0 */
{
    if ((v & 0xffffff) >= vm->nvol) return 0;
    for (EkoActorEntry *e = VOL(v)->list; e; e = e->next)
        if (e->actor == (actor & 0xffffff)) return (e->flags & 1) == 0;
    return 0;
}
void eko_actor_leave_all(EkoVM *vm, uint32_t actor)                /* 0x443ff0 */
{
    actor &= 0xffffff;
    for (uint32_t v = 0; v < vm->nvol; v++)
        if (eko_vol_has_actor_f1(vm, v, actor)) { eko_vol_perso_leave(vm, v, actor); }
}

/* collisions */
void eko_col_press(EkoVM *vm, uint32_t c, uint32_t actor)          /* 0x442080 -> 0x4445e0 */
{
    CHK_COL(c); EkoCollision *o = COL(c); actor &= 0xffffff;
    if (o->count == 0) o->flags2 |= 1;
    o->flags |= 3; o->count++;
    list_add(vm, &o->list, actor, 3);
    col_changed(vm, c & 0xffffff);
}
void eko_col_in(EkoVM *vm, uint32_t c, uint32_t actor)             /* 0x4420c0 -> 0x444610 */
{
    CHK_COL(c); EkoCollision *o = COL(c); actor &= 0xffffff;
    o->flags |= 1;
    EkoActorEntry *e = list_find(o->list, actor);
    if (e) e->flags |= 1; else warn(vm, "world_collision::MessageIn sans Press prealable");
    col_changed(vm, c & 0xffffff);
}
void eko_col_unpress(EkoVM *vm, uint32_t c, uint32_t actor)        /* 0x442100 -> 0x444650 */
{
    CHK_COL(c); EkoCollision *o = COL(c); actor &= 0xffffff;
    o->flags |= 4;
    EkoActorEntry *e = list_find(o->list, actor);
    if (e) e->flags |= 4; else warn(vm, "world_collision::UnPress sans Press prealable");
    col_changed(vm, c & 0xffffff);
}
void eko_col_perso_press(EkoVM *vm, uint32_t c, uint32_t actor)    /* 0x441fc0 -> 0x444690 */
{
    CHK_COL(c); EkoCollision *o = COL(c); actor &= 0xffffff; vm->last_perso_col = actor;
    if (o->count == 0) o->flags2 |= 1;
    o->flags |= 0x33; o->count++;
    list_add(vm, &o->list, actor, 3);
    col_changed(vm, c & 0xffffff);
}
void eko_col_perso_in(EkoVM *vm, uint32_t c, uint32_t actor)       /* 0x442000 -> 0x4446d0 */
{
    CHK_COL(c); EkoCollision *o = COL(c); vm->last_perso_col = actor & 0xffffff;
    o->flags |= 0x11;
    col_changed(vm, c & 0xffffff);
}
void eko_col_perso_unpress(EkoVM *vm, uint32_t c, uint32_t actor)  /* 0x442040 -> 0x4446f0 */
{
    CHK_COL(c); EkoCollision *o = COL(c); actor &= 0xffffff; vm->last_perso_col = actor;
    o->flags |= 0x44;
    EkoActorEntry *e = list_find(o->list, actor);
    if (e) e->flags |= 4; else warn(vm, "world_collision::MessagePersoUnpress sans Press prealable");
    col_changed(vm, c & 0xffffff);
}

void eko_msgmask_set(EkoVM *vm, uint32_t obj, uint32_t bits)       /* 0x443e50 */
{
    obj &= 0xffffff; if (obj > vm->nobj) return;
    uint32_t v = vm->msgmask[obj] | bits;
    if (v != vm->msgmask[obj]) { vm->msgmask[obj] = v; wake(vm, obj); }
}
void eko_msgmask_clear(EkoVM *vm, uint32_t obj, uint32_t bits)     /* 0x443e90 */
{
    obj &= 0xffffff; if (obj > vm->nobj) return;
    uint32_t v = vm->msgmask[obj] & ~bits;
    if (v != vm->msgmask[obj]) { vm->msgmask[obj] = v; wake(vm, obj); }
}
int eko_msgmask_test(EkoVM *vm, uint32_t obj, uint32_t bits)       /* 0x443ed0 */
{
    obj &= 0xffffff; if (obj > vm->nobj) return 0;
    return (vm->msgmask[obj] & bits) == bits;
}

/* ------------------------------------------------------------------ timers */
static void delay_add(EkoVM *vm, int32_t time, uint32_t target)   /* 0x444290: sorted insert */
{
    if (vm->ndelays >= EKO_MAX_TIMERS) return;
    int i = vm->ndelays;
    while (i > 0 && vm->delays[i - 1].time >= time) { vm->delays[i] = vm->delays[i - 1]; i--; }
    /* exe walks from the head and inserts before the first node with node.time >= new.time */
    vm->delays[i].time = time; vm->delays[i].target = target; vm->delays[i].used = 1;
    vm->ndelays++;
}
static int delay_pop_expired(EkoVM *vm, int32_t now, uint32_t *target)   /* 0x444310 */
{
    if (vm->ndelays == 0 || vm->delays[0].time > now) return 0;
    *target = vm->delays[0].target;
    memmove(&vm->delays[0], &vm->delays[1], (vm->ndelays - 1) * sizeof(EkoTimer));
    vm->ndelays--;
    return 1;
}
static void during_add(EkoVM *vm, int32_t time, uint32_t target)  /* 0x4440e0: append at tail */
{
    if (vm->ndurings >= EKO_MAX_TIMERS) return;
    vm->durings[vm->ndurings].time = time; vm->durings[vm->ndurings].target = target;
    vm->durings[vm->ndurings].used = 1; vm->ndurings++;
}
void eko_cancel_timers(EkoVM *vm, uint32_t obj)                    /* 0x444380 + 0x4441d0 */
{
    obj &= 0xfffffff;
    for (int i = 0; i < vm->ndelays; ) {
        if (eko_obj_of(vm, vm->delays[i].target) == obj) { memmove(&vm->delays[i], &vm->delays[i + 1], (vm->ndelays - i - 1) * sizeof(EkoTimer)); vm->ndelays--; }
        else i++;
    }
    for (int i = 0; i < vm->ndurings; ) {
        if (eko_obj_of(vm, vm->durings[i].target) == obj) { memmove(&vm->durings[i], &vm->durings[i + 1], (vm->ndurings - i - 1) * sizeof(EkoTimer)); vm->ndurings--; }
        else i++;
    }
}

/* ------------------------------------------------------------------ messages */
static void send_msg(EkoVM *vm, int n)                              /* 0x443220 + 0x441cd0 */
{
    if (n < 1 || n > vm->sp) { vm->error = 1; return; }
    EkoMsg *m;
    if (vm->nmsgs < EKO_MAX_MSGS) m = &vm->msgs[vm->nmsgs++];
    else { if (!vm->warned_too_many_msgs) { warn(vm, "Trop de mesages a l'init. Maximum 1280."); vm->warned_too_many_msgs = 1; } m = &vm->msgs[EKO_MAX_MSGS - 1]; }
    vm->stat_msgs_total++; vm->stat_msgs_out++;
    int32_t *a = &vm->stack[vm->sp - n];
    m->id = (uint32_t)a[0]; m->nargs = (uint32_t)(n - 1);
    int copy = n - 1; if (copy > EKO_MAX_MSG_ARGS) copy = EKO_MAX_MSG_ARGS;
    m->nargs = (uint32_t)copy;                                       /* the handlers loop to nargs over args[EKO_MAX_MSG_ARGS] */
    for (int i = 0; i < copy; i++) m->args[i] = (uint32_t)a[1 + i];
    vm->sp -= n;
    if (vm->on_msg && !vm->discard_msgs && !vm->defer_msgs) vm->on_msg(vm, m, vm->user);
}

/* ------------------------------------------------------------------ interpreter (0x4429f0) */
#define PUSH(v)  do { if (vm->sp >= EKO_STACK) { vm->error = 1; vm->stop = 1; break; } vm->stack[vm->sp++] = (int32_t)(v); } while (0)
#define POP()    (vm->sp > 0 ? vm->stack[--vm->sp] : (vm->error = 1, vm->stop = 1, 0))
#define TOP()    (*(vm->sp > 0 ? &vm->stack[vm->sp - 1] : (vm->error = 1, vm->stop = 1, &junk)))
#define BPUSH(b) do { if (vm->bsp >= EKO_STACK) { vm->error = 1; vm->stop = 1; break; } vm->bstack[vm->bsp++] = (b) ? 1 : 0; } while (0)
#define BPOP()   (vm->bsp > 0 ? vm->bstack[--vm->bsp] : (vm->error = 1, vm->stop = 1, 0))

static int vol_actor_flag(EkoVM *vm, uint32_t v, uint32_t actor, uint32_t fl, int need_volflag)
{
    v &= 0xffffff; if (v >= vm->nvol) return 0;
    EkoVolume *o = &vm->vol[v];
    if (need_volflag && !(o->flags & need_volflag)) return 0;
    for (EkoActorEntry *e = o->list; e; e = e->next)
        if (e->actor == actor && (e->flags & fl)) return 1;
    return 0;
}
static int col_actor_flag(EkoVM *vm, uint32_t c, uint32_t actor, uint32_t fl)
{
    c &= 0xffffff; if (c >= vm->ncol) return 0;
    for (EkoActorEntry *e = vm->col[c].list; e; e = e->next)
        if (e->actor == actor && (e->flags & fl)) return 1;
    return 0;
}

void eko_run(EkoVM *vm, uint32_t pc)
{
    uint32_t *w = vm->w; const uint32_t b = vm->base; const size_t n = vm->nwords;
    int32_t junk = 0;                                                   /* TOP() of an empty stack: a fault, not a write below the stack */
    int had = vm->error; vm->error = 0;                                 /* error: the fault of THIS run ends it (and stays set for ekorun's report), it
                                                                         * does not end every later run of the level after one first fault */
    vm->stop = 0;
    while (!vm->stop) {
        if ((size_t)(b + pc) >= n) { vm->error = 1; break; }
        uint32_t op = w[b + pc];
        if (op >= 0x3f) { vm->stop = 1; break; }              /* 0x442a1b */
        uint32_t a1 = (size_t)(b + pc + 1) < n ? w[b + pc + 1] : 0;
        uint32_t a2 = (size_t)(b + pc + 2) < n ? w[b + pc + 2] : 0;
        uint32_t a3 = (size_t)(b + pc + 3) < n ? w[b + pc + 3] : 0;
        int32_t x, y, *t;
        switch (op) {
        case 0:  pc += 1; break;                                        /* 0x443210 */
        case 1:  vm->error = 1; vm->stop = 1; break;                    /* 0x443200 would spin forever */
        case 2:  vm->stop = 1; pc += 1; break;                          /* 0x4431e0 */
        case 3:  PUSH(a1); pc += 2; break;                              /* 0x442cb0 */
        case 4:  PUSH((int32_t)(a1 < vm->nstr ? (intptr_t)a1 : 0)); pc += 2; break; /* 0x442cd0: pushes char*; we push the index */
        case 5:  PUSH(a1 < vm->nvars ? vm->varval[a1] : 0); pc += 2; break;         /* 0x442d00 */
        case 6:  x = POP(); eko_set_var(vm, a1, x); pc += 2; break;    /* 0x442d30 */
        case 7:  x = POP(); t = &TOP(); *t = *t + x; pc += 1; break;    /* 0x442d60 */
        case 8:  x = POP(); t = &TOP(); *t = *t - x; pc += 1; break;    /* 0x442d90 */
        case 9:  x = POP(); t = &TOP(); *t = *t * x; pc += 1; break;    /* 0x442dc0 */
        case 10: x = POP(); t = &TOP(); *t = x ? *t / x : 0; pc += 1; break;   /* 0x442df0 (idiv; exe traps on 0) */
        case 11: t = &TOP(); *t = -*t; pc += 1; break;                  /* 0x442e20 */
        case 12: x = POP(); BPUSH(x != 0); pc += 1; break;              /* 0x442e40 */
        case 13: y = POP(); x = POP(); BPUSH(x == y); pc += 1; break;
        case 14: y = POP(); x = POP(); BPUSH(x != y); pc += 1; break;
        case 15: y = POP(); x = POP(); BPUSH(x >  y); pc += 1; break;
        case 16: y = POP(); x = POP(); BPUSH(x >= y); pc += 1; break;
        case 17: y = POP(); x = POP(); BPUSH(x <  y); pc += 1; break;
        case 18: y = POP(); x = POP(); BPUSH(x <= y); pc += 1; break;
        case 19: { int q = BPOP(); int p = BPOP(); BPUSH(p || q); pc += 1; break; }   /* 0x443000 */
        case 20: { int q = BPOP(); int p = BPOP(); BPUSH(p && q); pc += 1; break; }   /* 0x443050 */
        case 21: if (vm->bsp > 0) vm->bstack[vm->bsp - 1] = !vm->bstack[vm->bsp - 1]; pc += 1; break; /* 0x4430a0 */
        case 22: pc = a1; break;                                        /* 0x4430f0 */
        case 23: pc = BPOP() ? pc + 2 : a1; break;                      /* 0x4430c0 */
        case 24: delay_add(vm, vm->time + (int32_t)a1, a2); pc += 3; break;   /* 0x443110 */
        case 25: pc += 2; break;                                        /* 0x4431d0 */
        case 26: during_add(vm, vm->time + (int32_t)a1, a2); pc += 3; break;  /* 0x443180 */
        case 27: PUSH(vm->time); pc += 1; break;                        /* 0x4431b0 */
        case 28: send_msg(vm, (int)a1); pc += 2; break;                 /* 0x443220 */
        case 29: BPUSH(a1 < vm->nvol && (vm->vol[a1].flags >> 5) & 1); pc += 2; break;
        case 30: { uint16_t f = a1 < vm->nvol ? vm->vol[a1].flags : 0; BPUSH(f == 0 || (f & 9)); pc += 2; break; }
        case 31: BPUSH(a1 < vm->nvol && (vm->vol[a1].flags >> 4) & 1); pc += 2; break;
        case 32: BPUSH(a1 < vm->nvol && (vm->vol[a1].flags >> 3) & 1); pc += 2; break;
        case 33: PUSH(a1 < vm->nvol ? vm->vol[a1].count : 0); pc += 2; break;
        case 34: {                                                      /* 0x443630 FOREACH */
            uint32_t v = a1 & 0xffffff;
            if (v < vm->nvol)
                for (EkoActorEntry *e = vm->vol[v].list; e; e = e->next)
                    if (!(e->flags & 1)) { vm->globals[0] = (int32_t)e->actor; eko_run(vm, pc + 3); }
            pc = a2; break;   /* note: vm->stop stays set if the body ran -> outer run ends here, as in the exe */
        }
        case 35: PUSH(a1 < 2 ? vm->globals[a1] : 0); pc += 2; break;
        case 36: x = POP(); if (a1 < 2) vm->globals[a1] = x; pc += 2; break;
        case 37: {                                                      /* 0x4432e0 */
            uint32_t v = a1; int r = 0;
            if (v < vm->nvol && (vm->vol[v].flags & 4) && (vm->vol[v].flags & 0x24))
                for (EkoActorEntry *e = vm->vol[v].list; e; e = e->next) if (e->actor == a2) { r = 1; break; }
            BPUSH(r); pc += 3; break;
        }
        case 38: {                                                      /* 0x4433c0 */
            uint32_t v = a1; int found = 0;
            if (v < vm->nvol) for (EkoActorEntry *e = vm->vol[v].list; e; e = e->next) if (e->actor == a2) { found = 1; break; }
            BPUSH(!found); pc += 3; break;
        }
        case 39: BPUSH(a1 < vm->ncol && (vm->col[a1].flags >> 5) & 1); pc += 2; break;
        case 40: BPUSH(a1 < vm->ncol && (vm->col[a1].flags >> 4) & 1); pc += 2; break;
        case 41: BPUSH(a1 < vm->ncol && (vm->col[a1].flags >> 6) & 1); pc += 2; break;
        case 42: BPUSH(a1 < vm->ncol && (vm->col[a1].flags >> 3) & 1); pc += 2; break;
        case 43: pc = (uint32_t)POP(); break;                           /* 0x4438e0 */
        case 44: BPUSH(a1 < vm->nvol && a2 < vm->nvol && vm->vol[a2].time - vm->vol[a1].time == 1); pc += 3; break;
        case 45: BPUSH(vol_actor_flag(vm, a1, a2, 2, 2)); pc += 3; break;   /* 0x443430 */
        case 46: BPUSH(vol_actor_flag(vm, a1, a2, 1, 1)); pc += 3; break;   /* 0x4434a0 */
        case 47: vm->stop = 1; pc += 1; break;
        case 48: BPUSH(a1 < vm->nvol && (vm->vol[a1].flags >> 6) & 1); pc += 2; break;
        case 49: BPUSH(a1 < vm->nvol && (vm->vol[a1].flags >> 2) & 1); pc += 2; break;
        case 50: {                                                      /* 0x443a80 */
            uint32_t acc = 0;
            if (a1 < vm->nvol && vm->vol[a1].list) { acc = 1; for (EkoActorEntry *e = vm->vol[a1].list; e; e = e->next) acc &= e->flags; }
            BPUSH(acc == 1); pc += 2; break;
        }
        case 51: BPUSH(a1 < vm->ncol && (vm->col[a1].flags2 & 1)); pc += 2; break;
        case 52: BPUSH(a1 < vm->ncol && (vm->col[a1].flags & 1)); pc += 2; break;
        case 53: {                                                      /* 0x443b40 */
            uint32_t acc = 0;
            if (a1 < vm->ncol && vm->col[a1].list) { acc = 4; for (EkoActorEntry *e = vm->col[a1].list; e; e = e->next) acc &= e->flags; }
            BPUSH(acc == 4); pc += 2; break;
        }
        case 54: BPUSH(col_actor_flag(vm, a1, a2, 2)); pc += 3; break;
        case 55: BPUSH(col_actor_flag(vm, a1, a2, 4)); pc += 3; break;
        case 56: BPUSH(col_actor_flag(vm, a1, a2, 1)); pc += 3; break;
        case 57: BPUSH(0); if (!vm->warned_cut) { warn(vm, "Emulation du mot cle cut ajournee."); vm->warned_cut = 1; } pc += 2; break;
        case 58: x = POP(); BPUSH(a1 <= vm->nobj && (vm->msgmask[a1] & (uint32_t)x)); pc += 2; break;   /* 0x443bd0 */
        case 59: if (a1 < vm->nobj) vm->msgmask[a1] = 0; pc += 2; break;                                 /* 0x443c20 */
        case 60: {                                                      /* 0x443960 */
            int r = 0;
            if (a1 < vm->nvol && (vm->vol[a1].flags & 1))
                for (EkoActorEntry *e = vm->vol[a1].list; e; e = e->next)
                    if (e->actor == a3 && (e->flags & 1)) {
                        if (a2 < vm->nvol) for (EkoActorEntry *f = vm->vol[a2].list; f; f = f->next)
                            if (f->actor == a3 && (f->flags & 4)) { r = 1; break; }
                        break;
                    }
            BPUSH(r); pc += 4; break;
        }
        case 61: x = POP(); delay_add(vm, vm->time + x, a1); pc += 2; break;   /* 0x443140 */
        case 62: {                                                      /* 0x443c50 */
            uint32_t m = (uint32_t)POP(); if (m == 0) m = 1;
            PUSH((int32_t)(crt_rand() % m)); pc += 1; break;          /* 0x443c74: the CRT rand() itself, the game's one stream */
        }
        default: vm->error = 1; vm->stop = 1; break;
        }
        if (vm->error) break;
    }
    if (vm->error) vm->sp = vm->bsp = 0;                                /* a faulted run leaves no half-built operands for the next one */
    vm->error |= had;
}

/* ------------------------------------------------------------------ init & tick */
static void run_object_init(EkoVM *vm, uint32_t o)   /* body of the loop at 0x442819 */
{
    uint32_t s = vm->base + vm->objs[o];
    if ((size_t)s + 1 >= vm->nwords) return;
    uint32_t w0 = vm->w[s], w1 = vm->w[s + 1];
    vm->w[s] = 0; vm->w[s + 1] = 0;
    eko_run(vm, vm->objs[o]);
    vm->nwake_cur = 0;                    /* [0x5d0570] = 0 */
    vm->w[s] = w0; vm->w[s + 1] = w1;
}

static void reset_lists(EkoVM *vm)         /* 0x444240 + 0x444090 */
{
    vm->ndelays = vm->ndurings = 0;
}

void eko_init(EkoVM *vm)                   /* 0x4427e0 */
{
    reset_lists(vm);
    vm->globals[0] = vm->globals[1] = 0;
    vm->bsp = vm->sp = 0;
    vm->discard_msgs = 1; vm->defer_msgs = 1;   /* the caller hands the messages over after this function: a variable written by one of them (1082 LevelIsEnable)
                                                * must wake its watcher in the first tick, and every wake list is cleared here */
    for (uint32_t o = 0; o < vm->nobj; o++) run_object_init(vm, o);
    vm->discard_msgs = 0;
    /* pass 1 done: clear stamps, msgmask, wake lists, discard messages (0x441d40), reset timers */
    memset(vm->stamp, 0, (vm->nobj + 1) * 4);
    memset(vm->msgmask, 0, (vm->nobj + 1) * 4);
    for (uint32_t i = 0; i < vm->nvol; i++) vm->vol[i].stamp = 0;
    for (uint32_t i = 0; i < vm->ncol; i++) vm->col[i].stamp = 0;
    vm->nwake_cur = vm->nwake_run = vm->nchanged_vol = vm->nchanged_col = 0;
    vm->nmsgs = 0;
    reset_lists(vm);
    for (uint32_t o = 0; o < vm->nobj; o++) run_object_init(vm, o);
    memset(vm->stamp, 0, (vm->nobj + 1) * 4);
    memset(vm->msgmask, 0, (vm->nobj + 1) * 4);
    vm->nwake_cur = vm->nwake_run = vm->nchanged_vol = vm->nchanged_col = 0;
    vm->bsp = vm->sp = 0;
    vm->defer_msgs = 0;
}

int eko_tick(EkoVM *vm, int32_t time)      /* 0x444050 then 0x442240 */
{
    vm->time = time;
    vm->stat_msgs_out = 0;
    int ran = 0;
    uint32_t target;
    while (delay_pop_expired(vm, vm->time, &target)) { vm->stat_delays_run++; eko_run(vm, target); }   /* 0x442350 */
    /* 0x4423a0 -> 0x444150: EVERY entry runs on every tick; one whose time has passed is unlinked first and runs a last time.
     * So DURING d t = "run t each tick for d" (W2B's boss intro blacks out the picture for 0.5 s with 1152 this way). */
    for (int i = 0; i < vm->ndurings; ) {
        uint32_t t = vm->durings[i].target;
        if (vm->durings[i].time < vm->time) { memmove(&vm->durings[i], &vm->durings[i + 1], (vm->ndurings - i - 1) * sizeof(EkoTimer)); vm->ndurings--; }
        else i++;
        vm->stat_durings_run++; eko_run(vm, t);
    }
    /* swap wake lists (0x442320) */
    { uint32_t *t = vm->wake_run; vm->wake_run = vm->wake_cur; vm->wake_cur = t;
      vm->nwake_run = vm->nwake_cur; vm->nwake_cur = 0; }
    for (int i = vm->nwake_run - 1; i >= 0; i--) {                    /* 0x442269 walks from the end */
        uint32_t o = vm->wake_run[i];
        if (o >= vm->nobj || vm->stamp[o] == vm->frame) continue;
        vm->stamp[o] = vm->frame;
        ran++;
        eko_run(vm, vm->objs[o]);
    }
    /* end of frame: clear transient volume/collision flags (0x4423f0 / 0x442450) */
    for (int i = 0; i < vm->nchanged_vol; i++) {
        EkoVolume *o = &vm->vol[vm->changed_vol[i]];
        o->count -= (uint16_t)list_remove_flagged(vm, &o->list, 9);            /* 0x4445c0 */
        o->flags &= 0xffa4; for (EkoActorEntry *e = o->list; e; e = e->next) e->flags &= 0xffffffa4;   /* 0x444590 */
        o->flags = 0;                                                            /* mov word [esi+2], 0 */
    }
    vm->nchanged_vol = 0;
    for (int i = 0; i < vm->nchanged_col; i++) {
        EkoCollision *o = &vm->col[vm->changed_col[i]];
        o->count -= (uint16_t)list_remove_flagged(vm, &o->list, 0x44);         /* 0x444760 */
        { uint16_t f = (uint16_t)(o->flags | (o->flags2 << 8)); f &= 0xfe99; o->flags = (uint8_t)f; o->flags2 = (uint8_t)(f >> 8); }  /* 0x444730 */
        for (EkoActorEntry *e = o->list; e; e = e->next) e->flags &= 0xfffffe99;
        o->flags = 0; o->flags2 = 0;                                             /* mov word [esi+2], 0 */
    }
    vm->nchanged_col = 0;
    vm->frame++;
    vm->stat_objects_run = (uint32_t)ran; vm->stat_objects_total += (uint32_t)ran;
    return ran;
}
