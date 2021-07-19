#include <hos/sched/life.hpp>
#include <hos/sched/mem.hpp>
#include <hos/sched/pass.hpp>
#include <hos/sched/sched.hpp>
#include <hos/util/viz.hpp>

namespace hos {

void PlotSchedule(const std::vector<hos::OpRef> &sched, const Graph &graph,
                  const std::string &dir, const std::string &name,
                  const std::string &format) {
    LOG_ASSERT(sched.size() == graph.ops.size());

    // Define DOT graph
    DotCreator<VertexRef> creator(name);

    // Add vertices
    for (auto &in : graph.inputs) creator.Node(in, in->value->name);
    for (auto [i, op] : EnumRange(sched))
        creator.Node(op, fmt::format("{}:{}", i, op->type));
    for (auto &out : graph.outputs) creator.Node(out, out->value->name);

    // Add edges
    for (auto &op : graph.ops)
        for (auto &pred : op->preds) creator.Edge(pred.lock(), op);
    for (auto &out : graph.outputs) creator.Edge(out->Def(), out);

    // Compile
    creator.Render(dir, format);
}

static std::unordered_map<OpRef, uint32_t> initPredCount(const Graph &graph) {
    std::unordered_map<OpRef, uint32_t> predCnt;
    for (auto &op : graph.ops) predCnt.insert({op, uint32_t(op->preds.size())});
    for (auto &input : graph.inputs)
        for (auto &succ : input->succs) predCnt[As<Op>(succ)]--;
    return predCnt;
}

static void extractZeroPredOp(std::unordered_map<OpRef, uint32_t> &predCnt,
                              std::vector<OpRef> &zeroPred) {
    for (auto &[op, cnt] : predCnt)
        if (cnt == 0) zeroPred.push_back(op);
    for (auto &op : zeroPred) predCnt.erase(op);
}

std::vector<OpRef> RandomSample(const Graph &graph, std::mt19937 &rng) {
    std::vector<OpRef> sched;
    auto predCnt = initPredCount(graph);
    std::vector<OpRef> zeroPred;
    extractZeroPredOp(predCnt, zeroPred);
    while (!zeroPred.empty()) {
        auto op = zeroPred[rng() % zeroPred.size()];
        sched.push_back(op);
        Remove(zeroPred, op);
        for (auto &succ : op->succs)
            if (Is<Op>(succ)) predCnt[As<Op>(succ)]--;
        extractZeroPredOp(predCnt, zeroPred);
    }
    return sched;
}

std::vector<OpRef> ReversePostOrder(const Graph &graph) {
    std::vector<OpRef> seq;
    for (auto v : RpoVertRange(graph))
        if (Is<Op>(v)) seq.push_back(Cast<Op>(v));
    return seq;
}

struct SchedResult {
    /// Scheduled sequence of ops
    std::vector<OpRef> seq;
    /// Memory states of scheduled sequence
    MemStateVec states;

    void Update(SchedResult &&other) {
        if (other.states.Peak() < this->states.Peak()) {
            this->seq.swap(other.seq);
            this->states.Swap(other.states);
        }
    }

    void Print() const {
        for (auto [op, state] : ZipRange(seq, states))
            LOG(INFO) << fmt::format("{:<18} {:>8}^ {:>8}_", op->type,
                                     state.first, state.second);
    }
};

struct PartialSchedResult : public SchedResult {
    /// Predecessor count of vertices
    /// This map serializes the graph structure to avoid traversal of the graph
    /// when computing zero-indegree sets.
    std::unordered_map<HierVertRef, uint32_t> predCnt;
    /// Use count of values
    std::unordered_map<ValueRef, uint32_t> useCnt;
};

struct GroupContext {
    /// Group that this context describes
    GroupRef group;
    /// Whether each value consumed in this group are killed by it
    std::vector<int> kill;

    GroupContext(const GroupRef &group,
                 const std::unordered_map<ValueRef, uint32_t> &useCnt)
        : group(group),
          kill(Transform<decltype(kill)>(group->consumed, [&](auto &pair) {
              return pair.second == useCnt.at(pair.first);
          })) {}

