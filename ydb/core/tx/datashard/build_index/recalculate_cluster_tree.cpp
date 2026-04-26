#include "kmeans_helper.h"
#include "../datashard_impl.h"
#include "../scan_common.h"
#include "../buffer_data.h"

#include <ydb/core/base/appdata.h>
#include <ydb/core/base/counters.h>
#include <ydb/core/scheme/scheme_tablecell.h>

#include <ydb/core/tx/tx_proxy/proxy.h>

#include <ydb/core/ydb_convert/table_description.h>
#include <ydb/core/ydb_convert/ydb_convert.h>
#include <yql/essentials/public/issue/yql_issue_message.h>

#include <util/generic/algorithm.h>
#include <util/string/builder.h>

namespace NKikimr::NDataShard {
using namespace NKMeans;

/*
 * TClusterTreeRecalculateScan recomputes the cluster tree centroids from the posting table data.
 * It scans the posting table for specified leaf clusters, aggregates embeddings per cluster ID,
 * and computes new centroids (mean embedding per cluster).
 *
 * Request:
 * - The client sends TEvClusterTreeRecalculateRequest with:
 *   - PathId: the table to scan (posting table or build table)
 *   - Parent: parent cluster ID for range filtering
 *   - ClusterIds: IDs of the leaf clusters to recalculate
 *   - Clusters: current centroids (same order as ClusterIds)
 *   - Settings: vector index settings (type, dimension, metric)
 *   - EmbeddingColumn: name of the embedding column
 *
 * Execution Flow:
 * - TClusterTreeRecalculateScan scans the relevant table range
 * - For each input row:
 *   - Extract __ydb_parent from key[0] (clear posting parent flag)
 *   - Look up the cluster index from ClusterIds
 *   - Aggregate the embedding to the cluster
 * - After all rows processed, compute new centroids
 * - Return new centroids with cluster IDs and sizes
 */

class TClusterTreeRecalculateScan: public TActor<TClusterTreeRecalculateScan>, public IActorExceptionHandler, public NTable::IScan {
protected:
    const ui64 TabletId = 0;
    const ui64 BuildId = 0;
    const TAutoPtr<TEvDataShard::TEvClusterTreeRecalculateResponse> Response;
    const TActorId ResponseActorId;

    TTags ScanTags;
    NTable::TPos EmbeddingPos = 0;

    ui64 ReadRows = 0;
    ui64 ReadBytes = 0;
    ui64 InvalidEmbeddingRows = 0;

    IDriver* Driver = nullptr;
    NYql::TIssues Issues;

    TLead Lead;

    // Map from cluster ID to index in the Clusters array
    TMap<ui64, ui32> ClusterIdToIndex;
    std::unique_ptr<IClusters> Clusters;

public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType()
    {
        return NKikimrServices::TActivity::CLUSTER_TREE_RECALCULATE_SCAN_ACTOR;
    }

    TClusterTreeRecalculateScan(ui64 tabletId, const TUserTable& table, TLead&& lead,
        const NKikimrTxDataShard::TEvClusterTreeRecalculateRequest& request,
        const TActorId& responseActorId, TAutoPtr<TEvDataShard::TEvClusterTreeRecalculateResponse>&& response,
        std::unique_ptr<IClusters>&& clusters)
        : TActor(&TThis::StateWork)
        , TabletId(tabletId)
        , BuildId(request.GetId())
        , Response(std::move(response))
        , ResponseActorId(responseActorId)
        , Lead(std::move(lead))
        , Clusters(std::move(clusters))
    {
        LOG_I("Create " << Debug());

        const ui32 numClusters = request.ClusterIdsSize();
        Y_ENSURE(numClusters > 0);

        // Build cluster ID to index mapping
        for (ui32 i = 0; i < numClusters; i++) {
            ClusterIdToIndex[request.GetClusterIds(i)] = i;
        }

        // If no centroids provided in the request, initialize with empty vectors
        if (request.ClustersSize() == 0) {
            TVector<TString> emptyClusters;
            emptyClusters.reserve(numClusters);
            for (ui32 i = 0; i < numClusters; i++) {
                emptyClusters.push_back(Clusters->GetEmptyRow());
            }
            Clusters->SetClusters(std::move(emptyClusters));
        } else {
            Y_ENSURE((size_t)request.ClustersSize() == numClusters);
            Clusters->SetClusters(TVector<TString>{request.GetClusters().begin(), request.GetClusters().end()});
        }

        const auto& embedding = request.GetEmbeddingColumn();
        NTable::TPos dataPos = 0;
        ScanTags = MakeScanTags(table, embedding, {}, false, EmbeddingPos, dataPos);
        Lead.SetTags(ScanTags);
    }

