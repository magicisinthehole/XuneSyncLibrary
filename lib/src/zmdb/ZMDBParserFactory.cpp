#include "ZMDBParserFactory.h"

namespace zmdb {

std::unique_ptr<ZMDBParserBase> ZMDBParserFactory::CreateParser(zune::DeviceFamily family) {
    switch (family) {
        case zune::DeviceFamily::Pavo:
            // Zune HD uses different ZMDB format
            return std::make_unique<ZuneHDParser>();

        case zune::DeviceFamily::Keel:
        case zune::DeviceFamily::Scorpius:
        case zune::DeviceFamily::Draco:
            // All Classic devices (Zune 30, 4/8/16, 80/120) use same parser
            return std::make_unique<ZuneClassicParser>();

        default:
            // Default to Classic parser for unknown types
            return std::make_unique<ZuneClassicParser>();
    }
}

} // namespace zmdb
