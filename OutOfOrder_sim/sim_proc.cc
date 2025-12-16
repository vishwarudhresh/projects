#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <iomanip>
#include <climits>
#include "sim_proc.h"

using namespace std;

struct instruction {
    uint64_t pc;
    int op_type;
    int dest, src1, src2;
    int seq_num;
    int dest_tag;
    int src1_tag, src2_tag;
    bool src1_ready, src2_ready;

    int fe_begin, fe_duration;
    int de_begin, de_duration;
    int rn_begin, rn_duration;
    int rr_begin, rr_duration;
    int di_begin, di_duration;
    int is_begin, is_duration;
    int ex_begin, ex_duration;
    int wb_begin, wb_duration;
    int rt_begin, rt_duration;

    int exec_timer;
    int exec_latency;

    instruction() {
        pc = 0;
        op_type = 0;
        dest = src1 = src2 = -1;
        seq_num = -1;
        dest_tag = src1_tag = src2_tag = -1;
        src1_ready = src2_ready = false;

        fe_begin = fe_duration = -1;
        de_begin = de_duration = -1;
        rn_begin = rn_duration = -1;
        rr_begin = rr_duration = -1;
        di_begin = di_duration = -1;
        is_begin = is_duration = -1;
        ex_begin = ex_duration = -1;
        wb_begin = wb_duration = -1;
        rt_begin = rt_duration = -1;

        exec_timer = 0;
        exec_latency = 0;
    }
};

struct rob_entry {
    bool valid, ready;
    int dest_reg;
    instruction *inst;
    rob_entry() {
        valid = false;
        ready = false;
        dest_reg = -1;
        inst = NULL;
    }
};

int current_cycle = 0;
int instruction_counter = 0;

vector<instruction*> DE, RN, RR, DI, IQ, EX_list, WB;
vector<rob_entry> ROB;
int rob_head = 0, rob_tail = 0, rob_count = 0;

int rename_table[67];

int ROB_SIZE, IQ_SIZE, WIDTH;

ifstream trace;
bool trace_done = false;
vector<instruction*> completed;



inline int get_latency(int op) {
    switch(op) {
        case 0: return 1;
        case 1: return 2;
        case 2: return 5;
        default: return 1;  
    }
}

/********************** FETCH *************************/

void Fetch() {
    
    if (trace_done || !DE.empty()) return;

    
    for (int fetch_idx = 0; fetch_idx < WIDTH; fetch_idx++) {
        uint64_t pc;
        int op, dst, s1, s2;
        
        
        if (!(trace >> hex >> pc >> dec >> op >> dst >> s1 >> s2)) {
            trace_done = true;
            break;
        }

        
        instruction *new_inst = new instruction();
        new_inst->pc = pc;
        new_inst->op_type = op;
        new_inst->dest = dst;
        new_inst->src1 = s1;
        new_inst->src2 = s2;
        new_inst->seq_num = instruction_counter++;

        
        new_inst->fe_begin = current_cycle;
        new_inst->fe_duration = 1;

        new_inst->de_begin = new_inst->fe_begin + new_inst->fe_duration;

        DE.push_back(new_inst);
    }
}

/********************** DECODE *************************/

void Decode() {
    
    if (DE.empty() || !RN.empty()) return;

    
    for (instruction *dec_inst : DE) {
        dec_inst->de_duration = (current_cycle - dec_inst->de_begin) + 1;
        dec_inst->rn_begin = dec_inst->de_begin + dec_inst->de_duration;
        RN.push_back(dec_inst);
    }
    DE.clear();
}

/********************** RENAME *************************/

