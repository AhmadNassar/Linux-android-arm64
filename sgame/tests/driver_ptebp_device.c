#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

enum { PAGES = 16, WORKERS = 8 };
static unsigned char *code;
static size_t page_size;
static pthread_t workers[WORKERS];
static unsigned worker_count;
static unsigned active, operation;
static uint64_t totals[WORKERS], wrong[WORKERS];

static char *literal(char *out, const char *value) {
    while (*value) *out++ = *value++;
    return out;
}
static char *hex(char *out, uint64_t value) {
    static const char digits[] = "0123456789abcdef";
    *out++ = '0'; *out++ = 'x';
    for (int shift = 60; shift >= 0; shift -= 4) *out++ = digits[(value >> shift) & 15];
    return out;
}
static void fault(int signal_number, siginfo_t *info, void *context) {
    ucontext_t *uc = context;
    uint64_t esr = 0;
    unsigned char *cursor = uc->uc_mcontext.__reserved;
    unsigned char *end = cursor + sizeof(uc->uc_mcontext.__reserved);
    while (cursor + sizeof(struct _aarch64_ctx) <= end) {
        struct _aarch64_ctx *head = (void *)cursor;
        if (!head->magic || head->size < sizeof(*head) || cursor + head->size > end) break;
        if (head->magic == ESR_MAGIC && head->size >= sizeof(struct esr_context))
            esr = ((struct esr_context *)head)->esr;
        cursor += head->size;
    }
    char output[1024], *out = output;
    out = literal(out, "{\"fault\":true,\"signal\":\""); out = hex(out, (unsigned)signal_number);
    out = literal(out, "\",\"code\":\""); out = hex(out, (unsigned)info->si_code);
    out = literal(out, "\",\"si_addr\":\""); out = hex(out, (uintptr_t)info->si_addr);
    out = literal(out, "\",\"pc\":\""); out = hex(out, uc->uc_mcontext.pc);
    out = literal(out, "\",\"far\":\""); out = hex(out, uc->uc_mcontext.fault_address);
    out = literal(out, "\",\"esr\":\""); out = hex(out, esr);
    out = literal(out, "\",\"x0\":\""); out = hex(out, uc->uc_mcontext.regs[0]);
    out = literal(out, "\",\"x1\":\""); out = hex(out, uc->uc_mcontext.regs[1]);
    out = literal(out, "\",\"lr\":\""); out = hex(out, uc->uc_mcontext.regs[30]);
    out = literal(out, "\",\"pstate\":\""); out = hex(out, uc->uc_mcontext.pstate);
    out = literal(out, "\"}\n");
    (void)write(STDOUT_FILENO, output, (size_t)(out - output));
    _exit(128 + signal_number);
}

