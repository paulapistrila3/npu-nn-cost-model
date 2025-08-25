// Copyright © 2025 Intel Corporation
// SPDX-License-Identifier: Apache 2.0
// LEGAL NOTICE: Your use of this software and any required dependent software (the “Software Package”)
// is subject to the terms and conditions of the software license agreements for the Software Package,
// which may also include notices, disclaimers, or license terms for third party or open source software
// included in or with the Software Package, and your use indicates your acceptance of all such terms.
// Please refer to the “third-party-programs.txt” or other similarly-named text file included with the
// Software Package for additional details.

#ifndef NN_COST_PROVIDER_H_
#define NN_COST_PROVIDER_H_

#include "inference/post_process.h"
#include "inference/postprocessing_factory.h"
#include "inference/preprocessing.h"
#include "inference/preprop_factory.h"
#include "inference/vpunn_runtime.h"
#include "vpu/cycles_interface_types.h"
#include "vpu/validation/dpu_operations_sanitizer.h"

#include "core/cache.h"

#include <thread>
#include <unordered_map>

#include <shared_mutex>

#include "vpu/nn_cost_provider_execution_context.h"

namespace VPUNN {

class NNCostProvider {
public:
    NNCostProvider(const DPU_OperationSanitizer& sanitizer, const std::string& filename = "",
                   const unsigned int batch_size = 1, bool profile = false, const unsigned int cache_size = 16384,
                   const std::string& dpu_cache_filename = "", bool tryToLoadPairedCache = false)
            : vpunn_runtime(filename, profile),
              preprocessing_factory{},
              postprocessing_factory{},
              preprocessing(init_preproc(preprocessing_factory, vpunn_runtime.model_version_info(), filename)),
              results_config(vpunn_runtime.model_version_info().get_output_interface_version()),
              post_processing(init_postproc(postprocessing_factory, vpunn_runtime.model_version_info(), filename)),
              cache(cache_size /*, preprocessing.output_size()*/, dpu_cache_filename,
                    (tryToLoadPairedCache ? filename : "")),
              cache_miss_serializer(get_env_vars({"ENABLE_VPUNN_CACHE_MISS_DATA_SERIALIZATION"})
                                            .at("ENABLE_VPUNN_CACHE_MISS_DATA_SERIALIZATION") == "TRUE"),
              sanitizer(sanitizer),
              batch_size(batch_size) {
        if (!is_initialized()) {
            return;
        }

        check_post_config(vpunn_runtime.model_version_info());
        correlate_preprocessor_with_model_inputs();
        cache_miss_serializer.initialize("cache_misses", FileMode::READ_WRITE, get_names_for_serializer());
    };

    NNCostProvider(const char* model_data, size_t model_data_length, const unsigned int batch_size,
                   bool copy_model_data, const DPU_OperationSanitizer& sanitizer, bool profile = false,
                   const unsigned int cache_size = 16384, const char* dpu_cache_data = nullptr,
                   size_t dpu_cache_data_length = 0)
            : vpunn_runtime(model_data, model_data_length, copy_model_data, profile),
              preprocessing_factory{},
              postprocessing_factory{},
              preprocessing(init_preproc(preprocessing_factory, vpunn_runtime.model_version_info(), "")),
              results_config(vpunn_runtime.model_version_info().get_output_interface_version()),
              post_processing(init_postproc(postprocessing_factory, vpunn_runtime.model_version_info(), "")),
              cache(cache_size, /* preprocessing.output_size(),*/ dpu_cache_data, dpu_cache_data_length),
              cache_miss_serializer(get_env_vars({"ENABLE_VPUNN_CACHE_MISS_DATA_SERIALIZATION"})
                                            .at("ENABLE_VPUNN_CACHE_MISS_DATA_SERIALIZATION") == "TRUE"),
              sanitizer(sanitizer),
              batch_size(batch_size) {
        if (!is_initialized()) {
            return;
        }

        check_post_config(vpunn_runtime.model_version_info());
        correlate_preprocessor_with_model_inputs();
        cache_miss_serializer.initialize("cache_misses", FileMode::READ_WRITE, get_names_for_serializer());
    };

    bool is_initialized() const {
        return vpunn_runtime.initialized();
    }

    const AccessCounter& getPreloadedCacheCounter() const {
        return cache.getPreloadedCacheCounter();
    }

