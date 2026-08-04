#include <library/cpp/testing/unittest/registar.h>
#include <library/cpp/threading/future/future.h>
#include <ydb/core/testlib/basics/runtime.h>
#include <ydb/core/testlib/basics/appdata.h>
#include <util/random/fast.h>
#include <util/stream/null.h>
#include <exception>
#include <functional>
#include <numeric>
#include <type_traits>
#include <utility>
#include "hive_impl.h"
#include "balancer.h"


using namespace NKikimr;
using namespace NHive;

namespace {

struct TEvHiveBalancerTest {
    enum EEv {
        // keep away from THive's TEvPrivate event ids - the mock hive receives both
        EvRunCallback = EventSpaceBegin(NActors::TEvents::ES_PRIVATE) + 4096,
        EvRunCallbackResult,
    };

    struct TEvRunCallback : NActors::TEventLocal<TEvRunCallback, EvRunCallback> {
        std::function<void()> Callback;

        explicit TEvRunCallback(std::function<void()> callback)
            : Callback(std::move(callback))
        {}
    };

    struct TEvRunCallbackResult : NActors::TEventLocal<TEvRunCallbackResult, EvRunCallbackResult> {};
};

// THive with a mocked tablet executor part: state changes requested by the standard
// handlers are applied to in-memory state immediately instead of running real transactions
class TTestHive : public THive {
public:
    TActorId TestEdge;

    TTestHive(TTabletStorageInfo* info, const TActorId& tablet)
        : THive(info, tablet)
    {
        Become(&TTestHive::StateTest);
    }

    void UpdateConfig(const NKikimrConfig::THiveConfig& config) {
        ClusterConfig = config;
        BuildCurrentConfig();
    }

    void StartBalancer(TBalancerSettings&& settings) {
        StartHiveBalancer(std::move(settings));
    }

    const TBalancerStats& GetBalancerStats(EBalancerType type) const {
        return BalancerStats[static_cast<std::size_t>(type)];
    }

    ui64 GetTotalBalancerMovements() const {
        ui64 movements = 0;
        for (const TBalancerStats& stats : BalancerStats) {
            movements += stats.TotalMovements;
        }
        return movements;
    }

    auto GetTestStats() const {
        return GetStats();
    }

    TNodeInfo& AddTestNode(TNodeId nodeId, const TSubDomainKey& domain, TTabletTypes::EType tabletType, ui64 maxCpu) {
        TNodeInfo& node = Nodes.emplace(std::piecewise_construct,
                                        std::forward_as_tuple(nodeId),
                                        std::forward_as_tuple(nodeId, *this)).first->second;
        node.Local = TActorId(nodeId, "testlocal");
        node.ServicedDomains = {domain};
        NActorsInterconnect::TNodeLocation location;
        location.SetDataCenter("dc-1");
        node.Location = TNodeLocation(location);
        node.LocationAcquired = true;
        node.TabletAvailability.emplace(tabletType, NKikimrLocal::TTabletAvailability());
        std::get<NMetrics::EResource::CPU>(node.ResourceMaximumValues) = maxCpu;
        node.ChangeVolatileState(TNodeInfo::EVolatileState::Connected);
        return node;
    }

    TLeaderTabletInfo& AddTestTablet(TTabletId tabletId, const TSubDomainKey& domain, TTabletTypes::EType tabletType, TNodeId nodeId, TObjectId objectId) {
        TLeaderTabletInfo& tablet = Tablets.emplace(std::piecewise_construct,
                                                    std::forward_as_tuple(tabletId),
                                                    std::forward_as_tuple(tabletId, *this)).first->second;
        tablet.SetType(tabletType);
        tablet.State = ETabletState::ReadyToWork;
        tablet.ObjectId = {0, objectId};
        tablet.AssignDomains(domain, {});
        UpdateCounterTabletsTotal(+1);
        tablet.InitTabletMetrics();
        tablet.BecomeRunning(nodeId);
        return tablet;
    }

protected:
    // the in-memory part of TTxRestartTablet + TTxUpdateTabletStatus: move the tablet
    // right away and report success back to the balancer
    void ExecuteRestartTablet(TFullTabletId tabletId, TNodeId preferredNodeId) override {
        TTabletInfo* tablet = FindTablet(tabletId);
        Y_ABORT_UNLESS(tablet != nullptr);
        tablet->BecomeStopped();
        tablet->BecomeRunning(preferredNodeId);
        TVector<TActorId> actorsToNotify;
        actorsToNotify.swap(tablet->ActorsToNotifyOnRestart);
        for (const TActorId& actor : actorsToNotify) {
            Send(actor, new TEvPrivate::TEvRestartComplete(tabletId, "OK"));
        }
    }

