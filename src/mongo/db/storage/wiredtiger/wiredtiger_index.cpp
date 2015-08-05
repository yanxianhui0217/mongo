// wiredtiger_index.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_index.h"

#include <set>

#include "mongo/base/checked_cast.h"
#include "mongo/db/json.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_customization_hooks.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/db/storage_options.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

#define TRACING_ENABLED 0

#if TRACING_ENABLED
#define TRACE_CURSOR log() << "WT index (" << (const void*)&_idx << ") "
#define TRACE_INDEX log() << "WT index (" << (const void*) this << ") "
#else
#define TRACE_CURSOR \
    if (0)           \
    log()
#define TRACE_INDEX \
    if (0)          \
    log()
#endif

namespace mongo {
namespace {

using std::string;
using std::vector;

static const int TempKeyMaxSize = 1024;  // this goes away with SERVER-3372

static const WiredTigerItem emptyItem(NULL, 0);

static const int kMinimumIndexVersion = 6;
static const int kCurrentIndexVersion = 6;  // New indexes use this by default.
static const int kMaximumIndexVersion = 6;
static_assert(kCurrentIndexVersion >= kMinimumIndexVersion,
              "kCurrentIndexVersion >= kMinimumIndexVersion");
static_assert(kCurrentIndexVersion <= kMaximumIndexVersion,
              "kCurrentIndexVersion <= kMaximumIndexVersion");

bool hasFieldNames(const BSONObj& obj) {
    BSONForEach(e, obj) {
        if (e.fieldName()[0])
            return true;
    }
    return false;
}

BSONObj stripFieldNames(const BSONObj& query) {
    if (!hasFieldNames(query))
        return query;

    BSONObjBuilder bb;
    BSONForEach(e, query) {
        bb.appendAs(e, StringData());
    }
    return bb.obj();
}

Status checkKeySize(const BSONObj& key) {
    if (key.objsize() >= TempKeyMaxSize) {
        string msg = mongoutils::str::stream()
            << "WiredTigerIndex::insert: key too large to index, failing " << ' ' << key.objsize()
            << ' ' << key;
        return Status(ErrorCodes::KeyTooLong, msg);
    }
    return Status::OK();
}

}  // namespace

Status WiredTigerIndex::dupKeyError(const BSONObj& key) {
    StringBuilder sb;
    sb << "E11000 duplicate key error";
    sb << " collection: " << _collectionNamespace;
    sb << " index: " << _indexName;
    sb << " dup key: " << key;
    return Status(ErrorCodes::DuplicateKey, sb.str());
}

// static
StatusWith<std::string> WiredTigerIndex::parseIndexOptions(const BSONObj& options) {
    StringBuilder ss;
    BSONForEach(elem, options) {
        if (elem.fieldNameStringData() == "configString") {
            Status status = WiredTigerUtil::checkTableCreationOptions(elem);
            if (!status.isOK()) {
                return status;
            }
            ss << elem.valueStringData() << ',';
        } else {
            // Return error on first unrecognized field.
            return StatusWith<std::string>(ErrorCodes::InvalidOptions,
                                           str::stream() << '\'' << elem.fieldNameStringData()
                                                         << '\'' << " is not a supported option.");
        }
    }
    return StatusWith<std::string>(ss.str());
}

// static
StatusWith<std::string> WiredTigerIndex::generateCreateString(const std::string& extraConfig,
                                                              const IndexDescriptor& desc) {
    str::stream ss;

    // Separate out a prefix and suffix in the default string. User configuration will override
    // values in the prefix, but not values in the suffix.  Page sizes are chosen so that index
    // keys (up to 1024 bytes) will not overflow.
    ss << "type=file,internal_page_max=16k,leaf_page_max=16k,";
    ss << "checksum=on,";
    if (wiredTigerGlobalOptions.useIndexPrefixCompression) {
        ss << "prefix_compression=true,";
    }

    ss << "block_compressor=" << wiredTigerGlobalOptions.indexBlockCompressor << ",";
    ss << WiredTigerCustomizationHooks::get(getGlobalServiceContext())
              ->getOpenConfig(desc.parentNS());
    ss << extraConfig;

    // Validate configuration object.
    // Raise an error about unrecognized fields that may be introduced in newer versions of
    // this storage engine.
    // Ensure that 'configString' field is a string. Raise an error if this is not the case.
    BSONElement storageEngineElement = desc.getInfoElement("storageEngine");
    if (storageEngineElement.isABSONObj()) {
        BSONObj storageEngine = storageEngineElement.Obj();
        StatusWith<std::string> parseStatus =
            parseIndexOptions(storageEngine.getObjectField(kWiredTigerEngineName));
        if (!parseStatus.isOK()) {
            return parseStatus;
        }
        if (!parseStatus.getValue().empty()) {
            ss << "," << parseStatus.getValue();
        }
    }

    // WARNING: No user-specified config can appear below this line. These options are required
    // for correct behavior of the server.

    // Indexes need to store the metadata for collation to work as expected.
    ss << ",key_format=u,value_format=u";

    // Index metadata
    ss << ",app_metadata=("
       << "formatVersion=" << kCurrentIndexVersion << ','
       << "infoObj=" << desc.infoObj().jsonString() << "),";

    LOG(3) << "index create string: " << ss.ss.str();
    return StatusWith<std::string>(ss);
}

int WiredTigerIndex::Create(OperationContext* txn,
                            const std::string& uri,
                            const std::string& config) {
    WT_SESSION* s = WiredTigerRecoveryUnit::get(txn)->getSession(txn)->getSession();
    LOG(1) << "create uri: " << uri << " config: " << config;
    return s->create(s, uri.c_str(), config.c_str());
}

WiredTigerIndex::WiredTigerIndex(OperationContext* ctx,
                                 const std::string& uri,
                                 const IndexDescriptor* desc)
    : _ordering(Ordering::make(desc->keyPattern())),
      _uri(uri),
      _tableId(WiredTigerSession::genTableId()),
      _collectionNamespace(desc->parentNS()),
      _indexName(desc->indexName()) {
    Status versionStatus = WiredTigerUtil::checkApplicationMetadataFormatVersion(
        ctx, uri, kMinimumIndexVersion, kMaximumIndexVersion);
    if (!versionStatus.isOK()) {
        fassertFailedWithStatusNoTrace(28579, versionStatus);
    }
}

Status WiredTigerIndex::insert(OperationContext* txn,
                               const BSONObj& key,
                               const RecordId& loc,
                               bool dupsAllowed) {
    invariant(loc.isNormal());
    dassert(!hasFieldNames(key));

    Status s = checkKeySize(key);
    if (!s.isOK())
        return s;

    WiredTigerCursor curwrap(_uri, _tableId, false, txn);
    curwrap.assertInActiveTxn();
    WT_CURSOR* c = curwrap.get();

    return _insert(c, key, loc, dupsAllowed);
}

void WiredTigerIndex::unindex(OperationContext* txn,
                              const BSONObj& key,
                              const RecordId& loc,
                              bool dupsAllowed) {
    invariant(loc.isNormal());
    dassert(!hasFieldNames(key));

    WiredTigerCursor curwrap(_uri, _tableId, false, txn);
    curwrap.assertInActiveTxn();
    WT_CURSOR* c = curwrap.get();
    invariant(c);

    _unindex(c, key, loc, dupsAllowed);
}

void WiredTigerIndex::fullValidate(OperationContext* txn,
                                   bool full,
                                   long long* numKeysOut,
                                   BSONObjBuilder* output) const {
    {
        std::vector<std::string> errors;
        int err = WiredTigerUtil::verifyTable(txn, _uri, output ? &errors : NULL);
        if (err == EBUSY) {
            const char* msg = "verify() returned EBUSY. Not treating as invalid.";
            warning() << msg;
            if (output) {
                if (!errors.empty()) {
                    *output << "errors" << errors;
                }
                *output << "warning" << msg;
            }
        } else if (err) {
            std::string msg = str::stream() << "verify() returned " << wiredtiger_strerror(err)
                                            << ". "
                                            << "This indicates structural damage. "
                                            << "Not examining individual index entries.";
            error() << msg;
            if (output) {
                errors.push_back(msg);
                *output << "errors" << errors;
                *output << "valid" << false;
            }
            return;
        }
    }

    if (output)
        *output << "valid" << true;

    auto cursor = newCursor(txn);
    long long count = 0;
    TRACE_INDEX << " fullValidate";

    const auto requestedInfo = TRACING_ENABLED ? Cursor::kKeyAndLoc : Cursor::kJustExistance;
    for (auto kv = cursor->seek(BSONObj(), true, requestedInfo); kv; kv = cursor->next()) {
        TRACE_INDEX << "\t" << kv->key << ' ' << kv->loc;
        count++;
    }

    if (numKeysOut) {
        *numKeysOut = count;
    }

    // Nothing further to do if 'full' validation is not requested.
    if (!full) {
        return;
    }

    invariant(output);
}

bool WiredTigerIndex::appendCustomStats(OperationContext* txn,
                                        BSONObjBuilder* output,
                                        double scale) const {
    {
        BSONObjBuilder metadata(output->subobjStart("metadata"));
        Status status = WiredTigerUtil::getApplicationMetadata(txn, uri(), &metadata);
        if (!status.isOK()) {
            metadata.append("error", "unable to retrieve metadata");
            metadata.append("code", static_cast<int>(status.code()));
            metadata.append("reason", status.reason());
        }
    }
    std::string type, sourceURI;
    WiredTigerUtil::fetchTypeAndSourceURI(txn, _uri, &type, &sourceURI);
    StatusWith<std::string> metadataResult = WiredTigerUtil::getMetadata(txn, sourceURI);
    StringData creationStringName("creationString");
    if (!metadataResult.isOK()) {
        BSONObjBuilder creationString(output->subobjStart(creationStringName));
        creationString.append("error", "unable to retrieve creation config");
        creationString.append("code", static_cast<int>(metadataResult.getStatus().code()));
        creationString.append("reason", metadataResult.getStatus().reason());
    } else {
        output->append(creationStringName, metadataResult.getValue());
        // Type can be "lsm" or "file"
        output->append("type", type);
    }

    WiredTigerSession* session = WiredTigerRecoveryUnit::get(txn)->getSession(txn);
    WT_SESSION* s = session->getSession();
    Status status =
        WiredTigerUtil::exportTableToBSON(s, "statistics:" + uri(), "statistics=(fast)", output);
    if (!status.isOK()) {
        output->append("error", "unable to retrieve statistics");
        output->append("code", static_cast<int>(status.code()));
        output->append("reason", status.reason());
    }
    return true;
}

Status WiredTigerIndex::dupKeyCheck(OperationContext* txn,
                                    const BSONObj& key,
                                    const RecordId& loc) {
    invariant(!hasFieldNames(key));
    invariant(unique());

    WiredTigerCursor curwrap(_uri, _tableId, false, txn);
    WT_CURSOR* c = curwrap.get();

    if (isDup(c, key, loc))
        return dupKeyError(key);
    return Status::OK();
}

bool WiredTigerIndex::isEmpty(OperationContext* txn) {
    WiredTigerCursor curwrap(_uri, _tableId, false, txn);
    WT_CURSOR* c = curwrap.get();
    if (!c)
        return true;
    int ret = WT_OP_CHECK(c->next(c));
    if (ret == WT_NOTFOUND)
        return true;
    invariantWTOK(ret);
    return false;
}

long long WiredTigerIndex::getSpaceUsedBytes(OperationContext* txn) const {
    WiredTigerSession* session = WiredTigerRecoveryUnit::get(txn)->getSession(txn);
    return static_cast<long long>(WiredTigerUtil::getIdentSize(session->getSession(), _uri));
}

bool WiredTigerIndex::isDup(WT_CURSOR* c, const BSONObj& key, const RecordId& loc) {
    invariant(unique());
    // First check whether the key exists.
    KeyString data(key, _ordering);
    WiredTigerItem item(data.getBuffer(), data.getSize());
    c->set_key(c, item.Get());
    int ret = WT_OP_CHECK(c->search(c));
    if (ret == WT_NOTFOUND) {
        return false;
    }
    invariantWTOK(ret);

    // If the key exists, check if we already have this loc at this key. If so, we don't
    // consider that to be a dup.
    WT_ITEM value;
    invariantWTOK(c->get_value(c, &value));
    BufReader br(value.data, value.size);
    while (br.remaining()) {
        if (KeyString::decodeRecordId(&br) == loc)
            return false;

        KeyString::TypeBits::fromBuffer(&br);  // Just calling this to advance reader.
    }
    return true;
}

Status WiredTigerIndex::initAsEmpty(OperationContext* txn) {
    // No-op
    return Status::OK();
}

/**
 * Base class for WiredTigerIndex bulk builders.
 *
 * Manages the bulk cursor used by bulk builders.
 */
class WiredTigerIndex::BulkBuilder : public SortedDataBuilderInterface {
public:
    BulkBuilder(WiredTigerIndex* idx, OperationContext* txn)
        : _ordering(idx->_ordering),
          _txn(txn),
          _session(WiredTigerRecoveryUnit::get(_txn)->getSessionCache()->getSession()),
          _cursor(openBulkCursor(idx)) {}

