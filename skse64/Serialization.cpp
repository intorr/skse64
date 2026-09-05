#include "skse64/Serialization.h"

#include "common/IMemoryFileStream.h"
#include "skse64/GameSettings.h"
#include "skse64/InternalSerialization.h"
#include "skse64/ScaleformValue.h"
#include "skse64_common/skse_version.h"

#include <shlobj.h>
#include <vector>

namespace Serialization
{
	const char * kSavegamePath = "\\My Games\\" SAVE_FOLDER_NAME "\\";

	// file format internals

	//	general format:
	//	Header			header
	//		PluginHeader	plugin[header.numPlugins]
	//			ChunkHeader		chunk[plugin.numChunks]
	//				UInt8			data[chunk.length]

	struct Header
	{
		enum
		{
			kSignature =		MACRO_SWAP32('SKSE'),	// endian-swapping so the order matches
			kVersion =			1,

			kVersion_Invalid =	0
		};

		UInt32	signature;
		UInt32	formatVersion;
		UInt32	skseVersion;
		UInt32	runtimeVersion;
		UInt32	numPlugins;
	};

	struct PluginHeader
	{
		UInt32	signature;
		UInt32	numChunks;
		UInt32	length;		// length of following data including ChunkHeader
	};

	struct ChunkHeader
	{
		UInt32	type;
		UInt32	version;
		UInt32	length;
	};

	// locals

	std::string		s_savePath;
	IMemoryFileStream	s_currentFile;

	typedef std::vector <PluginCallbacks>	PluginCallbackList;
	PluginCallbackList	s_pluginCallbacks;

	// handle of the plugin whose save handler is currently running (assigned in
	// HandleSaveGlobalData); never read anywhere -- dead state kept as a hook
	// for future per-plugin diagnostics (e.g. naming the offending plugin in
	// error logs); delete it and the assignment if it stays unused
	PluginHandle	s_currentPlugin = 0;

	Header			s_fileHeader = { 0 };

	UInt64			s_pluginHeaderOffset = 0;
	PluginHeader	s_pluginHeader = { 0 };

	bool			s_chunkOpen = false;
	UInt64			s_chunkHeaderOffset = 0;
	ChunkHeader		s_chunkHeader = { 0 };

	// end offset of the plugin data region currently being loaded; the record
	// readers (GetNextRecordInfo / ReadRecordData) are clamped to it so a corrupt
	// chunk can't run off the end of the in-memory image and trip
	// IMemoryFileStream's fatal read-past-end assert
	UInt64			s_pluginRegionEnd = 0;

	// utilities

	// make full path from save name
	std::string MakeSavePath(std::string name, const char * extension)
	{
		char	path[MAX_PATH];
		ASSERT(SUCCEEDED(SHGetFolderPath(NULL, CSIDL_MYDOCUMENTS, NULL, SHGFP_TYPE_CURRENT, path)));

		std::string	result = path;
		result += kSavegamePath;
		Setting* localSavePath = GetINISetting("sLocalSavePath:General");
		if(localSavePath && (localSavePath->GetType() == Setting::kType_String))
			result += localSavePath->data.s;
		else
			result += "Saves\\";

		result += "\\";
		result += name;
		if (extension)
			result += extension;
		return result;
	}

	PluginCallbacks * GetPluginInfo(PluginHandle plugin)
	{
		if(plugin >= s_pluginCallbacks.size())
			s_pluginCallbacks.resize(plugin + 1);

		return &s_pluginCallbacks[plugin];
	}

	// plugin API
	void SetUniqueID(PluginHandle plugin, UInt32 uid)
	{
		// check existing plugins
		for(PluginCallbackList::iterator iter = s_pluginCallbacks.begin(); iter != s_pluginCallbacks.end(); ++iter)
		{
			if(iter->hadUID && (iter->uid == uid))
			{
				UInt32	collidingID = iter - s_pluginCallbacks.begin();

				_ERROR("plugin serialization UID collision (uid = %08X, plugins = %d %d)", uid, plugin, collidingID);
			}
		}

		PluginCallbacks * info = GetPluginInfo(plugin);

		ASSERT(!info->hadUID);

		info->uid = uid;
		info->hadUID = true;
	}

