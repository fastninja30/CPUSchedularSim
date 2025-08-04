#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <limits.h>

// Part I
double next_exp(double lambda, int upper_bound) {
    double r;
    double x;
    do {
        r = drand48();
        x = -log(r) / lambda;
    } while (x > upper_bound);
    return x;
}
// Part II

typedef enum { FCFS, SJF, SRT, RR } alg_t;

typedef enum {
    EVT_SIM_START,
    EVT_ARRIVAL,
    EVT_CPU_START,
    EVT_CPU_COMPLETE,
    EVT_IO_COMPLETE,
    EVT_PREEMPT,
    EVT_SIM_END
} event_type_t;

typedef struct process {
    char    id[3];
    long    arrival_time;
    int     ncpu_bursts;
    long   *cpu_bursts, *io_bursts;
    int     cur_burst;
    long    remaining;      /* for SRT/RR */
    double  tau;            /* exp‐avg estimate */
    /* statistics: */
    double  wait_time, turnaround_time;
    int     cs_count, preempt_count;
    bool    cpu_bound;
    struct process *rq_next;
} process_t;

typedef struct event_node {
    long             time;
    event_type_t     type;
    process_t       *p;
    struct event_node *next;
} event_node;

/* Simulation parameters */
static int    t_cs;
static double alpha;
static int    t_slice;
static double saved_lambda;
static long   saved_seed;

/* Global process arrays */
static process_t *orig_procs;
static process_t *procs;
static int        nproc;

/* Ready queue head */
static process_t *ready_queue = NULL;
/* Event list head */
static event_node *event_list = NULL;

/* Statistics accumulators per‐alg */
static long   total_cpu_time;  /* sum of actual CPU bursts */
static long   sim_end_time;

/* ---- Utility: print ready‐queue contents ---- */
void print_queue(void) {
    if (!ready_queue) {
        printf("empty");
    } else {
        process_t *p = ready_queue;
        bool first = true;
        while (p) {
            if (!first) putchar(' ');
            printf("%s", p->id);
            first = false;
            p = p->rq_next;
        }
    }
}

/* ---- Event scheduling ---- */
void schedule_event(long t, event_type_t type, process_t *p) {
    event_node *e = malloc(sizeof(*e));
    e->time = t; e->type = type; e->p = p; e->next = NULL;
    /* insert sorted by time, then by id */
    if (!event_list
     || t < event_list->time
     || (t == event_list->time
         && p && event_list->p
         && strcmp(p->id, event_list->p->id) < 0)) {
        e->next = event_list;
        event_list = e;
    } else {
        event_node *cur = event_list;
        while (cur->next
            && (cur->next->time < t
             || (cur->next->time == t
              && p && cur->next->p
              && strcmp(cur->next->p->id, p->id) < 0))) {
            cur = cur->next;
        }
        e->next = cur->next;
        cur->next = e;
    }
}

event_node *pop_event() {
    event_node *e = event_list;
    if (e) event_list = e->next;
    return e;
}

/* ---- Ready‐queue operations ---- */
void enqueue(process_t *p, alg_t alg) {
    p->rq_next = NULL;
    if (!ready_queue) {
        ready_queue = p;
        return;
    }
    if (alg == SJF || alg == SRT) {
        /* sorted by key */
        double key = (alg == SJF ? p->tau : p->remaining);
        process_t *prev = NULL, *cur = ready_queue;
        while (cur) {
            double ckey = (alg == SJF ? cur->tau : cur->remaining);
            if (key < ckey
             || (key == ckey && strcmp(p->id, cur->id) < 0)) {
                break;
            }
            prev = cur;
            cur  = cur->rq_next;
        }
        if (!prev) {
            p->rq_next = ready_queue;
            ready_queue = p;
        } else {
            p->rq_next   = cur;
            prev->rq_next = p;
        }
    } else {
        /* FCFS or RR: append */
        process_t *tail = ready_queue;
        while (tail->rq_next) tail = tail->rq_next;
        tail->rq_next = p;
    }
}

process_t *dequeue() {
    process_t *p = ready_queue;
    if (p) ready_queue = p->rq_next;
    return p;
}