    ~BulkBuilder() {
        _cursor->close(_cursor);
        WiredTigerRecoveryUnit::get(_txn)->getSessionCache()->releaseSession(_session);
    }

protected:
    WT_CURSOR* openBulkCursor(WiredTigerIndex* idx) {
        // Open cursors can cause bulk open_cursor to fail with EBUSY.
        // TODO any other cases that could cause EBUSY?
        WiredTigerSession* outerSession = WiredTigerRecoveryUnit::get(_txn)->getSession(_txn);
        outerSession->closeAllCursors();

        // Not using cursor cache since we need to set "bulk".
        WT_CURSOR* cursor;
        // We use our own session to ensure we aren't in a transaction.
        WT_SESSION* session = _session->getSession();
        int err = session->open_cursor(session, idx->uri().c_str(), NULL, "bulk", &cursor);
        if (!err)
            return cursor;

        warning() << "failed to create WiredTiger bulk cursor: " << wiredtiger_strerror(err);
        warning() << "falling back to non-bulk cursor for index " << idx->uri();

        invariantWTOK(session->open_cursor(session, idx->uri().c_str(), NULL, NULL, &cursor));
        return cursor;
    }

    const Ordering _ordering;
    OperationContext* const _txn;
    WiredTigerSession* const _session;
    WT_CURSOR* const _cursor;
};

/**
 * Bulk builds a non-unique index.
 */
class WiredTigerIndex::StandardBulkBuilder : public BulkBuilder {
public:
    StandardBulkBuilder(WiredTigerIndex* idx, OperationContext* txn)
        : BulkBuilder(idx, txn), _idx(idx) {}

