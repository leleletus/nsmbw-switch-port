// r10b (8-nov-2026): fast C++ operator new/delete for the Switch.
//
// Measured on the real console (tools/nxbench): newlib malloc+free = ~420
// cycles, plus its global recursive lock (mutexLock/Unlock ~78 cycles). The
// render thread creates and destroys tens of thousands of small objects per
// frame (GX commands, vertex vectors, shared_ptr control blocks): on the real
// Switch _malloc_r + _free_r + their lock were ~12 % of the render thread.
//
// Small blocks (<= 1024 bytes) come from per-thread free lists, one per 16-byte
// size class, refilled from newlib in slabs and never given back (bounded by
// the peak use of each class). Each block carries a 16-byte header (class +
// magic), so a block freed by another thread simply joins that thread's list,
// and unsized deletes work. Larger blocks and over-aligned new go to newlib,
// with the same header. Only C++ new/delete change: malloc/free (Mesa, newlib,
// C code) are untouched. Linked in through nx_fast_new_link() (switch_platform).
#ifdef __SWITCH__
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <malloc.h>
#include <new>
#include <atomic>
#include <switch.h>  // r38c: svcSleepThread

namespace {

constexpr size_t kAlign = 16;
constexpr size_t kMaxSmall = 1024;
constexpr uint32_t kClasses = kMaxSmall / kAlign;  // 64 classes: 16, 32, ... 1024 bytes
constexpr uint32_t kSlabBytes = 16 * 1024;         // refill granularity per class
constexpr uint32_t kMagicSmall = 0x5AFEC0DEu;
constexpr uint32_t kMagicBig = 0xB16B00B5u;
constexpr uint32_t kMagicAligned = 0xA11C0DE5u;

struct alignas(16) Header {
  uint32_t cls;    // size class (1..kClasses) of a small block
  uint32_t magic;  // which path allocated the block
  void *raw;       // aligned path: the pointer to give back to free();
                   // small blocks: the Cache of the thread that allocated it
                   // (a tag only, never dereferenced)
};
static_assert(sizeof(Header) == 16, "the header must keep 16-byte alignment");

struct FreeNode {
  FreeNode *next;
};

struct Cache {
  FreeNode *head[kClasses + 1];
};
// Zero-initialised TLS (.tbss): usable before any constructor runs.
thread_local Cache t_cache;

// r13 fix (8-nov, real-console crash after 37 min: operator new -> bad_alloc ->
// abort): with the render worker one thread allocates GX commands and another
// frees them. Each free went to the FREEING thread's list, which never
// allocates those sizes, while the allocating thread kept carving new slabs:
// unbounded growth until the heap ran out. Now a block freed by a thread other
// than the one that allocated it goes to a shared per-class list (spinlock),
// and a thread whose own list is empty takes the whole shared list before
// carving a new slab.
struct Shared {
  std::atomic_flag lock = ATOMIC_FLAG_INIT;
  FreeNode *head = nullptr;
};
Shared g_shared[kClasses + 1];

inline void shared_lock(Shared &sh) {
  // r38c: giro puro sin ceder: si el que lo tiene es un hilo de menos prioridad del MISMO nucleo (p. ej. el
  // trabajador de render 0x2D bajo la CPU de sonido 0x2C en el nucleo 2), en Horizon no vuelve a correr nunca.
  // Tras un giro corto se duerme 10 us (cede el nucleo de verdad).
  for (unsigned spins = 0; sh.lock.test_and_set(std::memory_order_acquire);)
    if (++spins > 4096) svcSleepThread(10000);
}
inline void shared_unlock(Shared &sh) { sh.lock.clear(std::memory_order_release); }

__attribute__((noinline)) void *refill(Cache &tc, uint32_t c) {
  // Blocks other threads gave back first.
  Shared &sh = g_shared[c];
  if (__atomic_load_n(&sh.head, __ATOMIC_RELAXED)) {
    shared_lock(sh);
    FreeNode *list = sh.head;
    sh.head = nullptr;
    shared_unlock(sh);
    if (list) {
      tc.head[c] = list->next;
      ((Header *)list - 1)->raw = &tc;
      return list;
    }
  }
  const size_t stride = sizeof(Header) + (size_t)c * kAlign;
  size_t n = kSlabBytes / stride;
  if (n < 4) n = 4;
  char *slab = (char *)std::malloc(stride * n);
  if (!slab) return nullptr;
  // Block 0 is returned; 1..n-1 go to the free list.
  for (size_t i = 0; i < n; ++i) {
    Header *h = (Header *)(slab + i * stride);
    h->cls = c;
    h->magic = kMagicSmall;
    h->raw = nullptr;
    if (i) {
      FreeNode *f = (FreeNode *)(h + 1);
      f->next = tc.head[c];
      tc.head[c] = f;
    }
  }
  ((Header *)slab)->raw = &tc;
  return (Header *)slab + 1;
}

inline void *fast_alloc(size_t n) {
  if (n == 0) n = 1;
  if (__builtin_expect(n <= kMaxSmall, 1)) {
    const uint32_t c = (uint32_t)((n + kAlign - 1) / kAlign);
    Cache &tc = t_cache;
    if (FreeNode *f = tc.head[c]) {
      tc.head[c] = f->next;
      ((Header *)f - 1)->raw = &tc;  // owner = this thread
      return f;
    }
    return refill(tc, c);
  }
  Header *h = (Header *)std::malloc(n + sizeof(Header));
  if (!h) return nullptr;
  h->cls = 0;
  h->magic = kMagicBig;
  h->raw = nullptr;
  return h + 1;
}

inline void *fast_alloc_aligned(size_t n, size_t al) {
  if (al <= kAlign) return fast_alloc(n);
  // Room for the header right before an aligned payload.
  char *raw = (char *)memalign(al, n + al + sizeof(Header));
  if (!raw) return nullptr;
  char *p = raw + al;  // al >= 32 > sizeof(Header): the header fits before p
  Header *h = (Header *)p - 1;
  h->cls = 0;
  h->magic = kMagicAligned;
  h->raw = raw;
  return p;
}

inline void fast_free(void *p) {
  if (!p) return;
  Header *h = (Header *)p - 1;
  if (__builtin_expect(h->magic == kMagicSmall, 1)) {
    Cache &tc = t_cache;
    FreeNode *f = (FreeNode *)p;
    if (__builtin_expect(h->raw == &tc, 1)) {
      f->next = tc.head[h->cls];
      tc.head[h->cls] = f;
      return;
    }
    // Allocated by another thread: back to the shared list of its class.
    Shared &sh = g_shared[h->cls];
    shared_lock(sh);
    f->next = sh.head;
    sh.head = f;
    shared_unlock(sh);
    return;
  }
  if (h->magic == kMagicAligned) {
    std::free(h->raw);
    return;
  }
  std::free(h);  // kMagicBig
}

}  // namespace