    // the in-memory part of TTxUpdateTabletMetrics
    void ExecuteUpdateTabletMetrics(TEvHive::TEvTabletMetrics::TPtr event) override {
        TInstant now = TInstant::Now();
        auto reply = MakeHolder<TEvLocal::TEvTabletMetricsAck>();
        auto& record = event->Get()->Record;
        TNodeId nodeId = event->Sender.NodeId();
        for (const auto& metrics : record.GetTabletMetrics()) {
            TTabletId tabletId = metrics.GetTabletID();
            TFollowerId followerId = metrics.GetFollowerID();
            TTabletInfo* tablet = FindTablet(tabletId, followerId);
            if (tablet != nullptr && metrics.HasResourceUsage()) {
                tablet->UpdateResourceUsage(metrics.GetResourceUsage());
                tablet->Statistics.SetLastAliveTimestamp(now.MilliSeconds());
                tablet->ActualizeTabletStatistics(now);
            }
            reply->Record.AddTabletId(tabletId);
            reply->Record.AddFollowerId(followerId);
        }
        TNodeInfo* node = FindNode(nodeId);
        if (node != nullptr) {
            node->UpdateResourceMaximum(record.GetResourceMaximum());
            node->UpdateResourceTotalUsage(record);
            node->Statistics.SetLastAliveTimestamp(now.MilliSeconds());
            node->ActualizeNodeStatistics(now);
        }
        Send(event->Sender, reply.Release());
        UpdateTabletMetricsInProgress--;
    }

    void Handle(TEvHiveBalancerTest::TEvRunCallback::TPtr& ev) {
        ev->Get()->Callback();
        Send(ev->Sender, new TEvHiveBalancerTest::TEvRunCallbackResult());
    }

    void Handle(TEvPrivate::TEvBalancerOut::TPtr&) {
        if (TestEdge) {
            Send(TestEdge, new TEvPrivate::TEvBalancerOut());
        }
    }

    STATEFN(StateTest) {
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvHiveBalancerTest::TEvRunCallback, Handle);
            hFunc(TEvPrivate::TEvBalancerOut, Handle);
            // standard hive handlers driving the balancing cycle
            hFunc(TEvHive::TEvTabletMetrics, THive::Handle);
            hFunc(TEvPrivate::TEvProcessTabletBalancer, THive::Handle);
            hFunc(TEvPrivate::TEvLogTabletMoves, THive::Handle);
            default:
                // other internal hive events are irrelevant here
                break;
        }
    }
};

// Models an environment for THive's balancer: a hive with a set of alive nodes and running
// tablets, where the real THiveBalancer actor is triggered and paced by the standard hive
// machinery (TEvProcessTabletBalancer scheduling), tablet moves are mocked in memory, and
// tablet/node metrics are delivered through the standard TEvTabletMetrics handler
class THiveBalancerTestEnv {
public:
    static constexpr TTabletTypes::EType TABLET_TYPE = TTabletTypes::Dummy;