    Status addKey(const BSONObj& key, const RecordId& loc) {
        {
            const Status s = checkKeySize(key);
            if (!s.isOK())
                return s;
        }

        KeyString data(key, _idx->_ordering, loc);

        // Can't use WiredTigerCursor since we aren't using the cache.
        WiredTigerItem item(data.getBuffer(), data.getSize());
        _cursor->set_key(_cursor, item.Get());

        WiredTigerItem valueItem = data.getTypeBits().isAllZeros()
            ? emptyItem
            : WiredTigerItem(data.getTypeBits().getBuffer(), data.getTypeBits().getSize());

        _cursor->set_value(_cursor, valueItem.Get());

        invariantWTOK(_cursor->insert(_cursor));

        return Status::OK();
    }

    void commit(bool mayInterrupt) {
        // TODO do we still need this?
        // this is bizarre, but required as part of the contract
        WriteUnitOfWork uow(_txn);
        uow.commit();
    }

private:
    WiredTigerIndex* _idx;
};

/**
 * Bulk builds a unique index.
 *
 * In order to support unique indexes in dupsAllowed mode this class only does an actual insert
 * after it sees a key after the one we are trying to insert. This allows us to gather up all
 * duplicate locs and insert them all together. This is necessary since bulk cursors can only
 * append data.
 */
class WiredTigerIndex::UniqueBulkBuilder : public BulkBuilder {
public:
    UniqueBulkBuilder(WiredTigerIndex* idx, OperationContext* txn, bool dupsAllowed)
        : BulkBuilder(idx, txn), _idx(idx), _dupsAllowed(dupsAllowed) {}