namespace nwii::runtime::nx {
// Referenced from early_init so the archive member (and with it these
// operator new/delete definitions) is always linked.
void nx_fast_new_link() {}
}  // namespace nwii::runtime::nx

void *operator new(size_t n) {
  if (void *p = fast_alloc(n)) return p;
  throw std::bad_alloc();
}
void *operator new[](size_t n) {
  if (void *p = fast_alloc(n)) return p;
  throw std::bad_alloc();
}
void *operator new(size_t n, const std::nothrow_t &) noexcept { return fast_alloc(n); }
void *operator new[](size_t n, const std::nothrow_t &) noexcept { return fast_alloc(n); }
void *operator new(size_t n, std::align_val_t al) {
  if (void *p = fast_alloc_aligned(n, (size_t)al)) return p;
  throw std::bad_alloc();
}
void *operator new[](size_t n, std::align_val_t al) {
  if (void *p = fast_alloc_aligned(n, (size_t)al)) return p;
  throw std::bad_alloc();
}
void *operator new(size_t n, std::align_val_t al, const std::nothrow_t &) noexcept {
  return fast_alloc_aligned(n, (size_t)al);
}
void *operator new[](size_t n, std::align_val_t al, const std::nothrow_t &) noexcept {
  return fast_alloc_aligned(n, (size_t)al);
}

void operator delete(void *p) noexcept { fast_free(p); }
void operator delete[](void *p) noexcept { fast_free(p); }
void operator delete(void *p, size_t) noexcept { fast_free(p); }
void operator delete[](void *p, size_t) noexcept { fast_free(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { fast_free(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { fast_free(p); }
void operator delete(void *p, std::align_val_t) noexcept { fast_free(p); }
void operator delete[](void *p, std::align_val_t) noexcept { fast_free(p); }
void operator delete(void *p, size_t, std::align_val_t) noexcept { fast_free(p); }
void operator delete[](void *p, size_t, std::align_val_t) noexcept { fast_free(p); }
#endif