	void SetRevertCallback(PluginHandle plugin, SKSESerializationInterface::EventCallback callback)
	{
		GetPluginInfo(plugin)->revert = callback;
	}

	void SetSaveCallback(PluginHandle plugin, SKSESerializationInterface::EventCallback callback)
	{
		GetPluginInfo(plugin)->save = callback;
	}

	void SetLoadCallback(PluginHandle plugin, SKSESerializationInterface::EventCallback callback)
	{
		GetPluginInfo(plugin)->load = callback;
	}

	void SetFormDeleteCallback(PluginHandle plugin, SKSESerializationInterface::FormDeleteCallback callback)
	{
		GetPluginInfo(plugin)->formDelete = callback;
	}

	void SetSaveName(const char * name)
	{
		if(name)
		{
			std::string save_name(name);

			if (save_name.length() >= 4 && _stricmp(name+save_name.length()-4, ".ess") == 0)
				save_name = save_name.substr(0, save_name.length() - 4);								
			
			_MESSAGE("save name is %s", save_name.c_str());
			s_savePath = MakeSavePath(save_name, ".skse");
			_MESSAGE("full save path: %s", s_savePath.c_str());
		}
		else
		{
			_MESSAGE("cleared save path");
			s_savePath.clear();
		}
	}

	bool WriteRecord(UInt32 type, UInt32 version, const void * buf, UInt32 length)
	{
		if(!OpenRecord(type, version))
			return false;

		return WriteRecordData(buf, length);
	}

	// flush a chunk header to the file if one is currently open
	static void FlushWriteChunk(void)
	{
		if(!s_chunkOpen)
			return;

		UInt64	curOffset = s_currentFile.GetOffset();
		UInt64	chunkSize = curOffset - s_chunkHeaderOffset - sizeof(s_chunkHeader);

		ASSERT(chunkSize < 0x80000000);	// stupidity check

		s_chunkHeader.length = (UInt32)chunkSize;

		s_currentFile.SetOffset(s_chunkHeaderOffset);
		s_currentFile.WriteBuf(&s_chunkHeader, sizeof(s_chunkHeader));

		s_currentFile.SetOffset(curOffset);

		s_pluginHeader.length += chunkSize + sizeof(s_chunkHeader);

		s_chunkOpen = false;
	}

	bool OpenRecord(UInt32 type, UInt32 version)
	{
		if(!s_pluginHeader.numChunks)
		{
			ASSERT(!s_chunkOpen);

			s_pluginHeaderOffset = s_currentFile.GetOffset();
			s_currentFile.Skip(sizeof(s_pluginHeader));
		}

		FlushWriteChunk();

		s_chunkHeaderOffset = s_currentFile.GetOffset();
		s_currentFile.Skip(sizeof(s_chunkHeader));

		s_pluginHeader.numChunks++;

		s_chunkHeader.type = type;
		s_chunkHeader.version = version;
		s_chunkHeader.length = 0;

		s_chunkOpen = true;

		return true;
	}

	bool WriteRecordData(const void * buf, UInt32 length)
	{
		s_currentFile.WriteBuf(buf, length);

		return true;
	}

	static void FlushReadRecord(void)
	{
		if(s_chunkOpen)
		{
			if(s_chunkHeader.length)
			{
				// _WARNING("plugin didn't finish reading chunk");
				s_currentFile.Skip(s_chunkHeader.length);
			}

			s_chunkOpen = false;
		}
	}

	bool GetNextRecordInfo(UInt32 * type, UInt32 * version, UInt32 * length)
	{
		FlushReadRecord();

		if(!s_pluginHeader.numChunks)
			return false;

		// a bogus record count could ask for a header past the plugin's validated
		// region; refuse it instead of reading off the end of the image
		if((UInt64)s_currentFile.GetOffset() + sizeof(s_chunkHeader) > s_pluginRegionEnd)
			return false;

		s_pluginHeader.numChunks--;

		s_currentFile.ReadBuf(&s_chunkHeader, sizeof(s_chunkHeader));

		*type =		s_chunkHeader.type;
		*version =	s_chunkHeader.version;
		*length =	s_chunkHeader.length;

		s_chunkOpen = true;

		return true;
	}

