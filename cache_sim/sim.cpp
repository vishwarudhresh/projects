#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <bitset>
#include <list>
#include <queue>
#include <unordered_map>
#include <iomanip>
#include <inttypes.h>
#include "sim.h"

using namespace std;

int L1_reads = 0, L1_readmiss = 0, L1_writes = 0, L1_writemiss = 0, L1_writeback = 0;
int L2_reads = 0, L2_readmiss = 0, L2_writes = 0, L2_writemiss = 0, L2_writeback = 0;
int L1_prefetches = 0;
int L2_prefetches = 0;
int L2_prefetch_reads = 0;
int L2_prefetch_misses = 0;

class Cache;

class StreamBuffer {
public:
    deque<uint32_t> buffer;
    bool valid;
    int max_depth;
    Cache* parent_cache;
    int block_size;
    
    StreamBuffer(int depth) : valid(false), max_depth(depth), parent_cache(nullptr), block_size(0) {}
    
    bool check_hit(uint32_t block_num) {
        if (!valid || buffer.empty()) return false;
        
        for (size_t i = 0; i < buffer.size(); i++) {
            if (buffer[i] == block_num) {
                return true;
            }
        }
        return false;
    }
    
    int find_position(uint32_t block_num) {
        for (size_t i = 0; i < buffer.size(); i++) {
            if (buffer[i] == block_num) {
                return i;
            }
        }
        return -1;
    }
    
    void create_new_stream(uint32_t miss_block_num, Cache* cache, int bs) {
        parent_cache = cache;
        block_size = bs;
        
        buffer.clear();
        
        for (int i = 1; i <= max_depth; i++) {
            buffer.push_back(miss_block_num + i);
        }
        
        valid = true;
    }
    
    int advance_stream(uint32_t hit_block_num) {
        if (!valid) return 0;
        
        int pos = find_position(hit_block_num);
        if (pos == -1) return 0;
        
        int num_removed = pos + 1;
        for (int i = 0; i < num_removed; i++) {
            buffer.pop_front();
        }
        
        uint32_t next_block_to_prefetch;
        if (!buffer.empty()) {
            next_block_to_prefetch = buffer.back() + 1;
        } else {
            next_block_to_prefetch = hit_block_num + 1;
        }
        
        for (int i = 0; i < num_removed; i++) {
            buffer.push_back(next_block_to_prefetch);
            next_block_to_prefetch++;
        }
        
        return num_removed;
    }
};

class Cache {
public:
    int cache_size, block_size, assoc, number_set, index_calc, offset_calc;
    bool is_l1;
    
    struct cache_block {
        int valid, dirty;
        string tag;
        cache_block() : valid(0), dirty(0), tag("") {}
    };
    
    struct Set {
        list<cache_block> cache_order;
        unordered_map<string, list<cache_block>::iterator> map;
    };
    
    Set* cache_sets;
    Cache* next;
    Cache* prev;
    list<StreamBuffer*> stream_buffers;
    unordered_map<StreamBuffer*, list<StreamBuffer*>::iterator> sb_map;
    int num_stream_buffers;
    int stream_buffer_depth;
    bool use_stream_buffers;
    
    Cache(int Cache_size, int Block_size, int Assoc, int num_sb = 0, int sb_depth = 4, bool is_L1 = false) {
        cache_size = Cache_size;
        block_size = Block_size;
        assoc = Assoc;
        next = nullptr;
        prev = nullptr;
        is_l1 = is_L1;
        
        if (cache_size == 0) {
            number_set = index_calc = offset_calc = 0;
            cache_sets = nullptr;
            use_stream_buffers = false;
            return;
        }
        
        number_set = (cache_size) / (block_size * assoc);
        index_calc = log2(number_set);
        offset_calc = log2(block_size);
        cache_sets = new Set[number_set];
        
        num_stream_buffers = num_sb;
        stream_buffer_depth = sb_depth;
        use_stream_buffers = (num_sb > 0);
        
        for (int i = 0; i < num_stream_buffers; i++) {
            StreamBuffer* sb = new StreamBuffer(stream_buffer_depth);
            stream_buffers.push_back(sb);
            sb_map[sb] = --stream_buffers.end();
        }
    }
    
    ~Cache() {
        if (cache_sets) delete[] cache_sets;
        for (auto sb : stream_buffers) delete sb;
    }
    
    void move_sb_to_front(StreamBuffer* sb) {
        auto it = sb_map[sb];
        stream_buffers.erase(it);
        stream_buffers.push_front(sb);
        sb_map[sb] = stream_buffers.begin();
    }
    
