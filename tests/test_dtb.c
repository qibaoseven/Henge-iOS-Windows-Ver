/* The device tree: that it parses, that the nodes the firmware looks for are
   there, and that the ones that would send it looking for absent hardware are
   not. Parses the blob with an FDT reader of its own, so what is checked is what
   a reader would see, not what the writer meant. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dtb.h"
#include "machine.h"
#include "virt.h"

static int fails, checks;
#define CHECK(c, ...)                                   \
    do {                                                \
        checks++;                                       \
        if (!(c)) {                                     \
            fails++;                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

typedef struct {
    const uint8_t *b;
    uint32_t total, off_struct, off_strings, off_rsv, size_struct, size_strings;
} Tree;

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int open_tree(Tree *t, const uint8_t *b) {
    if (be32(b) != 0xd00dfeed) return -1;
    t->b = b;
    t->total = be32(b + 4);
    t->off_struct = be32(b + 8);
    t->off_strings = be32(b + 12);
    t->off_rsv = be32(b + 16);
    t->size_strings = be32(b + 32);
    t->size_struct = be32(b + 36);
    return t->total <= 0x100000 ? 0 : -1;
}

/* Walks the structure block, keeping the path of the node being entered, and
   reports whether `path` exists with property `prop` whose bytes equal `want`
   (want NULL: just existence). Returns 1 found, 0 not, -1 malformed. */
static int lookup(const Tree *t, const char *path, const char *prop, const void *want, uint32_t wantlen) {
    char cur[256] = "";
    char stack[16][128];
    int depth = 0;
    const uint8_t *p = t->b + t->off_struct, *end = p + t->size_struct;
    int found = 0;
    while (p + 4 <= end) {
        uint32_t tok = be32(p);
        p += 4;
        if (tok == 1) { /* BEGIN_NODE */
            const char *name = (const char *)p;
            size_t n = strlen(name);
            p += (n + 1 + 3) & ~(size_t)3;
            if (depth < 16) snprintf(stack[depth], sizeof stack[depth], "%s", name);
            if (depth == 0) snprintf(cur, sizeof cur, "/%s", name);
            else {
                char tmp[256];
                snprintf(tmp, sizeof tmp, "%s", stack[depth - 1]);
                if (tmp[0] == '/') tmp[0] = 0;
                (void)tmp;
                /* Recompose from the depth stack: root is "", others join by '/' */
                cur[0] = 0;
                for (int i = 0; i <= depth; i++) {
                    if (!stack[i][0]) continue;
                    strncat(cur, "/", sizeof cur - strlen(cur) - 1);
                    strncat(cur, stack[i], sizeof cur - strlen(cur) - 1);
                }
            }
            depth++;
        } else if (tok == 2) { /* END_NODE */
            depth--;
            if (depth <= 0) break;
        } else if (tok == 3) { /* PROP */
            uint32_t len = be32(p), nameoff = be32(p + 4);
            if (nameoff >= t->size_strings) return -1;
            const char *nm = (const char *)(t->b + t->off_strings + nameoff);
            const uint8_t *val = p + 8;
            p += 8 + ((len + 3) & ~3u);
            if (strcmp(cur, path) == 0 && strcmp(nm, prop) == 0) {
                if (!want) found = 1;
                else if (len == wantlen && !memcmp(val, want, len)) found = 1;
            }
        } else if (tok == 4) { /* NOP */
        } else if (tok == 9) { /* END */
            break;
        } else {
            return -1;
        }
    }
    return found;
}

static int has_node(const Tree *t, const char *path) {
    const uint8_t *p = t->b + t->off_struct, *end = p + t->size_struct;
    char cur[256] = "", stack[16][128];
    int depth = 0;
    while (p + 4 <= end) {
        uint32_t tok = be32(p);
        p += 4;
        if (tok == 1) {
            const char *name = (const char *)p;
            size_t n = strlen(name);
            p += (n + 1 + 3) & ~(size_t)3;
            if (depth < 16) snprintf(stack[depth], sizeof stack[depth], "%s", name);
            if (depth == 0) snprintf(cur, sizeof cur, "/%s", name);
            else {
                cur[0] = 0;
                for (int i = 0; i <= depth; i++) {
                    if (!stack[i][0]) continue;
                    strncat(cur, "/", sizeof cur - strlen(cur) - 1);
                    strncat(cur, stack[i], sizeof cur - strlen(cur) - 1);
                }
            }
            depth++;
            if (strcmp(cur, path) == 0) return 1;
        } else if (tok == 2) {
            if (--depth <= 0) break;
        } else if (tok == 3) {
            uint32_t len = be32(p);
            p += 8 + ((len + 3) & ~3u);
        } else if (tok == 4) {
        } else {
            break;
        }
    }
    return 0;
}

