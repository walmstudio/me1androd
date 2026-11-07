/* CPU profiling implementation for me1-token Stage 8 optimization.
 * Lightweight instrumentation for hot function detection.
 */

#include "cpu_profile.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Global profiling state */
CpuProfEntry cpu_prof_table[CPU_PROF_MAX_FUNCTIONS];
uint32_t cpu_prof_count = 0;

/* Name to index mapping (simple linear search, OK for small N) */
static int cpu_prof_find_index(const char* name) {
    for (uint32_t i = 0; i < cpu_prof_count; i++) {
        if (strcmp(cpu_prof_table[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

void cpu_prof_init(void) {
    memset(cpu_prof_table, 0, sizeof(cpu_prof_table));
    cpu_prof_count = 0;
}

void cpu_prof_register(const char* name) {
    if (cpu_prof_count >= CPU_PROF_MAX_FUNCTIONS) return;
    int idx = cpu_prof_find_index(name);
    if (idx >= 0) return; /* Already registered */
    
    cpu_prof_table[cpu_prof_count].name = name;
    cpu_prof_table[cpu_prof_count].total_cycles = 0;
    cpu_prof_table[cpu_prof_count].call_count = 0;
    cpu_prof_table[cpu_prof_count].min_cycles = UINT64_MAX;
    cpu_prof_table[cpu_prof_count].max_cycles = 0;
    cpu_prof_count++;
}

void cpu_prof_dump(const char* filepath) {
    FILE* f = fopen(filepath, "w");
    if (!f) return;
    
    fprintf(f, "# CPU Profile Data\n");
    fprintf(f, "# Format: name,total_cycles,call_count,avg_cycles,min_cycles,max_cycles\n");
    
    /* Sort by total cycles descending (simple bubble sort for small N) */
    for (uint32_t i = 0; i < cpu_prof_count; i++) {
        for (uint32_t j = i + 1; j < cpu_prof_count; j++) {
            if (cpu_prof_table[j].total_cycles > cpu_prof_table[i].total_cycles) {
                CpuProfEntry tmp = cpu_prof_table[i];
                cpu_prof_table[i] = cpu_prof_table[j];
                cpu_prof_table[j] = tmp;
            }
        }
    }
    
    for (uint32_t i = 0; i < cpu_prof_count; i++) {
        if (cpu_prof_table[i].call_count == 0) continue;
        double avg = (double)cpu_prof_table[i].total_cycles / (double)cpu_prof_table[i].call_count;
        fprintf(f, "%s,%llu,%llu,%.2f,%llu,%llu\n",
                cpu_prof_table[i].name,
                (unsigned long long)cpu_prof_table[i].total_cycles,
                (unsigned long long)cpu_prof_table[i].call_count,
                avg,
                (unsigned long long)cpu_prof_table[i].min_cycles,
                (unsigned long long)cpu_prof_table[i].max_cycles);
    }
    
    fclose(f);
}

void cpu_prof_reset(void) {
    for (uint32_t i = 0; i < cpu_prof_count; i++) {
        cpu_prof_table[i].total_cycles = 0;
        cpu_prof_table[i].call_count = 0;
        cpu_prof_table[i].min_cycles = UINT64_MAX;
        cpu_prof_table[i].max_cycles = 0;
    }
}