    bool check_stream_buffers(uint32_t byte_addr) {
        if (!use_stream_buffers) return false;
        
        uint32_t block_num = byte_addr / block_size;
        
        for (auto sb : stream_buffers) {
            if (sb->check_hit(block_num)) {
                return true;
            }
        }
        return false;
    }
    
    void advance_stream_buffer(uint32_t byte_addr) {
        if (!use_stream_buffers) return;
        
        uint32_t block_num = byte_addr / block_size;
        
        for (auto sb : stream_buffers) {
            if (sb->check_hit(block_num)) {
                int num_new_prefetches = sb->advance_stream(block_num);
                move_sb_to_front(sb);
                
                if (num_new_prefetches > 0) {
                    if (is_l1) {
                        L1_prefetches += num_new_prefetches;
                    } else {
                        L2_prefetches += num_new_prefetches;
                    }
                    
                    if (next != nullptr && next->cache_size > 0) {
                        size_t start_idx = sb->buffer.size() - num_new_prefetches;
                        for (size_t i = start_idx; i < sb->buffer.size(); i++) {
                            uint32_t prefetch_block = sb->buffer[i];
                            uint32_t prefetch_byte_addr = prefetch_block * block_size;
                            
                            if (is_l1) {
                                L2_prefetch_reads++;
                                next->process_prefetch_from_upper_level(prefetch_byte_addr);
                            }
                        }
                    }
                }
                return;
            }
        }
    }
    
    void allocate_stream_buffer(uint32_t miss_addr) {
        if (!use_stream_buffers) return;
        
        uint32_t miss_block_num = miss_addr / block_size;
        
        StreamBuffer* lru_sb = stream_buffers.back();
        
        lru_sb->create_new_stream(miss_block_num, this, block_size);
        
        move_sb_to_front(lru_sb);
        
        if (is_l1) {
            L1_prefetches += stream_buffer_depth;
        } else {
            L2_prefetches += stream_buffer_depth;
        }
        
        if (next != nullptr && next->cache_size > 0) {
            for (auto prefetch_block : lru_sb->buffer) {
                uint32_t prefetch_byte_addr = prefetch_block * block_size;
                
                if (is_l1) {
                    L2_prefetch_reads++;
                    next->process_prefetch_from_upper_level(prefetch_byte_addr);
                }
            }
        }
    }
    
    void process_prefetch_from_upper_level(uint32_t address) {
        if (cache_size == 0) return;
        
        bitset<32> bits(address);
        string address_bin = bits.to_string();
        
        string offset_block = address_bin.substr(32 - offset_calc, offset_calc);
        string index_block = address_bin.substr(32 - offset_calc - index_calc, index_calc);
        string tag_block = address_bin.substr(0, 32 - offset_calc - index_calc);
        
        int set_index = bitset<32>(index_block).to_ulong();
        
        Set& current_set = cache_sets[set_index];
        bool hit = (current_set.map.find(tag_block) != current_set.map.end());
        
        if (hit) {
            auto it = current_set.map[tag_block];
            cache_block block = *it;
            current_set.cache_order.erase(it);
            current_set.cache_order.push_front(block);
            current_set.map[block.tag] = current_set.cache_order.begin();
            
        } else {
            L2_prefetch_misses++;
            
            if (current_set.cache_order.size() >= (size_t)assoc) {
                cache_block lru = current_set.cache_order.back();
                current_set.cache_order.pop_back();
                current_set.map.erase(lru.tag);
                
                if (lru.dirty == 1) {
                    L2_writeback++;
                }
            }
            
            cache_block new_block;
            new_block.valid = 1;
            new_block.tag = tag_block;
            new_block.dirty = 0;
            
            current_set.cache_order.push_front(new_block);
            current_set.map[tag_block] = current_set.cache_order.begin();
        }
    }
    
