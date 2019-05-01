/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <android-base/properties.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <string>

#include "TestNeuralNetworksWrapper.h"
#include "fuzzing/OperationManager.h"
#include "fuzzing/RandomGraphGenerator.h"
#include "fuzzing/RandomGraphGeneratorUtils.h"

namespace android {
namespace nn {
namespace fuzzing_test {

using test_wrapper::Result;
constexpr char kRefDeviceName[] = "nnapi-reference";

// Manages compilation on one single device.
class CompilationForDevice : public test_wrapper::Compilation {
   public:
    CompilationForDevice() = default;
    CompilationForDevice(const CompilationForDevice&) = delete;
    CompilationForDevice& operator=(const CompilationForDevice&) = delete;

    bool initialize(const test_wrapper::Model* model, const ANeuralNetworksDevice* device) {
        int ret = ANeuralNetworksCompilation_createForDevices(model->getHandle(), &device, 1,
                                                              &mCompilation);
        return ret == ANEURALNETWORKS_NO_ERROR;
    }
};

// NN API fuzzer logging setting comes from system property debug.nn.fuzzer.log and
// debug.nn.fuzzer.dumpspec.
// * setprop debug.nn.fuzzer.log 1 : enable logging.
// * setprop debug.nn.fuzzer.log 0 : silence logging.
// * setprop debug.nn.fuzzer.dumpspec 1 : dump the randomly generated graph to a spec file.
// * setprop debug.nn.fuzzer.dumpspec 0 : do not dump the graph.
//
// Logs and spec files are dumped to /data/local/tmp/${testname}.{log,mod.py},
// e.g. for test case TestRandomGraph/RandomGraphTest/Large/0,
//      log : /data/local/tmp/TestRandomGraph_RandomGraphTest_Large_0.log
//      spec: /data/local/tmp/TestRandomGraph_RandomGraphTest_Large_0.mod.py
//
class RandomGraphTest : public ::testing::TestWithParam<uint32_t> {
   public:
    static void SetUpTestCase() {
        mEnableLog = ::android::base::GetProperty("debug.nn.fuzzer.log", "") == "1";
        mDumpSpec = ::android::base::GetProperty("debug.nn.fuzzer.dumpspec", "") == "1";

        // Get all the devices and device names.
        uint32_t numDevices = 0;
        ASSERT_EQ(ANeuralNetworks_getDeviceCount(&numDevices), ANEURALNETWORKS_NO_ERROR);
        for (uint32_t i = 0; i < numDevices; i++) {
            ANeuralNetworksDevice* device = nullptr;
            const char* name = nullptr;
            ASSERT_EQ(ANeuralNetworks_getDevice(i, &device), ANEURALNETWORKS_NO_ERROR);
            ASSERT_EQ(ANeuralNetworksDevice_getName(device, &name), ANEURALNETWORKS_NO_ERROR);
            mDevices.emplace(name, device);
        }
    }

   protected:
    virtual void SetUp() override {
        // Initialize logging.
        const ::testing::TestInfo* const testInfo =
                ::testing::UnitTest::GetInstance()->current_test_info();
        mTestName = mTestName + testInfo->test_case_name() + "_" + testInfo->name();
        std::replace(mTestName.begin(), mTestName.end(), '/', '_');
        if (mEnableLog) NN_FUZZER_LOG_INIT("/data/local/tmp/" + mTestName + ".log");
    }

    virtual void TearDown() override {
        if (::testing::Test::HasFailure() || mDumpSpec) {
            mGraph.dumpSpecFile("/data/local/tmp/" + mTestName + ".mod.py", mTestName);
        }
        NN_FUZZER_LOG_CLOSE;
    }