	UInt32 ReadRecordData(void * buf, UInt32 length)
	{
		ASSERT(s_chunkOpen);

		if(length > s_chunkHeader.length)
			length = s_chunkHeader.length;

		// clamp to the validated plugin region so a corrupt chunk length can't
		// run off the end of the image (which would trip the fatal assert); a
		// clamped read returns fewer bytes and the caller treats it as a failure
		UInt64	curOffset = (UInt64)s_currentFile.GetOffset();
		UInt64	remain = (curOffset < s_pluginRegionEnd) ? (s_pluginRegionEnd - curOffset) : 0;
		if((UInt64)length > remain)
			length = (UInt32)remain;

		if(length)
			s_currentFile.ReadBuf(buf, length);

		s_chunkHeader.length -= length;

		return length;
	}

	bool ResolveFormId(UInt32 formId, UInt32 * formIdOut)
	{
		UInt32	modID = formId >> 24;
		if (modID == 0xFF)
		{
			*formIdOut = formId;
			return true;
		}

		if (modID == 0xFE)
		{
			modID = formId >> 12;
		}

		UInt32	loadedModID = ResolveModIndex(modID);
		if (loadedModID < 0xFF)
		{
			*formIdOut = (formId & 0x00FFFFFF) | (((UInt32)loadedModID) << 24);
			return true;
		}
		else if (loadedModID > 0xFF)
		{
			*formIdOut = (loadedModID << 12) | (formId & 0x00000FFF);
			return true;
		}
		return false;
	}

	bool ResolveHandle(UInt64 handle, UInt64 * handleOut)
	{
		UInt32	modID = (handle & 0xFF000000) >> 24;
		if (modID == 0xFF)
		{
			*handleOut = handle;
			return true;
		}

		if (modID == 0xFE)
		{
			modID = (handle >> 12) & 0xFFFFF;
		}

		UInt64	loadedModID = (UInt64)ResolveModIndex(modID);
		if (loadedModID < 0xFF)
		{
			*handleOut = (handle & 0xFFFFFFFF00FFFFFF) | (((UInt64)loadedModID) << 24);
			return true;
		}
		else if (loadedModID > 0xFF)
		{
			*handleOut = (handle & 0xFFFFFFFF00000FFF) | (loadedModID << 12);
			return true;
		}
		return false;
	}

	// internal event handlers
	void HandleRevertGlobalData(void)
	{
		for(UInt32 i = 0; i < s_pluginCallbacks.size(); i++)
			if(s_pluginCallbacks[i].revert)
				s_pluginCallbacks[i].revert(&g_SKSESerializationInterface);
	}