    bool operator==(const GroupContext &other) const {
        return this->group == other.group && this->kill == other.kill;
    }
};

}  // namespace hos

namespace std {

template <>
struct hash<hos::GroupContext> {
    size_t operator()(const hos::GroupContext &ctx) const {
        return hos::Hash(ctx.group, ctx.kill);
    }
};

}  // namespace std

namespace hos {

/// Extract zero-indegree vertices from predecessor count map and move it to the
/// ordered vector
inline static void extractZeroIn(
    std::unordered_map<HierVertRef, uint32_t> &predCnt,
    std::vector<HierVertRef> &zeroIn) {
    for (auto &[vert, cnt] : predCnt)
        if (cnt == 0) Insert(zeroIn, vert);
    for (auto &vert : zeroIn) predCnt.erase(vert);
}

/// A sequence has only one possible schedule. This function also computes
/// memory states of each op and update predecessor count and use count map.
static SchedResult scheduleSequence(
    const SequenceRef &seq, std::unordered_map<ValueRef, uint32_t> &useCnt) {
    // Iterate each op and compute memory states
    MemStateVec states;
    for (auto &op : seq->ops) {
        // Consume use counts used by this value
        std::vector<ValueRef> killed;
        for (auto &val : op->inputs) {
            if (val->kind == ValueKind::PARAM) continue;
            auto cnt = --useCnt[val];
            if (cnt == 0) killed.push_back(val);
        }

        // See if output value can overlap one of the input
        auto ovlIdx = OverlapInput(op);
        if (ovlIdx != OVERLAP_FAILED && !Contains(killed, op->inputs[ovlIdx]))
            ovlIdx = OVERLAP_FAILED;

        // Compute increase in size at transition to transient state
        uint64_t inc = 0;
        if (ovlIdx == OVERLAP_FAILED)
            inc = std::transform_reduce(
                op->outputs.begin(), op->outputs.end(), 0ull, std::plus(),
                [](auto &val) { return val->type.Size(); });

        // Compute decrease in size at transition to stable state
        auto ovlVal = ovlIdx == OVERLAP_FAILED ? nullptr : op->inputs[0];
        auto dec = 0ull;
        for (auto &val : op->inputs) {
            if (val->kind == ValueKind::PARAM) continue;  // skip parameters
            if (!Contains(killed, val)) continue;         // value is not killed
            if (val == ovlVal)
                continue;  // overlapped value should not be counted
            dec += val->type.Size();
        }

        // Update memory states
        states.Append(inc, dec);

        // Remove killed values from use count map
        for (auto &val : killed) useCnt.erase(val);

        // Update use count for values generated by this op
        for (auto &val : op->outputs)
            useCnt.insert({val, uint32_t(val->uses.size())});
    }

    return {seq->ops, std::move(states)};
}

/// Schedule group with reverse post-order
/// This scheduling almost always produces suboptimal result, but is fast. The
/// result can be used when it does not lift memory peak.
static SchedResult scheduleGroupRpo(
    const GroupRef &group, std::unordered_map<ValueRef, uint32_t> &useCnt) {
    // Initialize vertex range
    VertRange<HierVertex, RpoIter<HierVertex>> vertRange(
        Transform<std::vector<HierVertRef>>(
            group->exits, [](auto &exit) { return HierVertRef(exit); }));

    // Schedule each sequence in reverse post-order
    std::vector<OpRef> opSeq;
    MemStateVec states;
    for (auto vert : vertRange) {
        auto seq = As<Sequence>(vert);
        auto [vertSeq, vertStates] = scheduleSequence(seq, useCnt);
        opSeq.insert(opSeq.end(), vertSeq.begin(), vertSeq.end());
        states.Extend(vertStates);
    }

    return {std::move(opSeq), std::move(states)};
}

static PartialSchedResult &updateResult(
    const HierVertRef &vert, const std::vector<HierVertRef> &zeroIn,
    const PartialSchedResult &result, SchedResult &&vertResult,
    std::unordered_map<ValueRef, uint32_t> &&useCnt,
    std::unordered_map<std::vector<HierVertRef>, PartialSchedResult> &newMemo) {
    // Extend op sequence
    auto seq = result.seq;
    Extend(seq, vertResult.seq);

    // Extend memory states
    auto states = result.states;
    states.Extend(vertResult.states);

    // Update zero-indegree set
    auto predCnt = result.predCnt;
    for (auto &succ : vert->succs) predCnt[succ]--;
    auto newZeroIn = zeroIn;
    Remove(newZeroIn, vert);
    extractZeroIn(predCnt, newZeroIn);

    // Memoize this partial result
    PartialSchedResult newResult{
        {seq, states}, std::move(predCnt), std::move(useCnt)};
    if (Contains(newMemo, newZeroIn))
        newMemo[newZeroIn].Update(std::move(newResult));
    else
        newMemo.insert({newZeroIn, std::move(newResult)});

    return newMemo.at(newZeroIn);
}

/// Use DP algorithm to schedule the group
static SchedResult scheduleGroupDp(
    const GroupRef &group,
    const std::unordered_map<ValueRef, uint32_t> &useCnt) {
    // Initialize predecessor count of sequences inside group
    std::unordered_map<HierVertRef, uint32_t> predCnt;
    for (auto &seq : group->seqs)
        predCnt.insert({seq, uint32_t(seq->preds.size())});

    // Initialize memoization map
    std::vector<HierVertRef> zeroIn;
    extractZeroIn(predCnt, zeroIn);
    std::unordered_map<std::vector<HierVertRef>, PartialSchedResult> memo;
    memo.insert(
        {zeroIn, {SchedResult{{}, MemStateVec()}, std::move(predCnt), useCnt}});

    // Iterate |V| steps
    auto nVert = group->seqs.size();
    for (auto i = 0u; i < nVert; i++) {
        decltype(memo) newMemo;
        for (const auto &[zeroIn, result] : memo) {
            // Add another vertex to the schedule
            for (auto &vert : zeroIn) {
                auto useCnt = result.useCnt;
                auto vertResult = scheduleSequence(As<Sequence>(vert), useCnt);
                updateResult(vert, zeroIn, result, std::move(vertResult),
                             std::move(useCnt), newMemo);
            }
        }
        newMemo.swap(memo);
    }

    return memo[{}];
}

static void updateGroupUseCount(
    const GroupRef &group, std::unordered_map<ValueRef, uint32_t> &useCnt) {
    // Reduce use count consumed by this group
    std::vector<ValueRef> killed;
    for (const auto &[val, num] : group->consumed) {
        useCnt[val] -= num;
        if (useCnt[val] == 0) killed.push_back(val);
    }

    // Erase killed values from use count map
    for (auto &val : killed) useCnt.erase(val);

    // Add values produces by this group
    useCnt.insert(group->produced.begin(), group->produced.end());
}

class HierScheduler {
public:
    HierScheduler(const HierGraph &hier,
                  std::unordered_map<GroupContext, SchedResult> &groupMemo)
        : hier(hier), groupMemo(groupMemo) {}

