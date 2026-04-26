#include "ut_helpers.h"

#include <ydb/core/base/table_index.h>
#include <ydb/core/protos/index_builder.pb.h>
#include <ydb/core/testlib/test_client.h>
#include <ydb/core/tx/datashard/ut_common/datashard_ut_common.h>
#include <ydb/core/tx/schemeshard/schemeshard.h>
#include <ydb/core/tx/tx_proxy/proxy.h>

#include <yql/essentials/public/issue/yql_issue_message.h>

#include <library/cpp/testing/unittest/registar.h>

namespace NKikimr {
using namespace Tests;
using Ydb::Table::VectorIndexSettings;
using namespace NTableIndex::NKMeans;

static std::atomic<ui64> sId = 1;
static constexpr const char* kMainTable = "/Root/table-main";

Y_UNIT_TEST_SUITE (TTxDataShardClusterTreeRecalculateScan) {

    static void DoBadRequest(Tests::TServer::TPtr server, TActorId sender,
        std::function<void(NKikimrTxDataShard::TEvClusterTreeRecalculateRequest&)> setupRequest,
        const TString& expectedError, bool expectedErrorSubstring = false)
    {
        auto id = sId.fetch_add(1, std::memory_order_relaxed);
        auto snapshot = CreateVolatileSnapshot(server, {kMainTable});
        auto datashards = GetTableShards(server, sender, kMainTable);
        TTableId tableId = ResolveTableId(server, sender, kMainTable);

        UNIT_ASSERT(datashards.size() == 1);

        auto ev = std::make_unique<TEvDataShard::TEvClusterTreeRecalculateRequest>();
        auto& rec = ev->Record;
        rec.SetId(1);
        rec.SetSeqNoGeneration(id);
        rec.SetSeqNoRound(1);
        rec.SetTabletId(datashards[0]);
        tableId.PathId.ToProto(rec.MutablePathId());
        rec.SetSnapshotTxId(snapshot.TxId);
        rec.SetSnapshotStep(snapshot.Step);

        VectorIndexSettings settings;
        settings.set_vector_dimension(2);
        settings.set_vector_type(VectorIndexSettings::VECTOR_TYPE_UINT8);
        settings.set_metric(VectorIndexSettings::DISTANCE_COSINE);
        *rec.MutableSettings() = settings;

        rec.SetParent(0);
        rec.AddClusterIds(10);
        rec.AddClusterIds(11);
        rec.SetEmbeddingColumn("embedding");

        setupRequest(rec);

        NKikimr::DoBadRequest<TEvDataShard::TEvClusterTreeRecalculateResponse>(
            server, sender, std::move(ev), datashards[0], expectedError, expectedErrorSubstring);
    }

    static TString DoRecalculate(Tests::TServer::TPtr server, TActorId sender,
        NTableIndex::NKMeans::TClusterId parent,
        const std::vector<ui64>& clusterIds,
        const std::vector<TString>& clusters,
        VectorIndexSettings::VectorType type,
        VectorIndexSettings::Metric metric)
    {
        auto id = sId.fetch_add(1, std::memory_order_relaxed);
        auto& runtime = *server->GetRuntime();
        auto snapshot = CreateVolatileSnapshot(server, {kMainTable});
        auto datashards = GetTableShards(server, sender, kMainTable);
        TTableId tableId = ResolveTableId(server, sender, kMainTable);

        TStringBuilder data;

        for (auto tid : datashards) {
            auto ev1 = std::make_unique<TEvDataShard::TEvClusterTreeRecalculateRequest>();
            auto ev2 = std::make_unique<TEvDataShard::TEvClusterTreeRecalculateRequest>();
            auto fill = [&](std::unique_ptr<TEvDataShard::TEvClusterTreeRecalculateRequest>& ev) {
                auto& rec = ev->Record;
                rec.SetId(1);
                rec.SetSeqNoGeneration(id);
                rec.SetSeqNoRound(1);
                rec.SetTabletId(tid);
                tableId.PathId.ToProto(rec.MutablePathId());
                rec.SetSnapshotTxId(snapshot.TxId);
                rec.SetSnapshotStep(snapshot.Step);

                VectorIndexSettings settings;
                settings.set_vector_dimension(2);
                settings.set_vector_type(type);
                settings.set_metric(metric);
                *rec.MutableSettings() = settings;

                rec.SetParent(parent);
                for (auto cid : clusterIds) {
                    rec.AddClusterIds(cid);
                }
                for (const auto& c : clusters) {
                    rec.AddClusters(c);
                }
                rec.SetEmbeddingColumn("embedding");
            };
            fill(ev1);
            fill(ev2);

            runtime.SendToPipe(tid, sender, ev1.release(), 0, GetPipeConfigWithRetries());
            runtime.SendToPipe(tid, sender, ev2.release(), 0, GetPipeConfigWithRetries());

            TAutoPtr<IEventHandle> handle;
            auto reply = runtime.GrabEdgeEventRethrow<TEvDataShard::TEvClusterTreeRecalculateResponse>(handle);

            NYql::TIssues issues;
            NYql::IssuesFromMessage(reply->Record.GetIssues(), issues);
            UNIT_ASSERT_EQUAL_C(reply->Record.GetStatus(), NKikimrIndexBuilder::EBuildStatus::DONE,
                                issues.ToOneLineString());

            UNIT_ASSERT((size_t)reply->Record.ClusterIdsSize() == clusterIds.size());
            UNIT_ASSERT((size_t)reply->Record.ClustersSize() == clusterIds.size());
            UNIT_ASSERT((size_t)reply->Record.ClusterSizesSize() == clusterIds.size());

            for (size_t i = 0; i < (size_t)reply->Record.ClusterIdsSize(); i++) {
                data.Out << "cluster " << reply->Record.GetClusterIds(i)
                         << " = " << reply->Record.GetClusters(i)
                         << " size = " << reply->Record.GetClusterSizes(i) << "\n";
            }
        }

        return data;
    }

    Y_UNIT_TEST(BadRequest) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        serverSettings.SetDomainName("Root");

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto& runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_DEBUG);
        runtime.SetLogPriority(NKikimrServices::BUILD_INDEX, NLog::PRI_TRACE);

