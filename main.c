#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#define EVT_TIME_SLICE  EVT_PREEMPT

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

// Algorithms
typedef enum { FCFS, SJF, SRT, RR } alg_t;

// Events
typedef enum {
    EVT_SIM_START,
    EVT_ARRIVAL,
    EVT_CPU_START,
    EVT_CPU_FINISH,
    EVT_PREEMPT,
    EVT_IO_START,
    EVT_IO_FINISH,
    EVT_SIM_END
} event_type_t;

// Storing processes from Part I
typedef struct {
    char       id[3];              // e.g. "A0"
    int        arrival_time;       // from Part I
    int        num_bursts;         // from Part I
    long      *cpu_bursts;         // array of burst lengths
    long      *io_bursts;          // array of I/O lengths (size = num_bursts–1)
    int        cur_burst;          // index of next CPU burst
    long       remaining_time;     // for SRT/RR: time left in current CPU burst
    double     tau;                // predicted next CPU burst (for SJF/SRT), init = 1/λ
    bool       is_cpu_bound;       // from Part I
} process_t;


typedef struct {
    long          time;            // timestamp of event
    event_type_t  type;
    process_t    *proc;
} event_t;

// FCFS & RR:
queue<process_t*> ready_fifo;

// SJF & SRT:
minheap<process_t*> ready_pq;

// Event-Queue Operations
void   event_heap_clear(void);
bool   event_heap_empty(void);
void   push_event(event_t ev);
event_t pop_event(void);


// Ready-Queue Operations
void        clear_ready_queue(alg_t alg);
bool        ready_empty(alg_t alg);
void        enqueue_ready(process_t *p, alg_t alg);
process_t * next_proc_from_ready(alg_t alg);

// Context-Switch Helper
void schedule_cpu_start(process_t *p) {
    long start_time = t + (tcs / 2);
    event_t ev;
    ev.time = start_time;
    ev.type = EVT_CPU_START;
    ev.proc = p;
    push_event(ev);
}

// State Reset / Utilities
void copy_initial_state(process_t dest[]);
const char * alg_name(alg_t alg);
bool cpu_idle(void);
bool all_done(void);

// Event Handlers
void handle_arrival   (process_t *p, alg_t alg);
void handle_cpu_start (process_t *p, alg_t alg);
void handle_cpu_finish(process_t *p, alg_t alg);
void handle_preempt   (process_t *p, alg_t alg);
void handle_io_start  (process_t *p, alg_t alg);
void handle_io_finish (process_t *p, alg_t alg);

void simulate(alg_t alg) {
    // 1. Deep-copy the original process array to reset cur_burst, remaining_time, tau, etc.
    process_t procs_copy[n];
    copy_initial_state(procs_copy);

    // 2. Clear event queue & ready queue; set t = 0
    event_heap_clear();
    clear_ready_queue();

    // 3. Seed initial arrivals
    for (int i = 0; i < n; i++) {
        push_event({ .time = procs_copy[i].arrival_time,
                     .type = EVT_ARRIVAL,
                     .proc = &procs_copy[i] });
    }
    push_event({ .time = 0, .type = EVT_SIM_START, .proc = NULL });

    // 4. Main loop
    while (!event_heap_empty()) {
        event_t ev = pop_event();
        t = ev.time;

        switch (ev.type) {
        case EVT_SIM_START:
            printf("time %ldms: Simulator started for %s [Q empty]\n",
                   t, alg_name(alg));
            break;

        case EVT_ARRIVAL:
            handle_arrival(ev.proc, alg);
            break;

        case EVT_CPU_START:
            handle_cpu_start(ev.proc, alg);
            break;

        case EVT_CPU_FINISH:
            handle_cpu_finish(ev.proc, alg);
            break;

        case EVT_PREEMPT:            // also covers time‐slice expires
            handle_preempt(ev.proc, alg);
            break;

        case EVT_IO_START:
            handle_io_start(ev.proc, alg);
            break;

        case EVT_IO_FINISH:
            handle_io_finish(ev.proc, alg);
            break;

        case EVT_SIM_END:
            printf("time %ldms: Simulator ended for %s\n", t, alg_name(alg));
            return;
        }
    }
}

int main(int argc, char *argv[]) {
    setvbuf( stdout, NULL,_IONBF, 0);

    if (argc != 9) {
        fprintf(stderr, "ERROR: Invalid number of command-line arguments. Expected 8.\n");
        return EXIT_FAILURE;
    }

    // Part I
    int n = atoi(argv[1]);
    int ncpu = atoi(argv[2]);
    long seed = atol(argv[3]);
    double lambda = atof(argv[4]);
    int upper_bound = atoi(argv[5]);


    // Part II
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

    // <-------------------------------  PART I ------------------------------->
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


    cpu_bound_avg_cpu = ceil(cpu_bound_avg_cpu * 1000.0) / 1000.0;
    io_bound_avg_cpu  = ceil(io_bound_avg_cpu * 1000.0) / 1000.0;
    overall_avg_cpu   = ceil(overall_avg_cpu * 1000.0) / 1000.0;
    cpu_bound_avg_io  = ceil(cpu_bound_avg_io * 1000.0) / 1000.0;
    io_bound_avg_io = ceil(io_bound_avg_io * 1000.0) / 1000.0;
    overall_avg_io = ceil(overall_avg_io * 1000.0) / 1000.0;

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

    return EXIT_SUCCESS;
}