    Status addKey(const BSONObj& newKey, const RecordId& loc) {
        {
            const Status s = checkKeySize(newKey);
            if (!s.isOK())
                return s;
        }

        const int cmp = newKey.woCompare(_key, _ordering);
        if (cmp != 0) {
            if (!_key.isEmpty()) {   // _key.isEmpty() is only true on the first call to addKey().
                invariant(cmp > 0);  // newKey must be > the last key
                // We are done with dups of the last key so we can insert it now.
                doInsert();
            }
            invariant(_records.empty());
        } else {
            // Dup found!
            if (!_dupsAllowed) {
                return _idx->dupKeyError(newKey);
            }

            // If we get here, we are in the weird mode where dups are allowed on a unique
            // index, so add ourselves to the list of duplicate locs. This also replaces the
            // _key which is correct since any dups seen later are likely to be newer.
        }

        _key = newKey.getOwned();
        _keyString.resetToKey(_key, _idx->ordering());
        _records.push_back(std::make_pair(loc, _keyString.getTypeBits()));

        return Status::OK();
    }

    void commit(bool mayInterrupt) {
        WriteUnitOfWork uow(_txn);
        if (!_records.empty()) {
            // This handles inserting the last unique key.
            doInsert();
        }
        uow.commit();
    }

private:
    void doInsert() {
        invariant(!_records.empty());

        KeyString value;
        for (size_t i = 0; i < _records.size(); i++) {
            value.appendRecordId(_records[i].first);
            // When there is only one record, we can omit AllZeros TypeBits. Otherwise they need
            // to be included.
            if (!(_records[i].second.isAllZeros() && _records.size() == 1)) {
                value.appendTypeBits(_records[i].second);
            }
        }

        WiredTigerItem keyItem(_keyString.getBuffer(), _keyString.getSize());
        WiredTigerItem valueItem(value.getBuffer(), value.getSize());

        _cursor->set_key(_cursor, keyItem.Get());
        _cursor->set_value(_cursor, valueItem.Get());

        invariantWTOK(_cursor->insert(_cursor));

        _records.clear();
    }

    WiredTigerIndex* _idx;
    const bool _dupsAllowed;
    BSONObj _key;
    KeyString _keyString;
    std::vector<std::pair<RecordId, KeyString::TypeBits>> _records;
};

namespace {

/**
 * Implements the basic WT_CURSOR functionality used by both unique and standard indexes.
 */
class WiredTigerIndexCursorBase : public SortedDataInterface::Cursor {
public:
    WiredTigerIndexCursorBase(const WiredTigerIndex& idx, OperationContext* txn, bool forward)
        : _txn(txn), _idx(idx), _forward(forward) {
        _cursor.emplace(_idx.uri(), _idx.tableId(), false, _txn);
    }
    boost::optional<IndexKeyEntry> next(RequestedInfo parts) override {
        // Advance on a cursor at the end is a no-op
        if (_eof)
            return {};

        if (!_lastMoveWasRestore)
            advanceWTCursor();
        updatePosition();
        return curr(parts);
    }