        InitRoot(server, sender);

        TShardedTableOptions options;
        options.Shards(1);
        CreateBuildTable(server, sender, options, "table-main");

        DoBadRequest(server, sender, [](NKikimrTxDataShard::TEvClusterTreeRecalculateRequest& request) {
            request.SetTabletId(0);
        }, TStringBuilder() << "{ <main>: Error: Wrong shard 0 this is " << GetTableShards(server, sender, kMainTable)[0] << " }");
        DoBadRequest(server, sender, [](NKikimrTxDataShard::TEvClusterTreeRecalculateRequest& request) {
            TPathId(0, 0).ToProto(request.MutablePathId());
        }, "{ <main>: Error: Unknown table id: 0 }");

        DoBadRequest(server, sender, [](NKikimrTxDataShard::TEvClusterTreeRecalculateRequest& request) {
            request.SetSnapshotStep(request.GetSnapshotStep() + 1);
        }, "Error: Unknown snapshot", true);
        DoBadRequest(server, sender, [](NKikimrTxDataShard::TEvClusterTreeRecalculateRequest& request) {
            request.SetSnapshotTxId(request.GetSnapshotTxId() + 1);
        }, "Error: Unknown snapshot", true);

        DoBadRequest(server, sender, [](NKikimrTxDataShard::TEvClusterTreeRecalculateRequest& request) {
            request.MutableSettings()->set_vector_type(VectorIndexSettings::VECTOR_TYPE_UNSPECIFIED);
        }, "{ <main>: Error: vector_type should be set }");
        DoBadRequest(server, sender, [](NKikimrTxDataShard::TEvClusterTreeRecalculateRequest& request) {
            request.MutableSettings()->set_metric(VectorIndexSettings::METRIC_UNSPECIFIED);
        }, "{ <main>: Error: either distance or similarity should be set }");

        DoBadRequest(server, sender, [](NKikimrTxDataShard::TEvClusterTreeRecalculateRequest& request) {
            request.ClearClusterIds();
        }, "{ <main>: Error: Should be requested for at least one cluster }");

        DoBadRequest(server, sender, [](NKikimrTxDataShard::TEvClusterTreeRecalculateRequest& request) {
            request.SetEmbeddingColumn("some");
        }, "{ <main>: Error: Unknown embedding column: some }");

