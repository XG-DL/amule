// This file is part of the aMule Project.

#include "CompletedDownloadHistory.h"

#include "CFile.h"
#include "Logger.h"
#include "Preferences.h"
#include "PicoJson_Inc.h"
#include <common/Format.h>
#include <limits>
#include <algorithm>

namespace
{
const uint8 kHistoryVersion = 1;
// Keep the record ceiling compatible with the 64 MiB portable file ceiling.
// At this size even a timestamp with 19 decimal digits remains exportable.
const uint32 kMaximumRecords = 700000;
const size_t kMaximumJsonBytes = 64 * 1024 * 1024;
const wxString kHistoryFilename = "completed_downloads.met";
} // namespace

CCompletedDownloadHistory::CCompletedDownloadHistory()
: m_retentionDays(thePrefs::GetCompletedHistoryRetentionDays())
{
	Load();
	if (Prune()) {
		m_dirty = true;
		Save();
	}
	m_flushThread = std::thread([this]() {
		std::unique_lock<std::mutex> lock(m_mutex);
		while (!m_stopping) {
			if (m_flushCondition.wait_for(
				    lock, std::chrono::seconds(30), [this]() { return m_stopping; }))
				break;
			if (m_dirty)
				Save();
		}
	});
}

CCompletedDownloadHistory::~CCompletedDownloadHistory()
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_stopping = true;
	}
	m_flushCondition.notify_all();
	if (m_flushThread.joinable())
		m_flushThread.join();
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_dirty)
		Save();
}

void CCompletedDownloadHistory::Load()
{
	const CPath path(thePrefs::GetConfigDir() + kHistoryFilename);
	if (!path.FileExists())
		return;
	CFile file;
	if (!file.Open(path)) {
		AddLogLineC(CFormat(_("WARNING: %s cannot be opened.")) % kHistoryFilename);
		return;
	}
	try {
		if (file.ReadUInt8() != kHistoryVersion) {
			AddLogLineC(_("WARNING: Completed download history has an invalid header."));
			return;
		}
		const uint32 count = file.ReadUInt32();
		if (count > kMaximumRecords) {
			AddLogLineC(_("WARNING: Completed download history contains too many records."));
			return;
		}
		for (uint32 i = 0; i < count; ++i) {
			CMD4Hash hash;
			file.Read(hash.GetHash(), 16);
			const time_t completedAt = static_cast<time_t>(file.ReadUInt64());
			if (!hash.IsEmpty() && completedAt > 0)
				m_entries[hash] = completedAt;
		}
		RebuildOldestIndex();
	} catch (const CSafeIOException &e) {
		m_entries.clear();
		AddLogLineC(CFormat(_("IO error while reading %s: %s")) % kHistoryFilename % e.what());
	}
}

void CCompletedDownloadHistory::RebuildOldestIndex()
{
	m_oldestFirst.clear();
	for (const auto &entry : m_entries)
		m_oldestFirst.emplace(entry.second, entry.first);
}

bool CCompletedDownloadHistory::Save()
{
	CFile file(thePrefs::GetConfigDir() + kHistoryFilename, CFile::write_safe);
	if (!file.IsOpened()) {
		AddLogLineC(CFormat(_("Error while saving %s: could not open the file.")) % kHistoryFilename);
		return false;
	}
	try {
		file.WriteUInt8(kHistoryVersion);
		file.WriteUInt32(static_cast<uint32>(m_entries.size()));
		for (const auto &entry : m_entries) {
			file.Write(entry.first.GetHash(), 16);
			file.WriteUInt64(static_cast<uint64>(entry.second));
		}
		file.Close();
		m_dirty = false;
		return true;
	} catch (const CIOFailureException &e) {
		AddLogLineC(CFormat(_("Error while saving %s: %s")) % kHistoryFilename % e.what());
		return false;
	}
}

bool CCompletedDownloadHistory::Prune()
{
	if (m_retentionDays == 0)
		return false;
	const time_t cutoff = time(nullptr) - static_cast<time_t>(m_retentionDays) * 24 * 60 * 60;
	bool changed = false;
	for (auto it = m_entries.begin(); it != m_entries.end();) {
		if (it->second < cutoff) {
			it = m_entries.erase(it);
			changed = true;
		} else
			++it;
	}
	if (changed)
		RebuildOldestIndex();
	return changed;
}