    void setEndPosition(const BSONObj& key, bool inclusive) override {
        TRACE_CURSOR << "setEndPosition inclusive: " << inclusive << ' ' << key;
        if (key.isEmpty()) {
            // This means scan to end of index.
            _endPosition.reset();
            return;
        }

        // NOTE: this uses the opposite rules as a normal seek because a forward scan should
        // end after the key if inclusive and before if exclusive.
        const auto discriminator =
            _forward == inclusive ? KeyString::kExclusiveAfter : KeyString::kExclusiveBefore;
        _endPosition = stdx::make_unique<KeyString>();
        _endPosition->resetToKey(stripFieldNames(key), _idx.ordering(), discriminator);
    }

    boost::optional<IndexKeyEntry> seek(const BSONObj& key,
                                        bool inclusive,
                                        RequestedInfo parts) override {
        const BSONObj finalKey = stripFieldNames(key);
        const auto discriminator =
            _forward == inclusive ? KeyString::kExclusiveBefore : KeyString::kExclusiveAfter;

        // By using a discriminator other than kInclusive, there is no need to distinguish
        // unique vs non-unique key formats since both start with the key.
        _query.resetToKey(finalKey, _idx.ordering(), discriminator);
        seekWTCursor(_query);
        updatePosition();
        return curr(parts);
    }

    boost::optional<IndexKeyEntry> seek(const IndexSeekPoint& seekPoint,
                                        RequestedInfo parts) override {
        // TODO: don't go to a bson obj then to a KeyString, go straight
        BSONObj key = IndexEntryComparison::makeQueryObject(seekPoint, _forward);

        // makeQueryObject handles the discriminator in the real exclusive cases.
        const auto discriminator =
            _forward ? KeyString::kExclusiveBefore : KeyString::kExclusiveAfter;
        _query.resetToKey(key, _idx.ordering(), discriminator);
        seekWTCursor(_query);
        updatePosition();
        return curr(parts);
    }

    void savePositioned() override {
        try {
            if (_cursor)
                _cursor->reset();
        } catch (const WriteConflictException& wce) {
            // Ignore since this is only called when we are about to kill our transaction
            // anyway.
        }

        // Our saved position is wherever we were when we last called updatePosition().
        // Any partially completed repositions should not effect our saved position.
    }

    void saveUnpositioned() override {
        savePositioned();
        _eof = true;
    }

    void restore() override {
        if (!_cursor) {
            _cursor.emplace(_idx.uri(), _idx.tableId(), false, _txn);
        }

        // Ensure an active session exists, so any restored cursors will bind to it
        invariant(WiredTigerRecoveryUnit::get(_txn)->getSession(_txn) == _cursor->getSession());

        if (!_eof) {
            _lastMoveWasRestore = !seekWTCursor(_key);
            TRACE_CURSOR << "restore _lastMoveWasRestore:" << _lastMoveWasRestore;
        }
    }

    void detachFromOperationContext() final {
        _txn = nullptr;
        _cursor = {};
    }

    void reattachToOperationContext(OperationContext* txn) final {
        _txn = txn;
        // _cursor recreated in restore() to avoid risk of WT_ROLLBACK issues.
    }

protected:
    // Called after _key has been filled in. Must not throw WriteConflictException.
    virtual void updateLocAndTypeBits() = 0;

    boost::optional<IndexKeyEntry> curr(RequestedInfo parts) const {
        if (_eof)
            return {};

        dassert(!atOrPastEndPointAfterSeeking());
        dassert(!_loc.isNull());

        BSONObj bson;
        if (TRACING_ENABLED || (parts & kWantKey)) {
            bson = KeyString::toBson(_key.getBuffer(), _key.getSize(), _idx.ordering(), _typeBits);

            TRACE_CURSOR << " returning " << bson << ' ' << _loc;
        }

        return {{std::move(bson), _loc}};
    }

    bool atOrPastEndPointAfterSeeking() const {
        if (_eof)
            return true;
        if (!_endPosition)
            return false;

        const int cmp = _key.compare(*_endPosition);

        // We set up _endPosition to be in between the last in-range value and the first
        // out-of-range value. In particular, it is constructed to never equal any legal index
        // key.
        dassert(cmp != 0);

        if (_forward) {
            // We may have landed after the end point.
            return cmp > 0;
        } else {
            // We may have landed before the end point.
            return cmp < 0;
        }
    }

