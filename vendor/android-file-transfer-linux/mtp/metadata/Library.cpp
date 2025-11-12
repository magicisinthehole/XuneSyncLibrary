#include <mtp/metadata/Library.h>
#include <mtp/ptp/Session.h>
#include <mtp/ptp/ObjectPropertyListParser.h>
#include <mtp/ptp/ByteArrayObjectStream.h>
#include <mtp/log.h>
#include <algorithm>
#include <unordered_map>
#include <iomanip>
#include <iostream>

namespace mtp
{
	namespace
	{
		const std::string UknownArtist		("UknownArtist");
		const std::string UknownAlbum		("UknownAlbum");
		const std::string VariousArtists	("VariousArtists");
	}

	Library::NameToObjectIdMap Library::ListAssociations(ObjectId parentId)
	{
		NameToObjectIdMap list;

		ByteArray data = _session->GetObjectPropertyList(parentId, ObjectFormat::Association, ObjectProperty::ObjectFilename, 0, 1);
		ObjectPropertyListParser<std::string> parser;
		parser.Parse(data, [&](ObjectId id, ObjectProperty property, const std::string &name) {
			list.insert(std::make_pair(name, id));
		});
		return list;
	}

	ObjectId Library::GetOrCreate(ObjectId parentId, const std::string &name)
	{
		auto objects = _session->GetObjectHandles(_storage, mtp::ObjectFormat::Association, parentId);
		for (auto id : objects.ObjectHandles)
		{
			auto oname = _session->GetObjectStringProperty(id, ObjectProperty::ObjectFilename);
			if (name == oname)
				return id;
		}
		return _session->CreateDirectory(name, parentId, _storage).ObjectId;
	}