    /// @brief provides the nickname of the model, used for cache and serializer
    /// @returns the nickname of the model
    std::string get_model_nickname() const noexcept {
        return model_nickname;
    }

protected:
    template <typename WlT>
    float infer(const WlT& workload) {
        auto& ctx = get_execution_context();

        float postProcessed_value{default_NN_output};

        if (!is_initialized()) {
            return default_NN_output;
        }

        const std::vector<float> descriptor{preprocessing.transformSingle(workload)};

        // Check for cache hit
        const auto cached_value = cache.get(descriptor);

        if (!cached_value) {
            const auto infered_value{vpunn_runtime.predict<float>(descriptor, ctx.runtime_buffer_data)[0]};
            cache.add(descriptor, infered_value);

            if (cache_miss_serializer.is_serialization_enabled()) {
                try {
                    auto wl_op = DPUOperation(workload, sanitizer.getDeviceConfiguration(workload.device));
                    const size_t wl_uid = wl_op.hash();
                    cache_miss_serializer.serialize(
                            wl_op, SerializableField<std::string>{"workload_uid", std::to_string(wl_uid)},
                            SerializableField<std::string>{"info", workload.get_layer_info()});
                    cache_miss_serializer.end();
                } catch (const std::exception& e) {
                    Logger::warning() << "Encountered invalid workload while serialization: " << e.what() << "\n";
                    cache_miss_serializer.clean_buffers();
                }
            }

            postProcessed_value = post_processing.process(workload, infered_value);
        } else {
            postProcessed_value = post_processing.process(workload, *cached_value);
        }

        return postProcessed_value;
    }

    /// returns a reference that is owned by the executor context, normally thread bounded
    template <typename WlT>
    const std::vector<float>& infer(const std::vector<WlT>& workloads) {
        auto& ctx = get_execution_context();

        ctx.workloads_results_buffer.resize(workloads.size());

        if (!is_initialized()) {
            std::fill(ctx.workloads_results_buffer.begin(), ctx.workloads_results_buffer.end(), default_NN_output);
            return ctx.workloads_results_buffer;
        }

        // Pre-process the workloads to generate descriptors
        // This is set up at ctor
        const auto model_batch_size{
                (ctx.runtime_buffer_data
                         .input_shapes()[0])[0]};  // how many wlds in a batch, this was established at the
                                                   // beginning. and it is obtained from the execution buffer!
        // transforms all at once, potential optimization is to do batch by batch
        const std::vector<float> descriptor_for_all = preprocessing.transformBatch(workloads, model_batch_size);

        const auto descriptor_size{preprocessing.output_size()};
        const auto inputs_to_process_in_batch{descriptor_size * model_batch_size};

        for (unsigned int wl_idx = 0; wl_idx < workloads.size(); wl_idx += model_batch_size) {
            // Slice the workload descriptors and predict on a single batch
            // pointer inside of the passed runtime_buffer_data
            const float* hw_overhead_arr = vpunn_runtime.predict(
                    &(descriptor_for_all[static_cast<size_t>(wl_idx) * static_cast<size_t>(descriptor_size)]),
                    inputs_to_process_in_batch, ctx.runtime_buffer_data);

            const auto complete_batch_end_idx{wl_idx + model_batch_size};
            auto end_idx{(complete_batch_end_idx > workloads.size()) ? workloads.size() : complete_batch_end_idx};

            // fill into result the data for this batch
            for (unsigned int idx = wl_idx; idx < end_idx; ++idx) {
                ctx.workloads_results_buffer[idx] = hw_overhead_arr[idx - wl_idx];
            }
        }
        //\todo: optimization (skip this if no processing required?)
        // post process the value : adapt it to the device and context.
        std::transform(workloads.cbegin(), workloads.cend(), ctx.workloads_results_buffer.cbegin(),
                       ctx.workloads_results_buffer.begin(), [this](const DPUWorkload& wl, const float nn_wl) {
                           return this->post_processing.process(wl, nn_wl);
                       });

        return ctx.workloads_results_buffer;
    }

    NNExecutionContext& get_execution_context() const {
        const auto thread_id = std::this_thread::get_id();

        // First, try to find the context with a shared (read) lock
        {
            std::shared_lock<std::shared_mutex> read_lock(context_map_mutex);
            auto it = context_map.find(thread_id);
            if (it != context_map.end()) {
                return *(it->second);
            }
        }

        // If not found, acquire an exclusive (write) lock to insert
        {
            std::unique_lock<std::shared_mutex> write_lock(context_map_mutex);
            // Double-check in case another thread inserted in the meantime
            auto it = context_map.find(thread_id);
            if (it == context_map.end()) {
                it = context_map
                             .emplace(thread_id,  // key
                                      std::make_shared<NNExecutionContext>(
                                              vpunn_runtime.createNewInferenceExecutionData(batch_size))  // context
                                      )
                             .first;
            }
            return *(it->second);
        }
    }

public:
    template <typename WlT>
    CyclesInterfaceType get_cost(const WlT& workload) {
        if (!is_initialized()) {
            return Cycles::ERROR_INFERENCE_NOT_POSSIBLE;
        }
        const float infered_value{infer(workload)};

        if (post_processing.is_NN_value_invalid(infered_value)) {
            return Cycles::ERROR_INVALID_OUTPUT_RANGE;
        } else {
            return static_cast<CyclesInterfaceType>(std::ceil(infered_value));
        }
    }