	void HandleSaveGlobalData(void)
	{
		_MESSAGE("creating co-save");

		if(s_savePath.empty())
		{
			_ERROR("HandleSaveGlobalData: no save path set");
			return;
		}

		// Build the co-save in a temporary file, then atomically rename it into
		// place on success. The previous .skse is only replaced once the whole
		// new file is committed: if saving fails or is interrupted, the old
		// .skse stays intact (only the temp file, removed below, is affected).
		// The ".skse" extension is swapped for ".tmp" instead of appended, so
		// the temp path is never longer than the final one (MAX_PATH edge).
		// "-5" assumes the path ends in ".skse" -- always true, since SetSaveName
		// is the only writer of s_savePath and appends that extension; guard it so
		// a future extension change is caught in debug instead of silently
		// producing a wrong temp path.
		ASSERT(s_savePath.size() >= 5 && _stricmp(s_savePath.c_str() + s_savePath.size() - 5, ".skse") == 0);

		std::string	tempPath = s_savePath.substr(0, s_savePath.size() - 5) + ".tmp";

		DeleteFile(tempPath.c_str());

		// create is in-memory: it flushes any still-pending image from a prior
		// save before starting. A failed save discards its image (the cleanup
		// below), so this is only reachable if that flush keeps failing (disk
		// error); the save aborts with the old .skse intact. Other disk errors
		// surface through Close() below (saveSucceeded)
		if(!s_currentFile.Create(tempPath.c_str()))
		{
			_ERROR("HandleSaveGlobalData: couldn't create save file (%s), a previous flush is still failing", tempPath.c_str());
			return;
		}

		bool	saveSucceeded = false;

		try
		{
			// init header
			s_fileHeader.signature =		Header::kSignature;
			s_fileHeader.formatVersion =	Header::kVersion;
			s_fileHeader.skseVersion =		PACKED_SKSE_VERSION;
			s_fileHeader.runtimeVersion =	RUNTIME_VERSION;
			s_fileHeader.numPlugins =		0;

			s_currentFile.Skip(sizeof(s_fileHeader));

			// iterate through plugins
			for(UInt32 i = 0; i < s_pluginCallbacks.size(); i++)
			{
				PluginCallbacks	* info = &s_pluginCallbacks[i];

				if(info->save && info->hadUID)
				{
					// set up header info
					s_currentPlugin = i;

					s_pluginHeader.signature = info->uid;
					s_pluginHeader.numChunks = 0;
					s_pluginHeader.length = 0;

					s_chunkOpen = false;

					bool	pluginSaveFailed = false;

					// call the plugin
					try
					{
						info->save(&g_SKSESerializationInterface);
					}
					catch( ... )
					{
						// the offset is kept so a mod author can locate where the
						// plugin's save handler blew up
						_ERROR("HandleSaveGlobalData: exception occurred saving %08X at offset %016I64X, discarding that plugin's last record; its data may be corrupt", s_pluginHeader.signature, s_currentFile.GetOffset());
						pluginSaveFailed = true;
					}

					if(pluginSaveFailed && s_chunkOpen)
					{
						// drop the incomplete chunk that was open when the plugin
						// threw (its header/data was never flushed); keep any
						// chunks the plugin fully wrote
						UInt64	partialSize = s_currentFile.GetOffset() - s_chunkHeaderOffset - sizeof(s_chunkHeader);

						_MESSAGE("HandleSaveGlobalData: discarding incomplete record from %08X (type %08X version %u, %u bytes written)", s_pluginHeader.signature, s_chunkHeader.type, s_chunkHeader.version, (UInt32)partialSize);

						s_currentFile.SetLength(s_chunkHeaderOffset);
						s_pluginHeader.numChunks--;
						s_chunkOpen = false;

						if(!s_pluginHeader.numChunks)
						{
							// the dropped chunk was this plugin's first record:
							// also remove the reserved (never filled) 12-byte
							// plugin header, otherwise a zeroed phantom header
							// is committed and every load warns about a
							// "plugin with signature 00000000"
							s_currentFile.SetLength(s_pluginHeaderOffset);
						}
					}

					// flush the remaining chunk data
					FlushWriteChunk();

					if(s_pluginHeader.numChunks)
					{
						UInt64	curOffset = s_currentFile.GetOffset();

						s_currentFile.SetOffset(s_pluginHeaderOffset);
						s_currentFile.WriteBuf(&s_pluginHeader, sizeof(s_pluginHeader));

						s_currentFile.SetOffset(curOffset);

						s_fileHeader.numPlugins++;
					}
				}
			}

			// write header
			s_currentFile.SetOffset(0);
			s_currentFile.WriteBuf(&s_fileHeader, sizeof(s_fileHeader));

			// flush the temporary file; reports whether the write to disk succeeded
			saveSucceeded = s_currentFile.Close();

			if(!saveSucceeded)
				_ERROR("HandleSaveGlobalData: failed to write the co-save to disk; plugin data was NOT saved (%s)", tempPath.c_str());
		}
		catch(...)
		{
			_ERROR("HandleSaveGlobalData: exception during save (outside a plugin handler)");
		}

		if(saveSucceeded)
		{
			// atomically replace the real co-save with the fully-written temp file
			if(!MoveFileExA(tempPath.c_str(), s_savePath.c_str(), MOVEFILE_REPLACE_EXISTING))
			{
				_ERROR("HandleSaveGlobalData: couldn't commit save file (%s), error %u", tempPath.c_str(), GetLastError());
				saveSucceeded = false;
			}
		}

		if(!saveSucceeded)
		{
			// drop the in-memory image on any failure. On a failed flush it still
			// holds the whole (stale) co-save, and leaving it dirty would make a
			// later save -- possibly of a differently-named slot -- flush it to
			// its own temp path as an orphan. After a failed move the image is
			// already clean, so this is a no-op there.
			s_currentFile.Discard();

			if(!DeleteFile(tempPath.c_str()))
			{
				UInt32	err = (UInt32)GetLastError();

				if(err != ERROR_FILE_NOT_FOUND)
					_WARNING("HandleSaveGlobalData: could not remove temp file (%s), error %u", tempPath.c_str(), err);
			}
		}
	}