bool CCompletedDownloadHistory::Contains(const CMD4Hash &hash) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return !hash.IsEmpty() && m_entries.find(hash) != m_entries.end();
}

time_t CCompletedDownloadHistory::CompletedAt(const CMD4Hash &hash) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const auto it = m_entries.find(hash);
	return it == m_entries.end() ? 0 : it->second;
}

uint32 CCompletedDownloadHistory::GetRetentionDays() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_retentionDays;
}

size_t CCompletedDownloadHistory::GetCount() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_entries.size();
}

void CCompletedDownloadHistory::Record(const CMD4Hash &hash, time_t completedAt)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (hash.IsEmpty() || !thePrefs::IsCompletedHistoryEnabled())
		return;
	m_retentionDays = thePrefs::GetCompletedHistoryRetentionDays();
	if (completedAt == 0)
		completedAt = time(nullptr);
	auto existing = m_entries.find(hash);
	if (existing == m_entries.end() && m_entries.size() >= kMaximumRecords) {
		if (!m_oldestFirst.empty()) {
			m_entries.erase(m_oldestFirst.begin()->second);
			m_oldestFirst.erase(m_oldestFirst.begin());
		}
		if (!m_limitWarningLogged) {
			AddLogLineC(_("WARNING: Completed download history reached its 700,000-record limit; "
				      "oldest entries will be replaced."));
			m_limitWarningLogged = true;
		}
	}
	if (existing != m_entries.end()) {
		auto range = m_oldestFirst.equal_range(existing->second);
		for (auto it = range.first; it != range.second; ++it) {
			if (it->second == hash) {
				m_oldestFirst.erase(it);
				break;
			}
		}
	}
	m_entries[hash] = completedAt;
	m_oldestFirst.emplace(completedAt, hash);
	++m_revision;
	m_dirty = true;
}

void CCompletedDownloadHistory::SetRetentionDays(uint32 days)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_retentionDays == days)
		return;
	m_retentionDays = days;
	if (Prune()) {
		m_dirty = true;
		Save();
	}
	++m_revision;
}

void CCompletedDownloadHistory::Clear()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_entries.clear();
	m_oldestFirst.clear();
	++m_revision;
	m_dirty = true;
	Save();
}

bool CCompletedDownloadHistory::ExportJson(std::string &json, std::string &error) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	// Bound portable history files independently of the HTTP per-request limit. The Web API
	// transfers these files in smaller chunks; keeping a total ceiling bounds parsing memory.
	size_t estimatedBytes = 64; // format/version/entries envelope
	for (const auto &entry : m_entries) {
		const size_t timestampBytes = std::to_string(static_cast<uint64>(entry.second)).size();
		const size_t entryBytes = 62 + timestampBytes;
		if (estimatedBytes > kMaximumJsonBytes - std::min(kMaximumJsonBytes, entryBytes)) {
			error = "History exceeds the 64 MiB portable transfer limit.";
			return false;
		}
		estimatedBytes += entryBytes;
	}
	if (estimatedBytes > kMaximumJsonBytes) {
		error = "History exceeds the 64 MiB portable transfer limit.";
		return false;
	}
	picojson::object root;
	root["format"] = picojson::value("amule.completed-download-history");
	root["version"] = picojson::value(1.0);
	picojson::array entries;
	entries.reserve(m_entries.size());
	for (const auto &entry : m_entries) {
		picojson::object item;
		item["hash"] = picojson::value(entry.first.EncodeSTL());
		item["completed_at"] = picojson::value(static_cast<double>(entry.second));
		entries.push_back(picojson::value(item));
	}
	root["entries"] = picojson::value(entries);
	json = picojson::value(root).serialize();
	if (json.size() > kMaximumJsonBytes) {
		json.clear();
		error = "History exceeds the 64 MiB portable transfer limit.";
		return false;
	}
	return true;
}

