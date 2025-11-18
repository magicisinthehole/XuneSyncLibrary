#include "ZMDBParserFactory.h"

namespace zmdb {

std::unique_ptr<ZMDBParserBase> ZMDBParserFactory::CreateParser(DeviceType type) {
    switch (type) {
        case DeviceType::ZuneHD:
            return std::make_unique<ZuneHDParser>();

        case DeviceType::Zune30:
            return std::make_unique<ZuneClassicParser>();

        default:
            // Default to Classic parser for unknown types
            return std::make_unique<ZuneClassicParser>();
    }
}

} // namespace zmdb
