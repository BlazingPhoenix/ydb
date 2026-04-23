#include <ydb/core/base/path.h>
#include <ydb/core/change_exchange/change_exchange.h>
#include <ydb/core/scheme/scheme_tablecell.h>
#include <ydb/core/testlib/tablet_helpers.h>
#include <ydb/core/tx/schemeshard/index/index_utils.h>
#include <ydb/core/tx/schemeshard/ut_helpers/helpers.h>


using namespace NKikimr;
using namespace NSchemeShard;
using namespace NSchemeShardUT_Private;
using namespace NKikimr::NTableIndex;
using namespace NKikimr::NTableIndex::NFulltext;

Y_UNIT_TEST_SUITE(TPostingIndexTests) {
    Y_UNIT_TEST(CreateTable) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TString fulltextSettings = R"(
            columns: {
                column: "text"
                analyzers: {
                    tokenizer: STANDARD
                    use_filter_lowercase: true
                }
            }
        )";
        TestCreateIndexedTable(runtime, ++txId, "/MyRoot", Sprintf(R"(
            TableDescription {
                Name: "texts"
                Columns { Name: "id" Type: "Uint64" }
                Columns { Name: "text" Type: "String" }
                Columns { Name: "covered" Type: "String" }
                Columns { Name: "another" Type: "Uint64" }
                KeyColumnNames: ["id"]
            }
            IndexDescription {
                Name: "idx_posting"
                KeyColumnNames: ["text"]
                DataColumnNames: ["covered"]
                Type: EIndexTypeGlobalFulltextRelevance
                FulltextIndexDescription: {
                    Settings: {
                        %s
                    }
                }
            }
        )", fulltextSettings.c_str()));
        env.TestWaitNotification(runtime, txId);

        for (ui32 reboot = 0; reboot < 2; reboot++) {
            TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/texts/idx_posting"),{
                NLs::PathExist,
                NLs::IndexType(NKikimrSchemeOp::EIndexTypeGlobalFulltextRelevance),
                NLs::IndexState(NKikimrSchemeOp::EIndexStateReady),
                NLs::IndexKeys({"text"}),
                NLs::IndexDataColumns({"covered"}),
                NLs::SpecializedIndexDescription(fulltextSettings),
                NLs::ChildrenCount(5),
            });

            // Main impl table: for Relevance type, data columns are NOT included in impl table
            // Columns: token + PK + freq
            TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/texts/idx_posting/indexImplTable"),{
                NLs::PathExist,
                NLs::CheckColumns("indexImplTable",
                        { TokenColumn, FreqColumn, "id" }, {},
                        { TokenColumn, "id" }, true) });

            // Docs table: PK + covered + doc_length
            TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/texts/idx_posting/indexImplDocsTable"),{
                NLs::PathExist,
                NLs::CheckColumns("indexImplDocsTable",
                        { DocLengthColumn, "id", "covered" }, {},
                        { "id" }, true) });

            // Dict table: token + freq
            TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/texts/idx_posting/indexImplDictTable"),{
                NLs::PathExist,
                NLs::CheckColumns("indexImplDictTable",
                        { TokenColumn, FreqColumn }, {},
                        { TokenColumn }, true) });

            // Stats table: id + doc_count + sum_doc_length
            TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/texts/idx_posting/indexImplStatsTable"),{
                NLs::PathExist,
                NLs::CheckColumns("indexImplStatsTable",
                        { IdColumn, DocCountColumn, SumDocLengthColumn }, {},
                        { IdColumn }, true) });

            // LSM posting table: word_id + doc_id + freq + positions
            TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/texts/idx_posting/indexImplPostingTable"),{
                NLs::PathExist,
                NLs::CheckColumns("indexImplPostingTable",
                        { WordIdColumn, DocIdColumn, FreqColumn, PositionsColumn }, {},
                        { WordIdColumn, DocIdColumn }, true) });

            Cerr << "Reboot SchemeShard.." << Endl;
            TActorId sender = runtime.AllocateEdgeActor();
            RebootTablet(runtime, TTestTxConfig::SchemeShard, sender);
        }
    }

    Y_UNIT_TEST(CreateTableNoDataColumns) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TString fulltextSettings = R"(
            columns: {
                column: "text"
                analyzers: {
                    tokenizer: STANDARD
                    use_filter_lowercase: true
                }
            }
        )";
        TestCreateIndexedTable(runtime, ++txId, "/MyRoot", Sprintf(R"(
            TableDescription {
                Name: "texts"
                Columns { Name: "id" Type: "Uint64" }
                Columns { Name: "text" Type: "String" }
                KeyColumnNames: ["id"]
            }
            IndexDescription {
                Name: "idx_posting"
                KeyColumnNames: ["text"]
                Type: EIndexTypeGlobalFulltextRelevance
                FulltextIndexDescription: {
                    Settings: {
                        %s
                    }
                }
            }
        )", fulltextSettings.c_str()));
        env.TestWaitNotification(runtime, txId);

        TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/texts/idx_posting"),{
            NLs::PathExist,
            NLs::IndexType(NKikimrSchemeOp::EIndexTypeGlobalFulltextRelevance),
            NLs::IndexState(NKikimrSchemeOp::EIndexStateReady),
            NLs::IndexKeys({"text"}),
            NLs::IndexDataColumns({}),
            NLs::ChildrenCount(5),
        });

        // Main impl table: no data columns for Relevance, only token + PK + freq
        TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/texts/idx_posting/indexImplTable"),{
            NLs::PathExist,
            NLs::CheckColumns("indexImplTable",
                    { TokenColumn, FreqColumn, "id" }, {},
                    { TokenColumn, "id" }, true) });

        // Docs table: PK + doc_length (no covered columns)
        TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/texts/idx_posting/indexImplDocsTable"),{
            NLs::PathExist,
            NLs::CheckColumns("indexImplDocsTable",
                    { DocLengthColumn, "id" }, {},
                    { "id" }, true) });

        // LSM posting table always has word_id + doc_id + freq + positions
        TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/texts/idx_posting/indexImplPostingTable"),{
            NLs::PathExist,
            NLs::CheckColumns("indexImplPostingTable",
                    { WordIdColumn, DocIdColumn, FreqColumn, PositionsColumn }, {},
                    { WordIdColumn, DocIdColumn }, true) });
    }

    Y_UNIT_TEST(CreateTablePrefix) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TString fulltextSettings = R"(
            columns: {
                column: "text"
                analyzers: {
                    tokenizer: STANDARD
                    use_filter_lowercase: true
                }
            }
        )";
        TestCreateIndexedTable(runtime, ++txId, "/MyRoot", Sprintf(R"(
            TableDescription {
                Name: "texts"
                Columns { Name: "id" Type: "Uint64" }
                Columns { Name: "text" Type: "String" }
                Columns { Name: "covered" Type: "String" }
                Columns { Name: "another" Type: "Uint64" }
                KeyColumnNames: ["id"]
            }
            IndexDescription {
                Name: "idx_posting"
                KeyColumnNames: [ "another", "text"]
                DataColumnNames: ["covered"]
                Type: EIndexTypeGlobalFulltextRelevance
                FulltextIndexDescription: {
                    Settings: {
                        %s
                    }
                }
            }
        )", fulltextSettings.c_str()), {NKikimrScheme::StatusInvalidParameter});
        env.TestWaitNotification(runtime, txId);

        TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/texts/idx_posting"),{
            NLs::PathNotExist,
        });
    }

    Y_UNIT_TEST(CreateTableMultipleColumns) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TString fulltextSettings = R"(
            columns: {
                column: "text1"
                analyzers: {
                    tokenizer: STANDARD
                    use_filter_lowercase: true
                }
            }
            columns: {
                column: "text2"
                analyzers: {
                    tokenizer: STANDARD
                    use_filter_lowercase: true
                }
            }
        )";
        TestCreateIndexedTable(runtime, ++txId, "/MyRoot", Sprintf(R"(
            TableDescription {
                Name: "texts"
                Columns { Name: "id" Type: "Uint64" }
                Columns { Name: "text1" Type: "String" }
                Columns { Name: "text2" Type: "String" }
                Columns { Name: "covered" Type: "String" }
                KeyColumnNames: ["id"]
            }
            IndexDescription {
                Name: "idx_posting"
                KeyColumnNames: ["text1", "text2"]
                DataColumnNames: ["covered"]
                Type: EIndexTypeGlobalFulltextRelevance
                FulltextIndexDescription: {
                    Settings: {
                        %s
                    }
                }
            }
        )", fulltextSettings.c_str()), {NKikimrScheme::StatusInvalidParameter});
        env.TestWaitNotification(runtime, txId);

        TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/texts/idx_posting"),{
            NLs::PathNotExist,
        });
    }

    Y_UNIT_TEST(CreateTableNotText) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TString fulltextSettings = R"(
            columns: {
                column: "text"
                analyzers: {
                    tokenizer: STANDARD
                    use_filter_lowercase: true
                }
            }
        )";
        TestCreateIndexedTable(runtime, ++txId, "/MyRoot", Sprintf(R"(
            TableDescription {
                Name: "texts"
                Columns { Name: "id" Type: "Uint64" }
                Columns { Name: "text" Type: "Uint64" }
                Columns { Name: "covered" Type: "String" }
                KeyColumnNames: ["id"]
            }
            IndexDescription {
                Name: "idx_posting"
                KeyColumnNames: ["text"]
                DataColumnNames: ["covered"]
                Type: EIndexTypeGlobalFulltextRelevance
                FulltextIndexDescription: {
                    Settings: {
                        %s
                    }
                }
            }
        )", fulltextSettings.c_str()), {NKikimrScheme::StatusInvalidParameter});
        env.TestWaitNotification(runtime, txId);

        TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/texts/idx_posting"),{
            NLs::PathNotExist,
        });
    }

    Y_UNIT_TEST(CreateTableColumnsMismatch) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TString fulltextSettings = R"(
            columns: {
                column: "text_wrong"
                analyzers: {
                    tokenizer: STANDARD
                    use_filter_lowercase: true
                }
            }
        )";
        TestCreateIndexedTable(runtime, ++txId, "/MyRoot", Sprintf(R"(
            TableDescription {
                Name: "texts"
                Columns { Name: "id" Type: "Uint64" }
                Columns { Name: "text" Type: "String" }
                Columns { Name: "covered" Type: "String" }
                KeyColumnNames: ["id"]
            }
            IndexDescription {
                Name: "idx_posting"
                KeyColumnNames: ["text"]
                DataColumnNames: ["covered"]
                Type: EIndexTypeGlobalFulltextRelevance
                FulltextIndexDescription: {
                    Settings: {
                        %s
                    }
                }
            }
        )", fulltextSettings.c_str()), {NKikimrScheme::StatusInvalidParameter});
        env.TestWaitNotification(runtime, txId);

        TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/texts/idx_posting"),{
            NLs::PathNotExist,
        });
    }

    Y_UNIT_TEST(CreateTableNoColumnsSettings) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TString fulltextSettings = R"(
        )";
        TestCreateIndexedTable(runtime, ++txId, "/MyRoot", Sprintf(R"(
            TableDescription {
                Name: "texts"
                Columns { Name: "id" Type: "Uint64" }
                Columns { Name: "text" Type: "String" }
                Columns { Name: "covered" Type: "String" }
                KeyColumnNames: ["id"]
            }
            IndexDescription {
                Name: "idx_posting"
                KeyColumnNames: ["text"]
                DataColumnNames: ["covered"]
                Type: EIndexTypeGlobalFulltextRelevance
                FulltextIndexDescription: {
                    Settings: {
                        %s
                    }
                }
            }
        )", fulltextSettings.c_str()), {NKikimrScheme::StatusInvalidParameter});
        env.TestWaitNotification(runtime, txId);

        TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/texts/idx_posting"),{
            NLs::PathNotExist,
        });
    }

    Y_UNIT_TEST(CreateTableUnsupportedSettings) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TString fulltextSettings = R"(
            columns: {
                column: "text"
                analyzers: {
                    tokenizer: STANDARD
                    use_filter_edge_ngram: true
                }
            }
        )";
        TestCreateIndexedTable(runtime, ++txId, "/MyRoot", Sprintf(R"(
            TableDescription {
                Name: "texts"
                Columns { Name: "id" Type: "Uint64" }
                Columns { Name: "text" Type: "String" }
                Columns { Name: "covered" Type: "String" }
                KeyColumnNames: ["id"]
            }
            IndexDescription {
                Name: "idx_posting"
                KeyColumnNames: ["text"]
                DataColumnNames: ["covered"]
                Type: EIndexTypeGlobalFulltextRelevance
                FulltextIndexDescription: {
                    Settings: {
                        %s
                    }
                }
            }
        )", fulltextSettings.c_str()), {NKikimrScheme::StatusInvalidParameter});
        env.TestWaitNotification(runtime, txId);

        TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/texts/idx_posting"),{
            NLs::PathNotExist,
        });
    }

    Y_UNIT_TEST(CreateTableWithCompositePK) {
        // Fulltext relevance index requires exactly one PK column of type Uint64
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TString fulltextSettings = R"(
            columns: {
                column: "text"
                analyzers: {
                    tokenizer: STANDARD
                    use_filter_lowercase: true
                }
            }
        )";
        TestCreateIndexedTable(runtime, ++txId, "/MyRoot", Sprintf(R"(
            TableDescription {
                Name: "texts"
                Columns { Name: "id1" Type: "Uint64" }
                Columns { Name: "id2" Type: "Uint32" }
                Columns { Name: "text" Type: "String" }
                KeyColumnNames: ["id1", "id2"]
            }
            IndexDescription {
                Name: "idx_posting"
                KeyColumnNames: ["text"]
                Type: EIndexTypeGlobalFulltextRelevance
                FulltextIndexDescription: {
                    Settings: {
                        %s
                    }
                }
            }
        )", fulltextSettings.c_str()), {NKikimrScheme::StatusInvalidParameter});
        env.TestWaitNotification(runtime, txId);

        TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/texts/idx_posting"),{
            NLs::PathNotExist,
        });
    }

    Y_UNIT_TEST(CreateTableBothPlainAndRelevanceIndexes) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TString fulltextSettings = R"(
            columns: {
                column: "text"
                analyzers: {
                    tokenizer: STANDARD
                    use_filter_lowercase: true
                }
            }
        )";
        TestCreateIndexedTable(runtime, ++txId, "/MyRoot", Sprintf(R"(
            TableDescription {
                Name: "texts"
                Columns { Name: "id" Type: "Uint64" }
                Columns { Name: "text" Type: "String" }
                KeyColumnNames: ["id"]
            }
            IndexDescription {
                Name: "idx_plain"
                KeyColumnNames: ["text"]
                Type: EIndexTypeGlobalFulltextPlain
                FulltextIndexDescription: {
                    Settings: {
                        %s
                    }
                }
            }
            IndexDescription {
                Name: "idx_posting"
                KeyColumnNames: ["text"]
                Type: EIndexTypeGlobalFulltextRelevance
                FulltextIndexDescription: {
                    Settings: {
                        %s
                    }
                }
            }
        )", fulltextSettings.c_str(), fulltextSettings.c_str()));
        env.TestWaitNotification(runtime, txId);

        TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/texts/idx_plain"),{
            NLs::PathExist,
            NLs::IndexType(NKikimrSchemeOp::EIndexTypeGlobalFulltextPlain),
            NLs::ChildrenCount(1),
        });

        TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/texts/idx_posting"),{
            NLs::PathExist,
            NLs::IndexType(NKikimrSchemeOp::EIndexTypeGlobalFulltextRelevance),
            NLs::ChildrenCount(5),
        });
    }

    Y_UNIT_TEST(CreateTableUtf8Column) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TString fulltextSettings = R"(
            columns: {
                column: "text"
                analyzers: {
                    tokenizer: STANDARD
                    use_filter_lowercase: true
                }
            }
        )";
        TestCreateIndexedTable(runtime, ++txId, "/MyRoot", Sprintf(R"(
            TableDescription {
                Name: "texts"
                Columns { Name: "id" Type: "Uint64" }
                Columns { Name: "text" Type: "Utf8" }
                KeyColumnNames: ["id"]
            }
            IndexDescription {
                Name: "idx_posting"
                KeyColumnNames: ["text"]
                Type: EIndexTypeGlobalFulltextRelevance
                FulltextIndexDescription: {
                    Settings: {
                        %s
                    }
                }
            }
        )", fulltextSettings.c_str()));
        env.TestWaitNotification(runtime, txId);

        TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/texts/idx_posting"),{
            NLs::PathExist,
            NLs::IndexType(NKikimrSchemeOp::EIndexTypeGlobalFulltextRelevance),
            NLs::IndexState(NKikimrSchemeOp::EIndexStateReady),
            NLs::ChildrenCount(5),
        });
    }
}
