/* EKO CODE virtual machine – C reimplementation of the script interpreter of
 * Woody Woodpecker: Escape from Buzz Buzzard Park (PC, Eko Software 2001).
 *
 * Behaviour mirrors Woody.exe (build 2001-10-17): loader 0x442570, init 0x4427e0,
 * interpreter 0x4429f0, handler table 0x442a30, tick 0x442240, volume/collision
 * helpers 0x4443f0..0x444800, timer lists 0x444090..0x444380.
 * See docs/VM.md.
 */
#ifndef EKOVM_H
#define EKOVM_H
#include <stdint.h>
#include <stddef.h>

#define EKO_MAX_MSGS      1280   /* 0x500 records in the exe (0x5bd300) */
#define EKO_MAX_MSG_ARGS  10     /* 48-byte record: id, nargs, 10 args */
#define EKO_MAX_WAKE      1001   /* 0x3e8 + 1 */
#define EKO_MAX_TIMERS    1000   /* 0x3e80 bytes / 16 per list */
#define EKO_MAX_ACTORS    500    /* actor-entry pool 0x5d6378..0x5d7ae8 = 0x1770/12 */
#define EKO_STACK         1024

typedef struct {
    uint32_t id;
    uint32_t nargs;
    uint32_t args[EKO_MAX_MSG_ARGS];
} EkoMsg;

/* actor entry in a volume / collision list: {next, actor, flags} */
typedef struct EkoActorEntry {
    struct EkoActorEntry *next;
    uint32_t actor;
    uint32_t flags;
} EkoActorEntry;

/* world_volume slot (16 bytes in the exe) */
typedef struct {
    uint16_t count;
    uint16_t flags;
    uint32_t time;          /* frame of last event */
    const uint32_t *watch;  /* [count][obj ids...] inside the code image */
    EkoActorEntry *list;
    uint32_t stamp;         /* frame stamp used for the changed-list */
} EkoVolume;

/* world_collision slot (12 bytes in the exe): count u16, flags u8 at +2, flags2 u8 at +3 */
typedef struct {
    uint16_t count;
    uint8_t flags;
    uint8_t flags2;
    const uint32_t *watch;
    EkoActorEntry *list;
    uint32_t stamp;
} EkoCollision;

typedef struct {
    int32_t time;           /* absolute VM time (1/100 s) */
    uint32_t target;        /* code index */
    int used;
    int order;              /* insertion order (for the unsorted DURING list) */
} EkoTimer;

typedef struct EkoVM EkoVM;
typedef void (*EkoMsgCallback)(EkoVM *vm, const EkoMsg *msg, void *user);
typedef void (*EkoWarnCallback)(EkoVM *vm, const char *text, void *user);

struct EkoVM {
    /* code image */
    uint32_t *w; size_t nwords;
    uint32_t nobj; const uint32_t *objs; uint32_t base;
    uint32_t nvars; int32_t *varval; const uint32_t **varwatch;
    uint32_t nvol;  EkoVolume *vol;
    uint32_t nstr;  const char **str;
    uint32_t ncol;  EkoCollision *col;
    uint32_t compiler_version;

    /* execution state */
    int32_t stack[EKO_STACK]; int sp;
    uint8_t bstack[EKO_STACK]; int bsp;
    int32_t globals[2];
    int32_t time;
    uint32_t frame;
    int stop;
    int error;              /* set on stack over/underflow or bad opcode */

    /* outgoing messages (script -> engine) */
    EkoMsg msgs[EKO_MAX_MSGS]; int nmsgs;
    int warned_too_many_msgs;

    /* timers */
    EkoTimer delays[EKO_MAX_TIMERS];  int ndelays;
    EkoTimer durings[EKO_MAX_TIMERS]; int ndurings; int during_order;

    /* wake lists (double buffered) and per-object stamps */
    uint32_t wake_a[EKO_MAX_WAKE], wake_b[EKO_MAX_WAKE];
    uint32_t *wake_cur, *wake_run; int nwake_cur, nwake_run;
    uint32_t *stamp;        /* per object: frame last executed */
    uint32_t *msgmask;      /* per object: message flag bits (0x5d054c) */
    uint32_t *changed_vol;  int nchanged_vol;   /* volumes touched this frame */
    uint32_t *changed_col;  int nchanged_col;
    uint32_t *vol_stamp, *col_stamp;