__attribute__((noinline)) static uint64_t read_instruction(const void *address) {
    uint64_t value;
    __asm__ volatile("ldr %w0,[%1]" : "=r"(value) : "r"(address) : "memory");
    return value;
}
__attribute__((noinline)) static uint64_t read_exclusive(const void *address) {
    uint64_t value;
    __asm__ volatile("ldxr %w0,[%1]\n\tclrex" : "=r"(value) : "r"(address) : "memory");
    return value;
}
static uint64_t now_ns(void) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000 + (uint64_t)now.tv_nsec;
}
static void *run_worker(void *argument) {
    unsigned index = (unsigned)(uintptr_t)argument;
    uint64_t count = 0, bad = 0, deadline = now_ns() + UINT64_C(8000000000);
    while (__atomic_load_n(&active, __ATOMIC_ACQUIRE)) {
        for (unsigned page = 0; page < PAGES; ++page) {
            unsigned char *address = code + page * page_size;
            if (operation == 3) {
                if (read_exclusive(address) != 0x91001c00U) ++bad;
                continue;
            }
            if (operation != 1) {
                uint64_t (*target)(uint64_t) = (void *)address;
                if (target(5) != 12) ++bad;
            }
            if (operation != 0 && read_instruction(address) != 0x91001c00U) ++bad;
        }
        ++count;
        if (!(count & 255) && now_ns() >= deadline) break;
    }
    totals[index] = count * PAGES;
    wrong[index] = bad;
    return NULL;
}
static void stop_workers(void) {
    __atomic_store_n(&active, 0, __ATOMIC_RELEASE);
    uint64_t total = 0, bad = 0;
    for (unsigned i = 0; i < worker_count; ++i) {
        pthread_join(workers[i], NULL); total += totals[i]; bad += wrong[i];
    }
    if (worker_count) printf("{\"stopped\":true,\"accesses\":%" PRIu64 ",\"bad\":%" PRIu64 "}\n", total, bad);
    worker_count = 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    struct rlimit no_core = {0, 0}; setrlimit(RLIMIT_CORE, &no_core);
    if (argc == 2 && !strcmp(argv[1], "--daemon")) {
        pid_t child = fork();
        if (child < 0) return 1;
        if (child) return 0;
        if (setsid() < 0) return 2;
        for (int fd = 3; fd < 1024; ++fd) close(fd);
    }
    signal(SIGHUP, SIG_IGN);
    struct sigaction action = {0}; action.sa_sigaction = fault; action.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &action, NULL); sigaction(SIGILL, &action, NULL);
    sigaction(SIGBUS, &action, NULL); sigaction(SIGTRAP, &action, NULL);
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    code = mmap(NULL, page_size * PAGES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) return 3;
    for (unsigned i = 0; i < PAGES; ++i) {
        uint32_t *words = (void *)(code + page_size * i);
        words[0] = 0x91001c00U; words[1] = 0xd65f03c0U;
    }
    __builtin___clear_cache((char *)code, (char *)code + page_size * PAGES);
    if (mprotect(code, page_size * PAGES, PROT_READ | PROT_EXEC)) return 4;
    for (unsigned i = 0; i < PAGES; ++i) {
        uint64_t (*target)(uint64_t) = (void *)(code + page_size * i);
        if (target(5) != 12 || read_instruction(code + page_size * i) != 0x91001c00U) return 5;
    }
    unlink("control");
    if (mkfifo("control", 0600)) return 6;
    int descriptor = open("control", O_RDWR | O_CLOEXEC);
    if (descriptor < 0) return 7;
    FILE *commands = fdopen(descriptor, "r");
    if (!commands) return 8;
    printf("{\"ready\":true,\"pid\":%d,\"address\":\"%p\",\"pages\":%u,\"page_size\":%zu,\"read_pc\":\"%p\"}\n", getpid(), code, PAGES, page_size, read_instruction);
    char line[128];
    while (fgets(line, sizeof(line), commands)) {
        if (!strcmp(line, "quit\n")) break;
        if (!strcmp(line, "stop\n")) { stop_workers(); continue; }
        char mode[16]; unsigned count = 0;
        if (sscanf(line, "start %15s %u", mode, &count) == 2) {
            stop_workers();
            if (count < 1 || count > WORKERS) { puts("{\"error\":\"workers\"}"); continue; }
            operation = !strcmp(mode, "exec") ? 0 : !strcmp(mode, "read") ? 1 :
                        !strcmp(mode, "fallback") ? 3 : 2;
            __atomic_store_n(&active, 1, __ATOMIC_RELEASE);
            for (unsigned i = 0; i < count; ++i) {
                int status = pthread_create(&workers[i], NULL, run_worker, (void *)(uintptr_t)i);
                if (status) { printf("{\"pthread_error\":%d}\n", status); break; }
                ++worker_count;
            }
            printf("{\"started\":\"%s\",\"workers\":%u}\n", mode, worker_count); continue;
        }
        if (!strcmp(line, "call\n") || !strcmp(line, "read\n")) {
            unsigned bad = 0;
            for (unsigned i = 0; i < PAGES; ++i) {
                void *address = code + page_size * i;
                if (line[0] == 'c') { uint64_t (*target)(uint64_t) = address; if (target(5) != 12) ++bad; }
                else if (read_instruction(address) != 0x91001c00U) ++bad;
            }
            printf("{\"operation\":\"%s\",\"bad\":%u}\n", line[0] == 'c' ? "call" : "read", bad); continue;
        }
        if (!strcmp(line, "kernelread\n")) {
            int channel[2]; uint32_t value = 0;
            if (pipe(channel)) return 9;
            ssize_t sent = write(channel[1], code, sizeof(value)); int saved = errno;
            ssize_t received = sent > 0 ? read(channel[0], &value, sizeof(value)) : -1;
            close(channel[0]); close(channel[1]);
            printf("{\"operation\":\"kernelread\",\"sent\":%zd,\"received\":%zd,\"value\":\"0x%08x\",\"errno\":%d}\n", sent, received, value, saved); continue;
        }
        if (!strcmp(line, "fallback\n")) {
            uint64_t value = read_exclusive(code);
            printf("{\"operation\":\"fallback\",\"value\":\"0x%08" PRIx64 "\",\"bad\":%u}\n",
                   value, value != 0x91001c00U);
            continue;
        }
        puts("{\"error\":\"command\"}");
    }
    stop_workers(); fclose(commands); unlink("control");
    puts("{\"exit\":\"clean\"}"); return 0;
}
