#pragma once

#include "src/processor/operator/include/filtering_operator.h"
#include "src/processor/operator/scan_column/include/scan_column.h"
#include "src/storage/storage_structure/include/column.h"

namespace kuzu {
namespace processor {

class AdjColumnExtend : public ScanSingleColumn, public FilteringOperator {

public:
    AdjColumnExtend(const DataPos& inputNodeIDVectorPos, const DataPos& outputNodeIDVectorPos,
        Column* nodeIDColumn, unique_ptr<PhysicalOperator> child, uint32_t id,
        const string& paramsString)
        : ScanSingleColumn{inputNodeIDVectorPos, outputNodeIDVectorPos, move(child), id,
              paramsString},
          FilteringOperator{1 /* numStatesToSave */}, nodeIDColumn{nodeIDColumn} {}

    PhysicalOperatorType getOperatorType() override { return COLUMN_EXTEND; }

    shared_ptr<ResultSet> init(ExecutionContext* context) override;

    bool getNextTuples() override;

    unique_ptr<PhysicalOperator> clone() override {
        return make_unique<AdjColumnExtend>(inputNodeIDVectorPos, outputVectorPos, nodeIDColumn,
            children[0]->clone(), id, paramsString);
    }

private:
    Column* nodeIDColumn;
};

} // namespace processor
} // namespace kuzu
