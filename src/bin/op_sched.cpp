#include <fstream>
#include <hos/sched/pass.hpp>
#include <hos/sched/sched.hpp>
#include <hos/sched/life.hpp>

using namespace hos;

int main(int argc, char const *argv[]) {
    // Initialize glog
    FLAGS_minloglevel = 0;
    google::LogToStderr();
    google::InitGoogleLogging(argv[0]);

    // Build compitation graph from ONNX model
    std::ifstream ifs(argv[1], std::ifstream::binary);
    onnx::ModelProto model;
    model.ParseFromIstream(&ifs);
    ifs.close();
    Graph graph(model, "nasnet_mobile");
    model.Clear();
    
    // Build hierarchical graph
    HierGraph hier(graph);
    RunPass<JoinSequencePass, MakeGroupPass>(hier);
    auto sched = HierarchicalSchedule(hier);
    // auto sched = ReversePostOrder(graph);
    LOG(INFO) << EstimatePeak(sched, graph.inputs);

    return 0;
}