    void cache_update(string oper, string tag, int set_index, uint32_t address, bool from_writeback = false) {
        if (cache_size == 0) return;
        
        Set& current_set = cache_sets[set_index];
        bool hit = (current_set.map.find(tag) != current_set.map.end());
        
        if (is_l1 && !from_writeback) {
            if (oper == "r") L1_reads++;
            else L1_writes++;
        }
        
        if (hit) {
            auto it = current_set.map[tag];
            cache_block block = *it;
            current_set.cache_order.erase(it);
            
            if (oper == "w") block.dirty = 1;
            
            current_set.cache_order.push_front(block);
            current_set.map[block.tag] = current_set.cache_order.begin();
            
            if (use_stream_buffers) {
                bool sb_hit = check_stream_buffers(address);
                if (sb_hit) {
                    advance_stream_buffer(address);
                }
            }
            
        } else {
            bool found_in_stream_buffer = false;
            if (use_stream_buffers) {
                found_in_stream_buffer = check_stream_buffers(address);
            }
            
            if (is_l1 && !from_writeback && !found_in_stream_buffer) {
                if (oper == "r") L1_readmiss++;
                else L1_writemiss++;
            }
            
            if (!is_l1) {
                if (!found_in_stream_buffer) {
                    if (from_writeback) {
                        L2_writemiss++;
                    } else {
                        L2_readmiss++;
                    }
                }
            }
            
            if (current_set.cache_order.size() >= (size_t)assoc) {
                cache_block lru = current_set.cache_order.back();
                current_set.cache_order.pop_back();
                current_set.map.erase(lru.tag);
                
                if (lru.dirty == 1) {
                    if (is_l1) {
                        L1_writeback++;
                        if (next != nullptr && next->cache_size > 0) {
                            L2_writes++;
                            uint32_t wb_addr = (bitset<32>(lru.tag).to_ulong() << (index_calc + offset_calc)) | 
                                              (set_index << offset_calc);
                            next->process_from_upper_level("w", wb_addr, true);
                        }
                    } else {
                        L2_writeback++;
                    }
                }
            }
            
            if (found_in_stream_buffer) {
                advance_stream_buffer(address);
            } else {
                if (is_l1 && !from_writeback && next != nullptr && next->cache_size > 0) {
                    L2_reads++;
                    uint32_t fetch_addr = (bitset<32>(tag).to_ulong() << (index_calc + offset_calc)) | 
                                         (set_index << offset_calc);
                    next->process_from_upper_level("r", fetch_addr, false);
                }
                if (use_stream_buffers) {
                    allocate_stream_buffer(address);
                }
            }
            
            cache_block new_block;
            new_block.valid = 1;
            new_block.tag = tag;
            new_block.dirty = (oper == "w") ? 1 : 0;
            
            current_set.cache_order.push_front(new_block);
            current_set.map[tag] = current_set.cache_order.begin();
        }
    }
    
    void request(uint32_t address, char rw) {
        if (cache_size == 0) return;
        
        bitset<32> bits(address);
        string address_bin = bits.to_string();
        
        string offset_block = address_bin.substr(32 - offset_calc, offset_calc);
        string index_block = address_bin.substr(32 - offset_calc - index_calc, index_calc);
        string tag_block = address_bin.substr(0, 32 - offset_calc - index_calc);
        
        int set_index = bitset<32>(index_block).to_ulong();
        string oper = (rw == 'r') ? "r" : "w";
        
        cache_update(oper, tag_block, set_index, address, false);
    }
    
    void process_from_upper_level(string oper, uint32_t address, bool from_writeback) {
        if (cache_size == 0) return;
        
        bitset<32> bits(address);
        string address_bin = bits.to_string();
        
        string offset_block = address_bin.substr(32 - offset_calc, offset_calc);
        string index_block = address_bin.substr(32 - offset_calc - index_calc, index_calc);
        string tag_block = address_bin.substr(0, 32 - offset_calc - index_calc);
        
        int set_index = bitset<32>(index_block).to_ulong();
        
        cache_update(oper, tag_block, set_index, address, from_writeback);
    }
    
    void display_contents(string cache_name) {
        if (cache_size == 0) return;
        
        cout << "===== " << cache_name << " contents =====" << "\n";
        for (int i = 0; i < number_set; i++) {
            cout << "set " << setw(6) << i << ":";
            for (auto it = cache_sets[i].cache_order.begin(); it != cache_sets[i].cache_order.end(); ++it) {
                unsigned long tag_val = bitset<32>(it->tag).to_ulong();
                cout << "  " << hex << setw(5) << tag_val << dec;
                if (it->dirty) cout << " D";
                else cout << "  ";
            }
            cout << "\n";
        }
    }
    
    void display_stream_buffers() {
        if (!use_stream_buffers) return;
        
        cout << "===== Stream Buffer(s) contents =====" << "\n";
        for (auto sb : stream_buffers) {
            if (sb->valid && !sb->buffer.empty()) {
                for (auto block_num : sb->buffer) {
                    cout << " " << hex << block_num << dec;
                }
                cout << "\n";
            }
        }
        cout << "\n";
    }
};

