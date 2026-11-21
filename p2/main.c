/**
 * Tony Givargis
 * Copyright (C), 2023-2026
 * University of California, Irvine
 *
 * CS 238P - Operating Systems
 * main.c
 */
#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <unistd.h>
#include "system.h"
#include "scheduler.h"

/* Protect printf from timer interruption. */
static void print_line(const char *name, int number)
{
    sigset_t alarms, previous;
    sigemptyset(&alarms);
    sigaddset(&alarms, SIGALRM);
    if (sigprocmask(SIG_BLOCK, &alarms, &previous)) _exit(2);
    printf("%s %d\n", name, number);
    if (sigprocmask(SIG_SETMASK, &previous, NULL)) _exit(2);
}

static void _thread_(void *arg)
{
    int i;
    for (i = 0; i < 100; ++i) {
        print_line((const char *)arg, i);
        us_sleep(20000);
        scheduler_yield();
    }
}

/* Busy work with no yield: only the timer can switch threads. */
static void automatic_thread(void *arg)
{
    struct timespec start, now;
    int i;
    for (i = 0; i < 4; ++i) {
        print_line((const char *)arg, i);
        if (clock_gettime(CLOCK_MONOTONIC, &start)) _exit(2);
        do {
            if (clock_gettime(CLOCK_MONOTONIC, &now)) _exit(2);
        } while ((now.tv_sec - start.tv_sec) * 1000000000L
                 + now.tv_nsec - start.tv_nsec < 1200000000L);
    }
}

int main(int argc, char *argv[])
{
    scheduler_fnc_t function = _thread_;
    if (argc == 2 && !strcmp(argv[1], "--preempt"))
        function = automatic_thread;
    else if (argc != 1) {
        fprintf(stderr, "usage: %s [--preempt]\n", argv[0]);
        return 1;
    }
    if (scheduler_create(function, "hello") ||
        scheduler_create(function, "world") ||
        scheduler_create(function, "love") ||
        scheduler_create(function, "this") ||
        scheduler_create(function, "course!")) {
        TRACE("scheduler_create");
        scheduler_execute();
        return 1;
    }
    scheduler_execute();
    return 0;
}