	Library::Library(const mtp::SessionPtr & session, ProgressReporter && reporter): _session(session)
	{
		auto storages = _session->GetStorageIDs();
		if (storages.StorageIDs.empty())
			throw std::runtime_error("no storages found");

		u64 progress = 0, total = 0;
		if (reporter)
			reporter(State::Initialising, progress, total);

		_artistSupported = _session->GetDeviceInfo().Supports(ObjectFormat::Artist);
		debug("device supports ObjectFormat::Artist: ", _artistSupported? "yes": "no");
		{
			auto propsSupported = _session->GetObjectPropertiesSupported(ObjectFormat::AbstractAudioAlbum);
			_albumDateAuthoredSupported = propsSupported.Supports(ObjectProperty::DateAuthored);
			_albumCoverSupported = propsSupported.Supports(ObjectProperty::RepresentativeSampleData);
			mtp::debug("abstract album supports date authored: ", _albumDateAuthoredSupported, ", cover: ", _albumCoverSupported);
		}

		_storage = storages.StorageIDs[0]; //picking up first storage.
		//zune fails to create artist/album without storage id
		{
			ByteArray data = _session->GetObjectPropertyList(Session::Root, ObjectFormat::Association, ObjectProperty::ObjectFilename, 0, 1);
			ObjectStringPropertyListParser::Parse(data, [&](ObjectId id, ObjectProperty property, const std::string &name)
			{
				if (name == "Artists")
					_artistsFolder = id;
				else if (name == "Albums")
					_albumsFolder = id;
				else if (name == "Music")
					_musicFolder = id;
			});
		}
		if (_artistSupported && _artistsFolder == ObjectId())
			_artistsFolder = _session->CreateDirectory("Artists", Session::Root, _storage).ObjectId;
		if (_albumsFolder == ObjectId())
			_albumsFolder = _session->CreateDirectory("Albums", Session::Root, _storage).ObjectId;
		if (_musicFolder == ObjectId())
			_musicFolder = _session->CreateDirectory("Music", Session::Root, _storage).ObjectId;

		debug("artists folder: ", _artistsFolder != ObjectId()? _artistsFolder.Id: 0);
		debug("albums folder: ", _albumsFolder.Id);
		debug("music folder: ", _musicFolder.Id);

		auto musicFolders = ListAssociations(_musicFolder);

		using namespace mtp;

		ByteArray artists, albums;
		if (_artistSupported)
		{
			debug("getting artists...");
			if (reporter)
				reporter(State::QueryingArtists, progress, total);

			artists = _session->GetObjectPropertyList(Session::Root, mtp::ObjectFormat::Artist, mtp::ObjectProperty::Name, 0, 1);
			HexDump("artists", artists);

			total += ObjectStringPropertyListParser::GetSize(artists);
		}
		{
			debug("getting albums...");
			if (reporter)
				reporter(State::QueryingAlbums, progress, total);

			albums = _session->GetObjectPropertyList(Session::Root, mtp::ObjectFormat::AbstractAudioAlbum,  mtp::ObjectProperty::Name, 0, 1);
			HexDump("albums", artists);

			total += ObjectStringPropertyListParser::GetSize(albums);
		}

		if (_artistSupported)
		{
			if (reporter)
				reporter(State::LoadingArtists, progress, total);

			ObjectStringPropertyListParser::Parse(artists, [&](ObjectId id, ObjectProperty property, const std::string &name)
			{
				debug("artist: ", name, "\t", id.Id);
				auto artist = std::make_shared<Artist>();
				artist->Id = id;
				artist->Name = name;
				auto it = musicFolders.find(name);
				if (it != musicFolders.end())
					artist->MusicFolderId = it->second;
				else
					artist->MusicFolderId = _session->CreateDirectory(name, _musicFolder, _storage).ObjectId;

				_artists.insert(std::make_pair(name, artist));
				if (reporter)
					reporter(State::LoadingArtists, ++progress, total);
			});
		}

		if (reporter)
			reporter(State::LoadingAlbums, progress, total);

		std::unordered_map<ArtistPtr, NameToObjectIdMap> albumFolders;
		ObjectStringPropertyListParser::Parse(albums, [&](ObjectId id, ObjectProperty property, const std::string &name)
		{
			auto artistName = _session->GetObjectStringProperty(id, ObjectProperty::Artist);

			std::string albumDate;
			if (_albumDateAuthoredSupported)
				albumDate = _session->GetObjectStringProperty(id, ObjectProperty::DateAuthored);

			auto artist = GetArtist(artistName);
			if (!artist)
				artist = CreateArtist(artistName);

			debug("album: ", artistName, " -- ", name, "\t", id.Id, "\t", albumDate);
			auto album = std::make_shared<Album>();
			album->Name = name;
			album->Artist = artist;
			album->Id = id;
			album->Year = !albumDate.empty()? ConvertDateTime(albumDate): 0;
			if (albumFolders.find(artist) == albumFolders.end()) {
				albumFolders[artist] = ListAssociations(artist->MusicFolderId);
			}
			auto it = albumFolders.find(artist);
			if (it == albumFolders.end())
				throw std::runtime_error("no iterator after insert, internal error");

			const auto & albums = it->second;
			auto alit = albums.find(name);
			if (alit != albums.end())
				album->MusicFolderId = alit->second;
			else
				album->MusicFolderId = _session->CreateDirectory(name, artist->MusicFolderId, _storage).ObjectId;

			_albums.insert(std::make_pair(std::make_pair(artist, name), album));
			if (reporter)
				reporter(State::LoadingAlbums, ++progress, total);
		});

		if (reporter)
			reporter(State::Loaded, progress, total);
	}

	Library::~Library()
	{ }

	Library::ArtistPtr Library::GetArtist(std::string name)
	{
		if (name.empty())
			name = UknownArtist;

		auto it = _artists.find(name);
		return it != _artists.end()? it->second: ArtistPtr();
	}


