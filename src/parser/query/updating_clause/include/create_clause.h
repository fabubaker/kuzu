#pragma once

#include "updating_clause.h"

#include "src/parser/query/graph_pattern/include/pattern_element.h"

namespace kuzu {
namespace parser {

class CreateClause : public UpdatingClause {
public:
    CreateClause(vector<unique_ptr<PatternElement>> patternElements)
        : UpdatingClause{ClauseType::CREATE}, patternElements{std::move(patternElements)} {};
    ~CreateClause() override = default;

    inline const vector<unique_ptr<PatternElement>>& getPatternElements() const {
        return patternElements;
    }

private:
    vector<unique_ptr<PatternElement>> patternElements;
};

} // namespace parser
} // namespace kuzu
