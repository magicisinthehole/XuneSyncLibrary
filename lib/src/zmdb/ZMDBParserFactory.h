#pragma once

#include "ZMDBParserBase.h"
#include "ZuneHDParser.h"
#include "ZuneClassicParser.h"
#include <memory>

namespace zmdb {

/**
 * Factory for creating device-specific ZMDB parsers.
 *
 * Routes to appropriate parser implementation based on device type:
 * - ZuneHD → ZuneHDParser (fully implemented)
 * - Zune30/Classic → ZuneClassicParser (stubbed for now)
 */
class ZMDBParserFactory {
public:
    /**
     * Create a parser for the given device type.
     *
     * @param type Device type (ZuneHD or Zune30)
     * @return Unique pointer to parser instance
     */
    static std::unique_ptr<ZMDBParserBase> CreateParser(DeviceType type);
};

} // namespace zmdb
