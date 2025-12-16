#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sim_bp.h"

int main (int argc, char* argv[])
{
    FILE *FP;
    char *trace_file;
    bp_params params;
    char outcome;
    unsigned long int addr;
    
    if (!(argc == 4 || argc == 5 || argc == 7))
    {
        printf("Error: Wrong number of inputs:%d\n", argc-1);
        exit(EXIT_FAILURE);
    }
    
    params.bp_name  = argv[1];
    
    if(strcmp(params.bp_name, "bimodal") == 0)
    {
        if(argc != 4)
        {
            printf("Error: %s wrong number of inputs:%d\n", params.bp_name, argc-1);
            exit(EXIT_FAILURE);
        }
        params.M2       = strtoul(argv[2], NULL, 10);
        trace_file      = argv[3];
        printf("COMMAND\n%s %s %lu %s\n", argv[0], params.bp_name, params.M2, trace_file);
    }
    else if(strcmp(params.bp_name, "gshare") == 0)
    {
        if(argc != 5)
        {
            printf("Error: %s wrong number of inputs:%d\n", params.bp_name, argc-1);
            exit(EXIT_FAILURE);
        }
        params.M1       = strtoul(argv[2], NULL, 10);
        params.N        = strtoul(argv[3], NULL, 10);
        trace_file      = argv[4];
        printf("COMMAND\n%s %s %lu %lu %s\n", argv[0], params.bp_name, params.M1, params.N, trace_file);

    }
    else if(strcmp(params.bp_name, "hybrid") == 0)
    {
        if(argc != 7)
        {
            printf("Error: %s wrong number of inputs:%d\n", params.bp_name, argc-1);
            exit(EXIT_FAILURE);
        }
        params.K        = strtoul(argv[2], NULL, 10);
        params.M1       = strtoul(argv[3], NULL, 10);
        params.N        = strtoul(argv[4], NULL, 10);
        params.M2       = strtoul(argv[5], NULL, 10);
        trace_file      = argv[6];
        printf("COMMAND\n%s %s %lu %lu %lu %lu %s\n", argv[0], params.bp_name, params.K, params.M1, params.N, params.M2, trace_file);

    }
    else
    {
        printf("Error: Wrong branch predictor name:%s\n", params.bp_name);
        exit(EXIT_FAILURE);
    }
    
    FP = fopen(trace_file, "r");
    if(FP == NULL)
    {
        printf("Error: Unable to open file %s\n", trace_file);
        exit(EXIT_FAILURE);
    }
    
    unsigned int *bimodal_table = NULL;
    unsigned int *gshare_table = NULL;
    unsigned int *chooser_table = NULL;
    unsigned int ghr = 0;
    unsigned int predictions = 0;
    unsigned int mispredictions = 0;
    
    if(strcmp(params.bp_name, "bimodal") == 0) {
        unsigned int size = 1 << params.M2;
        bimodal_table = (unsigned int*)malloc(size * sizeof(unsigned int));
        for(unsigned int i = 0; i < size; i++) bimodal_table[i] = 2;
    } else if(strcmp(params.bp_name, "gshare") == 0) {
        unsigned int size = 1 << params.M1;
        gshare_table = (unsigned int*)malloc(size * sizeof(unsigned int));
        for(unsigned int i = 0; i < size; i++) gshare_table[i] = 2;
    } else if(strcmp(params.bp_name, "hybrid") == 0) {
        unsigned int size_chooser = 1 << params.K;
        unsigned int size_gshare = 1 << params.M1;
        unsigned int size_bimodal = 1 << params.M2;
        chooser_table = (unsigned int*)malloc(size_chooser * sizeof(unsigned int));
        gshare_table = (unsigned int*)malloc(size_gshare * sizeof(unsigned int));
        bimodal_table = (unsigned int*)malloc(size_bimodal * sizeof(unsigned int));
        for(unsigned int i = 0; i < size_chooser; i++) chooser_table[i] = 1;
        for(unsigned int i = 0; i < size_gshare; i++) gshare_table[i] = 2;
        for(unsigned int i = 0; i < size_bimodal; i++) bimodal_table[i] = 2;
    }
    
    char str[2];
    while(fscanf(FP, "%lx %s", &addr, str) != EOF)
    {
        outcome = str[0];
        int taken = (outcome == 't') ? 1 : 0;
        int prediction = 0;
        
        if(strcmp(params.bp_name, "bimodal") == 0) {
            unsigned int idx = (addr >> 2) & ((1 << params.M2) - 1);
            prediction = (bimodal_table[idx] >= 2) ? 1 : 0;
            if(taken) {
                if(bimodal_table[idx] < 3) bimodal_table[idx]++;
            } else {
                if(bimodal_table[idx] > 0) bimodal_table[idx]--;
            }
        } else if(strcmp(params.bp_name, "gshare") == 0) {
            if(params.N == 0) {
                unsigned int idx = (addr >> 2) & ((1 << params.M1) - 1);
                prediction = (gshare_table[idx] >= 2) ? 1 : 0;
                if(taken) {
                    if(gshare_table[idx] < 3) gshare_table[idx]++;
                } else {
                    if(gshare_table[idx] > 0) gshare_table[idx]--;
                }
            } else {
                unsigned int pc_bits = (addr >> 2) & ((1 << params.M1) - 1);
                unsigned int pc_upper = pc_bits >> (params.M1 - params.N);
                unsigned int pc_lower = pc_bits & ((1 << (params.M1 - params.N)) - 1);
                unsigned int idx = ((pc_upper ^ ghr) << (params.M1 - params.N)) | pc_lower;
                prediction = (gshare_table[idx] >= 2) ? 1 : 0;
                if(taken) {
                    if(gshare_table[idx] < 3) gshare_table[idx]++;
                } else {
                    if(gshare_table[idx] > 0) gshare_table[idx]--;
                }
                ghr = ((ghr >> 1) | ((taken ? 1 : 0) << (params.N - 1))) & ((1 << params.N) - 1);
            }
        } else if(strcmp(params.bp_name, "hybrid") == 0) {
            int gshare_pred;
            unsigned int idx_gshare;
            
            if(params.N == 0) {
                idx_gshare = (addr >> 2) & ((1 << params.M1) - 1);
                gshare_pred = (gshare_table[idx_gshare] >= 2) ? 1 : 0;
            } else {
                unsigned int pc_bits_gshare = (addr >> 2) & ((1 << params.M1) - 1);
                unsigned int pc_upper = pc_bits_gshare >> (params.M1 - params.N);
                unsigned int pc_lower = pc_bits_gshare & ((1 << (params.M1 - params.N)) - 1);
                idx_gshare = ((pc_upper ^ ghr) << (params.M1 - params.N)) | pc_lower;
                gshare_pred = (gshare_table[idx_gshare] >= 2) ? 1 : 0;
            }
            
            unsigned int idx_bimodal = (addr >> 2) & ((1 << params.M2) - 1);
            int bimodal_pred = (bimodal_table[idx_bimodal] >= 2) ? 1 : 0;
            
            unsigned int idx_chooser = (addr >> 2) & ((1 << params.K) - 1);
            prediction = (chooser_table[idx_chooser] >= 2) ? gshare_pred : bimodal_pred;
            
            if(chooser_table[idx_chooser] >= 2) {
                if(taken) {
                    if(gshare_table[idx_gshare] < 3) gshare_table[idx_gshare]++;
                } else {
                    if(gshare_table[idx_gshare] > 0) gshare_table[idx_gshare]--;
                }
            } else {
                if(taken) {
                    if(bimodal_table[idx_bimodal] < 3) bimodal_table[idx_bimodal]++;
                } else {
                    if(bimodal_table[idx_bimodal] > 0) bimodal_table[idx_bimodal]--;
                }
            }
            
            if(params.N > 0) {
                ghr = ((ghr >> 1) | ((taken ? 1 : 0) << (params.N - 1))) & ((1 << params.N) - 1);
            }
            
            int gshare_correct = (gshare_pred == taken);
            int bimodal_correct = (bimodal_pred == taken);
            
            if(gshare_correct && !bimodal_correct) {
                if(chooser_table[idx_chooser] < 3) chooser_table[idx_chooser]++;
            } else if(!gshare_correct && bimodal_correct) {
                if(chooser_table[idx_chooser] > 0) chooser_table[idx_chooser]--;
            }
        }
        
        predictions++;
        if(prediction != taken) mispredictions++;
    }
    
    printf("OUTPUT\n");
    printf("number of predictions:\t\t%u\n", predictions);
    printf("number of mispredictions:\t%u\n", mispredictions);
    printf("misprediction rate:\t\t%.2f%%\n", (double)mispredictions / predictions * 100);
    
    if(strcmp(params.bp_name, "bimodal") == 0) {
        printf("FINAL BIMODAL CONTENTS\n");
        unsigned int size = 1 << params.M2;
        for(unsigned int i = 0; i < size; i++) {
            printf("%d\t%d\n", i, bimodal_table[i]);
        }
        free(bimodal_table);
    } else if(strcmp(params.bp_name, "gshare") == 0) {
        printf("FINAL GSHARE CONTENTS\n");
        unsigned int size = 1 << params.M1;
        for(unsigned int i = 0; i < size; i++) {
            printf("%d\t%d\n", i, gshare_table[i]);
        }
        free(gshare_table);
    } else if(strcmp(params.bp_name, "hybrid") == 0) {
        printf("FINAL CHOOSER CONTENTS\n");
        unsigned int size = 1 << params.K;
        for(unsigned int i = 0; i < size; i++) {
            printf("%d\t%d\n", i, chooser_table[i]);
        }
        printf("FINAL GSHARE CONTENTS\n");
        size = 1 << params.M1;
        for(unsigned int i = 0; i < size; i++) {
            printf("%d\t%d\n", i, gshare_table[i]);
        }
        printf("FINAL BIMODAL CONTENTS\n");
        size = 1 << params.M2;
        for(unsigned int i = 0; i < size; i++) {
            printf("%d\t%d\n", i, bimodal_table[i]);
        }
        free(chooser_table);
        free(gshare_table);
        free(bimodal_table);
    }
    
    fclose(FP);
    return 0;
}