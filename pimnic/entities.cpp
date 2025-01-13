#include <string>
#include <map>
#include <immintrin.h>
#include <x86intrin.h>
#include <unistd.h>
#include "entities.h"
#include "../util/utils.h"
#include "../util/virt2phys.h"
#include "../bfdma/dma_export.h"

using namespace std;

#define BANK_CHUNK_SIZE 0x20000
#define BANK_NEXT_CHUNK_OFFSET 0x100000

static uint32_t address_translation_on_mram(uint32_t byte_offset){
    /* We have observed that, within the 26 address bits of the MRAM address, we need to apply an address translation:
     *
     * virtual[13: 0] = physical[13: 0]
     * virtual[20:14] = physical[21:15]
     * virtual[   21] = physical[   14]
     * virtual[25:22] = physical[25:22]
     *
     * This function computes the "virtual" mram address based on the given "physical" mram address.
     */

    uint32_t mask_21_to_15 = ((1 << (21 - 15 + 1)) - 1) << 15;
    uint32_t mask_21_to_14 = ((1 << (21 - 14 + 1)) - 1) << 14;
    uint32_t bits_21_to_15 = (byte_offset & mask_21_to_15) >> 15;
    uint32_t bit_14 = (byte_offset >> 14) & 1;
    uint32_t unchanged_bits = byte_offset & ~mask_21_to_14;

    return unchanged_bits | (bits_21_to_15 << 14) | (bit_14 << 21);
}


class DPU {
public:
    DPU(int slice, int dpu, uint64_t base_region_addr) : slice_id(slice), dpu_id(dpu), index(dpu_id * 8 + slice_id), base_region_addr(base_region_addr) {}
	DPU() = default;
	DPU(dpu_t* dpu_o, uint64_t base_region_addr) : dpu_o(dpu_o), base_region_addr(base_region_addr) {
		dpu_id = dpu_o->dpu_id;
		slice_id = dpu_o->slice_id;
		index = dpu_id * 8 + slice_id;
	}
    int slice_id;
    int dpu_id;
    int index;
	dpu_t* dpu_o;
	uint64_t base_region_addr;

	void get_access(){
		uint8_t mask = 1 << slice_id;
		dpu_switch_mux_for_dpu_line(dpu_o->rank, (uint8_t)dpu_id, mask);
	}

	uint64_t get_word_start(uint32_t offset){
		uint32_t bank_offset = (0x40000 * ((dpu_id) % 4) + ((dpu_id >= 4) ? 0x40 : 0));
		//0, 256K, 512K, 768K, 0+64B, 256K+64B, 512K+64B, 768K+64B

		uint32_t transformed_offset = address_translation_on_mram(offset);
		uint64_t word_offset = transformed_offset / 8;//addr within a word is lost
		uint64_t true_word = word_offset * 16;//move two cache lines, i.e., 16 words
		uint64_t true_byte = true_word * 8;
		uint64_t addr = (true_byte % BANK_CHUNK_SIZE) + (true_byte / BANK_CHUNK_SIZE) * BANK_NEXT_CHUNK_OFFSET;

		if(offset == 0x2000000){
			printf("offset:%x, transformed_offset:%x, word_offset:%lx, true_word:%lx, true_byte:%lx, addr:%lx\n", offset, transformed_offset, word_offset, true_word, true_byte, addr);
		}
		return addr + bank_offset;// + slice_offset;
	}

	uint64_t get_addr(uint32_t offset){
		uint32_t slice_offset = slice_id;
		return get_word_start(offset) + slice_offset + (offset%8)*8;
	}

	void write(uint32_t _offset, char byte){
		get_access();
		char value = byte;
		uint64_t offset = get_addr(_offset);
		char* ptr = (char*)(base_region_addr + offset);
		*ptr = value;
		flush_cache_line(ptr);
		__builtin_ia32_sfence();
	}
};

class RANK {
public:
    uint64_t base_region_addr;
    map<int, DPU> dpus;
	dpu_rank_t* rank_o;
	RANK() = default;
	RANK(dpu_rank_t* rank_o) : rank_o(rank_o) {
		dpu_rank_handler_t handler = rank_o->handler_context->handler;
		base_region_addr = handler->__get_base_region_address(rank_o);
	}
	void open_access(){
		uint8_t mask = 0xff;
		for(int i=0;i<8;i++){
			dpu_switch_mux_for_dpu_line(rank_o, (uint8_t)i, mask);
		}
	}
};



map<dpu_rank_t*, RANK> _ranks;

int bank_init(dpu_t* dpu_o){
	dpu_rank_t* rank_o = dpu_o->rank;
	dpu_rank_handler_t handler = rank_o->handler_context->handler;
	uint64_t base_region_addr = handler->__get_base_region_address(rank_o);
	int index = dpu_o->dpu_id * 8 + dpu_o->slice_id;

	auto rank_it = _ranks.find(rank_o);
	if(rank_it != _ranks.end()){
		auto& rank = rank_it->second;
		if(rank.dpus.find(index) != rank.dpus.end()){
			return -1;
		}else{
			rank.dpus.emplace(index, DPU(dpu_o, base_region_addr));
		}
	}else{
		_ranks.emplace(rank_o, RANK(rank_o));
		auto& new_rank = _ranks[rank_o];
		new_rank.dpus.emplace(index, DPU(dpu_o, base_region_addr));
	}
	return 0;
}

void bank_list() {
    for (auto it = _ranks.begin(); it != _ranks.end(); ++it) {
        auto& rank = it->second;
        uint64_t offset = 0;
        if (it != _ranks.begin()) {
            offset = it->second.base_region_addr - prev(it)->second.base_region_addr;
        }
        offset = offset / 1024 / 1024 / 1024;
        printf("bank_list, bank base_region_addr:%lx, offset:%lx GB, num_dpus:%lu\n", it->second.base_region_addr, offset, rank.dpus.size());
        for (const auto& iter : rank.dpus) {
            const auto& dpu = iter.second;
            printf("dpu_id:%d, slice_id:%d\n", dpu.dpu_id, dpu.slice_id);
        }
    }
}


void bank_write_test(int dpu_idx, int size_shift, int value){
	printf("Number of ranks:%lu\n", _ranks.size());
	for(auto it = _ranks.begin(); it != _ranks.end(); ++it){
		auto& rank = it->second;
		printf("Number of dpus:%lu\n", rank.dpus.size());

		if(rank.dpus.find(dpu_idx) != rank.dpus.end()){
			auto dpu = rank.dpus[dpu_idx];
			for(int i=0;i<(1<<size_shift);i+=(1<<0)){
				dpu.write(i, value);
			}
		}else{
			printf("DPU not found\n");
		}
	}
}


void export_test(){
	for(auto it = _ranks.begin(); it != _ranks.end(); ++it){
		auto& rank = it->second;
		rank.open_access();

		auto addr = rank.base_region_addr;

		export_buffer((void*)addr, 1024);
	}
}