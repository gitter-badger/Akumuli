#include "compression.h"
#include "perftest_tools.h"

#include <iostream>
#include <cstdlib>
#include <algorithm>

using namespace Akumuli;

int main() {
    const uint64_t N_TIMESTAMPS = 100;
    const uint64_t N_PARAMS = 100;
    UncompressedChunk header;
    std::cout << "Testing timestamp sequence" << std::endl;
    int c = 100;
    std::vector<aku_ParamId> ids;
    for (uint64_t id = 0; id < N_PARAMS; id++) { ids.push_back(id); }
    //std::random_shuffle(ids.begin(), ids.end());
    for (uint64_t id = 0; id < N_PARAMS; id++) {
        for (uint64_t ts = 0; ts < N_TIMESTAMPS; ts++) {
            header.paramids.push_back(ids[id]);
            int k = rand() % 2;
            if (k) {
                c++;
            } else if (c > 0) {
                c--;
            }
            header.timestamps.push_back(ts + c);
            ChunkValue cvalue;
            cvalue.type = ChunkValue::FLOAT;
            cvalue.value.floatval = id + ts;
            header.values.push_back(cvalue);
        }
    }

    ByteVector out;
    out.resize(N_PARAMS*N_TIMESTAMPS*24);

    const size_t UNCOMPRESSED_SIZE = header.paramids.size()*8    // Didn't count lengths and offsets
                                   + header.timestamps.size()*8  // because because this arrays contains
                                   + header.values.size()*8;     // no information and should be compressed
                                                                 // to a few bytes

    struct Writer : ChunkWriter {
        ByteVector *out;
        Writer(ByteVector *out) : out(out) {}

        virtual aku_MemRange allocate() {
            aku_MemRange range = {
                out->data(),
                static_cast<uint32_t>(out->size())
            };
            return range;
        }

        //! Commit changes
        virtual aku_Status commit(size_t bytes_written) {
            return AKU_SUCCESS;
        }
    } writer(&out);

    aku_Timestamp tsbegin, tsend;
    uint32_t n;
    auto status = CompressionUtil::encode_chunk(&n, &tsbegin, &tsend, &writer, header);
    if (status != AKU_SUCCESS) {
        std::cout << "Encoding error" << std::endl;
        return 1;
    }

    const size_t COMPRESSED_SIZE = out.size();

    std::cout << "Uncompressed: " << UNCOMPRESSED_SIZE
              << ", compressed: " << COMPRESSED_SIZE
              << std::endl;

    // Try to decompress
    UncompressedChunk decomp;
    const unsigned char* pbegin = out.data();
    const unsigned char* pend = pbegin + out.size();
    CompressionUtil::decode_chunk(&decomp, pbegin, pend, header.timestamps.size());

    for (auto i = 0u; i < header.timestamps.size(); i++) {
        if (header.timestamps.at(i) != decomp.timestamps.at(i)) {
            std::cout << "Error, bad timestamp at " << i << std::endl;
        }
        if (header.paramids.at(i) != decomp.paramids.at(i)) {
            std::cout << "Error, bad paramid at " << i << std::endl;
        }
        ChunkValue origvalue = header.values.at(i);
        ChunkValue decvalue = decomp.values.at(i);
        if (origvalue.type != decvalue.type) {
            std::cout << "Error, bad type at " << i << std::endl;
        } else {
            if (origvalue.type == ChunkValue::FLOAT) {
                if (origvalue.value.floatval != decvalue.value.floatval) {
                    std::cout << "Error, bad value at " << i << std::endl;
                }
            } else {
                if (origvalue.value.blobval.length != decvalue.value.blobval.length) {
                    std::cout << "Error, bad length at " << i << std::endl;
                }
                if (origvalue.value.blobval.offset != decvalue.value.blobval.offset) {
                    std::cout << "Error, bad offset at " << i << std::endl;
                }
            }
        }
    }
}
