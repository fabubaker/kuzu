#pragma once

#include "node_pattern.h"

namespace kuzu {
namespace parser {

enum ArrowDirection : uint8_t { LEFT = 0, RIGHT = 1 };

/**
 * RelationshipPattern represents "-[relName:RelTable]-"
 */
class RelPattern : public NodePattern {

public:
    RelPattern(string name, string tableName, string lowerBound, string upperBound,
        ArrowDirection arrowDirection,
        vector<pair<string, unique_ptr<ParsedExpression>>> propertyKeyValPairs)
        : NodePattern{std::move(name), std::move(tableName), std::move(propertyKeyValPairs)},
          lowerBound{std::move(lowerBound)}, upperBound{std::move(upperBound)},
          arrowDirection{arrowDirection} {}

    ~RelPattern() = default;

    inline string getLowerBound() const { return lowerBound; }

    inline string getUpperBound() const { return upperBound; }

    inline ArrowDirection getDirection() const { return arrowDirection; }

    bool equals(const kuzu::parser::NodePattern& other) const override {
        auto& otherRel = (RelPattern&)other;
        return NodePattern::equals(other) && lowerBound == otherRel.lowerBound &&
               upperBound == otherRel.upperBound && arrowDirection == otherRel.arrowDirection;
    }

private:
    string lowerBound;
    string upperBound;
    ArrowDirection arrowDirection;
};

} // namespace parser
} // namespace kuzu
