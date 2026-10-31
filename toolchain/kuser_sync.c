/* __sync_* atomics for ARMv5 (no ldrex/strex) on Linux, via the kernel
 * user helpers — what libgcc's linux-atomic.c does; zig's compiler_rt lacks them. */
typedef int (kuser_cmpxchg_t)(int oldval, int newval, volatile int *ptr);
typedef void (kuser_dmb_t)(void);
#define KCAS (*(kuser_cmpxchg_t *)0xffff0fc0)
#define KDMB (*(kuser_dmb_t *)0xffff0fa0)

enum { ADD, SUB, AND, OR, XOR, NAND, UMAX, UMIN, XCHG };

static unsigned apply(int op, unsigned a, unsigned b) {
  switch (op) {
  case ADD: return a + b;  case SUB: return a - b;
  case AND: return a & b;  case OR:  return a | b;
  case XOR: return a ^ b;  case NAND: return ~(a & b);
  case UMAX: return a > b ? a : b; case UMIN: return a < b ? a : b;
  default: return b;
  }
}

/* Read-modify-write `size` bytes at p (little-endian, naturally aligned). */
static unsigned rmw(volatile void *p, unsigned v, int op, int size) {
  volatile int *w = (volatile int *)((unsigned long)p & ~3UL);
  unsigned shift = ((unsigned long)p & 3UL) * 8;
  unsigned mask = size == 4 ? 0xffffffffu : (((1u << (size * 8)) - 1) << shift);
  unsigned vmask = size == 4 ? 0xffffffffu : ((1u << (size * 8)) - 1);
  for (;;) {
    unsigned oldw = (unsigned)*w;
    unsigned oldv = (oldw & mask) >> shift;
    unsigned newv = apply(op, oldv, v & vmask) & vmask;
    unsigned neww = (oldw & ~mask) | (newv << shift);
    if (KCAS((int)oldw, (int)neww, w) == 0) return oldv;
  }
}

static unsigned cas(volatile void *p, unsigned expect, unsigned desired, int size) {
  volatile int *w = (volatile int *)((unsigned long)p & ~3UL);
  unsigned shift = ((unsigned long)p & 3UL) * 8;
  unsigned vmask = size == 4 ? 0xffffffffu : ((1u << (size * 8)) - 1);
  unsigned mask = vmask << shift;
  for (;;) {
    unsigned oldw = (unsigned)*w;
    unsigned oldv = (oldw & mask) >> shift;
    if (oldv != (expect & vmask)) return oldv;
    unsigned neww = (oldw & ~mask) | ((desired & vmask) << shift);
    if (KCAS((int)oldw, (int)neww, w) == 0) return oldv;
  }
}

#define FETCH(name, op, sz) \
  unsigned name##_##sz(volatile void *p, unsigned v) __asm__("__sync_" #name "_" #sz); \
  unsigned name##_##sz(volatile void *p, unsigned v) { return rmw(p, v, op, sz); }
#define ALL(name, op) FETCH(name, op, 1) FETCH(name, op, 2) FETCH(name, op, 4)
ALL(fetch_and_add, ADD) ALL(fetch_and_sub, SUB) ALL(fetch_and_and, AND)
ALL(fetch_and_or, OR) ALL(fetch_and_xor, XOR) ALL(fetch_and_nand, NAND)
ALL(fetch_and_umax, UMAX) ALL(fetch_and_umin, UMIN) ALL(lock_test_and_set, XCHG)

#define CAS(sz) \
  unsigned vcas_##sz(volatile void *p, unsigned e, unsigned d) __asm__("__sync_val_compare_and_swap_" #sz); \
  unsigned vcas_##sz(volatile void *p, unsigned e, unsigned d) { return cas(p, e, d, sz); }
CAS(1) CAS(2) CAS(4)

void sync_synchronize(void) __asm__("__sync_synchronize");
void sync_synchronize(void) { KDMB(); }

/* 64-bit: __kuser_cmpxchg64 only exists from Linux 3.1 (this kernel is 2.6.28),
 * so serialize through one spinlock built on the 32-bit helper (single core). */
static volatile int lock64;
static void lk(void) { while (KCAS(0, 1, &lock64) != 0) { } }
static void ulk(void) { KDMB(); lock64 = 0; }
typedef unsigned long long u64;
static u64 rmw8(volatile u64 *p, u64 v, int op) {
  lk(); u64 o = *p, n;
  switch (op) { case ADD: n = o + v; break; case SUB: n = o - v; break;
    case AND: n = o & v; break; case OR: n = o | v; break; case XOR: n = o ^ v; break;
    case NAND: n = ~(o & v); break; case UMAX: n = o > v ? o : v; break;
    case UMIN: n = o < v ? o : v; break; default: n = v; }
  *p = n; ulk(); return o;
}
#define FETCH8(name, op) \
  u64 name##_8(volatile u64 *p, u64 v) __asm__("__sync_" #name "_8"); \
  u64 name##_8(volatile u64 *p, u64 v) { return rmw8(p, v, op); }
FETCH8(fetch_and_add, ADD) FETCH8(fetch_and_sub, SUB) FETCH8(fetch_and_and, AND)
FETCH8(fetch_and_or, OR) FETCH8(fetch_and_xor, XOR) FETCH8(fetch_and_nand, NAND)
FETCH8(fetch_and_umax, UMAX) FETCH8(fetch_and_umin, UMIN) FETCH8(lock_test_and_set, XCHG)
u64 vcas_8(volatile u64 *p, u64 e, u64 d) __asm__("__sync_val_compare_and_swap_8");
u64 vcas_8(volatile u64 *p, u64 e, u64 d) { lk(); u64 o = *p; if (o == e) *p = d; ulk(); return o; }

/* ffmpeg 7.1 aac_fixed references USAC (xHE-AAC) state reset that is only built
 * with the float decoder. Podcasts don't use USAC: report unsupported. */
int ff_aac_usac_reset_state(void *ac, void *oc) { (void)ac; (void)oc; return -1163346256; /* AVERROR_PATCHWELCOME */ }