void Rename() {
    if (RN.empty() || !RR.empty()) return;
    
    
    if (rob_count + (int)RN.size() > ROB_SIZE) return;

    
    for (instruction *rename_inst : RN) {
        rename_inst->rn_duration = (current_cycle - rename_inst->rn_begin) + 1;

        
        rename_inst->dest_tag = rob_tail;

        
        ROB[rob_tail].valid = true;
        ROB[rob_tail].ready = false;  
        ROB[rob_tail].dest_reg = rename_inst->dest;
        ROB[rob_tail].inst = rename_inst;

        
        rob_tail = (rob_tail + 1) % ROB_SIZE;
        rob_count++;

        
        if (rename_inst->src1 != -1) {
            rename_inst->src1_tag = rename_table[rename_inst->src1];
            
            rename_inst->src1_ready = (rename_inst->src1_tag == -1) || 
                                      ROB[rename_inst->src1_tag].ready;
        } else {
            rename_inst->src1_tag = -1;
            rename_inst->src1_ready = true;
        }

        
        if (rename_inst->src2 != -1) {
            rename_inst->src2_tag = rename_table[rename_inst->src2];
            
            rename_inst->src2_ready = (rename_inst->src2_tag == -1) || 
                                      ROB[rename_inst->src2_tag].ready;
        } else {
            rename_inst->src2_tag = -1;
            rename_inst->src2_ready = true;
        }

        
        if (rename_inst->dest != -1)
            rename_table[rename_inst->dest] = rename_inst->dest_tag;

        rename_inst->rr_begin = rename_inst->rn_begin + rename_inst->rn_duration;
        RR.push_back(rename_inst);
    }
    RN.clear();
}

/********************** REG READ *************************/

void RegRead() {
    
    if (RR.empty() || !DI.empty()) return;

    
    for (instruction *rr_inst : RR) {
        rr_inst->rr_duration = (current_cycle - rr_inst->rr_begin) + 1;

        
        if (rr_inst->src1_tag != -1)
            rr_inst->src1_ready |= ROB[rr_inst->src1_tag].ready;
        if (rr_inst->src2_tag != -1)
            rr_inst->src2_ready |= ROB[rr_inst->src2_tag].ready;

        rr_inst->di_begin = rr_inst->rr_begin + rr_inst->rr_duration;
        DI.push_back(rr_inst);
    }
    RR.clear();
}

/********************** DISPATCH *************************/

void Dispatch() {
    if (DI.empty()) return;

    
    int available_slots = IQ_SIZE - (int)IQ.size();
    if (available_slots < (int)DI.size()) return;

    
    for (instruction *dispatch_inst : DI) {
        dispatch_inst->di_duration = (current_cycle - dispatch_inst->di_begin) + 1;

        
        
        if (dispatch_inst->src1_tag != -1)
            dispatch_inst->src1_ready |= ROB[dispatch_inst->src1_tag].ready;
        if (dispatch_inst->src2_tag != -1)
            dispatch_inst->src2_ready |= ROB[dispatch_inst->src2_tag].ready;

        dispatch_inst->is_begin = dispatch_inst->di_begin + dispatch_inst->di_duration;
        IQ.push_back(dispatch_inst);
    }
    DI.clear();
}

/********************** ISSUE *************************/

void Issue() {
    int issued_count = 0;
    vector<instruction*> remaining_insts;

    
    while (issued_count < WIDTH && !IQ.empty()) {
        int selected_idx = -1;
        int oldest_seq = INT_MAX;

        
        for (int i = 0; i < (int)IQ.size(); i++) {
            instruction *curr = IQ[i];
            if (!curr) continue;

            
            
            bool operand1_ready = curr->src1_ready;
            bool operand2_ready = curr->src2_ready;
            
            
            if (curr->src1_tag != -1 && !operand1_ready)
                operand1_ready = ROB[curr->src1_tag].ready;
            if (curr->src2_tag != -1 && !operand2_ready)
                operand2_ready = ROB[curr->src2_tag].ready;

            
            if (operand1_ready && operand2_ready) {
                if (curr->seq_num < oldest_seq) {
                    oldest_seq = curr->seq_num;
                    selected_idx = i;
                }
            }
        }

        
        if (selected_idx == -1) break;

        
        instruction *issue_inst = IQ[selected_idx];
        issue_inst->is_duration = (current_cycle - issue_inst->is_begin) + 1;

        
        issue_inst->ex_begin = issue_inst->is_begin + issue_inst->is_duration;
        issue_inst->exec_latency = get_latency(issue_inst->op_type);
        issue_inst->exec_timer = issue_inst->exec_latency;
        issue_inst->ex_duration = issue_inst->exec_latency;

        EX_list.push_back(issue_inst);

        IQ[selected_idx] = NULL;
        issued_count++;
    }

    
    for (instruction *curr : IQ)
        if (curr) remaining_insts.push_back(curr);

    IQ.swap(remaining_insts);
}
/********************** EXECUTE *************************/

