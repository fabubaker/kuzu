#include "include/plan_mapper.h"

#include "src/binder/expression/include/node_expression.h"
#include "src/planner/logical_plan/logical_operator/include/logical_delete.h"
#include "src/processor/operator/update/include/delete.h"

namespace kuzu {
namespace processor {

unique_ptr<PhysicalOperator> PlanMapper::mapLogicalDeleteToPhysical(
    LogicalOperator* logicalOperator, MapperContext& mapperContext) {
    auto& logicalDelete = (LogicalDelete&)*logicalOperator;
    auto prevOperator = mapLogicalOperatorToPhysical(logicalOperator->getChild(0), mapperContext);
    auto& nodesStore = storageManager.getNodesStore();
    vector<DataPos> nodeIDVectorPositions;
    vector<DataPos> primaryKeyVectorPositions;
    vector<NodeTable*> nodeTables;
    for (auto i = 0u; i < logicalDelete.getNumExpressions(); ++i) {
        auto& nodeExpression = (NodeExpression&)*logicalDelete.getNodeExpression(i);
        auto nodeIDExpression = nodeExpression.getNodeIDPropertyExpression();
        nodeIDVectorPositions.push_back(
            mapperContext.getDataPos(nodeIDExpression->getUniqueName()));
        nodeTables.push_back(nodesStore.getNodeTable(nodeExpression.getTableID()));
        auto primaryKeyExpression = logicalDelete.getPrimaryKeyExpression(i);
        primaryKeyVectorPositions.push_back(
            mapperContext.getDataPos(primaryKeyExpression->getUniqueName()));
    }
    return make_unique<DeleteNodeStructuredProperty>(move(nodeIDVectorPositions),
        move(primaryKeyVectorPositions), move(nodeTables), move(prevOperator), getOperatorID(),
        logicalDelete.getExpressionsForPrinting());
}

} // namespace processor
} // namespace kuzu
