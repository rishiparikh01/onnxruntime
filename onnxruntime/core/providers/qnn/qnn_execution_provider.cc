// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License

#include "qnn_execution_provider.h"

#include <filesystem>
#include "core/providers/common.h"
#include "core/framework/compute_capability.h"
#include "core/graph/graph_viewer.h"
#include "core/session/onnxruntime_session_options_config_keys.h"
#include "core/session/onnxruntime_cxx_api.h"
#include "core/framework/kernel_registry.h"
#include "core/providers/partitioning_utils.h"
#include "core/providers/qnn/builder/op_builder_factory.h"
#include "core/providers/partitioning_utils.h"
#include "core/providers/qnn/builder/qnn_model_wrapper.h"
#include "core/providers/qnn/builder/op_builder_factory.h"
#include "core/providers/qnn/builder/qnn_def.h"

namespace onnxruntime {

constexpr const char* QNN = "QNN";

std::string GetFileNameFromModelPath(onnxruntime::Path model_path) {
  auto model_path_components = model_path.GetComponents();
  // There's no model path if model loaded from buffer stead of file
  if (model_path_components.empty()) {
    return "";
  }
  return PathToUTF8String(model_path_components.back());
}

void QNNExecutionProvider::ParseProfilingLevel(std::string profiling_level_string) {
  std::transform(profiling_level_string.begin(),
                 profiling_level_string.end(),
                 profiling_level_string.begin(),
                 [](unsigned char c) { return static_cast<unsigned char>(std::tolower(c)); });
  LOGS_DEFAULT(VERBOSE) << "profiling_level: " << profiling_level_string;
  if (profiling_level_string == "off") {
    profiling_level_ = qnn::ProfilingLevel::OFF;
  } else if (profiling_level_string == "basic") {
    profiling_level_ = qnn::ProfilingLevel::BASIC;
  } else if (profiling_level_string == "detailed") {
    profiling_level_ = qnn::ProfilingLevel::DETAILED;
  } else {
    LOGS_DEFAULT(WARNING) << "Profiling level not valid.";
  }
}

void QNNExecutionProvider::ParseHtpPerformanceMode(std::string htp_performance_mode_string) {
  std::transform(htp_performance_mode_string.begin(),
                 htp_performance_mode_string.end(),
                 htp_performance_mode_string.begin(),
                 [](unsigned char c) { return static_cast<unsigned char>(std::tolower(c)); });
  LOGS_DEFAULT(VERBOSE) << "Htp performance mode: " << htp_performance_mode_string;
  if (htp_performance_mode_string == "burst") {
    htp_performance_mode_ = qnn::HtpPerformanceMode::kHtpBurst;
  } else if (htp_performance_mode_string == "balanced") {
    htp_performance_mode_ = qnn::HtpPerformanceMode::kHtpBalanced;
  } else if (htp_performance_mode_string == "default") {
    htp_performance_mode_ = qnn::HtpPerformanceMode::kHtpDefault;
  } else if (htp_performance_mode_string == "high_performance") {
    htp_performance_mode_ = qnn::HtpPerformanceMode::kHtpHighPerformance;
  } else if (htp_performance_mode_string == "high_power_saver") {
    htp_performance_mode_ = qnn::HtpPerformanceMode::kHtpHighPowerSaver;
  } else if (htp_performance_mode_string == "low_balanced") {
    htp_performance_mode_ = qnn::HtpPerformanceMode::kHtpLowBalanced;
  } else if (htp_performance_mode_string == "low_power_saver") {
    htp_performance_mode_ = qnn::HtpPerformanceMode::kHtpLowPowerSaver;
  } else if (htp_performance_mode_string == "power_saver") {
    htp_performance_mode_ = qnn::HtpPerformanceMode::kHtpPowerSaver;
  } else if (htp_performance_mode_string == "sustained_high_performance") {
    htp_performance_mode_ = qnn::HtpPerformanceMode::kHtpSustainedHighPerformance;
  } else {
    LOGS_DEFAULT(WARNING) << "Htp performance mode not valid.";
  }
}

void QNNExecutionProvider::ParseHtpGraphFinalizationOptimizationMode(const std::string& htp_graph_finalization_opt_mode_string) {
  LOGS_DEFAULT(VERBOSE) << "HTP graph finalization optimization mode: "
                        << htp_graph_finalization_opt_mode_string;

  if (htp_graph_finalization_opt_mode_string.empty() || htp_graph_finalization_opt_mode_string == "0") {
    htp_graph_finalization_opt_mode_ = qnn::HtpGraphFinalizationOptimizationMode::kDefault;
  } else if (htp_graph_finalization_opt_mode_string == "1") {
    htp_graph_finalization_opt_mode_ = qnn::HtpGraphFinalizationOptimizationMode::kMode1;
  } else if (htp_graph_finalization_opt_mode_string == "2") {
    htp_graph_finalization_opt_mode_ = qnn::HtpGraphFinalizationOptimizationMode::kMode2;
  } else if (htp_graph_finalization_opt_mode_string == "3") {
    htp_graph_finalization_opt_mode_ = qnn::HtpGraphFinalizationOptimizationMode::kMode3;
  } else {
    LOGS_DEFAULT(WARNING) << "Invalid HTP graph finalization optimization mode: "
                          << htp_graph_finalization_opt_mode_string;
  }
}

QNNExecutionProvider::QNNExecutionProvider(const ProviderOptions& provider_options_map,
                                           const SessionOptions* session_options)
    : IExecutionProvider{onnxruntime::kQnnExecutionProvider, true},
      runtime_options_(provider_options_map) {
  if (session_options) {
    disable_cpu_ep_fallback_ = session_options->config_options.GetConfigOrDefault(
                                   kOrtSessionOptionsDisableCPUEPFallback, "0") == "1";
  }

  static const std::string CONTEXT_CACHE_ENABLED = "qnn_context_cache_enable";
  auto context_cache_enabled_pos = runtime_options_.find(CONTEXT_CACHE_ENABLED);
  if (context_cache_enabled_pos != runtime_options_.end()) {
    if (context_cache_enabled_pos->second == "1") {
      context_cache_enabled_ = true;
      LOGS_DEFAULT(VERBOSE) << "Context cache enabled.";
    }
  }

  static const std::string CONTEXT_CACHE_PATH = "qnn_context_cache_path";
  auto context_cache_path_pos = runtime_options_.find(CONTEXT_CACHE_PATH);
  if (context_cache_path_pos != runtime_options_.end()) {
    context_cache_path_ = context_cache_path_pos->second;
    LOGS_DEFAULT(VERBOSE) << "User specified context cache path: " << context_cache_path_;
  }

  bool qnn_context_embed_mode = true;
  static const std::string CONTEXT_CACHE_EMBED_MODE = "qnn_context_embed_mode";
  auto context_cache_embed_mode_pos = runtime_options_.find(CONTEXT_CACHE_EMBED_MODE);
  if (context_cache_embed_mode_pos != runtime_options_.end()) {
    qnn_context_embed_mode = context_cache_embed_mode_pos->second == "1";
    LOGS_DEFAULT(VERBOSE) << "User specified context cache embed mode: " << qnn_context_embed_mode;
  }

  static const std::string BACKEND_PATH = "backend_path";
  auto backend_path_pos = runtime_options_.find(BACKEND_PATH);

  std::string backend_path;
  if (backend_path_pos != runtime_options_.end()) {
    backend_path = backend_path_pos->second;
    LOGS_DEFAULT(VERBOSE) << "Backend path: " << backend_path;
  } else {
    LOGS_DEFAULT(ERROR) << "No backend path provided.";
  }

  static const std::string PROFILING_LEVEL = "profiling_level";
  auto profiling_level_pos = runtime_options_.find(PROFILING_LEVEL);
  if (profiling_level_pos != runtime_options_.end()) {
    ParseProfilingLevel(profiling_level_pos->second);
  }

  static const std::string RPC_CONTROL_LANTENCY = "rpc_control_latency";
  auto latency_pos = runtime_options_.find(RPC_CONTROL_LANTENCY);
  if (latency_pos != runtime_options_.end()) {
    rpc_control_latency_ = static_cast<uint32_t>(std::stoul(latency_pos->second));
    LOGS_DEFAULT(VERBOSE) << "rpc_control_latency: " << rpc_control_latency_;
  }

  htp_performance_mode_ = qnn::HtpPerformanceMode::kHtpDefault;
  static const std::string HTP_PERFORMANCE_MODE = "htp_performance_mode";
  auto htp_performance_mode_pos = runtime_options_.find(HTP_PERFORMANCE_MODE);
  if (htp_performance_mode_pos != runtime_options_.end()) {
    ParseHtpPerformanceMode(htp_performance_mode_pos->second);
  }

  htp_graph_finalization_opt_mode_ = qnn::HtpGraphFinalizationOptimizationMode::kDefault;
  static const std::string HTP_GRAPH_FINALIZATION_OPT_MODE = "htp_graph_finalization_optimization_mode";
  auto htp_graph_finalization_opt_mode_pos = runtime_options_.find(HTP_GRAPH_FINALIZATION_OPT_MODE);
  if (htp_graph_finalization_opt_mode_pos != runtime_options_.end()) {
    ParseHtpGraphFinalizationOptimizationMode(htp_graph_finalization_opt_mode_pos->second);
  }

  // Enable use of QNN Saver if the user provides a path the QNN Saver backend library.
  static const std::string QNN_SAVER_PATH_KEY = "qnn_saver_path";
  std::string qnn_saver_path;
  auto qnn_saver_path_pos = runtime_options_.find(QNN_SAVER_PATH_KEY);
  if (qnn_saver_path_pos != runtime_options_.end()) {
    qnn_saver_path = qnn_saver_path_pos->second;
    LOGS_DEFAULT(VERBOSE) << "User specified QNN Saver path: " << qnn_saver_path;
  }

  qnn_backend_manager_ = std::make_unique<qnn::QnnBackendManager>(
      std::move(backend_path),
      profiling_level_,
      rpc_control_latency_,
      htp_performance_mode_,
      std::move(qnn_saver_path));
  qnn_cache_model_handler_ = std::make_unique<qnn::QnnCacheModelHandler>(qnn_context_embed_mode);
}

bool QNNExecutionProvider::IsNodeSupported(qnn::QnnModelWrapper& qnn_model_wrapper, const NodeUnit& node_unit,
                                           std::unordered_map<const NodeUnit*, bool>& node_unit_supported_result,
                                           const logging::Logger& logger) const {
  // If we have visited one of the nodes in the node_unit, use the result directly
  const auto it = node_unit_supported_result.find(&node_unit);
  if (it != node_unit_supported_result.cend()) {
    return it->second;
  } else {
    const std::string& op_type = node_unit.OpType();

    bool supported = false;
    const auto* op_builder = qnn::GetOpBuilder(op_type);
    if (op_builder == nullptr) {
      LOGS(logger, WARNING) << "Operators of type `" << node_unit.OpType() << "` are not supported by QNN EP."
                            << node_unit.OpType() << " node `" << node_unit.Name()
                            << "` will not be assigned to QNN EP.";
    } else {
      auto status = op_builder->IsOpSupported(qnn_model_wrapper,
                                              node_unit, logger);
      if (Status::OK() != status) {
        LOGS(logger, WARNING) << node_unit.OpType() << " node `" << node_unit.Name()
                              << "` is not supported: " << status.ErrorMessage();
      }
      supported = (Status::OK() == status);
    }
    node_unit_supported_result[&node_unit] = supported;
    return supported;
  }
}

std::unordered_set<const Node*>
QNNExecutionProvider::GetSupportedNodes(const GraphViewer& graph_viewer,
                                        const std::unordered_map<const Node*, const NodeUnit*>& node_unit_map,
                                        const size_t node_unit_size,
                                        bool load_from_cached_context,
                                        const logging::Logger& logger) const {
  std::unordered_set<const Node*> supported_nodes{};
  // Enable Qnn context cache requires the whole graph partitioned to Qnn EP
  // Blindly filter in all nodes if context cache is enabled
  if (load_from_cached_context) {
    for (const auto& node : graph_viewer.Nodes()) {
      supported_nodes.insert(&node);
    }
    return supported_nodes;
  }

  // This holds the result of whether a NodeUnit is supported or not,
  // to prevent nodes in a NodeUnit to be checked for multiple times
  std::unordered_map<const NodeUnit*, bool> node_unit_supported_result;
  node_unit_supported_result.reserve(node_unit_size);

  std::unordered_set<std::string> initializer_input_lookup;
  auto graph_initializers = graph_viewer.GetAllInitializedTensors();
  for (auto graph_ini : graph_initializers) {
    initializer_input_lookup.emplace(graph_ini.first);
  }

  // Util function that initializes a table that maps a graph input or output name to its index.
  auto init_input_output_index_map = [](std::unordered_map<std::string, size_t>& index_map,
                                        const std::vector<const NodeArg*>& node_args) {
    const size_t num_args = node_args.size();
    for (size_t i = 0; i < num_args; i++) {
      index_map.emplace(node_args[i]->Name(), i);
    }
  };

  std::unordered_map<std::string, size_t> model_input_index_map;
  init_input_output_index_map(model_input_index_map, graph_viewer.GetInputs());  // GetInputs excludes initializers.

  std::unordered_map<std::string, size_t> model_output_index_map;
  init_input_output_index_map(model_output_index_map, graph_viewer.GetOutputs());

  auto qnn_model_wrapper = qnn::QnnModelWrapper(graph_viewer, logger,
                                                qnn_backend_manager_->GetQnnInterface(),
                                                qnn_backend_manager_->GetQnnBackendHandle(),
                                                model_input_index_map,
                                                model_output_index_map,
                                                initializer_input_lookup,
                                                qnn_backend_manager_->GetQnnBackendType());

  const auto& node_indices = graph_viewer.GetNodesInTopologicalOrder();
  for (size_t i = 0; i < node_indices.size(); i++) {
    gsl::not_null<const onnxruntime::Node*> node(graph_viewer.GetNode(node_indices[i]));

    // Get the node_unit associated with the node. Note that the node may not be the node_unit's target node.
    const NodeUnit* node_unit = node_unit_map.at(node);

    // Visiting 'nodes' in topological order does not guarantee that 'node_units' are
    // also visited in topological order. Skip this node if it is not the node_unit's target node
    // to ensure 'node_units' are visited in topological order.
    if (node != &node_unit->GetNode()) {
      continue;
    }
    const bool supported = IsNodeSupported(qnn_model_wrapper,
                                           *node_unit,
                                           node_unit_supported_result,
                                           logger);
    LOGS(logger, VERBOSE) << "Node supported: [" << supported
                          << "] index: [" << node->Index()
                          << "] name: [" << node->Name()
                          << "] Operator type: [" << node->OpType()
                          << "] as part of the NodeUnit type: [" << node_unit->OpType()
                          << "] index: [" << node_unit->Index()
                          << "] name: [" << node_unit->Name()
                          << "]";
    if (supported) {
      // If the node_unit is supported, add all of its nodes to the supported list.
      for (const auto* node_in_group : node_unit->GetAllNodesInGroup()) {
        supported_nodes.insert(node_in_group);
      }
    }
  }

  return supported_nodes;
}

std::vector<std::unique_ptr<ComputeCapability>>
QNNExecutionProvider::GetCapability(const onnxruntime::GraphViewer& graph_viewer,
                                    const IKernelLookup& /*kernel_lookup*/) const {
  std::vector<std::unique_ptr<ComputeCapability>> result;

  if (graph_viewer.IsSubgraph()) {
    return result;
  }

  const auto& logger = *GetLogger();
  bool load_from_cached_context = false;
  bool is_qnn_ctx_model = qnn::IsQnnCtxModel(graph_viewer);
  if (is_qnn_ctx_model) {
    load_from_cached_context = true;
  }

  // This is for case: QDQ model + Onnx Qnn context cache model
  if (context_cache_enabled_ && !is_qnn_ctx_model) {
    load_from_cached_context = qnn_cache_model_handler_->IsContextCacheFileExists(context_cache_path_,
                                                                                  graph_viewer.Description(),
                                                                                  graph_viewer.ModelPath().ToPathString());
  }

  // Load from cached context will load the QnnSystem lib and skip the Qnn context creation
  auto rt = qnn_backend_manager_->SetupBackend(logger, load_from_cached_context);
  if (Status::OK() != rt) {
    LOGS(logger, ERROR) << "QNN SetupBackend failed " << rt.ErrorMessage();
    return result;
  }

  if ((context_cache_enabled_ || is_qnn_ctx_model) && !IsNpuBackend(qnn_backend_manager_->GetQnnBackendType())) {
    LOGS(logger, ERROR) << "Qnn context cache only works for HTP or DSP backend.";
    return result;
  }

  // Get all the NodeUnits in the graph_viewer
  std::vector<std::unique_ptr<NodeUnit>> node_unit_holder;
  std::unordered_map<const Node*, const NodeUnit*> node_unit_map;

  std::tie(node_unit_holder, node_unit_map) = GetAllNodeUnits(graph_viewer);

  const auto supported_nodes = GetSupportedNodes(graph_viewer, node_unit_map, node_unit_holder.size(),
                                                 load_from_cached_context, logger);

  // Helper function that returns a string that lists all unsupported nodes.
  // Ex: { name: mul_123, type: Mul }, {}, ...
  auto get_unsupported_node_names = [&node_unit_holder, &supported_nodes]() -> std::string {
    std::stringstream ss;
    const size_t num_node_units = node_unit_holder.size();

    for (size_t i = 0; i < num_node_units; ++i) {
      const auto& node_unit = node_unit_holder[i];

      if (supported_nodes.find(&node_unit->GetNode()) == supported_nodes.end()) {
        ss << "{ name: " << node_unit->Name() << ", type: " << node_unit->OpType() << " }";
        if (i == num_node_units - 1) {
          ss << ", ";
        }
      }
    }

    return ss.str();
  };

  if (supported_nodes.empty()) {
    LOGS(logger, INFO) << "Number of partitions supported by QNN EP: 0";
    return result;
  }

  const auto gen_metadef_name = [&]() {
    uint64_t model_hash;
    int metadef_id = GenerateMetaDefId(graph_viewer, model_hash);
    return MakeString(QNN, "_", model_hash, "_", metadef_id);
  };

  const size_t num_nodes_in_graph = static_cast<size_t>(graph_viewer.NumberOfNodes());
  size_t num_of_supported_nodes = 0;

  // Create partitions from supported nodes.
  {
    std::vector<std::unique_ptr<ComputeCapability>> partitions = utils::CreateSupportedPartitions(graph_viewer,
                                                                                                  supported_nodes, {},
                                                                                                  gen_metadef_name, QNN,
                                                                                                  kQnnExecutionProvider,
                                                                                                  true);

    // Filter out partitions that consist of a single QuantizeLinear or DequantizeLinear node.
    // We also count the number of supported nodes in all valid partitions.
    for (auto& partition : partitions) {
      bool is_valid_partition = true;
      size_t nodes_in_partition = 0;

      if (partition && partition->sub_graph) {
        nodes_in_partition = partition->sub_graph->nodes.size();

        if (nodes_in_partition == 1) {
          const Node* node = graph_viewer.GetNode(partition->sub_graph->nodes[0]);

          if (!node) {
            LOGS(logger, ERROR) << "QNN EP: Invalid node in partition of one node.";
            is_valid_partition = false;
          } else if (node->OpType() == "QuantizeLinear" || node->OpType() == "DequantizeLinear") {
            LOGS(logger, WARNING) << "QNN EP does not support a single Quantize/Dequantize node in a partition.";
            is_valid_partition = false;
          }
        }
      } else {
        LOGS(logger, ERROR) << "QNN EP: Invalid partition.";
        is_valid_partition = false;
      }

      if (is_valid_partition) {
        result.push_back(std::move(partition));
        num_of_supported_nodes += nodes_in_partition;
      }
    }
  }

  const size_t num_of_partitions = result.size();

  if (!is_qnn_ctx_model && load_from_cached_context && 1 == num_of_partitions) {
    rt = qnn_cache_model_handler_->ValidateWithContextFile(GetFileNameFromModelPath(graph_viewer.ModelPath()),
                                                           result[0]->sub_graph->GetMetaDef()->name,
                                                           logger);
    if (Status::OK() != rt) {
      LOGS(logger, ERROR) << "QNN failed to validate context cache metadata: " << rt.ErrorMessage();
      return result;
    }
  }

  const auto summary_msg = MakeString("Number of partitions supported by QNN EP: ", num_of_partitions,
                                      ", number of nodes in the graph: ", num_nodes_in_graph,
                                      ", number of nodes supported by QNN: ", num_of_supported_nodes);
  LOGS(logger, INFO) << summary_msg;

  // Print list of unsupported nodes to the ERROR logger if the CPU EP
  // has been disabled for this inference session.
  if (disable_cpu_ep_fallback_ && num_nodes_in_graph != num_of_supported_nodes) {
    LOGS(logger, ERROR) << "Unsupported nodes in QNN EP: " << get_unsupported_node_names();
  }

  return result;
}

DataLayout QNNExecutionProvider::GetPreferredLayout() const {
  return DataLayout::NHWC;
}

Status QNNExecutionProvider::CreateComputeFunc(std::vector<NodeComputeInfo>& node_compute_funcs,
                                               const logging::Logger& logger) {
  NodeComputeInfo compute_info;
  compute_info.create_state_func = [&](ComputeContext* context, FunctionState* state) {
    LOGS(logger, VERBOSE) << "compute_info.create_state_func context->node_name: " << context->node_name;
    *state = qnn_models_[context->node_name].get();
    return 0;
  };

  compute_info.release_state_func = [](FunctionState state) {
    // the 'state' is a qnn::QnnModel managed by unique_ptr
    ORT_UNUSED_PARAMETER(state);
  };

  compute_info.compute_func = [](FunctionState state, const OrtApi*, OrtKernelContext* context) {
    Ort::KernelContext ctx(context);
    qnn::QnnModel* model = reinterpret_cast<qnn::QnnModel*>(state);
    Status result = model->ExecuteGraph(ctx);
    return result;
  };

  node_compute_funcs.push_back(compute_info);

  return Status::OK();
}

void QNNExecutionProvider::InitQnnGraphConfigs(qnn::QnnGraphConfigsBuilder& configs_builder) const {
  if (qnn_backend_manager_->GetQnnBackendType() == qnn::QnnBackendType::HTP &&
      htp_graph_finalization_opt_mode_ != qnn::HtpGraphFinalizationOptimizationMode::kDefault) {
    QnnHtpGraph_CustomConfig_t& htp_graph_opt_config = configs_builder.PushHtpGraphCustomConfig();
    htp_graph_opt_config.option = QNN_HTP_GRAPH_CONFIG_OPTION_OPTIMIZATION;
    htp_graph_opt_config.optimizationOption.type = QNN_HTP_GRAPH_OPTIMIZATION_TYPE_FINALIZE_OPTIMIZATION_FLAG;
    htp_graph_opt_config.optimizationOption.floatValue = static_cast<float>(htp_graph_finalization_opt_mode_);

    QnnGraph_Config_t& graph_opt_config = configs_builder.PushGraphConfig();
    graph_opt_config.option = QNN_GRAPH_CONFIG_OPTION_CUSTOM;
    graph_opt_config.customConfig = &htp_graph_opt_config;
  }
}

Status QNNExecutionProvider::CompileFromOrtGraph(const std::vector<FusedNodeAndGraph>& fused_nodes_and_graphs,
                                                 std::vector<NodeComputeInfo>& node_compute_funcs,
                                                 const logging::Logger& logger) {
  for (const auto& fused_node_and_graph : fused_nodes_and_graphs) {
    Node& fused_node = fused_node_and_graph.fused_node;
    const onnxruntime::GraphViewer& graph_viewer(fused_node_and_graph.filtered_graph);

    std::unique_ptr<qnn::QnnModel> qnn_model = std::make_unique<qnn::QnnModel>(logger,
                                                                               qnn_backend_manager_.get());

    qnn::QnnGraphConfigsBuilder graph_configs_builder;
    InitQnnGraphConfigs(graph_configs_builder);

    ORT_RETURN_IF_ERROR(qnn_model->ComposeGraph(graph_viewer, fused_node, graph_configs_builder.GetQnnGraphConfigs()));
    ORT_RETURN_IF_ERROR(qnn_model->FinalizeGraphs());
    ORT_RETURN_IF_ERROR(qnn_model->SetupQnnInputOutput());

    LOGS(logger, VERBOSE) << "fused node name: " << fused_node.Name();
    qnn_models_.emplace(fused_node.Name(), std::move(qnn_model));

    ORT_RETURN_IF_ERROR(CreateComputeFunc(node_compute_funcs, logger));
  }
  return Status::OK();
}

Status QNNExecutionProvider::Compile(const std::vector<FusedNodeAndGraph>& fused_nodes_and_graphs,
                                     std::vector<NodeComputeInfo>& node_compute_funcs) {
  const auto& logger = *GetLogger();
  Node& fused_node = fused_nodes_and_graphs[0].fused_node;
  const onnxruntime::GraphViewer& graph_viewer(fused_nodes_and_graphs[0].filtered_graph);

  bool is_qnn_ctx_model = false;
  ORT_RETURN_IF_ERROR(qnn::IsFusedGraphHasCtxNode(fused_nodes_and_graphs, is_qnn_ctx_model));

  bool is_ctx_file_exist = qnn_cache_model_handler_->GetIsContextCacheFileExists();
  if (is_qnn_ctx_model || (context_cache_enabled_ && is_ctx_file_exist)) {
    ORT_RETURN_IF(fused_nodes_and_graphs.size() != 1, "Only support single partition for context cache feature.");
    std::unique_ptr<qnn::QnnModel> qnn_model = std::make_unique<qnn::QnnModel>(logger, qnn_backend_manager_.get());
    // Load and execute from cached context if exist
    ORT_RETURN_IF_ERROR(qnn_cache_model_handler_->LoadQnnCtxFromOnnxModel(graph_viewer,
                                                                          context_cache_path_,
                                                                          is_qnn_ctx_model,
                                                                          is_ctx_file_exist,
                                                                          qnn_backend_manager_.get(),
                                                                          *(qnn_model.get()),
                                                                          logger));
    ORT_RETURN_IF_ERROR(qnn_model->SetGraphInputOutputInfo(graph_viewer, fused_node));
    ORT_RETURN_IF_ERROR(qnn_model->SetupQnnInputOutput());

    // fused node name is QNNExecutionProvider_QNN_[hash_id]_[id]
    // the name here should be same with context->node_name in compute_info
    LOGS(logger, VERBOSE) << "fused node name: " << fused_node.Name();
    qnn_models_.emplace(fused_node.Name(), std::move(qnn_model));

    ORT_RETURN_IF_ERROR(CreateComputeFunc(node_compute_funcs, logger));
    return Status::OK();
  }

  ORT_RETURN_IF_ERROR(CompileFromOrtGraph(fused_nodes_and_graphs, node_compute_funcs, logger));
  if (context_cache_enabled_ && !is_qnn_ctx_model) {
    ORT_RETURN_IF(fused_nodes_and_graphs.size() != 1, "Only support single partition for context cache feature.");
    uint64_t buffer_size(0);
    auto context_buffer = qnn_backend_manager_->GetContextBinaryBuffer(buffer_size);
    ORT_RETURN_IF_ERROR(qnn_cache_model_handler_->GenerateCtxCacheOnnxModel(context_buffer.get(),
                                                                            buffer_size,
                                                                            qnn_backend_manager_->GetSdkVersion(),
                                                                            fused_nodes_and_graphs,
                                                                            qnn_models_,
                                                                            logger));
  }
  return Status::OK();
}
}  // namespace onnxruntime