	Library::ArtistPtr Library::CreateArtist(std::string name, const std::string& guid)
	{
		if (name.empty())
			name = UknownArtist;

		auto artist = std::make_shared<Artist>();
		artist->Name = name;
		artist->MusicFolderId = GetOrCreate(_musicFolder, name);

		// Convert GUID string to 16-byte binary if provided (needed for albums regardless of artist support)
		if (!guid.empty()) {
			// Parse GUID string (format: "45a663b5-b1cb-4a91-bff6-2bef7bbfdd76")
			// GUIDs use mixed endianness: first 3 components little-endian, last 8 bytes big-endian
			std::string hex = guid;
			// Remove dashes
			hex.erase(std::remove(hex.begin(), hex.end(), '-'), hex.end());

			if (hex.length() == 32) {
				artist->Guid.resize(16);

				// Component 1: 4 bytes (32-bit) - little-endian
				for (int i = 3; i >= 0; --i) {
					artist->Guid[3 - i] = static_cast<u8>(std::stoul(hex.substr(i * 2, 2), nullptr, 16));
				}

				// Component 2: 2 bytes (16-bit) - little-endian
				for (int i = 1; i >= 0; --i) {
					artist->Guid[4 + (1 - i)] = static_cast<u8>(std::stoul(hex.substr(8 + i * 2, 2), nullptr, 16));
				}

				// Component 3: 2 bytes (16-bit) - little-endian
				for (int i = 1; i >= 0; --i) {
					artist->Guid[6 + (1 - i)] = static_cast<u8>(std::stoul(hex.substr(12 + i * 2, 2), nullptr, 16));
				}

				// Component 4: 8 bytes - big-endian (as-is)
				for (size_t i = 0; i < 8; ++i) {
					artist->Guid[8 + i] = static_cast<u8>(std::stoul(hex.substr(16 + i * 2, 2), nullptr, 16));
				}
			}
		}

		if (_artistSupported)
		{
			ByteArray propList;
			OutputStream os(propList);

			// When GUID is provided, create metadata artist object (0xB218) with GUID
			// This is what Windows does - single artist object with metadata
			if (!artist->Guid.empty())
			{
				std::cout << "  Creating metadata artist object (0xB218) with GUID for: " << name << std::endl;

				os.Write32(4); // 4 properties (matches Windows)

				// Property 1: 0xDAB0 (Zune_CollectionID) = 0 (Uint8)
				os.Write32(0); // object handle
				os.Write16(0xDAB0);
				os.Write16(static_cast<u16>(DataTypeCode::Uint8));
				os.Write8(0);

				// Property 2: ObjectFilename (0xDC07)
				os.Write32(0); // object handle
				os.Write16(static_cast<u16>(ObjectProperty::ObjectFilename));
				os.Write16(static_cast<u16>(DataTypeCode::String));
				os.WriteString(name + ".art");

				// Property 3: Zune GUID (0xDA97) - Uint128 (16 bytes)
				os.Write32(0); // object handle
				os.Write16(0xDA97);
				os.Write16(static_cast<u16>(DataTypeCode::Uint128));
				for (const auto& byte : artist->Guid) {
					os.Write8(byte);
				}

				// Property 4: Name (0xDC44)
				os.Write32(0); // object handle
				os.Write16(static_cast<u16>(ObjectProperty::Name));
				os.Write16(static_cast<u16>(DataTypeCode::String));
				os.WriteString(name);

				// Query object handles before creating artist (Windows does this)
				auto handles = _session->GetObjectHandles(_storage, ObjectFormat::Any, Session::Root);

				// Query property descriptions for 0xB218 format (Windows does this)
				try {
					_session->Operation9802(0xDAB0, 0xB218);
					_session->Operation9802(0xDC07, 0xB218);
					_session->Operation9802(0xDA97, 0xB218);
					_session->Operation9802(0xDC44, 0xB218);
				} catch (...) {
					// Non-critical if device doesn't support these queries
				}

				// Create the metadata artist object (format 0xB218)
				auto response = _session->SendObjectPropList(_storage, _artistsFolder, ObjectFormat::Artist, 0, propList);
				artist->Id = response.ObjectId;

				std::cout << "  ✓ Metadata artist object created (ID: 0x" << std::hex << artist->Id << std::dec << ")" << std::endl;

				// Send empty object data (0 bytes) - required by MTP protocol
				ByteArray empty_data;
				IObjectInputStreamPtr empty_stream = std::make_shared<ByteArrayObjectInputStream>(empty_data);
				_session->SendObject(empty_stream);

				// Query all properties back to verify creation (Windows does this)
				try {
					ByteArray propList = _session->GetObjectPropertyList(
						artist->Id,
						ObjectFormat::Any,      // 0x00000000
						ObjectProperty::All,    // 0xFFFFFFFF
						0,                      // groupCode
						0                       // depth
					);
					std::cout << "  ✓ Retrieved " << propList.size() << " bytes of property data from device" << std::endl;
				} catch (const std::exception& e) {
					std::cout << "  Warning: GetObjectPropertyList failed: " << e.what() << std::endl;
				}
			}
			else
			{
				// No GUID - create regular artist object (2 properties)
				os.Write32(2); //number of props

				os.Write32(0); //object handle
				os.Write16(static_cast<u16>(ObjectProperty::Name));
				os.Write16(static_cast<u16>(DataTypeCode::String));
				os.WriteString(name);

				os.Write32(0); //object handle
				os.Write16(static_cast<u16>(ObjectProperty::ObjectFilename));
				os.Write16(static_cast<u16>(DataTypeCode::String));
				os.WriteString(name + ".art");

				auto response = _session->SendObjectPropList(_storage, _artistsFolder, ObjectFormat::Artist, 0, propList);
				artist->Id = response.ObjectId;
			}
		}

		_artists.insert(std::make_pair(name, artist));
		return artist;
	}