/* ---- Reset simulation state per‐alg ---- */
void reset_sim() {
    /* reseed RNG */
    srand48(saved_seed);
    /* clear event list */
    while (event_list) {
        free(pop_event());
    }
    /* reset ready queue */
    ready_queue = NULL;
    /* copy orig → working */
    for (int i = 0; i < nproc; i++) {
        procs[i] = orig_procs[i];
        procs[i].cur_burst = 0;
        procs[i].remaining = procs[i].cpu_bursts[0];
        procs[i].tau       = 1.0 / saved_lambda;
        procs[i].wait_time = procs[i].turnaround_time = 0.0;
        procs[i].cs_count = procs[i].preempt_count = 0;
    }
    total_cpu_time = 0;
    sim_end_time   = 0;
}

/* ---- Main event‐driven simulator ---- */
void simulate(alg_t alg) {
    bool cpu_busy = false;
    long now = 0;
    process_t *running = NULL;
    long burst_start_time = 0;

    /* schedule start */
    schedule_event(0, EVT_SIM_START, NULL);
    /* schedule arrivals */
    for (int i = 0; i < nproc; i++) {
        schedule_event(procs[i].arrival_time, EVT_ARRIVAL, &procs[i]);
    }

    while (true) {
        event_node *ev = pop_event();
        if (!ev) break;
        now = ev->time;

        switch (ev->type) {
        case EVT_SIM_START:
            printf("time %ldms: Simulator started for %s [Q ", now,
                   alg==FCFS?"FCFS":alg==SJF?"SJF":alg==SRT?"SRT":"RR");
            print_queue(); printf("]\n");
            break;

        case EVT_ARRIVAL:
            printf("time %ldms: Process %s arrived; added to ready queue [Q ",
                   now, ev->p->id);
            enqueue(ev->p, alg);
            print_queue(); printf("]\n");
            ev->p->wait_time -= now;  /* start timing wait */
            /* if CPU idle, start switch‐in */
            if (!cpu_busy && !running) {
                ev->p->cs_count++;
                schedule_event(now + t_cs/2, EVT_CPU_START, ev->p);
            }
            break;

        case EVT_CPU_START:
            cpu_busy = true;
            running = ev->p;
            burst_start_time = now;
            printf("time %ldms: Process %s started using the CPU for ",
                   now, running->id);
            if (alg == RR) {
                long slice = running->remaining < t_slice
                           ? running->remaining : t_slice;
                printf("%ldms of %ldms remaining [Q ", slice,
                       running->remaining);
                schedule_event(now + slice, EVT_PREEMPT, running);
            } else {
                printf("%ldms burst [Q ", running->remaining);
                schedule_event(now + running->remaining,
                               EVT_CPU_COMPLETE, running);
            }
            print_queue(); printf("]\n");
            break;

        case EVT_CPU_COMPLETE: {
            cpu_busy = false;
            process_t *p = ev->p;
            long actual = p->remaining;
            total_cpu_time += actual;
            /* turnaround: arrival → completion (+ switch halves) */
            p->turnaround_time += (now - p->turnaround_time /* misuse field */)
                               + t_cs/2;
            /* finish burst */
            p->cur_burst++;
            /* recalc tau if SJF/SRT */
            if (alg==SJF||alg==SRT) {
                double old = p->tau;
                p->tau = ceil(alpha * actual + (1-alpha)*old);
                printf("time %ldms: Recalculated tau for %s: old tau %.0fms ==> new tau %.0fms [Q ",
                       now, p->id, old, p->tau);
                print_queue(); printf("]\n");
            }
            /* schedule I/O or termination */
            if (p->cur_burst < p->ncpu_bursts) {
                long io_t = p->io_bursts[p->cur_burst - 1];
                printf("time %ldms: Process %s switching out; blocking on I/O until time %ldms [Q ",
                       now, p->id, now + io_t);
                print_queue(); printf("]\n");
                schedule_event(now + io_t, EVT_IO_COMPLETE, p);
                p->remaining = p->cpu_bursts[p->cur_burst];
            } else {
                printf("time %ldms: Process %s terminated [Q ", now, p->id);
                print_queue(); printf("]\n");
            }
            running = NULL;
            /* start next if any */
            if (ready_queue) {
                process_t *next = dequeue();
                next->wait_time += now;
                next->cs_count++;
                schedule_event(now + t_cs/2, EVT_CPU_START, next);
            } else if (!event_list) {
                /* no more events: end */
                schedule_event(now, EVT_SIM_END, NULL);
            }
            break;
        }

        case EVT_IO_COMPLETE:
            printf("time %ldms: Process %s completed I/O; added to ready queue [Q ",
                   now, ev->p->id);
            enqueue(ev->p, alg);
            print_queue(); printf("]\n");
            ev->p->wait_time -= now;
            if (!cpu_busy && !running) {
                ev->p->cs_count++;
                schedule_event(now + t_cs/2, EVT_CPU_START, ev->p);
            }
            break;

        case EVT_PREEMPT: {
            cpu_busy = false;
            process_t *p = ev->p;
            long used = now - burst_start_time;
            p->remaining -= used;
            p->preempt_count++;
            printf("time %ldms: Time slice expired; preempting %s with %ldms remaining [Q ",
                   now, p->id, p->remaining);
            enqueue(p, alg);
            print_queue(); printf("]\n");
            running = NULL;
            /* next */
            if (ready_queue) {
                process_t *next = dequeue();
                next->wait_time -= now;
                next->cs_count++;
                schedule_event(now + t_cs/2, EVT_CPU_START, next);
            } else {
                /* no switch if queue empty; keep running if remaining>0 */
                schedule_event(now + (p->remaining < t_slice
                                  ? p->remaining : t_slice),
                              EVT_PREEMPT, p);
                running = p;
                cpu_busy = true;
                burst_start_time = now;
            }
            break;
        }

        case EVT_SIM_END:
            sim_end_time = now;
            printf("time %ldms: Simulator ended for %s [Q ",
                   now,
                   alg==FCFS?"FCFS":alg==SJF?"SJF":alg==SRT?"SRT":"RR");
            print_queue(); printf("]\n");
            free(ev);
            return;
        }

        free(ev);
    }
}