    std::vector<OpRef> Schedule() {
        // Initialize predecessor count of vertices
        std::unordered_map<HierVertRef, uint32_t> predCnt;
        for (auto vert : RpoHierRange(hier)) {
            if (Is<HierInput>(vert) || Is<HierOutput>(vert)) continue;
            predCnt.insert({vert, uint32_t(vert->preds.size())});
        }
        auto nVert = predCnt.size();

        // Initialize use count of values
        std::unordered_map<ValueRef, uint32_t> useCnt;
        for (auto &input : hier.inputs) {
            for (auto &succ : input->succs) predCnt[succ]--;
            auto &val = input->value;
            useCnt.insert({val, uint32_t(val->uses.size())});
        }

        // Initialize memoization map
        std::vector<HierVertRef> zeroIn;
        extractZeroIn(predCnt, zeroIn);
        auto initSize = std::transform_reduce(
            hier.inputs.begin(), hier.inputs.end(), 0ull, std::plus(),
            [](auto &input) { return input->value->type.Size(); });
        std::unordered_map<std::vector<HierVertRef>, PartialSchedResult> memo;
        memo.insert({zeroIn,
                     {SchedResult{{}, MemStateVec(initSize)},
                      std::move(predCnt), std::move(useCnt)}});

        // Iterate |V| steps
        for (auto i = 0u; i < nVert; i++) {
            // Iterate each partial result and build partial schedule with one
            // more vertex
            decltype(memo) newMemo;
            for (const auto &[zeroIn, result] : memo) {
                // Add another vertex to the schedule
                for (auto &vert : zeroIn) {
                    auto useCnt = result.useCnt;
                    auto vertResult =
                        scheduleVertex(vert, useCnt, result.states);
                    updateResult(vert, zeroIn, result, std::move(vertResult),
                                 std::move(useCnt), newMemo);
                }
            }
            newMemo.swap(memo);
        }

        return memo[{}].seq;
    }

private:
    SchedResult scheduleVertex(const HierVertRef &vert,
                               std::unordered_map<ValueRef, uint32_t> &useCnt,
                               const MemStateVec &prevStates) {
        switch (vert->Kind()) {
            case HierKind::SEQUENCE:
                return scheduleSequence(Cast<Sequence>(vert), useCnt);

            case HierKind::GROUP: {
                // Check if there is memoized result
                auto group = Cast<Group>(vert);
                GroupContext ctx(group, useCnt);
                if (Contains(groupMemo, ctx)) {
                    // Use memoized result, also update use count
                    updateGroupUseCount(group, useCnt);
                    return groupMemo[ctx];
                }

                // Try schedule using reverse post-order
                auto rpoUseCnt = useCnt;
                auto rpoResult = scheduleGroupRpo(group, rpoUseCnt);

                // Use RPO schedule if peak is not lifted
                if (rpoResult.states.Peak() + prevStates.Latest() <=
                    prevStates.Peak()) {
                    useCnt.swap(rpoUseCnt);
                    return rpoResult;
                }

                // Schedule group using DP and memoize the result
                auto dpResult = scheduleGroupDp(group, useCnt);
                useCnt.swap(rpoUseCnt);  // final use count is same
                groupMemo.insert({ctx, dpResult});
                return dpResult;
            }

            default:
                LOG(FATAL) << "Unreachable";
        }
        LOG(FATAL) << "Unreachable.";
    }