    TInitialState Prepare(IDriver* driver, TIntrusiveConstPtr<TScheme>) final
    {
        TActivationContext::AsActorContext().RegisterWithSameMailbox(this);
        LOG_I("Prepare " << Debug());

        Driver = driver;

        return {EScan::Feed, {}};
    }

    TAutoPtr<IDestructable> Finish(const std::exception& exc) final
    {
        Issues.AddIssue(NYql::TIssue(TStringBuilder()
            << "Scan failed " << exc.what()));
        return Finish(EStatus::Exception);
    }

    TAutoPtr<IDestructable> Finish(EStatus status) final
    {
        auto& record = Response->Record;
        record.MutableMeteringStats()->SetReadRows(ReadRows);
        record.MutableMeteringStats()->SetReadBytes(ReadBytes);
        record.MutableMeteringStats()->SetCpuTimeUs(Driver->GetTotalCpuTimeUs());

        if (status == EStatus::Exception) {
            record.SetStatus(NKikimrIndexBuilder::EBuildStatus::BUILD_ERROR);
        } else if (status != NTable::EStatus::Done) {
            record.SetStatus(NKikimrIndexBuilder::EBuildStatus::ABORTED);
        } else {
            record.SetStatus(NKikimrIndexBuilder::EBuildStatus::DONE);
            FillResponse();
        }

        if (InvalidEmbeddingRows > 0) {
            Issues.AddIssue(NYql::TIssue(TStringBuilder()
                << InvalidEmbeddingRows << " row(s) with invalid vector format were skipped during cluster tree recalculation")
                .SetCode(NYql::DEFAULT_ERROR, NYql::TSeverityIds::S_WARNING));
        }
        NYql::IssuesToMessage(Issues, record.MutableIssues());

        if (Response->Record.GetStatus() == NKikimrIndexBuilder::DONE) {
            LOG_N("Done " << Debug() << " " << ToShortDebugString(Response->Record));
        } else {
            LOG_E("Failed " << Debug() << " " << ToShortDebugString(Response->Record));
        }
        Send(ResponseActorId, Response.Release());

        Driver = nullptr;
        this->PassAway();
        return nullptr;
    }

    bool OnUnhandledException(const std::exception& exc) final
    {
        if (!Driver) {
            return false;
        }
        Driver->Throw(exc);
        return true;
    }

    void Describe(IOutputStream& out) const final
    {
        out << Debug();
    }

    EScan Seek(TLead& lead, ui64 seq) final
    {
        LOG_T("Seek " << seq << " " << Debug());

        lead = Lead;

        return EScan::Feed;
    }

    EScan Feed(TArrayRef<const TCell> key, const TRow& row) final
    {
        ++ReadRows;
        ReadBytes += CountRowCellBytes(key, *row);

        // Extract __ydb_parent from key[0] (first key column)
        Y_ENSURE(key.size() >= 1);
        const ui64 parentRaw = key[0].AsValue<ui64>();
        const ui64 clusterId = parentRaw & ~PostingParentFlag;

        // Check if this cluster is in our target list
        const auto* idx = ClusterIdToIndex.FindPtr(clusterId);
        if (!idx) {
            return EScan::Feed;
        }

        if (!Clusters->IsExpectedFormat(row.Get(EmbeddingPos).AsRef())) {
            ++InvalidEmbeddingRows;
            return EScan::Feed;
        }

        Clusters->AggregateToCluster(*idx, row.Get(EmbeddingPos).AsRef());

        return EScan::Feed;
    }