void Execute() {
    vector<instruction*> still_executing;
    vector<instruction*> completed_execution;

    
    for (instruction *ex_inst : EX_list) {
        
        if (ex_inst->ex_begin > current_cycle) {
            still_executing.push_back(ex_inst);
            continue;
        }

        
        ex_inst->exec_timer--;
        
        if (ex_inst->exec_timer <= 0) {
            
            completed_execution.push_back(ex_inst);
        } else {
            
            still_executing.push_back(ex_inst);
        }
    }

    EX_list.swap(still_executing);

    
    for (instruction *done_inst : completed_execution) {
        done_inst->wb_begin = done_inst->ex_begin + done_inst->ex_duration;
        WB.push_back(done_inst);

        int produced_tag = done_inst->dest_tag;

        
        for (instruction *waiting_inst : IQ) {
            if (!waiting_inst) continue;
            if (produced_tag >= 0 && waiting_inst->src1_tag == produced_tag) 
                waiting_inst->src1_ready = true;
            if (produced_tag >= 0 && waiting_inst->src2_tag == produced_tag) 
                waiting_inst->src2_ready = true;
        }

        
        for (instruction *di_inst : DI) {
            if (produced_tag >= 0 && di_inst->src1_tag == produced_tag) 
                di_inst->src1_ready = true;
            if (produced_tag >= 0 && di_inst->src2_tag == produced_tag) 
                di_inst->src2_ready = true;
        }

        
        for (instruction *rr_inst : RR) {
            if (produced_tag >= 0 && rr_inst->src1_tag == produced_tag) 
                rr_inst->src1_ready = true;
            if (produced_tag >= 0 && rr_inst->src2_tag == produced_tag) 
                rr_inst->src2_ready = true;
        }
    }
}

/********************** WRITEBACK *************************/

void Writeback() {
    if (WB.empty()) return;

    
    for (instruction *wb_inst : WB) {
        wb_inst->wb_duration = (current_cycle - wb_inst->wb_begin) + 1;

        int rob_tag = wb_inst->dest_tag;
        
        
        if (rob_tag >= 0 && rob_tag < ROB_SIZE) {
            ROB[rob_tag].ready = true;
            
            
            if (wb_inst->rt_begin == -1) {
                wb_inst->rt_begin = wb_inst->wb_begin + wb_inst->wb_duration;
            }
        }
    }

    WB.clear();
}

/********************** RETIRE *************************/

void Retire() {
    int num_retired = 0;
    
    
    while (num_retired < WIDTH && rob_count > 0) {
        rob_entry &current_entry = ROB[rob_head];
        
        
        if (!current_entry.valid || !current_entry.ready) 
            break;

        instruction *retiring_inst = current_entry.inst;

        
        retiring_inst->rt_duration = (current_cycle - retiring_inst->rt_begin) + 1;

        
        if (retiring_inst->dest != -1) {
            if (rename_table[retiring_inst->dest] == rob_head) {
                rename_table[retiring_inst->dest] = -1;
            }
        }

        
        current_entry.valid = false;
        
        
        
        current_entry.dest_reg = -1;
        current_entry.inst = NULL;

        
        rob_head = (rob_head + 1) % ROB_SIZE;
        rob_count--;

        completed.push_back(retiring_inst);
        num_retired++;
    }
}