    /// Hierarchical graph to be scheduled
    const HierGraph &hier;
    /// Scheduling result of each group, under different contexts
    std::unordered_map<GroupContext, SchedResult> &groupMemo;
};

using VertListFunc =
    std::function<std::vector<HierVertRef>(const HierVertRef &)>;
using GetSeqListFromGroupFunc =
    std::function<std::vector<SequenceRef>(const GroupRef &)>;

static std::unordered_map<SequenceRef, std::vector<HierVertRef>>
findEdgesToRestore(const std::vector<SequenceRef> &frontier,
                   const std::vector<HierVertRef> &neighbors,
                   const VertListFunc &getNeighborPrev,
                   const GetSeqListFromGroupFunc &getNeighborFrontier) {
    // Initialize map for all frontiers
    std::unordered_map<SequenceRef, std::vector<HierVertRef>> restoreMap;
    for (auto &seq : frontier) restoreMap.insert({seq, {}});

    // Iterate neighbors and restore edges
    for (auto &vert : neighbors) {
        if (Is<Group>(vert)) {
            // Check frontiers of this group
            auto neighGrp = Cast<Group>(vert);
            auto neighGrpFront = getNeighborFrontier(neighGrp);
            for (auto &neighGrpVert : neighGrpFront) {
                auto prevOuts = getNeighborPrev(neighGrpVert);
                for (auto &out : prevOuts) {
                    if (!Is<Sequence>(out)) continue;
                    auto outSeq = Cast<Sequence>(out);
                    if (Contains(restoreMap, outSeq))
                        Insert(restoreMap[outSeq], vert);
                }
            }
        } else {
            auto prevOuts = getNeighborPrev(vert);
            for (auto &out : prevOuts) {
                if (!Is<Sequence>(out)) continue;
                auto outSeq = Cast<Sequence>(out);
                if (Contains(restoreMap, outSeq))
                    restoreMap[outSeq].push_back(vert);
            }
        }
    }

    return restoreMap;
}

static void ungroup(const GroupRef &group) {
    // Reconnect predecessors with input frontiers
    auto inRestore = findEdgesToRestore(group->inFront, group->Preds(),
                                        std::mem_fn(&HierVertex::prevSuccs),
                                        std::mem_fn(&Group::outFront));
    for (auto &[front, restores] : inRestore) {
        for (auto &neigbor : restores) {
            AddUnique(front->preds, std::weak_ptr(neigbor));
            Remove(neigbor->succs, HierVertRef(group));
            AddUnique(neigbor->succs, HierVertRef(front));
        }
    }

    // Reconnect successors with output frontiers
    auto outRestore = findEdgesToRestore(
        group->outFront, group->succs,
        [](auto &vert) {
            return Transform<std::vector<HierVertRef>>(
                vert->prevPreds, [](auto &pred) { return pred.lock(); });
        },
        std::mem_fn(&Group::inFront));
    for (auto &[front, restores] : outRestore) {
        for (auto &neighbor : restores) {
            AddUnique(front->succs, neighbor);
            Remove(neighbor->preds, std::weak_ptr<HierVertex>(group));
            AddUnique(neighbor->preds, std::weak_ptr<HierVertex>(front));
        }
    }

    // Remove group
    for (auto &seq : group->seqs) seq->group = {};
}

static bool tryUngroupSucc(const SequenceRef &seq) {
    bool changed = false;
    while (true) {
        for (auto &succ : seq->succs) {
            if (!succ) continue;
            if (Is<Group>(succ)) {
                ungroup(Cast<Group>(succ));
                changed = true;
                continue;
            }
        }
        break;
    }
    return changed;
}

std::vector<OpRef> HierarchicalSchedule(const Graph &graph) {
    // Build hierarchical graph
    HierGraph hier(graph);
    RunPass<JoinSequencePass, MakeGroupPass>(hier);

    // Initialize memoization map for sharing results across iterations
    std::unordered_map<GroupContext, SchedResult> groupMemo;

    // Record schedule and peak
    std::vector<OpRef> lastSched;
    uint64_t lastPeak = UINT64_MAX;
    std::set<ValueRef> lastPeakValues;

    // Iteratively schedule hierarchical graph
    while (true) {
        auto sched = HierScheduler(hier, groupMemo).Schedule();
        auto stat = ComputeLifetime(sched, graph);

        // Find peak and alive values
        auto peak = EstimatePeak(sched, graph.inputs);
        std::set<ValueRef> peakValues;
        auto sizeRange = stat.SizeRange();
        for (auto it = sizeRange.begin(); it != sizeRange.end(); ++it) {
            auto size = (*it).second;
            if (size != peak) continue;
            for (auto &val : it.AliveValues()) peakValues.insert(val);
        }

        LOG_ASSERT(!peakValues.empty());
        LOG(INFO) << "Peak: " << peak / 1024;
        for (auto &val : peakValues) LOG(INFO) << val->name;
        LOG(INFO);

        // Locate sequences related to this peak
        std::unordered_set<SequenceRef> relSeqs;
        for (auto &val : peakValues)
            relSeqs.insert(hier.opToSeq[val->def.lock()]);

        // Ungroup
        bool changed = false;
        for (auto &seq : relSeqs) {
            // Ungroups those which contains peak sequences
            auto group = seq->group.lock();
            if (group != nullptr) {
                ungroup(group);
                changed = true;
            }

            // Ungroup successor groups of peak sequences
            changed |= tryUngroupSucc(seq);
        }

        // Break if peak is caused by the same set of values as last time and
        // nothing more can be done to the graph
        if (peak == lastPeak && peakValues == lastPeakValues && !changed) break;

        // Update record for next iteration
        lastSched = sched;
        lastPeak = peak;
        lastPeakValues = peakValues;
    }

    return lastSched;
}

}  // namespace hos