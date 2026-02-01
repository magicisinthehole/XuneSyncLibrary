#pragma once

#include "ZMDBParserBase.h"
#include "ZuneHDParser.h"
#include "ZuneClassicParser.h"
#include "../ZuneDeviceIdentification.h"
#include <memory>

namespace zmdb {

/**
 * Factory for creating device-specific ZMDB parsers.
 *
 * Routes to appropriate parser implementation based on device family:
 * - Pavo (Zune HD) → ZuneHDParser (fully implemented)
 * - Keel/Scorpius/Draco (Classic devices) → ZuneClassicParser
 */
class ZMDBParserFactory {
public:
    /**
     * Create a parser for the given device family.
     *
     * @param family Device family from MTP property 0xd21a
     * @return Unique pointer to parser instance
     */
    static std::unique_ptr<ZMDBParserBase> CreateParser(zune::DeviceFamily family);
};

} // namespace zmdb