bool CCompletedDownloadHistory::BeginExport(
	const std::string &id, size_t &total, uint64 &revision, std::string &error)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const auto now = std::chrono::steady_clock::now();
	for (auto it = m_exports.begin(); it != m_exports.end();) {
		if (it->second.expires <= now)
			it = m_exports.erase(it);
		else
			++it;
	}
	if (id.empty() || m_exports.find(id) != m_exports.end() || !m_exports.empty()) {
		error = "Another completed-history export is already in progress.";
		return false;
	}
	total = m_entries.size();
	revision = m_revision;
	ExportCursor cursor;
	cursor.next = m_entries.begin();
	cursor.revision = m_revision;
	cursor.expires = now + std::chrono::minutes(10);
	m_exports.emplace(id, std::move(cursor));
	return true;
}

bool CCompletedDownloadHistory::ExportJsonBatch(const std::string &id,
	size_t offset,
	size_t limit,
	std::string &json,
	size_t &total,
	std::string &error)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	auto cursor = m_exports.find(id);
	if (cursor == m_exports.end()) {
		error = "Completed-history export session was not found.";
		return false;
	}
	if (cursor->second.expires <= std::chrono::steady_clock::now()) {
		m_exports.erase(cursor);
		error = "Completed-history export session expired.";
		return false;
	}
	total = m_entries.size();
	if (cursor->second.revision != m_revision) {
		error = "Completed-download history changed during export; start the export again.";
		return false;
	}
	if (offset == cursor->second.lastOffset) {
		json = cursor->second.lastJson;
		return true;
	}
	if (offset != cursor->second.expectedOffset || offset > total || limit == 0 || limit > 4096) {
		error = "Invalid completed-history batch range.";
		return false;
	}
	picojson::array entries;
	entries.reserve(std::min(limit, total - offset));
	for (size_t i = 0; i < limit && cursor->second.next != m_entries.end(); ++i, ++cursor->second.next) {
		picojson::object item;
		item["hash"] = picojson::value(cursor->second.next->first.EncodeSTL());
		item["completed_at"] = picojson::value(static_cast<double>(cursor->second.next->second));
		entries.push_back(picojson::value(item));
	}
	json = picojson::value(entries).serialize();
	if (json.size() > 512 * 1024) {
		json.clear();
		error = "Completed-history batch exceeded the transfer limit.";
		return false;
	}
	cursor->second.lastOffset = offset;
	cursor->second.lastJson = json;
	cursor->second.expectedOffset = offset + entries.size();
	cursor->second.expires = std::chrono::steady_clock::now() + std::chrono::minutes(10);
	return true;
}

void CCompletedDownloadHistory::EndExport(const std::string &id)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_exports.erase(id);
}

bool CCompletedDownloadHistory::BeginImport(const std::string &id, bool merge, std::string &error)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const auto now = std::chrono::steady_clock::now();
	for (auto it = m_imports.begin(); it != m_imports.end();) {
		if (it->second.expires <= now)
			it = m_imports.erase(it);
		else
			++it;
	}
	const auto completed = m_committedImports.find(id);
	if (completed != m_committedImports.end() && completed->second > std::chrono::steady_clock::now())
		return true;
	const auto existing = m_imports.find(id);
	if (existing != m_imports.end() && existing->second.merge == merge)
		return true;
	if (id.empty() || existing != m_imports.end() || !m_imports.empty()) {
		error = "Another completed-history import is already in progress.";
		return false;
	}
	StagedImport staged;
	staged.merge = merge;
	staged.expires = now + std::chrono::minutes(10);
	m_imports.emplace(id, std::move(staged));
	return true;
}