    void advanceWTCursor() {
        WT_CURSOR* c = _cursor->get();
        int ret = WT_OP_CHECK(_forward ? c->next(c) : c->prev(c));
        if (ret == WT_NOTFOUND) {
            _cursorAtEof = true;
            return;
        }
        invariantWTOK(ret);
        _cursorAtEof = false;
    }

    // Seeks to query. Returns true on exact match.
    bool seekWTCursor(const KeyString& query) {
        WT_CURSOR* c = _cursor->get();

        int cmp = -1;
        const WiredTigerItem keyItem(query.getBuffer(), query.getSize());
        c->set_key(c, keyItem.Get());

        int ret = WT_OP_CHECK(c->search_near(c, &cmp));
        if (ret == WT_NOTFOUND) {
            _cursorAtEof = true;
            TRACE_CURSOR << "\t not found";
            return false;
        }
        invariantWTOK(ret);
        _cursorAtEof = false;

        TRACE_CURSOR << "\t cmp: " << cmp;

        if (cmp == 0) {
            // Found it!
            return true;
        }

        // Make sure we land on a matching key (after/before for forward/reverse).
        if (_forward ? cmp < 0 : cmp > 0) {
            advanceWTCursor();
        }

        return false;
    }

    /**
     * This must be called after moving the cursor to update our cached position. It should not
     * be called after a restore that did not restore to original state since that does not
     * logically move the cursor until the following call to next().
     */
    void updatePosition() {
        _lastMoveWasRestore = false;
        if (_cursorAtEof) {
            _eof = true;
            _loc = RecordId();
            return;
        }

        _eof = false;

        WT_CURSOR* c = _cursor->get();
        WT_ITEM item;
        invariantWTOK(c->get_key(c, &item));
        _key.resetFromBuffer(item.data, item.size);

        if (atOrPastEndPointAfterSeeking()) {
            _eof = true;
            return;
        }

        updateLocAndTypeBits();
    }

    OperationContext* _txn;
    boost::optional<WiredTigerCursor> _cursor;
    const WiredTigerIndex& _idx;  // not owned
    const bool _forward;

    // These are where this cursor instance is. They are not changed in the face of a failing
    // next().
    KeyString _key;
    KeyString::TypeBits _typeBits;
    RecordId _loc;
    bool _eof = false;

    // This differs from _eof in that it always reflects the result of the most recent call to
    // reposition _cursor.
    bool _cursorAtEof = false;

    // Used by next to decide to return current position rather than moving. Should be reset to
    // false by any operation that moves the cursor, other than subsequent save/restore pairs.
    bool _lastMoveWasRestore = false;

    KeyString _query;

    std::unique_ptr<KeyString> _endPosition;
};

class WiredTigerIndexStandardCursor final : public WiredTigerIndexCursorBase {
public:
    WiredTigerIndexStandardCursor(const WiredTigerIndex& idx, OperationContext* txn, bool forward)
        : WiredTigerIndexCursorBase(idx, txn, forward) {}

    void updateLocAndTypeBits() override {
        _loc = KeyString::decodeRecordIdAtEnd(_key.getBuffer(), _key.getSize());

        WT_CURSOR* c = _cursor->get();
        WT_ITEM item;
        invariantWTOK(c->get_value(c, &item));
        BufReader br(item.data, item.size);
        _typeBits.resetFromBuffer(&br);
    }
};

class WiredTigerIndexUniqueCursor final : public WiredTigerIndexCursorBase {
public:
    WiredTigerIndexUniqueCursor(const WiredTigerIndex& idx, OperationContext* txn, bool forward)
        : WiredTigerIndexCursorBase(idx, txn, forward) {}

    void restore() override {
        WiredTigerIndexCursorBase::restore();

        // In addition to seeking to the correct key, we also need to make sure that the loc is
        // on the correct side of _loc.
        if (_lastMoveWasRestore)
            return;  // We are on a different key so no need to check loc.
        if (_eof)
            return;

        // If we get here we need to look at the actual RecordId for this key and make sure we
        // are supposed to see it.
        WT_CURSOR* c = _cursor->get();
        WT_ITEM item;
        invariantWTOK(c->get_value(c, &item));

        BufReader br(item.data, item.size);
        RecordId locInIndex = KeyString::decodeRecordId(&br);

        TRACE_CURSOR << "restore"
                     << " _loc:" << _loc << " locInIndex:" << locInIndex;

        if (locInIndex == _loc)
            return;

        _lastMoveWasRestore = true;
        if (_forward && (locInIndex < _loc))
            advanceWTCursor();
        if (!_forward && (locInIndex > _loc))
            advanceWTCursor();
    }