    EScan Exhausted() final
    {
        LOG_T("Exhausted " << Debug());

        return EScan::Final;
    }

protected:
    STFUNC(StateWork) {
        switch (ev->GetTypeRewrite()) {
            default:
                LOG_E("StateWork unexpected event type: " << ev->GetTypeRewrite()
                    << " event: " << ev->ToString() << " " << Debug());
        }
    }

    TString Debug() const
    {
        return TStringBuilder() << "TClusterTreeRecalculateScan TabletId: " << TabletId << " Id: " << BuildId
            << " " << Clusters->Debug();
    }

    void FillResponse()
    {
        auto& record = Response->Record;
        Clusters->RecomputeClusters();
        const auto& clusters = Clusters->GetClusters();
        const auto& sizes = Clusters->GetNextClusterSizes();

        // Fill in the original ClusterIds from the request
        // The request and response should maintain the same order
        for (const auto& [clusterId, idx] : ClusterIdToIndex) {
            record.AddClusterIds(clusterId);
            record.AddClusters(clusters[idx]);
            record.AddClusterSizes(sizes[idx]);
        }
        record.SetStatus(NKikimrIndexBuilder::EBuildStatus::DONE);
    }
};

class TDataShard::TTxHandleSafeClusterTreeRecalculateScan final: public NTabletFlatExecutor::TTransactionBase<TDataShard> {
public:
    TTxHandleSafeClusterTreeRecalculateScan(TDataShard* self, TEvDataShard::TEvClusterTreeRecalculateRequest::TPtr&& ev)
        : TTransactionBase(self)
        , Ev(std::move(ev))
    {
    }

    bool Execute(TTransactionContext&, const TActorContext& ctx) final
    {
        Self->HandleSafe(Ev, ctx);
        return true;
    }