    explicit THiveBalancerTestEnv(ui32 nodeCount)
        : HiveStorage(new TTabletStorageInfo())
        , Runtime(nodeCount)
        , Domain(1, 1)
    {
        HiveStorage->TabletType = TTabletTypes::Hive;
        Runtime.Initialize(TAppPrepare().Unwrap());
        Runtime.SetLogPriority(NKikimrServices::HIVE, NLog::PRI_TRACE);
        Edge = Runtime.AllocateEdgeActor();
        Hive = new TTestHive(HiveStorage.Get(), TActorId());
        Hive->TestEdge = Edge;
        HiveActor = Runtime.Register(Hive);
        // let the events scheduled by hive (balancer runs etc.) fire as model time advances
        Runtime.EnableScheduleForActor(HiveActor, true);
        // move away from the time origin so that balancer cooldowns comparing against
        // default-initialized timestamps do not get in the way
        Runtime.AdvanceCurrentTime(TDuration::Hours(1));
    }

    void UpdateConfig(const NKikimrConfig::THiveConfig& config) {
        RunInHive([this, &config] {
            Hive->UpdateConfig(config);
        });
    }

    NKikimrConfig::THiveConfig GetHiveConfig() {
        return RunInHive([&] {
            return Hive->CurrentConfig;
        });
    }

    TNodeId AddNode(ui32 nodeIndex, ui64 maxCpu) {
        TNodeId nodeId = Runtime.GetNodeId(nodeIndex);
        RunInHive([this, nodeId, maxCpu] {
            Hive->AddTestNode(nodeId, Domain, TABLET_TYPE, maxCpu);
        });
        NodeIds.push_back(nodeId);
        NodeMaxCpu[nodeId] = maxCpu;
        return nodeId;
    }

    void AddTablet(TTabletId tabletId, TNodeId nodeId, ui64 cpu) {
        RunInHive([this, tabletId, nodeId] {
            Hive->AddTestTablet(tabletId, Domain, TABLET_TYPE, nodeId, /* objectId = */ tabletId);
        });
        // the tablet reports this CPU usage with every metrics batch
        TabletCpu[tabletId] = cpu;
    }

    void UpdateTabletCpu(TTabletId tabletId, ui64 cpu) {
        TabletCpu[tabletId] = cpu;
    }

    void UpdateNodeMaximumCpu(TNodeId nodeId, ui64 maxCpu) {
        NodeMaxCpu[nodeId] = maxCpu;
    }

    // makes every node report its overall CPU usage as the sum of the tablets running
    // on it multiplied by the given factor
    void SetNodeCpuTotalMultiplier(double multiplier) {
        NodeCpuTotalMultiplier = multiplier;
    }

    // emulates TLocal on every node sending its periodic TEvTabletMetrics batch
    // to the standard hive handler
    void SendNodeMetrics() {
        struct TNodeBatch {
            TNodeId NodeId;
            std::vector<TFullTabletId> Tablets;
        };
        std::vector<TNodeBatch> batches;
        RunInHive([&] {
            for (TNodeId nodeId : NodeIds) {
                TNodeBatch& batch = batches.emplace_back();
                batch.NodeId = nodeId;
                TNodeInfo* node = Hive->FindNode(nodeId);
                auto itRunning = node->Tablets.find(TTabletInfo::EVolatileState::TABLET_VOLATILE_STATE_RUNNING);
                if (itRunning != node->Tablets.end()) {
                    for (const TTabletInfo* tablet : itRunning->second) {
                        batch.Tablets.push_back(tablet->GetFullTabletId());
                    }
                }
            }
        });
        for (const TNodeBatch& batch : batches) {
            auto event = MakeHolder<TEvHive::TEvTabletMetrics>();
            NKikimrHive::TEvTabletMetrics& record = event->Record;
            ui64 tabletsCpu = 0;
            for (TFullTabletId tabletId : batch.Tablets) {
                ui64 cpu = TabletCpu[tabletId.first];
                tabletsCpu += cpu;
                auto& metrics = *record.AddTabletMetrics();
                metrics.SetTabletID(tabletId.first);
                metrics.SetFollowerID(tabletId.second);
                metrics.MutableResourceUsage()->SetCPU(cpu);
            }
            ui64 maxCpu = NodeMaxCpu[batch.NodeId];
            ui64 totalCpu = static_cast<ui64>(tabletsCpu * NodeCpuTotalMultiplier);
            record.MutableTotalResourceUsage()->SetCPU(totalCpu);
            record.SetTotalNodeUsage(static_cast<double>(totalCpu) / maxCpu);
            record.SetTotalNodeCpuUsage(static_cast<double>(totalCpu) / maxCpu);
            record.MutableResourceMaximum()->SetCPU(maxCpu);
            Runtime.Send(new IEventHandle(HiveActor, GetLocalId(batch.NodeId), event.Release()), 0, true);
        }
        RunInHive([] {}); // make sure all the batches are processed
    }