    // Compile and execute the generated graph on a device selected by name.
    void compute(const test_wrapper::Model* model, uint32_t numOps, const std::string& name) {
        SCOPED_TRACE("Device: " + name);
        ASSERT_TRUE(mDevices.find(name) != mDevices.end());
        const auto device = mDevices[name];
        bool isRef = name.compare(kRefDeviceName) == 0;

        // Check if the device fully supports the graph.
        constexpr int kMaxNumberOperations = 1000;
        ASSERT_TRUE(numOps <= kMaxNumberOperations);
        bool supported[kMaxNumberOperations] = {false};
        ASSERT_EQ(ANeuralNetworksModel_getSupportedOperationsForDevices(model->getHandle(), &device,
                                                                        1, supported),
                  ANEURALNETWORKS_NO_ERROR);
        if (!std::all_of(supported, supported + numOps, [](bool v) { return v; })) {
            // The reference device should always support all operations.
            ASSERT_FALSE(isRef);
            std::cout << "[          ]   SKIP: " << name << " does not support the graph.\n";
            return;
        }

        // Since this test is introduced in Android Q, we only assert no compilation or execution
        // failure if the device has feature level >= Q (API level 29). For pre-Q devices, we allow
        // them to fail with OP_FAILED, but must not hang or crash.
        int64_t featureLevel;
        ASSERT_EQ(ANeuralNetworksDevice_getFeatureLevel(device, &featureLevel),
                  ANEURALNETWORKS_NO_ERROR);

        // Create compilation for device.
        CompilationForDevice compilation;
        ASSERT_TRUE(compilation.initialize(model, device));
        Result compileReturn = compilation.finish();
        if (featureLevel >= __ANDROID_API_Q__) {
            ASSERT_EQ(compileReturn, Result::NO_ERROR);
        } else {
            ASSERT_TRUE(compileReturn == Result::NO_ERROR || compileReturn == Result::OP_FAILED);
        }

        // Create request.
        test_wrapper::Execution execution(&compilation);
        std::vector<OperandBuffer> outputs;
        if (isRef) {
            mGraph.createRequest(&execution);
        } else {
            mGraph.createRequest(&execution, &outputs);
        }

        // Compute result.
        Result executeReturn = execution.compute();
        if (featureLevel >= __ANDROID_API_Q__) {
            ASSERT_EQ(executeReturn, Result::NO_ERROR);
            if (!isRef) mGraph.checkResults(outputs, mCriteria);
        } else {
            ASSERT_TRUE(executeReturn == Result::NO_ERROR || executeReturn == Result::OP_FAILED);
        }
    }

    // Main test entrance.
    void testRandomGraph(uint32_t numOperations, uint32_t dimensionRange) {
        // Generate a random graph.
        ASSERT_TRUE(mGraph.generate(kSeed, numOperations, dimensionRange));

        // Create a model from the random graph.
        test_wrapper::Model model;
        mGraph.createModel(&model);
        ASSERT_TRUE(model.isValid());
        ASSERT_EQ(model.finish(), Result::NO_ERROR);

        // Compute reference result.
        compute(&model, numOperations, kRefDeviceName);

        for (auto& pair : mDevices) {
            // Skip the nnapi reference device.
            if (pair.first.compare(kRefDeviceName) == 0) continue;
            compute(&model, numOperations, pair.first);
        }
    }

    enum GraphSize : uint32_t { SINGLE = 1, SMALL = 5, LARGE = 40 };
    enum DimensionRange : uint32_t { NARROW = 10, WIDE = 1000 };

    static bool mEnableLog;
    static bool mDumpSpec;
    static std::map<std::string, ANeuralNetworksDevice*> mDevices;