    void Complete(const TActorContext&) final
    {
    }

private:
    TEvDataShard::TEvClusterTreeRecalculateRequest::TPtr Ev;
};

void TDataShard::Handle(TEvDataShard::TEvClusterTreeRecalculateRequest::TPtr& ev, const TActorContext&)
{
    Execute(new TTxHandleSafeClusterTreeRecalculateScan(this, std::move(ev)));
}

void TDataShard::HandleSafe(TEvDataShard::TEvClusterTreeRecalculateRequest::TPtr& ev, const TActorContext& ctx)
{
    auto& request = ev->Get()->Record;
    const ui64 id = request.GetId();
    auto rowVersion = request.HasSnapshotStep() || request.HasSnapshotTxId()
        ? TRowVersion(request.GetSnapshotStep(), request.GetSnapshotTxId())
        : GetMvccTxVersion(EMvccTxMode::ReadOnly);
    TScanRecord::TSeqNo seqNo = {request.GetSeqNoGeneration(), request.GetSeqNoRound()};

    try {
        auto response = MakeHolder<TEvDataShard::TEvClusterTreeRecalculateResponse>();
        FillScanResponseCommonFields(*response, id, TabletID(), seqNo);

        LOG_N("Starting TClusterTreeRecalculateScan TabletId: " << TabletID()
            << " " << ToShortDebugString(request)
            << " row version " << rowVersion);

        if (VolatileTxManager.HasVolatileTxsAtSnapshot(rowVersion)) {
            VolatileTxManager.AttachWaitingSnapshotEvent(rowVersion, std::unique_ptr<IEventHandle>(ev.Release()));
            return;
        }

        auto badRequest = [&](const TString& error) {
            response->Record.SetStatus(NKikimrIndexBuilder::EBuildStatus::BAD_REQUEST);
            auto issue = response->Record.AddIssues();
            issue->set_severity(NYql::TSeverityIds::S_ERROR);
            issue->set_message(error);
        };
        auto trySendBadRequest = [&] {
            if (response->Record.GetStatus() == NKikimrIndexBuilder::EBuildStatus::BAD_REQUEST) {
                LOG_E("Rejecting TClusterTreeRecalculateScan bad request TabletId: " << TabletID()
                    << " " << ToShortDebugString(request)
                    << " with response " << ToShortDebugString(response->Record));
                ctx.Send(ev->Sender, std::move(response));
                return true;
            } else {
                return false;
            }
        };

        // 1. Validating table and path existence
        if (request.GetTabletId() != TabletID()) {
            badRequest(TStringBuilder() << "Wrong shard " << request.GetTabletId() << " this is " << TabletID());
        }
        if (!IsStateActive()) {
            badRequest(TStringBuilder() << "Shard " << TabletID() << " is " << State << " and not ready for requests");
        }
        const auto pathId = TPathId::FromProto(request.GetPathId());
        const auto* userTableIt = GetUserTables().FindPtr(pathId.LocalPathId);
        if (!userTableIt) {
            badRequest(TStringBuilder() << "Unknown table id: " << pathId.LocalPathId);
        }
        if (trySendBadRequest()) {
            return;
        }
        const auto& userTable = **userTableIt;

        // 2. Validating request fields
        if (request.HasSnapshotStep() || request.HasSnapshotTxId()) {
            const TSnapshotKey snapshotKey(pathId, rowVersion.Step, rowVersion.TxId);
            if (!SnapshotManager.FindAvailable(snapshotKey)) {
                badRequest(TStringBuilder() << "Unknown snapshot for path id " << pathId.OwnerId << ":" << pathId.LocalPathId
                    << ", snapshot step is " << snapshotKey.Step << ", snapshot tx is " << snapshotKey.TxId);
            }
        }

        const auto parent = request.GetParent();
        NTable::TLead lead;
        if (parent == 0) {
            lead.To({}, NTable::ESeek::Lower);
        } else {
            TCell from, to;
            const auto range = CreateRangeFrom(userTable, parent, from, to);
            if (range.IsEmptyRange(userTable.KeyColumnTypes)) {
                badRequest(TStringBuilder() << " requested range doesn't intersect with table range");
            }
            lead = CreateLeadFrom(range);
        }

        auto tags = GetAllTags(userTable);
        if (!tags.contains(request.GetEmbeddingColumn())) {
            badRequest(TStringBuilder() << "Unknown embedding column: " << request.GetEmbeddingColumn());
        }

        // 3. Validating vector index settings
        TString error;
        auto clusters = NKikimr::NKMeans::CreateClusters(request.GetSettings(), 0, error);
        if (!clusters) {
            badRequest(error);
        } else if (request.ClusterIdsSize() < 1) {
            badRequest("Should be requested for at least one cluster");
        } else if (request.ClustersSize() != 0 && request.ClustersSize() != request.ClusterIdsSize()) {
            badRequest("ClusterIds and Clusters sizes must match");
        }

        if (trySendBadRequest()) {
            return;
        }

        TAutoPtr<NTable::IScan> scan = new TClusterTreeRecalculateScan(
            TabletID(), userTable, std::move(lead), request, ev->Sender, std::move(response), std::move(clusters)
        );

        StartScan(this, std::move(scan), id, seqNo, rowVersion, userTable.LocalTid);
    } catch (const std::exception& exc) {
        FailScan<TEvDataShard::TEvClusterTreeRecalculateResponse>(id, TabletID(), ev->Sender, seqNo, exc, "TClusterTreeRecalculateScan");
    }
}

}