bool CCompletedDownloadHistory::ImportJsonBatch(
	const std::string &id, const std::string &json, std::string &error)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	auto staged = m_imports.find(id);
	if (staged == m_imports.end()) {
		error = "Completed-history import session was not found.";
		return false;
	}
	if (staged->second.expires <= std::chrono::steady_clock::now()) {
		m_imports.erase(staged);
		error = "Completed-history import session expired.";
		return false;
	}
	picojson::value value;
	const std::string parseError = picojson::parse(value, json);
	if (!parseError.empty() || !value.is<picojson::array>()) {
		error = "History batch is not a valid JSON array.";
		return false;
	}
	const picojson::array &array = value.get<picojson::array>();
	if (array.size() > 4096) {
		error = "History import exceeds the record limit.";
		return false;
	}
	for (const auto &entryValue : array) {
		if (!entryValue.is<picojson::object>()) {
			error = "History file contains an invalid entry.";
			return false;
		}
		const picojson::object &item = entryValue.get<picojson::object>();
		const auto hashValue = item.find("hash");
		const auto timeValue = item.find("completed_at");
		if (hashValue == item.end() || !hashValue->second.is<std::string>() ||
			timeValue == item.end() || !timeValue->second.is<double>()) {
			error = "History file entry is missing its hash or completion time.";
			return false;
		}
		const double seconds = timeValue->second.get<double>();
		CMD4Hash hash;
		if (!hash.Decode(hashValue->second.get<std::string>()) || hash.IsEmpty() || seconds < 1 ||
			seconds > static_cast<double>(std::numeric_limits<time_t>::max())) {
			error = "History file contains an invalid hash or completion time.";
			return false;
		}
		const time_t completedAt = static_cast<time_t>(seconds);
		if (static_cast<double>(completedAt) != seconds) {
			error = "History file completion times must be whole Unix seconds.";
			return false;
		}
		if (staged->second.entries.find(hash) == staged->second.entries.end() &&
			staged->second.entries.size() >= kMaximumRecords) {
			error = "History import exceeds the record limit.";
			return false;
		}
		auto inserted = staged->second.entries.emplace(hash, completedAt);
		if (!inserted.second && completedAt > inserted.first->second)
			inserted.first->second = completedAt;
	}
	staged->second.expires = std::chrono::steady_clock::now() + std::chrono::minutes(10);
	return true;
}

bool CCompletedDownloadHistory::CommitImport(const std::string &id, std::string &error)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const auto now = std::chrono::steady_clock::now();
	for (auto it = m_committedImports.begin(); it != m_committedImports.end();) {
		if (it->second <= now)
			it = m_committedImports.erase(it);
		else
			++it;
	}
	auto staged = m_imports.find(id);
	if (staged == m_imports.end()) {
		if (m_committedImports.find(id) != m_committedImports.end())
			return true;
		error = "Completed-history import session was not found.";
		return false;
	}
	if (staged->second.expires <= now) {
		m_imports.erase(staged);
		error = "Completed-history import session expired.";
		return false;
	}
	m_retentionDays = thePrefs::GetCompletedHistoryRetentionDays();
	if (m_retentionDays != 0) {
		const time_t cutoff = time(nullptr) - static_cast<time_t>(m_retentionDays) * 24 * 60 * 60;
		for (auto it = staged->second.entries.begin(); it != staged->second.entries.end();) {
			if (it->second < cutoff)
				it = staged->second.entries.erase(it);
			else
				++it;
		}
	}
	if (staged->second.merge) {
		size_t additions = 0;
		for (const auto &entry : staged->second.entries) {
			if (m_entries.find(entry.first) == m_entries.end())
				++additions;
		}
		if (additions > kMaximumRecords - std::min<size_t>(m_entries.size(), kMaximumRecords)) {
			error = "The merged history would exceed the record limit.";
			return false;
		}
		for (auto entry = staged->second.entries.begin(); entry != staged->second.entries.end();
			++entry) {
			auto current = m_entries.find(entry->first);
			if (current == m_entries.end()) {
				m_entries.emplace(entry->first, entry->second);
				entry->second =
					static_cast<time_t>(-1); // rollback marker: inserted by this commit
			} else if (entry->second > current->second) {
				const time_t oldValue = current->second;
				current->second = entry->second;
				entry->second = oldValue; // rollback marker: previous timestamp
			} else {
				entry->second = 0; // unchanged by this commit
			}
		}
	} else {
		const bool wasDirty = m_dirty;
		std::map<CMD4Hash, time_t> previous;
		previous.swap(m_entries);
		m_entries.swap(staged->second.entries);
		RebuildOldestIndex();
		m_dirty = true;
		if (!Save()) {
			m_entries.swap(previous);
			RebuildOldestIndex();
			m_dirty = wasDirty;
			m_imports.erase(staged);
			error = "Could not save the imported history.";
			return false;
		}
		m_imports.erase(staged);
		++m_revision;
		m_committedImports[id] = now + std::chrono::minutes(10);
		return true;
	}
	const bool wasDirty = m_dirty;
	m_dirty = true;
	if (!Save()) {
		for (const auto &entry : staged->second.entries) {
			if (entry.second == static_cast<time_t>(-1))
				m_entries.erase(entry.first);
			else if (entry.second > 0)
				m_entries[entry.first] = entry.second;
		}
		RebuildOldestIndex();
		m_dirty = wasDirty;
		m_imports.erase(staged);
		error = "Could not save the imported history.";
		return false;
	}
	m_imports.erase(staged);
	++m_revision;
	m_committedImports[id] = now + std::chrono::minutes(10);
	return true;
}

