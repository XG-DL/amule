// This file is part of the aMule Project.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation; either version 2 of the License, or (at your option)
// any later version.

#ifndef COMPLETEDDOWNLOADHISTORY_H
#define COMPLETEDDOWNLOADHISTORY_H

#include <map>
#include <ctime>
#include <string>
#include <mutex>
#include <chrono>
#include <condition_variable>
#include <thread>

#include "MD4Hash.h"

/** Persistent record of successfully completed downloads, independent of the shared-file list. */
class CCompletedDownloadHistory
{
public:
	CCompletedDownloadHistory();
	~CCompletedDownloadHistory();
	bool Contains(const CMD4Hash &hash) const;
	time_t CompletedAt(const CMD4Hash &hash) const;
	void Record(const CMD4Hash &hash, time_t completedAt = 0);
	void SetRetentionDays(uint32 days);
	uint32 GetRetentionDays() const;
	void Clear();
	bool ExportJson(std::string &json, std::string &error) const;
	bool BeginExport(const std::string &id, size_t &total, uint64 &revision, std::string &error);
	bool ExportJsonBatch(const std::string &id,
		size_t offset,
		size_t limit,
		std::string &json,
		size_t &total,
		std::string &error);
	void EndExport(const std::string &id);
	bool ImportJson(const std::string &json, bool merge, std::string &error);
	bool BeginImport(const std::string &id, bool merge, std::string &error);
	bool ImportJsonBatch(const std::string &id, const std::string &json, std::string &error);
	bool CommitImport(const std::string &id, std::string &error);
	void AbortImport(const std::string &id);
	size_t GetCount() const;

private:
	void Load();
	void RebuildOldestIndex();
	bool Save();
	bool Prune();
	std::map<CMD4Hash, time_t> m_entries;
	std::multimap<time_t, CMD4Hash> m_oldestFirst;
	struct StagedImport
	{
		std::map<CMD4Hash, time_t> entries;
		bool merge = false;
		std::chrono::steady_clock::time_point expires;
	};
	std::map<std::string, StagedImport> m_imports;
	std::map<std::string, std::chrono::steady_clock::time_point> m_committedImports;
	struct ExportCursor
	{
		std::map<CMD4Hash, time_t>::const_iterator next;
		size_t expectedOffset = 0;
		size_t lastOffset = static_cast<size_t>(-1);
		std::string lastJson;
		uint64 revision = 0;
		std::chrono::steady_clock::time_point expires;
	};
	std::map<std::string, ExportCursor> m_exports;
	uint32 m_retentionDays;
	uint64 m_revision = 1;
	bool m_limitWarningLogged = false;
	bool m_dirty = false;
	bool m_stopping = false;
	std::condition_variable m_flushCondition;
	std::thread m_flushThread;
	mutable std::mutex m_mutex;
};

#endif // COMPLETEDDOWNLOADHISTORY_H
