
#include "src/common/include/vector/node_vector.h"

namespace graphflow {
namespace common {

NodeIDVector::NodeIDVector(NodeIDCompressionScheme scheme) {
    this->compressionScheme = scheme;
    this->buffer = new uint8_t[VECTOR_CAPACITY * getNumBytes(scheme)];
    this->values = this->buffer;
}

NodeIDVector::NodeIDVector(label_t label, NodeIDCompressionScheme scheme) : NodeIDVector(scheme) {
    this->commonLabel = label;
}

NodeIDVector::NodeIDVector(NodeIDCompressionScheme scheme, bool isSequence) : NodeIDVector(scheme) {
    this->isSequence = isSequence;
}

NodeIDVector::NodeIDVector(label_t label, NodeIDCompressionScheme scheme, bool isSequence)
    : NodeIDVector(label, scheme) {
    this->isSequence = isSequence;
}

NodeIDSequenceVector::NodeIDSequenceVector() {
    this->compressionScheme = LABEL_0_NODEOFFSET_8_BYTES;
    this->buffer = new uint8_t[8];
    this->values = this->buffer;
    this->isSequence = true;
}

NodeIDSequenceVector::NodeIDSequenceVector(label_t label) : NodeIDSequenceVector() {
    this->commonLabel = label;
}

// The structs below are used to cast the byte array to the appropriate structure.

// nodeID_t { uint32_t uint64_t }

struct nodeID_32_32_t {
    uint32_t label;
    uint32_t nodeOffset;
};

struct nodeID_32_16_t {
    uint32_t label;
    uint16_t nodeOffset;
};

struct nodeID_16_64_t {
    uint16_t label;
    uint64_t nodeOffset;
};

struct nodeID_16_32_t {
    uint16_t label;
    uint32_t nodeOffset;
};

struct nodeID_16_16_t {
    uint16_t label;
    uint16_t nodeOffset;
};

struct nodeID_8_64_t {
    uint8_t label;
    uint64_t nodeOffset;
};

struct nodeID_8_32_t {
    uint8_t label;
    uint32_t nodeOffset;
};

struct nodeID_8_16_t {
    uint8_t label;
    uint16_t nodeOffset;
};

void NodeIDVector::readValue(uint64_t index, nodeID_t& nodeID) {
    switch (compressionScheme) {
    case LABEL_0_NODEOFFSET_2_BYTES:
        nodeID.offset = ((uint16_t*)values)[index];
        break;
    case LABEL_0_NODEOFFSET_4_BYTES:
        nodeID.offset = ((uint32_t*)values)[index];
        break;
    case LABEL_0_NODEOFFSET_8_BYTES:
        nodeID.offset = ((node_offset_t*)values)[index];
        break;
    case LABEL_1_NODEOFFSET_2_BYTES:
        nodeID.label = ((nodeID_8_16_t*)values)[index].label;
        nodeID.offset = ((nodeID_8_16_t*)values)[index].nodeOffset;
        break;
    case LABEL_1_NODEOFFSET_4_BYTES:
        nodeID.label = ((nodeID_8_32_t*)values)[index].label;
        nodeID.offset = ((nodeID_8_32_t*)values)[index].nodeOffset;
        break;
    case LABEL_1_NODEOFFSET_8_BYTES:
        nodeID.label = ((nodeID_8_64_t*)values)[index].label;
        nodeID.offset = ((nodeID_8_64_t*)values)[index].nodeOffset;
        break;
    case LABEL_2_NODEOFFSET_2_BYTES:
        nodeID.label = ((nodeID_16_16_t*)values)[index].label;
        nodeID.offset = ((nodeID_16_16_t*)values)[index].nodeOffset;
        break;
    case LABEL_2_NODEOFFSET_4_BYTES:
        nodeID.label = ((nodeID_16_32_t*)values)[index].label;
        nodeID.offset = ((nodeID_16_32_t*)values)[index].nodeOffset;
        break;
    case LABEL_2_NODEOFFSET_8_BYTES:
        nodeID.label = ((nodeID_16_64_t*)values)[index].label;
        nodeID.offset = ((nodeID_16_64_t*)values)[index].nodeOffset;
        break;
    case LABEL_4_NODEOFFSET_2_BYTES:
        nodeID.label = ((nodeID_32_16_t*)values)[index].label;
        nodeID.offset = ((nodeID_32_16_t*)values)[index].nodeOffset;
        break;
    case LABEL_4_NODEOFFSET_4_BYTES:
        nodeID.label = ((nodeID_32_32_t*)values)[index].label;
        nodeID.offset = ((nodeID_32_32_t*)values)[index].nodeOffset;
        break;
    case LABEL_4_NODEOFFSET_8_BYTES:
        nodeID.label = ((nodeID_t*)values)[index].label;
        nodeID.offset = ((nodeID_t*)values)[index].offset;
        break;
    default:
        throw invalid_argument("Unsupported compression scheme.");
    }
}

} // namespace common
} // namespace graphflow