	void HandleLoadGlobalData(void)
	{
		_MESSAGE("loading co-save");

		if(!s_currentFile.Open(s_savePath.c_str()))
		{
			// no co-save yet (first load) or the file couldn't be read; either
			// way there is nothing to load, so skip without disturbing the game
			_MESSAGE("HandleLoadGlobalData: co-save not loaded (%s)", s_savePath.c_str());
			return;
		}

		try
		{
			// reset per-load state left over from an earlier load in this session.
			// the no-data dispatch below bounds record reads against
			// s_pluginRegionEnd and s_pluginHeader.numChunks; if those retained
			// values from a previous (larger) co-save, a data-less plugin would be
			// handed phantom records, or a read could run off the end of a smaller
			// image and trip the fatal assert
			s_pluginHeader = { 0 };
			s_pluginRegionEnd = 0;
			s_chunkOpen = false;

			Header	header;

			// a co-save shorter than its own header cannot be parsed; abort here
			// rather than in the ReadBuf below, which would run off the end of the
			// in-memory image and trip its fatal assert
			if(s_currentFile.GetRemain() < (SInt64)sizeof(header))
			{
				_ERROR("HandleLoadGame: co-save is %u bytes but needs %u for its header; aborting load", (UInt32)s_currentFile.GetRemain(), (UInt32)sizeof(header));
				goto done;
			}

			s_currentFile.ReadBuf(&header, sizeof(header));

			if(header.signature != Header::kSignature)
			{
				_ERROR("HandleLoadGame: invalid file signature (found %08X expected %08X)", header.signature, Header::kSignature);
				goto done;
			}

			if(header.formatVersion <= Header::kVersion_Invalid)
			{
				_ERROR("HandleLoadGame: version invalid (%08X)", header.formatVersion);
				goto done;
			}

			if(header.formatVersion > Header::kVersion)
			{
				_ERROR("HandleLoadGame: version too new (found %08X current %08X)", header.formatVersion, Header::kVersion);
				goto done;
			}

			// reset flags
			for(PluginCallbackList::iterator iter = s_pluginCallbacks.begin(); iter != s_pluginCallbacks.end(); ++iter)
				iter->hadData = false;
			
			// iterate through plugin data chunks
			while(s_currentFile.GetRemain() >= sizeof(PluginHeader))
			{
				s_currentFile.ReadBuf(&s_pluginHeader, sizeof(s_pluginHeader));

				UInt64	pluginChunkStart = s_currentFile.GetOffset();

				// the plugin's declared data length must fit in the file: a bogus
				// value would position the stream past the end (or let a record
				// read run off the image into its fatal assert). The stream is
				// corrupt from here, so stop loading further plugins
				if(pluginChunkStart + s_pluginHeader.length > (UInt64)s_currentFile.GetLength())
				{
					_ERROR("HandleLoadGame: plugin %08X claims %u bytes past the end of the co-save; stopping", s_pluginHeader.signature, s_pluginHeader.length);

					// the no-data dispatch below still sees this header; drop
					// its chunk count, otherwise a stale count (with a stale
					// s_pluginRegionEnd left over from an earlier load in the
					// session) hands phantom records to plugins that had no
					// data in this file
					s_pluginHeader.numChunks = 0;
					break;
				}

				// bound this plugin's record reads to its validated region so a
				// corrupt chunk (see GetNextRecordInfo / ReadRecordData) degrades to
				// a stopped load instead of tripping the image's fatal assert
				s_pluginRegionEnd = pluginChunkStart + s_pluginHeader.length;

				UInt32	pluginIdx = kPluginHandle_Invalid;

				for(PluginCallbackList::iterator iter = s_pluginCallbacks.begin(); iter != s_pluginCallbacks.end(); ++iter)
					if(iter->hadUID && (iter->uid == s_pluginHeader.signature))
						pluginIdx = iter - s_pluginCallbacks.begin();

				try
				{
					if(pluginIdx != kPluginHandle_Invalid)
					{
						PluginCallbacks	* info = &s_pluginCallbacks[pluginIdx];

						info->hadData = true;

						if(info->load)
						{
							s_chunkOpen = false;
							info->load(&g_SKSESerializationInterface);
						}
					}
					else
					{
						_WARNING("HandleLoadGame: plugin with signature %08X not loaded", s_pluginHeader.signature);
					}
				}
				catch( ... )
				{
					_ERROR("HandleLoadGame: exception occurred loading %08X", s_pluginHeader.signature);
				}

				// if plugin failed to read all its data or threw exception, jump to the next chunk
				UInt64	expectedOffset = pluginChunkStart + s_pluginHeader.length;
				if(s_currentFile.GetOffset() != expectedOffset)
				{
					_WARNING("HandleLoadGame: plugin did not read all of its data (at %016I64X expected %016I64X)", s_currentFile.GetOffset(), expectedOffset);
					s_currentFile.SetOffset(expectedOffset);
				}
			}

			// call load on plugins that had no data
			for(PluginCallbackList::iterator iter = s_pluginCallbacks.begin(); iter != s_pluginCallbacks.end(); ++iter) {
				if(!iter->hadData && iter->load) {
					// drop a record the previous (data) plugin may have left open:
					// without this, the first GetNextRecordInfo below would run
					// FlushReadRecord() and Skip() the stale s_chunkHeader.length
					// from a stream offset that is already at the end of the data,
					// pushing it past the end of the image
					s_chunkOpen = false;

					// isolate this plugin the same way as the data loop above: a
					// throw in its reset-to-defaults path must not abort the
					// loads of the remaining plugins
					try
					{
						iter->load(&g_SKSESerializationInterface);
					}
					catch(...)
					{
						_ERROR("HandleLoadGame: exception occurred (no-data) loading %08X", iter->uid);
					}
				}
			}
		}
		catch(...)
		{
			// last-resort guard: every plugin load is already isolated above (data
			// loop and no-data loop), so this only catches an unexpected throw
			// outside a plugin handler; the stream is still closed via done:
			_ERROR("HandleLoadGame: exception during load (outside a plugin handler)");
		}

	done:
		s_currentFile.Close();
	}

