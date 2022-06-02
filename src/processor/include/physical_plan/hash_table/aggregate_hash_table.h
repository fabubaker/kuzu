#pragma once

#include "src/function/aggregate/include/aggregate_function.h"
#include "src/processor/include/physical_plan/hash_table/base_hash_table.h"
#include "src/storage/buffer_manager/include/memory_manager.h"

using namespace graphflow::function;

namespace graphflow {
namespace processor {

constexpr uint16_t HASH_PREFIX_SHIFT = (sizeof(hash_t) - sizeof(uint16_t)) * 8;

struct HashSlot {
    uint16_t hashPrefix;     // 16 high bits of the hash value for fast comparison.
    uint8_t* groupByKeysPtr; // pointer to the tuple buffer which stores [hash, groupKey1, ...
                             // groupKeyN, aggregateState1, ..., aggregateStateN]
};

/**
 * AggregateHashTable Design
 *
 * 1. Payload
 * Entry layout: [hash, groupKey1, ... groupKeyN, aggregateState1, ..., aggregateStateN]
 * Payload is stored in the factorizedTable.
 *
 * 2. Hash slot
 * Layout : see HashSlot struct
 * If the groupByKeysPtr is a nullptr, then the current hashSlot is unused.
 *
 * 3. Collision handling
 * Linear probing. When collision happens, we find the next hash slot whose groupByKeysPtr is a
 * nullptr.
 *
 */

class AggregateHashTable : public BaseHashTable {

public:
    // Used by distinct aggregate hash table only.
    inline AggregateHashTable(MemoryManager& memoryManager,
        const vector<DataType>& groupByHashKeysDataTypes,
        const vector<unique_ptr<AggregateFunction>>& aggregateFunctions,
        uint64_t numEntriesToAllocate)
        : AggregateHashTable(memoryManager, groupByHashKeysDataTypes, vector<DataType>(),
              aggregateFunctions, numEntriesToAllocate) {}

    AggregateHashTable(MemoryManager& memoryManager, vector<DataType> groupByHashKeysDataTypes,
        vector<DataType> groupByNonHashKeysDataTypes,
        const vector<unique_ptr<AggregateFunction>>& aggregateFunctions,
        uint64_t numEntriesToAllocate);

    inline uint64_t getNumEntries() const { return factorizedTable->getNumTuples(); }

    inline vector<DataType> getGroupByHashKeysDataTypes() const { return groupByHashKeysDataTypes; }

    inline vector<DataType> getGroupByNonHashKeysDataTypes() const {
        return groupByNonHashKeysDataTypes;
    }

    inline void append(const vector<ValueVector*>& groupByHashKeyVectors,
        const vector<ValueVector*>& aggregateVectors, uint64_t multiplicity) {
        append(groupByHashKeyVectors, vector<ValueVector*>(), aggregateVectors, multiplicity);
    }

    //! update aggregate states for an input
    void append(const vector<ValueVector*>& groupByKeyVectors,
        const vector<ValueVector*>& groupByNonHashKeyVectors,
        const vector<ValueVector*>& aggregateVectors, uint64_t multiplicity);

    bool isAggregateValueDistinctForGroupByKeys(
        const vector<ValueVector*>& groupByKeyVectors, ValueVector* aggregateVector);

    //! merge aggregate hash table by combining aggregate states under the same key
    void merge(AggregateHashTable& other);

    void finalizeAggregateStates();

    uint8_t* getEntry(uint64_t idx) { return factorizedTable->getTuple(idx); }

    FactorizedTable* getFactorizedTable() { return factorizedTable.get(); }

private:
    uint8_t* findEntry(const vector<ValueVector*>& groupByKeyVectors, hash_t hash);

    uint8_t* findEntry(uint8_t* entryBuffer, hash_t hash);

    uint8_t* createEntry(const vector<ValueVector*>& groupByKeyVectors,
        const vector<ValueVector*>& groupByNonKeyVectors, hash_t hash);

    uint8_t* createEntry(uint8_t* groupByKeys, hash_t hash);

    inline uint64_t getNumBytesForHash() const { return sizeof(hash_t); }

    uint64_t getNumBytesForGroupByHashKeys() const;

    uint64_t getNumBytesForGroupByNonHashKeys() const;

    inline uint64_t getGroupByKeysOffsetInEntry() const { return getNumBytesForHash(); }

    inline uint64_t getAggregateStatesOffsetInEntry() const {
        return getGroupByKeysOffsetInEntry() + getNumBytesForGroupByHashKeys() +
               getNumBytesForGroupByNonHashKeys();
    }

    void increaseSlotOffset(uint64_t& slotOffset) const;

    //! compute hash for multiple flat key vectors
    hash_t computeHash(const vector<ValueVector*>& keyVectors);

    //! compute hash for single flat key vector
    hash_t computeHash(ValueVector* keyVector);

    //! compute hash for multiple keys
    hash_t computeHash(uint8_t* keys);

    //! compute hash for single key value
    hash_t computeHash(const DataType& keyDataType, uint8_t* keyValue, uint64_t colIdx);

    //! check if flat key vectors are the same as keys stored in entry
    bool matchGroupByKeys(const vector<ValueVector*>& keyVectors, uint8_t* entry);

    //! check if keys are the same as keys stored in entryBufferToMatch.
    bool matchGroupByKeys(uint8_t* keys, uint8_t* entryBufferToMatch);

    void fillTupleWithInitialNullAggregateState();

    //! find an uninitialized hash slot for given hash and fill hash slot with block id and offset
    void fillHashSlot(hash_t hash, uint8_t* groupByKeysAndAggregateStateBuffer);

    void resize(uint64_t newSize);

    HashSlot* getHashSlot(uint64_t slotIdx) {
        assert(slotIdx < maxNumHashSlots);
        return (HashSlot*)(hashSlotsBlocks[slotIdx / numHashSlotsPerBlock]->getData() +
                           slotIdx % numHashSlotsPerBlock * sizeof(HashSlot));
    }

    void addDataBlocksIfNecessary(uint64_t maxNumHashSlots);

    inline void fillTupleWithGroupByKeys(uint8_t* tupleBuffer, uint8_t* groupByKeys) {
        memcpy(tupleBuffer, groupByKeys,
            getNumBytesForGroupByHashKeys() + getNumBytesForGroupByNonHashKeys());
    }

    inline void fillTupleWithNullMap(uint8_t* entryNullBuffer, uint8_t* groupByKeyNullBuffer) {
        memcpy(entryNullBuffer, groupByKeyNullBuffer,
            factorizedTable->getTableSchema().getNumBytesForNullMap());
    }

    bool compareEntryWithKeys(
        uint8_t* keyBuffer, uint8_t* tupleBuffer, bool isStrCol, uint64_t numBytesToCompare);

private:
    vector<DataType> groupByHashKeysDataTypes;
    vector<DataType> groupByNonHashKeysDataTypes;
    vector<unique_ptr<AggregateFunction>> aggregateFunctions;

    //! special handling of distinct aggregate
    vector<unique_ptr<AggregateHashTable>> distinctHashTables;

    bool hasStrCol = false;
};

class AggregateHashTableUtils {

public:
    static vector<unique_ptr<AggregateHashTable>> createDistinctHashTables(
        MemoryManager& memoryManager, const vector<DataType>& groupByKeyDataTypes,
        const vector<unique_ptr<AggregateFunction>>& aggregateFunctions);
};

} // namespace processor
} // namespace graphflow
