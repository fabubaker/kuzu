#include "test/binder/binder_test_utils.h"

bool BinderTestUtils::equals(const Expression& left, const Expression& right) {
    auto result = left.expressionType == right.expressionType && left.dataType == right.dataType &&
                  left.children.size() == right.children.size() &&
                  left.getUniqueName() == right.getUniqueName();
    if (!result) {
        return false;
    }
    for (auto i = 0u; i < left.children.size(); ++i) {
        if (!equals(*left.children[i], *right.children[i])) {
            return false;
        }
    }
    if (EXISTENTIAL_SUBQUERY == left.expressionType) {
        throw invalid_argument(
            "Equal comparison of ExistentialSubqueryExpression is not implemented.");
    } else if (PROPERTY == left.expressionType) {
        return equals((PropertyExpression&)left, (PropertyExpression&)right);
    } else if (VARIABLE == left.expressionType) {
        if (NODE == left.dataType) {
            return equals((NodeExpression&)left, (NodeExpression&)right);
        } else if (REL == left.dataType) {
            return equals((RelExpression&)left, (RelExpression&)right);
        } else {
            return true;
        }
    } else if (isExpressionLiteral(left.expressionType)) {
        return equals((LiteralExpression&)left, (LiteralExpression&)right);
    }
    return true;
}

bool BinderTestUtils::equals(const PropertyExpression& left, const PropertyExpression& right) {
    return left.propertyKey == right.propertyKey;
}

bool BinderTestUtils::equals(const RelExpression& left, const RelExpression& right) {
    return left.label == right.label && equals(*left.srcNode, *right.srcNode) &&
           equals(*left.dstNode, *right.dstNode);
}

bool BinderTestUtils::equals(const NodeExpression& left, const NodeExpression& right) {
    return left.label == right.label;
}

bool BinderTestUtils::equals(const LiteralExpression& left, const LiteralExpression& right) {
    return left.literal.toString() == right.literal.toString();
}
