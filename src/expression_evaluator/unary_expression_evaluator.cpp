#include "src/expression_evaluator/include/unary_expression_evaluator.h"

namespace graphflow {
namespace evaluator {

UnaryExpressionEvaluator::UnaryExpressionEvaluator(MemoryManager& memoryManager,
    unique_ptr<ExpressionEvaluator> child, ExpressionType expressionType, DataType dataType) {
    childrenExpr.push_back(move(child));
    this->expressionType = expressionType;
    this->dataType = dataType;
    operation = getUnaryOperation(expressionType);
    result = createResultValueVector(memoryManager);
}

void UnaryExpressionEvaluator::evaluate() {
    childrenExpr[0]->evaluate();
    operation(*childrenExpr[0]->result, *result);
}

shared_ptr<ValueVector> UnaryExpressionEvaluator::createResultValueVector(
    MemoryManager& memoryManager) {
    auto valueVector = make_shared<ValueVector>(&memoryManager, dataType);
    if (expressionType == CAST_TO_UNSTRUCTURED_VECTOR) {
        auto unstructuredValues = (Value*)valueVector->values;
        for (auto i = 0u; i < DEFAULT_VECTOR_CAPACITY; i++) {
            unstructuredValues[i].dataType = childrenExpr[0]->dataType;
        }
    }
    valueVector->state = childrenExpr[0]->result->state;
    valueVector->nullMask = childrenExpr[0]->result->nullMask;
    return valueVector;
}

} // namespace evaluator
} // namespace graphflow