/* Copyright 2015 Google Inc. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <algorithm>
#include <set>
#include <unordered_map>
#include <vector>

#include "tensorflow/core/common_runtime/constant_folding.h"

#include "tensorflow/core/common_runtime/device_factory.h"
#include "tensorflow/core/common_runtime/executor.h"
#include "tensorflow/core/common_runtime/rendezvous_mgr.h"
#include "tensorflow/core/graph/algorithm.h"
#include "tensorflow/core/graph/node_builder.h"
#include "tensorflow/core/graph/subgraph.h"
#include "tensorflow/core/lib/core/threadpool.h"
#include "tensorflow/core/public/session_options.h"

namespace tensorflow {

namespace {

bool IsConstantFoldable(const Node* n,
                        std::function<bool(const Node*)> consider) {
  if (n->op_def().is_stateful()) {
    return false;
  }
  if (consider && !consider(n)) {
    return false;
  }
  if (n->IsControlFlow() || n->IsSend() || n->IsRecv()) {
    return false;
  }
  return true;
}

// Returns the constant foldable nodes in `nodes_result` in data flow order.
void FindConstantFoldableNodes(const Graph* graph, ConstantFoldingOptions opts,
                               std::vector<Node*>* nodes_result) {
  std::set<const Node*> node_set;
  std::vector<Node*>& nodes = *nodes_result;
  bool internal_node_inserted = false;
  // Walk the nodes in data flow order
  ReverseDFS(*graph, nullptr,
             [&nodes, &node_set, &internal_node_inserted, opts](Node* n) {
               if (n->IsConstant()) {
                 // Constants are definitely constant foldable
                 node_set.insert(n);
                 nodes.push_back(n);
               } else if (IsConstantFoldable(n, opts.consider)) {
                 // Check whether the set of this node's in_nodes is completely
                 // included in
                 // the set of constant foldable nodes. If true, then this nodes
                 // is also
                 // constant foldable.
                 bool all_parents_constant = n->num_inputs() > 0;
                 for (const Node* parent : n->in_nodes()) {
                   if (node_set.count(parent) == 0) {
                     all_parents_constant = false;
                     break;
                   }
                 }
                 if (all_parents_constant) {
                   node_set.insert(n);
                   nodes.push_back(n);
                   internal_node_inserted = true;
                 }
               }
             });
  // If we have inserted just leaf level nodes, then there is nothing to fold.
  if (!internal_node_inserted) {
    nodes.clear();
  }
}

// Given the constant foldable nodes in 'nodes', returns a new graph 'g'. 'g'
// will contain copies of the nodes in 'nodes'. In addition, if there is an edge
// going from a node 'n' in 'nodes' to another node in 'orig_graph' but not in
// 'nodes', then 'nodes_to_fetch' will contain the mapping from the
// corresponding copy of 'n' in 'g' to 'n'.
Graph* GetConstantGraph(const Graph* orig_graph,
                        const std::vector<Node*>& nodes,
                        std::unordered_map<Node*, Node*>* nodes_to_fetch) {
  Graph* constant_graph = new Graph(orig_graph->op_registry());
  std::unordered_map<Node*, Node*> node_map;
  std::set<Node*> already_added;
  already_added.insert(constant_graph->source_node());
  already_added.insert(constant_graph->sink_node());
  node_map[orig_graph->source_node()] = constant_graph->source_node();
  node_map[orig_graph->sink_node()] = constant_graph->sink_node();
  for (Node* n : nodes) {
    Node* added = constant_graph->CopyNode(n);
    node_map[n] = added;
    already_added.insert(added);
    for (const Edge* in_edge : n->in_edges()) {
      Node* in = in_edge->src();
      CHECK_GT(node_map.count(in), 0);
      CHECK_GT(already_added.count(node_map[in]), 0);
      constant_graph->AddEdge(node_map[in], in_edge->src_output(), added,
                              in_edge->dst_input());
    }
  }

  for (auto const& added_nodes : node_map) {
    bool should_fetch = false;
    for (const Edge* out_edge : added_nodes.first->out_edges()) {
      if (node_map.count(out_edge->dst()) == 0) {
        should_fetch = true;
        break;
      }
    }
    if (should_fetch) {
      nodes_to_fetch->insert({added_nodes.second, added_nodes.first});
    }
  }

  return constant_graph;
}

void ReplaceNodeWithConstant(Graph* graph, Node* n, const Tensor& constant) {
  std::vector<std::tuple<int, Node*, int>> old_edges;
  for (const Edge* out_edge : n->out_edges()) {
    old_edges.push_back(std::make_tuple(out_edge->src_output(), out_edge->dst(),
                                        out_edge->dst_input()));
  }
  string node_name = n->name();
  graph->RemoveNode(n);
  Node* constant_node;
  TF_CHECK_OK(NodeBuilder(graph->NewName(node_name), "Const")
                  .Attr("dtype", constant.dtype())
                  .Attr("value", constant)
                  .Finalize(graph, &constant_node));
  for (auto edge : old_edges) {
    graph->AddEdge(constant_node, std::get<0>(edge), std::get<1>(edge),
                   std::get<2>(edge));
  }
}

Device* GetCPUDevice() {
  static Device* device = nullptr;
  if (!device) {
    std::vector<Device*> devices;
    DeviceFactory::GetFactory(DEVICE_CPU)
        ->CreateDevices(SessionOptions{}, "", &devices);
    if (devices.size() > 0) {
      device = devices[0];
    }
  }
  return device;
}

thread::ThreadPool* GetThreadPool() {
  static thread::ThreadPool* thread_pool =
      new thread::ThreadPool(Env::Default(), "Compute", 1);
  return thread_pool;
}

// A simple rendezvous class.
// Assumes a single sender and a single receiver, no duplicate sends, and no
// sends of dead tensors.
class SimpleRendezvous : public Rendezvous {
 public:
  explicit SimpleRendezvous() {}

  Status Send(const string& key, const Args& send_args, const Tensor& val,
              const bool is_dead) override {
    if (is_dead) {
      return errors::Internal("Send of a dead tensor");
    }
    ParsedKey parsed;
    TF_RETURN_IF_ERROR(ParseKey(key, &parsed));

    mutex_lock l(mu_);
    if (table_.count(parsed.edge_name) > 0) {
      return errors::Internal("Send of an already sent tensor");
    }
    table_[parsed.edge_name] = val;
    return Status::OK();
  }

  void RecvAsync(const string& key, const Args& recv_args,
                 DoneCallback done) override {
    Tensor tensor;
    Status status = Status::OK();
    {
      mutex_lock l(mu_);
      if (table_.count(key) <= 0) {
        status = errors::Internal("Did not find key ", key);
      } else {
        tensor = table_[key];
      }
    }
    done(status, Args{}, recv_args, tensor, false);
  }

  void StartAbort(const Status& status) override {}

 private:
  typedef std::unordered_map<string, Tensor> Table;

  mutex mu_;
  Table table_ GUARDED_BY(mu_);
};

}  // namespace

bool DoConstantFolding(const ConstantFoldingOptions& opts, Graph* graph) {
  Device* device = GetCPUDevice();
  thread::ThreadPool* thread_pool = GetThreadPool();
  if (!device || !thread_pool) {
    VLOG(1) << "Cannot find a device and/or a thread pool to do constant "
               "folding on";
    return false;
  }

  std::vector<Node*> constant_foldable_nodes;
  FindConstantFoldableNodes(graph, opts, &constant_foldable_nodes);
  if (constant_foldable_nodes.empty()) {
    VLOG(1) << "No constant foldable nodes found";
    return false;
  }

  std::unordered_map<Node*, Node*> nodes_to_fetch;
  Graph* constant_graph =
      GetConstantGraph(graph, constant_foldable_nodes, &nodes_to_fetch);

  if (nodes_to_fetch.empty()) {
    VLOG(1) << "No constant nodes found that feed into the original graph.";
    delete constant_graph;
    return false;
  }
  VLOG(1) << "Constant foldable " << constant_graph->num_node_ids() << " : "
          << graph->num_node_ids();

  // Create a local executor and evaluate the constant foldable nodes.
  subgraph::NameIndex name_index;
  for (Node* n : constant_graph->nodes()) {
    name_index[n->name()] = n;
  }

  std::vector<Node*> fetch_nodes;
  std::vector<string> nodes_to_fetch_names;
  std::vector<Node*> nodes_to_replace;
  for (auto n : nodes_to_fetch) {
    nodes_to_fetch_names.push_back(n.first->name());
    nodes_to_replace.push_back(n.second);
  }
  // For nodes that need to be fetched back from the constant_graph, attach Send
  // nodes.
  if (!subgraph::FetchOutputs(constant_graph, device->attributes(),
                              nodes_to_fetch_names, &name_index, &fetch_nodes)
           .ok()) {
    return false;
  }

  CHECK_EQ(fetch_nodes.size(), nodes_to_fetch.size());

  // Create the local executor and the Rendezvous for fetching back the
  // constants.
  auto runner = [thread_pool](Executor::Args::Closure c) {
    thread_pool->Schedule(c);
  };
  LocalExecutorParams params;
  params.device = device;
  params.create_kernel = [device, constant_graph](const NodeDef& ndef,
                                                  OpKernel** kernel) {
    return CreateNonCachedKernel(device, nullptr, ndef,
                                 constant_graph->versions().producer(), kernel);
  };
  params.delete_kernel = [](OpKernel* kernel) { delete kernel; };
  Executor* executor;
  if (!NewLocalExecutor(params, constant_graph, &executor).ok()) {
    return false;
  }

  std::unique_ptr<Executor> executor_unref(executor);

  SimpleRendezvous* rendez = new SimpleRendezvous;
  core::ScopedUnref rendez_unref(rendez);

  Executor::Args args;
  args.runner = runner;
  args.rendezvous = rendez;

  // Run the constant_graph.
  Notification executor_done;
  Status executor_done_status;
  ExecutorBarrier* barrier = new ExecutorBarrier(
      1, rendez, [&executor_done, &executor_done_status](const Status& ret) {
        executor_done_status = ret;
        executor_done.Notify();
      });

  executor->RunAsync(args, barrier->Get());

  if (!executor_done_status.ok()) {
    return false;
  }
  executor_done.WaitForNotification();

  // Keep track of the nodes that will be orphaned once the internal nodes have
  // been constant folded and replaced, so we can delete them later.
  std::set<Node*> replaced_nodes_set(nodes_to_replace.begin(),
                                     nodes_to_replace.end());
  std::vector<Node*> to_delete;
  for (Node* n : constant_foldable_nodes) {
    if (replaced_nodes_set.count(n) == 0) {
      to_delete.push_back(n);
    }
  }
  // Fetch the constant nodes and replace the corresponding nodes in the
  // original graph with those constants.
  for (size_t c = 0; c < fetch_nodes.size(); ++c) {
    Tensor output;
    bool is_dead;
    string tensor_name;
    if (!GetNodeAttr(fetch_nodes[c]->def(), "tensor_name", &tensor_name).ok()) {
      // We successfully replaced some nodes previously, but had a problem with
      // this node. Don't bother processing the rest of the nodes.
      return c > 0;
    }
    Status s = rendez->Recv(tensor_name, Rendezvous::Args(), &output, &is_dead);
    if (!s.ok() || is_dead) {
      return c > 0;
    }
    VLOG(1) << "Replacing " << nodes_to_replace[c]->DebugString()
            << " with constant " << output.DebugString();
    ReplaceNodeWithConstant(graph, nodes_to_replace[c], output);
  }

  // Delete the orphaned nodes in the original graph.
  for (Node* n : to_delete) {
    graph->RemoveNode(n);
  }
  return true;
}

}  // namespace tensorflow
