/* Red-zone protection (src/isa_rewrite.c isa_red_zone_*, scripts/red_zone.py): the fixture's
 * functions keep a value in the red zone across a store that faults on a write-protected page. The
 * fault handler does what Windows' exception dispatch does: it zeroes the bytes below the faulting
 * rsp (here 2048, on an alternate signal stack), then lets the store run. Unprotected, the value is
 * lost; with the patch groups applied every function returns it, the fault comes from inside a stub
 * (isa_red_zone_site_of_stub names the site) and the registers and flags come out right.
 * Usage: red-zone-test tests/data/red_zone_image.bin tests/data/red_zone_table.bin */
#define _GNU_SOURCE
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include "isa_rewrite.h"

typedef uint64_t (ABI *Function)(uint32_t *page, uint64_t value);
enum { PAGE = 4096, IMAGE = 2 * PAGE, DATA = PAGE, INDEX = 0xf00, CLOBBER = 2048, AREA = 4 * PAGE };

static unsigned char *region, *pristine;
static IsaTable *table;
static volatile int faults, in_stub;
static volatile uint64_t stub_site;

static void handler(int sig, siginfo_t *info, void *context) {
    (void)sig;
    ucontext_t *uc = context;
    unsigned char *page = region + DATA;
    assert((unsigned char *)info->si_addr >= page && (unsigned char *)info->si_addr < page + PAGE);
    uintptr_t rip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP], rsp = (uintptr_t)uc->uc_mcontext.gregs[REG_RSP];
    uint64_t site = 0;
    in_stub = isa_red_zone_site_of_stub(table, rip, &site);
    stub_site = site;
    memset((void *)(rsp - CLOBBER), 0, CLOBBER);     /* Windows writes its exception frame here */
    assert(mprotect(page, PAGE, PROT_READ | PROT_WRITE) == 0);
    faults++;
}

static uint32_t index_count(void) { uint32_t n; memcpy(&n, pristine + INDEX, 4); return n; }
static void entry(uint32_t i, uint32_t *function, uint32_t *fault) {
    memcpy(function, pristine + INDEX + 4 + 8 * i, 4);
    memcpy(fault, pristine + INDEX + 8 + 8 * i, 4);
}

/* The value each fixture function returns (tests/test_red_zone.py fixture_functions). */
static uint64_t expected(uint32_t i, uint64_t value) {
    return i == 4 ? value + ((uint32_t)value < 5) : value;   /* "flags": adc rax, 0 after cmp esi, 5 */
}

/* Calls function i with the data page write-protected; returns its result. The call itself steps
 * over this frame's red zone (the clobber reaches 2 KiB below the guest rsp). */
static uint64_t call(uint32_t i, uint64_t value) {
    uint32_t function, fault;
    entry(i, &function, &fault);
    assert(mprotect(region + DATA, PAGE, PROT_READ) == 0);
    faults = in_stub = 0;
    uint64_t result = ((Function)(void *)(region + function))((uint32_t *)(void *)(region + DATA + 0x40), value);
    assert(mprotect(region + DATA, PAGE, PROT_READ | PROT_WRITE) == 0);
    return result;
}

static void load_image(const char *path) {
    FILE *f = fopen(path, "rb");
    assert(f != NULL);
    assert(fread(pristine, 1, IMAGE, f) == IMAGE && fgetc(f) == EOF);
    fclose(f);
    memcpy(region, pristine, IMAGE);
}

static void test_unprotected_loses_the_value(void) {
    memcpy(region, pristine, IMAGE);
    uint64_t r = call(0, 0x1122334455667788ull);
    assert(faults == 1 && !in_stub);
    assert(r == 0);                                 /* the red-zone slot was zeroed under the store */
}

static void test_protected_functions(void) {
    memcpy(region, pristine, IMAGE);
    IsaRedZoneReport z = isa_red_zone_apply(table, region, region + table->rz_code_offset);
    assert(z.groups == 9 && z.protected_instructions == 9 && z.short_relays == 2 && z.skipped == 0);
    for (uint32_t i = 0; i < index_count(); i++) {
        uint32_t function, fault;
        entry(i, &function, &fault);
        static const uint64_t values[] = {0x1122334455667788ull, 3, 0xffffffffull, 0x8000000000000001ull};
        for (size_t k = 0; k < sizeof values / sizeof values[0]; k++) {
            uint32_t *slot = (uint32_t *)(void *)(region + DATA + 0x40);
            *slot = 0;
            uint64_t r = call(i, values[k]);
            assert(r == expected(i, values[k]));
            int stores = !(i == 2 && (uint32_t)values[k] == 0) && !(i >= 8 && (uint32_t)values[k] == 0xffffffffu);
            if (i == 3) {                                /* "rip": mov dword [rip + data+0x10], 1 */
                assert(faults == 1 && *(uint32_t *)(void *)(region + DATA + 0x10) == 1);
            } else {
                assert(faults == stores);
                if (stores) assert(*slot == (i == 0 ? 1u : (uint32_t)values[k]) || i == 5);
            }
            if (faults && fault) assert(in_stub && stub_site == fault);
            if (i == 7) assert(!in_stub);                  /* "dead": nothing live, not patched */
        }
    }
}

static void test_changed_bytes_skip_the_group(void) {
    memcpy(region, pristine, IMAGE);
    uint32_t function, fault;
    entry(1, &function, &fault);
    region[fault + 1] ^= 0x01;                         /* a game patch touched the "forward" site */
    IsaRedZoneReport z = isa_red_zone_apply(table, region, region + table->rz_code_offset);
    assert(z.groups == 8 && z.skipped == 1);
    assert(region[fault] == pristine[fault]);
}

static void test_misplaced_area_is_refused(void) {
    memcpy(region, pristine, IMAGE);
    IsaRedZoneReport z = isa_red_zone_apply(table, region, region + table->rz_code_offset + PAGE);
    assert(z.groups == 0 && z.skipped == 9);
    assert(memcmp(region, pristine, IMAGE) == 0);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: red-zone-test red_zone_image.bin red_zone_table.bin\n");
        return 2;
    }
    region = mmap(NULL, IMAGE + AREA, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    pristine = malloc(IMAGE);
    assert(region != MAP_FAILED && pristine);
    load_image(argv[1]);
    table = isa_table_load(argv[2]);
    assert(table && table->rz && table->rz_code_offset == IMAGE && table->rz_code_bytes <= AREA);
    assert(table->rz_stats[4] == 9 && table->rz_stats[6] == 2);   /* protected, short_relays */
    stack_t ss = {.ss_sp = malloc(1 << 16), .ss_size = 1 << 16};
    assert(ss.ss_sp && sigaltstack(&ss, NULL) == 0);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    assert(sigaction(SIGSEGV, &sa, NULL) == 0);
    test_unprotected_loses_the_value();
    test_protected_functions();
    test_changed_bytes_skip_the_group();
    test_misplaced_area_is_refused();
    puts("red-zone-test ok");
    return 0;
}
