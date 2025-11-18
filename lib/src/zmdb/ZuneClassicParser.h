#pragma once

#include "ZMDBParserBase.h"

namespace zmdb {

/**
 * ZMDB parser for Zune Classic devices (Zune 30, etc.).
 *
 * This is a stub implementation. The actual Classic parser will be implemented
 * later based on Zune Classic ZMDB analysis.
 *
 * For now, this returns an empty library.
 */
class ZuneClassicParser : public ZMDBParserBase {
public:
    ZuneClassicParser() = default;
    ~ZuneClassicParser() override = default;

    /**
     * Extract complete library from Zune Classic ZMDB file.
     *
     * @param zmdb_data Raw ZMDB file bytes
     * @return Empty library (stub implementation)
     */
    ZMDBLibrary ExtractLibrary(const std::vector<uint8_t>& zmdb_data) override;
};

} // namespace zmdb