    std::vector<CyclesInterfaceType> get_cost(const std::vector<DPUWorkload>& workloads) {
        const auto number_of_workloads{workloads.size()};  ///< fixed value remembered here, workloads is non const
        std::vector<CyclesInterfaceType> cycles_vector(number_of_workloads);

        const std::vector<float>& NN_results{infer(workloads)};  // reference inside of context

        for (unsigned int idx = 0; idx < workloads.size(); ++idx) {
            const auto nn_output_cycles = NN_results[idx];
            if (post_processing.is_NN_value_invalid(nn_output_cycles)) {
                cycles_vector[idx] = Cycles::ERROR_INVALID_OUTPUT_RANGE;
            } else {
                cycles_vector[idx] = static_cast<CyclesInterfaceType>(std::ceil(nn_output_cycles));
            }
        }

        return cycles_vector;  // RVO
    }

    /// @brief Only used as a WA to share fixed cache outside of nn_cost_provider - will be removed in future.
    CyclesInterfaceType get_cached(const DPUWorkload& workload) {
        if (!is_initialized()) {
            return Cycles::ERROR_CACHE_MISS;
        }

        const std::vector<float> vector = preprocessing.transformSingle(workload);
        const auto cached_value = cache.get(vector);
        if (!cached_value) {
            return Cycles::ERROR_CACHE_MISS;
        } else {
            auto postProcessed_value = post_processing.process(workload, *cached_value);
            if (post_processing.is_NN_value_invalid(postProcessed_value)) {
                return Cycles::ERROR_INVALID_OUTPUT_RANGE;
            } else {
                return static_cast<CyclesInterfaceType>(std::ceil(postProcessed_value));
            }
        }
    }

    void add_to_cache(const DPUWorkload& workload, const float value) {
        if (!is_initialized()) {
            return;
        }

        const std::vector<float> vector = preprocessing.transformSingle(workload);
        cache.add(vector, value);
    }

    /// @brief provides the input and output versions of the loaded NN (debug purposes)
    std::tuple<int, int> getNNVersion() const {
        const auto& version{vpunn_runtime.model_version_info()};
        return std::make_tuple(version.get_input_interface_version(), version.get_output_interface_version());
    }

    std::string get_DPU_nickname() const noexcept {
        return model_nickname;
    }

    /// @brief Provides identifiers for data to be serialized
    // template <bool B = serialization_enabled, typename std::enable_if<B, int>::type = 0>
    static const std::vector<std::string> get_names_for_serializer() {
        auto fields = std::vector<std::string>(DPUOperation::_get_member_names().cbegin(),
                                               DPUOperation::_get_member_names().cend());
        fields.emplace_back("vpunn_cycles");
        fields.emplace_back("cost_source");
        fields.emplace_back("error_info");
        fields.emplace_back("info");
        fields.emplace_back("workload_uid");

        return fields;
    }

    /// @brief provides the value interval where the NN raw outputs are considered valid and will be used to further
    /// compute information
    ///
    /// @returns a pair containing (minimum_valid_value maximum_valid_value)
    std::pair<float, float> get_NN_Valid_interval() const noexcept {
        return post_processing.get_NN_Valid_interval();
    }

private:
    const Runtime vpunn_runtime;  ///< the loaded inference model is here, used for FW propagation
    const RuntimeProcessingFactory preprocessing_factory;
    const PostProcessingFactory postprocessing_factory;
    const Preprocessing<float>& preprocessing;  ///< prepares the input vector for the runtime, configured at ctor
    const PostProcessSupport results_config;
    const IPostProcess& post_processing;
    LRUCache<std::vector<float>, float>
            cache;  ///< cache for inferred values, used only for single workload, not for batches
    mutable CSVSerializer cache_miss_serializer;            ///< serializer for missed cache
    const std::string model_nickname{make_DPU_nickname()};  ///< nickname for the model, used for cache and serializer
    const DPU_OperationSanitizer& sanitizer;  ///< sanitizer for the workload, used to check and sanitize the workload
    const float default_NN_output{-1.0F};     ///< this is the value used in no NN output is present (like not loaded).
    const unsigned int batch_size{1};         ///< the batch size used for the inference, set at ctor, used for context