/********************** ADVANCE CYCLE *************************/

bool Advance_Cycle() {
    current_cycle++;
    
    if (rob_count == 0 && DE.empty() && RN.empty() && RR.empty() &&
        DI.empty() && IQ.empty() && EX_list.empty() && WB.empty() && trace_done)
        return false;

    return true;
}
int main(int argc, char* argv[]) {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    if (argc != 5) {
        cerr << "Usage: " << argv[0] << " <ROB_SIZE> <IQ_SIZE> <WIDTH> <tracefile>\n";
        return 1;
    }

    ROB_SIZE = (int)strtoul(argv[1], NULL, 10);
    IQ_SIZE = (int)strtoul(argv[2], NULL, 10);
    WIDTH = (int)strtoul(argv[3], NULL, 10);

    ROB.resize(ROB_SIZE);
    for (int i = 0; i < ROB_SIZE; i++) {
        ROB[i].valid = false;
        ROB[i].ready = false;
        ROB[i].dest_reg = -1;
        ROB[i].inst = NULL;
    }

    for (int i = 0; i < 67; i++) rename_table[i] = -1;

    trace.open(argv[4]);
    if (!trace.is_open()) {
        cerr << "Error: Unable to open file " << argv[4] << endl;
        return 1;
    }

    
    
    
    do {
        Retire();      
        Writeback();   
        Execute();     
        Issue();       
        Dispatch();    
        RegRead();     
        Rename();      
        Decode();      
        Fetch();       
    } while (Advance_Cycle());

    
    for (instruction* inst : completed) {
        cout << inst->seq_num << " fu{" << inst->op_type << "} "
             << "src{" << inst->src1 << "," << inst->src2 << "} "
             << "dst{" << inst->dest << "} ";

        auto print_stage = [](const char* name, int begin, int dur) {
            cout << name << "{";
            if (begin != -1 && dur != -1) cout << begin << "," << dur;
            else cout << "0,0";
            cout << "} ";
        };

        print_stage("FE", inst->fe_begin, inst->fe_duration);
        print_stage("DE", inst->de_begin, inst->de_duration);
        print_stage("RN", inst->rn_begin, inst->rn_duration);
        print_stage("RR", inst->rr_begin, inst->rr_duration);
        print_stage("DI", inst->di_begin, inst->di_duration);
        print_stage("IS", inst->is_begin, inst->is_duration);
        print_stage("EX", inst->ex_begin, inst->ex_duration);
        print_stage("WB", inst->wb_begin, inst->wb_duration);

        cout << "RT{";
        if (inst->rt_begin != -1 && inst->rt_duration != -1)
            cout << inst->rt_begin << "," << inst->rt_duration;
        else cout << "0,0";
        cout << "}\n";
    }
    
    cout << "# === Simulator Command =========\n";
    cout << "# ./sim " << ROB_SIZE << " " << IQ_SIZE << " " << WIDTH << " " << argv[4] << endl;
    cout << "# === Processor Configuration ===\n";
    cout << "# ROB_SIZE = " << ROB_SIZE << "\n# IQ_SIZE  = " << IQ_SIZE << "\n# WIDTH    = " << WIDTH << endl;
    cout << "# === Simulation Results ========\n";
    cout << "# Dynamic Instruction Count    = " << completed.size() << endl;
    cout << "# Cycles                       = " << current_cycle << endl;
    cout << fixed << setprecision(2);
    if (current_cycle > 0)
        cout << "# Instructions Per Cycle (IPC) = " << (double)completed.size() / current_cycle << endl;
    else
        cout << "# Instructions Per Cycle (IPC) = 0.00" << endl;

    trace.close();
    for (instruction* inst : completed) delete inst;

    return 0;
}