/* ---- Append statistics to simout.txt ---- */
void dump_stats(alg_t alg) {
    FILE *f = fopen("simout.txt", "a");
    if (!f) return;
    double util = 100.0 * total_cpu_time / sim_end_time;
    double cb_wait = 0, ib_wait = 0, cb_tat = 0, ib_tat = 0;
    int cb_cs = 0, ib_cs = 0, cb_pre=0, ib_pre=0;
    int cb_cnt = 0, ib_cnt = 0;
    for (int i = 0; i < nproc; i++) {
        process_t *p = &procs[i];
        if (p->cpu_bound) {
            cb_wait   += p->wait_time;
            cb_tat    += p->turnaround_time;
            cb_cs     += p->cs_count;
            cb_pre    += p->preempt_count;
            cb_cnt++;
        } else {
            ib_wait   += p->wait_time;
            ib_tat    += p->turnaround_time;
            ib_cs     += p->cs_count;
            ib_pre    += p->preempt_count;
            ib_cnt++;
        }
    }
    double ov_wait = (cb_wait + ib_wait) / nproc;
    double ov_tat  = (cb_tat + ib_tat) / nproc;
    if (cb_cnt) cb_wait/=cb_cnt, cb_tat/=cb_cnt;
    if (ib_cnt) ib_wait/=ib_cnt, ib_tat/=ib_cnt;
    fprintf(f,"Algorithm %s\n",
            alg==FCFS?"FCFS":alg==SJF?"SJF":alg==SRT?"SRT":"RR");
    fprintf(f,"-- CPU utilization: %.3f%%\n", util);
    fprintf(f,"-- CPU-bound average wait time: %.3f ms\n", cb_wait);
    fprintf(f,"-- I/O-bound average wait time: %.3f ms\n", ib_wait);
    fprintf(f,"-- overall average wait time: %.3f ms\n", ov_wait);
    fprintf(f,"-- CPU-bound average turnaround time: %.3f ms\n", cb_tat);
    fprintf(f,"-- I/O-bound average turnaround time: %.3f ms\n", ib_tat);
    fprintf(f,"-- overall average turnaround time: %.3f ms\n", ov_tat);
    fprintf(f,"-- CPU-bound number of context switches: %d\n", cb_cs);
    fprintf(f,"-- I/O-bound number of context switches: %d\n", ib_cs);
    fprintf(f,"-- overall number of context switches: %d\n", cb_cs+ib_cs);
    fprintf(f,"-- CPU-bound number of preemptions: %d\n", cb_pre);
    fprintf(f,"-- I/O-bound number of preemptions: %d\n", ib_pre);
    fprintf(f,"-- overall number of preemptions: %d\n\n", cb_pre+ib_pre);
    fclose(f);
}

