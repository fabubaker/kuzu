#pragma once

#include "src/common/include/utils.h"
#include "src/function/hash/operations/include/hash_operations.h"
#include "src/processor/result/include/factorized_table.h"
#include "src/storage/buffer_manager/include/memory_manager.h"

using namespace kuzu::common;
using namespace kuzu::function::operation;

namespace kuzu {
namespace processor {

class BaseHashTable {
public:
    explicit BaseHashTable(MemoryManager& memoryManager)
        : maxNumHashSlots{0}, bitmask{0}, numSlotsPerBlockLog2{0}, slotIdxInBlockMask{0},
          memoryManager{memoryManager} {}

    inline uint64_t getSlotIdxForHash(hash_t hash) const { return hash & bitmask; }

protected:
    uint64_t maxNumHashSlots;
    uint64_t bitmask;
    vector<unique_ptr<DataBlock>> hashSlotsBlocks;
    uint64_t numSlotsPerBlockLog2;
    uint64_t slotIdxInBlockMask;
    MemoryManager& memoryManager;
    unique_ptr<FactorizedTable> factorizedTable;
};

} // namespace processor
} // namespace kuzu