    // advances model time and delivers the events that became due, letting hive run
    // its scheduled activities (such as balancer runs)
    void AdvanceTime(TDuration duration) {
        Runtime.AdvanceCurrentTime(duration);
        Runtime.SimulateSleep(TDuration::MicroSeconds(1));
    }

    // starts the balancer manually, waits until it finishes and returns the number of movements it made
    ui64 RunBalancer(TBalancerSettings settings) {
        EBalancerType type = settings.Type;
        RunInHive([this, settings = std::move(settings)]() mutable {
            Hive->StartBalancer(std::move(settings));
        });
        TAutoPtr<IEventHandle> handle;
        Runtime.GrabEdgeEventRethrow<TEvPrivate::TEvBalancerOut>(handle);
        ui64 movements = 0;
        RunInHive([&] {
            movements = Hive->GetBalancerStats(type).LastRunMovements;
        });
        return movements;
    }

    ui64 GetTotalBalancerMovements() {
        ui64 movements = 0;
        RunInHive([&] {
            movements = Hive->GetTotalBalancerMovements();
        });
        return movements;
    }

    // node usage scatter as hive itself sees it, by the given resource
    double GetCpuScatter() {
        double scatter = 0;
        RunInHive([&] {
            auto stats = Hive->GetTestStats();
            scatter = std::get<NMetrics::EResource::CPU>(stats.ScatterByResource);
        });
        return scatter;
    }

    std::vector<double> GetNodeUsages(EResourceToBalance resource) {
        std::vector<double> usages;
        RunInHive([&] {
            for (TNodeId nodeId : NodeIds) {
                usages.push_back(Hive->FindNode(nodeId)->GetNodeUsage(resource));
            }
        });
        return usages;
    }

    std::vector<ui32> GetNodeTabletCounts() {
        std::vector<ui32> counts;
        RunInHive([&] {
            for (TNodeId nodeId : NodeIds) {
                counts.push_back(Hive->FindNode(nodeId)->GetTabletsRunning());
            }
        });
        return counts;
    }

    const std::vector<TNodeId>& GetNodeIds() const {
        return NodeIds;
    }

    static double GetMaxMinDiff(const std::vector<double>& values) {
        auto [min, max] = std::minmax_element(values.begin(), values.end());
        return *max - *min;
    }

    static double GetStdDev(const std::vector<double>& values) {
        if (values.empty()) {
            return 0;
        }
        double mean = std::accumulate(values.begin(), values.end(), 0.0) / values.size();
        double varianceSum = 0;
        for (double value : values) {
            varianceSum += (value - mean) * (value - mean);
        }
        return sqrt(varianceSum / values.size());
    }