        // Multiple issues
        DoBadRequest(server, sender, [](NKikimrTxDataShard::TEvClusterTreeRecalculateRequest& request) {
            request.ClearClusterIds();
            request.SetEmbeddingColumn("some");
        }, "[ { <main>: Error: Unknown embedding column: some } { <main>: Error: Should be requested for at least one cluster } ]");
    }

    Y_UNIT_TEST(BasicRecalculate) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        serverSettings.SetDomainName("Root");

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto& runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_DEBUG);
        runtime.SetLogPriority(NKikimrServices::BUILD_INDEX, NLog::PRI_TRACE);

        InitRoot(server, sender);

        TShardedTableOptions options;
        options.Shards(1);
        CreateBuildTable(server, sender, options, "table-main");

        // Insert data for 2 clusters: cluster 10 (3 rows) and cluster 11 (3 rows)
        ExecSQL(server, sender,
                R"(UPSERT INTO `/Root/table-main`
                    (__ydb_parent, key, embedding, data) VALUES
                    (10, 1, "\x30\x30\x02", "one"),
                    (10, 2, "\x31\x28\x02", "two"),
                    (10, 3, "\x29\x31\x02", "three"),
                    (11, 4, "\x20\x40\x02", "four"),
                    (11, 5, "\x15\x40\x02", "five"),
                    (11, 6, "\x10\x40\x02", "six");)");

        // Recalculate without providing old centroids
        auto result = DoRecalculate(server, sender, 0,
            {10, 11}, {}, VectorIndexSettings::VECTOR_TYPE_UINT8, VectorIndexSettings::DISTANCE_MANHATTAN);
        UNIT_ASSERT_VALUES_EQUAL(result,
            "cluster 10 = \x2E\x2D\2 size = 3\n"
            "cluster 11 = \x17\x40\2 size = 3\n");

        // Recalculate again with the same data and verify idempotent result
        result = DoRecalculate(server, sender, 0,
            {10, 11}, {}, VectorIndexSettings::VECTOR_TYPE_UINT8, VectorIndexSettings::DISTANCE_MANHATTAN);
        UNIT_ASSERT_VALUES_EQUAL(result,
            "cluster 10 = \x2E\x2D\2 size = 3\n"
            "cluster 11 = \x17\x40\2 size = 3\n");

        // Test with old centroids provided
        result = DoRecalculate(server, sender, 0,
            {10, 11}, {"\x30\x30\x02", "\x10\x40\x02"},
            VectorIndexSettings::VECTOR_TYPE_UINT8, VectorIndexSettings::DISTANCE_MANHATTAN);
        UNIT_ASSERT_VALUES_EQUAL(result,
            "cluster 10 = \x2E\x2D\2 size = 3\n"
            "cluster 11 = \x17\x40\2 size = 3\n");
    }

    Y_UNIT_TEST(DifferentMetrics) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        serverSettings.SetDomainName("Root");

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto& runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_DEBUG);
        runtime.SetLogPriority(NKikimrServices::BUILD_INDEX, NLog::PRI_TRACE);

        InitRoot(server, sender);

        TShardedTableOptions options;
        options.Shards(1);
        CreateBuildTable(server, sender, options, "table-main");

        ExecSQL(server, sender,
                R"(UPSERT INTO `/Root/table-main`
                    (__ydb_parent, key, embedding, data) VALUES
                    (10, 1, "\x30\x30\x02", "one"),
                    (10, 2, "\x31\x28\x02", "two"),
                    (10, 3, "\x29\x31\x02", "three"),
                    (11, 4, "\x20\x40\x02", "four"),
                    (11, 5, "\x15\x40\x02", "five"),
                    (11, 6, "\x10\x40\x02", "six");)");

        // Recalculate is metric-independent: it computes mean embeddings per cluster ID
        // rather than assigning points by distance. All metrics produce the same result.
        auto expected =
            "cluster 10 = \x2E\x2D\2 size = 3\n"
            "cluster 11 = \x17\x40\2 size = 3\n";

        auto result = DoRecalculate(server, sender, 0,
            {10, 11}, {}, VectorIndexSettings::VECTOR_TYPE_UINT8, VectorIndexSettings::DISTANCE_MANHATTAN);
        UNIT_ASSERT_VALUES_EQUAL(result, expected);

        result = DoRecalculate(server, sender, 0,
            {10, 11}, {}, VectorIndexSettings::VECTOR_TYPE_UINT8, VectorIndexSettings::DISTANCE_COSINE);
        UNIT_ASSERT_VALUES_EQUAL(result, expected);

        result = DoRecalculate(server, sender, 0,
            {10, 11}, {}, VectorIndexSettings::VECTOR_TYPE_UINT8, VectorIndexSettings::SIMILARITY_COSINE);
        UNIT_ASSERT_VALUES_EQUAL(result, expected);

        result = DoRecalculate(server, sender, 0,
            {10, 11}, {}, VectorIndexSettings::VECTOR_TYPE_UINT8, VectorIndexSettings::SIMILARITY_INNER_PRODUCT);
        UNIT_ASSERT_VALUES_EQUAL(result, expected);
    }

    Y_UNIT_TEST(EmptyCluster) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        serverSettings.SetDomainName("Root");

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto& runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_DEBUG);
        runtime.SetLogPriority(NKikimrServices::BUILD_INDEX, NLog::PRI_TRACE);

        InitRoot(server, sender);

        TShardedTableOptions options;
        options.Shards(1);
        CreateBuildTable(server, sender, options, "table-main");

        // Insert data for cluster 10 only, cluster 11 has no data
        ExecSQL(server, sender,
                R"(UPSERT INTO `/Root/table-main`
                    (__ydb_parent, key, embedding, data) VALUES
                    (10, 1, "\x30\x30\x02", "one"),
                    (10, 2, "\x31\x28\x02", "two"),
                    (10, 3, "\x29\x31\x02", "three");)");

        auto result = DoRecalculate(server, sender, 0,
            {10, 11}, {}, VectorIndexSettings::VECTOR_TYPE_UINT8, VectorIndexSettings::SIMILARITY_COSINE);

        // Check cluster 10 (non-empty): centroid \x2E\x2D\x02, size 3
        UNIT_ASSERT_STRING_CONTAINS(result, "cluster 10 = ");
        UNIT_ASSERT_STRING_CONTAINS(result, " size = 3");
        // Check cluster 11 (empty): centroid \x00\x00\x02, size 0
        UNIT_ASSERT_STRING_CONTAINS(result, "cluster 11 = ");
        UNIT_ASSERT_STRING_CONTAINS(result, " size = 0");

        // Verify the bytes of cluster 11 centroid (empty cluster = NUL-filled)
        auto pos = result.find("cluster 11 = ");
        UNIT_ASSERT(pos != TString::npos);
        pos += TString("cluster 11 = ").size();
        // Centroid should be 3 bytes: two NULs + format byte 0x02
        UNIT_ASSERT(pos + 3 <= result.size());
        const char* centroid = result.data() + pos;
        UNIT_ASSERT_VALUES_EQUAL((int)(ui8)centroid[0], 0);
        UNIT_ASSERT_VALUES_EQUAL((int)(ui8)centroid[1], 0);
        UNIT_ASSERT_VALUES_EQUAL((int)(ui8)centroid[2], 2);
    }

    Y_UNIT_TEST(SingleCluster) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        serverSettings.SetDomainName("Root");

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto& runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_DEBUG);
        runtime.SetLogPriority(NKikimrServices::BUILD_INDEX, NLog::PRI_TRACE);

        InitRoot(server, sender);

        TShardedTableOptions options;
        options.Shards(1);
        CreateBuildTable(server, sender, options, "table-main");

        // Insert data for cluster 10 only
        ExecSQL(server, sender,
                R"(UPSERT INTO `/Root/table-main`
                    (__ydb_parent, key, embedding, data) VALUES
                    (10, 1, "\x30\x30\x02", "one"),
                    (10, 2, "\x40\x20\x02", "two"),
                    (10, 3, "\x50\x10\x02", "three");)");

        auto result = DoRecalculate(server, sender, 0,
            {10}, {}, VectorIndexSettings::VECTOR_TYPE_UINT8, VectorIndexSettings::DISTANCE_COSINE);
        UNIT_ASSERT_VALUES_EQUAL(result,
            "cluster 10 = \x40\x20\2 size = 3\n");
    }

    Y_UNIT_TEST(InvalidEmbeddingWarning) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        serverSettings.SetDomainName("Root");

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto& runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_DEBUG);
        runtime.SetLogPriority(NKikimrServices::BUILD_INDEX, NLog::PRI_TRACE);

        InitRoot(server, sender);

        TShardedTableOptions options;
        options.Shards(1);
        CreateBuildTable(server, sender, options, "table-main");

        // 2 valid rows, 3 invalid rows (no format byte \x02)
        ExecSQL(server, sender,
            R"(UPSERT INTO `/Root/table-main` (__ydb_parent, key, embedding, data) VALUES )"
            "(10, 1, \"\x30\x30\x02\", \"one\"),"
            "(10, 2, \"\x31\x31\x02\", \"two\"),"
            "(10, 3, \"invalid\", \"three\"),"
            "(10, 4, \"\", \"four\"),"
            "(10, 5, \"bad\", \"five\");");

        auto id = sId.fetch_add(1, std::memory_order_relaxed);
        auto snapshot = CreateVolatileSnapshot(server, {kMainTable});
        auto datashards = GetTableShards(server, sender, kMainTable);
        TTableId tableId = ResolveTableId(server, sender, kMainTable);
        auto tid = datashards[0];

        auto ev = std::make_unique<TEvDataShard::TEvClusterTreeRecalculateRequest>();
        auto& rec = ev->Record;
        rec.SetId(1);
        rec.SetSeqNoGeneration(id);
        rec.SetSeqNoRound(1);
        rec.SetTabletId(tid);
        tableId.PathId.ToProto(rec.MutablePathId());
        rec.SetSnapshotTxId(snapshot.TxId);
        rec.SetSnapshotStep(snapshot.Step);

        VectorIndexSettings settings;
        settings.set_vector_dimension(2);
        settings.set_vector_type(VectorIndexSettings::VECTOR_TYPE_UINT8);
        settings.set_metric(VectorIndexSettings::DISTANCE_COSINE);
        *rec.MutableSettings() = settings;

        rec.SetParent(0);
        rec.AddClusterIds(10);
        rec.SetEmbeddingColumn("embedding");

        runtime.SendToPipe(tid, sender, ev.release(), 0, GetPipeConfigWithRetries());

        TAutoPtr<IEventHandle> handle;
        auto reply = runtime.GrabEdgeEventRethrow<TEvDataShard::TEvClusterTreeRecalculateResponse>(handle);

        UNIT_ASSERT_EQUAL(reply->Record.GetStatus(), NKikimrIndexBuilder::EBuildStatus::DONE);

        NYql::TIssues issues;
        NYql::IssuesFromMessage(reply->Record.GetIssues(), issues);
        TString issuesStr = issues.ToOneLineString();
        UNIT_ASSERT_STRING_CONTAINS(issuesStr, "3 row(s) with invalid vector format were skipped during cluster tree recalculation");
    }

    Y_UNIT_TEST(PostingParentFlag) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        serverSettings.SetDomainName("Root");

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto& runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_DEBUG);
        runtime.SetLogPriority(NKikimrServices::BUILD_INDEX, NLog::PRI_TRACE);

        InitRoot(server, sender);

        TShardedTableOptions options;
        options.Shards(1);
        CreateBuildTable(server, sender, options, "table-main");

        // Insert data with PostingParentFlag set on __ydb_parent values.
        // SetPostingParentFlag(n) = n | (1ull << 63) = n + 9223372036854775808
        // Cluster 10: 9223372036854775818
        // Cluster 11: 9223372036854775819
        ExecSQL(server, sender,
            R"(UPSERT INTO `/Root/table-main`
                (__ydb_parent, key, embedding, data) VALUES
                (9223372036854775818, 1, "\x30\x30\x02", "one"),
                (9223372036854775818, 2, "\x31\x28\x02", "two"),
                (9223372036854775818, 3, "\x29\x31\x02", "three"),
                (9223372036854775819, 4, "\x20\x40\x02", "four"),
                (9223372036854775819, 5, "\x15\x40\x02", "five"),
                (9223372036854775819, 6, "\x10\x40\x02", "six");)");

        auto result = DoRecalculate(server, sender, 0,
            {10, 11}, {}, VectorIndexSettings::VECTOR_TYPE_UINT8, VectorIndexSettings::DISTANCE_MANHATTAN);
        UNIT_ASSERT_VALUES_EQUAL(result,
            "cluster 10 = \x2E\x2D\2 size = 3\n"
            "cluster 11 = \x17\x40\2 size = 3\n");
    }

    Y_UNIT_TEST(MultipleShards) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        serverSettings.SetDomainName("Root");

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto& runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_DEBUG);
        runtime.SetLogPriority(NKikimrServices::BUILD_INDEX, NLog::PRI_TRACE);

        InitRoot(server, sender);

        TShardedTableOptions options;
        options.Shards(3);
        CreateBuildTable(server, sender, options, "table-main");

        // Insert data across multiple shards for clusters 10 and 11
        ExecSQL(server, sender,
                R"(UPSERT INTO `/Root/table-main`
                    (__ydb_parent, key, embedding, data) VALUES
                    (10, 1, "\x30\x30\x02", "one"),
                    (10, 2, "\x31\x28\x02", "two"),
                    (11, 3, "\x20\x40\x02", "four"),
                    (11, 4, "\x10\x40\x02", "six"),
                    (10, 5, "\x29\x31\x02", "three"),
                    (11, 6, "\x15\x40\x02", "five");)");

        auto result = DoRecalculate(server, sender, 0,
            {10, 11}, {}, VectorIndexSettings::VECTOR_TYPE_UINT8, VectorIndexSettings::DISTANCE_MANHATTAN);

        // With 3 shards we get separate results per shard. Verify the
        // operation completed and returned cluster data from all shards.
        UNIT_ASSERT(!result.empty());
        UNIT_ASSERT_STRING_CONTAINS(result, "cluster 10");
        UNIT_ASSERT_STRING_CONTAINS(result, "cluster 11");
    }

}

} // namespace NKikimr
