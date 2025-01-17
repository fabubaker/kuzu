#pragma once

#include "src/processor/operator/aggregate/include/base_aggregate_scan.h"
#include "src/processor/operator/aggregate/include/simple_aggregate.h"

namespace kuzu {
namespace processor {

class SimpleAggregateScan : public BaseAggregateScan {

public:
    SimpleAggregateScan(shared_ptr<SimpleAggregateSharedState> sharedState,
        unique_ptr<ResultSetDescriptor> resultSetDescriptor, vector<DataPos> aggregatesPos,
        vector<DataType> aggregateDataTypes, unique_ptr<PhysicalOperator> child, uint32_t id,
        const string& paramsString)
        : BaseAggregateScan{move(resultSetDescriptor), move(aggregatesPos),
              move(aggregateDataTypes), move(child), id, paramsString},
          sharedState{move(sharedState)} {}

    // This constructor is used for cloning only.
    SimpleAggregateScan(shared_ptr<SimpleAggregateSharedState> sharedState,
        unique_ptr<ResultSetDescriptor> resultSetDescriptor, vector<DataPos> aggregatesPos,
        vector<DataType> aggregateDataTypes, uint32_t id, const string& paramsString)
        : BaseAggregateScan{move(resultSetDescriptor), move(aggregatesPos),
              move(aggregateDataTypes), id, paramsString},
          sharedState{move(sharedState)} {}

    bool getNextTuples() override;

    // SimpleAggregateScan is the source operator of a pipeline, so it should not clone its child.
    unique_ptr<PhysicalOperator> clone() override {
        return make_unique<SimpleAggregateScan>(sharedState, resultSetDescriptor->copy(),
            aggregatesPos, aggregateDataTypes, id, paramsString);
    }

private:
    shared_ptr<SimpleAggregateSharedState> sharedState;
};

} // namespace processor
} // namespace kuzu
