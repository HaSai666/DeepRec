/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

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

#include <queue>
#include <unordered_set>

#include "odl_processor/core/graph_optimizer.h"
#include "odl_processor/core/util/utils.h"
#include "tensorflow/core/graph/graph.h"
#include "tensorflow/core/protobuf/meta_graph.pb.h"
#include "tensorflow/core/graph/graph_constructor.h"

namespace tensorflow {
namespace processor {

/// IREE

namespace {

std::unordered_set<std::string> GetBlackOpsSet() {
  std::unordered_set<std::string> s = {
    "Unique",
    "SparseSegmentMean",
    "SparseSegmentMeanGrad",
    "SparseSegmentSum",
    "SparseSegmentSumGrad",
    "SparseSegmentSqrt",
    "SparseSegmentSqrtNGrad",
    "UnsortedSegmentSum",
    "UnsortedSegmentSumGrad",
    "UnsortedSegmentMax",
    "UnsortedSegmentMaxGrad",
    "HashTableV2",
    "LookupTableFindV2",
    "ParseExample",
    "Bucketize"
    // Add other ops
  };

  return s;
}

} // namespace


MetaGraphDef GetMetaGraphDefFromSavedModel(
    const std::string& saved_model_str) {
  SavedModel saved_model;
  if (!saved_model.ParseFromString(saved_model_str)) {
    LOG(FATAL) << "Can not parse saved model from pb string.";
  }

  // TODO: How about many meta graphs?
  MetaGraphDef meta_gdef = saved_model.meta_graphs()[0];

  return meta_gdef;
}

MetaGraphDef GetMetaGraphDefFromSavedModelText(
    const std::string& str) {
  SavedModel saved_model;
  if (!tensorflow::protobuf::TextFormat::ParseFromString(str, &saved_model)) {
    LOG(FATAL) << "Can not parse saved model from text.";
  }

  // TODO: How about many meta graphs?
  MetaGraphDef meta_gdef = saved_model.meta_graphs()[0];

  return meta_gdef;
} 

// Function will return static ops set
void StaticShapeCluteringStrategy::GetStaticGraphOps(
    const GraphDef& gdef,
    std::vector<std::string>& inputs,
    std::vector<std::string>& outputs,
    std::unordered_set<std::string>* static_ops_name) {
  std::unordered_map<std::string, bool> dynamic_ops_map =
      GetNodesHasDynamicShapeMap(gdef);

  std::unordered_map<std::string, bool> has_control_flow_input =
      GetNodesHasControlFlowInputs(gdef);

  std::unordered_map<std::string, const NodeDef*> nodes;
  for (const NodeDef& node : gdef.node()) {
    nodes[node.name()] = &node;
  }

  std::unordered_set<const NodeDef*> static_nodes;
  std::unordered_set<const NodeDef*> visited;
  std::queue<const NodeDef*> q;
  for (auto output : outputs) {
    q.push(nodes[output]);
    visited.insert(nodes[output]);
  }

  std::unordered_set<std::string> black_ops = GetBlackOpsSet();
  while (!q.empty()) {
    const NodeDef* curr_node = q.front();
    // 1) no control edge
    // 2) no dynamic shape
    // 3) no blacklist ops
    if (!has_control_flow_input[curr_node->name()] &&
        !dynamic_ops_map[curr_node->name()] &&
        black_ops.find(curr_node->op()) == black_ops.end()) {
      // Add op into static ops set
      //(*static_ops)[curr_node->name()] = curr_node;
      static_ops_name->insert(curr_node->name());

      for (auto in_name : curr_node->input()) {
        size_t offset = in_name.find(":");
        in_name = in_name.substr(0, offset);
        if (visited.find(nodes[in_name]) == visited.end()) {
          q.push(nodes[in_name]);
          visited.insert(nodes[in_name]);
        }
      }
    }

    q.pop();
  }
}

void StaticShapeCluteringStrategy::Run(
    const GraphDef& gdef,
    std::vector<std::string>& inputs,
    std::vector<std::string>& outputs,
    ClusteredGraphInfo* clustered_graph_info) {
  std::unordered_map<std::string, bool> dynamic_ops_map =
      GetNodesHasDynamicShapeMap(gdef);

  std::unordered_map<std::string, bool> has_control_flow_input =
      GetNodesHasControlFlowInputs(gdef);

  std::unordered_map<std::string, const NodeDef*> nodes;
  for (const NodeDef& node : gdef.node()) {
    nodes[node.name()] = &node;
  }

  GraphDef& dynamic_graph_def = clustered_graph_info->tf_subgraph;
  GraphDef& static_graph_def = clustered_graph_info->iree_subgraph;

  std::unordered_set<const NodeDef*> static_nodes;
  std::unordered_set<const NodeDef*> visited;
  std::queue<const NodeDef*> q;
  for (auto output : outputs) {
    q.push(nodes[output]);
    visited.insert(nodes[output]);
  }

  std::unordered_set<std::string> black_ops = GetBlackOpsSet();
  while (!q.empty()) {
    const NodeDef* curr_node = q.front();
    // 1) no control edge
    // 2) no dynamic shape
    // 3) no blacklist ops
    if (!has_control_flow_input[curr_node->name()] &&
        !dynamic_ops_map[curr_node->name()] &&
        black_ops.find(curr_node->op()) == black_ops.end()) {
      // Add op into static_graph_def
      NodeDef* new_node = static_graph_def.add_node();
      new_node->CopyFrom(*curr_node);
      static_nodes.insert(curr_node);

      for (auto in_name : curr_node->input()) {
        size_t offset = in_name.find(":");
        in_name = in_name.substr(0, offset);
        if (visited.find(nodes[in_name]) == visited.end()) {
          q.push(nodes[in_name]);
          visited.insert(nodes[in_name]);
        }
      }
    }

    q.pop();
  }

  // TODO: Add version and library ops

  // TODO: Add placeholder here

  for (const NodeDef& node : gdef.node()) {
    if (static_nodes.find(&node) == static_nodes.end()) {
      NodeDef* new_node = dynamic_graph_def.add_node();
      new_node->CopyFrom(node);
    }
  }

  // TODO: Add tf_signature
}

void StaticShapeCluteringStrategy::Run(
    const MetaGraphDef& mgdef,
    ClusteredGraphInfo* clustered_graph_info) {
  std::vector<std::string> inputs;
  std::vector<std::string> outputs;

  // TODO: FIXME consider several signature_defs here
  // Now only consider one signature_def
  for (auto sdef : mgdef.signature_def()) {
    for (auto input : sdef.second.inputs()) {
      // convert "input_example_tensor:0" to "input_example_tensor"
      std::string name = input.second.name();
      size_t offset = name.find(":");
      inputs.push_back(name.substr(0, offset));
    }

    for (auto output : sdef.second.outputs()) {
      // convert "Reshape_2:0" to "Reshape_2"
      std::string name = output.second.name();
      size_t offset = name.find(":");
      outputs.push_back(name.substr(0, offset));
    }

    // TODO: FIXME
    break;
  }

  Run(mgdef.graph_def(), inputs, outputs,
      clustered_graph_info);
}

void StaticShapeCluteringStrategy::GetDynamicAndStaticSignatureDef(
    const MetaGraphDef& mgdef,
    std::map<string, SignatureDef>* dynamic_sdef,
    std::map<string, SignatureDef>* static_sdef) {
  Graph graph(OpRegistry::Global());
  GraphConstructorOptions opts;
  opts.allow_internal_ops = true;
  opts.allow_internal_ops = true;
 
  Status s = ConvertGraphDefToGraph(opts, mgdef.graph_def(), &graph);
  if (!s.ok()) {
    LOG(FATAL) << "can not convert graphdef to graph, " << s.error_message();
  }
  std::unordered_map<std::string, Node*> nodes_map;
  for (Node* n : graph.nodes()) {
    nodes_map[n->name()] = n;
  }

  // maybe have many signature_defs in the meta graphdef
  for (auto sdef : mgdef.signature_def()) {
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;
    // filter the dupilication ops, like
    // key: "in_A:0" and key: "in_A:1",
    std::unordered_set<std::string> existed;
    for (auto input : sdef.second.inputs()) {
      // convert "input_example_tensor:0" to "input_example_tensor"
      std::string name = input.second.name();
      size_t offset = name.find(":");
      name = name.substr(0, offset);
      if (existed.find(name) != existed.end()) continue;
      existed.insert(name);
      inputs.push_back(name);
    }

    existed.clear();
    for (auto output : sdef.second.outputs()) {
      // convert "Reshape_2:0" to "Reshape_2"
      std::string name = output.second.name();
      size_t offset = name.find(":");
      name = name.substr(0, offset);
      if (existed.find(name) != existed.end()) continue;
      existed.insert(name);
      outputs.push_back(name);
    }

    // get those ops which will be insert into static graphdef
    std::unordered_set<std::string> static_ops_name;
    GetStaticGraphOps(mgdef.graph_def(), inputs, outputs, &static_ops_name);

    std::string signature_def_key(sdef.first);
    // should create a static graphdef
    if (static_ops_name.size() > 0) {
      (*dynamic_sdef)[signature_def_key] = sdef.second;
      (*static_sdef)[signature_def_key] = sdef.second;
      (*dynamic_sdef)[signature_def_key].clear_inputs();
      (*dynamic_sdef)[signature_def_key].clear_outputs();
      (*static_sdef)[signature_def_key].clear_inputs();
      (*static_sdef)[signature_def_key].clear_outputs();

      // 1) set signature_def output of static/dynamic graphdef
      for (auto output : sdef.second.outputs()) {
        std::string name = output.second.name();
        size_t offset = name.find(":");
        name = name.substr(0, offset);
        if (static_ops_name.find(name) != static_ops_name.end()) {
          (*(*static_sdef)[signature_def_key].mutable_outputs())[output.first] =
              output.second;
        } else {
          (*(*dynamic_sdef)[signature_def_key].mutable_outputs())[output.first] =
              output.second;
        }
      }

      // 2) set signature_def input of static/dynamic graphdef
      for (auto input : sdef.second.inputs()) {
        std::string name = input.second.name();
        size_t offset = name.find(":");
        name = name.substr(0, offset);
        if (static_ops_name.find(name) != static_ops_name.end()) {
          (*(*static_sdef)[signature_def_key].mutable_inputs())[input.first] =
              input.second;
        } else {
          (*(*dynamic_sdef)[signature_def_key].mutable_inputs())[input.first] =
              input.second;
        }
      }
 
      std::unordered_map<std::string, const NodeDef*> nodes;
      for (const NodeDef& node : mgdef.graph_def().node()) {
        nodes[node.name()] = &node;
      }

      // 3) create signature_def of those ops connect dynamic and static graph
      for (auto& op_name : static_ops_name) {
        for (std::string in_name : nodes[op_name]->input()) {
          // NOTE(jiankeng.pt): NO control edge here, so no need to consider '^AAA' case
          auto offset = in_name.find(":");
          in_name = in_name.substr(0, offset);
          // the node named 'in_name' in dynamic graph,
          // so need add the node into the input signature of static graph,
          // and add it into the output signature of dynamic graph.
          if (static_ops_name.find(in_name) == static_ops_name.end()) {
            std::vector<std::vector<int>> shapes;
            NodeDef* tmp_in_def = const_cast<NodeDef*>(nodes[in_name]);
            AttrValue attr_value = (*tmp_in_def->mutable_attr())["_output_shapes"];
            int output_tensor_count = -1;
            Status s = AttrValueHasType(attr_value, "list(shape)");
            if (!s.ok()) {
              s = AttrValueHasType(attr_value, "shape");
              if (!s.ok()) {
                LOG(FATAL) << "Can not found the _output_shapes attr, "
                           << "we dont know the output tensor count.";
              }
              std::vector<int> op_shape;
              op_shape.push_back(attr_value.shape().dim().size());
              shapes.push_back(op_shape);
              output_tensor_count = 1;
            } else {
              output_tensor_count = 0;
              for (const auto& curr_shape : attr_value.list().shape()) {
                ++output_tensor_count;
                std::vector<int> op_shape;
                for (auto d : curr_shape.dim()) {
                  op_shape.push_back(d.size());
                }
                shapes.push_back(op_shape);
              }
            }

            // add signature
            std::string input_base_name = "static_sig_inputs_" + in_name + "_";
            std::string output_base_name = "dynamic_sig_outputs_" + in_name + "_";
            for (auto i = 0; i < output_tensor_count; ++i) {
              std::string target_name = in_name + ":" + std::to_string(i);
              TensorInfo tinfo;
              tinfo.set_name(target_name);
              tinfo.set_dtype(nodes_map[in_name]->output_type(i));
              for (size_t j = 0; j < shapes[i].size(); ++j) {
                tinfo.mutable_tensor_shape()->add_dim()->set_size(shapes[i][j]);
              }

              // input signature for static graph
              std::string in_key_name = input_base_name + std::to_string(i);
              (*(*static_sdef)[signature_def_key].mutable_inputs())[in_key_name] = tinfo;

              // output signature for dynamic graph
              std::string out_key_name = output_base_name + std::to_string(i);
              (*(*dynamic_sdef)[signature_def_key].mutable_outputs())[out_key_name] = tinfo;
            }
          }
        }
      }
    } else {
      (*dynamic_sdef)[signature_def_key] = sdef.second;
      (*static_sdef)[signature_def_key] = SignatureDef();
    }
  }
}

void StaticShapeCluteringStrategy::GetDynamicAndStaticMetaGraphDef(
    const MetaGraphDef& mgdef,
    MetaGraphDef* dynamic_mgdef,
    MetaGraphDef* static_mgdef) {
  std::map<string, SignatureDef> dynamic_sdef, static_sdef;
  GetDynamicAndStaticSignatureDef(mgdef, &dynamic_sdef, &static_sdef);

  *dynamic_mgdef = mgdef;
  dynamic_mgdef->clear_signature_def();
  auto dyn_sig_def = dynamic_mgdef->mutable_signature_def();
  for (auto sdef : dynamic_sdef) {
    (*dyn_sig_def)[sdef.first] = sdef.second;
  }

  *static_mgdef = mgdef;
  static_mgdef->clear_signature_def();
  auto sta_sig_def = static_mgdef->mutable_signature_def();
  for (auto sdef : static_sdef) {
    (*sta_sig_def)[sdef.first] = sdef.second;
  }
}

void StaticShapeCluteringStrategy::Run(
    const std::string& tag,
    const SavedModel& saved_model,
    ClusteredGraphInfo* clustered_graph_info) {
  clustered_graph_info->tf_saved_model.set_saved_model_schema_version(
      saved_model.saved_model_schema_version());
  clustered_graph_info->iree_saved_model.set_saved_model_schema_version(
      saved_model.saved_model_schema_version());

  // maybe have many meta_graphs here, select the
  // meta graphdef according the tag.
  for (const MetaGraphDef& mgdef : saved_model.meta_graphs()) {
    bool is_target = false;
    for (auto t : mgdef.meta_info_def().tags()) {
      if (t == tag) {
        is_target = true;
        break;
      }
    }
    if (!is_target) continue;

    MetaGraphDef* dynamic_mgdef =
      clustered_graph_info->tf_saved_model.add_meta_graphs();
    MetaGraphDef* static_mgdef =
      clustered_graph_info->iree_saved_model.add_meta_graphs();

    GetDynamicAndStaticMetaGraphDef(mgdef, dynamic_mgdef, static_mgdef);

    break;
  }
  // return:
  // clustered_graph_info->tf_saved_model
  // clustered_graph_info->iree_saved_model
}

ClusteredGraphInfo ClusteringGraphDef(
    const MetaGraphDef& mgdef,
    CluteringStrategy* cluster_strategy) {
  static StaticShapeCluteringStrategy static_strategy;
  if (cluster_strategy == nullptr) {
    cluster_strategy = &static_strategy;
  }

  ClusteredGraphInfo info;
  cluster_strategy->Run(mgdef, &info);

  return info;
}

ClusteredGraphInfo ClusteringGraphDef(
    const std::string& tag,
    const SavedModel& saved_model,
    CluteringStrategy* cluster_strategy) {
  static StaticShapeCluteringStrategy static_strategy;
  if (cluster_strategy == nullptr) {
    cluster_strategy = &static_strategy;
  }

  ClusteredGraphInfo info;
  cluster_strategy->Run(tag, saved_model, &info);

  return info;
}

ClusteredGraphInfo ClusteringGraphDef(
    const std::string& tag,
    const std::string& saved_model_str,
    CluteringStrategy* cluster_strategy) {
  SavedModel saved_model;
  if (!tensorflow::protobuf::TextFormat::ParseFromString(
      saved_model_str, &saved_model)) {
    LOG(FATAL) << "Can not parse saved model from text.";
  }

  return ClusteringGraphDef(tag, saved_model, cluster_strategy);
}

/// Tensorflow

SavedModelOptimizer::SavedModelOptimizer(
    const std::string& signature_name,
    MetaGraphDef* mgdef)
  : signature_name_(signature_name), meta_graph_def_(mgdef) {
}

SavedModelOptimizer::~SavedModelOptimizer() {

}

void SavedModelOptimizer::Optimize() {

  FreezeSignatureDef();

  // For example:
  // convert KvResourceGather to KvLookup
  // convert KvResourceImportV2 to KvInsert
  ConvertKVOps();

  RewriteDefaultValueOp();

  // Add other passes here
}

namespace {

struct SrcInfo {
  Node* src_node;
  int src_slot;
};

void ReplaceKVOpsWithLookupOrInsertOps (
    const std::string& remote_op_name,
    Node* node, Graph* graph,
    std::vector<SrcInfo>& input_info,
    std::unordered_map<std::string, AttrValue*>& attr_info) {

  // Create KvLookup/KvInsert op here and remove the node
  NodeDef remote_def;
  remote_def.set_name(node->name());
  remote_def.set_op(remote_op_name);

  // Set attrs
  for (auto attr : attr_info) {
    (*remote_def.mutable_attr())[attr.first] = *(attr.second);
  }

  Status status;
  Node* remote_lookup_node = graph->AddNode(remote_def, &status);
  TF_CHECK_OK(status);

  // Add input egdes
  for (size_t i = 0; i < input_info.size(); ++i) {
    graph->AddEdge(input_info[i].src_node,
                   input_info[i].src_slot,
                   remote_lookup_node, i);
  }

  // Add output edges
  for (const Edge* edge : node->out_edges()) {
    graph->AddEdge(remote_lookup_node, edge->src_output(),
                   edge->dst(), edge->dst_input());
  }

  // remove current node
  graph->RemoveNode(node);
}

} // namespace

void SavedModelOptimizer::ConvertKVOps() {
  Graph graph(OpRegistry::Global());
  GraphConstructorOptions opts;
  opts.allow_internal_ops = true;
  opts.allow_internal_ops = true;
  Status s = ConvertGraphDefToGraph(
      opts, meta_graph_def_->graph_def(), &graph);
  if (!s.ok()) {
    LOG(FATAL) << "can not convert graphdef to graph, " << s.error_message();
  }

  // Add a placeholder for version which set by user.
  NodeDef version_def;
  version_def.set_name("odl_version");
  version_def.set_op("Placeholder");
  (*version_def.mutable_attr())["dtype"].set_type(DT_STRING);
  Status status;
  Node* version_node = graph.AddNode(version_def, &status);
  TF_CHECK_OK(status);

  // Find sparse lookup/Import ops and replace them
  // with KvLookup and KvInsert
  for (Node* node : graph.nodes()) {
    // Get input edges
    std::vector<const Edge*> input_edges;
    // Check the edges' order
    for (const Edge* edge : node->in_edges()) {
      input_edges.push_back(edge);
    }

    std::vector<SrcInfo> input_info;
    std::unordered_map<std::string, AttrValue*> attr_info;

    if (node->op_def().name() == "KvResourceGather") {
      // version
      input_info.push_back(SrcInfo{version_node, 0});
      // indices
      input_info.push_back(SrcInfo{input_edges[1]->src(),
                                   input_edges[1]->src_output()});
      // default_value
      input_info.push_back(SrcInfo{input_edges[2]->src(),
                                   input_edges[2]->src_output()});

      AttrValue var_name_value;
      SetAttrValue(input_edges[0]->src()->name(), &var_name_value);
      AttrValue* dtype_value =
        const_cast<AttrValue*>(node->attrs().Find("dtype"));
      AttrValue* tkeys_value =
        const_cast<AttrValue*>(node->attrs().Find("Tkeys"));
      if (!dtype_value || !tkeys_value) {
        LOG(FATAL) << "Miss dtype or Tkeys attr, " << node->DebugString();
      }
      attr_info["var_name"] = &var_name_value;
      attr_info["dtype"] = dtype_value;
      attr_info["Tkeys"] = tkeys_value;

      ReplaceKVOpsWithLookupOrInsertOps(
          "KvLookup", node, &graph, input_info, attr_info);

    } else if (node->op_def().name() == "KvResourceImportV2") {
      // version
      input_info.push_back(SrcInfo{version_node, 0});
      // prefix
      input_info.push_back(SrcInfo{input_edges[0]->src(),
                                   input_edges[0]->src_output()});
      // tensor_names
      input_info.push_back(SrcInfo{input_edges[3]->src(),
                                   input_edges[3]->src_output()});
 
      AttrValue var_name_value;
      SetAttrValue(input_edges[1]->src()->name(), &var_name_value);
      attr_info["var_name"] = &var_name_value;

      ReplaceKVOpsWithLookupOrInsertOps(
          "KvInsert", node, &graph, input_info, attr_info);
    }
  }

  // replace the graph def in saved_model_bundle
  graph.ToGraphDef(meta_graph_def_->mutable_graph_def());
}

void SavedModelOptimizer::RewriteDefaultValueOp() {

}

void SavedModelOptimizer::FreezeSignatureDef() {
  std::map<string, SignatureDef> new_signature_def;
  bool found = false;
  for (auto sdef : meta_graph_def_->signature_def()) {
    if (sdef.first == signature_name_) {
      new_signature_def[signature_name_] = sdef.second;
      found = true;
      break;
    }
  }

  if (!found) {
    LOG(FATAL) << "Not found the signature_def with user specified signature name.";
  }

  meta_graph_def_->clear_signature_def();
  auto sig_def = meta_graph_def_->mutable_signature_def();
  for (auto sdef : new_signature_def) {
    (*sig_def)[sdef.first] = sdef.second;
  }
}

} // namespace processor
} // namespace tensorflow