void CCompletedDownloadHistory::AbortImport(const std::string &id)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_imports.erase(id);
}

bool CCompletedDownloadHistory::ImportJson(const std::string &json, bool merge, std::string &error)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (json.size() > kMaximumJsonBytes) {
		error = "History file exceeds the 64 MiB portable transfer limit.";
		return false;
	}
	picojson::value root;
	const std::string parseError = picojson::parse(root, json);
	if (!parseError.empty() || !root.is<picojson::object>()) {
		error = "History file is not valid JSON.";
		return false;
	}
	const picojson::object &object = root.get<picojson::object>();
	const auto format = object.find("format");
	const auto version = object.find("version");
	const auto entries = object.find("entries");
	if (format == object.end() || !format->second.is<std::string>() ||
		format->second.get<std::string>() != "amule.completed-download-history" ||
		version == object.end() || !version->second.is<double>() ||
		version->second.get<double>() != 1.0 || entries == object.end() ||
		!entries->second.is<picojson::array>()) {
		error = "History file format or version is not supported.";
		return false;
	}
	const picojson::array &array = entries->second.get<picojson::array>();
	if (array.size() > kMaximumRecords) {
		error = "History file contains too many records.";
		return false;
	}
	std::map<CMD4Hash, time_t> imported;
	for (const auto &value : array) {
		if (!value.is<picojson::object>()) {
			error = "History file contains an invalid entry.";
			return false;
		}
		const picojson::object &item = value.get<picojson::object>();
		const auto hashValue = item.find("hash");
		const auto timeValue = item.find("completed_at");
		if (hashValue == item.end() || !hashValue->second.is<std::string>() ||
			timeValue == item.end() || !timeValue->second.is<double>()) {
			error = "History file entry is missing its hash or completion time.";
			return false;
		}
		const double seconds = timeValue->second.get<double>();
		CMD4Hash hash;
		if (!hash.Decode(hashValue->second.get<std::string>()) || hash.IsEmpty() || seconds < 1 ||
			seconds > static_cast<double>(std::numeric_limits<time_t>::max())) {
			error = "History file contains an invalid hash or completion time.";
			return false;
		}
		const time_t completedAt = static_cast<time_t>(seconds);
		if (static_cast<double>(completedAt) != seconds) {
			error = "History file completion times must be whole Unix seconds.";
			return false;
		}
		auto inserted = imported.emplace(hash, completedAt);
		if (!inserted.second && completedAt > inserted.first->second)
			inserted.first->second = completedAt;
	}
	std::map<CMD4Hash, time_t> previous;
	const bool wasDirty = m_dirty;
	if (merge) {
		previous = m_entries;
		size_t additions = 0;
		for (const auto &entry : imported) {
			if (m_entries.find(entry.first) == m_entries.end())
				++additions;
		}
		if (additions > kMaximumRecords - std::min<size_t>(m_entries.size(), kMaximumRecords)) {
			error = "The merged history would exceed the record limit.";
			return false;
		}
		for (const auto &entry : imported) {
			auto current = m_entries.emplace(entry.first, entry.second);
			if (!current.second && entry.second > current.first->second)
				current.first->second = entry.second;
		}
	} else {
		previous.swap(m_entries);
		m_entries.swap(imported);
	}
	m_retentionDays = thePrefs::GetCompletedHistoryRetentionDays();
	Prune();
	RebuildOldestIndex();
	m_dirty = true;
	if (!Save()) {
		m_entries.swap(previous);
		RebuildOldestIndex();
		m_dirty = wasDirty;
		error = "Could not save the imported history.";
		return false;
	}
	++m_revision;
	return true;
}