    const uint32_t kSeed = GetParam();
    std::string mTestName;
    RandomGraph mGraph;
    AccuracyCriteria mCriteria;
};

bool RandomGraphTest::mEnableLog = false;
bool RandomGraphTest::mDumpSpec = false;
std::map<std::string, ANeuralNetworksDevice*> RandomGraphTest::mDevices;

// Single-op graph with dimensions in range [1, 1000].
class SingleOperationTest : public RandomGraphTest {};
#define TEST_SINGLE_OPERATION(operation, halVersion, criteria)              \
    TEST_P(SingleOperationTest, operation##_##halVersion) {                 \
        OperationFilter filter = {.opcodes = {ANEURALNETWORKS_##operation}, \
                                  .versions = {HalVersion::halVersion}};    \
        OperationManager::get()->applyFilter(filter);                       \
        mCriteria = (criteria);                                             \
        testRandomGraph(GraphSize::SINGLE, DimensionRange::WIDE);           \
    }

// TODO: Adjust the accuracy criteria based on testing.
// We define three sets of accuracy criteria for single-operation tests.

// This is for operations that only copy buffers around without any computation on buffer values.
// Most of these operations fall into categories of reshape or selection, e.g. RESHAPE, GATHER.
// Additionally, operations with only logical or comparison arithmetic also use this criteria, e.g.
// EQUAL, ARGMAX, TOPK_V2.
const AccuracyCriteria kStrictCriteria = {
        .float32 = {.atol = 1e-6f, .rtol = 1e-6f, .bias = 1e-7f, .mse = 1e-10f},
        .float16 = {.atol = 1e-3f, .rtol = 1e-3f, .bias = 1e-4f, .mse = 1e-8f},
        .int32 = {.atol = 1},
        .quant8Asymm = {.atol = 1, .bias = 0.1f, .mse = 0.1f},
        .quant8Symm = {.atol = 1, .bias = 0.1f, .mse = 0.1f},
        .quant16Asymm = {.atol = 1, .bias = 0.1f, .mse = 0.1f},
        .quant16Symm = {.atol = 1, .bias = 0.1f, .mse = 0.1f}};

// This is for operations that only do simple and single computation on buffer values, such as
// addition, multiplication, or requantization. Most of these operations fall into categories of
// broadcast or elementwise, e.g ADD, FLOOR.
const AccuracyCriteria kMediumCriteria = {
        .float32 = {.atol = 1e-5f, .rtol = 1e-5f, .bias = 1e-6f, .mse = 1e-8f},
        .float16 = {.atol = 1e-2f, .rtol = 1e-2f, .bias = 1e-3f, .mse = 1e-6f},
        .int32 = {.atol = 1},
        .quant8Asymm = {.atol = 2, .bias = 0.5f, .mse = 0.5f},
        .quant8Symm = {.atol = 2, .bias = 0.5f, .mse = 0.5f},
        .quant16Asymm = {.atol = 2, .bias = 0.5f, .mse = 0.5f},
        .quant16Symm = {.atol = 2, .bias = 0.5f, .mse = 0.5f}};

// This is for operations that involve sophisticated computations on buffer values, either a single
// but complex transformation, e.g. LOGISTIC, or multiple transformations with accumulated errors,
// e.g. CONV_2D, REDUCE_*.
const AccuracyCriteria kRelaxedCriteria = {
        .float32 = {.atol = 1e-3f, .rtol = 1e-3f, .bias = 2e-5f, .mse = 1e-7f},
        .float16 = {.atol = 1.0f, .rtol = 1.0f, .bias = 5e-3f, .mse = 1e-4f},
        .int32 = {.atol = 1},
        .quant8Asymm = {.atol = 8, .bias = 1, .mse = 1},
        .quant8Symm = {.atol = 8, .bias = 1, .mse = 1},
        .quant16Asymm = {.atol = 8, .bias = 1, .mse = 1},
        .quant16Symm = {.atol = 8, .bias = 1, .mse = 1}};

/*-- NNAPI 1.0 Operations ---------------------------------------------------*/

// TODO: The following 1.0 operation signatures are currently not defined:
// - ANEURALNETWORKS_LSH_PROJECTION
// - ANEURALNETWORKS_LSTM
// - ANEURALNETWORKS_RNN
// - ANEURALNETWORKS_SVDF

TEST_SINGLE_OPERATION(ADD, V1_0, kMediumCriteria);
TEST_SINGLE_OPERATION(MUL, V1_0, kMediumCriteria);
TEST_SINGLE_OPERATION(FLOOR, V1_0, kMediumCriteria);
TEST_SINGLE_OPERATION(LOGISTIC, V1_0, kRelaxedCriteria);
TEST_SINGLE_OPERATION(RELU, V1_0, kMediumCriteria);
TEST_SINGLE_OPERATION(RELU1, V1_0, kMediumCriteria);
TEST_SINGLE_OPERATION(RELU6, V1_0, kMediumCriteria);
TEST_SINGLE_OPERATION(TANH, V1_0, kRelaxedCriteria);
TEST_SINGLE_OPERATION(SOFTMAX, V1_0, kRelaxedCriteria);
TEST_SINGLE_OPERATION(L2_NORMALIZATION, V1_0, kRelaxedCriteria);
TEST_SINGLE_OPERATION(LOCAL_RESPONSE_NORMALIZATION, V1_0, kRelaxedCriteria);
TEST_SINGLE_OPERATION(AVERAGE_POOL_2D, V1_0, kRelaxedCriteria);
TEST_SINGLE_OPERATION(L2_POOL_2D, V1_0, kRelaxedCriteria);
TEST_SINGLE_OPERATION(MAX_POOL_2D, V1_0, kRelaxedCriteria);
TEST_SINGLE_OPERATION(CONV_2D, V1_0, kRelaxedCriteria);
TEST_SINGLE_OPERATION(DEPTHWISE_CONV_2D, V1_0, kRelaxedCriteria);
TEST_SINGLE_OPERATION(CONCATENATION, V1_0, kMediumCriteria);
TEST_SINGLE_OPERATION(RESIZE_BILINEAR, V1_0, kRelaxedCriteria);
TEST_SINGLE_OPERATION(DEPTH_TO_SPACE, V1_0, kStrictCriteria);
TEST_SINGLE_OPERATION(SPACE_TO_DEPTH, V1_0, kStrictCriteria);
TEST_SINGLE_OPERATION(EMBEDDING_LOOKUP, V1_0, kStrictCriteria);
TEST_SINGLE_OPERATION(HASHTABLE_LOOKUP, V1_0, kStrictCriteria);
TEST_SINGLE_OPERATION(FULLY_CONNECTED, V1_0, kRelaxedCriteria);
TEST_SINGLE_OPERATION(RESHAPE, V1_0, kStrictCriteria);
TEST_SINGLE_OPERATION(DEQUANTIZE, V1_0, kMediumCriteria);

/*-- NNAPI 1.1 Operations ---------------------------------------------------*/

TEST_SINGLE_OPERATION(SUB, V1_1, kMediumCriteria);
TEST_SINGLE_OPERATION(DIV, V1_1, kRelaxedCriteria);
TEST_SINGLE_OPERATION(BATCH_TO_SPACE_ND, V1_1, kStrictCriteria);
TEST_SINGLE_OPERATION(SPACE_TO_BATCH_ND, V1_1, kStrictCriteria);
TEST_SINGLE_OPERATION(MEAN, V1_1, kRelaxedCriteria);
TEST_SINGLE_OPERATION(PAD, V1_1, kStrictCriteria);
TEST_SINGLE_OPERATION(TRANSPOSE, V1_1, kStrictCriteria);
TEST_SINGLE_OPERATION(SQUEEZE, V1_1, kStrictCriteria);
TEST_SINGLE_OPERATION(STRIDED_SLICE, V1_1, kStrictCriteria);

/*-- NNAPI 1.0 and 1.1 Operations with Extended Behavior in 1.2 -------------*/

TEST_SINGLE_OPERATION(ADD, V1_2, kMediumCriteria);
TEST_SINGLE_OPERATION(MUL, V1_2, kMediumCriteria);
TEST_SINGLE_OPERATION(SUB, V1_2, kMediumCriteria);
TEST_SINGLE_OPERATION(DIV, V1_2, kRelaxedCriteria);
TEST_SINGLE_OPERATION(FLOOR, V1_2, kMediumCriteria);
TEST_SINGLE_OPERATION(LOGISTIC, V1_2, kRelaxedCriteria);
TEST_SINGLE_OPERATION(RELU, V1_2, kMediumCriteria);
TEST_SINGLE_OPERATION(RELU1, V1_2, kMediumCriteria);
TEST_SINGLE_OPERATION(RELU6, V1_2, kMediumCriteria);
TEST_SINGLE_OPERATION(TANH, V1_2, kRelaxedCriteria);
TEST_SINGLE_OPERATION(CONCATENATION, V1_2, kMediumCriteria);
TEST_SINGLE_OPERATION(DEPTH_TO_SPACE, V1_2, kStrictCriteria);
TEST_SINGLE_OPERATION(SPACE_TO_DEPTH, V1_2, kStrictCriteria);
TEST_SINGLE_OPERATION(BATCH_TO_SPACE_ND, V1_2, kStrictCriteria);
TEST_SINGLE_OPERATION(SPACE_TO_BATCH_ND, V1_2, kStrictCriteria);
TEST_SINGLE_OPERATION(FULLY_CONNECTED, V1_2, kRelaxedCriteria);
TEST_SINGLE_OPERATION(RESHAPE, V1_2, kStrictCriteria);
TEST_SINGLE_OPERATION(MEAN, V1_2, kRelaxedCriteria);
TEST_SINGLE_OPERATION(PAD, V1_2, kStrictCriteria);
TEST_SINGLE_OPERATION(TRANSPOSE, V1_2, kStrictCriteria);
TEST_SINGLE_OPERATION(CONV_2D, V1_2, kRelaxedCriteria);
TEST_SINGLE_OPERATION(DEPTHWISE_CONV_2D, V1_2, kRelaxedCriteria);
TEST_SINGLE_OPERATION(AVERAGE_POOL_2D, V1_2, kRelaxedCriteria);
TEST_SINGLE_OPERATION(L2_POOL_2D, V1_2, kRelaxedCriteria);
TEST_SINGLE_OPERATION(MAX_POOL_2D, V1_2, kRelaxedCriteria);
TEST_SINGLE_OPERATION(RESIZE_BILINEAR, V1_2, kRelaxedCriteria);
TEST_SINGLE_OPERATION(SOFTMAX, V1_2, kRelaxedCriteria);
TEST_SINGLE_OPERATION(L2_NORMALIZATION, V1_2, kRelaxedCriteria);
TEST_SINGLE_OPERATION(LOCAL_RESPONSE_NORMALIZATION, V1_2, kRelaxedCriteria);
TEST_SINGLE_OPERATION(DEQUANTIZE, V1_2, kMediumCriteria);
TEST_SINGLE_OPERATION(SQUEEZE, V1_2, kStrictCriteria);
TEST_SINGLE_OPERATION(STRIDED_SLICE, V1_2, kStrictCriteria);

/*-- NNAPI 1.2 Operations ---------------------------------------------------*/

// TODO: The following 1.2 operation signatures are currently not defined:
// - ANEURALNETWORKS_AXIS_ALIGNED_BBOX_TRANSFORM
// - ANEURALNETWORKS_BIDIRECTIONAL_SEQUENCE_LSTM
// - ANEURALNETWORKS_BIDIRECTIONAL_SEQUENCE_RNN
// - ANEURALNETWORKS_BOX_WITH_NMS_LIMIT
// - ANEURALNETWORKS_DETECTION_POSTPROCESSING
// - ANEURALNETWORKS_GENERATE_PROPOSALS
// - ANEURALNETWORKS_QUANTIZED_16BIT_LSTM
// - ANEURALNETWORKS_RANDOM_MULTINOMIAL
// - ANEURALNETWORKS_UNIDIRECTIONAL_SEQUENCE_LSTM
// - ANEURALNETWORKS_UNIDIRECTIONAL_SEQUENCE_RNN

TEST_SINGLE_OPERATION(ABS, V1_2, kMediumCriteria);
TEST_SINGLE_OPERATION(EXP, V1_2, kRelaxedCriteria);
TEST_SINGLE_OPERATION(LOG, V1_2, kRelaxedCriteria);
TEST_SINGLE_OPERATION(NEG, V1_2, kMediumCriteria);
TEST_SINGLE_OPERATION(RSQRT, V1_2, kRelaxedCriteria);
TEST_SINGLE_OPERATION(SIN, V1_2, kRelaxedCriteria);
TEST_SINGLE_OPERATION(SQRT, V1_2, kRelaxedCriteria);
TEST_SINGLE_OPERATION(ARGMAX, V1_2, kStrictCriteria);
TEST_SINGLE_OPERATION(ARGMIN, V1_2, kStrictCriteria);
TEST_SINGLE_OPERATION(EQUAL, V1_2, kStrictCriteria);
TEST_SINGLE_OPERATION(GREATER, V1_2, kStrictCriteria);
TEST_SINGLE_OPERATION(GREATER_EQUAL, V1_2, kStrictCriteria);
TEST_SINGLE_OPERATION(LESS, V1_2, kStrictCriteria);
TEST_SINGLE_OPERATION(LESS_EQUAL, V1_2, kStrictCriteria);
TEST_SINGLE_OPERATION(LOGICAL_AND, V1_2, kStrictCriteria);
TEST_SINGLE_OPERATION(LOGICAL_NOT, V1_2, kStrictCriteria);
TEST_SINGLE_OPERATION(LOGICAL_OR, V1_2, kStrictCriteria);
TEST_SINGLE_OPERATION(NOT_EQUAL, V1_2, kStrictCriteria);
TEST_SINGLE_OPERATION(MAXIMUM, V1_2, kMediumCriteria);
TEST_SINGLE_OPERATION(MINIMUM, V1_2, kMediumCriteria);
TEST_SINGLE_OPERATION(POW, V1_2, kRelaxedCriteria);
TEST_SINGLE_OPERATION(PRELU, V1_2, kMediumCriteria);
TEST_SINGLE_OPERATION(REDUCE_ALL, V1_2, kRelaxedCriteria);
TEST_SINGLE_OPERATION(REDUCE_ANY, V1_2, kRelaxedCriteria);
TEST_SINGLE_OPERATION(REDUCE_MAX, V1_2, kRelaxedCriteria);
TEST_SINGLE_OPERATION(REDUCE_MIN, V1_2, kRelaxedCriteria);
TEST_SINGLE_OPERATION(REDUCE_PROD, V1_2, kRelaxedCriteria);
TEST_SINGLE_OPERATION(REDUCE_SUM, V1_2, kRelaxedCriteria);
TEST_SINGLE_OPERATION(CHANNEL_SHUFFLE, V1_2, kStrictCriteria);
TEST_SINGLE_OPERATION(INSTANCE_NORMALIZATION, V1_2, kRelaxedCriteria);
TEST_SINGLE_OPERATION(LOG_SOFTMAX, V1_2, kRelaxedCriteria);
TEST_SINGLE_OPERATION(GROUPED_CONV_2D, V1_2, kRelaxedCriteria);
TEST_SINGLE_OPERATION(TRANSPOSE_CONV_2D, V1_2, kRelaxedCriteria);
TEST_SINGLE_OPERATION(RESIZE_NEAREST_NEIGHBOR, V1_2, kRelaxedCriteria);
TEST_SINGLE_OPERATION(PAD_V2, V1_2, kStrictCriteria);
TEST_SINGLE_OPERATION(QUANTIZE, V1_2, kMediumCriteria);
TEST_SINGLE_OPERATION(CAST, V1_2, kMediumCriteria);
TEST_SINGLE_OPERATION(EXPAND_DIMS, V1_2, kStrictCriteria);
TEST_SINGLE_OPERATION(TILE, V1_2, kStrictCriteria);
TEST_SINGLE_OPERATION(GATHER, V1_2, kStrictCriteria);
TEST_SINGLE_OPERATION(SELECT, V1_2, kStrictCriteria);
TEST_SINGLE_OPERATION(TOPK_V2, V1_2, kStrictCriteria);
TEST_SINGLE_OPERATION(SLICE, V1_2, kStrictCriteria);
TEST_SINGLE_OPERATION(SPLIT, V1_2, kMediumCriteria);
TEST_SINGLE_OPERATION(ROI_ALIGN, V1_2, kRelaxedCriteria);
TEST_SINGLE_OPERATION(ROI_POOLING, V1_2, kRelaxedCriteria);
TEST_SINGLE_OPERATION(HEATMAP_MAX_KEYPOINT, V1_2, kRelaxedCriteria);

const AccuracyCriteria kSmallGraphCriteria = {
        .float32 = {.atol = 1e-2f, .rtol = 1e-2f, .bias = 2e-5f, .mse = 1e-7f},
        .float16 = {.atol = 1.0f, .rtol = 1.0f, .bias = 5e-3f, .mse = 1e-4f},
        .int32 = {.atol = 1},
        .quant8Asymm = {.atol = 8, .bias = 1, .mse = 1},
        .quant8Symm = {.atol = 8, .bias = 1, .mse = 1},
        .quant16Asymm = {.atol = 8, .bias = 1, .mse = 1},
        .quant16Symm = {.atol = 8, .bias = 1, .mse = 1}};

const AccuracyCriteria kLargeGraphCriteria = {
        .float32 = {.atol = 1e-1f, .rtol = 1e-1f, .bias = 1e-2f, .mse = 1e-4f},
        .float16 = {.atol = 1.0f, .rtol = 1.0f, .bias = 1e-1f, .mse = 5e-2f},
        .int32 = {.atol = 1},
        .quant8Asymm = {.atol = 10, .bias = 2, .mse = 2},
        .quant8Symm = {.atol = 10, .bias = 2, .mse = 2},
        .quant16Asymm = {.atol = 10, .bias = 2, .mse = 2},
        .quant16Symm = {.atol = 10, .bias = 2, .mse = 2}};

// Due to the limitation of the random graph generator, graphs generated with mixed-type or
// mixed-rank operations are likely to result in a disconnected network. Thus, we filter the
// operation signatures by primary data type and rank first, then generate random graph tests for
// each combination.
//
// Two parameterized tests are created for each filter:
// * 5-op graph with dimensions in range [1, 1000].
// * 40-op graph with dimensions in range [1, 10].
//
#define TEST_RANDOM_GRAPH_WITH_DATA_TYPE_AND_RANK(dataType, rank)                  \
    TEST_P(RandomGraphTest, SmallGraph_##dataType##_Rank##rank) {                  \
        OperationFilter filter = {.dataTypes = {Type::dataType}, .ranks = {rank}}; \
        OperationManager::get()->applyFilter(filter);                              \
        mCriteria = kSmallGraphCriteria;                                           \
        testRandomGraph(GraphSize::SMALL, DimensionRange::WIDE);                   \
    }                                                                              \
    TEST_P(RandomGraphTest, LargeGraph_##dataType##_Rank##rank) {                  \
        OperationFilter filter = {.dataTypes = {Type::dataType}, .ranks = {rank}}; \
        OperationManager::get()->applyFilter(filter);                              \
        mCriteria = kLargeGraphCriteria;                                           \
        testRandomGraph(GraphSize::LARGE, DimensionRange::NARROW);                 \
    }

// Random graph test with TENSOR_QUANT8_ASYMM as the primary data type is currently not defined.
// The generated graph with TENSOR_QUANT8_ASYMM as the primary data type will likely to result in
// disconnected graphs due to the mismatch between quantized parameters.

TEST_RANDOM_GRAPH_WITH_DATA_TYPE_AND_RANK(TENSOR_FLOAT32, 4);
TEST_RANDOM_GRAPH_WITH_DATA_TYPE_AND_RANK(TENSOR_FLOAT32, 3);
TEST_RANDOM_GRAPH_WITH_DATA_TYPE_AND_RANK(TENSOR_FLOAT32, 2);
TEST_RANDOM_GRAPH_WITH_DATA_TYPE_AND_RANK(TENSOR_FLOAT32, 1);

TEST_RANDOM_GRAPH_WITH_DATA_TYPE_AND_RANK(TENSOR_FLOAT16, 4);
TEST_RANDOM_GRAPH_WITH_DATA_TYPE_AND_RANK(TENSOR_FLOAT16, 3);
TEST_RANDOM_GRAPH_WITH_DATA_TYPE_AND_RANK(TENSOR_FLOAT16, 2);
TEST_RANDOM_GRAPH_WITH_DATA_TYPE_AND_RANK(TENSOR_FLOAT16, 1);

TEST_RANDOM_GRAPH_WITH_DATA_TYPE_AND_RANK(TENSOR_INT32, 4);
TEST_RANDOM_GRAPH_WITH_DATA_TYPE_AND_RANK(TENSOR_INT32, 3);
TEST_RANDOM_GRAPH_WITH_DATA_TYPE_AND_RANK(TENSOR_INT32, 2);
TEST_RANDOM_GRAPH_WITH_DATA_TYPE_AND_RANK(TENSOR_INT32, 1);

TEST_RANDOM_GRAPH_WITH_DATA_TYPE_AND_RANK(TENSOR_BOOL8, 4);
TEST_RANDOM_GRAPH_WITH_DATA_TYPE_AND_RANK(TENSOR_BOOL8, 3);
TEST_RANDOM_GRAPH_WITH_DATA_TYPE_AND_RANK(TENSOR_BOOL8, 2);
TEST_RANDOM_GRAPH_WITH_DATA_TYPE_AND_RANK(TENSOR_BOOL8, 1);

constexpr uint32_t kFirstSeed = 0;
constexpr uint32_t kNumTestCases = 100;

INSTANTIATE_TEST_CASE_P(TestRandomGraph, SingleOperationTest,
                        ::testing::Range(kFirstSeed, kFirstSeed + kNumTestCases));

INSTANTIATE_TEST_CASE_P(TestRandomGraph, RandomGraphTest,
                        ::testing::Range(kFirstSeed, kFirstSeed + kNumTestCases));

}  // namespace fuzzing_test
}  // namespace nn
}  // namespace android