    // Map of (thread ID, instance ID) to execution contexts
    mutable std::map<std::thread::id, std::shared_ptr<NNExecutionContext>> context_map;
    mutable std::shared_mutex context_map_mutex;  ///< Mutex to protect the context map
private:
    /// @brief obtains the actual preprocessing instance from factory. The factory must live longer than the instance
    /// created. warning: Throws if not possible
    static Preprocessing<float>& init_preproc(const RuntimeProcessingFactory& factory,
                                              const ModelVersion& version_service, std::string_view filename) {
        // let's initialize the preproc aspects based on input version
        const int input_version = version_service.get_input_interface_version();
        //  checking if either we have some preprocess or we have a unsupported version
        if (factory.exists_preprocessing(input_version)) {
            auto& preproc = factory.make_preprocessing(input_version);
            return preproc;
        } else {
            std::stringstream buffer;
            buffer << "Cannot create preprocessing stage!.Preprocessing with version (" << input_version
                   << ") is not known/supported. Filename: " << filename
                   << " , Version info (raw): " << version_service.get_raw_name();
            std::string details = buffer.str();
            Logger::error() << details;

            throw std::runtime_error(details);
        }
    }

    static const IPostProcess& init_postproc(const PostProcessingFactory& factory, const ModelVersion& version_service,
                                             std::string_view filename) {
        const int version = version_service.get_output_interface_version();
        //  checking if either we have some process or we have a unsupported version
        if (factory.exists(version)) {
            auto& proc = factory.make(version);
            return proc;
        } else {
            std::stringstream buffer;
            buffer << "Cannot create post processing stage!.Post processing with version (" << version
                   << ") is not known/supported. Filename: " << filename
                   << " , Version info (raw): " << version_service.get_raw_name();
            std::string details = buffer.str();
            Logger::error() << details;

            throw std::runtime_error(details);
        }
    }

    /**
     * @brief check post config will check if we either have an empty model, or the output version is supported
     *
     * If we have an empty ideal model or we got the output version supported this function will be exited with
     * no errors. In case that we got a model with an unsupported output version this function will throw a runtime
     * error specifying the output version and the full raw name of it.
     *
     * @throws std::runtime_error In case that we got a model with an unsupported output version you will get a runtime
     * error
     *
     * @param v is the model version we took info about the full_raw_name in case we have an empty model
     */
    void check_post_config(const ModelVersion& v) {
        const auto raw_name_intf = v.get_raw_name();

        // in case we have an empty ideal model the raw_name is defaulted to none and we should continue the run
        if (raw_name_intf == "none") {
            return;
        }
        // in case we want a deprecated version with hw_overhead we will put config to unsupported
        // the NN model will have a unknown output and it should be thrown
        if (!results_config.is_output_supported()) {
            std::stringstream buffer;
            buffer << "Cannot load/handle Models output version. The output version: ("
                   << v.get_output_interface_version()
                   << ") is not known/supported. Version info (raw):" << v.get_raw_name();
            std::string details = buffer.str();
            Logger::error() << details;

            throw std::runtime_error(details);
        }
    }

    /// @brief  check and try to make the preprocessing output to be the same as what model expects
    /// This mechanism is unsafe at this moment and lets you change the preprocessing output to a bigger or smaller size
    /// The result may be impossibility to run (if smaller) or leaving empty zeros at the end (only partial fill)
    ///
    void correlate_preprocessor_with_model_inputs() {
        auto& ctx = get_execution_context();

        const auto model_input_size = (ctx.runtime_buffer_data.input_shapes()[0])[1];
        const auto preprocessing_output_size = preprocessing.output_size();
        if (model_input_size != preprocessing_output_size) {
            // if the preprocessing output size is not equal to the model input size, we throw an error
            std::stringstream buffer;
            buffer << "Cannot match preprocesing size with NN input expectations!\n.Preprocessing descriptor size = "
                   << preprocessing_output_size << ".  NN input buffer size: = " << model_input_size << ".";
            std::string details = buffer.str();
            Logger::error() << details;

            throw std::runtime_error(details);
        }
    }

    std::string make_DPU_nickname() const noexcept {
        std::string full{vpunn_runtime.model_version_info().get_raw_name()};
        const char delim{'$'};
        const auto first = full.find_first_of(delim);
        const auto last = full.find_last_of(delim);
        if (first == std::string::npos || last == std::string::npos) {
            return full;
        }
        auto nick = full.substr(first + 1, last - first - 1);

        std::replace(nick.begin(), nick.end(), ' ', '_');

        return "sim_" + nick;
    }
};

}  // namespace VPUNN

#endif
