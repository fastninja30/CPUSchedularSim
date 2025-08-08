#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>

//-----------------------------------------------------------------------------
// DATA STRUCTURES & FORWARD DECLARATIONS
//-----------------------------------------------------------------------------

typedef struct Process Process;

typedef struct {
    int cpu_time;
    int io_time;
} Burst;

typedef enum {
    NEW, READY, RUNNING, WAITING, SWITCHING_IN, TERMINATED
} ProcessState;

struct Process {
    char id[4];
    bool is_cpu_bound;
    int arrival_time;
    int num_bursts;
    Burst* bursts;

    // Simulation state
    ProcessState state;
    int current_burst;
    int remaining_cpu_time;
    int time_entered_ready;
    int io_completion_time;
    double tau;
    int burst_start_time; 
    int turnaround_start_time;
    bool was_preempted_this_burst; // For RR stat
    
    // Statistics
    double total_wait_time;
    double total_turnaround_time;
    int num_context_switches;
    int num_preemptions;
    int rr_bursts_in_slice;

    bool requeue_from_preempt;     // NEW: mark delayed RR requeue
};

typedef struct Node {
    Process* p;
    struct Node* next;
} Node;

// Global simulation parameters (to be set in main)
int T_CS;
double ALPHA;
int T_SLICE;
bool PRINT_ALL_EVENTS;


//-----------------------------------------------------------------------------
// HELPER FUNCTIONS
//-----------------------------------------------------------------------------

double next_exp(double lambda, int upper_bound) {
    double r;
    double x;
    do {
        r = drand48();
        x = -log(r) / lambda;
    } while (x > upper_bound);
    return x;
}

double ceil_3dp(double val) {
    return ceil(val * 1000.0) / 1000.0;
}

void print_queue(Node* head) {
    printf("[Q");
    if (head == NULL) {
        printf(" empty]\n");
    } else {
        Node* curr = head;
        while (curr != NULL) {
            printf(" %s", curr->p->id);
            curr = curr->next;
        }
        printf("]\n");
    }
}

static inline void dbg_enq(const char* why, int t, const char* who, Node* q_after) {
    //printf("DBG %6d: ENQ %-4s via %-10s -> ", t, who, why); print_queue(q_after);
}

static inline void dbg_note(const char* what, int t) {
    //printf("DBG %6d: %s\n", t, what);
}
void push_back(Node** head, Process* p) {
    Node* new_node = (Node*)malloc(sizeof(Node));
    new_node->p = p;
    new_node->next = NULL;
    if (*head == NULL) {
        *head = new_node;
        return;
    }
    Node* curr = *head;
    while (curr->next != NULL) {
        curr = curr->next;
    }
    curr->next = new_node;
}

Process* pop_front(Node** head) {
    if (*head == NULL) return NULL;
    Node* temp = *head;
    Process* p = temp->p;
    *head = (*head)->next;
    free(temp);
    return p;
}

int compare_processes(const void* a, const void* b) {
    Process* pA = *(Process**)a;
    Process* pB = *(Process**)b;
    return strcmp(pA->id, pB->id);
}

/* ---------- SRT helpers (new) ---------- */

// predicted remaining = ceil(tau - executed_so_far)
// executed_so_far = cpu_burst - remaining_cpu_time
// If remaining_cpu_time == 0 (hasn't started this burst), executed_so_far = 0.
static inline int srt_key(Process *p) {
    int cpu = p->bursts[p->current_burst].cpu_time;
    int executed = (p->remaining_cpu_time == 0) ? 0 : (cpu - p->remaining_cpu_time);
    int pred = (int)ceil(p->tau - executed);
    return (pred > 0) ? pred : 0;
}

/* Insert sorted by tau (SJF). Tiebreak by ID. */
void push_sorted_sjf(Node** head, Process* p) {
    Node* new_node = (Node*)malloc(sizeof(Node));
    new_node->p = p;
    new_node->next = NULL;

    if (*head == NULL) { *head = new_node; return; }

    Node *prev = NULL, *cur = *head;
    while (cur) {
        if (p->tau < cur->p->tau) break;
        if (p->tau == cur->p->tau && strcmp(p->id, cur->p->id) < 0) break;
        prev = cur; cur = cur->next;
    }
    new_node->next = cur;
    if (prev) prev->next = new_node; else *head = new_node;
}

