/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include "logdevice/server/locallogstore/RocksDBLogStoreBase.h"

#include "logdevice/server/locallogstore/RocksDBMemTableRep.h"
#include "logdevice/server/locallogstore/RocksDBSettings.h"
#include "logdevice/server/locallogstore/RocksDBWriter.h"

namespace facebook { namespace logdevice {

using RocksDBKeyFormat::LogSnapshotBlobKey;

const char* const RocksDBLogStoreBase::OLD_SCHEMA_VERSION_KEY =
    "schema_version";
const char* const RocksDBLogStoreBase::NEW_SCHEMA_VERSION_KEY =
    ".schema_version";

RocksDBLogStoreBase::RocksDBLogStoreBase(uint32_t shard_idx,
                                         const std::string& path,
                                         RocksDBLogStoreConfig rocksdb_config,
                                         StatsHolder* stats_holder)
    : shard_idx_(shard_idx),
      db_path_(path),
      writer_(new RocksDBWriter(this, *rocksdb_config.getRocksDBSettings())),
      stats_(stats_holder),
      statistics_(rocksdb_config.options_.statistics),
      rocksdb_config_(std::move(rocksdb_config)) {
  // Per RocksDB instance option overrides.
  registerListener(rocksdb_config_.options_);
  installMemTableRep();
}

RocksDBLogStoreBase::~RocksDBLogStoreBase() {
  if (fail_safe_mode_.load()) {
    PER_SHARD_STAT_DECR(getStatsHolder(), failed_safe_log_stores, shard_idx_);
  }

  // Clears the last reference to all column family handles in the map
  // by copying it to a vector and then clearing it. This is required to
  // satisfy TSAN which otherwise will complain about lock-order-inversion
  // There are two locks that are acquired
  // 1/ cf_accessor_ 's lock
  // 2/ RocksDB internal lock when flush is called
  // Destructor thread T1 acquires 1 followed by 2 (because destroying cf
  // calls flush)
  // Other flush thread T2 can acquire 2 followed by 1 (as part of callback to
  // markMemtableRepImmutable)
  // By moving the handles out of map and then destroying, we are preventing
  // destructor thread from acquiring 2 while holding 1
  std::vector<RocksDBCFPtr> cf_to_delete;
  cf_accessor_.withWLock([&](auto& locked_accessor) {
    for (auto& kv : locked_accessor) {
      cf_to_delete.push_back(std::move(kv.second));
      kv.second.reset();
    }
  });
  cf_to_delete.clear();

  // Destruction of db_ could trigger a flush of dirty memtable
  // when WAL is not used for writes. Such a flush, could in turn
  // callback into this class if we have regiestered event listeners.
  // Hence we should not depend on the default order of destruction
  // but rather destroy here so that callback does not get called on
  // a semi-destroyed object
  db_.reset();
}

RocksDBIterator
RocksDBLogStoreBase::newIterator(rocksdb::ReadOptions ropt,
                                 rocksdb::ColumnFamilyHandle* cf) const {
  std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(ropt, cf));
  ld_check(it != nullptr);
  return RocksDBIterator(std::move(it), ropt, this);
}

int RocksDBLogStoreBase::sync(Durability durability) {
  ld_check(!getSettings()->read_only);
  if (durability <= Durability::ASYNC_WRITE && syncWAL() != 0) {
    return -1;
  }
  if (durability <= Durability::MEMORY) {
    return flushAllMemtables();
  }
  return 0;
}

int RocksDBLogStoreBase::flushAllMemtables(bool wait) {
  // Assume default column family only.
  auto options = rocksdb::FlushOptions();
  options.wait = wait;
  rocksdb::Status status = db_->Flush(options);
  if (!status.ok()) {
    enterFailSafeIfFailed(status, "Flush()");
    err = E::LOCAL_LOG_STORE_WRITE;
    return -1;
  }
  return 0;
}

int RocksDBLogStoreBase::isCFEmpty(rocksdb::ColumnFamilyHandle* cf) const {
  RocksDBIterator it = newIterator(getDefaultReadOptions(), cf);
  it.Seek(rocksdb::Slice("", 0));
  // schema_version isn't visible from outside of this LocalLogStore class,
  // so it doesn't count as non-emptiness.
  if (it.status().ok() && it.Valid() &&
      (it.key().compare(OLD_SCHEMA_VERSION_KEY) == 0 ||
       it.key().compare(NEW_SCHEMA_VERSION_KEY) == 0)) {
    it.Next();
  }
  if (!it.status().ok()) {
    ld_error("Error checking if database is empty: %s",
             it.status().ToString().c_str());
    return -1;
  }
  return !it.Valid();
}

void RocksDBLogStoreBase::registerListener(rocksdb::Options& options) {
  options.listeners.push_back(std::make_shared<Listener>(this));
}

void RocksDBLogStoreBase::installMemTableRep() {
  auto create_memtable_factory = [this]() {
    mtr_factory_ = std::make_shared<RocksDBMemTableRepFactory>(
        this,
        std::make_unique<rocksdb::SkipListFactory>(
            getSettings()->skip_list_lookahead));
  };

  if (!rocksdb_config_.options_.memtable_factory) {
    create_memtable_factory();
  } else {
    // In tests someone might want to override the memtable factory
    // implementation. Allowing to do that.
    mtr_factory_ = std::dynamic_pointer_cast<RocksDBMemTableRepFactory>(
        rocksdb_config_.options_.memtable_factory);
    if (!mtr_factory_) {
      if (rocksdb_config_.options_.memtable_factory != nullptr) {
        ld_warning(
            "MemTable Factory needs to inherit from RocksDBMemTableRepFactory, "
            "ignoring value passed in and creating a new factory of known "
            "type.");
      }
      create_memtable_factory();
    } else {
      mtr_factory_->setStore(this);
    }
  }

  rocksdb_config_.options_.memtable_factory =
      rocksdb_config_.metadata_options_.memtable_factory = mtr_factory_;
}

FlushToken RocksDBLogStoreBase::maxFlushToken() const {
  return mtr_factory_->maxFlushToken();
}

FlushToken RocksDBLogStoreBase::flushedUpThrough() const {
  return mtr_factory_->flushedUpThrough();
}

SteadyTimestamp RocksDBLogStoreBase::oldestUnflushedDataTimestamp() const {
  return mtr_factory_->oldestUnflushedDataTimestamp();
}

bool RocksDBLogStoreBase::isFlushInProgress() {
#ifdef LOGDEVICED_ROCKSDB_HAS_GET_AGGREGATED_INT_PROPERTY
  size_t res;
  // Note that kNumImmutableMemTable, despite the name, counts only
  // *non-flushed* immutable memtables (i.e. doesn't count pinned ones).
  // RocksDB-side stall happens when there are at least two
  // (max_write_buffer_number) non-flushed immutable memtables in some column
  // family.
  if (!db_->GetAggregatedIntProperty(
          rocksdb::DB::Properties::kNumImmutableMemTable, &res)) {
    RATELIMIT_WARNING(std::chrono::seconds(10),
                      2,
                      "Failed to get kNumImmutableMemTable property.");
    return false;
  }
  return res > 0;
#else
  return false;
#endif
}

void RocksDBLogStoreBase::adviseUnstallingLowPriWrites(
    bool dont_stall_anymore) {
  if (dont_stall_anymore) {
    // Shutdown thread can race with storage thread to get cv_mutex,
    // if shutdown thread wins the race we need to make sure that storage thread
    // does not end up waiting after the mutex is dropped by shutdown
    // thread. To avoid this, update dont_stall_untill to max before notifying
    // all.
    dont_stall_until_.store(std::chrono::steady_clock::duration::max());
    std::unique_lock<std::mutex> cv_lock(stall_cv_mutex_);
    stall_cv_.notify_all();
  } else {
    stall_cv_.notify_all();
  }
}

void RocksDBLogStoreBase::stallLowPriWrite() {
  auto cache_says_no_stall = [&] {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return now < dont_stall_until_.load();
  };

  if (cache_says_no_stall()) {
    // Fast path: shouldStallLowPriWrites() returned false recently.
    return;
  }

  std::unique_lock<std::mutex> lock(stall_mutex_);

  auto stats_time = std::chrono::steady_clock::now();

  while (!cache_says_no_stall()) {
    if (!shouldStallLowPriWrites()) {
      auto now = std::chrono::steady_clock::now().time_since_epoch();
      dont_stall_until_.store(now + getSettings()->stall_cache_ttl_);
      break;
    }

    // Stall. Since we're still holding stall_mutex_, other threads will be
    // stalled without doing more calls to isFlushInProgress().

    // Need separate mutex here because we want stall_mutex_ to stay locked.
    std::unique_lock<std::mutex> cv_lock(stall_cv_mutex_);
    // Skipping stall if cv was just signalled by shutdown code. We are
    // allowing this write to progress without stalling.
    if (!cache_says_no_stall()) {
      stall_cv_.wait_for(cv_lock, getSettings()->stall_cache_ttl_);
    }

    // Bump stat.
    auto t = std::chrono::steady_clock::now();
    PER_SHARD_STAT_ADD(
        stats_,
        write_stall_microsec,
        shard_idx_,
        std::chrono::duration_cast<std::chrono::microseconds>(t - stats_time)
            .count());
    stats_time = t;
  }

  lock.unlock();
  stall_cv_.notify_all();
}

int RocksDBLogStoreBase::readAllLogSnapshotBlobsImpl(
    LogSnapshotBlobType snapshots_type,
    LogSnapshotBlobCallback callback,
    rocksdb::ColumnFamilyHandle* snapshots_cf) {
  ld_check(snapshots_cf);

  auto it = newIterator(getDefaultReadOptions(), snapshots_cf);
  LogSnapshotBlobKey seek_target(snapshots_type, LOGID_INVALID);
  it.Seek(rocksdb::Slice(
      reinterpret_cast<const char*>(&seek_target), sizeof(seek_target)));
  for (; it.status().ok() && it.Valid(); it.Next()) {
    auto key_raw = it.key();
    if (!LogSnapshotBlobKey::valid(
            snapshots_type, key_raw.data(), key_raw.size())) {
      break;
    }

    auto logid = LogSnapshotBlobKey::getLogID(key_raw.data());
    Slice blob = Slice(it.value().data(), it.value().size());
    int rv = callback(logid, blob);
    if (rv != 0) {
      return -1;
    }
  }

  return it.status().ok() ? 0 : -1;
}

rocksdb::ReadOptions RocksDBLogStoreBase::translateReadOptions(
    const LocalLogStore::ReadOptions& opts,
    bool single_log,
    rocksdb::Slice* upper_bound) {
  rocksdb::ReadOptions rocks_options = single_log
      ? RocksDBLogStoreBase::getReadOptionsSinglePrefix()
      : RocksDBLogStoreBase::getDefaultReadOptions();

  rocks_options.fill_cache = opts.fill_cache;
  rocks_options.read_tier =
      opts.allow_blocking_io ? rocksdb::kReadAllTier : rocksdb::kBlockCacheTier;

  // Tailing iterator isn't tied to a snapshot of the database, so using it
  // allows us to cache and reuse the iterator.
  rocks_options.tailing = opts.tailing;

  if (upper_bound != nullptr && !upper_bound->empty()) {
    // Since this iterator is only used to read data for a given log, setting
    // iterate_upper_bound allows RocksDB to release some resources when child
    // iterators move past all the records for this log.
    rocks_options.iterate_upper_bound = upper_bound;
  }

  return rocks_options;
}

int RocksDBLogStoreBase::syncWAL() {
  rocksdb::Status status = writer_->syncWAL();
  if (!status.ok()) {
    err = E::LOCAL_LOG_STORE_WRITE;
    return -1;
  }
  return 0;
}

FlushToken RocksDBLogStoreBase::maxWALSyncToken() const {
  return writer_->maxWALSyncToken();
}

FlushToken RocksDBLogStoreBase::walSyncedUpThrough() const {
  return writer_->walSyncedUpThrough();
}

int RocksDBLogStoreBase::readLogMetadata(logid_t log_id,
                                         LogMetadata* metadata) {
  return writer_->readLogMetadata(log_id, metadata, getMetadataCFHandle());
}
int RocksDBLogStoreBase::readStoreMetadata(StoreMetadata* metadata) {
  return writer_->readStoreMetadata(metadata, getMetadataCFHandle());
}
int RocksDBLogStoreBase::readPerEpochLogMetadata(logid_t log_id,
                                                 epoch_t epoch,
                                                 PerEpochLogMetadata* metadata,
                                                 bool find_last_available,
                                                 bool allow_blocking_io) const {
  return writer_->readPerEpochLogMetadata(log_id,
                                          epoch,
                                          metadata,
                                          getMetadataCFHandle(),
                                          find_last_available,
                                          allow_blocking_io);
}

int RocksDBLogStoreBase::writeLogMetadata(logid_t log_id,
                                          const LogMetadata& metadata,
                                          const WriteOptions& write_options) {
  return writer_->writeLogMetadata(
      log_id, metadata, write_options, getMetadataCFHandle());
}
int RocksDBLogStoreBase::writeStoreMetadata(const StoreMetadata& metadata,
                                            const WriteOptions& write_options) {
  return writer_->writeStoreMetadata(
      metadata, write_options, getMetadataCFHandle());
}

int RocksDBLogStoreBase::updateLogMetadata(logid_t log_id,
                                           ComparableLogMetadata& metadata,
                                           const WriteOptions& write_options) {
  return writer_->updateLogMetadata(
      log_id, metadata, write_options, getMetadataCFHandle());
}
int RocksDBLogStoreBase::updatePerEpochLogMetadata(
    logid_t log_id,
    epoch_t epoch,
    PerEpochLogMetadata& metadata,
    LocalLogStore::SealPreemption seal_preempt,
    const WriteOptions& write_options) {
  return writer_->updatePerEpochLogMetadata(log_id,
                                            epoch,
                                            metadata,
                                            seal_preempt,
                                            write_options,
                                            getMetadataCFHandle());
}

int RocksDBLogStoreBase::deleteStoreMetadata(
    const StoreMetadataType type,
    const WriteOptions& write_options) {
  return writer_->deleteStoreMetadata(
      type, write_options, getMetadataCFHandle());
}
int RocksDBLogStoreBase::deleteLogMetadata(logid_t first_log_id,
                                           logid_t last_log_id,
                                           const LogMetadataType type,
                                           const WriteOptions& write_options) {
  return writer_->deleteLogMetadata(
      first_log_id, last_log_id, type, write_options, getMetadataCFHandle());
}
int RocksDBLogStoreBase::deletePerEpochLogMetadata(
    logid_t log_id,
    epoch_t epoch,
    const PerEpochLogMetadataType type,
    const WriteOptions& write_options) {
  return writer_->deletePerEpochLogMetadata(
      log_id, epoch, type, write_options, getMetadataCFHandle());
}

RocksDBCFPtr
RocksDBLogStoreBase::getColumnFamilyPtr(uint32_t column_family_id) {
  RocksDBCFPtr cf_ptr;
  cf_accessor_.withRLock([&](auto& locked_accessor) {
    const auto& iter = locked_accessor.find(column_family_id);
    if (iter != locked_accessor.end()) {
      cf_ptr = iter->second;
    }
  });

  return cf_ptr;
}

}} // namespace facebook::logdevice
