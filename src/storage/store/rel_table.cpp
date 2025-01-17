#include "src/storage/store/include/rel_table.h"

#include "spdlog/spdlog.h"

using namespace kuzu::catalog;

namespace kuzu {
namespace storage {

RelTable::RelTable(const Catalog& catalog, table_id_t tableID, BufferManager& bufferManager,
    MemoryManager& memoryManager, bool isInMemoryMode, WAL* wal)
    : logger{LoggerUtils::getOrCreateLogger("storage")}, tableID{tableID},
      isInMemoryMode{isInMemoryMode},
      adjAndPropertyListsUpdateStore{make_unique<AdjAndPropertyListsUpdateStore>(
          memoryManager, *catalog.getReadOnlyVersion()->getRelTableSchema(tableID))},
      wal{wal} {
    loadColumnsAndListsFromDisk(catalog, bufferManager);
}

void RelTable::loadColumnsAndListsFromDisk(
    const catalog::Catalog& catalog, BufferManager& bufferManager) {
    initAdjColumnOrLists(catalog, bufferManager, wal);
    initPropertyListsAndColumns(catalog, bufferManager, wal);
}

vector<AdjLists*> RelTable::getAdjListsForNodeTable(table_id_t tableID) {
    vector<AdjLists*> retVal;
    auto it = adjLists[FWD].find(tableID);
    if (it != adjLists[FWD].end()) {
        retVal.push_back(it->second.get());
    }
    it = adjLists[BWD].find(tableID);
    if (it != adjLists[BWD].end()) {
        retVal.push_back(it->second.get());
    }
    return retVal;
}

vector<AdjColumn*> RelTable::getAdjColumnsForNodeTable(table_id_t tableID) {
    vector<AdjColumn*> retVal;
    auto it = adjColumns[FWD].find(tableID);
    if (it != adjColumns[FWD].end()) {
        retVal.push_back(it->second.get());
    }
    it = adjColumns[BWD].find(tableID);
    if (it != adjColumns[BWD].end()) {
        retVal.push_back(it->second.get());
    }
    return retVal;
}

void RelTable::prepareCommitOrRollbackIfNecessary(bool isCommit) {
    performOpOnListsWithUpdates(
        std::bind(&Lists::prepareCommitOrRollbackIfNecessary, std::placeholders::_1, isCommit),
        std::bind(&RelTable::addToUpdatedRelTables, this));
}

void RelTable::checkpointInMemoryIfNecessary() {
    performOpOnListsWithUpdates(
        std::bind(&Lists::checkpointInMemoryIfNecessary, std::placeholders::_1),
        std::bind(&RelTable::clearAdjAndPropertyListsUpdateStore, this));
}

void RelTable::rollbackInMemoryIfNecessary() {
    performOpOnListsWithUpdates(
        std::bind(&Lists::rollbackInMemoryIfNecessary, std::placeholders::_1),
        std::bind(&RelTable::clearAdjAndPropertyListsUpdateStore, this));
}

// This function assumes that the order of vectors in relPropertyVectorsPerRelTable as:
// [relProp1, relProp2, ..., relPropN] and all vectors are flat.
void RelTable::insertRels(shared_ptr<ValueVector>& srcNodeIDVector,
    shared_ptr<ValueVector>& dstNodeIDVector, vector<shared_ptr<ValueVector>>& relPropertyVectors) {
    assert(srcNodeIDVector->state->isFlat());
    assert(dstNodeIDVector->state->isFlat());
    auto srcTableID =
        ((nodeID_t*)srcNodeIDVector
                ->values)[srcNodeIDVector->state->selVector
                              ->selectedPositions[srcNodeIDVector->state->getPositionOfCurrIdx()]]
            .tableID;
    auto dstTableID =
        ((nodeID_t*)dstNodeIDVector
                ->values)[dstNodeIDVector->state->selVector
                              ->selectedPositions[dstNodeIDVector->state->getPositionOfCurrIdx()]]
            .tableID;
    for (auto direction : REL_DIRECTIONS) {
        auto boundTableID = (direction == RelDirection::FWD ? srcTableID : dstTableID);
        auto boundVector = (direction == RelDirection::FWD ? srcNodeIDVector : dstNodeIDVector);
        auto nbrVector = (direction == RelDirection::FWD ? dstNodeIDVector : srcNodeIDVector);
        if (adjColumns[direction].contains(boundTableID)) {
            auto nodeOffset =
                boundVector->readNodeOffset(boundVector->state->getPositionOfCurrIdx());
            if (!adjColumns[direction]
                     .at(boundTableID)
                     ->isNull(nodeOffset, Transaction::getDummyWriteTrx().get())) {
                throw RuntimeException(StringUtils::string_format(
                    "RelTable %d is a %s table, but node(nodeOffset: %d, tableID: %d) has "
                    "more than one neighbour in the %s direction.",
                    tableID, inferRelMultiplicity(srcTableID, dstTableID).c_str(), nodeOffset,
                    boundTableID, getRelDirectionAsString(direction).c_str()));
            }
            adjColumns[direction].at(boundTableID)->writeValues(boundVector, nbrVector);
            for (auto i = 0; i < relPropertyVectors.size(); i++) {
                propertyColumns[direction].at(boundTableID)[i]->writeValues(
                    boundVector, relPropertyVectors[i]);
            }
        }
    }
    adjAndPropertyListsUpdateStore->insertRelIfNecessary(
        srcNodeIDVector, dstNodeIDVector, relPropertyVectors);
}

void RelTable::initEmptyRelsForNewNode(nodeID_t& nodeID) {
    for (auto direction : REL_DIRECTIONS) {
        if (adjColumns[direction].contains(nodeID.tableID)) {
            adjColumns[direction].at(nodeID.tableID)->setNodeOffsetToNull(nodeID.offset);
        }
    }
    adjAndPropertyListsUpdateStore->initEmptyListInPersistentStore(nodeID);
}

void RelTable::initAdjColumnOrLists(
    const Catalog& catalog, BufferManager& bufferManager, WAL* wal) {
    logger->info("Initializing AdjColumns and AdjLists for rel {}.", tableID);
    adjColumns = vector<table_adj_columns_map_t>{2};
    adjLists = vector<table_adj_lists_map_t>{2};
    for (auto relDirection : REL_DIRECTIONS) {
        const auto& nodeTableIDs =
            catalog.getReadOnlyVersion()->getNodeTableIDsForRelTableDirection(
                tableID, relDirection);
        const auto& nbrNodeTableIDs =
            catalog.getReadOnlyVersion()->getNodeTableIDsForRelTableDirection(
                tableID, !relDirection);
        for (auto nodeTableID : nodeTableIDs) {
            NodeIDCompressionScheme nodeIDCompressionScheme(nbrNodeTableIDs);
            logger->debug("DIRECTION {} nodeTableForAdjColumnAndProperties {} relTable {} "
                          "nodeIDCompressionScheme: commonTableID: {}",
                relDirection, nodeTableID, tableID, nodeIDCompressionScheme.getCommonTableID());
            if (catalog.getReadOnlyVersion()->isSingleMultiplicityInDirection(
                    tableID, relDirection)) {
                // Add adj column.
                auto adjColumn = make_unique<AdjColumn>(
                    StorageUtils::getAdjColumnStructureIDAndFName(
                        wal->getDirectory(), tableID, nodeTableID, relDirection),
                    bufferManager, nodeIDCompressionScheme, isInMemoryMode, wal);
                adjColumns[relDirection].emplace(nodeTableID, move(adjColumn));
            } else {
                // Add adj list.
                auto adjList = make_unique<AdjLists>(
                    StorageUtils::getAdjListsStructureIDAndFName(
                        wal->getDirectory(), tableID, nodeTableID, relDirection),
                    bufferManager, nodeIDCompressionScheme, isInMemoryMode, wal,
                    adjAndPropertyListsUpdateStore.get());
                adjLists[relDirection].emplace(nodeTableID, move(adjList));
            }
        }
    }
    logger->info("Initializing AdjColumns and AdjLists for rel {} done.", tableID);
}

void RelTable::initPropertyListsAndColumns(
    const Catalog& catalog, BufferManager& bufferManager, WAL* wal) {
    logger->info("Initializing PropertyLists and PropertyColumns for rel {}.", tableID);
    propertyLists = vector<table_property_lists_map_t>{2};
    propertyColumns = vector<table_property_columns_map_t>{2};
    if (!catalog.getReadOnlyVersion()->getRelProperties(tableID).empty()) {
        for (auto relDirection : REL_DIRECTIONS) {
            if (catalog.getReadOnlyVersion()->isSingleMultiplicityInDirection(
                    tableID, relDirection)) {
                initPropertyColumnsForRelTable(catalog, relDirection, bufferManager, wal);
            } else {
                initPropertyListsForRelTable(catalog, relDirection, bufferManager, wal);
            }
        }
    }
    logger->info("Initializing PropertyLists and PropertyColumns for rel {} Done.", tableID);
}

void RelTable::initPropertyColumnsForRelTable(
    const Catalog& catalog, RelDirection relDirection, BufferManager& bufferManager, WAL* wal) {
    logger->debug("Initializing PropertyColumns: relTable {}", tableID);
    for (auto& nodeTableID :
        catalog.getReadOnlyVersion()->getNodeTableIDsForRelTableDirection(tableID, relDirection)) {
        auto& properties = catalog.getReadOnlyVersion()->getRelProperties(tableID);
        propertyColumns[relDirection].emplace(
            nodeTableID, vector<unique_ptr<Column>>(properties.size()));
        for (auto& property : properties) {
            logger->debug(
                "DIR {} nodeIDForAdjColumnAndProperties {} propertyIdx {} type {} name `{}`",
                relDirection, nodeTableID, property.propertyID, property.dataType.typeID,
                property.name);
            propertyColumns[relDirection].at(nodeTableID)[property.propertyID] =
                ColumnFactory::getColumn(
                    StorageUtils::getRelPropertyColumnStructureIDAndFName(wal->getDirectory(),
                        tableID, nodeTableID, relDirection, property.propertyID),
                    property.dataType, bufferManager, isInMemoryMode, wal);
        }
    }
    logger->debug("Initializing PropertyColumns done.");
}

void RelTable::initPropertyListsForRelTable(
    const Catalog& catalog, RelDirection relDirection, BufferManager& bufferManager, WAL* wal) {
    logger->debug("Initializing PropertyLists for rel {}", tableID);
    for (auto& nodeTableID :
        catalog.getReadOnlyVersion()->getNodeTableIDsForRelTableDirection(tableID, relDirection)) {
        auto& properties = catalog.getReadOnlyVersion()->getRelProperties(tableID);
        auto adjListsHeaders = adjLists[relDirection].at(nodeTableID)->getHeaders();
        propertyLists[relDirection].emplace(nodeTableID,
            vector<unique_ptr<ListsWithAdjAndPropertyListsUpdateStore>>(properties.size()));
        for (auto& property : properties) {
            auto propertyID = property.propertyID;
            logger->debug("relDirection {} nodeTableForAdjColumnAndProperties {} propertyIdx {} "
                          "type {} name `{}`",
                relDirection, nodeTableID, propertyID, property.dataType.typeID, property.name);
            propertyLists[relDirection].at(nodeTableID)[property.propertyID] =
                ListsFactory::getListsWithAdjAndPropertyListsUpdateStore(
                    StorageUtils::getRelPropertyListsStructureIDAndFName(
                        wal->getDirectory(), tableID, nodeTableID, relDirection, property),
                    property.dataType, adjListsHeaders, bufferManager, isInMemoryMode, wal,
                    adjAndPropertyListsUpdateStore.get());
        }
    }
    logger->debug("Initializing PropertyLists for rel {} done.", tableID);
}

void RelTable::performOpOnListsWithUpdates(
    std::function<void(Lists*)> opOnListsWithUpdates, std::function<void()> opIfHasUpdates) {
    auto& listUpdatesPerDirection =
        adjAndPropertyListsUpdateStore->getListUpdatesPerTablePerDirection();
    for (auto& relDirection : REL_DIRECTIONS) {
        for (auto& listUpdatesPerTable : listUpdatesPerDirection[relDirection]) {
            if (!listUpdatesPerTable.second.empty()) {
                auto tableID = listUpdatesPerTable.first;
                opOnListsWithUpdates(adjLists[relDirection].at(tableID).get());
                for (auto& propertyList : propertyLists[relDirection].at(tableID)) {
                    opOnListsWithUpdates(propertyList.get());
                }
            }
        }
    }
    if (adjAndPropertyListsUpdateStore->hasUpdates()) {
        opIfHasUpdates();
    }
}

string RelTable::inferRelMultiplicity(table_id_t srcTableID, table_id_t dstTableID) {
    auto isFWDColumn = adjColumns[RelDirection::FWD].contains(srcTableID);
    auto isBWDColumn = adjColumns[RelDirection::BWD].contains(dstTableID);
    if (isFWDColumn && isBWDColumn) {
        return "ONE_ONE";
    } else if (isFWDColumn && !isBWDColumn) {
        return "MANY_ONE";
    } else if (!isFWDColumn && isBWDColumn) {
        return "ONE_MANY";
    } else {
        return "MANY_MANY";
    }
}

} // namespace storage
} // namespace kuzu
