    #include <stdio.h>
    #include <stdlib.h>
    #include <string.h>
    #include <math.h>
    #include <stdbool.h>
    
    #define EVT_TIME_SLICE EVT_PREEMPT
    #define MAX_EVENTS 10000
    #define MAX_PROCS 260
    
    // --- Part I random generator ---
    double next_exp(double lambda, int upper_bound) {
        double r, x;
        do {
            r = drand48();
            x = -log(r) / lambda;
        } while (x > upper_bound);
        return x;
    }
    
    // --- Part II types ---
    typedef enum { FCFS, SJF, SRT, RR } alg_t;
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
    
    typedef struct {
        char       id[3];
        int        arrival_time;
        int        num_bursts;
        long      *cpu_bursts;
        long      *io_bursts;
        int        cur_burst;
        long       remaining_time;
        double     tau;
        bool       is_cpu_bound;
    } process_t;
    
    typedef struct {
        long          time;
        event_type_t  type;
        process_t    *proc;
    } event_t;
    
    // --- Globals ---
    static int     n, ncpu;
    static long    seed;
    static double  lambda;
    static int     upper_bound;
    static int     tcs;
    static double  alpha;
    static int     tslice;
    static long    t;  // current time
    static alg_t   current_alg;
    static process_t procs_orig[MAX_PROCS];
    static process_t procs_copy[MAX_PROCS];
    static process_t *running = NULL;
    static long last_start_time;
    
    // --- Event heap ---
    static event_t event_heap[MAX_EVENTS];
    static int     event_size = 0;
    
    static int event_rank(event_type_t type) {
        switch(type) {
            case EVT_SIM_START:  return 0;
            case EVT_CPU_FINISH: return 1;
            case EVT_CPU_START:  return 2;
            case EVT_IO_FINISH:  return 3;
            case EVT_ARRIVAL:    return 4;
            case EVT_PREEMPT:    return 5;
            case EVT_IO_START:   return 6;
            case EVT_SIM_END:    return 7;
            default:             return 8;
        }
    }
    
    static int event_cmp(const event_t *a, const event_t *b) {
        if (a->time < b->time) return -1;
        if (a->time > b->time) return 1;
        int ra = event_rank(a->type), rb = event_rank(b->type);
        if (ra < rb) return -1;
        if (ra > rb) return 1;
        if (!a->proc || !b->proc) return 0;
        return strcmp(a->proc->id, b->proc->id);
    }
    
    void event_heap_clear(void) { event_size = 0; }
    bool event_heap_empty(void) { return event_size == 0; }
    void push_event(event_t ev) {
        if (event_size >= MAX_EVENTS) { fprintf(stderr,"Heap overflow\n"); exit(1); }
        int i = event_size++;
        event_heap[i] = ev;
        while (i > 0) {
            int p = (i-1)/2;
            if (event_cmp(&event_heap[i],&event_heap[p])<0) { event_t tmp=event_heap[i]; event_heap[i]=event_heap[p]; event_heap[p]=tmp; i=p; }
            else break;
        }
    }
    event_t pop_event(void) {
        if (event_size==0){fprintf(stderr,"Heap empty\n");exit(1);}    
        event_t root=event_heap[0]; event_heap[0]=event_heap[--event_size];
        int i=0;
        while (1) {
            int l=2*i+1, r=2*i+2, best=i;
            if (l<event_size && event_cmp(&event_heap[l],&event_heap[best])<0) best=l;
            if (r<event_size && event_cmp(&event_heap[r],&event_heap[best])<0) best=r;
            if (best==i) break;
            event_t tmp=event_heap[i]; event_heap[i]=event_heap[best]; event_heap[best]=tmp; i=best;
        }
        return root;
    }
    
    // --- Ready queues ---
    static process_t *ready_fifo[MAX_PROCS];
    static int         fifo_head=0, fifo_tail=0, fifo_count=0;
    static process_t *ready_pq[MAX_PROCS];
    static int         ready_pq_size=0;
    
    static int ready_cmp(process_t *a, process_t *b, alg_t alg) {
        if (alg==SJF) { if (a->tau<b->tau) return -1; if (a->tau>b->tau) return 1; }
        else if (alg==SRT) { if (a->remaining_time<b->remaining_time) return -1; if (a->remaining_time>b->remaining_time) return 1; }
        return strcmp(a->id,b->id);
    }
    
    void clear_ready_queue(alg_t alg) {
        fifo_head=fifo_tail=fifo_count=0; ready_pq_size=0;
    }
    bool ready_empty(alg_t alg) {
        return (alg==FCFS||alg==RR) ? fifo_count==0 : ready_pq_size==0;
    }
    void enqueue_ready(process_t *p, alg_t alg) {
        if (alg==FCFS||alg==RR) {
            ready_fifo[fifo_tail]=p; fifo_tail=(fifo_tail+1)%MAX_PROCS; fifo_count++;
        } else {
            int i=ready_pq_size++; ready_pq[i]=p;
            while(i>0){int par=(i-1)/2; if(ready_cmp(ready_pq[i],ready_pq[par],alg)<0){process_t*tmp=ready_pq[i];ready_pq[i]=ready_pq[par];ready_pq[par]=tmp;i=par;} else break;}
        }
    }
    process_t* next_proc_from_ready(alg_t alg) {
        if(alg==FCFS||alg==RR){process_t*p=ready_fifo[fifo_head];fifo_head=(fifo_head+1)%MAX_PROCS;fifo_count--;return p;} 
        process_t*root=ready_pq[0]; ready_pq[0]=ready_pq[--ready_pq_size]; int i=0;
        while(1){int l=2*i+1,r=2*i+2,b=i; if(l<ready_pq_size&&ready_cmp(ready_pq[l],ready_pq[b],alg)<0)b=l; if(r<ready_pq_size&&ready_cmp(ready_pq[r],ready_pq[b],alg)<0)b=r; if(b==i)break;process_t*tmp=ready_pq[i];ready_pq[i]=ready_pq[b];ready_pq[b]=tmp;i=b;} return root;
    }
    char* ready_queue_to_string(alg_t alg) {
    char *buf = malloc(MAX_PROCS*4 + 1);
    if (!buf) {
        fprintf(stderr, "ERROR: malloc failed in ready_queue_to_string\n");
        exit(EXIT_FAILURE);
    }
    buf[0] = '\0';

    if (ready_empty(alg)) {
        strcpy(buf, "empty");
        return buf;
    }

    if (alg == FCFS || alg == RR) {
        int idx = fifo_head;
        for (int i = 0; i < fifo_count; i++) {
            if (!ready_fifo[idx]) {
                fprintf(stderr,
                    "ERROR: NULL pointer in ready_fifo at idx=%d (fifo_count=%d, fifo_head=%d)\n",
                    idx, fifo_count, fifo_head);
                exit(EXIT_FAILURE);
            }
            strcat(buf, ready_fifo[idx]->id);
            if (i < fifo_count - 1) strcat(buf, " ");
            idx = (idx + 1) % MAX_PROCS;
        }
    } else {
        int sz = ready_pq_size;
        // copy pointers to temp array
        process_t *copy[MAX_PROCS];
        for (int i = 0; i < sz; i++) {
            if (!ready_pq[i]) {
                fprintf(stderr,
                    "ERROR: NULL pointer in ready_pq at i=%d (ready_pq_size=%d)\n",
                    i, ready_pq_size);
                exit(EXIT_FAILURE);
            }
            copy[i] = ready_pq[i];
        }
        int total = sz;
        for (int i = 0; i < total; i++) {
            process_t *p = copy[0];
            copy[0] = copy[--sz];
            int j = 0;
            // heapify down
            while (1) {
                int l = 2*j + 1, r = 2*j + 2, best = j;
                if (l < sz && ready_cmp(copy[l], copy[best], alg) < 0) best = l;
                if (r < sz && ready_cmp(copy[r], copy[best], alg) < 0) best = r;
                if (best == j) break;
                process_t *tmp = copy[j];
                copy[j] = copy[best];
                copy[best] = tmp;
                j = best;
            }
            strcat(buf, p->id);
            if (i < total - 1) strcat(buf, " ");
        }
    }

    return buf;
}

    
    // --- Context-switch helper ---
    void schedule_cpu_start(process_t *p) {
        push_event((event_t){.time=t+tcs/2,.type=EVT_CPU_START,.proc=p});
    }
    
    // --- Utilities ---
    void copy_initial_state(process_t dest[]) {
        for(int i=0;i<n;i++) dest[i]=procs_orig[i];
        t=0;
    }
    const char* alg_name(alg_t alg) {
        return (alg==FCFS?"FCFS":alg==SJF?"SJF":alg==SRT?"SRT":"RR");
    }
    bool cpu_idle(void) {
        // CPU idle if no process is currently running: check next event not CPU_START?
        return running == NULL; // simplified: simulate always schedules
    }
    bool all_done(void) {
        // Check if all processes have cur_burst==num_bursts
        for(int i=0;i<n;i++) if(procs_copy[i].cur_burst<procs_copy[i].num_bursts) return false;
        return true;
    }
    
    // --- Handlers ---
    void handle_sim_start(process_t *p, alg_t alg){
        // Print startup and show whatever is in the ready queue
        char *Q = ready_queue_to_string(alg);
        printf("time %ldms: Simulator started for %s [Q %s]\n",
            t, alg_name(alg), strcmp(Q,"empty")==0 ? "empty" : Q);
        free(Q);

        // use ready_empty() to decide whether to pop,
        // not whether the string said “empty.”
        if (!ready_empty(alg)) {
            process_t *next = next_proc_from_ready(alg);
            schedule_cpu_start(next);
        }
    }
    void handle_arrival(process_t *p, alg_t alg) {
        // 1a: enqueue & print
        enqueue_ready(p, alg);
        char *Q = ready_queue_to_string(alg);
        if (alg == SJF || alg == SRT) {
            printf("time %ldms: Process %s (tau %.0fms) arrived; added to ready queue [Q %s]\n",
                t, p->id, p->tau, Q);
        } else {
            printf("time %ldms: Process %s arrived; added to ready queue [Q %s]\n",
                t, p->id, Q);
        }
        free(Q);

        // 1b: SRT preempt-on-arrival
        if (alg == SRT && running && p->tau < running->remaining_time) {
            // preempt current immediately
            push_event((event_t){ .time = t, .type = EVT_PREEMPT, .proc = running });
        }

        // 1c: if CPU idle, start next
        if (cpu_idle()) {
            process_t *next = next_proc_from_ready(alg);
            schedule_cpu_start(next);
        }
    }

    // 2) EVT_CPU_START
    void handle_cpu_start(process_t *p, alg_t alg) {
        // mark running & record start
        running = p;
        last_start_time = t;

        // print
        char *Q = ready_queue_to_string(alg);
        if (alg == SJF || alg == SRT) {
            printf("time %ldms: Process %s (tau %.0fms) started using the CPU for %ldms burst [Q %s]\n",
                t, p->id, p->tau, p->remaining_time, Q);
        } else {
            printf("time %ldms: Process %s started using the CPU for %ldms burst [Q %s]\n",
                t, p->id, p->remaining_time, Q);
        }
        free(Q);

        // schedule finish
        push_event((event_t){
            .time = t + p->remaining_time,
            .type = EVT_CPU_FINISH,
            .proc = p
        });
        // RR time‐slice
        if (alg == RR && tslice < p->remaining_time) {
            push_event((event_t){
                .time = t + tslice,
                .type = EVT_PREEMPT,
                .proc = p
            });
        }
    }

    // 3) EVT_CPU_FINISH
    void handle_cpu_finish(process_t *p, alg_t alg) {
        // 1) clear running immediately
        running = NULL;

        // 2) “completed a CPU burst” at exactly t
        char *Q1 = ready_queue_to_string(alg);
        int bursts_left = p->num_bursts - p->cur_burst - 1;
        if (alg == SJF || alg == SRT) {
            printf("time %ldms: Process %s (tau %.0fms) completed a CPU burst; %d bursts to go [Q %s]\n",
                t, p->id, p->tau, bursts_left, Q1);
        } else {
            printf("time %ldms: Process %s completed a CPU burst; %d bursts to go [Q %s]\n",
                t, p->id, bursts_left, Q1);
        }
        free(Q1);

        // 3) recalc tau (still at time t)
        if (alg == SJF || alg == SRT) {
            double old = p->tau;
            double actual = (double)p->cpu_bursts[p->cur_burst];
            p->tau = ceil(alpha*actual + (1 - alpha)*old);

            char *Q2 = ready_queue_to_string(alg);
            printf("time %ldms: Recalculated tau for process %s: old tau %.0fms ==> new tau %.0fms [Q %s]\n",
                t, p->id, old, p->tau, Q2);
            free(Q2);
        }

        // 4) advance burst index
        p->cur_burst++;

        // 5) if that was the last burst, terminate
        if (bursts_left == 0) {
            char *Q3 = ready_queue_to_string(alg);
            printf("time %ldms: Process %s terminated [Q %s]\n",
                t, p->id, Q3);
            free(Q3);
            t += tcs/2;
            if (all_done()) {
                push_event((event_t){ .time = t, .type = EVT_SIM_END, .proc = NULL });
            }
            return;
        }

        // 6) “switching out of CPU; blocking on I/O” at the same t
        long io_finish = t + p->io_bursts[p->cur_burst - 1];
        char *Q4 = ready_queue_to_string(alg);
        printf("time %ldms: Process %s switching out of CPU; blocking on I/O until time %ldms [Q %s]\n",
            t, p->id, io_finish, Q4);
        free(Q4);

        // 7) incur the first half of the context switch
        t += tcs/2;

        // 8) schedule I/O start and finish at t and t+io
        push_event((event_t){ .time = t,         .type = EVT_IO_START,  .proc = p });
        push_event((event_t){ .time = io_finish, .type = EVT_IO_FINISH, .proc = p });

        // 9) immediately dispatch the next ready process with the second half
        if (!ready_empty(alg)) {
            process_t *next = next_proc_from_ready(alg);
            // here we want CPU_START at t + tcs/2, so add that explicitly:
            push_event((event_t){
            .time  = t + tcs/2,
            .type  = EVT_CPU_START,
            .proc  = next
            });
    }
    }

    // 4) EVT_PREEMPT
    void handle_preempt(process_t *p, alg_t alg) {
        // clear running, measure run, incur half‐tcs
        running = NULL;
        long ran = t - last_start_time;
        p->remaining_time -= ran;
        t += tcs / 2;

        // requeue & print
        enqueue_ready(p, alg);
        char *Q = ready_queue_to_string(alg);
        printf("time %ldms: Process %s preempted; added to ready queue [Q %s]\n",
            t, p->id, Q);
        free(Q);

        // dispatch next
        process_t *next = next_proc_from_ready(alg);
        if (alg == RR && next == p) {
            // if RR and queue was empty, just restart p
            push_event((event_t){ .time = t, .type = EVT_CPU_START, .proc = p });
        } else {
            schedule_cpu_start(next);
        }
    }

    // 5) EVT_IO_START
    void handle_io_start(process_t *p, alg_t alg) {
        long io_finish = t + p->io_bursts[p->cur_burst - 1];
        char *Q = ready_queue_to_string(alg);
        printf("time %ldms: Process %s starting I/O burst; will finish at %ldms [Q %s]\n",
            t, p->id, io_finish, Q);
        free(Q);
    }

    // 6) EVT_IO_FINISH
    void handle_io_finish(process_t *p, alg_t alg) {
        enqueue_ready(p, alg);
        char *Q = ready_queue_to_string(alg);
        printf("time %ldms: Process %s completed I/O; added to ready queue [Q %s]\n",
            t, p->id, Q);
        free(Q);

        if (cpu_idle()) {
            process_t *next = next_proc_from_ready(alg);
            schedule_cpu_start(next);
        }
    }
    
    // --- Simulation ---
    void simulate(alg_t alg) {
        current_alg=alg;
        copy_initial_state(procs_copy);
        event_heap_clear(); clear_ready_queue(alg);
        for(int i=0;i<n;i++) push_event((event_t){.time=procs_copy[i].arrival_time,.type=EVT_ARRIVAL,.proc=&procs_copy[i]});
        push_event((event_t){.time=0,.type=EVT_SIM_START,.proc=NULL});
        while(!event_heap_empty()){
            event_t ev=pop_event(); t=ev.time;
            switch(ev.type) {
                case EVT_SIM_START: handle_sim_start(ev.proc,alg); break;
                case EVT_ARRIVAL:   handle_arrival(ev.proc,alg); break;
                case EVT_CPU_START: handle_cpu_start(ev.proc,alg); break;
                case EVT_CPU_FINISH:handle_cpu_finish(ev.proc,alg); break;
                case EVT_PREEMPT:   handle_preempt(ev.proc,alg); break;
                case EVT_IO_START:  handle_io_start(ev.proc,alg); break;
                case EVT_IO_FINISH: handle_io_finish(ev.proc,alg); break;
                case EVT_SIM_END:   printf("time %ldms: Simulator ended for %s [Q empty]\n\n",t,alg_name(alg)); return;
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
    n = atoi(argv[1]);
    ncpu = atoi(argv[2]);
    seed = atol(argv[3]);
    lambda = atof(argv[4]);
    upper_bound = atoi(argv[5]);


    // Part II
    tcs = atoi(argv[6]);
    alpha = atof(argv[7]);
    tslice = atoi(argv[8]);

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
        fprintf(stderr, "ERROR: Time to context switch must be a positive even integer.\n");
    }
    if (alpha <= 0.0 || alpha > 1.0) {
        fprintf(stderr, "ERROR: Alpha must be between 0 and 1.\n");
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
        // 1) ID
        sprintf(procs_orig[i].id, "%c%d", 'A' + (i/10), i%10);

        procs_orig[i].arrival_time = (int)floor(next_exp(lambda, upper_bound));

        // 3) number of bursts
        int B = (int)ceil(drand48() * 32.0);
        if (B == 0) B = 1;
        procs_orig[i].num_bursts = B;

        // 4) allocate arrays
        procs_orig[i].cpu_bursts = malloc(sizeof(long)*B);
        procs_orig[i].io_bursts  = malloc(sizeof(long)*(B-1));
        procs_orig[i].cur_burst  = 0;
        procs_orig[i].tau        = 1.0 / lambda;
        procs_orig[i].is_cpu_bound = (i < ncpu);

        printf("%s process %s: arrival time %dms; %d CPU %s:\n",
            procs_orig[i].is_cpu_bound ? "CPU-bound" : "I/O-bound",
            procs_orig[i].id,
            procs_orig[i].arrival_time,
            B, B==1 ? "burst" : "bursts"
        );
        // Generate CPU bursts and accumulate
        for (int j = 0; j < B; j++) {
            long bt = (long)ceil(next_exp(lambda, upper_bound));
            if (procs_orig[i].is_cpu_bound) bt *= 4;
            procs_orig[i].cpu_bursts[j] = bt;
            if (procs_orig[i].is_cpu_bound) {
                cpu_bound_total_cpu_time += bt;
                cpu_bound_cpu_burst_count++;
            } else {
                io_bound_total_cpu_time += bt;
                io_bound_cpu_burst_count++;
            }
            // Generate I/O bursts (one fewer) and accumulate
            if (j < B - 1) {
                long io = (long)ceil(next_exp(lambda, upper_bound));
                io *= 8;
                if (procs_orig[i].is_cpu_bound) io /= 8;
                procs_orig[i].io_bursts[j] = io;
                if (procs_orig[i].is_cpu_bound) {
                    cpu_bound_total_io_time += io;
                    cpu_bound_io_burst_count++;
                } else {
                    io_bound_total_io_time += io;
                    io_bound_io_burst_count++;
                }
            }        
        }
        procs_orig[i].remaining_time = procs_orig[i].cpu_bursts[0];
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


    // <-------------------------------  PART II ------------------------------->
    printf("<<< PROJECT PART II\n");
    printf("<<< -- t_cs=%dms; alpha=%.2f; t_slice=%dms\n",tcs, alpha, tslice);
    simulate(FCFS);
    //for(alg_t a=FCFS;a<=RR;a++) simulate(a);

    // Free
    for (int i=0;i<n;i++) {
        free(procs_orig[i].cpu_bursts);
        free(procs_orig[i].io_bursts);
    }
    return EXIT_SUCCESS;
}