    template <typename TCallback>
    auto RunInHive(TCallback&& callback) -> std::decay_t<std::invoke_result_t<std::decay_t<TCallback>&>> {
        using TStoredCallback = std::decay_t<TCallback>;
        using TResult = std::decay_t<std::invoke_result_t<TStoredCallback&>>;

        auto promise = NThreading::NewPromise<TResult>();
        auto future = promise.GetFuture();
        auto hiveCallback = [
            callback = TStoredCallback(std::forward<TCallback>(callback)),
            promise = std::move(promise)
        ]() mutable {
            try {
                if constexpr (std::is_void_v<TResult>) {
                    std::invoke(callback);
                    promise.SetValue();
                } else {
                    promise.SetValue(std::invoke(callback));
                }
            } catch (...) {
                promise.SetException(std::current_exception());
            }
        };

        Runtime.Send(new IEventHandle(HiveActor, Edge, new TEvHiveBalancerTest::TEvRunCallback(std::move(hiveCallback))), 0, true);
        TAutoPtr<IEventHandle> handle;
        Runtime.GrabEdgeEventRethrow<TEvHiveBalancerTest::TEvRunCallbackResult>(handle);

        if constexpr (std::is_void_v<TResult>) {
            future.GetValue();
        } else {
            return future.ExtractValue();
        }
    }

    TTestHive* GetHive() {
        return Hive;
    }

private:
    static TActorId GetLocalId(TNodeId nodeId) {
        return TActorId(nodeId, "testlocal");
    }

    TIntrusivePtr<TTabletStorageInfo> HiveStorage;
    TTestBasicRuntime Runtime;
    TTestHive* Hive;
    TActorId HiveActor;
    TActorId Edge;
    TSubDomainKey Domain;
    std::vector<TNodeId> NodeIds;
    std::unordered_map<TNodeId, ui64> NodeMaxCpu;
    std::unordered_map<TTabletId, ui64> TabletCpu;
    double NodeCpuTotalMultiplier = 1.0;
};

} // namespace

