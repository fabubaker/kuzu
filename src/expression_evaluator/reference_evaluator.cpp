#include "include/reference_evaluator.h"

namespace kuzu {
namespace evaluator {

inline static bool isTrue(ValueVector& vector, uint64_t pos) {
    assert(vector.dataType.typeID == BOOL);
    return !vector.isNull(pos) && ((bool*)vector.values)[pos];
}

void ReferenceExpressionEvaluator::init(const ResultSet& resultSet, MemoryManager* memoryManager) {
    assert(children.empty());
    resultVector =
        resultSet.dataChunks[vectorPos.dataChunkPos]->valueVectors[vectorPos.valueVectorPos];
}

bool ReferenceExpressionEvaluator::select(SelectionVector& selVector) {
    uint64_t numSelectedValues = 0;
    if (resultVector->state->isFlat()) {
        auto pos = resultVector->state->getPositionOfCurrIdx();
        numSelectedValues += isTrue(*resultVector, pos);
    } else {
        auto selectedBuffer = resultVector->state->selVector->getSelectedPositionsBuffer();
        if (resultVector->state->selVector->isUnfiltered()) {
            for (auto i = 0u; i < resultVector->state->selVector->selectedSize; i++) {
                selectedBuffer[numSelectedValues] = i;
                numSelectedValues += isTrue(*resultVector, i);
            }
        } else {
            for (auto i = 0u; i < resultVector->state->selVector->selectedSize; i++) {
                auto pos = resultVector->state->selVector->selectedPositions[i];
                selectedBuffer[numSelectedValues] = pos;
                numSelectedValues += isTrue(*resultVector, pos);
            }
        }
        selVector.selectedSize = numSelectedValues;
    }
    return numSelectedValues > 0;
}

} // namespace evaluator
} // namespace kuzu
