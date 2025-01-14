#pragma once

#include "src/processor/operator/include/physical_operator.h"
#include "src/processor/operator/var_length_extend/include/var_length_adj_list_extend.h"
#include "src/processor/result/include/result_set.h"
#include "src/storage/storage_structure/include/column.h"
#include "src/storage/storage_structure/include/lists/lists.h"

using namespace std;
using namespace kuzu::common;

namespace kuzu {
namespace processor {

struct ColumnExtendDFSLevelInfo : DFSLevelInfo {
    ColumnExtendDFSLevelInfo(uint8_t level, ExecutionContext& context)
        : DFSLevelInfo(level, context), hasBeenExtended{false} {}

    void reset();

    bool hasBeenExtended;
};

class VarLengthColumnExtend : public VarLengthExtend {

public:
    VarLengthColumnExtend(const DataPos& boundNodeDataPos, const DataPos& nbrNodeDataPos,
        BaseColumnOrList* storage, uint8_t lowerBound, uint8_t upperBound,
        unique_ptr<PhysicalOperator> child, uint32_t id, const string& paramsString)
        : VarLengthExtend(boundNodeDataPos, nbrNodeDataPos, storage, lowerBound, upperBound,
              move(child), id, paramsString) {}

    PhysicalOperatorType getOperatorType() override { return VAR_LENGTH_COLUMN_EXTEND; }

    shared_ptr<ResultSet> init(ExecutionContext* context) override;

    bool getNextTuples() override;

    unique_ptr<PhysicalOperator> clone() override {
        return make_unique<VarLengthColumnExtend>(boundNodeDataPos, nbrNodeDataPos, storage,
            lowerBound, upperBound, children[0]->clone(), id, paramsString);
    }

private:
    // This function resets the dfsLevelInfo at level and adds the dfsLevelInfo to the
    // dfsStack if the parent has adjacent nodes. The function returns true if the
    // parent has adjacent nodes, otherwise returns false.
    bool addDFSLevelToStackIfParentExtends(
        shared_ptr<ValueVector>& parentValueVector, uint8_t level);
};

} // namespace processor
} // namespace kuzu
