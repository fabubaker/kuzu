#pragma once

#include "query_summary.h"

#include "src/common/types/include/types.h"
#include "src/processor/include/physical_plan/result/factorized_table.h"
#include "src/processor/include/physical_plan/result/flat_tuple.h"

using namespace graphflow::processor;

namespace graphflow {
namespace main {

struct QueryResultHeader {

    explicit QueryResultHeader(std::vector<common::DataType> columnDataTypes)
        : columnDataTypes{move(columnDataTypes)} {};

    std::vector<common::DataType> columnDataTypes;
};

class QueryResult {
    friend class Connection;

public:
    QueryResult() = default;
    ~QueryResult() = default;

    inline bool isSuccess() const { return success; }
    inline string getErrorMessage() const { return errMsg; }

    inline void setResultHeaderAndTable(std::unique_ptr<QueryResultHeader> header,
        std::shared_ptr<processor::FactorizedTable> factorizedTable) {
        this->header = move(header);
        this->factorizedTable = move(factorizedTable);
        resetIterator();
    }

    bool hasNext();

    // TODO: this is not efficient and should be replaced by iterator
    std::unique_ptr<processor::FlatTuple> getNext();

    inline uint64_t getNumColumns() const { return header->columnDataTypes.size(); }

    inline QuerySummary* getQuerySummary() const { return querySummary.get(); }

    // TODO: interfaces below should be removed
    // used in shell to walk the result twice (first time getting maximum column width)
    inline void resetIterator() { iterator = make_unique<FlatTupleIterator>(*factorizedTable); }

    inline uint64_t getNumTuples() {
        return querySummary->getIsExplain() ? 0 : factorizedTable->getTotalNumFlatTuples();
    }

private:
    void validateQuerySucceed();

    bool success = true;
    std::string errMsg;

    std::unique_ptr<QueryResultHeader> header;
    std::shared_ptr<processor::FactorizedTable> factorizedTable;
    std::unique_ptr<processor::FlatTupleIterator> iterator;
    std::unique_ptr<QuerySummary> querySummary;
};

} // namespace main
} // namespace graphflow