	void Library::UpdateArtistGuid(ArtistPtr artist, const std::string& guid)
	{
		if (!artist) {
			error("UpdateArtistGuid: artist is null");
			return;
		}

		if (guid.empty()) {
			debug("UpdateArtistGuid: GUID string is empty, nothing to update");
			return;
		}

		// Parse GUID string (format: "45a663b5-b1cb-4a91-bff6-2bef7bbfdd76")
		// GUIDs use mixed endianness: first 3 components little-endian, last 8 bytes big-endian
		std::string hex = guid;
		// Remove dashes
		hex.erase(std::remove(hex.begin(), hex.end(), '-'), hex.end());

		if (hex.length() != 32) {
			error("UpdateArtistGuid: Invalid GUID format (expected 32 hex chars after removing dashes, got ", hex.length(), ")");
			return;
		}

		std::cout << "UpdateArtistGuid: Updating artist '" << artist->Name << "' with GUID: " << guid << std::endl;

		artist->Guid.resize(16);

		// Component 1: 4 bytes (32-bit) - little-endian
		for (int i = 3; i >= 0; --i) {
			artist->Guid[3 - i] = static_cast<u8>(std::stoul(hex.substr(i * 2, 2), nullptr, 16));
		}

		// Component 2: 2 bytes (16-bit) - little-endian
		for (int i = 1; i >= 0; --i) {
			artist->Guid[4 + (1 - i)] = static_cast<u8>(std::stoul(hex.substr(8 + i * 2, 2), nullptr, 16));
		}

		// Component 3: 2 bytes (16-bit) - little-endian
		for (int i = 1; i >= 0; --i) {
			artist->Guid[6 + (1 - i)] = static_cast<u8>(std::stoul(hex.substr(12 + i * 2, 2), nullptr, 16));
		}

		// Component 4: 8 bytes - big-endian (as-is)
		for (size_t i = 0; i < 8; ++i) {
			artist->Guid[8 + i] = static_cast<u8>(std::stoul(hex.substr(16 + i * 2, 2), nullptr, 16));
		}

		// Verbose logging to confirm GUID was set
		std::cout << "  GUID bytes (hex): ";
		for (size_t i = 0; i < artist->Guid.size(); ++i) {
			if (i > 0) std::cout << ":";
			std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)artist->Guid[i];
		}
		std::cout << std::dec << std::endl;
		std::cout << "  Artist GUID vector size: " << artist->Guid.size() << " bytes" << std::endl;
	}

	void Library::ValidateArtistGuid(const std::string& artist_name, const std::string& track_name, const std::string& guid)
	{
		if (!_artistSupported) {
			debug("ValidateArtistGuid: Artist objects not supported by device");
			return;
		}

		if (guid.empty()) {
			debug("ValidateArtistGuid: GUID is empty, skipping validation");
			return;
		}

		// NOTE: CreateArtist now creates the metadata artist object (0xB218) with GUID
		// This function only needs to call Operation 0x922A to register the track context
		std::cout << "ValidateArtistGuid: Registering track context with Operation 0x922A" << std::endl;
		std::cout << "  Artist: " << artist_name << std::endl;
		std::cout << "  Track: " << track_name << std::endl;

		// Call Operation 0x922A to register track name for metadata retrieval
		// This operation appears ~376ms after artist deletion in Windows capture (frame 5956)
		// CRITICAL: This must be called even if property setting fails!
		std::cout << "  Calling Operation 0x922A with track name: " << track_name << std::endl;
		try {
			_session->Operation922a(track_name);
			std::cout << "  ✓ Operation 0x922A completed - track context registered" << std::endl;
		} catch (const std::exception& e) {
			error("ValidateArtistGuid: Operation 0x922A failed: ", e.what());
		}
	}

	Library::AlbumPtr Library::GetAlbum(const ArtistPtr & artist, std::string name)
	{
		if (name.empty())
			name = UknownAlbum;

		auto it = _albums.find(std::make_pair(artist, name));
		return it != _albums.end()? it->second: AlbumPtr();
	}

	Library::AlbumPtr Library::CreateAlbum(const ArtistPtr & artist, std::string name, int year)
	{
		if (!artist)
			throw std::runtime_error("artists is required");

		if (name.empty())
			name = UknownAlbum;

		ByteArray propList;
		OutputStream os(propList);
		bool sendYear = year != 0 && _albumDateAuthoredSupported;
		bool hasGuid = !artist->Guid.empty();

		// Verbose logging for album creation with GUID
		if (hasGuid) {
			debug("CreateAlbum: Creating album '", name, "' with Zune artist GUID");
			debug("  Artist: ", artist->Name);
			debug("  GUID (hex): ");
			for (size_t i = 0; i < artist->Guid.size(); ++i) {
				if (i > 0) std::cerr << ":";
				std::cerr << hex(artist->Guid[i], 2);
			}
			std::cerr << std::endl;
		}

		os.Write32(3 + (sendYear? 1: 0)); //number of props

		if (_artistSupported)
		{
			os.Write32(0); //object handle
			os.Write16(static_cast<u16>(ObjectProperty::ArtistId));
			os.Write16(static_cast<u16>(DataTypeCode::Uint32));
			os.Write32(artist->Id.Id);
		}
		else
		{
			os.Write32(0); //object handle
			os.Write16(static_cast<u16>(ObjectProperty::Artist));
			os.Write16(static_cast<u16>(DataTypeCode::String));
			os.WriteString(artist->Name);
		}

		os.Write32(0); //object handle
		os.Write16(static_cast<u16>(ObjectProperty::Name));
		os.Write16(static_cast<u16>(DataTypeCode::String));
		os.WriteString(name);

		os.Write32(0); //object handle
		os.Write16(static_cast<u16>(ObjectProperty::ObjectFilename));
		os.Write16(static_cast<u16>(DataTypeCode::String));
		os.WriteString(artist->Name + "--" + name + ".alb");

		if (sendYear)
		{
			os.Write32(0); //object handle
			os.Write16(static_cast<u16>(ObjectProperty::DateAuthored));
			os.Write16(static_cast<u16>(DataTypeCode::String));
			os.WriteString(ConvertYear(year));
		}

		auto album = std::make_shared<Album>();
		album->Artist = artist;
		album->Name = name;
		album->Year = year;
		album->MusicFolderId = GetOrCreate(artist->MusicFolderId, name);

		auto response = _session->SendObjectPropList(_storage, _albumsFolder, ObjectFormat::AbstractAudioAlbum, 0, propList);
		album->Id = response.ObjectId;

		_albums.insert(std::make_pair(std::make_pair(artist, name), album));
		return album;
	}

	bool Library::HasTrack(const AlbumPtr & album, const std::string &name, int trackIndex)
	{
		if (!album)
			return false;

		LoadRefs(album);

		auto & tracks = album->Tracks;
		auto range = tracks.equal_range(name);
		for(auto i = range.first; i != range.second; ++i)
		{
			if (i->second == trackIndex)
				return true;
		}

		return false;
	}

	Library::NewTrackInfo Library::CreateTrack(const ArtistPtr & artist,
		const AlbumPtr & album,
		ObjectFormat type,
		std::string name, const std::string & genre, int trackIndex,
		const std::string &filename, size_t size)
	{
		ByteArray propList;
		OutputStream os(propList);

		os.Write32(3 + (!genre.empty()? 1: 0) + (trackIndex? 1: 0)); //number of props

		if (_artistSupported)
		{
			os.Write32(0); //object handle
			os.Write16(static_cast<u16>(ObjectProperty::ArtistId));
			os.Write16(static_cast<u16>(DataTypeCode::Uint32));
			os.Write32(artist->Id.Id);
		}
		else
		{
			os.Write32(0); //object handle
			os.Write16(static_cast<u16>(ObjectProperty::Artist));
			os.Write16(static_cast<u16>(DataTypeCode::String));
			os.WriteString(artist->Name);
		}


		os.Write32(0); //object handle
		os.Write16(static_cast<u16>(ObjectProperty::Name));
		os.Write16(static_cast<u16>(DataTypeCode::String));
		os.WriteString(name);

		if (trackIndex)
		{
			os.Write32(0); //object handle
			os.Write16(static_cast<u16>(ObjectProperty::Track));
			os.Write16(static_cast<u16>(DataTypeCode::Uint16));
			os.Write16(trackIndex);
		}

		if (!genre.empty())
		{
			os.Write32(0); //object handle
			os.Write16(static_cast<u16>(ObjectProperty::Genre));
			os.Write16(static_cast<u16>(DataTypeCode::String));
			os.WriteString(genre);
		}

		os.Write32(0); //object handle
		os.Write16(static_cast<u16>(ObjectProperty::ObjectFilename));
		os.Write16(static_cast<u16>(DataTypeCode::String));
		os.WriteString(filename);

		auto response = _session->SendObjectPropList(_storage, album->MusicFolderId, type, size, propList);
		NewTrackInfo ti;
		ti.Id = response.ObjectId;
		ti.Name = name;
		ti.Index = trackIndex;
		return ti;
	}

	void Library::LoadRefs(AlbumPtr album)
	{
		if (!album || album->RefsLoaded)
			return;

		auto refs = _session->GetObjectReferences(album->Id).ObjectHandles;
		std::copy(refs.begin(), refs.end(), std::inserter(album->Refs, album->Refs.begin()));
		for(auto trackId : refs)
		{
			auto name = _session->GetObjectStringProperty(trackId, ObjectProperty::Name);
			auto index = _session->GetObjectIntegerProperty(trackId, ObjectProperty::Track);
			debug("[", index, "]: ", name);
			album->Tracks.insert(std::make_pair(name, index));
		}
		album->RefsLoaded = true;
	}

	void Library::AddTrack(AlbumPtr album, const NewTrackInfo & ti)
	{
		if (!album)
			return;

		LoadRefs(album);

		auto & refs = album->Refs;
		auto & tracks = album->Tracks;

		msg::ObjectHandles handles;
		std::copy(refs.begin(), refs.end(), std::back_inserter(handles.ObjectHandles));
		handles.ObjectHandles.push_back(ti.Id);
		_session->SetObjectReferences(album->Id, handles);
		refs.insert(ti.Id);
		tracks.insert(std::make_pair(ti.Name, ti.Index));
	}

	void Library::AddCover(AlbumPtr album, const mtp::ByteArray &data)
	{
		if (!album || !_albumCoverSupported)
			return;

		mtp::debug("sending ", data.size(), " bytes of album cover...");
		_session->SetObjectPropertyAsArray(album->Id, mtp::ObjectProperty::RepresentativeSampleData, data);
	}

	bool Library::Supported(const mtp::SessionPtr & session)
	{
		auto & gdi = session->GetDeviceInfo();
		return
			gdi.Supports(OperationCode::GetObjectPropList) &&
			gdi.Supports(OperationCode::SendObjectPropList) &&
			gdi.Supports(OperationCode::SetObjectReferences) &&
			gdi.Supports(ObjectFormat::AbstractAudioAlbum);
		;
	}

	std::vector<Library::AlbumPtr> Library::GetAlbumsByArtist(const ArtistPtr & artist)
	{
		std::vector<AlbumPtr> result;
		if (!artist)
			return result;

		for (auto& [key, album] : _albums) {
			if (album->Artist == artist) {
				result.push_back(album);
			}
		}
		return result;
	}

	void Library::UpdateAlbumArtist(AlbumPtr album, ArtistPtr new_artist)
	{
		if (!album || !new_artist) {
			error("UpdateAlbumArtist: null album or artist");
			return;
		}

		debug("UpdateAlbumArtist: Updating album '", album->Name, "' to new artist '", new_artist->Name, "'");

		// Update local cache
		album->Artist = new_artist;

		// Update on device via MTP
		if (_artistSupported) {
			_session->SetObjectProperty(
				album->Id,
				ObjectProperty::ArtistId,
				(u64)new_artist->Id.Id
			);
			debug("  ✓ Album ArtistId property updated on device");
		} else {
			// For devices without artist support, update artist name string
			_session->SetObjectProperty(
				album->Id,
				ObjectProperty::Artist,
				new_artist->Name
			);
			debug("  ✓ Album Artist property (string) updated on device");
		}
	}

	std::vector<ObjectId> Library::GetTracksForAlbum(const AlbumPtr & album)
	{
		std::vector<ObjectId> result;
		if (!album)
			return result;

		LoadRefs(album);
		result.insert(result.end(), album->Refs.begin(), album->Refs.end());
		return result;
	}

	void Library::UpdateTrackArtist(ObjectId track_id, ArtistPtr new_artist)
	{
		if (!new_artist) {
			error("UpdateTrackArtist: null artist");
			return;
		}

		debug("UpdateTrackArtist: Updating track ", track_id.Id, " to artist '", new_artist->Name, "'");

		// Update on device via MTP
		if (_artistSupported) {
			_session->SetObjectProperty(
				track_id,
				ObjectProperty::ArtistId,
				(u64)new_artist->Id.Id
			);
			debug("  ✓ Track ArtistId property updated");
		} else {
			// For devices without artist support, update artist name string
			_session->SetObjectProperty(
				track_id,
				ObjectProperty::Artist,
				new_artist->Name
			);
			debug("  ✓ Track Artist property (string) updated");
		}
	}

}
