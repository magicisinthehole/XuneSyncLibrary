#include "ZuneUploadPrimitives.h"
#include <cli/PosixStreams.h>
#include <mtp/ptp/ObjectFormat.h>

namespace zune {

// ── Property List Writing Helpers ────────────────────────────────────────

void UploadPrimitives::WritePropString(mtp::OutputStream& os, uint16_t prop, const std::string& value) {
    os.Write32(0); os.Write16(prop); os.Write16(MtpType::String); os.WriteString(value);
}

void UploadPrimitives::WritePropU8(mtp::OutputStream& os, uint16_t prop, uint8_t value) {
    os.Write32(0); os.Write16(prop); os.Write16(MtpType::Uint8); os.Write8(value);
}

void UploadPrimitives::WritePropU16(mtp::OutputStream& os, uint16_t prop, uint16_t value) {
    os.Write32(0); os.Write16(prop); os.Write16(MtpType::Uint16); os.Write16(value);
}

void UploadPrimitives::WritePropU32(mtp::OutputStream& os, uint16_t prop, uint32_t value) {
    os.Write32(0); os.Write16(prop); os.Write16(MtpType::Uint32); os.Write32(value);
}

void UploadPrimitives::WritePropU128(mtp::OutputStream& os, uint16_t prop, const uint8_t* bytes, size_t len) {
    os.Write32(0); os.Write16(prop); os.Write16(MtpType::Uint128);
    for (size_t i = 0; i < len && i < 16; ++i) os.Write8(bytes[i]);
    for (size_t i = len; i < 16; ++i) os.Write8(0);
}

const uint16_t* UploadPrimitives::GetBatchFormats(bool isHD) {
    return isHD ? HD_BATCH_FORMATS : CLASSIC_BATCH_FORMATS;
}

size_t UploadPrimitives::GetBatchFormatCount(bool isHD) {
    return isHD ? HD_BATCH_FORMAT_COUNT : CLASSIC_BATCH_FORMAT_COUNT;
}

// ── Pre-Upload Operations ────────────────────────────────────────────────

void UploadPrimitives::QueryStorageInfo(const SessionPtr& session, uint32_t storageId) {
    // Pcap: GetStorageInfo (non-fresh devices only)
    try { session->GetStorageInfo(mtp::StorageId(storageId)); } catch (...) {}
}

RootDiscoveryResult UploadPrimitives::DiscoverRoot(const SessionPtr& session, uint32_t storageId) {
    RootDiscoveryResult result;
    result.storage_id = storageId;

    // Pcap: GetObjectHandles root
    auto handles = session->GetObjectHandles(
        mtp::StorageId(storageId), mtp::ObjectFormat::Any, mtp::Session::Root);
    result.root_object_count = static_cast<int>(handles.ObjectHandles.size());

    // Pcap: Root re-enum with depth=1 (returns names for root AND children)
    RootReEnum(session);

    // Parse root folder names from ObjFileName depth=1 response
    if (!handles.ObjectHandles.empty()) {
        try {
            auto name_data = session->GetObjectPropertyList(
                mtp::Session::Device, mtp::ObjectFormat(0),
                mtp::ObjectProperty(MtpProp::ObjectFileName), 0, 1);

            if (name_data.size() >= 4) {
                uint32_t n = *reinterpret_cast<const uint32_t*>(name_data.data());
                size_t off = 4;
                for (uint32_t i = 0; i < n && off + 8 <= name_data.size(); ++i) {
                    uint32_t handle = *reinterpret_cast<const uint32_t*>(name_data.data() + off);
                    off += 4 + 2 + 2; // handle + prop + type
                    std::string name;
                    if (off < name_data.size()) {
                        uint8_t nchars = name_data[off++];
                        if (nchars > 0 && off + nchars * 2 <= name_data.size()) {
                            for (uint8_t c = 0; c < nchars; ++c) {
                                uint16_t ch = *reinterpret_cast<const uint16_t*>(name_data.data() + off + c * 2);
                                if (ch == 0) break;
                                if (ch < 128) name += static_cast<char>(ch);
                            }
                            off += nchars * 2;
                        }
                    }
                    if (name == "Music") result.music_folder = handle;
                    else if (name == "Albums") result.albums_folder = handle;
                    else if (name == "Artists") result.artists_folder = handle;
                }
            }
        } catch (...) {}
    }

    return result;
}

void UploadPrimitives::RootReEnum(const SessionPtr& session) {
    // Pcap: GetObjPropList(Device, prop=StorageID, depth=1) + GetObjPropList(Device, prop=ObjFileName, depth=1)
    // depth=1 is critical — includes immediate children of root
    try {
        session->GetObjectPropertyList(
            mtp::Session::Device, mtp::ObjectFormat(0),
            mtp::ObjectProperty(MtpProp::StorageID), 0, 1);
    } catch (...) {}
    try {
        session->GetObjectPropertyList(
            mtp::Session::Device, mtp::ObjectFormat(0),
            mtp::ObjectProperty(MtpProp::ObjectFileName), 0, 1);
    } catch (...) {}
}

// ── Folder Discovery & Creation ──────────────────────────────────────────

std::vector<FolderChild> UploadPrimitives::DiscoverFolderChildren(
    const SessionPtr& session, uint32_t storageId, uint32_t folderId)
{
    std::vector<FolderChild> children;
    auto folderObj = mtp::ObjectId(folderId);
    auto storageObj = mtp::StorageId(storageId);

    // Pcap 3-op pattern:
    // 1. GetObjPropList grp=2 (folder subset properties)
    try { session->GetObjectPropertyList(
        folderObj, mtp::ObjectFormat(0), mtp::ObjectProperty(0), 2, 0); } catch (...) {}

    // 2. GetObjectHandles (enumerate children)
    std::vector<mtp::ObjectId> handles;
    try {
        auto result = session->GetObjectHandles(storageObj, mtp::ObjectFormat::Any, folderObj);
        handles = result.ObjectHandles;
    } catch (...) {}

    // 3. GetObjPropList prop=ObjFileName depth=1 (folder + children names)
    try {
        auto data = session->GetObjectPropertyList(
            folderObj, mtp::ObjectFormat(0),
            mtp::ObjectProperty(MtpProp::ObjectFileName), 0, 1);

        if (data.size() >= 4) {
            uint32_t n = *reinterpret_cast<const uint32_t*>(data.data());
            size_t off = 4;
            for (uint32_t i = 0; i < n && off + 8 <= data.size(); ++i) {
                uint32_t handle = *reinterpret_cast<const uint32_t*>(data.data() + off);
                off += 4 + 2 + 2;
                std::string name;
                if (off < data.size()) {
                    uint8_t nchars = data[off++];
                    if (nchars > 0 && off + nchars * 2 <= data.size()) {
                        for (uint8_t c = 0; c < nchars; ++c) {
                            uint16_t ch = *reinterpret_cast<const uint16_t*>(data.data() + off + c * 2);
                            if (ch == 0) break;
                            if (ch < 128) name += static_cast<char>(ch);
                        }
                        off += nchars * 2;
                    }
                }
                // First entry is the folder itself; rest are children
                if (handle != folderId && !name.empty()) {
                    children.push_back({name, handle});
                }
            }
        }
    } catch (...) {}

    return children;
}

uint32_t UploadPrimitives::CreateFolder(
    const SessionPtr& session, uint32_t storageId, uint32_t parentId,
    const std::string& name)
{
    mtp::ByteArray propList;
    mtp::OutputStream os(propList);
    os.Write32(1);
    WritePropString(os, MtpProp::ObjectFileName, name);

    auto resp = session->SendObjectPropList(
        mtp::StorageId(storageId),
        mtp::ObjectId(parentId),
        mtp::ObjectFormat::Association, 0, propList);
    return resp.ObjectId.Id;
}

void UploadPrimitives::FolderReadback(
    const SessionPtr& session, uint32_t folderId, uint32_t storageId)
{
    auto obj = mtp::ObjectId(folderId);
    // PersistentUID read
    try { session->GetObjectPropertyList(
        obj, mtp::ObjectFormat(0),
        mtp::ObjectProperty(MtpProp::PersistentUID), 0, 0); } catch (...) {}
    // ParentObject + StorageID (group=4)
    try { session->GetObjectPropertyList(
        obj, mtp::ObjectFormat(0),
        mtp::ObjectProperty(0), 4, 0); } catch (...) {}
    // GetObjectHandles children
    try { session->GetObjectHandles(
        mtp::StorageId(storageId), mtp::ObjectFormat::Any, obj); } catch (...) {}
}

void UploadPrimitives::FirstFolderReadback(
    const SessionPtr& session, uint32_t folderId, uint32_t storageId, bool isHD)
{
    auto obj = mtp::ObjectId(folderId);
    // Pcap: PersistentUID batch → PersistentUID read → StorageID batch → grp=4 read → GetObjectHandles
    QueryBatchDescriptors(session, MtpProp::PersistentUID, isHD);
    try { session->GetObjectPropertyList(
        obj, mtp::ObjectFormat(0),
        mtp::ObjectProperty(MtpProp::PersistentUID), 0, 0); } catch (...) {}
    QueryBatchDescriptors(session, MtpProp::StorageID, isHD);
    try { session->GetObjectPropertyList(
        obj, mtp::ObjectFormat(0),
        mtp::ObjectProperty(0), 4, 0); } catch (...) {}
    try { session->GetObjectHandles(
        mtp::StorageId(storageId), mtp::ObjectFormat::Any, obj); } catch (...) {}
}

// ── Artist Metadata (HD Only) ────────────────────────────────────────────

uint32_t UploadPrimitives::CreateArtistMetadata(
    const SessionPtr& session, uint32_t storageId, uint32_t artistsFolderId,
    const std::string& name, const uint8_t* guidBytes, size_t guidLen)
{
    bool hasGuid = (guidBytes != nullptr && guidLen >= 16);
    uint32_t propCount = hasGuid ? 4 : 3;

    mtp::ByteArray propList;
    mtp::OutputStream os(propList);
    os.Write32(propCount);
    WritePropU8(os, MtpProp::ZuneCollectionId, 0);
    WritePropString(os, MtpProp::ObjectFileName, name + ".art");
    if (hasGuid) WritePropU128(os, MtpProp::DA97, guidBytes, guidLen);
    WritePropString(os, MtpProp::Name, name);

    auto resp = session->SendObjectPropList(
        mtp::StorageId(storageId),
        mtp::ObjectId(artistsFolderId),
        mtp::ObjectFormat(MtpFmt::ArtistMeta), 0, propList);

    // Empty SendObject (required for metadata objects)
    mtp::ByteArray empty;
    session->SendObject(std::make_shared<mtp::ByteArrayObjectInputStream>(empty));

    // Verification read
    try { session->GetObjectPropertyList(
        resp.ObjectId, mtp::ObjectFormat(0),
        mtp::ObjectProperty(0xFFFFFFFF), 0, 0); } catch (...) {}

    return resp.ObjectId.Id;
}

// ── Track Operations ─────────────────────────────────────────────────────

uint32_t UploadPrimitives::CreateTrack(
    const SessionPtr& session, uint32_t storageId, uint32_t albumFolderId,
    const TrackProperties& props, uint16_t formatCode, uint64_t fileSize)
{
    bool hasRating = (props.rating >= 0);
    // Base: 13 (MetaGenre through Genre, minus DAB8/DAB9/Rating)
    // HD adds: DAB8+DAB9 = +2
    // Rating: +1 when present
    uint32_t propCount = 13 + (props.is_hd ? 2 : 0) + (hasRating ? 1 : 0);

    mtp::ByteArray propList;
    mtp::OutputStream os(propList);
    os.Write32(propCount);

    // Exact pcap order
    WritePropU16(os, MtpProp::MetaGenre, 1);
    WritePropString(os, MtpProp::ObjectFileName, props.filename);
    WritePropString(os, MtpProp::AlbumName, props.album_name);
    WritePropString(os, MtpProp::Name, props.title);
    WritePropString(os, MtpProp::AlbumArtist, props.album_artist);
    WritePropU16(os, MtpProp::DC9D, 0);  // Always 0 on Zune
    WritePropU8(os, MtpProp::ZuneCollectionId, 0);
    WritePropString(os, MtpProp::Artist, props.artist);
    WritePropString(os, MtpProp::DateAuthored, props.date_authored);
    WritePropU8(os, MtpProp::ZunePropDAB2, 0);
    if (props.is_hd) {
        WritePropU32(os, MtpProp::DiscNumber, props.disc_number > 0 ? props.disc_number : 1);
        WritePropU32(os, MtpProp::ArtistId, props.artist_meta_id);
    }
    WritePropU32(os, MtpProp::Duration, props.duration_ms);
    if (hasRating)
        WritePropU16(os, MtpProp::Rating, static_cast<uint16_t>(props.rating));
    WritePropU16(os, MtpProp::Track, props.track_number);
    WritePropString(os, MtpProp::Genre, props.genre.empty() ? "Unknown" : props.genre);

    auto resp = session->SendObjectPropList(
        mtp::StorageId(storageId),
        mtp::ObjectId(albumFolderId),
        static_cast<mtp::ObjectFormat>(formatCode),
        fileSize, propList);

    return resp.ObjectId.Id;
}

void UploadPrimitives::UploadAudioData(const SessionPtr& session, const std::string& filePath) {
    auto stream = std::make_shared<cli::ObjectInputStream>(filePath);
    stream->SetTotal(stream->GetSize());
    session->SendObject(stream);
}

void UploadPrimitives::VerifyTrack(const SessionPtr& session, uint32_t trackId) {
    try { session->GetObjectPropertyList(
        mtp::ObjectId(trackId), mtp::ObjectFormat(0),
        mtp::ObjectProperty(0xFFFFFFFF), 0, 0); } catch (...) {}
}

// ── Album Metadata Operations ────────────────────────────────────────────

uint32_t UploadPrimitives::CreateAlbumMetadata(
    const SessionPtr& session, uint32_t storageId, uint32_t albumsFolderId,
    const AlbumProperties& props)
{
    mtp::ByteArray propList;
    mtp::OutputStream os(propList);

    if (props.is_hd) {
        os.Write32(6);
        WritePropString(os, MtpProp::Artist, props.artist);
        WritePropString(os, MtpProp::DateAuthored, props.date_authored);
        WritePropU8(os, MtpProp::ZuneCollectionId, 0);
        WritePropString(os, MtpProp::ObjectFileName,
            props.artist + "--" + props.album_name + ".alb");
        WritePropU32(os, MtpProp::ArtistId, props.artist_meta_id);
        WritePropString(os, MtpProp::Name, props.album_name);
    } else {
        os.Write32(4);
        WritePropString(os, MtpProp::Artist, props.artist);
        WritePropU8(os, MtpProp::ZuneCollectionId, 0);
        WritePropString(os, MtpProp::ObjectFileName,
            props.artist + "--" + props.album_name + ".alb");
        WritePropString(os, MtpProp::Name, props.album_name);
    }

    auto resp = session->SendObjectPropList(
        mtp::StorageId(storageId),
        mtp::ObjectId(albumsFolderId),
        mtp::ObjectFormat(MtpFmt::AbstractAlbum), 0, propList);

    // Empty SendObject (required for metadata objects)
    mtp::ByteArray empty;
    session->SendObject(std::make_shared<mtp::ByteArrayObjectInputStream>(empty));

    // Verification read
    try { session->GetObjectPropertyList(
        resp.ObjectId, mtp::ObjectFormat(0),
        mtp::ObjectProperty(0xFFFFFFFF), 0, 0); } catch (...) {}

    return resp.ObjectId.Id;
}

void UploadPrimitives::SetAlbumArtwork(
    const SessionPtr& session, uint32_t albumObjId,
    const uint8_t* data, size_t size)
{
    auto obj = mtp::ObjectId(albumObjId);

    // Read current artwork value
    try { session->GetObjectProperty(obj, mtp::ObjectProperty(MtpProp::RepSampleData)); } catch (...) {}

    // Set artwork
    mtp::ByteArray artData(data, data + size);
    session->SetObjectPropertyAsArray(obj, mtp::ObjectProperty(MtpProp::RepSampleData), artData);

    // Set format to JPEG
    mtp::ByteArray fmtVal;
    mtp::OutputStream fmtOs(fmtVal);
    fmtOs.Write16(MtpFmt::JPEG);
    session->SetObjectProperty(obj, mtp::ObjectProperty(MtpProp::RepSampleFormat), fmtVal);
}

void UploadPrimitives::ReadAlbumArtworkCurrent(const SessionPtr& session, uint32_t albumObjId) {
    try { session->GetObjectProperty(
        mtp::ObjectId(albumObjId), mtp::ObjectProperty(MtpProp::RepSampleData)); } catch (...) {}
}

void UploadPrimitives::SetAlbumReferences(
    const SessionPtr& session, uint32_t albumObjId,
    const uint32_t* trackIds, size_t count)
{
    mtp::msg::ObjectHandles refs;
    for (size_t i = 0; i < count; ++i)
        refs.ObjectHandles.push_back(mtp::ObjectId(trackIds[i]));
    session->SetObjectReferences(mtp::ObjectId(albumObjId), refs);
}

void UploadPrimitives::VerifyAlbum(
    const SessionPtr& session, uint32_t albumObjId, bool includeParentDesc)
{
    auto obj = mtp::ObjectId(albumObjId);
    // Subset read (grp=2)
    try { session->GetObjectPropertyList(
        obj, mtp::ObjectFormat(0), mtp::ObjectProperty(0), 2, 0); } catch (...) {}
    // ParentObject descriptor (first album only)
    if (includeParentDesc) {
        try { session->GetObjectPropertyDesc(
            mtp::ObjectProperty(MtpProp::ParentObject),
            mtp::ObjectFormat(MtpFmt::AbstractAlbum)); } catch (...) {}
    }
    // ALL read
    try { session->GetObjectPropertyList(
        obj, mtp::ObjectFormat(0),
        mtp::ObjectProperty(0xFFFFFFFF), 0, 0); } catch (...) {}
}

// ── Finalization ─────────────────────────────────────────────────────────

void UploadPrimitives::RegisterTrackContext(const SessionPtr& session, const std::string& trackName) {
    session->Operation922a(trackName);
}

// ── Property Descriptor Queries ──────────────────────────────────────────

void UploadPrimitives::QueryFolderDescriptors(const SessionPtr& session) {
    try { session->GetObjectPropertiesSupported(mtp::ObjectFormat(MtpFmt::Folder)); } catch (...) {}
    try { session->GetObjectPropertyDesc(
        mtp::ObjectProperty(MtpProp::ObjectFileName),
        mtp::ObjectFormat(MtpFmt::Folder)); } catch (...) {}
}

void UploadPrimitives::QueryBatchDescriptors(
    const SessionPtr& session, uint16_t propCode, bool isHD)
{
    const uint16_t* formats = GetBatchFormats(isHD);
    size_t count = GetBatchFormatCount(isHD);
    for (size_t i = 0; i < count; ++i) {
        try { session->GetObjectPropertyDesc(
            mtp::ObjectProperty(propCode),
            mtp::ObjectFormat(formats[i])); } catch (...) {}
    }
}

void UploadPrimitives::QueryTrackDescriptors(
    const SessionPtr& session, uint16_t formatCode, bool isHD)
{
    // Exact order from pcap
    const uint16_t classic_props[] = {
        MtpProp::MetaGenre, MtpProp::ObjectFileName, MtpProp::AlbumName,
        MtpProp::Name, MtpProp::AlbumArtist, MtpProp::DC9D,
        MtpProp::ZuneCollectionId, MtpProp::Artist, MtpProp::DateAuthored,
        MtpProp::ZunePropDAB2, MtpProp::Duration, MtpProp::Rating,
        MtpProp::Track, MtpProp::Genre,
    };
    const uint16_t hd_props[] = {
        MtpProp::MetaGenre, MtpProp::ObjectFileName, MtpProp::AlbumName,
        MtpProp::Name, MtpProp::AlbumArtist, MtpProp::DC9D,
        MtpProp::ZuneCollectionId, MtpProp::Artist, MtpProp::DateAuthored,
        MtpProp::ZunePropDAB2, MtpProp::DiscNumber, MtpProp::ArtistId,
        MtpProp::Duration, MtpProp::Rating, MtpProp::Track, MtpProp::Genre,
    };
    const uint16_t* props = isHD ? hd_props : classic_props;
    size_t count = isHD ? 16 : 14;
    auto fmt = mtp::ObjectFormat(formatCode);
    for (size_t i = 0; i < count; ++i) {
        try { session->GetObjectPropertyDesc(mtp::ObjectProperty(props[i]), fmt); } catch (...) {}
    }
}

void UploadPrimitives::QueryAlbumDescriptors(const SessionPtr& session, bool isHD) {
    auto fmt = mtp::ObjectFormat(MtpFmt::AbstractAlbum);
    if (isHD) {
        const uint16_t props[] = {
            MtpProp::Artist, MtpProp::DateAuthored, MtpProp::ZuneCollectionId,
            MtpProp::ObjectFileName, MtpProp::ArtistId, MtpProp::Name,
        };
        for (auto p : props) {
            try { session->GetObjectPropertyDesc(mtp::ObjectProperty(p), fmt); } catch (...) {}
        }
    } else {
        const uint16_t props[] = {
            MtpProp::Artist, MtpProp::ZuneCollectionId,
            MtpProp::ObjectFileName, MtpProp::Name,
        };
        for (auto p : props) {
            try { session->GetObjectPropertyDesc(mtp::ObjectProperty(p), fmt); } catch (...) {}
        }
    }
}

void UploadPrimitives::QueryArtistDescriptors(const SessionPtr& session) {
    auto fmt = mtp::ObjectFormat(MtpFmt::ArtistMeta);
    const uint16_t props[] = {
        MtpProp::ZuneCollectionId, MtpProp::ObjectFileName,
        MtpProp::DA97, MtpProp::Name,
    };
    for (auto p : props) {
        try { session->GetObjectPropertyDesc(mtp::ObjectProperty(p), fmt); } catch (...) {}
    }
}

void UploadPrimitives::QueryArtworkDescriptors(const SessionPtr& session) {
    auto fmt = mtp::ObjectFormat(MtpFmt::AbstractAlbum);
    try { session->GetObjectPropertyDesc(
        mtp::ObjectProperty(MtpProp::RepSampleData), fmt); } catch (...) {}
    try { session->GetObjectPropertyDesc(
        mtp::ObjectProperty(MtpProp::RepSampleFormat), fmt); } catch (...) {}
}

} // namespace zune