/* Insert sorted by predicted remaining (SRT). Tiebreak by ID. */
void push_sorted_srt(Node** head, Process* p) {
    Node* new_node = (Node*)malloc(sizeof(Node));
    new_node->p = p;
    new_node->next = NULL;

    if (*head == NULL) { *head = new_node; return; }

    Node *prev = NULL, *cur = *head;
    int keyp = srt_key(p);

    while (cur) {
        int keyc = srt_key(cur->p);
        if (keyp < keyc) break;
        if (keyp == keyc && strcmp(p->id, cur->p->id) < 0) break;
        prev = cur; cur = cur->next;
    }
    new_node->next = cur;
    if (prev) prev->next = new_node; else *head = new_node;
}

void reset_simulation_state(Process* master, Process* working, int n, double initial_tau) {
    for (int i = 0; i < n; i++) {
        working[i] = master[i];
        
        working[i].state = NEW;
        working[i].current_burst = 0;
        working[i].remaining_cpu_time = 0;
        working[i].time_entered_ready = 0;
        working[i].io_completion_time = -1;
        working[i].tau = initial_tau;
        working[i].burst_start_time = 0;
        working[i].turnaround_start_time = 0;
        working[i].was_preempted_this_burst = false;

        working[i].total_wait_time = 0;
        working[i].total_turnaround_time = 0;
        working[i].num_context_switches = 0;
        working[i].num_preemptions = 0;
        working[i].rr_bursts_in_slice = 0;

        working[i].was_preempted_this_burst = false;
        working[i].requeue_from_preempt = false;    
    }
}