    EkoActorEntry actor_pool[EKO_MAX_ACTORS]; EkoActorEntry *actor_free;
    uint32_t last_perso_vol, last_perso_col;   /* 0x5d6364 / 0x5d6368 */
    int warned_cut;

    /* statistics (0x5d03f0..0x5d0410) */
    uint32_t stat_objects_run, stat_objects_total, stat_msgs_out, stat_msgs_total,
             stat_delays_run, stat_durings_run;

    /* host hooks */
    EkoMsgCallback on_msg; EkoWarnCallback on_warn; void *user;
    int discard_msgs;                  /* set during the first init pass: the exe throws those messages away */
    int defer_msgs;                    /* set during the whole init: the exe only hands the queue (0x5bd300) to the game AFTER the tick */
};

/* lifecycle */
int  eko_load(EkoVM *vm, const void *data, size_t size);   /* 0 on success */
void eko_free(EkoVM *vm);
void eko_init(EkoVM *vm);                 /* 0x4427e0: two passes, first pass' messages discarded */
int  eko_tick(EkoVM *vm, int32_t time);   /* 0x444050 + 0x442240; returns number of objects run */
void eko_run(EkoVM *vm, uint32_t pc);     /* 0x4429f0 */

/* engine -> VM (callback ids 100..103 and collision variants) */
void eko_set_var(EkoVM *vm, uint32_t var, int32_t value);          /* 100 / 0x443ca0 */
void eko_vol_enter(EkoVM *vm, uint32_t vol, uint32_t actor);       /* 101 / 0x4443f0 */
void eko_vol_leave(EkoVM *vm, uint32_t vol, uint32_t actor);       /* 102 / 0x444470 */
void eko_vol_in(EkoVM *vm, uint32_t vol, uint32_t actor);          /* 103 / 0x444430 */
void eko_vol_perso_enter(EkoVM *vm, uint32_t vol, uint32_t actor); /* 0x4444e0 */
void eko_vol_perso_in(EkoVM *vm, uint32_t vol, uint32_t actor);    /* 0x444530 */
void eko_vol_perso_leave(EkoVM *vm, uint32_t vol, uint32_t actor); /* 0x444550 */
void eko_col_press(EkoVM *vm, uint32_t col, uint32_t actor);       /* 0x4445e0 */
void eko_col_in(EkoVM *vm, uint32_t col, uint32_t actor);          /* 0x444610 */
void eko_col_unpress(EkoVM *vm, uint32_t col, uint32_t actor);     /* 0x444650 */
void eko_col_perso_press(EkoVM *vm, uint32_t col, uint32_t actor); /* 0x444690 */
void eko_col_perso_in(EkoVM *vm, uint32_t col, uint32_t actor);    /* 0x4446d0 */
void eko_col_perso_unpress(EkoVM *vm, uint32_t col, uint32_t actor);/* 0x4446f0 */
void eko_msgmask_set(EkoVM *vm, uint32_t obj, uint32_t bits);      /* 0x443e50 */
void eko_msgmask_clear(EkoVM *vm, uint32_t obj, uint32_t bits);    /* 0x443e90 */
int  eko_msgmask_test(EkoVM *vm, uint32_t obj, uint32_t bits);     /* 0x443ed0 */
int  eko_vol_has_actor_f1(EkoVM *vm, uint32_t vol, uint32_t actor);/* 0x443e20 */
void eko_actor_leave_all(EkoVM *vm, uint32_t actor);               /* 0x443ff0 */
void eko_cancel_timers(EkoVM *vm, uint32_t obj);                   /* 0x444380 + 0x4441d0 */
uint32_t eko_obj_of(const EkoVM *vm, uint32_t pc);                 /* 0x444060 */

/* outgoing queue access (engine drains after each tick: 0x441d20/0x441d30/0x441d40) */
static inline int eko_msg_count(const EkoVM *vm) { return vm->nmsgs; }
static inline void eko_msg_reset(EkoVM *vm) { vm->nmsgs = 0; }

extern const char *eko_opcode_names[64];
#endif