    void updateLocAndTypeBits() override {
        // We assume that cursors can only ever see unique indexes in their "pristine" state,
        // where no duplicates are possible. The cases where dups are allowed should hold
        // sufficient locks to ensure that no cursor ever sees them.
        WT_CURSOR* c = _cursor->get();
        WT_ITEM item;
        invariantWTOK(c->get_value(c, &item));

        BufReader br(item.data, item.size);
        _loc = KeyString::decodeRecordId(&br);
        _typeBits.resetFromBuffer(&br);

        if (!br.atEof()) {
            severe() << "Unique index cursor seeing multiple records for key "
                     << curr(kWantKey)->key;
            fassertFailed(28608);
        }
    }

    boost::optional<IndexKeyEntry> seekExact(const BSONObj& key, RequestedInfo parts) override {
        _query.resetToKey(stripFieldNames(key), _idx.ordering());
        const WiredTigerItem keyItem(_query.getBuffer(), _query.getSize());

        WT_CURSOR* c = _cursor->get();
        c->set_key(c, keyItem.Get());

        // Using search rather than search_near.
        int ret = WT_OP_CHECK(c->search(c));
        if (ret != WT_NOTFOUND)
            invariantWTOK(ret);
        _cursorAtEof = ret == WT_NOTFOUND;
        updatePosition();
        dassert(_eof || _key.compare(_query) == 0);
        return curr(parts);
    }
};

}  // namespace

WiredTigerIndexUnique::WiredTigerIndexUnique(OperationContext* ctx,
                                             const std::string& uri,
                                             const IndexDescriptor* desc)
    : WiredTigerIndex(ctx, uri, desc) {}

std::unique_ptr<SortedDataInterface::Cursor> WiredTigerIndexUnique::newCursor(OperationContext* txn,
                                                                              bool forward) const {
    return stdx::make_unique<WiredTigerIndexUniqueCursor>(*this, txn, forward);
}

SortedDataBuilderInterface* WiredTigerIndexUnique::getBulkBuilder(OperationContext* txn,
                                                                  bool dupsAllowed) {
    return new UniqueBulkBuilder(this, txn, dupsAllowed);
}

Status WiredTigerIndexUnique::_insert(WT_CURSOR* c,
                                      const BSONObj& key,
                                      const RecordId& loc,
                                      bool dupsAllowed) {
    const KeyString data(key, _ordering);
    WiredTigerItem keyItem(data.getBuffer(), data.getSize());

    KeyString value(loc);
    if (!data.getTypeBits().isAllZeros())
        value.appendTypeBits(data.getTypeBits());

    WiredTigerItem valueItem(value.getBuffer(), value.getSize());
    c->set_key(c, keyItem.Get());
    c->set_value(c, valueItem.Get());
    int ret = WT_OP_CHECK(c->insert(c));

    if (ret != WT_DUPLICATE_KEY) {
        return wtRCToStatus(ret);
    }

    // we might be in weird mode where there might be multiple values
    // we put them all in the "list"
    // Note that we can't omit AllZeros when there are multiple locs for a value. When we remove
    // down to a single value, it will be cleaned up.
    ret = WT_OP_CHECK(c->search(c));
    invariantWTOK(ret);

    WT_ITEM old;
    invariantWTOK(c->get_value(c, &old));

    bool insertedLoc = false;

    value.resetToEmpty();
    BufReader br(old.data, old.size);
    while (br.remaining()) {
        RecordId locInIndex = KeyString::decodeRecordId(&br);
        if (loc == locInIndex)
            return Status::OK();  // already in index

        if (!insertedLoc && loc < locInIndex) {
            value.appendRecordId(loc);
            value.appendTypeBits(data.getTypeBits());
            insertedLoc = true;
        }

        // Copy from old to new value
        value.appendRecordId(locInIndex);
        value.appendTypeBits(KeyString::TypeBits::fromBuffer(&br));
    }

    if (!dupsAllowed)
        return dupKeyError(key);

    if (!insertedLoc) {
        // This loc is higher than all currently in the index for this key
        value.appendRecordId(loc);
        value.appendTypeBits(data.getTypeBits());
    }

    valueItem = WiredTigerItem(value.getBuffer(), value.getSize());
    c->set_value(c, valueItem.Get());
    return wtRCToStatus(c->update(c));
}

void WiredTigerIndexUnique::_unindex(WT_CURSOR* c,
                                     const BSONObj& key,
                                     const RecordId& loc,
                                     bool dupsAllowed) {
    KeyString data(key, _ordering);
    WiredTigerItem keyItem(data.getBuffer(), data.getSize());
    c->set_key(c, keyItem.Get());

    if (!dupsAllowed) {
        // nice and clear
        int ret = WT_OP_CHECK(c->remove(c));
        if (ret == WT_NOTFOUND) {
            return;
        }
        invariantWTOK(ret);
        return;
    }

    // dups are allowed, so we have to deal with a vector of RecordIds.

    int ret = WT_OP_CHECK(c->search(c));
    if (ret == WT_NOTFOUND)
        return;
    invariantWTOK(ret);

    WT_ITEM old;
    invariantWTOK(c->get_value(c, &old));

    bool foundLoc = false;
    std::vector<std::pair<RecordId, KeyString::TypeBits>> records;

    BufReader br(old.data, old.size);
    while (br.remaining()) {
        RecordId locInIndex = KeyString::decodeRecordId(&br);
        KeyString::TypeBits typeBits = KeyString::TypeBits::fromBuffer(&br);

        if (loc == locInIndex) {
            if (records.empty() && !br.remaining()) {
                // This is the common case: we are removing the only loc for this key.
                // Remove the whole entry.
                invariantWTOK(WT_OP_CHECK(c->remove(c)));
                return;
            }

            foundLoc = true;
            continue;
        }

        records.push_back(std::make_pair(locInIndex, typeBits));
    }

    if (!foundLoc) {
        warning().stream() << loc << " not found in the index for key " << key;
        return;  // nothing to do
    }

    // Put other locs for this key back in the index.
    KeyString newValue;
    invariant(!records.empty());
    for (size_t i = 0; i < records.size(); i++) {
        newValue.appendRecordId(records[i].first);
        // When there is only one record, we can omit AllZeros TypeBits. Otherwise they need
        // to be included.
        if (!(records[i].second.isAllZeros() && records.size() == 1)) {
            newValue.appendTypeBits(records[i].second);
        }
    }

    WiredTigerItem valueItem = WiredTigerItem(newValue.getBuffer(), newValue.getSize());
    c->set_value(c, valueItem.Get());
    invariantWTOK(c->update(c));
}

// ------------------------------

WiredTigerIndexStandard::WiredTigerIndexStandard(OperationContext* ctx,
                                                 const std::string& uri,
                                                 const IndexDescriptor* desc)
    : WiredTigerIndex(ctx, uri, desc) {}

std::unique_ptr<SortedDataInterface::Cursor> WiredTigerIndexStandard::newCursor(
    OperationContext* txn, bool forward) const {
    return stdx::make_unique<WiredTigerIndexStandardCursor>(*this, txn, forward);
}

SortedDataBuilderInterface* WiredTigerIndexStandard::getBulkBuilder(OperationContext* txn,
                                                                    bool dupsAllowed) {
    // We aren't unique so dups better be allowed.
    invariant(dupsAllowed);
    return new StandardBulkBuilder(this, txn);
}

Status WiredTigerIndexStandard::_insert(WT_CURSOR* c,
                                        const BSONObj& keyBson,
                                        const RecordId& loc,
                                        bool dupsAllowed) {
    invariant(dupsAllowed);

    TRACE_INDEX << " key: " << keyBson << " loc: " << loc;

    KeyString key(keyBson, _ordering, loc);
    WiredTigerItem keyItem(key.getBuffer(), key.getSize());

    WiredTigerItem valueItem = key.getTypeBits().isAllZeros()
        ? emptyItem
        : WiredTigerItem(key.getTypeBits().getBuffer(), key.getTypeBits().getSize());

    c->set_key(c, keyItem.Get());
    c->set_value(c, valueItem.Get());
    int ret = WT_OP_CHECK(c->insert(c));

    if (ret != WT_DUPLICATE_KEY)
        return wtRCToStatus(ret);
    // If the record was already in the index, we just return OK.
    // This can happen, for example, when building a background index while documents are being
    // written and reindexed.
    return Status::OK();
}

void WiredTigerIndexStandard::_unindex(WT_CURSOR* c,
                                       const BSONObj& key,
                                       const RecordId& loc,
                                       bool dupsAllowed) {
    invariant(dupsAllowed);
    KeyString data(key, _ordering, loc);
    WiredTigerItem item(data.getBuffer(), data.getSize());
    c->set_key(c, item.Get());
    int ret = WT_OP_CHECK(c->remove(c));
    if (ret != WT_NOTFOUND) {
        invariantWTOK(ret);
    }
}

// ---------------- for compatability with rc4 and previous ------

int index_collator_customize(WT_COLLATOR* coll,
                             WT_SESSION* s,
                             const char* uri,
                             WT_CONFIG_ITEM* metadata,
                             WT_COLLATOR** collp) {
    fassertFailedWithStatusNoTrace(28580,
                                   Status(ErrorCodes::UnsupportedFormat,
                                          str::stream()
                                              << "Found an index from an unsupported RC version."
                                              << " Please restart with --repair to fix."));
}

extern "C" MONGO_COMPILER_API_EXPORT int index_collator_extension(WT_CONNECTION* conn,
                                                                  WT_CONFIG_ARG* cfg) {
    static WT_COLLATOR idx_static;

    idx_static.customize = index_collator_customize;
    return conn->add_collator(conn, "mongo_index", &idx_static, NULL);
}

}  // namespace mongo