//-----------------------------------------------------------------------------
// SIMULATION ENGINE
//-----------------------------------------------------------------------------
void run_simulation(const char* algo, Process* processes, int n, int ncpu, double initial_tau) {
    int time = 0;
    int terminated_count = 0;
    PRINT_ALL_EVENTS = true;
    
    Node* ready_queue = NULL;

    // Buffer for RR preempted processes at the *current* timestamp
    Process* preempt_buf[n];
    int preempt_cnt = 0;

    Process* running_process = NULL;
    Process* switching_in_process = NULL;  
    
    int cpu_busy_until = 0;
    int switch_in_end_time = -1;
    double total_cpu_active_time = 0;

    printf("\n");
    if (PRINT_ALL_EVENTS) {
        printf("time %dms: Simulator started for %s ", time, algo);
        print_queue(ready_queue);
    }

    while (terminated_count < n) {
        int next_t = -1;

        if (running_process && (next_t == -1 || cpu_busy_until < next_t)) next_t = cpu_busy_until;
        if (!running_process && cpu_busy_until > time && (next_t == -1 || cpu_busy_until < next_t)) next_t = cpu_busy_until;
        if (switching_in_process && (next_t == -1 || switch_in_end_time < next_t)) next_t = switch_in_end_time;

        for (int i = 0; i < n; i++) {
            if (processes[i].state == NEW && (next_t == -1 || processes[i].arrival_time < next_t)) next_t = processes[i].arrival_time;
            if (processes[i].state == WAITING && (next_t == -1 || processes[i].io_completion_time < next_t)) next_t = processes[i].io_completion_time;
        }

        if (strcmp(algo, "RR") == 0 && running_process) {
            int slice_end_time = running_process->burst_start_time + T_SLICE;
            if (slice_end_time < cpu_busy_until && (next_t == -1 || slice_end_time < next_t)) next_t = slice_end_time;
        }

        if (next_t == -1) break;

        if (running_process) {
            int time_ran = next_t - time;
            running_process->remaining_cpu_time -= time_ran;
            total_cpu_active_time += time_ran;
        }
        time = next_t;

        /* ========= EVENT HANDLING at time ========= */

        // (1) CPU Burst Completion
        if (running_process && time >= cpu_busy_until) {
            running_process->total_turnaround_time += (time + T_CS / 2.0) - running_process->turnaround_start_time;

            if (time < 10000 && PRINT_ALL_EVENTS && running_process->current_burst + 1 < running_process->num_bursts) {
                if (strcmp(algo, "SJF") == 0 || strcmp(algo, "SRT") == 0)
                    printf("time %dms: Process %s (tau %.0fms) completed a CPU burst; %d %s to go ",
                           time, running_process->id, running_process->tau,
                           running_process->num_bursts - running_process->current_burst - 1,
                           (running_process->num_bursts - running_process->current_burst - 1 == 1) ? "burst" : "bursts");
                else
                    printf("time %dms: Process %s completed a CPU burst; %d %s to go ",
                           time, running_process->id,
                           running_process->num_bursts - running_process->current_burst - 1,
                           (running_process->num_bursts - running_process->current_burst - 1 == 1) ? "burst" : "bursts");
                print_queue(ready_queue);
            }

            if (strcmp(algo, "SJF") == 0 || strcmp(algo, "SRT") == 0) {
                double old_tau = running_process->tau;
                running_process->tau = ceil(ALPHA * running_process->bursts[running_process->current_burst].cpu_time
                                            + (1 - ALPHA) * old_tau);
                if (time < 10000 && PRINT_ALL_EVENTS && running_process->current_burst + 1 < running_process->num_bursts) {
                    printf("time %dms: Recalculated tau for process %s: old tau %.0fms ==> new tau %.0fms ",
                           time, running_process->id, old_tau, running_process->tau);
                    print_queue(ready_queue);
                }
            }

            if (strcmp(algo, "RR") == 0 && !running_process->was_preempted_this_burst) {
                running_process->rr_bursts_in_slice++;
            }

            running_process->current_burst++;
            if (running_process->current_burst == running_process->num_bursts) {
                running_process->state = TERMINATED;
                terminated_count++;
                if (PRINT_ALL_EVENTS) {
                    printf("time %dms: Process %s terminated ", time, running_process->id);
                    print_queue(ready_queue);
                }
            } else {
                running_process->state = WAITING;
                running_process->io_completion_time = time + T_CS / 2 + running_process->bursts[running_process->current_burst - 1].io_time;
                if (time < 10000 && PRINT_ALL_EVENTS) {
                    printf("time %dms: Process %s switching out of CPU; blocking on I/O until time %dms ",
                           time, running_process->id, running_process->io_completion_time);
                    print_queue(ready_queue);
                }
            }
            running_process = NULL;
            cpu_busy_until = time + T_CS/2;
        }

        // (5) RR time-slice expiration (first instance)
        if (strcmp(algo, "RR") == 0 && running_process &&
            time == (running_process->burst_start_time + T_SLICE) &&
            time < cpu_busy_until) {

            if (ready_queue == NULL) {
                if (time < 10000 && PRINT_ALL_EVENTS) {
                    printf("time %dms: Time slice expired; no preemption because ready queue is empty ", time);
                    print_queue(ready_queue);
                }
                running_process->was_preempted_this_burst = true;
                running_process->burst_start_time = time;
            } else {
                running_process->num_preemptions++;
                running_process->was_preempted_this_burst = true;
                if (time < 10000 && PRINT_ALL_EVENTS) {
                    printf("time %dms: Time slice expired; preempting process %s with %dms remaining ",
                           time, running_process->id, running_process->remaining_cpu_time);
                    print_queue(ready_queue);
                }
                running_process->state = WAITING;
                running_process->io_completion_time = time + T_CS/2;
                running_process->requeue_from_preempt = true;

                running_process = NULL;
                cpu_busy_until = time + T_CS / 2;
            }
        }
        // (2) Handle switch-in completion
        if (switching_in_process && time == switch_in_end_time) {
            running_process = switching_in_process;
            switching_in_process = NULL;
            switch_in_end_time = -1;

            running_process->state = RUNNING;
            running_process->burst_start_time = time;
            cpu_busy_until = time + running_process->remaining_cpu_time;

            bool srt_immediate = false;
            Process *preemptor = NULL;
            if (strcmp(algo, "SRT") == 0 && ready_queue != NULL) {
                int head_key = srt_key(ready_queue->p);
                int run_key  = srt_key(running_process);
                if (head_key < run_key) {
                    srt_immediate = true;
                    preemptor = ready_queue->p;
                }
            }

            if (time < 10000 && PRINT_ALL_EVENTS) {
                Process *rp = running_process;
                printf("time %dms: Process %s", time, rp->id);
                if (strcmp(algo, "SJF") == 0 || strcmp(algo, "SRT") == 0)
                    printf(" (tau %.0fms)", rp->tau);
                printf(" started using the CPU ");
                if (rp->remaining_cpu_time != rp->bursts[rp->current_burst].cpu_time)
                    printf("for remaining %dms of %dms burst ",
                           rp->remaining_cpu_time, rp->bursts[rp->current_burst].cpu_time);
                else
                    printf("for %dms burst ", rp->remaining_cpu_time);
                print_queue(ready_queue);

                if (srt_immediate) {
                    printf("time %dms: Process %s (tau %.0fms) will preempt %s ",
                           time, preemptor->id, preemptor->tau, rp->id);
                    print_queue(ready_queue);
                }
            }

            if (srt_immediate) {
                running_process->num_preemptions++;
                running_process->state = WAITING;
                running_process->io_completion_time = time + T_CS / 2;
                running_process->requeue_from_preempt = true;
                running_process = NULL;
                cpu_busy_until = time + T_CS / 2;
            }
        }

        // (3) I/O Burst Completions
        Process* io_completed[n]; int io_count = 0;
        for (int i = 0; i < n; i++) {
            if (processes[i].state == WAITING && processes[i].io_completion_time == time)
                io_completed[io_count++] = &processes[i];
        }
        if (io_count > 0) {
            Process* preempt_requeues[n]; int rq_count = 0;
            Process* real_io[n];          int ro_count = 0;

            for (int i = 0; i < io_count; i++) {
                Process* p = io_completed[i];
                if (p->requeue_from_preempt) {
                    preempt_requeues[rq_count++] = p;
                } else {
                    real_io[ro_count++] = p;
                }
            }

            if (rq_count > 0) qsort(preempt_requeues, rq_count, sizeof(Process*), compare_processes);
            if (ro_count > 0) qsort(real_io, ro_count, sizeof(Process*), compare_processes);

            for (int i = 0; i < rq_count; i++) {
                Process* p = preempt_requeues[i];
                p->requeue_from_preempt = false;
                p->state = READY;
                p->time_entered_ready = time;
                if (strcmp(algo, "RR") == 0) push_back(&ready_queue, p);
                else push_sorted_srt(&ready_queue, p);
            }

            for (int i = 0; i < ro_count; i++) {
                Process* p = real_io[i];
                bool preempted = false;

                if (strcmp(algo, "SRT") == 0 && running_process) {
                    int running_rem = srt_key(running_process);
                    int newcomer = srt_key(p);
                    if (newcomer < running_rem) preempted = true;
                }

                p->state = READY;
                p->time_entered_ready = time;
                p->turnaround_start_time = time;

                if (strcmp(algo, "FCFS") == 0 || strcmp(algo, "RR") == 0) push_back(&ready_queue, p);
                else if (strcmp(algo, "SJF") == 0) push_sorted_sjf(&ready_queue, p);
                else push_sorted_srt(&ready_queue, p);

                if (preempted) {
                    running_process->num_preemptions++;
                    int running_rem = srt_key(running_process);
                    if (time < 10000 && PRINT_ALL_EVENTS) {
                        printf("time %dms: Process %s (tau %.0fms) completed I/O; preempting %s (predicted remaining time %dms) ",
                               time, p->id, p->tau, running_process->id, running_rem);
                        print_queue(ready_queue);
                    }
                    running_process->state = WAITING;
                    running_process->io_completion_time = time + T_CS / 2;
                    running_process->requeue_from_preempt = true;
                    running_process = NULL;
                    cpu_busy_until = time + T_CS / 2;
                } else {
                    if (time < 10000 && PRINT_ALL_EVENTS) {
                        if (strcmp(algo, "SJF") == 0 || strcmp(algo, "SRT") == 0) 
                            printf("time %dms: Process %s (tau %.0fms) completed I/O; added to ready queue ", time, p->id, p->tau);
                        else
                            printf("time %dms: Process %s completed I/O; added to ready queue ", time, p->id);
                        print_queue(ready_queue);
                    }
                }
            }
        }

        // (4) New Process Arrivals
        Process* arrived[n]; int arr_count = 0;
        for (int i = 0; i < n; i++) {
            if (processes[i].state == NEW && processes[i].arrival_time == time)
                arrived[arr_count++] = &processes[i];
        }
        qsort(arrived, arr_count, sizeof(Process*), compare_processes);

        for (int i = 0; i < arr_count; i++) {
            Process* p = arrived[i];
            bool preempted = false;
            if (strcmp(algo, "SRT") == 0 && running_process) {
                int running_rem = srt_key(running_process);
                int newcomer = srt_key(p);
                if (newcomer < running_rem) preempted = true;
            }

            p->state = READY;
            p->time_entered_ready = time;
            p->turnaround_start_time = time;

            if (strcmp(algo, "FCFS") == 0 || strcmp(algo, "RR") == 0) push_back(&ready_queue, p);
            else if (strcmp(algo, "SJF") == 0) push_sorted_sjf(&ready_queue, p);
            else push_sorted_srt(&ready_queue, p);

            if (preempted) {
                running_process->num_preemptions++;
                int running_rem = srt_key(running_process);
                if (time < 10000 && PRINT_ALL_EVENTS) {
                    printf("time %dms: Process %s (tau %.0fms) arrived; preempting %s (predicted remaining time %dms) ",
                           time, p->id, p->tau, running_process->id, running_rem);
                    print_queue(ready_queue);
                }
                running_process->state = WAITING;
                running_process->io_completion_time = time + T_CS / 2;
                running_process->requeue_from_preempt = true;
                running_process = NULL;
                cpu_busy_until = time + T_CS / 2;
            } else {
                if (time < 10000 && PRINT_ALL_EVENTS) {
                    if (strcmp(algo, "SJF") == 0 || strcmp(algo, "SRT") == 0) 
                        printf("time %dms: Process %s (tau %.0fms) arrived; added to ready queue ", time, p->id, p->tau);
                    else
                        printf("time %dms: Process %s arrived; added to ready queue ", time, p->id);
                    print_queue(ready_queue);
                }
            }
        }

        // (5) RR time-slice expiration (second instance)
        if (strcmp(algo, "RR") == 0 && running_process &&
            time == (running_process->burst_start_time + T_SLICE) &&
            time < cpu_busy_until) {

            if (ready_queue == NULL) {
                if (time < 10000 && PRINT_ALL_EVENTS) {
                    printf("time %dms: Time slice expired; no preemption because ready queue is empty ", time);
                    print_queue(ready_queue);
                }
                running_process->was_preempted_this_burst = true;
                running_process->burst_start_time = time;
            } else {
                running_process->num_preemptions++;
                running_process->was_preempted_this_burst = true;
                if (time < 10000 && PRINT_ALL_EVENTS) {
                    printf("time %dms: Time slice expired; preempting process %s with %dms remaining ",
                           time, running_process->id, running_process->remaining_cpu_time);
                    print_queue(ready_queue);
                }
                running_process->state = WAITING;
                running_process->io_completion_time = time + T_CS/2;
                running_process->requeue_from_preempt = true;

                running_process = NULL;
                cpu_busy_until = time + T_CS / 2;
            }
        }

        // (6) Buffer enqueing
        if (preempt_cnt > 0) {
            for (int k = 0; k < preempt_cnt; k++) {
                push_back(&ready_queue, preempt_buf[k]);
            }
            preempt_cnt = 0;
        }
        
        // (7) Scheduler
        if (!running_process && !switching_in_process && time >= cpu_busy_until && ready_queue != NULL) {
            Process* p = pop_front(&ready_queue);
            p->num_context_switches++;
            
            long wait_this_time = time - p->time_entered_ready;
            p->total_wait_time += wait_this_time;

            if (p->remaining_cpu_time == 0) {
                p->remaining_cpu_time = p->bursts[p->current_burst].cpu_time;
                p->was_preempted_this_burst = false;
            }
            
            switching_in_process = p;
            p->state = SWITCHING_IN;
            switch_in_end_time = time + T_CS/2;
            cpu_busy_until = switch_in_end_time;
        }
    }
    
    if (terminated_count == n) {
        printf("time %dms: Simulator ended for %s ", cpu_busy_until, algo);
        print_queue(ready_queue);
    }
    
    // --- STATISTICS CALCULATION ---
    FILE* outfile = fopen("simout.txt", "a");
    fprintf(outfile, "\nAlgorithm %s\n", algo);
    double avg_wait_cpu = 0, avg_wait_io = 0, avg_turn_cpu = 0, avg_turn_io = 0;
    int cs_cpu = 0, cs_io = 0, p_cpu = 0, p_io = 0;
    int bursts_cpu = 0, bursts_io = 0;
    int rr_slice_cpu = 0, rr_slice_io = 0;
    int total_bursts_count = 0;
    for(int i=0; i<n; i++){
        total_bursts_count += processes[i].num_bursts;
        if(processes[i].is_cpu_bound){
            bursts_cpu += processes[i].num_bursts;
            avg_wait_cpu += processes[i].total_wait_time;
            avg_turn_cpu += processes[i].total_turnaround_time;
            cs_cpu += processes[i].num_context_switches;
            p_cpu += processes[i].num_preemptions;
            rr_slice_cpu += processes[i].rr_bursts_in_slice;
        } else {
            bursts_io += processes[i].num_bursts;
            avg_wait_io += processes[i].total_wait_time;
            avg_turn_io += processes[i].total_turnaround_time;
            cs_io += processes[i].num_context_switches;
            p_io += processes[i].num_preemptions;
            rr_slice_io += processes[i].rr_bursts_in_slice;
        }
    }
    int final_time = cpu_busy_until;
    fprintf(outfile, "-- CPU utilization: %.3f%%\n", final_time > 0 ? ceil_3dp(100.0 * total_cpu_active_time / final_time) : 0.000);
    fprintf(outfile, "-- CPU-bound average wait time: %.3f ms\n", bursts_cpu ? ceil_3dp(avg_wait_cpu/bursts_cpu) : 0.000);
    fprintf(outfile, "-- I/O-bound average wait time: %.3f ms\n", bursts_io ? ceil_3dp(avg_wait_io/bursts_io) : 0.000);
    fprintf(outfile, "-- overall average wait time: %.3f ms\n", total_bursts_count ? ceil_3dp((avg_wait_cpu+avg_wait_io)/total_bursts_count) : 0.000);
    fprintf(outfile, "-- CPU-bound average turnaround time: %.3f ms\n", bursts_cpu ? ceil_3dp(avg_turn_cpu/bursts_cpu) : 0.000);
    fprintf(outfile, "-- I/O-bound average turnaround time: %.3f ms\n", bursts_io ? ceil_3dp(avg_turn_io/bursts_io) : 0.000);
    fprintf(outfile, "-- overall average turnaround time: %.3f ms\n", total_bursts_count ? ceil_3dp((avg_turn_cpu+avg_turn_io)/total_bursts_count) : 0.000);
    fprintf(outfile, "-- CPU-bound number of context switches: %d\n", cs_cpu);
    fprintf(outfile, "-- I/O-bound number of context switches: %d\n", cs_io);
    fprintf(outfile, "-- overall number of context switches: %d\n", cs_cpu + cs_io);
    fprintf(outfile, "-- CPU-bound number of preemptions: %d\n", p_cpu);
    fprintf(outfile, "-- I/O-bound number of preemptions: %d\n", p_io);
    fprintf(outfile, "-- overall number of preemptions: %d\n", p_cpu + p_io);
    if (strcmp(algo, "RR") == 0) {
        fprintf(outfile, "-- CPU-bound percentage of CPU bursts completed within one time slice: %.3f%%\n",
                bursts_cpu ? ceil_3dp(100.0 * rr_slice_cpu / bursts_cpu) : 0.000);
        fprintf(outfile, "-- I/O-bound percentage of CPU bursts completed within one time slice: %.3f%%\n",
                bursts_io ? ceil_3dp(100.0 * rr_slice_io / bursts_io) : 0.000);
        fprintf(outfile, "-- overall percentage of CPU bursts completed within one time slice: %.3f%%\n",
                total_bursts_count ? ceil_3dp(100.0 * (rr_slice_cpu+rr_slice_io) / total_bursts_count) : 0.000);
    }
    fclose(outfile);
}
//-----------------------------------------------------------------------------
// MAIN FUNCTION
//-----------------------------------------------------------------------------
int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc != 9) {
        fprintf(stderr, "ERROR: Invalid number of command-line arguments. Expected 8.\n");
        return EXIT_FAILURE;
    }
    int n = atoi(argv[1]);
    int ncpu = atoi(argv[2]);
    long seed = atol(argv[3]);
    double lambda = atof(argv[4]);
    int upper_bound = atoi(argv[5]);
    T_CS = atoi(argv[6]);
    ALPHA = atof(argv[7]);
    T_SLICE = atoi(argv[8]);

    if (n <= 0 || n > 260) { fprintf(stderr, "ERROR: The number of processes must be between 1 and 260.\n"); return EXIT_FAILURE; }
    if (ncpu < 0 || ncpu > n) { fprintf(stderr, "ERROR: The number of CPU-bound processes must be between 0 and the total number of processes.\n"); return EXIT_FAILURE; }
    if (lambda <= 0) { fprintf(stderr, "ERROR: Lambda must be a positive value.\n"); return EXIT_FAILURE; }
    if (upper_bound <= 0) { fprintf(stderr, "ERROR: The upper bound must be a positive value.\n"); return EXIT_FAILURE; }
    if (T_CS <= 0 || T_CS % 2 != 0) { fprintf(stderr, "ERROR: Context switch time t_cs must be a positive even integer.\n"); return EXIT_FAILURE; }
    if (ALPHA < 0 || ALPHA > 1) { fprintf(stderr, "ERROR: Alpha must be a value in the range [0, 1].\n"); return EXIT_FAILURE; }
    if (T_SLICE <= 0) { fprintf(stderr, "ERROR: Time slice t_slice must be a positive integer.\n"); return EXIT_FAILURE; }
    
    srand48(seed);
    Process* master_processes = (Process*)malloc(sizeof(Process) * n);
    double initial_tau = 1.0 / lambda;

    long double cpu_bound_total_cpu_time = 0; double cpu_bound_cpu_burst_count = 0;
    long double cpu_bound_total_io_time = 0; double cpu_bound_io_burst_count = 0;
    long double io_bound_total_cpu_time = 0; double io_bound_cpu_burst_count = 0;
    long double io_bound_total_io_time = 0; double io_bound_io_burst_count = 0;

    printf("<<< PROJECT PART I\n");
    printf("<<< -- process set (n=%d) with %d CPU-bound %s\n", n, ncpu, (ncpu == 1) ? "process" : "processes");
    printf("<<< -- seed=%ld; lambda=%.6f; bound=%d\n", seed, lambda, upper_bound);

    for (int i = 0; i < n; i++) {
        Process* p = &master_processes[i];
        p->is_cpu_bound = (i < ncpu);
        sprintf(p->id, "%c%d", 'A' + (i / 10), i % 10);
        p->arrival_time = (int)floor(next_exp(lambda, upper_bound));
        p->num_bursts = (int)ceil(drand48() * 32.0);
        if (p->num_bursts == 0) p->num_bursts = 1;
        p->bursts = (Burst*)malloc(sizeof(Burst) * p->num_bursts);

        printf("%s process %s: arrival time %dms; %d CPU %s\n",
               p->is_cpu_bound ? "CPU-bound" : "I/O-bound",
               p->id, p->arrival_time, p->num_bursts, (p->num_bursts == 1) ? "burst" : "bursts");

        for (int j = 0; j < p->num_bursts; j++) {
            double cpu_time = ceil(next_exp(lambda, upper_bound));
            if (p->is_cpu_bound) cpu_time *= 4;
            p->bursts[j].cpu_time = (int)cpu_time;
            if (p->is_cpu_bound) { cpu_bound_total_cpu_time += cpu_time; cpu_bound_cpu_burst_count++; }
            else { io_bound_total_cpu_time += cpu_time; io_bound_cpu_burst_count++; }

            if (j < p->num_bursts - 1) {
                double io_time_base = ceil(next_exp(lambda, upper_bound));
                p->bursts[j].io_time = p->is_cpu_bound ? (int)io_time_base : (int)io_time_base * 8;
                if (p->is_cpu_bound) { cpu_bound_total_io_time += p->bursts[j].io_time; cpu_bound_io_burst_count++; }
                else { io_bound_total_io_time += p->bursts[j].io_time; io_bound_io_burst_count++; }
            } else {
                p->bursts[j].io_time = 0;
            }
        }
    }
    
    FILE *outfile = fopen("simout.txt", "w");
    if (outfile == NULL) { fprintf(stderr, "ERROR: Could not open file simout.txt for writing.\n"); return EXIT_FAILURE; }
    fprintf(outfile, "-- number of processes: %d\n", n);
    fprintf(outfile, "-- number of CPU-bound processes: %d\n", ncpu);
    fprintf(outfile, "-- number of I/O-bound processes: %d\n", n - ncpu);
    fprintf(outfile, "-- CPU-bound average CPU burst time: %.3f ms\n", cpu_bound_cpu_burst_count ? ceil_3dp(cpu_bound_total_cpu_time / cpu_bound_cpu_burst_count) : 0.000);
    fprintf(outfile, "-- I/O-bound average CPU burst time: %.3f ms\n", io_bound_cpu_burst_count ? ceil_3dp(io_bound_total_cpu_time / io_bound_cpu_burst_count) : 0.000);
    fprintf(outfile, "-- overall average CPU burst time: %.3f ms\n", (cpu_bound_cpu_burst_count + io_bound_cpu_burst_count) ? ceil_3dp((cpu_bound_total_cpu_time + io_bound_total_cpu_time) / (cpu_bound_cpu_burst_count + io_bound_cpu_burst_count)) : 0.000);
    fprintf(outfile, "-- CPU-bound average I/O burst time: %.3f ms\n", cpu_bound_io_burst_count ? ceil_3dp(cpu_bound_total_io_time / cpu_bound_io_burst_count) : 0.000);
    fprintf(outfile, "-- I/O-bound average I/O burst time: %.3f ms\n", io_bound_io_burst_count ? ceil_3dp(io_bound_total_io_time / io_bound_io_burst_count) : 0.000);
    fprintf(outfile, "-- overall average I/O burst time: %.3f ms\n", (cpu_bound_io_burst_count + io_bound_io_burst_count) ? ceil_3dp((cpu_bound_total_io_time + io_bound_total_io_time) / (cpu_bound_io_burst_count + io_bound_io_burst_count)) : 0.000);
    fclose(outfile);
    
    // --- SIMULATIONS (PART II) ---
    printf("\n<<< PROJECT PART II\n");
    printf("<<< -- t_cs=%dms; alpha=%.2f; t_slice=%dms", T_CS, ALPHA, T_SLICE);

    Process* working_processes = (Process*)malloc(sizeof(Process) * n);
    
    reset_simulation_state(master_processes, working_processes, n, initial_tau);
    srand48(seed);
    run_simulation("FCFS", working_processes, n, ncpu, initial_tau);
    
    reset_simulation_state(master_processes, working_processes, n, initial_tau);
    srand48(seed);
    run_simulation("SJF", working_processes, n, ncpu, initial_tau);

    reset_simulation_state(master_processes, working_processes, n, initial_tau);
    srand48(seed);
    run_simulation("SRT", working_processes, n, ncpu, initial_tau);

    reset_simulation_state(master_processes, working_processes, n, initial_tau);
    srand48(seed);
    run_simulation("RR", working_processes, n, ncpu, initial_tau);

    // --- CLEAN UP ---
    free(working_processes);
    for (int i = 0; i < n; i++) {
        free(master_processes[i].bursts);
    }
    free(master_processes);

    return EXIT_SUCCESS;
}
