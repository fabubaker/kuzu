#include "include/query_graph.h"

namespace kuzu {
namespace binder {

std::size_t SubqueryGraphHasher::operator()(const SubqueryGraph& key) const {
    if (0 == key.queryRelsSelector.count()) {
        return hash<bitset<MAX_NUM_VARIABLES>>{}(key.queryNodesSelector);
    }
    return hash<bitset<MAX_NUM_VARIABLES>>{}(key.queryRelsSelector);
}

bool SubqueryGraph::containAllVariables(unordered_set<string>& variables) const {
    for (auto& var : variables) {
        if (queryGraph.containsQueryNode(var) &&
            !queryNodesSelector[queryGraph.getQueryNodePos(var)]) {
            return false;
        }
        if (queryGraph.containsQueryRel(var) &&
            !queryRelsSelector[queryGraph.getQueryRelPos(var)]) {
            return false;
        }
    }
    return true;
}

unordered_set<uint32_t> SubqueryGraph::getNodeNbrPositions() const {
    unordered_set<uint32_t> result;
    for (auto relPos = 0u; relPos < queryGraph.getNumQueryRels(); ++relPos) {
        if (!queryRelsSelector[relPos]) { // rel not in subgraph, no need to check
            continue;
        }
        auto rel = queryGraph.getQueryRel(relPos);
        auto srcNodePos = queryGraph.getQueryNodePos(*rel->getSrcNode());
        if (!queryNodesSelector[srcNodePos]) {
            result.insert(srcNodePos);
        }
        auto dstNodePos = queryGraph.getQueryNodePos(*rel->getDstNode());
        if (!queryNodesSelector[dstNodePos]) {
            result.insert(dstNodePos);
        }
    }
    return result;
}

unordered_set<uint32_t> SubqueryGraph::getRelNbrPositions() const {
    unordered_set<uint32_t> result;
    for (auto relPos = 0u; relPos < queryGraph.getNumQueryRels(); ++relPos) {
        if (queryRelsSelector[relPos]) { // rel already in subgraph, cannot be rel neighbour
            continue;
        }
        auto rel = queryGraph.getQueryRel(relPos);
        auto srcNodePos = queryGraph.getQueryNodePos(*rel->getSrcNode());
        auto dstNodePos = queryGraph.getQueryNodePos(*rel->getDstNode());
        if (queryNodesSelector[srcNodePos] || queryNodesSelector[dstNodePos]) {
            result.insert(relPos);
        }
    }
    return result;
}

unordered_set<SubqueryGraph, SubqueryGraphHasher> SubqueryGraph::getNbrSubgraphs(
    uint32_t size) const {
    auto result = getBaseNbrSubgraph();
    for (auto i = 1u; i < size; ++i) {
        unordered_set<SubqueryGraph, SubqueryGraphHasher> tmp;
        for (auto& prevNbr : result) {
            for (auto& subgraph : getNextNbrSubgraphs(prevNbr)) {
                tmp.insert(subgraph);
            }
        }
        result = move(tmp);
    }
    return result;
}

vector<uint32_t> SubqueryGraph::getConnectedNodePos(const SubqueryGraph& nbr) const {
    vector<uint32_t> result;
    for (auto& nodePos : getNodeNbrPositions()) {
        if (nbr.queryNodesSelector[nodePos]) {
            result.push_back(nodePos);
        }
    }
    for (auto& nodePos : nbr.getNodeNbrPositions()) {
        if (queryNodesSelector[nodePos]) {
            result.push_back(nodePos);
        }
    }
    return result;
}

unordered_set<uint32_t> SubqueryGraph::getNodePositionsIgnoringNodeSelector() const {
    unordered_set<uint32_t> result;
    for (auto nodePos = 0u; nodePos < queryGraph.getNumQueryNodes(); ++nodePos) {
        if (queryNodesSelector[nodePos]) {
            result.insert(nodePos);
        }
    }
    for (auto relPos = 0u; relPos < queryGraph.getNumQueryRels(); ++relPos) {
        auto rel = queryGraph.getQueryRel(relPos);
        if (queryRelsSelector[relPos]) {
            result.insert(queryGraph.getQueryNodePos(rel->getSrcNodeName()));
            result.insert(queryGraph.getQueryNodePos(rel->getDstNodeName()));
        }
    }
    return result;
}

unordered_set<SubqueryGraph, SubqueryGraphHasher> SubqueryGraph::getBaseNbrSubgraph() const {
    unordered_set<SubqueryGraph, SubqueryGraphHasher> result;
    for (auto& nodePos : getNodeNbrPositions()) {
        auto nbr = SubqueryGraph(queryGraph);
        nbr.addQueryNode(nodePos);
        result.insert(move(nbr));
    }
    for (auto& relPos : getRelNbrPositions()) {
        auto nbr = SubqueryGraph(queryGraph);
        nbr.addQueryRel(relPos);
        result.insert(move(nbr));
    }
    return result;
}

unordered_set<SubqueryGraph, SubqueryGraphHasher> SubqueryGraph::getNextNbrSubgraphs(
    const SubqueryGraph& prevNbr) const {
    unordered_set<SubqueryGraph, SubqueryGraphHasher> result;
    for (auto& nodePos : prevNbr.getNodeNbrPositions()) {
        if (queryNodesSelector[nodePos]) {
            continue;
        }
        auto nbr = prevNbr;
        nbr.addQueryNode(nodePos);
        result.insert(move(nbr));
    }
    for (auto& relPos : prevNbr.getRelNbrPositions()) {
        if (queryRelsSelector[relPos]) {
            continue;
        }
        auto nbr = prevNbr;
        nbr.addQueryRel(relPos);
        result.insert(move(nbr));
    }
    return result;
}

void QueryGraph::addQueryNode(shared_ptr<NodeExpression> queryNode) {
    // Note that a node may be added multiple times. We should only keep one of it.
    // E.g. MATCH (a:person)-[:knows]->(b:person), (a)-[:knows]->(c:person)
    // a will be added twice during binding
    if (containsQueryNode(queryNode->getUniqueName())) {
        return;
    }
    queryNodeNameToPosMap.insert({queryNode->getUniqueName(), queryNodes.size()});
    queryNodes.push_back(queryNode);
}

void QueryGraph::addQueryRel(shared_ptr<RelExpression> queryRel) {
    assert(!containsQueryRel(queryRel->getUniqueName()));
    queryRelNameToPosMap.insert({queryRel->getUniqueName(), queryRels.size()});
    queryRels.push_back(queryRel);
}

void QueryGraph::merge(const QueryGraph& other) {
    for (auto& otherNode : other.queryNodes) {
        addQueryNode(otherNode);
    }
    for (auto& otherRel : other.queryRels) {
        addQueryRel(otherRel);
    }
}

bool QueryGraph::canProjectExpression(Expression* expression) const {
    for (auto& variable : expression->getDependentVariableNames()) {
        if (!containsQueryNode(variable) && !containsQueryRel(variable)) {
            return false;
        }
    }
    return true;
}

bool QueryGraph::isConnected(const QueryGraph& other) {
    for (auto& queryNode : queryNodes) {
        if (other.containsQueryNode(queryNode->getUniqueName())) {
            return true;
        }
    }
    return false;
}

vector<shared_ptr<Expression>> QueryGraph::getNodeIDExpressions() const {
    vector<shared_ptr<Expression>> result;
    for (auto& queryNode : queryNodes) {
        result.push_back(queryNode->getNodeIDPropertyExpression());
    }
    return result;
}

void QueryGraphCollection::addAndMergeQueryGraphIfConnected(
    unique_ptr<QueryGraph> queryGraphToAdd) {
    bool isMerged = false;
    for (auto& queryGraph : queryGraphs) {
        if (queryGraph->isConnected(*queryGraphToAdd)) {
            queryGraph->merge(*queryGraphToAdd);
            isMerged = true;
        }
    }
    if (!isMerged) {
        queryGraphs.push_back(std::move(queryGraphToAdd));
    }
}

expression_vector QueryGraphCollection::getNodeIDExpressions() const {
    expression_vector result;
    for (auto& queryGraph : queryGraphs) {
        for (auto& nodeID : queryGraph->getNodeIDExpressions()) {
            result.push_back(nodeID);
        }
    }
    return result;
}

unique_ptr<QueryGraphCollection> QueryGraphCollection::copy() const {
    auto result = make_unique<QueryGraphCollection>();
    for (auto& queryGraph : queryGraphs) {
        result->queryGraphs.push_back(queryGraph->copy());
    }
    return result;
}

void PropertyKeyValCollection::addPropertyKeyValPair(
    const Expression& variable, expression_pair propertyKeyValPair) {
    auto varName = variable.getUniqueName();
    if (!varNameToPropertyKeyValPairs.contains(varName)) {
        varNameToPropertyKeyValPairs.insert({varName, unordered_map<string, expression_pair>{}});
    }
    auto property = (PropertyExpression*)propertyKeyValPair.first.get();
    assert(!varNameToPropertyKeyValPairs.at(varName).contains(property->getPropertyName()));
    varNameToPropertyKeyValPairs.at(varName).insert(
        {property->getPropertyName(), std::move(propertyKeyValPair)});
}

vector<expression_pair> PropertyKeyValCollection::getPropertyKeyValPairs(
    const kuzu::binder::Expression& variable) const {
    auto varName = variable.getUniqueName();
    if (!varNameToPropertyKeyValPairs.contains(varName)) {
        return vector<expression_pair>{};
    }
    vector<expression_pair> result;
    for (auto& [_, setItem] : varNameToPropertyKeyValPairs.at(varName)) {
        result.push_back(setItem);
    }
    return result;
}

vector<expression_pair> PropertyKeyValCollection::getAllPropertyKeyValPairs() const {
    vector<expression_pair> result;
    for (auto& [varName, keyValPairsMap] : varNameToPropertyKeyValPairs) {
        for (auto& [propertyName, keyValPairs] : keyValPairsMap) {
            result.push_back(keyValPairs);
        }
    }
    return result;
}

bool PropertyKeyValCollection::hasPropertyKeyValPair(
    const Expression& variable, const string& propertyName) const {
    auto varName = variable.getUniqueName();
    if (!varNameToPropertyKeyValPairs.contains(varName)) {
        return false;
    }
    if (!varNameToPropertyKeyValPairs.at(varName).contains(propertyName)) {
        return false;
    }
    return true;
}

expression_pair PropertyKeyValCollection::getPropertyKeyValPair(
    const Expression& variable, const string& propertyName) const {
    assert(hasPropertyKeyValPair(variable, propertyName));
    return varNameToPropertyKeyValPairs.at(variable.getUniqueName()).at(propertyName);
}

} // namespace binder
} // namespace kuzu