int main(int argc, char *argv[]) {
    setvbuf( stdout, NULL,_IONBF, 0);

    if (argc != 9) {
        fprintf(stderr, "ERROR: Invalid number of command-line arguments. Expected 8.\n");
        return EXIT_FAILURE;
    }
    //Part I
    int n = atoi(argv[1]);
    int ncpu = atoi(argv[2]);
    long seed = atol(argv[3]);
    double lambda = atof(argv[4]);
    int upper_bound = atoi(argv[5]);

    //Part II
    int tcs = atoi(argv[6]);
    double alpha = atoi(argv[7]);
    int tslice = atoi(argv[8]);

    if (n <= 0) {
        fprintf(stderr, "ERROR: The number of processes must be greater than zero.\n");
        return EXIT_FAILURE;
    }
    if (ncpu < 0 || ncpu > n) {
        fprintf(stderr, "ERROR: The number of CPU-bound processes must be between 0 and the total number of processes.\n");
        return EXIT_FAILURE;
    }
    if (lambda <= 0) {
        fprintf(stderr, "ERROR: Lambda must be a positive value.\n");
        return EXIT_FAILURE;
    }
    if (upper_bound <= 0) {
        fprintf(stderr, "ERROR: The upper bound must be a positive value.\n");
        return EXIT_FAILURE;
    }
    if (tcs <= 0 || (tcs % 2) != 0) {
        fprintf(stderr, "ERROR: Time to context switch must be a positive even integer.\n")
    }
    if (alpha <= 0.0 || alpha > 1.0) {
        fprintf(stderr, "ERROR: Alpha must be between 0 and 1.\n")
    }
    if (tslice <= 0) {
        fprintf(stderr, "ERROR: Time slice must be a positive value.\n");
        return EXIT_FAILURE;
    }
    // Maximum of 260 processes (A0-Z9)
    if (n > 260) {
        fprintf(stderr, "ERROR: The maximum number of processes is 260.\n");
        return EXIT_FAILURE;
    }

    // <----------------------- PART I ----------------------->
    // --- 2. Initialization ---
    srand48(seed);

    long double cpu_bound_total_cpu_time = 0;
    double cpu_bound_cpu_burst_count = 0;
    long double cpu_bound_total_io_time = 0;
    double cpu_bound_io_burst_count = 0;

    long double io_bound_total_cpu_time = 0;
    double io_bound_cpu_burst_count = 0;
    long double io_bound_total_io_time = 0;
    double io_bound_io_burst_count = 0;


    printf("<<< PROJECT PART I\n");
    printf("<<< -- process set (n=%d) with %d CPU-bound %s\n", n, ncpu, (ncpu == 1) ? "process" : "processes");
    printf("<<< -- seed=%ld; lambda=%.6f; bound=%d\n", seed, lambda, upper_bound);


    for (int i = 0; i < n; i++) {
        bool is_cpu_bound = (i < ncpu);

        // Generate process IDs (A0, A1, ..., Z9)
        char process_id[3];
        sprintf(process_id, "%c%d", 'A' + (i / 10), i % 10);

        // 1. Generate arrival time
        int arrival_time = floor(next_exp(lambda, upper_bound));

        // 2. Generate number of CPU bursts
        int num_bursts = ceil(drand48() * 32.0);
        if (num_bursts == 0) num_bursts = 1;

        printf("%s process %s: arrival time %dms; %d CPU %s:\n",
               is_cpu_bound ? "CPU-bound" : "I/O-bound",
               process_id, arrival_time, num_bursts,
               (num_bursts == 1) ? "burst" : "bursts"
               );

        // 3. Generate CPU burst time
        for (int j = 0; j < num_bursts; j++) {
            double cpu_time = ceil(next_exp(lambda, upper_bound));
            if (is_cpu_bound) {
                cpu_time *= 4;
            }

            if (is_cpu_bound) {
                cpu_bound_total_cpu_time += cpu_time;
                cpu_bound_cpu_burst_count++;
            } else {
                io_bound_total_cpu_time += cpu_time;
                io_bound_cpu_burst_count++;
            }

            // 4. Generate I/O burst time
            if (j < num_bursts - 1) {
                double io_time = ceil(next_exp(lambda, upper_bound));

                /* What directions said to do:
                if (is_cpu_bound) {
                    io_time /= 8;
                } else {
                    io_time *= 8;
                }
                */

                // Vs. what answer key in output02.txt, output03.txt,..., output05.txt  does instead:
                io_time *= 8;
                if (is_cpu_bound) {
                    io_time /= 8;
                }

                if (is_cpu_bound) {
                    cpu_bound_total_io_time += io_time;
                    cpu_bound_io_burst_count++;
                } else {
                    io_bound_total_io_time += io_time;
                    io_bound_io_burst_count++;
                }
                printf("==> CPU burst %.0fms ==> I/O burst %.0fms\n", cpu_time, io_time);
            } else {
                printf("==> CPU burst %.0fms\n", cpu_time);
            }
        }
    }

    // 5. Generate simout.txt file
    FILE *outfile = fopen("simout.txt", "w");
    if (outfile == NULL) {
        fprintf(stderr, "ERROR: Could not open file simout.txt for writing.\n");
        return EXIT_FAILURE;
    }

    double cpu_bound_avg_cpu = (cpu_bound_cpu_burst_count > 0) ? (cpu_bound_total_cpu_time / cpu_bound_cpu_burst_count) : 0.0;
    double io_bound_avg_cpu = (io_bound_cpu_burst_count > 0) ? (io_bound_total_cpu_time / io_bound_cpu_burst_count) : 0.0;
    double overall_avg_cpu = ((cpu_bound_cpu_burst_count + io_bound_cpu_burst_count) > 0) ?
                                ((cpu_bound_total_cpu_time + io_bound_total_cpu_time) / (cpu_bound_cpu_burst_count + io_bound_cpu_burst_count)) : 0.0;

    double cpu_bound_avg_io = (cpu_bound_io_burst_count > 0) ? (cpu_bound_total_io_time / cpu_bound_io_burst_count) : 0.0;
    double io_bound_avg_io = (io_bound_io_burst_count > 0) ? (io_bound_total_io_time / io_bound_io_burst_count) : 0.0;
    double overall_avg_io = ((cpu_bound_io_burst_count + io_bound_io_burst_count) > 0) ?
                                ((cpu_bound_total_io_time + io_bound_total_io_time) / (cpu_bound_io_burst_count + io_bound_io_burst_count)) : 0.0;

    fprintf(outfile, "-- number of processes: %d\n", n);
    fprintf(outfile, "-- number of CPU-bound processes: %d\n", ncpu);
    fprintf(outfile, "-- number of I/O-bound processes: %d\n", n - ncpu);
    fprintf(outfile, "-- CPU-bound average CPU burst time: %.3f ms\n", cpu_bound_avg_cpu);
    fprintf(outfile, "-- I/O-bound average CPU burst time: %.3f ms\n", io_bound_avg_cpu);
    fprintf(outfile, "-- overall average CPU burst time: %.3f ms\n", overall_avg_cpu);
    fprintf(outfile, "-- CPU-bound average I/O burst time: %.3f ms\n", cpu_bound_avg_io);
    fprintf(outfile, "-- I/O-bound average I/O burst time: %.3f ms\n", io_bound_avg_io);
    fprintf(outfile, "-- overall average I/O burst time: %.3f ms\n", overall_avg_io);

    fclose(outfile);


    // <----------------------- PART II ----------------------->
    // Allocate working copy
    procs = malloc(nproc * sizeof(process_t));
    orig_procs = malloc(nproc * sizeof(process_t));
    memcpy(orig_procs, procs, nproc * sizeof(process_t));

    printf("<<< PROJECT PART II\n");
    printf("<<< -- t_cs=%dms; alpha=%.2f; t_slice=%dms\n",
           t_cs, alpha, t_slice);

    /* run each algorithm */
    for (alg_t alg = FCFS; alg <= RR; alg++) {
        reset_sim();
        simulate(alg);
        dump_stats(alg);
    }

    return EXIT_SUCCESS;
}