int main(int argc, char *argv[]) {
    if (argc != 9) {
        printf("Error: Expected 8 command-line arguments.\n");
        return 1;
    }
    
    cache_params_t params;
    params.BLOCKSIZE = atoi(argv[1]);
    params.L1_SIZE = atoi(argv[2]);
    params.L1_ASSOC = atoi(argv[3]);
    params.L2_SIZE = atoi(argv[4]);
    params.L2_ASSOC = atoi(argv[5]);
    params.PREF_N = atoi(argv[6]);
    params.PREF_M = atoi(argv[7]);
    char* trace_file = argv[8];
    
    FILE* fp = fopen(trace_file, "r");
    if (!fp) {
        printf("Error: Unable to open file %s\n", trace_file);
        return 1;
    }
    
    printf("===== Simulator configuration =====\n");
    printf("BLOCKSIZE:  %u\n", params.BLOCKSIZE);
    printf("L1_SIZE:    %u\n", params.L1_SIZE);
    printf("L1_ASSOC:   %u\n", params.L1_ASSOC);
    printf("L2_SIZE:    %u\n", params.L2_SIZE);
    printf("L2_ASSOC:   %u\n", params.L2_ASSOC);
    printf("PREF_N:     %u\n", params.PREF_N);
    printf("PREF_M:     %u\n", params.PREF_M);
    printf("trace_file: %s\n\n", trace_file);
    
    Cache* L1;
    Cache* L2 = nullptr;
    
    if (params.L2_SIZE > 0) {
        L1 = new Cache(params.L1_SIZE, params.BLOCKSIZE, params.L1_ASSOC, 
                       0, 0, true);
        L2 = new Cache(params.L2_SIZE, params.BLOCKSIZE, params.L2_ASSOC, 
                       params.PREF_N, params.PREF_M, false);
        L1->next = L2;
        L2->prev = L1;
    } else {
        L1 = new Cache(params.L1_SIZE, params.BLOCKSIZE, params.L1_ASSOC, 
                       params.PREF_N, params.PREF_M, true);
    }
    
    char rw;
    uint32_t addr;
    while (fscanf(fp, "%c %x\n", &rw, &addr) == 2) {
        L1->request(addr, rw);
    }
    fclose(fp);
    
    int total_accesses = L1_reads + L1_writes;
    int total_misses = L1_readmiss + L1_writemiss;
    double l1_miss_rate = (total_accesses > 0) ? (double)total_misses / total_accesses : 0.0;
    double l2_miss_rate = (L2_reads > 0) ? (double)L2_readmiss / L2_reads : 0.0;
    
    int memory_traffic;
    if (params.L2_SIZE > 0) {
        memory_traffic = L2_readmiss + L2_writemiss + L2_writeback + L2_prefetches;
    } else {
        memory_traffic = L1_readmiss + L1_writemiss + L1_writeback + L1_prefetches;
    }
    
    L1->display_contents("L1");
    cout << "\n";
    
    if (L2) {
        L2->display_contents("L2");
        cout << "\n";
    }
    
    if (L2) {
        L2->display_stream_buffers();
    } else {
        L1->display_stream_buffers();
    }
    
    printf("===== Measurements =====\n");
    printf("a. L1 reads:                   %d\n", L1_reads);
    printf("b. L1 read misses:             %d\n", L1_readmiss);
    printf("c. L1 writes:                  %d\n", L1_writes);
    printf("d. L1 write misses:            %d\n", L1_writemiss);
    printf("e. L1 miss rate:               %.4f\n", l1_miss_rate);
    printf("f. L1 writebacks:              %d\n", L1_writeback);
    printf("g. L1 prefetches:              %d\n", L1_prefetches);
    printf("h. L2 reads (demand):          %d\n", L2_reads);
    printf("i. L2 read misses (demand):    %d\n", L2_readmiss);
    printf("j. L2 reads (prefetch):        %d\n", L2_prefetch_reads);
    printf("k. L2 read misses (prefetch):  %d\n", L2_prefetch_misses);
    printf("l. L2 writes:                  %d\n", L2_writes);
    printf("m. L2 write misses:            %d\n", L2_writemiss);
    printf("n. L2 miss rate:               %.4f\n", l2_miss_rate);
    printf("o. L2 writebacks:              %d\n", L2_writeback);
    printf("p. L2 prefetches:              %d\n", L2_prefetches);
    printf("q. memory traffic:             %d\n", memory_traffic);
    
    delete L1;
    if (L2) delete L2;
    
    return 0;
}