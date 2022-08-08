// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

#include "storage/persistent_index.h"

#include <gtest/gtest.h>

#include <cstdlib>

#include "fs/fs_memory.h"
#include "fs/fs_util.h"
#include "storage/chunk_helper.h"
#include "storage/rowset/rowset.h"
#include "storage/rowset/rowset_factory.h"
#include "storage/rowset/rowset_writer.h"
#include "storage/rowset/rowset_writer_context.h"
#include "storage/rowset_update_state.h"
#include "storage/storage_engine.h"
#include "storage/tablet_manager.h"
#include "storage/update_manager.h"
#include "testutil/assert.h"
#include "testutil/parallel_test.h"
#include "util/coding.h"
#include "util/faststring.h"

namespace starrocks {
PARALLEL_TEST(PersistentIndexTest, test_mutable_index) {
    using Key = uint64_t;
    int N = 1000;
    vector<Key> keys;
    vector<Slice> key_slices;
    vector<IndexValue> values;
    keys.reserve(N);
    key_slices.reserve(N);
    for (int i = 0; i < N; i++) {
        keys.emplace_back(i);
        values.emplace_back(i * 2);
        key_slices.emplace_back((uint8_t*)(&keys[i]), sizeof(Key));
    }
    ASSIGN_OR_ABORT(auto idx, MutableIndex::create(sizeof(Key), "./PersistentIndexTest_test_mutable_index"));

    // test insert
    ASSERT_OK(idx->insert(keys.size(), key_slices.data(), values.data()));
    // insert duplicate should return error
    ASSERT_FALSE(idx->insert(keys.size(), key_slices.data(), values.data()).ok());

    // test get
    vector<IndexValue> get_values(keys.size());
    KeysInfo get_not_found;
    size_t get_num_found = 0;
    ASSERT_TRUE(idx->get(keys.size(), key_slices.data(), get_values.data(), &get_not_found, &get_num_found).ok());
    ASSERT_EQ(keys.size(), get_num_found);
    ASSERT_EQ(get_not_found.key_idxes.size(), 0);
    for (int i = 0; i < values.size(); i++) {
        ASSERT_EQ(values[i], get_values[i]);
    }
    vector<Key> get2_keys;
    vector<Slice> get2_key_slices;
    get2_keys.reserve(N);
    get2_key_slices.reserve(N);
    for (int i = 0; i < N; i++) {
        get2_keys.emplace_back(i * 2);
        get2_key_slices.emplace_back((uint8_t*)(&get2_keys[i]), sizeof(Key));
    }
    vector<IndexValue> get2_values(get2_keys.size());
    KeysInfo get2_not_found;
    size_t get2_num_found = 0;
    // should only find 0,2,..N-2, not found: N,N+2, .. N*2-2
    ASSERT_TRUE(idx->get(get2_keys.size(), get2_key_slices.data(), get2_values.data(), &get2_not_found, &get2_num_found)
                        .ok());
    ASSERT_EQ(N / 2, get2_num_found);

    // test erase
    vector<Key> erase_keys;
    vector<Slice> erase_key_slices;
    erase_keys.reserve(N);
    erase_key_slices.reserve(N);
    size_t num = 0;
    for (int i = 0; i < N + 3; i += 3) {
        erase_keys.emplace_back(i);
        erase_key_slices.emplace_back((uint8_t*)(&erase_keys[num]), sizeof(Key));
        num++;
    }
    vector<IndexValue> erase_old_values(erase_keys.size());
    KeysInfo erase_not_found;
    size_t erase_num_found = 0;
    ASSERT_TRUE(idx->erase(erase_keys.size(), erase_key_slices.data(), erase_old_values.data(), &erase_not_found,
                           &erase_num_found)
                        .ok());
    ASSERT_EQ(erase_num_found, (N + 2) / 3);
    // N+2 not found
    ASSERT_EQ(erase_not_found.key_idxes.size(), 1);

    // test upsert
    vector<Key> upsert_keys(N, 0);
    vector<Slice> upsert_key_slices;
    vector<IndexValue> upsert_values(upsert_keys.size());
    upsert_key_slices.reserve(N);
    size_t expect_exists = 0;
    size_t expect_not_found = 0;
    for (int i = 0; i < N; i++) {
        upsert_keys[i] = i * 2;
        if (i % 3 != 0 && i * 2 < N) {
            expect_exists++;
        }
        upsert_key_slices.emplace_back((uint8_t*)(&upsert_keys[i]), sizeof(Key));
        if (i * 2 >= N && i * 2 != N + 2) {
            expect_not_found++;
        }
        upsert_values[i] = i * 3;
    }
    vector<IndexValue> upsert_old_values(upsert_keys.size());
    KeysInfo upsert_not_found;
    size_t upsert_num_found = 0;
    ASSERT_TRUE(idx->upsert(upsert_keys.size(), upsert_key_slices.data(), upsert_values.data(),
                            upsert_old_values.data(), &upsert_not_found, &upsert_num_found)
                        .ok());
    ASSERT_EQ(upsert_num_found, expect_exists);
    ASSERT_EQ(upsert_not_found.key_idxes.size(), expect_not_found);
}

PARALLEL_TEST(PersistentIndexTest, test_mutable_index_wal) {
    FileSystem* fs = FileSystem::Default();
    const std::string kPersistentIndexDir = "./PersistentIndexTest_test_mutable_index_wal";
    const std::string kIndexFile = "./PersistentIndexTest_test_mutable_index_wal/index.l0.0.0";
    bool created;
    ASSERT_OK(fs->create_dir_if_missing(kPersistentIndexDir, &created));

    using Key = uint64_t;
    PersistentIndexMetaPB index_meta;
    int N = 1000000;
    // insert
    vector<Key> keys;
    vector<Slice> key_slices;
    vector<IndexValue> values;
    keys.reserve(N);
    key_slices.reserve(N);
    for (int i = 0; i < N; i++) {
        keys.emplace_back(i);
        values.emplace_back(i * 2);
        key_slices.emplace_back((uint8_t*)(&keys[i]), sizeof(Key));
    }
    // erase
    vector<Key> erase_keys;
    vector<Slice> erase_key_slices;
    erase_keys.reserve(N / 2);
    erase_key_slices.reserve(N / 2);
    for (int i = 0; i < N / 2; i++) {
        erase_keys.emplace_back(i);
        erase_key_slices.emplace_back((uint8_t*)(&erase_keys[i]), sizeof(Key));
    }
    // append invalid wal
    std::vector<Key> invalid_keys;
    std::vector<Slice> invalid_key_slices;
    std::vector<IndexValue> invalid_values;
    invalid_keys.reserve(N / 2);
    invalid_key_slices.reserve(N / 2);
    for (int i = 0; i < N / 2; i++) {
        invalid_keys.emplace_back(i);
        invalid_values.emplace_back(i * 2);
        invalid_key_slices.emplace_back((uint8_t*)(&invalid_keys[i]), sizeof(Key));
    }

    {
        ASSIGN_OR_ABORT(auto wfile, FileSystem::Default()->new_writable_file(kIndexFile));
        ASSERT_OK(wfile->close());
    }

    {
        EditVersion version(0, 0);
        index_meta.set_key_size(sizeof(Key));
        index_meta.set_size(0);
        version.to_pb(index_meta.mutable_version());
        MutableIndexMetaPB* l0_meta = index_meta.mutable_l0_meta();
        IndexSnapshotMetaPB* snapshot_meta = l0_meta->mutable_snapshot();
        version.to_pb(snapshot_meta->mutable_version());

        PersistentIndex index(kPersistentIndexDir);
        //ASSERT_TRUE(index.create(sizeof(Key), version).ok());

        ASSERT_OK(index.load(index_meta));
        ASSERT_OK(index.prepare(EditVersion(1, 0)));
        ASSERT_OK(index.insert(N, key_slices.data(), values.data(), false));
        ASSERT_OK(index.commit(&index_meta));
        ASSERT_OK(index.on_commited());

        std::vector<IndexValue> old_values(keys.size());
        ASSERT_TRUE(index.prepare(EditVersion(2, 0)).ok());
        ASSERT_TRUE(index.upsert(keys.size(), key_slices.data(), values.data(), old_values.data()).ok());
        ASSERT_TRUE(index.commit(&index_meta).ok());
        ASSERT_TRUE(index.on_commited().ok());

        vector<IndexValue> erase_old_values(erase_keys.size());
        ASSERT_TRUE(index.prepare(EditVersion(3, 0)).ok());
        ASSERT_TRUE(index.erase(erase_keys.size(), erase_key_slices.data(), erase_old_values.data()).ok());
        // update PersistentMetaPB in memory
        ASSERT_TRUE(index.commit(&index_meta).ok());
        ASSERT_TRUE(index.on_commited().ok());

        std::vector<IndexValue> get_values(keys.size());
        ASSERT_TRUE(index.get(keys.size(), key_slices.data(), get_values.data()).ok());
        ASSERT_EQ(keys.size(), get_values.size());
        for (int i = 0; i < N / 2; i++) {
            ASSERT_EQ(NullIndexValue, get_values[i].get_value());
        }
        for (int i = N / 2; i < values.size(); i++) {
            ASSERT_EQ(values[i], get_values[i]);
        }
    }

    {
        // rebuild mutableindex according PersistentIndexMetaPB
        PersistentIndex new_index(kPersistentIndexDir);
        //ASSERT_TRUE(new_index.create(sizeof(Key), EditVersion(3, 0)).ok());
        ASSERT_TRUE(new_index.load(index_meta).ok());
        std::vector<IndexValue> get_values(keys.size());

        ASSERT_TRUE(new_index.get(keys.size(), key_slices.data(), get_values.data()).ok());
        ASSERT_EQ(keys.size(), get_values.size());
        for (int i = 0; i < N / 2; i++) {
            ASSERT_EQ(NullIndexValue, get_values[i].get_value());
        }
        for (int i = N / 2; i < values.size(); i++) {
            ASSERT_EQ(values[i], get_values[i]);
        }

        // upsert key/value to new_index
        vector<IndexValue> old_values(invalid_keys.size());
        ASSERT_TRUE(new_index.prepare(EditVersion(4, 0)).ok());
        ASSERT_TRUE(new_index
                            .upsert(invalid_keys.size(), invalid_key_slices.data(), invalid_values.data(),
                                    old_values.data())
                            .ok());
        ASSERT_TRUE(new_index.commit(&index_meta).ok());
        ASSERT_TRUE(new_index.on_commited().ok());
    }
    // rebuild mutableindex according to PersistentIndexMetaPB
    {
        PersistentIndex index(kPersistentIndexDir);
        //ASSERT_TRUE(index.create(sizeof(Key), EditVersion(4, 0)).ok());
        ASSERT_TRUE(index.load(index_meta).ok());
        std::vector<IndexValue> get_values(keys.size());

        ASSERT_TRUE(index.get(keys.size(), key_slices.data(), get_values.data()).ok());
        ASSERT_EQ(keys.size(), get_values.size());
        for (int i = 0; i < values.size(); i++) {
            ASSERT_EQ(values[i], get_values[i]);
        }
    }
    ASSERT_TRUE(fs::remove_all(kPersistentIndexDir).ok());
}

PARALLEL_TEST(PersistentIndexTest, test_mutable_flush_to_immutable) {
    using Key = uint64_t;
    int N = 200000;
    vector<Key> keys(N);
    vector<IndexValue> values(N);
    vector<Slice> key_slices;
    key_slices.reserve(N);
    for (int i = 0; i < N; i++) {
        keys[i] = i;
        values[i] = i * 2;
        key_slices.emplace_back((uint8_t*)(&keys[i]), sizeof(Key));
    }
    auto rs = MutableIndex::create(sizeof(Key), "./PersistentIndexTest_test_mutable_flush_to_immutable");
    ASSERT_TRUE(rs.ok());
    std::unique_ptr<MutableIndex> idx = std::move(rs).value();

    // test insert
    ASSERT_TRUE(idx->insert(keys.size(), key_slices.data(), values.data()).ok());

    ASSERT_TRUE(idx->flush_to_immutable_index(".", EditVersion(1, 1)).ok());

    ASSIGN_OR_ABORT(auto fs, FileSystem::CreateSharedFromString("posix://"));
    ASSIGN_OR_ABORT(auto rf, fs->new_random_access_file("./index.l1.1.1"));
    auto st_load = ImmutableIndex::load(std::move(rf));
    if (!st_load.ok()) {
        LOG(WARNING) << st_load.status();
    }
    ASSERT_TRUE(st_load.ok());
    auto& idx_loaded = st_load.value();
    KeysInfo keys_info;
    for (size_t i = 0; i < N; i++) {
        keys_info.key_idxes.emplace_back(i);
        uint64_t h = key_index_hash(&keys[i], sizeof(Key));
        keys_info.hashes.emplace_back(h);
    }
    vector<IndexValue> get_values(N);
    size_t num_found = 0;
    auto st_get = idx_loaded->get(N, key_slices.data(), keys_info, get_values.data(), &num_found, sizeof(Key));
    if (!st_get.ok()) {
        LOG(WARNING) << st_get;
    }
    ASSERT_TRUE(st_get.ok());
    ASSERT_EQ(N, num_found);
    for (size_t i = 0; i < N; i++) {
        ASSERT_EQ(values[i], get_values[i]);
    }
    ASSERT_TRUE(idx_loaded->check_not_exist(N, key_slices.data(), sizeof(Key)).is_already_exist());

    vector<Key> check_not_exist_keys(10);
    vector<Slice> check_not_exist_key_slices(10);
    for (int i = 0; i < 10; i++) {
        check_not_exist_keys[i] = N + i;
        check_not_exist_key_slices[i] = Slice((uint8_t*)(&check_not_exist_keys[i]), sizeof(Key));
    }
    ASSERT_TRUE(idx_loaded->check_not_exist(10, check_not_exist_key_slices.data(), sizeof(Key)).ok());
}

TabletSharedPtr create_tablet(int64_t tablet_id, int32_t schema_hash) {
    TCreateTabletReq request;
    request.tablet_id = tablet_id;
    request.__set_version(1);
    request.__set_version_hash(0);
    request.tablet_schema.schema_hash = schema_hash;
    request.tablet_schema.short_key_column_count = 6;
    request.tablet_schema.keys_type = TKeysType::PRIMARY_KEYS;
    request.tablet_schema.storage_type = TStorageType::COLUMN;

    TColumn k1;
    k1.column_name = "pk";
    k1.__set_is_key(true);
    k1.column_type.type = TPrimitiveType::BIGINT;
    request.tablet_schema.columns.push_back(k1);

    TColumn k2;
    k2.column_name = "v1";
    k2.__set_is_key(false);
    k2.column_type.type = TPrimitiveType::SMALLINT;
    request.tablet_schema.columns.push_back(k2);

    TColumn k3;
    k3.column_name = "v2";
    k3.__set_is_key(false);
    k3.column_type.type = TPrimitiveType::INT;
    request.tablet_schema.columns.push_back(k3);
    auto st = StorageEngine::instance()->create_tablet(request);
    CHECK(st.ok()) << st.to_string();
    return StorageEngine::instance()->tablet_manager()->get_tablet(tablet_id, false);
}

RowsetSharedPtr create_rowset(const TabletSharedPtr& tablet, const vector<int64_t>& keys,
                              vectorized::Column* one_delete = nullptr) {
    RowsetWriterContext writer_context;
    RowsetId rowset_id = StorageEngine::instance()->next_rowset_id();
    writer_context.rowset_id = rowset_id;
    writer_context.tablet_id = tablet->tablet_id();
    writer_context.tablet_schema_hash = tablet->schema_hash();
    writer_context.partition_id = 0;
    writer_context.rowset_path_prefix = tablet->schema_hash_path();
    writer_context.rowset_state = COMMITTED;
    writer_context.tablet_schema = &tablet->tablet_schema();
    writer_context.version.first = 0;
    writer_context.version.second = 0;
    writer_context.segments_overlap = NONOVERLAPPING;
    std::unique_ptr<RowsetWriter> writer;
    EXPECT_TRUE(RowsetFactory::create_rowset_writer(writer_context, &writer).ok());
    auto schema = ChunkHelper::convert_schema(tablet->tablet_schema());
    auto chunk = ChunkHelper::new_chunk(schema, keys.size());
    auto& cols = chunk->columns();
    size_t size = keys.size();
    for (size_t i = 0; i < size; i++) {
        cols[0]->append_datum(vectorized::Datum(keys[i]));
        cols[1]->append_datum(vectorized::Datum((int16_t)(keys[i] % size + 1)));
        cols[2]->append_datum(vectorized::Datum((int32_t)(keys[i] % size + 2)));
    }
    if (one_delete == nullptr && !keys.empty()) {
        CHECK_OK(writer->flush_chunk(*chunk));
    } else if (one_delete == nullptr) {
        CHECK_OK(writer->flush());
    } else if (one_delete != nullptr) {
        CHECK_OK(writer->flush_chunk_with_deletes(*chunk, *one_delete));
    }
    return *writer->build();
}

void build_persistent_index_from_tablet(size_t N) {
    FileSystem* fs = FileSystem::Default();
    const std::string kPersistentIndexDir = "./persistent_index_test";
    bool created;
    ASSERT_OK(fs->create_dir_if_missing(kPersistentIndexDir, &created));

    TabletSharedPtr tablet = create_tablet(rand(), rand());
    ASSERT_EQ(1, tablet->updates()->version_history_count());
    std::vector<int64_t> keys(N);
    std::vector<Slice> key_slices;
    key_slices.reserve(N);
    for (int64_t i = 0; i < N; ++i) {
        keys[i] = i;
        key_slices.emplace_back((uint8_t*)(&keys[i]), sizeof(uint64_t));
    }

    RowsetSharedPtr rowset = create_rowset(tablet, keys);
    auto pool = StorageEngine::instance()->update_manager()->apply_thread_pool();
    auto version = 2;
    auto st = tablet->rowset_commit(version, rowset);
    ASSERT_TRUE(st.ok()) << st.to_string();
    // Ensure that there is at most one thread doing the version apply job.
    ASSERT_LE(pool->num_threads(), 1);
    ASSERT_EQ(version, tablet->updates()->max_version());
    ASSERT_EQ(version, tablet->updates()->version_history_count());
    // call `get_applied_rowsets` to wait rowset apply finish
    std::vector<RowsetSharedPtr> rowsets;
    EditVersion full_edit_version;
    ASSERT_TRUE(tablet->updates()->get_applied_rowsets(version, &rowsets, &full_edit_version).ok());

    auto manager = StorageEngine::instance()->update_manager();
    auto index_entry = manager->index_cache().get_or_create(tablet->tablet_id());
    index_entry->update_expire_time(MonotonicMillis() + manager->get_cache_expire_ms());
    auto& primary_index = index_entry->value();
    st = primary_index.load(tablet.get());
    if (!st.ok()) {
        LOG(WARNING) << "load primary index from tablet failed";
        ASSERT_TRUE(false);
    }

    RowsetUpdateState state;
    st = state.load(tablet.get(), rowset.get());
    if (!st.ok()) {
        LOG(WARNING) << "failed to load rowset update state: " << st.to_string();
        ASSERT_TRUE(false);
    }
    using ColumnUniquePtr = std::unique_ptr<vectorized::Column>;
    const std::vector<ColumnUniquePtr>& upserts = state.upserts();

    PersistentIndex persistent_index(kPersistentIndexDir);
    ASSERT_TRUE(persistent_index.load_from_tablet(tablet.get()).ok());

    // check data in persistent index
    for (size_t i = 0; i < upserts.size(); ++i) {
        auto& pks = *upserts[i];

        std::vector<uint64_t> primary_results;
        std::vector<uint64_t> persistent_results;
        primary_results.resize(pks.size());
        persistent_results.resize(pks.size());
        primary_index.get(pks, &primary_results);
        if (pks.is_binary()) {
            persistent_index.get(pks.size(), reinterpret_cast<const Slice*>(pks.raw_data()),
                                 reinterpret_cast<IndexValue*>(persistent_results.data()));
        } else {
            size_t key_size = primary_index.key_size();
            ASSERT_TRUE(key_size == sizeof(uint64_t));
            std::vector<Slice> col_key_slices;
            for (size_t i = 0; i < pks.size(); ++i) {
                col_key_slices.emplace_back(pks.raw_data() + i * key_size, key_size);
            }
            persistent_index.get(pks.size(), col_key_slices.data(),
                                 reinterpret_cast<IndexValue*>(persistent_results.data()));
        }

        ASSERT_EQ(primary_results.size(), persistent_results.size());
        for (size_t j = 0; j < primary_results.size(); ++j) {
            ASSERT_EQ(primary_results[i], persistent_results[i]);
        }
        primary_results.clear();
        persistent_results.clear();
    }

    {
        // load data from index file
        PersistentIndex persistent_index(kPersistentIndexDir);
        Status st = persistent_index.load_from_tablet(tablet.get());
        if (!st.ok()) {
            LOG(WARNING) << "build persistent index failed: " << st.to_string();
            ASSERT_TRUE(false);
        }
        for (size_t i = 0; i < upserts.size(); ++i) {
            auto& pks = *upserts[i];
            std::vector<uint64_t> primary_results;
            std::vector<uint64_t> persistent_results;
            primary_results.resize(pks.size());
            persistent_results.resize(pks.size());
            primary_index.get(pks, &primary_results);
            if (pks.is_binary()) {
                persistent_index.get(pks.size(), reinterpret_cast<const Slice*>(pks.raw_data()),
                                     reinterpret_cast<IndexValue*>(persistent_results.data()));
            } else {
                size_t key_size = primary_index.key_size();
                std::vector<Slice> col_key_slices;
                for (size_t i = 0; i < pks.size(); ++i) {
                    col_key_slices.emplace_back(pks.raw_data() + i * key_size, key_size);
                }
                persistent_index.get(pks.size(), col_key_slices.data(),
                                     reinterpret_cast<IndexValue*>(persistent_results.data()));
            }
            ASSERT_EQ(primary_results.size(), persistent_results.size());
            for (size_t j = 0; j < primary_results.size(); ++j) {
                ASSERT_EQ(primary_results[i], persistent_results[i]);
            }
            primary_results.clear();
            persistent_results.clear();
        }
    }

    manager->index_cache().release(index_entry);
    ASSERT_TRUE(fs::remove_all(kPersistentIndexDir).ok());
}

PARALLEL_TEST(PersistentIndexTest, test_build_from_tablet) {
    // dump snapshot
    build_persistent_index_from_tablet(100000);
    // write wal
    build_persistent_index_from_tablet(250000);
    // flush l1
    build_persistent_index_from_tablet(1000000);
}

PARALLEL_TEST(PersistentIndexTest, test_replace) {
    FileSystem* fs = FileSystem::Default();
    const std::string kPersistentIndexDir = "./PersistentIndexTest_test_replace";
    const std::string kIndexFile = "./PersistentIndexTest_test_replace/index.l0.0.0";
    bool created;
    ASSERT_OK(fs->create_dir_if_missing(kPersistentIndexDir, &created));

    using Key = uint64_t;
    PersistentIndexMetaPB index_meta;
    // insert
    vector<Key> keys;
    vector<Slice> key_slices;
    vector<IndexValue> values;
    vector<uint32_t> src_rssid;
    vector<IndexValue> replace_values;
    int N = 1000000;
    keys.reserve(N);
    key_slices.reserve(N);
    for (int i = 0; i < N; i++) {
        keys.emplace_back(i);
        key_slices.emplace_back((uint8_t*)(&keys[i]), sizeof(Key));
        values.emplace_back(i * 2);
        replace_values.emplace_back(i * 3);
    }

    for (int i = 0; i < N / 2; i++) {
        src_rssid.emplace_back(0);
    }
    for (int i = N / 2; i < N; i++) {
        src_rssid.emplace_back(1);
    }

    ASSIGN_OR_ABORT(auto wfile, FileSystem::Default()->new_writable_file(kIndexFile));

    EditVersion version(0, 0);
    index_meta.set_key_size(sizeof(Key));
    index_meta.set_size(0);
    version.to_pb(index_meta.mutable_version());
    MutableIndexMetaPB* l0_meta = index_meta.mutable_l0_meta();
    IndexSnapshotMetaPB* snapshot_meta = l0_meta->mutable_snapshot();
    version.to_pb(snapshot_meta->mutable_version());

    PersistentIndex index(kPersistentIndexDir);

    ASSERT_TRUE(index.load(index_meta).ok());
    ASSERT_TRUE(index.prepare(EditVersion(1, 0)).ok());
    ASSERT_TRUE(index.insert(N, key_slices.data(), values.data(), false).ok());
    ASSERT_TRUE(index.commit(&index_meta).ok());
    ASSERT_TRUE(index.on_commited().ok());

    std::vector<IndexValue> get_values(keys.size());
    ASSERT_TRUE(index.get(keys.size(), key_slices.data(), get_values.data()).ok());
    ASSERT_EQ(keys.size(), get_values.size());
    for (int i = 0; i < values.size(); i++) {
        ASSERT_EQ(values[i], get_values[i]);
    }

    //replace
    std::vector<uint32_t> failed(keys.size());
    Status st = index.try_replace(N, key_slices.data(), replace_values.data(), src_rssid, &failed);
    ASSERT_TRUE(st.ok());
    std::vector<IndexValue> new_get_values(keys.size());
    ASSERT_TRUE(index.get(keys.size(), key_slices.data(), new_get_values.data()).ok());
    ASSERT_EQ(keys.size(), new_get_values.size());
    for (int i = 0; i < N / 2; i++) {
        ASSERT_EQ(replace_values[i], new_get_values[i]);
    }
    for (int i = N / 2; i < N; i++) {
        ASSERT_EQ(values[i], new_get_values[i]);
    }
    ASSERT_TRUE(fs::remove_all(kPersistentIndexDir).ok());
}

PARALLEL_TEST(PersistentIndexTest, test_get_move_buckets) {
    const std::string kPersistentIndexDir = "./PersistentIndexTest_test_get_move_buckets";
    PersistentIndex index(kPersistentIndexDir);
    std::vector<uint8_t> bucket_packs_in_page;
    bucket_packs_in_page.reserve(16);
    srand((int)time(NULL));
    for (int32_t i = 0; i < 16; ++i) {
        bucket_packs_in_page.emplace_back(rand() % 32);
    }
    int32_t sum = 0;
    for (int32_t i = 0; i < 16; ++i) {
        sum += bucket_packs_in_page[i];
    }

    for (int32_t i = 0; i < 100; ++i) {
        int32_t target = rand() % sum;
        auto ret = index.test_get_move_buckets(target, bucket_packs_in_page.data());
        int32_t find_target = 0;
        for (int32_t i = 0; i < ret.size(); ++i) {
            find_target += bucket_packs_in_page[ret[i]];
        }
        ASSERT_TRUE(find_target >= target);
    }
    ASSERT_TRUE(fs::remove_all(kPersistentIndexDir).ok());
}

PARALLEL_TEST(PersistentIndexTest, test_flush_varlen_to_immutable) {
    const std::string kPersistentIndexDir = "./PersistentIndexTest_test_flush_varlen_to_immutable";
    ASSIGN_OR_ABORT(auto fs, FileSystem::CreateSharedFromString("posix://"));
    bool created;
    ASSERT_OK(fs->create_dir_if_missing(kPersistentIndexDir, &created));
    PersistentIndex index(kPersistentIndexDir);
    using Key = std::string;
    int N = 200000;
    EditVersion version(1, 0);
    vector<Key> keys(N);
    vector<Slice> keys_slice(N);
    vector<IndexValue> values(N);
    for (int i = 0; i < N; i++) {
        keys[i] = "test_varlen_" + std::to_string(i);
        values[i] = i;
        keys_slice[i] = Slice(keys[i].data(), keys[i].size());
    }
    auto flush_st = index.test_flush_varlen_to_immutable_index(kPersistentIndexDir, version, N, keys_slice.data(),
                                                               values.data());
    if (!flush_st.ok()) {
        LOG(WARNING) << flush_st;
    }
    ASSERT_TRUE(flush_st.ok());

    std::string l1_file_path = kPersistentIndexDir + "/index.l1.1.0";
    ASSIGN_OR_ABORT(auto rf, fs->new_random_access_file(l1_file_path));
    auto st_load = ImmutableIndex::load(std::move(rf));
    if (!st_load.ok()) {
        LOG(WARNING) << st_load.status();
    }
    ASSERT_TRUE(st_load.ok());
    auto& idx_loaded = st_load.value();
    KeysInfo keys_info;
    for (size_t i = 0; i < N; i++) {
        keys_info.key_idxes.emplace_back(i);
        uint64_t h = key_index_hash(keys[i].data(), keys[i].size());
        keys_info.hashes.emplace_back(h);
    }
    vector<IndexValue> get_values(N);
    size_t num_found = 0;
    auto st_get = idx_loaded->get(N, keys_slice.data(), keys_info, get_values.data(), &num_found, 0);
    if (!st_get.ok()) {
        LOG(WARNING) << st_get;
    }
    ASSERT_TRUE(st_get.ok());
    ASSERT_EQ(N, num_found);
    for (size_t i = 0; i < N; i++) {
        ASSERT_EQ(values[i], get_values[i]);
    }

    auto st_check = idx_loaded->check_not_exist(N, keys_slice.data(), 0);
    LOG(WARNING) << "check status is " << st_check;
    ASSERT_TRUE(idx_loaded->check_not_exist(N, keys_slice.data(), 0).is_already_exist());

    ASSERT_TRUE(fs::remove_all(kPersistentIndexDir).ok());
}

} // namespace starrocks
