#include "include/plan_mapper.h"

#include "src/binder/expression/include/node_expression.h"
#include "src/planner/logical_plan/logical_operator/include/logical_create.h"
#include "src/processor/operator/update/include/create.h"

namespace kuzu {
namespace processor {

unique_ptr<PhysicalOperator> PlanMapper::mapLogicalCreateNodeToPhysical(
    LogicalOperator* logicalOperator, MapperContext& mapperContext) {
    auto logicalCreateNode = (LogicalCreateNode*)logicalOperator;
    auto prevOperator = mapLogicalOperatorToPhysical(logicalOperator->getChild(0), mapperContext);
    auto& nodesStore = storageManager.getNodesStore();
    auto catalogContent = catalog->getReadOnlyVersion();
    vector<unique_ptr<CreateNodeInfo>> createNodeInfos;
    for (auto i = 0u; i < logicalCreateNode->getNumNodeAndPrimaryKeys(); ++i) {
        auto nodeAndPrimaryKey = logicalCreateNode->getNodeAndPrimaryKey(i);
        auto nodeTableID = nodeAndPrimaryKey->node->getTableID();
        auto table = nodesStore.getNodeTable(nodeTableID);
        auto primaryKeyEvaluator =
            expressionMapper.mapExpression(nodeAndPrimaryKey->primaryKey, mapperContext);
        vector<RelTable*> relTablesToInit;
        for (auto& [relTableID, relTableSchema] : catalogContent->getRelTableSchemas()) {
            if (relTableSchema->edgeContainsNodeTable(nodeTableID)) {
                relTablesToInit.push_back(storageManager.getRelsStore().getRelTable(relTableID));
            }
        }
        auto outDataPos = mapperContext.getDataPos(nodeAndPrimaryKey->node->getIDProperty());
        createNodeInfos.push_back(make_unique<CreateNodeInfo>(
            table, std::move(primaryKeyEvaluator), relTablesToInit, outDataPos));
    }
    return make_unique<CreateNode>(std::move(createNodeInfos), std::move(prevOperator),
        getOperatorID(), logicalCreateNode->getExpressionsForPrinting());
}

unique_ptr<PhysicalOperator> PlanMapper::mapLogicalCreateRelToPhysical(
    LogicalOperator* logicalOperator, MapperContext& mapperContext) {
    auto logicalCreateRel = (LogicalCreateRel*)logicalOperator;
    auto prevOperator = mapLogicalOperatorToPhysical(logicalOperator->getChild(0), mapperContext);
    auto& relStore = storageManager.getRelsStore();
    vector<unique_ptr<CreateRelInfo>> createRelInfos;
    for (auto i = 0u; i < logicalCreateRel->getNumRels(); ++i) {
        auto rel = logicalCreateRel->getRel(i);
        auto table = relStore.getRelTable(rel->getTableID());
        auto srcNodePos = mapperContext.getDataPos(rel->getSrcNode()->getIDProperty());
        auto srcNodeTableID = rel->getSrcNode()->getTableID();
        auto dstNodePos = mapperContext.getDataPos(rel->getDstNode()->getIDProperty());
        auto dstNodeTableID = rel->getDstNode()->getTableID();
        vector<unique_ptr<BaseExpressionEvaluator>> evaluators;
        uint32_t relIDEvaluatorIdx = UINT32_MAX;
        auto setItems = logicalCreateRel->getSetItems(i);
        for (auto j = 0u; j < setItems.size(); ++j) {
            auto& [lhs, rhs] = setItems[j];
            auto propertyExpression = static_pointer_cast<PropertyExpression>(lhs);
            if (propertyExpression->isInternalID()) {
                relIDEvaluatorIdx = j;
            }
            evaluators.push_back(expressionMapper.mapExpression(rhs, mapperContext));
        }
        assert(relIDEvaluatorIdx != UINT32_MAX);
        createRelInfos.push_back(make_unique<CreateRelInfo>(table, srcNodePos, srcNodeTableID,
            dstNodePos, dstNodeTableID, std::move(evaluators), relIDEvaluatorIdx));
    }
    return make_unique<CreateRel>(relStore.getRelsStatistics(), std::move(createRelInfos),
        std::move(prevOperator), getOperatorID(), logicalOperator->getExpressionsForPrinting());
}

} // namespace processor
} // namespace kuzu