	void HandleDeleteSave(std::string saveName)
	{
		std::string savePath = MakeSavePath(saveName, NULL);
		std::string coSavePath = savePath;
		savePath += ".ess";
		coSavePath += ".skse";

		// Old save file really gone?
		IFileStream	saveFile;
		if (!saveFile.Open(savePath.c_str()))
		{
			_MESSAGE("deleting co-save %s", coSavePath.c_str());	
			DeleteFile(coSavePath.c_str());
		}
		else
		{
			_MESSAGE("skipped delete of co-save %s", coSavePath.c_str());	
		}
	}

	void HandleDeletedForm(UInt64 handle)
	{
		for(UInt32 i = 0; i < s_pluginCallbacks.size(); i++)
			if(s_pluginCallbacks[i].formDelete)
				s_pluginCallbacks[i].formDelete(handle);
	}

	template <>
	bool WriteData<BSFixedString>(SKSESerializationInterface * intfc, const BSFixedString * str)
	{
		return WriteData<const char>(intfc, str->data);
	}

	template <>
	bool ReadData<BSFixedString>(SKSESerializationInterface * intfc, BSFixedString * str)
	{
		char buf[257] = { 0 };
		UInt16 len = 0;

		if (intfc->ReadRecordData(&len, sizeof(len)) != sizeof(len))
			return false;

		if (len > 256)
			return false;

		if (intfc->ReadRecordData(buf, len) != len)
			return false;

		*str = BSFixedString(buf);
		return true;
	}