Y_UNIT_TEST_SUITE(THiveBalancerTest) {
    Y_UNIT_TEST(BalanceByCpuWithHeaviestStrategyStabilizes) {
        static constexpr ui32 NUM_NODES = 12;
        static constexpr ui32 OVERLOADED_NODES = 2;
        static constexpr ui64 NUM_TABLETS = 800;
        static constexpr ui64 NUM_TABLETS_TO_OVERLOADED_NODES = NUM_TABLETS * 5 / 8;
        // CPU is measured in microseconds per second, 1'000'000 = 1 CPU (full core)
        static constexpr ui64 NODE_MAX_CPU = 20'000'000; // 20 CPU per node
        static constexpr ui64 TABLET_MAX_CPU = 300'000; // each tablet uses 0.0 .. 0.3 CPU
        static constexpr double NODE_TOTAL_CPU_MULTIPLIER = 1.4; // node usage exceeds the sum of its tablets
        static constexpr ui64 FIRST_TABLET_ID = 1'000;
        // TLocal sends batches of updated tablet metrics to hive with this interval
        // (TABLET_METRICS_BATCH_INTERVAL in local.cpp - a constant, not a config setting)
        static constexpr TDuration METRICS_PERIOD = TDuration::MilliSeconds(5000);

        THiveBalancerTestEnv env(NUM_NODES);
        env.SetNodeCpuTotalMultiplier(NODE_TOTAL_CPU_MULTIPLIER);

        NKikimrConfig::THiveConfig hiveConfig = env.GetHiveConfig();
        hiveConfig.SetTabletKickCooldownPeriod(60);
        hiveConfig.SetMaxMovementsOnAutoBalancer(3);
        env.UpdateConfig(hiveConfig);

        const TDuration balancerPeriod = TDuration::Seconds(hiveConfig.GetMinPeriodBetweenBalance());
        const double minScatterToBalance = hiveConfig.GetMinCPUScatterToBalance();

        for (ui32 nodeIndex = 0; nodeIndex < NUM_NODES; ++nodeIndex) {
            env.AddNode(nodeIndex, NODE_MAX_CPU);
        }
        const std::vector<TNodeId>& nodes = env.GetNodeIds();

        // tablets use ~equal CPU on average, but the first 4 nodes are loaded much heavier,
        // so that the CPU scatter is above MinCPUScatterToBalance and hive starts balancing
        TReallyFastRng32 rng(42);
        for (ui64 i = 0; i < NUM_TABLETS; ++i) {
            TNodeId nodeId = (i < NUM_TABLETS_TO_OVERLOADED_NODES) ? nodes[i % OVERLOADED_NODES] : nodes[OVERLOADED_NODES + i % (NUM_NODES - OVERLOADED_NODES)];
            ui64 cpu = rng() % (TABLET_MAX_CPU + 1);
            env.AddTablet(FIRST_TABLET_ID + i, nodeId, cpu);
        }

        // deliver the initial metrics through the standard handler
        env.SendNodeMetrics();

        std::vector<double> initialUsages = env.GetNodeUsages(EResourceToBalance::CPU);
        double initialScatter = env.GetCpuScatter();
        double initialStdDev = THiveBalancerTestEnv::GetStdDev(initialUsages);
        double initialMaxMinDiff = THiveBalancerTestEnv::GetMaxMinDiff(initialUsages);
        Cerr << "Initial usages: " << initialUsages
              << " scatter: " << initialScatter
              << " stddev: " << initialStdDev << Endl;
        UNIT_ASSERT_GT_C(initialScatter, minScatterToBalance, "the setup must be skewed enough to trigger balancing");

        // run the model: metrics arrive every METRICS_PERIOD, hive runs the balancer
        // on its own schedule in between
        static constexpr ui32 MAX_WAVES = 1000;
        static constexpr ui32 STABLE_WAVES_TO_STOP = 3;
        ui32 waves = 0;
        ui32 stableWaves = 0;
        ui64 lastMovements = 0;
        while (stableWaves < STABLE_WAVES_TO_STOP && waves < MAX_WAVES) {
            for (TDuration time; time < METRICS_PERIOD; time += balancerPeriod) {
                env.AdvanceTime(balancerPeriod);
            }
            env.RunInHive([&env]() {
                for (TNodeId nodeId : env.GetNodeIds()) {
                    Cerr << "Node " << nodeId << " runs "
                          << env.GetHive()->FindNode(nodeId)->GetTabletsRunning() << " tablets" << Endl;
                }
            });
            env.SendNodeMetrics();
            ++waves;
            ui64 movements = env.GetTotalBalancerMovements();
            stableWaves = movements == lastMovements ? stableWaves + 1 : 0;
            Cerr << "Wave " << waves << ": " << movements - lastMovements << " movements, "
                  << movements << " total, scatter " << env.GetCpuScatter() << Endl;
            lastMovements = movements;
        }

        std::vector<double> finalUsages = env.GetNodeUsages(EResourceToBalance::CPU);
        double finalScatter = env.GetCpuScatter();
        double finalStdDev = THiveBalancerTestEnv::GetStdDev(finalUsages);
        double finalMaxMinDiff = THiveBalancerTestEnv::GetMaxMinDiff(finalUsages);
        Cerr << "Final usages: " << finalUsages
              << " scatter: " << finalScatter
              << " stddev: " << finalStdDev
              << " after " << waves << " waves and " << lastMovements << " movements" << Endl;

        // the balancer must stabilize - stop generating movements - and not oscillate forever
        UNIT_ASSERT_C(stableWaves >= STABLE_WAVES_TO_STOP, "balancer did not stabilize in " << waves << " waves");
        UNIT_ASSERT_C(lastMovements > 0, "balancer did not make any movements");

        // the balancer must have reached its configured goal...
        UNIT_ASSERT_LT_C(finalScatter, minScatterToBalance, "CPU scatter was not reduced below the configured threshold");
        // ...and the nodes must end up more equally distributed by CPU than they started
        UNIT_ASSERT_LT(finalStdDev, initialStdDev);
        UNIT_ASSERT_LT(finalMaxMinDiff, initialMaxMinDiff);

        // no tablets are lost or duplicated in the process
        std::vector<ui32> tabletCounts = env.GetNodeTabletCounts();
        ui64 tabletsTotal = std::accumulate(tabletCounts.begin(), tabletCounts.end(), (ui64)0);
        UNIT_ASSERT_VALUES_EQUAL(tabletsTotal, NUM_TABLETS);
    }
}
