// Copyright (c) 2009 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/history/history_database.h"

#include <algorithm>
#include <set>
#include <string>

#include "base/file_util.h"
#include "base/histogram.h"
#include "base/rand_util.h"
#include "base/string_util.h"
#include "chrome/common/sqlite_utils.h"

using base::Time;

namespace history {

namespace {

// Current version number. We write databases at the "current" version number,
// but any previous version that can read the "compatible" one can make do with
// or database without *too* many bad effects.
static const int kCurrentVersionNumber = 17;
static const int kCompatibleVersionNumber = 16;
static const char kEarlyExpirationThresholdKey[] = "early_expiration_threshold";

void ComputeDatabaseMetrics(const FilePath& history_name, sqlite3* db) {
  if (base::RandInt(1, 100) != 50)
    return;  // Only do this computation sometimes since it can be expensive.

  int64 file_size = 0;
  if (!file_util::GetFileSize(history_name, &file_size))
    return;
  int file_mb = static_cast<int>(file_size / (1024 * 1024));
  UMA_HISTOGRAM_MEMORY_MB("History.DatabaseFileMB", file_mb);

  SQLStatement url_count;
  if (url_count.prepare(db, "SELECT count(*) FROM urls") != SQLITE_OK ||
      url_count.step() != SQLITE_ROW)
    return;
  UMA_HISTOGRAM_COUNTS("History.URLTableCount", url_count.column_int(0));

  SQLStatement visit_count;
  if (visit_count.prepare(db, "SELECT count(*) FROM visits") != SQLITE_OK ||
      visit_count.step() != SQLITE_ROW)
    return;
  UMA_HISTOGRAM_COUNTS("History.VisitTableCount", visit_count.column_int(0));
}

}  // namespace

HistoryDatabase::HistoryDatabase()
    : transaction_nesting_(0),
      db_(NULL),
      statement_cache_(NULL),
      needs_version_17_migration_(false) {
}

HistoryDatabase::~HistoryDatabase() {
}

InitStatus HistoryDatabase::Init(const FilePath& history_name,
                                 const FilePath& bookmarks_path) {
  // OpenSqliteDb uses the narrow version of open, indicating to sqlite that we
  // want the database to be in UTF-8 if it doesn't already exist.
  DCHECK(!db_) << "Already initialized!";
  if (OpenSqliteDb(history_name, &db_) != SQLITE_OK)
    return INIT_FAILURE;
  statement_cache_ = new SqliteStatementCache;
  DBCloseScoper scoper(&db_, &statement_cache_);

  // Set the database page size to something a little larger to give us
  // better performance (we're typically seek rather than bandwidth limited).
  // This only has an effect before any tables have been created, otherwise
  // this is a NOP. Must be a power of 2 and a max of 8192.
  sqlite3_exec(db_, "PRAGMA page_size=4096", NULL, NULL, NULL);

  // Increase the cache size. The page size, plus a little extra, times this
  // value, tells us how much memory the cache will use maximum.
  // 6000 * 4MB = 24MB
  // TODO(brettw) scale this value to the amount of available memory.
  sqlite3_exec(db_, "PRAGMA cache_size=6000", NULL, NULL, NULL);

  // Wrap the rest of init in a tranaction. This will prevent the database from
  // getting corrupted if we crash in the middle of initialization or migration.
  TransactionScoper transaction(this);

  // Make sure the statement cache is properly initialized.
  statement_cache_->set_db(db_);

  // Prime the cache.
  MetaTableHelper::PrimeCache(std::string(), db_);

  // Create the tables and indices.
  // NOTE: If you add something here, also add it to
  //       RecreateAllButStarAndURLTables.
  if (!meta_table_.Init(std::string(), kCurrentVersionNumber,
                        kCompatibleVersionNumber, db_))
    return INIT_FAILURE;
  if (!CreateURLTable(false) || !InitVisitTable() ||
      !InitKeywordSearchTermsTable() || !InitDownloadTable() ||
      !InitSegmentTables())
    return INIT_FAILURE;
  CreateMainURLIndex();
  CreateSupplimentaryURLIndices();

  // Version check.
  InitStatus version_status = EnsureCurrentVersion(bookmarks_path);
  if (version_status != INIT_OK)
    return version_status;

  // Succeeded: keep the DB open by detaching the auto-closer.
  scoper.Detach();
  db_closer_.Attach(&db_, &statement_cache_);
  ComputeDatabaseMetrics(history_name, db_);
  return INIT_OK;
}

void HistoryDatabase::BeginExclusiveMode() {
  sqlite3_exec(db_, "PRAGMA locking_mode=EXCLUSIVE", NULL, NULL, NULL);
}

// static
int HistoryDatabase::GetCurrentVersion() {
  return kCurrentVersionNumber;
}

void HistoryDatabase::BeginTransaction() {
  DCHECK(db_);
  if (transaction_nesting_ == 0) {
    int rv = sqlite3_exec(db_, "BEGIN TRANSACTION", NULL, NULL, NULL);
    DCHECK(rv == SQLITE_OK) << "Failed to begin transaction";
  }
  transaction_nesting_++;
}

void HistoryDatabase::CommitTransaction() {
  DCHECK(db_);
  DCHECK_GT(transaction_nesting_, 0) << "Committing too many transactions";
  transaction_nesting_--;
  if (transaction_nesting_ == 0) {
    int rv = sqlite3_exec(db_, "COMMIT", NULL, NULL, NULL);
    DCHECK(rv == SQLITE_OK) << "Failed to commit transaction";
  }
}

bool HistoryDatabase::RecreateAllTablesButURL() {
  if (!DropVisitTable())
    return false;
  if (!InitVisitTable())
    return false;

  if (!DropKeywordSearchTermsTable())
    return false;
  if (!InitKeywordSearchTermsTable())
    return false;

  if (!DropSegmentTables())
    return false;
  if (!InitSegmentTables())
    return false;

  // We also add the supplementary URL indices at this point. This index is
  // over parts of the URL table that weren't automatically created when the
  // temporary URL table was
  CreateSupplimentaryURLIndices();
  return true;
}

void HistoryDatabase::Vacuum() {
  DCHECK_EQ(0, transaction_nesting_) <<
      "Can not have a transaction when vacuuming.";
  sqlite3_exec(db_, "VACUUM", NULL, NULL, NULL);
}

bool HistoryDatabase::SetSegmentID(VisitID visit_id, SegmentID segment_id) {
  SQLStatement s;
  if (s.prepare(db_, "UPDATE visits SET segment_id = ? WHERE id = ?") !=
      SQLITE_OK) {
    NOTREACHED();
    return false;
  }
  s.bind_int64(0, segment_id);
  s.bind_int64(1, visit_id);
  return s.step() == SQLITE_DONE;
}

SegmentID HistoryDatabase::GetSegmentID(VisitID visit_id) {
  SQLStatement s;
  if (s.prepare(db_, "SELECT segment_id FROM visits WHERE id = ?")
      != SQLITE_OK) {
    NOTREACHED();
    return 0;
  }

  s.bind_int64(0, visit_id);
  if (s.step() == SQLITE_ROW) {
    if (s.column_type(0) == SQLITE_NULL)
      return 0;
    else
      return s.column_int64(0);
  }
  return 0;
}

Time HistoryDatabase::GetEarlyExpirationThreshold() {
  if (!cached_early_expiration_threshold_.is_null())
    return cached_early_expiration_threshold_;

  int64 threshold;
  if (!meta_table_.GetValue(kEarlyExpirationThresholdKey, &threshold)) {
    // Set to a very early non-zero time, so it's before all history, but not
    // zero to avoid re-retrieval.
    threshold = 1L;
  }

  cached_early_expiration_threshold_ = Time::FromInternalValue(threshold);
  return cached_early_expiration_threshold_;
}

void HistoryDatabase::UpdateEarlyExpirationThreshold(Time threshold) {
  meta_table_.SetValue(kEarlyExpirationThresholdKey,
                       threshold.ToInternalValue());
  cached_early_expiration_threshold_ = threshold;
}

sqlite3* HistoryDatabase::GetDB() {
  return db_;
}

SqliteStatementCache& HistoryDatabase::GetStatementCache() {
  return *statement_cache_;
}

// Migration -------------------------------------------------------------------

InitStatus HistoryDatabase::EnsureCurrentVersion(
    const FilePath& tmp_bookmarks_path) {
  // We can't read databases newer than we were designed for.
  if (meta_table_.GetCompatibleVersionNumber() > kCurrentVersionNumber) {
    LOG(WARNING) << "History database is too new.";
    return INIT_TOO_NEW;
  }

  // NOTICE: If you are changing structures for things shared with the archived
  // history file like URLs, visits, or downloads, that will need migration as
  // well. Instead of putting such migration code in this class, it should be
  // in the corresponding file (url_database.cc, etc.) and called from here and
  // from the archived_database.cc.

  int cur_version = meta_table_.GetVersionNumber();

  // Put migration code here

  if (cur_version == 15) {
    if (!MigrateBookmarksToFile(tmp_bookmarks_path) ||
        !DropStarredIDFromURLs()) {
      LOG(WARNING) << "Unable to update history database to version 16.";
      return INIT_FAILURE;
    }
    ++cur_version;
    meta_table_.SetVersionNumber(cur_version);
    meta_table_.SetCompatibleVersionNumber(
        std::min(cur_version, kCompatibleVersionNumber));
  }

  if (cur_version == 16) {
#if !defined(OS_WIN)
    // In this version we bring the time format on Mac & Linux in sync with the
    // Windows version so that profiles can be moved between computers.
    MigrateTimeEpoch();
#endif
    // On all platforms we bump the version number, so on Windows this
    // migration is a NOP. We keep the compatible version at 16 since things
    // will basically still work, just history will be in the future if an
    // old version reads it.
    ++cur_version;
    meta_table_.SetVersionNumber(cur_version);
  }

  // When the version is too old, we just try to continue anyway, there should
  // not be a released product that makes a database too old for us to handle.
  LOG_IF(WARNING, cur_version < kCurrentVersionNumber) <<
      "History database version " << cur_version << " is too old to handle.";

  return INIT_OK;
}

#if !defined(OS_WIN)
void HistoryDatabase::MigrateTimeEpoch() {
  // Update all the times in the URLs and visits table in the main database.
  // For visits, clear the indexed flag since we'll delete the FTS databases in
  // the next step.
  sqlite3_exec(GetDB(),
      "UPDATE urls "
      "SET last_visit_time = last_visit_time + 11644473600000000 "
      "WHERE id IN (SELECT id FROM urls WHERE last_visit_time > 0);",
      NULL, NULL, NULL);
  sqlite3_exec(GetDB(),
      "UPDATE visits "
      "SET visit_time = visit_time + 11644473600000000, is_indexed = 0 "
      "WHERE id IN (SELECT id FROM visits WHERE visit_time > 0);",
      NULL, NULL, NULL);
  sqlite3_exec(GetDB(),
      "UPDATE segment_usage "
      "SET time_slot = time_slot + 11644473600000000 "
      "WHERE id IN (SELECT id FROM segment_usage WHERE time_slot > 0);",
      NULL, NULL, NULL);

  // Erase all the full text index files. These will take a while to update and
  // are less important, so we just blow them away. Same with the archived
  // database.
  needs_version_17_migration_ = true;
}
#endif

}  // namespace history
