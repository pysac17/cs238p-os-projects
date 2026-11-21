/**
 * Tony Givargis
 * Copyright (C), 2023-2026
 * University of California, Irvine
 *
 * CS 238P - Operating Systems
 * scheduler.c
 */
#define _POSIX_C_SOURCE 200809L
#undef _FORTIFY_SOURCE
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/time.h>
#include "system.h"
#include "scheduler.h"

#if !defined(__linux__) || !defined(__x86_64__)
#error "Build in the x86-64 Linux VM."
#endif

#ifdef USE_VALGRIND
#include <valgrind/valgrind.h>
#else
#define VALGRIND_STACK_REGISTER(a, b) 0U
#define VALGRIND_STACK_DEREGISTER(id) ((void)(id))
#endif

#define STACK_BYTES (1024UL * 1024UL)
enum thread_state { STATE_INIT, STATE_RUNNING, STATE_RUNNABLE, STATE_TERMINATED };
struct thread {
    sigjmp_buf context;
    scheduler_fnc_t function;
    void *argument;
    void *stack;
    void *stack_top;
    unsigned int valgrind_stack;
    enum thread_state state;
    int saved_errno;
    struct thread *next;
};
static struct thread *head, *tail, *current;
static sigjmp_buf dispatcher;
static sigset_t alarm_set, user_mask;
static volatile sig_atomic_t executing;
static size_t remaining;

static void die(const char *message);
static void block_timer(sigset_t *previous);
static void restore_mask(const sigset_t *mask);
static void timer_handler(int signal_number);
static void thread_entry(void) __attribute__((used, noreturn));

/* 1: create a record and a separate aligned stack. */
int scheduler_create(scheduler_fnc_t function, void *argument)
{
    struct thread *thread;
    sigset_t previous;
    size_t page;
    int error;
    if (!function) { errno = EINVAL; return -1; }
    block_timer(&previous);
    page = page_size();
    thread = calloc(1, sizeof *thread);
    if (!thread) { restore_mask(&previous); return -1; }
    thread->stack = malloc(STACK_BYTES + page);
    if (!thread->stack) {
        error = errno;
        free(thread);
        restore_mask(&previous);
        errno = error;
        return -1;
    }
    thread->stack_top = (char *)memory_align(thread->stack, page) + STACK_BYTES;
    thread->valgrind_stack = VALGRIND_STACK_REGISTER(
        memory_align(thread->stack, page), thread->stack_top);
    thread->function = function;
    thread->argument = argument;
    thread->state = STATE_INIT;
    thread->next = head ? head : thread;
    if (tail) tail->next = thread;
    else head = thread;
    tail = thread;
    ++remaining;
    restore_mask(&previous);
    return 0;
}

/* Start on the new stack, aligned to 16 bytes before CALL. */
extern void scheduler_stack_enter(void *top) __attribute__((noreturn));
__asm__(
    ".text\n.globl scheduler_stack_enter\n"
    ".type scheduler_stack_enter,@function\n"
    "scheduler_stack_enter:\n"
    "movq %rdi, %rsp\n"
    "andq $-16, %rsp\n"
    "xorq %rbp, %rbp\n"
    "call thread_entry\n"
    "ud2\n"
    ".size scheduler_stack_enter, .-scheduler_stack_enter\n"
);

/* 2: select, pause, resume, finish, and clean up. */
static struct thread *candidate(void)
{
    struct thread *first, *thread;
    if (!head) return NULL;
    first = current ? current->next : head;
    thread = first;
    do {
        if (thread->state == STATE_INIT || thread->state == STATE_RUNNABLE)
            return thread;
        thread = thread->next;
    } while (thread != first);
    return NULL;
}

