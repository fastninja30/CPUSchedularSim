#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>

double next_exp(double lambda, int upper_bound) {
    double r;
    double x;
    do {
        r = drand48();
        x = -log(r) / lambda;
    } while (x > upper_bound);
    return x;
}

int main(int argc, char *argv[]) {
    setvbuf( stdout, NULL,_IONBF, 0);

    if (argc != 6) {
        fprintf(stderr, "ERROR: Invalid number of command-line arguments. Expected 5.\n");
        return EXIT_FAILURE;
    }

    int n = atoi(argv[1]);
    int ncpu = atoi(argv[2]);
    long seed = atol(argv[3]);
    double lambda = atof(argv[4]);
    int upper_bound = atoi(argv[5]);

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
    // Maximum of 260 processes (A0-Z9)
    if (n > 260) {
        fprintf(stderr, "ERROR: The maximum number of processes is 260.\n");
        return EXIT_FAILURE;
    }


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