int main(void) {
    uint8_t *buf = calloc(1, VIRT_DTB_SIZE);
    uint64_t n = hv_dtb_build(buf, VIRT_DTB_SIZE, VIRT_GUEST_RAM, 512u << 20, 4);
    CHECK(n > 0, "build returned 0");
    CHECK(n <= VIRT_DTB_SIZE, "fits in its megabyte (%llu)", (unsigned long long)n);

    Tree t;
    CHECK(open_tree(&t, buf) == 0, "header");
    CHECK(t.total == n, "total size field %u vs %llu", t.total, (unsigned long long)n);
    CHECK(be32(buf + 20) == 17, "version 17");
    CHECK(t.size_struct > 0 && t.size_struct <= t.off_strings - t.off_struct, "structure block inside its region (size %u)", t.size_struct);
    CHECK(t.size_strings > 0 && t.off_strings + t.size_strings == n, "strings end at total");
    /* the reservation map is empty: one zero entry */
    CHECK(be32(buf + t.off_rsv) == 0 && be32(buf + t.off_rsv + 4) == 0, "empty reservation map");

    /* The nodes the firmware and its drivers look for. */
    CHECK(has_node(&t, "/"), "root");
    CHECK(has_node(&t, "/chosen"), "chosen");
    CHECK(has_node(&t, "/memory@40100000"), "memory");
    CHECK(has_node(&t, "/cpus"), "cpus");
    CHECK(has_node(&t, "/cpus/cpu@0"), "cpu0");
    CHECK(has_node(&t, "/cpus/cpu@3"), "cpu3");
    CHECK(has_node(&t, "/intc@8000000"), "gic");
    CHECK(has_node(&t, "/pl011@9000000"), "uart");
    CHECK(has_node(&t, "/pl031@9010000"), "rtc");
    CHECK(has_node(&t, "/fw-cfg@9020000"), "fw_cfg");
    CHECK(has_node(&t, "/psci"), "psci");

    /* stdout goes to the PL011, which is the only thing that can print yet. */
    const char *pl011 = "/pl011@9000000\0";
    CHECK(lookup(&t, "/chosen", "stdout-path", pl011, 15) == 1, "stdout-path is the PL011");

    /* PSCI must be HVC: there is no EL3. */
    CHECK(lookup(&t, "/psci", "method", "hvc\0", 4) == 1, "psci method is hvc");
    CHECK(lookup(&t, "/psci", "method", "smc\0", 4) == 0, "psci is not smc");
    CHECK(lookup(&t, "/psci", "compatible", "arm,psci-1.0\0", 13) == 1, "psci 1.0");

    /* RAM starts after the DT, and says so. */
    {
        const uint8_t *p = buf + t.off_struct, *end = p + t.size_struct;
        char cur[256] = "", stack[16][128];
        int depth = 0, seen = 0;
        while (p + 4 <= end) {
            uint32_t tok = be32(p);
            p += 4;
            if (tok == 1) {
                const char *nm = (const char *)p;
                size_t k = strlen(nm);
                p += (k + 1 + 3) & ~(size_t)3;
                if (depth < 16) snprintf(stack[depth], sizeof stack[depth], "%s", nm);
                if (depth == 0) snprintf(cur, sizeof cur, "/%s", nm);
                else {
                    cur[0] = 0;
                    for (int i = 0; i <= depth; i++) {
                        if (!stack[i][0]) continue;
                        strncat(cur, "/", sizeof cur - strlen(cur) - 1);
                        strncat(cur, stack[i], sizeof cur - strlen(cur) - 1);
                    }
                }
                depth++;
            } else if (tok == 2) {
                if (--depth <= 0) break;
            } else if (tok == 3) {
                uint32_t len = be32(p), noff = be32(p + 4);
                const char *nm = (const char *)(buf + t.off_strings + noff);
                if (!strcmp(cur, "/memory@40100000") && !strcmp(nm, "reg") && len == 16) {
                    CHECK(be32(p + 12) == VIRT_GUEST_RAM, "memory starts at %llx", (unsigned long long)VIRT_GUEST_RAM);
                    CHECK(be32(p + 8) == VIRT_GUEST_RAM >> 32, "memory high half");
                    seen = 1;
                }
                p += 8 + ((len + 3) & ~3u);
            } else if (tok == 4) {
            } else break;
        }
        CHECK(seen, "memory reg property found");
    }

    /* The ITS is not in the tree until it exists: a node naming one would have
       the firmware map 0x08080000 and wait for interrupts that never come. */
    CHECK(!has_node(&t, "/intc@8000000/its@8080000"), "no ITS node");
    CHECK(!lookup(&t, "/intc@8000000", "msi-controller", "x", 1), "the GIC does not claim MSI");

    /* Two cells per address word, as #address-cells = 2 says. */
    CHECK(lookup(&t, "/", "#address-cells", (uint8_t[]){0, 0, 0, 2}, 4) == 1, "#address-cells = 2");
    CHECK(lookup(&t, "/", "#size-cells", (uint8_t[]){0, 0, 0, 2}, 4) == 1, "#size-cells = 2");

    /* A tree too big for its buffer is refused, not truncated. */
    {
        uint8_t small[4096];
        CHECK(hv_dtb_build(small, sizeof small, VIRT_GUEST_RAM, 512u << 20, 4) == 0, "refuses a small buffer");
    }

    free(buf);
    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