	template <>
	bool WriteData<std::string>(SKSESerializationInterface * intfc, const std::string * str)
	{
		// check the length before narrowing to UInt16: a string longer than
		// 65535 would otherwise wrap (e.g. 65536 -> 0) and silently write a
		// short/empty payload that returns "success"
		size_t totalLen = str->length();
		if (totalLen > 256)
			return false;

		UInt16 len = (UInt16)totalLen;

		if (! intfc->WriteRecordData(&len, sizeof(len)))
			return false;
		if (! intfc->WriteRecordData(str->data(), len))
			return false;
		return true;
	}

	template <>
	bool ReadData<std::string>(SKSESerializationInterface * intfc, std::string * str)
	{
		char buf[257] = { 0 };
		UInt16 len = 0;

		if (intfc->ReadRecordData(&len, sizeof(len)) != sizeof(len))
			return false;

		if (len > 256)
			return false;

		if (intfc->ReadRecordData(buf, len) != len)
			return false;

		*str = std::string(buf);
		return true;
	}

	template <>
	bool WriteData<const char>(SKSESerializationInterface * intfc, const char* str)
	{
		// check the length before narrowing to UInt16: a string longer than
		// 65535 would otherwise wrap (e.g. 65536 -> 0) and silently write a
		// short/empty payload that returns "success"
		size_t totalLen = strlen(str);
		if (totalLen > 256)
			return false;

		UInt16 len = (UInt16)totalLen;

		if (! intfc->WriteRecordData(&len, sizeof(len)))
			return false;
		if (! intfc->WriteRecordData(str, len))
			return false;
		return true;
	}

	template <>
	bool WriteData<GFxValue>(SKSESerializationInterface* intfc, const GFxValue* val)
	{
		UInt32 type = val->GetType();

		// validate the variant BEFORE committing the 4-byte type tag, so a logical
		// write failure (an unsupported type, or a string too long to serialize)
		// leaves no orphan tag in the open chunk for the reader to choke on: the
		// read side consumes the tag first, then fails on the missing/short payload
		// (a torn record)
		switch (type)
		{
		case GFxValue::kType_Bool:
		case GFxValue::kType_Number:
			break; // fixed-width, always writable
		case GFxValue::kType_String:
		{
			const char * s = val->GetString();
			if (!s || strlen(s) > 256)
				return false;
			break;
		}
		default:
			// Unsupported
			return false;
		}

		if (! WriteData(intfc, &type))
			return false;

		switch (type)
		{
		case GFxValue::kType_Bool:
		{
			bool t = val->GetBool();
			return WriteData(intfc, &t);
		}
		case GFxValue::kType_Number:
		{
			double t = val->GetNumber();
			return WriteData(intfc, &t);
		}
		case GFxValue::kType_String:
		{
			return WriteData<const char>(intfc, val->GetString());
		}
		default:
			return false; // unreachable (validated above)
		}
	}

	template <>
	bool ReadData<GFxValue>(SKSESerializationInterface* intfc, GFxValue* val)
	{
		UInt32 type;
		if (! ReadData(intfc, &type))
			return false;

		switch (type)
		{
		case GFxValue::kType_Bool:
		{
			bool t;
			if (! ReadData(intfc, &t))
				return false;
			val->SetBool(t);
			return true;
		}
		case GFxValue::kType_Number:
		{
			double t;
			if (! ReadData(intfc, &t))
				return false;
			val->SetNumber(t);
			return true;
		}
		case GFxValue::kType_String:
		{
			// As usual, using string cache to manage strings
			BSFixedString t;
			if (! ReadData(intfc, &t))
				return false;
			val->SetString(t.data);
			return true;
		}
		default:
			// Unsupported
			return false;
		}

		return false;
	}
}

SKSESerializationInterface	g_SKSESerializationInterface =
{
	SKSESerializationInterface::kVersion,

	Serialization::SetUniqueID,

	Serialization::SetRevertCallback,
	Serialization::SetSaveCallback,
	Serialization::SetLoadCallback,
	Serialization::SetFormDeleteCallback,

	Serialization::WriteRecord,
	Serialization::OpenRecord,
	Serialization::WriteRecordData,

	Serialization::GetNextRecordInfo,
	Serialization::ReadRecordData,
	Serialization::ResolveHandle,
	Serialization::ResolveFormId
};