static void schedule(void)
{
    struct thread *thread = candidate();
    int first_run;
    if (!thread) { errno = EINVAL; die("no runnable thread"); }
    first_run = thread->state == STATE_INIT;
    current = thread;
    current->state = STATE_RUNNING;
    errno = current->saved_errno;
    if (!first_run) siglongjmp(current->context, 1);
    scheduler_stack_enter(current->stack_top);
}

void scheduler_yield(void)
{
    sigset_t previous;
    int error = errno;
    block_timer(&previous);
    if (executing && current) {
        current->saved_errno = error;
        if (sigsetjmp(current->context, 1) == 0) {
            current->state = STATE_RUNNABLE;
            siglongjmp(dispatcher, 1);
        }
    }
    restore_mask(&previous);
    errno = error;
}

static void thread_entry(void)
{
    errno = current->saved_errno;
    restore_mask(&user_mask);
    current->function(current->argument);
    block_timer(NULL);
    current->state = STATE_TERMINATED;
    --remaining;
    siglongjmp(dispatcher, 1);
}

static void destroy(void)
{
    struct thread *thread, *next;
    tail->next = NULL;
    for (thread = head; thread; thread = next) {
        next = thread->next;
        VALGRIND_STACK_DEREGISTER(thread->valgrind_stack);
        free(thread->stack);
        free(thread);
    }
    head = tail = current = NULL;
    remaining = 0;
}

/* 3: timer setup, automatic switching, and signal protection. */
void scheduler_execute(void)
{
    struct sigaction action = {0}, old_action;
    struct itimerval timer = {0}, old_timer;
    struct timespec no_wait = {0, 0};
    sigset_t previous;
    const char *setting;
    char *end;
    long quantum = 1000000;
    if (executing || !head) return;
    block_timer(&previous);
    if (getitimer(ITIMER_REAL, &old_timer)) die("getitimer");
    if (old_timer.it_value.tv_sec || old_timer.it_value.tv_usec) {
        fprintf(stderr, "scheduler: timer already in use\n");
        restore_mask(&previous);
        return;
    }
    setting = getenv("SCHEDULER_QUANTUM_US");
    if (setting) {
        errno = 0;
        quantum = strtol(setting, &end, 10);
        if (errno || end == setting || *end || quantum < 0 || quantum > 10000000) {
            fprintf(stderr, "scheduler: quantum must be 0..10000000\n");
            restore_mask(&previous);
            return;
        }
    }
    action.sa_handler = timer_handler;
    action.sa_flags = SA_RESTART;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGALRM, &action, &old_action)) die("sigaction");
    user_mask = previous;
    sigdelset(&user_mask, SIGALRM);
    timer.it_interval.tv_sec = quantum / 1000000;
    timer.it_interval.tv_usec = quantum % 1000000;
    timer.it_value = timer.it_interval;
    executing = 1;
    current = NULL;
    if (setitimer(ITIMER_REAL, &timer, NULL)) die("setitimer");
    while (remaining) {
        if (sigsetjmp(dispatcher, 1) != 0 && !remaining) break;
        schedule();
    }
    timer.it_value.tv_sec = timer.it_value.tv_usec = 0;
    if (setitimer(ITIMER_REAL, &timer, NULL)) die("stop timer");
    executing = 0;
    while (sigtimedwait(&alarm_set, NULL, &no_wait) >= 0) { }
    if (sigaction(SIGALRM, &old_action, NULL)) die("restore handler");
    destroy();
    restore_mask(&previous);
}

static void timer_handler(int signal_number)
{
    (void)signal_number;
    scheduler_yield();
}

static void block_timer(sigset_t *previous)
{
    sigemptyset(&alarm_set);
    sigaddset(&alarm_set, SIGALRM);
    if (sigprocmask(SIG_BLOCK, &alarm_set, previous)) die("block timer");
}

static void restore_mask(const sigset_t *mask)
{
    if (sigprocmask(SIG_SETMASK, mask, NULL)) die("restore mask");
}

static void die(const char *message)
{
    perror(message);
    _exit(2);
}
