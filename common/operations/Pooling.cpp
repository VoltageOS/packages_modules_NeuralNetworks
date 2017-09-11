/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "Operations.h"
#include "OperationsUtils.h"

#include "internal/optimized/optimized_ops.h"

namespace android {
namespace nn {

bool genericPoolingPrepare(const Shape& input,
                           int32_t padding_left, int32_t padding_right,
                           int32_t padding_top, int32_t padding_bottom,
                           int32_t stride_width, int32_t stride_height,
                           int32_t filter_width, int32_t filter_height,
                           Shape* output) {
    DCHECK_EQ(getNumberOfDimensions(input), 4);
    DCHECK_EQ(stride_width, stride_height);

    uint32_t batches      = getSizeOfDimension(input, 0);
    uint32_t width        = getSizeOfDimension(input, 2);
    uint32_t height       = getSizeOfDimension(input, 1);
    uint32_t channels_out = getSizeOfDimension(input, 3);

    uint32_t outWidth = computeOutSize(width, filter_width, stride_width,
                                       padding_left, padding_right);
    uint32_t outHeight = computeOutSize(height, filter_height, stride_height,
                                        padding_top, padding_bottom);

    output->type = input.type;
    output->dimensions = {batches, outHeight, outWidth, channels_out};
    return true;
}


#define ANDROID_NN_POOLING_PARAMETERS                                           \
    uint32_t height       = getSizeOfDimension(inputShape, 1);                  \
    uint32_t width        = getSizeOfDimension(inputShape, 2);                  \
    uint32_t outHeight    = getSizeOfDimension(outputShape, 1);                 \
    uint32_t outWidth     = getSizeOfDimension(outputShape, 2);                 \
                                                                                \
    uint32_t paddingHeight = (uint32_t)padding_top;                             \
    uint32_t paddingWidth = (uint32_t)padding_left;

bool averagePoolFloat32(const float* inputData, const Shape& inputShape,
                        int32_t padding_left, int32_t padding_right,
                        int32_t padding_top, int32_t padding_bottom,
                        int32_t stride_width, int32_t stride_height,
                        int32_t filter_width, int32_t filter_height, int32_t activation,
                        float* outputData, const Shape& outputShape) {

    ANDROID_NN_POOLING_PARAMETERS

    #define ANDROID_NN_AVERAGE_POOL(activation)                                \
        optimized_ops::AveragePool<FusedActivationFunctionType::activation>(   \
            inputData, convertShapeToDims(inputShape),                         \
            stride_width, paddingWidth, paddingHeight,                         \
            filter_width, filter_height,                                       \
            outputData, convertShapeToDims(outputShape))

    ANDROID_NN_MACRO_DISPATCH(ANDROID_NN_AVERAGE_POOL)
    #undef ANDROID_NN_AVERAGE_POOL

    return true;
}

bool averagePoolQuant8(const uint8_t* inputData, const Shape& inputShape,
                       int32_t padding_left, int32_t padding_right,
                       int32_t padding_top, int32_t padding_bottom,
                       int32_t stride_width, int32_t stride_height,
                       int32_t filter_width, int32_t filter_height, int32_t activation,
                       uint8_t* outputData, const Shape& outputShape) {

    ANDROID_NN_POOLING_PARAMETERS

    int32_t output_activation_min = 0;
    int32_t output_activation_max = 0;

    CalculateActivationRangeUint8(activation, outputShape,
                                  &output_activation_min,
                                  &output_activation_max);

    #define ANDROID_NN_AVERAGE_POOL(activation)                                \
        optimized_ops::AveragePool<FusedActivationFunctionType::activation>(   \
            inputData, convertShapeToDims(inputShape),                         \
            stride_width, paddingWidth, paddingHeight,                         \
            filter_width, filter_height,                                       \
            output_activation_min, output_activation_max,                      \
            outputData, convertShapeToDims(outputShape))

    ANDROID_NN_MACRO_DISPATCH(ANDROID_NN_AVERAGE_POOL)
    #undef ANDROID_NN_AVERAGE_POOL

    return true;
}

bool l2PoolFloat32(const float* inputData, const Shape& inputShape,
                   int32_t padding_left, int32_t padding_right,
                   int32_t padding_top, int32_t padding_bottom,
                   int32_t stride_width, int32_t stride_height,
                   int32_t filter_width, int32_t filter_height, int32_t activation,
                   float* outputData, const Shape& outputShape) {

    ANDROID_NN_POOLING_PARAMETERS

    #define ANDROID_NN_L2_POOL(activation)                                     \
        optimized_ops::L2Pool<FusedActivationFunctionType::activation>(        \
            inputData, convertShapeToDims(inputShape),                         \
            stride_width, paddingWidth, paddingHeight,                         \
            filter_width, filter_height,                                       \
            outputData, convertShapeToDims(outputShape))

    ANDROID_NN_MACRO_DISPATCH(ANDROID_NN_L2_POOL)
    #undef ANDROID_NN_L2_POOL

    return true;
}

bool maxPoolFloat32(const float* inputData, const Shape& inputShape,
                    int32_t padding_left, int32_t padding_right,
                    int32_t padding_top, int32_t padding_bottom,
                    int32_t stride_width, int32_t stride_height,
                    int32_t filter_width, int32_t filter_height, int32_t activation,
                    float* outputData, const Shape& outputShape) {

    ANDROID_NN_POOLING_PARAMETERS

    #define ANDROID_NN_MAX_POOL(activation)                                    \
        optimized_ops::MaxPool<FusedActivationFunctionType::activation>(       \
            inputData, convertShapeToDims(inputShape),                         \
            stride_width, paddingWidth, paddingHeight,                         \
            filter_width, filter_height,                                       \
            outputData, convertShapeToDims(outputShape))

    ANDROID_NN_MACRO_DISPATCH(ANDROID_NN_MAX_POOL)
    #undef ANDROID_NN_MAX_POOL

    return true;
}

bool maxPoolQuant8(const uint8_t* inputData, const Shape& inputShape,
                   int32_t padding_left, int32_t padding_right,
                   int32_t padding_top, int32_t padding_bottom,
                   int32_t stride_width, int32_t stride_height,
                   int32_t filter_width, int32_t filter_height, int32_t activation,
                   uint8_t* outputData, const Shape& outputShape) {

    ANDROID_NN_POOLING_PARAMETERS

    int32_t output_activation_min = 0;
    int32_t output_activation_max = 0;

    CalculateActivationRangeUint8(activation, outputShape,
                                  &output_activation_min,
                                  &output_activation_max);

    #define ANDROID_NN_MAX_POOL(activation)                                    \
        optimized_ops::MaxPool<FusedActivationFunctionType::activation>(       \
            inputData, convertShapeToDims(inputShape),                         \
            stride_width, paddingWidth, paddingHeight,                         \
            filter_width, filter_height,                                       \
            output_activation_min, output_activation_max,                      \
            outputData, convertShapeToDims(outputShape))

    ANDROID_NN_MACRO_DISPATCH(ANDROID_NN_MAX_POOL)
    #undef ANDROID_NN_MAX_POOL

    return true;
}

#undef ANDROID_NN_POOLING_PARAMETERS
}  // namespace nn
}  // namespace android
