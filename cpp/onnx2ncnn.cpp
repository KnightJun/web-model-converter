// Tencent is pleased to support the open source community by making ncnn available.
//
// Copyright (C) 2017 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include "onnx2ncnn.h"

#include <float.h>
#include <stdio.h>
#include <limits.h>

#include <iostream>

#include <fstream>
#include <set>
#include <limits>
#include <algorithm>
#include <vector>

#include <onnx/onnx_pb.h>
#include <wmc_utils.h>

static std::vector<int> get_node_attr_ai(const ONNX_NAMESPACE::NodeProto& node, const char* key)
{
    std::vector<int> v;

    for (int i=0; i<node.attribute_size(); i++)
    {
        const ONNX_NAMESPACE::AttributeProto& attr = node.attribute(i);
        if (attr.name() == key)
        {
            v.resize(attr.ints_size());
            for (int j=0; j<attr.ints_size(); j++)
            {
                v[j] = attr.ints(j);
            }

            break;
        }
    }

    return v;
}

static std::vector<float> get_node_attr_af(const ONNX_NAMESPACE::NodeProto& node, const char* key)
{
    std::vector<float> v;

    for (int i=0; i<node.attribute_size(); i++)
    {
        const ONNX_NAMESPACE::AttributeProto& attr = node.attribute(i);
        if (attr.name() == key)
        {
            v.resize(attr.floats_size());
            for (int j=0; j<attr.floats_size(); j++)
            {
                v[j] = attr.floats(j);
            }

            break;
        }
    }

    return v;
}

static int get_node_attr_i(const ONNX_NAMESPACE::NodeProto& node, const char* key, int def = 0)
{
    for (int i=0; i<node.attribute_size(); i++)
    {
        const ONNX_NAMESPACE::AttributeProto& attr = node.attribute(i);
        if (attr.name() == key)
        {
            return attr.i();
        }
    }

    return def;
}

static float get_node_attr_f(const ONNX_NAMESPACE::NodeProto& node, const char* key, float def = 0.f)
{
    for (int i=0; i<node.attribute_size(); i++)
    {
        const ONNX_NAMESPACE::AttributeProto& attr = node.attribute(i);
        if (attr.name() == key)
        {
            return attr.f();
        }
    }

    return def;
}

static std::string get_node_attr_s(const ONNX_NAMESPACE::NodeProto& node, const char* key, const std::string& def = std::string())
{
    for (int i=0; i<node.attribute_size(); i++)
    {
        const ONNX_NAMESPACE::AttributeProto& attr = node.attribute(i);
        if (attr.name() == key)
        {
            return attr.s();
        }
    }

    return def;
}

static ONNX_NAMESPACE::TensorProto get_node_attr_tensor(const ONNX_NAMESPACE::NodeProto& node, const char* key)
{
    for (int i=0; i<node.attribute_size(); i++)
    {
        const ONNX_NAMESPACE::AttributeProto& attr = node.attribute(i);
        if (attr.name() == key)
        {
            return attr.t();
        }
    }

    return ONNX_NAMESPACE::TensorProto();
}

static int get_tensor_proto_data_size(const ONNX_NAMESPACE::TensorProto& tp)
{
    if (tp.has_raw_data())
    {
        const std::string& raw_data = tp.raw_data();
        int size = (int)raw_data.size() / 4;
        return size;
    }
    else if (tp.data_type() == 1)
    {
        return tp.float_data_size();
    }

    return 0;
}

static void fwrite_tensor_proto_data(const ONNX_NAMESPACE::TensorProto& tp, std::vector<char> &bv)
{
    int size = get_tensor_proto_data_size(tp);

    if (tp.has_raw_data())
    {
        const std::string& raw_data = tp.raw_data();
        for (const auto &x : raw_data) {
            bv.push_back(x);
        }
    }
    else if (tp.data_type() == 1)
    {
        const auto *data = reinterpret_cast<const char *>(tp.float_data().data());
        for (size_t i = 0; i < sizeof(float) * size; i++) {
            bv.push_back(data[i]);
        }
    }
}

tl::expected<NcnnModel, std::string> onnx2ncnn(const std::string &model_str)
{
    std::vector<char> pp;
    std::vector<char> bv;

    ONNX_NAMESPACE::ModelProto model;
    bool s1 = model.ParseFromString(model_str);

    // load
    if (!s1)
    {
        return tl::make_unexpected( "read_proto_from_binary failed");
    }

    // magic
    fprintf(pp, "7767517\n");

    const ONNX_NAMESPACE::GraphProto& graph = model.graph();
    ONNX_NAMESPACE::GraphProto* mutable_graph = model.mutable_graph();

    int node_count = graph.node_size();

    // node reference
    std::map<std::string, int> node_reference;

    // weight node and weight reshape node
    std::map<std::string, ONNX_NAMESPACE::TensorProto> weights;

    // weight node before BinaryOp
    std::map<std::string, ONNX_NAMESPACE::TensorProto> binaryop_weights;

    for (int j=0; j<graph.initializer_size(); j++)
    {
        const ONNX_NAMESPACE::TensorProto& initializer = graph.initializer(j);

//         fprintf(stderr, "weight = %s\n", initializer.name().c_str());

        weights[initializer.name()] = initializer;
    }

    // global definition line
    // [layer count] [blob count]
    std::set<std::string> blob_names;
    for (int i=0; i<node_count; i++)
    {
        const ONNX_NAMESPACE::NodeProto& node = graph.node(i);

        const std::string& op = node.op_type();

        std::string name = node.name();
        if (name.empty())
        {
            name = node.output(0);
        }

        if (op == "Constant")
        {
            ONNX_NAMESPACE::TensorProto tensor = get_node_attr_tensor(node, "value");
            weights[node.output(0)] = tensor;
            continue;
        }
        else if (op == "Reshape")
        {
            if (node.input_size() == 1)
            {
                const std::string& input_name = node.input(0);

                // check weight
                if (weights.find(input_name) != weights.end())
                {
                    weights[node.output(0)] = weights[input_name];
                    continue;
                }
            }
            else if (node.input_size() == 2)
            {
                // opset 5
                const std::string& input_name = node.input(0);

                // check weight
                if (weights.find(input_name) != weights.end())
                {
                    weights[node.output(0)] = weights[input_name];

                    // set weight shape directly
                    const ONNX_NAMESPACE::TensorProto& shape_tp = weights[node.input(1)];
                    const int64_t* shape_data = shape_tp.int64_data().data();

                    weights[node.output(0)].clear_dims();
                    for (int j=0; j<shape_tp.int64_data_size(); j++)
                    {
                        weights[node.output(0)].add_dims(shape_data[j]);
                    }

                    continue;
                }
            }
        }
        else
        {
            bool isBinaryOp = false;
            if (op == "Add" || op == "Mul")
            {
                isBinaryOp = true;
            }

            if (isBinaryOp)
            {
                // check weights
                for (int j=0; j<node.input_size(); j++)
                {
                    const std::string& input_name = node.input(j);

                    std::map<std::string, ONNX_NAMESPACE::TensorProto>::iterator it = weights.find(input_name);
                    if (it != weights.end())
                    {
                        // binary op with weight, insert MemoryData layer and const blob
                        binaryop_weights[input_name] = it->second;
                        weights.erase(it);
                    }
                }
            }
        }

        for (int j=0; j<(int)node.input_size(); j++)
        {
            const std::string& input_name = node.input(j);

            // check weight
            if (weights.find(input_name) != weights.end())
            {
                continue;
            }

            blob_names.insert(input_name);

            if (node_reference.find(input_name) == node_reference.end())
            {
                node_reference[input_name] = 1;
            }
            else
            {
                node_reference[input_name] = node_reference[input_name] + 1;
            }
        }

        if (op == "Dropout")
        {
            const std::string& output_name = node.output(0);
            blob_names.insert(output_name);
            continue;
        }

        for (int j=0; j<(int)node.output_size(); j++)
        {
            const std::string& output_name = node.output(j);

            blob_names.insert(output_name);
        }
    }

    // include Input node
    int input_node_count = 0;
    for (int j=0; j<graph.input_size(); j++)
    {
        const std::string& input_name = graph.input(j).name();

        // check weight
        if (weights.find(input_name) != weights.end())
            continue;

        // check weight before BinaryOp
        if (binaryop_weights.find(input_name) != binaryop_weights.end())
            continue;

        blob_names.insert(input_name);

        input_node_count++;
    }

    // op chain fusion
    int reduced_node_count = 0;
    for (int i=0; i<node_count; i++)
    {
        ONNX_NAMESPACE::NodeProto* node = mutable_graph->mutable_node(i);

        // MatMul <= Transpose(weight) - MatMul
        if (node->op_type() == "Transpose")
        {
            // check weight
            if (weights.find(node->input(0)) == weights.end())
                continue;

            ONNX_NAMESPACE::TensorProto& B = weights[node->input(0)];
            if (B.dims_size() != 2)
                continue;

            if (node_reference[node->output(0)] != 1)
                continue;

            // perm = (1, 0)
            std::vector<int> perm = get_node_attr_ai(*node, "perm");
            if (perm.size() != 2)
                continue;
            if (perm[0] != 1 || perm[1] != 0)
                continue;

            if (i+1 >= node_count)
                continue;

            ONNX_NAMESPACE::NodeProto* node2 = mutable_graph->mutable_node(i+1);

            if (node2->op_type() != "MatMul")
                continue;

            // reduce
            node->set_op_type("noop_reducedncnn");

            node_reference.erase(node_reference.find(node->output(0)));
            blob_names.erase(node->output(0));

            node2->set_input(1, node->input(0));

            // permute weight
            {
                const int h = B.dims(0);
                const int w = B.dims(1);

                std::vector<float> permuted_data;
                permuted_data.reserve(h * w);
                const float* bptr = B.has_raw_data() ? (const float*)B.raw_data().data() : B.float_data().data();

                for (int j=0; j<w; j++)
                {
                    for (int k=0; k<h; k++)
                    {
                        float vb = bptr[ k*w + j ];
                        permuted_data.push_back(vb);
                    }
                }

                B.set_dims(0, w);
                B.set_dims(1, h);

                if (B.has_raw_data())
                {
                    B.set_raw_data(permuted_data.data(), permuted_data.size() * sizeof(float));
                }
                else
                {
                    for (int j=0; j<(int)permuted_data.size(); j++)
                        B.set_float_data(j, permuted_data[j]);
                }
            }

            reduced_node_count += 1;
            i += 1;
        }
    }

    // remove node_reference entry with reference equals to one
    int splitncnn_blob_count = 0;
    std::map<std::string, int>::iterator it = node_reference.begin();
    while (it != node_reference.end())
    {
        if (it->second == 1)
        {
            node_reference.erase(it++);
        }
        else
        {
            splitncnn_blob_count += it->second;
//             fprintf(stderr, "%s %d\n", it->first.c_str(), it->second);
            ++it;
        }
    }

    fprintf(pp, "%lu %lu\n", node_count - reduced_node_count + input_node_count + node_reference.size() + graph.initializer_size() - weights.size(), blob_names.size() + splitncnn_blob_count);

    int internal_split = 0;

    // place Input at the beginning
    for (int j=0; j<graph.input_size(); j++)
    {
        const std::string& input_name = graph.input(j).name();

        // check weight
        if (weights.find(input_name) != weights.end())
            continue;

        // check weight before BinaryOp
        if (binaryop_weights.find(input_name) != binaryop_weights.end())
            continue;

        fprintf(pp, "%-16s %-24s 0 1 %s\n", "Input", input_name.c_str(), input_name.c_str());

        // split the input
        if (node_reference.find(input_name) == node_reference.end()){
            continue;
        }

        int refcount = node_reference[input_name];
        if (refcount <= 1){
            continue;
        }

        char splitname[256];
        sprintf(splitname, "splitncnn_input%d", j);
        fprintf(pp, "%-16s %-24s %d %d", "Split", splitname, 1, refcount);
        fprintf(pp, " %s", input_name.c_str());

        for (int k=0; k<refcount; k++){
            fprintf(pp, " %s_splitncnn_%d", input_name.c_str(), k);
        }
        fprintf(pp, "\n");
    }

    // place MemoryData next
    for (int j=0; j<graph.input_size(); j++)
    {
        const std::string& input_name = graph.input(j).name();

        // check weight before BinaryOp
        if (binaryop_weights.find(input_name) == binaryop_weights.end())
            continue;

        fprintf(pp, "%-16s %-24s 0 1 %s", "MemoryData", input_name.c_str(), input_name.c_str());

        const ONNX_NAMESPACE::TensorProto& M = binaryop_weights[input_name];

        if (M.dims_size() == 1) {
            fprintf(pp, " 0=%d", (int)M.dims(0));
        } else if (M.dims_size() == 2) {
            fprintf(pp, " 0=%d", (int)M.dims(1));
            fprintf(pp, " 1=%d", (int)M.dims(0));
        } else if (M.dims_size() == 3) {
            fprintf(pp, " 0=%d", (int)M.dims(2));
            fprintf(pp, " 1=%d", (int)M.dims(1));
            fprintf(pp, " 2=%d", (int)M.dims(0));
        }

        fprintf(pp, "\n");

        fwrite_tensor_proto_data(M, bv);
    }

    for (int i=0; i<node_count; i++)
    {
        const ONNX_NAMESPACE::NodeProto& node = graph.node(i);

        const std::string& op = node.op_type();

//         fprintf(stderr, "op = %s\n", op.c_str());

        if (op == "noop_reducedncnn")
        {
            continue;
        }

        std::string name = node.name();
        if (name.empty())
        {
            name = node.output(0);
        }

        int input_size = node.input_size();
        int output_size = node.output_size();

        for (int j=0; j<(int)node.input_size(); j++)
        {
            const std::string& input_name = node.input(j);

            // check weight
            if (weights.find(input_name) != weights.end())
            {
                input_size--;
            }

//             fprintf(stderr, "  input = %s\n", input_name.c_str());
        }
        /*
        for (int j=0; j<(int)node.output_size(); j++)
        {
            const std::string& output_name = node.output(j);
            fprintf(stderr, "  output = %s\n", output_name.c_str());
        } 
        */

        if (op == "Abs")
        {
            fprintf(pp, "%-16s", "UnaryOp");
        }
        else if (op == "Acos")
        {
            fprintf(pp, "%-16s", "UnaryOp");
        }
        else if (op == "Add")
        {
            fprintf(pp, "%-16s", "BinaryOp");
        }
        else if (op == "Asin")
        {
            fprintf(pp, "%-16s", "UnaryOp");
        }
        else if (op == "Atan")
        {
            fprintf(pp, "%-16s", "UnaryOp");
        }
        else if (op == "AveragePool" || op == "MaxPool")
        {
            fprintf(pp, "%-16s", "Pooling");
        }
        else if (op == "BatchNormalization")
        {
            fprintf(pp, "%-16s", "BatchNorm");
        }
        else if (op == "Ceil")
        {
            fprintf(pp, "%-16s", "UnaryOp");
        }
        else if (op == "Clip")
        {
            fprintf(pp, "%-16s", "Clip");
        }
        else if (op == "Concat")
        {
            fprintf(pp, "%-16s", "Concat");
        }
        else if (op == "Constant")
        {
            // check weight before BinaryOp
            if (binaryop_weights.find(node.output(0)) != binaryop_weights.end())
            {
                fprintf(pp, "%-16s", "MemoryData");
            }
            else
            {
                continue;
            }
        }
        else if (op == "Conv")
        {
            int group = get_node_attr_i(node, "group", 1);
            if (group > 1) {
                fprintf(pp, "%-16s", "ConvolutionDepthWise");
            } else {
                fprintf(pp, "%-16s", "Convolution");
            }
        }
        else if (op == "ConvTranspose")
        {
            int group = get_node_attr_i(node, "group", 1);
            if (group > 1) {
                fprintf(pp, "%-16s", "DeconvolutionDepthWise");
            } else {
                fprintf(pp, "%-16s", "Deconvolution");
            }
        }
        else if (op == "Cos")
        {
            fprintf(pp, "%-16s", "UnaryOp");
        }
        else if (op == "Div")
        {
            fprintf(pp, "%-16s", "BinaryOp");
        }
        else if (op == "Dropout")
        {
            fprintf(pp, "%-16s", "Dropout");
            output_size = 1;
        }
        else if (op == "Elu")
        {
            fprintf(pp, "%-16s", "ELU");
        }
        else if (op == "Exp")
        {
            fprintf(pp, "%-16s", "UnaryOp");
        }
        else if (op == "Flatten")
        {
            fprintf(pp, "%-16s", "Flatten");
        }
        else if (op == "Floor")
        {
            fprintf(pp, "%-16s", "UnaryOp");
        }
        else if (op == "Gemm")
        {
            float alpha = get_node_attr_f(node, "alpha", 1.f);
            float beta = get_node_attr_f(node, "beta", 1.f);
            int transA = get_node_attr_i(node, "transA", 0);
            int transB = get_node_attr_i(node, "transB", 0);

            if (alpha == 1.f && beta == 1.f)
            {
                // InnerProduct-like A * B + C
                if (transA == 0 && transB == 1)
                {
                    fprintf(pp, "%-16s", "InnerProduct");
                }
            }

            // TODO
        }
        else if (op == "GlobalAveragePool")
        {
            fprintf(pp, "%-16s", "Pooling");
        }
        else if (op == "GlobalMaxPool")
        {
            fprintf(pp, "%-16s", "Pooling");
        }
        else if (op == "ImageScaler")
        {
            fprintf(pp, "%-16s", "Scale");
        }
        else if (op == "InstanceNormalization")
        {
            fprintf(pp, "%-16s", "InstanceNorm");
        }
        else if (op == "LeakyRelu")
        {
            fprintf(pp, "%-16s", "ReLU");
        }
        else if (op == "Log")
        {
            fprintf(pp, "%-16s", "UnaryOp");
        }
        else if (op == "LRN")
        {
            fprintf(pp, "%-16s", "LRN");
        }
        else if (op == "MatMul")
        {
            fprintf(pp, "%-16s", "InnerProduct");
        }
        else if (op == "Max")
        {
            fprintf(pp, "%-16s", "BinaryOp");
        }
        else if (op == "Min")
        {
            fprintf(pp, "%-16s", "BinaryOp");
        }
        else if (op == "Mul")
        {
            fprintf(pp, "%-16s", "BinaryOp");
        }
        else if (op == "Neg")
        {
            fprintf(pp, "%-16s", "UnaryOp");
        }
        else if (op == "Pad")
        {
            fprintf(pp, "%-16s", "Padding");
        }
        else if (op == "Pow")
        {
            fprintf(pp, "%-16s", "BinaryOp");
        }
        else if (op == "PRelu")
        {
            fprintf(pp, "%-16s", "PReLU");
        }
        else if (op == "Reciprocal")
        {
            fprintf(pp, "%-16s", "UnaryOp");
        }
        else if (op == "Relu")
        {
            fprintf(pp, "%-16s", "ReLU");
        }
        else if (op == "Reshape")
        {
            if (node.input_size() == 1 || node.input_size() == 2)
            {
                const std::string& input_name = node.input(0);

                // skip weight reshape
                if (weights.find(input_name) != weights.end())
                {
                    continue;
                }
            }
            fprintf(pp, "%-16s", "Reshape");
        }
        else if (op == "Sigmoid")
        {
            fprintf(pp, "%-16s", "Sigmoid");
        }
        else if (op == "Sin")
        {
            fprintf(pp, "%-16s", "UnaryOp");
        }
        else if (op == "Slice")
        {
            fprintf(pp, "%-16s", "Crop");
        }
        else if (op == "Softmax")
        {
            fprintf(pp, "%-16s", "Softmax");
        }
        else if (op == "Sqrt")
        {
            fprintf(pp, "%-16s", "UnaryOp");
        }
        else if (op == "Sub")
        {
            fprintf(pp, "%-16s", "BinaryOp");
        }
        else if (op == "Sum")
        {
            fprintf(pp, "%-16s", "Eltwise");
        }
        else if (op == "Tan")
        {
            fprintf(pp, "%-16s", "UnaryOp");
        }
        else if (op == "Transpose")
        {
            fprintf(pp, "%-16s", "Permute");
        }
        else if (op == "Upsample" || op == "Resize")
        {
            fprintf(pp, "%-16s", "Interp");
        }
        else
        {
            return tl::make_unexpected(op + " not supported yet!");
        }

        fprintf(pp, " %-24s %d %d", name.c_str(), input_size, output_size);

        for (int j=0; j<node.input_size(); j++)
        {
            std::string input_name = node.input(j);

            // check weight
            if (weights.find(input_name) != weights.end())
            {
                continue;
            }

            if (node_reference.find(input_name) != node_reference.end())
            {
                int refidx = node_reference[input_name] - 1;
                node_reference[input_name] = refidx;

                char splitsuffix[256];
                sprintf(splitsuffix, "_splitncnn_%d", refidx);
                input_name = input_name + splitsuffix;
            }

            fprintf(pp, " %s", input_name.c_str());
        }

        for (int j=0; j<output_size; j++)
        {
            const std::string& output_name = node.output(j);

            fprintf(pp, " %s", output_name.c_str());
        }

        if (op == "Abs")
        {
            int op_type = 0;
            fprintf(pp, " 0=%d", op_type);
        }
        else if (op == "Acos")
        {
            int op_type = 13;
            fprintf(pp, " 0=%d", op_type);
        }
        else if (op == "Add")
        {
            int op_type = 0;
            fprintf(pp, " 0=%d", op_type);
        }
        else if (op == "Asin")
        {
            int op_type = 12;
            fprintf(pp, " 0=%d", op_type);
        }
        else if (op == "Atan")
        {
            int op_type = 14;
            fprintf(pp, " 0=%d", op_type);
        }
        else if (op == "AveragePool" || op == "MaxPool")
        {
            std::string auto_pad = get_node_attr_s(node, "auto_pad");//TODO
            std::vector<int> kernel_shape = get_node_attr_ai(node, "kernel_shape");
            std::vector<int> strides = get_node_attr_ai(node, "strides");
            std::vector<int> pads = get_node_attr_ai(node, "pads");

            int pool = op == "AveragePool" ? 1 : 0;
            int pad_mode = 1;

            if (auto_pad == "SAME_LOWER" || auto_pad == "SAME_UPPER")
            {
                // TODO
                pad_mode = 2;
            }

            fprintf(pp, " 0=%d", pool);

            if (kernel_shape.size() == 1) {
                fprintf(pp, " 1=%d", kernel_shape[0]);
            } else if (kernel_shape.size() == 2) {
                fprintf(pp, " 1=%d", kernel_shape[1]);
                fprintf(pp, " 11=%d", kernel_shape[0]);
            }

            if (strides.size() == 1) {
                fprintf(pp, " 2=%d", strides[0]);
            } else if (strides.size() == 2) {
                fprintf(pp, " 2=%d", strides[1]);
                fprintf(pp, " 12=%d", strides[0]);
            }

            if (pads.size() == 1) {
                fprintf(pp, " 3=%d", pads[0]);
            } else if (pads.size() == 2) {
                fprintf(pp, " 3=%d", pads[1]);
                fprintf(pp, " 13=%d", pads[0]);
            } else if (pads.size() == 4) {
                fprintf(pp, " 3=%d", pads[1]);
                fprintf(pp, " 13=%d", pads[0]);
                fprintf(pp, " 14=%d", pads[3]);
                fprintf(pp, " 15=%d", pads[2]);
            }

            fprintf(pp, " 5=%d", pad_mode);
        }
        else if (op == "BatchNormalization")
        {
            float epsilon = get_node_attr_f(node, "epsilon", 1e-5f);

            const ONNX_NAMESPACE::TensorProto& scale = weights[node.input(1)];
            const ONNX_NAMESPACE::TensorProto& B = weights[node.input(2)];
            const ONNX_NAMESPACE::TensorProto& mean = weights[node.input(3)];
            const ONNX_NAMESPACE::TensorProto& var = weights[node.input(4)];

            int channels = get_tensor_proto_data_size(scale);

            fprintf(pp, " 0=%d", channels);

            fwrite_tensor_proto_data(scale, bv);
            fwrite_tensor_proto_data(mean, bv);
            // apply epsilon to var
            {
                const float* v = var.has_raw_data() ? (const float*)var.raw_data().data() : var.float_data().data();

                for (int j=0; j<channels; j++)
                {
                    float ve = v[j] + epsilon;
                    fwrite(&ve, sizeof(float), 1, bv);
                }
            }
            fwrite_tensor_proto_data(B, bv);
        }
        else if (op == "Ceil")
        {
            int op_type = 3;
            fprintf(pp, " 0=%d", op_type);
        }
        else if (op == "Clip")
        {
            float min = get_node_attr_f(node, "min", -FLT_MAX);
            float max = get_node_attr_f(node, "max", FLT_MAX);
            fprintf(pp, " 0=%f", min);
            fprintf(pp, " 1=%f", max);
        }
        else if (op == "Concat")
        {
            int axis = get_node_attr_i(node, "axis", 1);
            fprintf(pp, " 0=%d", axis-1);
        }
        else if (op == "Constant")
        {
            // check weight before BinaryOp
            if (binaryop_weights.find(name) != binaryop_weights.end())
            {
                const ONNX_NAMESPACE::TensorProto& M = binaryop_weights[name];

                if (M.dims_size() == 1) {
                    fprintf(pp, " 0=%d", (int)M.dims(0));
                } else if (M.dims_size() == 2) {
                    fprintf(pp, " 0=%d", (int)M.dims(1));
                } else if (M.dims_size() == 3) {
                    fprintf(pp, " 0=%d", (int)M.dims(2));
                    fprintf(pp, " 1=%d", (int)M.dims(1));
                } else if (M.dims_size() == 4) {
                    fprintf(pp, " 0=%d", (int)M.dims(3));
                    fprintf(pp, " 1=%d", (int)M.dims(2));
                    fprintf(pp, " 2=%d", (int)M.dims(1));
                }

                fwrite_tensor_proto_data(M, bv);
            }
        }
        else if (op == "Conv")
        {
            const ONNX_NAMESPACE::TensorProto& W = weights[node.input(1)];

            int num_filter = W.dims(0);
            int has_bias = node.input_size() == 3 ? 1 : 0;

            std::string auto_pad = get_node_attr_s(node, "auto_pad");//TODO
            std::vector<int> kernel_shape = get_node_attr_ai(node, "kernel_shape");
            std::vector<int> dilations = get_node_attr_ai(node, "dilations");
            std::vector<int> strides = get_node_attr_ai(node, "strides");
            std::vector<int> pads = get_node_attr_ai(node, "pads");
            int group = get_node_attr_i(node, "group", 1);

            fprintf(pp, " 0=%d", num_filter);

            if (kernel_shape.size() == 1) {
                fprintf(pp, " 1=%d", kernel_shape[0]);
            } else if (kernel_shape.size() == 2) {
                fprintf(pp, " 1=%d", kernel_shape[1]);
                fprintf(pp, " 11=%d", kernel_shape[0]);
            }

            if (dilations.size() == 1) {
                fprintf(pp, " 2=%d", dilations[0]);
            } else if (dilations.size() == 2) {
                fprintf(pp, " 2=%d", dilations[1]);
                fprintf(pp, " 12=%d", dilations[0]);
            }

            if (strides.size() == 1) {
                fprintf(pp, " 3=%d", strides[0]);
            } else if (strides.size() == 2) {
                fprintf(pp, " 3=%d", strides[1]);
                fprintf(pp, " 13=%d", strides[0]);
            }

            if (auto_pad == "SAME_LOWER" || auto_pad == "SAME_UPPER")
            {
                // TODO
                fprintf(pp, " 4=-233");
            }
            else
            {

            if (pads.size() == 1) {
                fprintf(pp, " 4=%d", pads[0]);
            } else if (pads.size() == 2) {
                fprintf(pp, " 4=%d", pads[1]);
                fprintf(pp, " 14=%d", pads[0]);
            } else if (pads.size() == 4) {
                fprintf(pp, " 4=%d", pads[1]);
                fprintf(pp, " 14=%d", pads[0]);
                // TODO hpad2=pads[2]   wpad2=pads[3]
            }

            }

            fprintf(pp, " 5=%d", has_bias);

            fprintf(pp, " 6=%d", get_tensor_proto_data_size(W));

            if (group > 1) {
                fprintf(pp, " 7=%d", group);
            }

            int quantize_tag = 0;
            fwrite(&quantize_tag, sizeof(int), 1, bv);

            fwrite_tensor_proto_data(W, bv);

            if (has_bias)
            {
                const ONNX_NAMESPACE::TensorProto& B = weights[node.input(2)];
                fwrite_tensor_proto_data(B, bv);
            }
        }
        else if (op == "ConvTranspose")
        {
            const ONNX_NAMESPACE::TensorProto& W = weights[node.input(1)];

            int has_bias = node.input_size() == 3 ? 1 : 0;

            std::string auto_pad = get_node_attr_s(node, "auto_pad");//TODO
            std::vector<int> kernel_shape = get_node_attr_ai(node, "kernel_shape");
            std::vector<int> dilations = get_node_attr_ai(node, "dilations");
            std::vector<int> strides = get_node_attr_ai(node, "strides");
            std::vector<int> output_padding = get_node_attr_ai(node, "output_padding");//TODO implement adj
            std::vector<int> output_shape = get_node_attr_ai(node, "output_shape");//TODO
            std::vector<int> pads = get_node_attr_ai(node, "pads");
            int group = get_node_attr_i(node, "group", 1);
            int num_filter = W.dims(1) * group;

            fprintf(pp, " 0=%d", num_filter);

            if (kernel_shape.size() == 1) {
                fprintf(pp, " 1=%d", kernel_shape[0]);
            } else if (kernel_shape.size() == 2) {
                fprintf(pp, " 1=%d", kernel_shape[1]);
                fprintf(pp, " 11=%d", kernel_shape[0]);
            }

            if (dilations.size() == 1) {
                fprintf(pp, " 2=%d", dilations[0]);
            } else if (dilations.size() == 2) {
                fprintf(pp, " 2=%d", dilations[1]);
                fprintf(pp, " 12=%d", dilations[0]);
            }

            if (strides.size() == 1) {
                fprintf(pp, " 3=%d", strides[0]);
            } else if (strides.size() == 2) {
                fprintf(pp, " 3=%d", strides[1]);
                fprintf(pp, " 13=%d", strides[0]);
            }

            if (auto_pad == "SAME_LOWER" || auto_pad == "SAME_UPPER")
            {
                // TODO
                fprintf(pp, " 4=-233");
            }
            else
            {

            if (pads.size() == 1) {
                fprintf(pp, " 4=%d", pads[0]);
            } else if (pads.size() == 2) {
                fprintf(pp, " 4=%d", pads[1]);
                fprintf(pp, " 14=%d", pads[0]);
            } else if (pads.size() == 4) {
                fprintf(pp, " 4=%d", pads[1]);
                fprintf(pp, " 14=%d", pads[0]);
                // TODO hpad2=pads[2]   wpad2=pads[3]
            }

            }

            fprintf(pp, " 5=%d", has_bias);

            fprintf(pp, " 6=%d", get_tensor_proto_data_size(W));

            if (group > 1) {
                fprintf(pp, " 7=%d", group);
            }

            int quantize_tag = 0;
            fwrite(&quantize_tag, sizeof(int), 1, bv);

            int maxk = 0;
            if (kernel_shape.size() == 2)
            {
                maxk = kernel_shape[1] * kernel_shape[0];
            }
            else
            {
                maxk = kernel_shape[0] * kernel_shape[0];
            }
            int weight_data_size = get_tensor_proto_data_size(W);
            const float* weight_data = 0;
            if (W.has_raw_data())
            {
                weight_data = (const float*)W.raw_data().data();
            }
            else if (W.data_type() == 1)
            {
                weight_data = W.float_data().data();
            }
            for (int g=0; g<group; g++)
            {
            // reorder weight from inch-outch to outch-inch
            int num_filter_g = num_filter / group;
            int num_input = weight_data_size / maxk / num_filter_g / group;
            const float* weight_data_ptr = weight_data + g * maxk * num_filter_g * num_input;
            for (int k=0; k<num_filter_g; k++)
            {
                for (int j=0; j<num_input; j++)
                {
                    fwrite(weight_data_ptr + (j*num_filter_g + k) * maxk, sizeof(float), maxk, bv);
                }
            }
            }

            if (has_bias)
            {
                const ONNX_NAMESPACE::TensorProto& B = weights[node.input(2)];
                fwrite_tensor_proto_data(B, bv);
            }
        }
        else if (op == "Cos")
        {
            int op_type = 10;
            fprintf(pp, " 0=%d", op_type);
        }
        else if (op == "Div")
        {
            int op_type = 3;
            fprintf(pp, " 0=%d", op_type);
        }
        else if (op == "Dropout")
        {
            // no-op
        }
        else if (op == "Elu")
        {
            float alpha = get_node_attr_f(node, "alpha", 1.f);
            fprintf(pp, " 0=%f", alpha);
        }
        else if (op == "Exp")
        {
            int op_type = 7;
            fprintf(pp, " 0=%d", op_type);
        }
        else if (op == "Flatten")
        {
            int axis = get_node_attr_i(node, "axis", 1);
            if (axis != 1)
            {
                return tl::make_unexpected( "Unsupported Flatten axis " + std::to_string(axis) + "!");
            }
        }
        else if (op == "Floor")
        {
            int op_type = 2;
            fprintf(pp, " 0=%d", op_type);
        }
        else if (op == "Gemm")
        {
            float alpha = get_node_attr_f(node, "alpha", 1.f);
            float beta = get_node_attr_f(node, "beta", 1.f);
            int transA = get_node_attr_i(node, "transA", 0);
            int transB = get_node_attr_i(node, "transB", 0);

            if (alpha == 1.f && beta == 1.f)
            {
                // InnerProduct-like A * B + C
                if (transA == 0 && transB == 1)
                {
                    const ONNX_NAMESPACE::TensorProto& B = weights[node.input(1)];
                    const ONNX_NAMESPACE::TensorProto& C = weights[node.input(2)];

                    fprintf(pp, " 0=%d", get_tensor_proto_data_size(C));
                    fprintf(pp, " 1=1");
                    fprintf(pp, " 2=%d", get_tensor_proto_data_size(B));

                    int quantize_tag = 0;
                    fwrite(&quantize_tag, sizeof(int), 1, bv);

                    fwrite_tensor_proto_data(B, bv);
                    fwrite_tensor_proto_data(C, bv);
                }
            }
        }
        else if (op == "GlobalAveragePool")
        {
            int pool = 1;
            int global_pool = 1;

            fprintf(pp, " 0=%d", pool);
            fprintf(pp, " 4=%d", global_pool);
        }
        else if (op == "GlobalMaxPool")
        {
            int pool = 0;
            int global_pool = 1;

            fprintf(pp, " 0=%d", pool);
            fprintf(pp, " 4=%d", global_pool);
        }
        else if (op == "ImageScaler")
        {
            std::vector<float> bias = get_node_attr_af(node, "bias");
            float scale = get_node_attr_f(node, "scale", 1.f);

            int channels = bias.size();

            fprintf(pp, " 0=%d", channels);
            fprintf(pp, " 1=1");

            for (int j=0; j<channels; j++)
            {
                fwrite(&scale, sizeof(float), 1, bv);
            }
            fwrite(&bias[0], sizeof(float), channels, bv);
        }
        else if (op == "InstanceNormalization")
        {
            float eps = get_node_attr_f(node, "epsilon", 1e-5f);
            const ONNX_NAMESPACE::TensorProto& scale = weights[node.input(1)];
            const ONNX_NAMESPACE::TensorProto& B = weights[node.input(2)];
            int channels = get_tensor_proto_data_size(scale);

            fprintf(pp, " 0=%d", channels);
            fprintf(pp, " 1=%f", eps);
            fwrite_tensor_proto_data(scale, bv);
            fwrite_tensor_proto_data(B, bv);
        }
        else if (op == "LeakyRelu")
        {
            float alpha = get_node_attr_f(node, "alpha", 0.01f);

            fprintf(pp, " 0=%f", alpha);
        }
        else if (op == "Log")
        {
            int op_type = 8;
            fprintf(pp, " 0=%d", op_type);
        }
        else if (op == "LRN")
        {
            float alpha = get_node_attr_f(node, "alpha", 1.f);
            float beta = get_node_attr_f(node, "beta", 0.5f);
            float bias = get_node_attr_f(node, "bias", 1.f);
            int size = get_node_attr_i(node, "size", 1);

            int norm_region = 0;

            fprintf(pp, " 0=%d", norm_region);
            fprintf(pp, " 1=%d", size);
            fprintf(pp, " 2=%f", alpha);
            fprintf(pp, " 3=%f", beta);
            fprintf(pp, " 4=%f", bias);
        }
        else if (op == "MatMul")
        {
            const ONNX_NAMESPACE::TensorProto& B = weights[node.input(1)];

            int weight_data_size = get_tensor_proto_data_size(B);

            int num_output = B.dims(B.dims_size()-1);
            int num_input = weight_data_size / num_output;

            fprintf(pp, " 0=%d", num_output);
            fprintf(pp, " 1=0");
            fprintf(pp, " 2=%d", weight_data_size);

            int quantize_tag = 0;
            fwrite(&quantize_tag, sizeof(int), 1, bv);

            // reorder num_input-num_output to num_output-num_input
            {
                const float* bptr = B.has_raw_data() ? (const float*)B.raw_data().data() : B.float_data().data();

                for (int j=0; j<num_output; j++)
                {
                    for (int k=0; k<num_input; k++)
                    {
                        float vb = bptr[ k*num_output + j ];
                        fwrite(&vb, sizeof(float), 1, bv);
                    }
                }
            }

//                 fwrite_tensor_proto_data(B, bv)
        }
        else if (op == "Max")
        {
            int op_type = 4;
            fprintf(pp, " 0=%d", op_type);
        }
        else if (op == "Min")
        {
            int op_type = 5;
            fprintf(pp, " 0=%d", op_type);
        }
        else if (op == "Mul")
        {
            int op_type = 2;
            fprintf(pp, " 0=%d", op_type);
        }
        else if (op == "Neg")
        {
            int op_type = 1;
            fprintf(pp, " 0=%d", op_type);
        }
        else if (op == "Pad")
        {
            std::string mode = get_node_attr_s(node, "mode");
            std::vector<int> pads = get_node_attr_ai(node, "pads");
            float value = get_node_attr_f(node, "value", 0.f);

            int type = 0;
            if (mode == "constant")
            {
                type = 0;
            }
            else if (mode == "edge")
            {
                type = 1;
            }
            else if (mode == "reflect")
            {
                // FIXME
            }

            int top = pads[0];
            int bottom = pads[2];
            int left = pads[1];
            int right = pads[3];

            fprintf(pp, " 0=%d", top);
            fprintf(pp, " 1=%d", bottom);
            fprintf(pp, " 2=%d", left);
            fprintf(pp, " 3=%d", right);
            fprintf(pp, " 4=%d", type);
            fprintf(pp, " 5=%f", value);
        }
        else if (op == "Pow")
        {
            int op_type = 6;
            fprintf(pp, " 0=%d", op_type);
        }
        else if (op == "PRelu")
        {
            const ONNX_NAMESPACE::TensorProto& slope = weights[node.input(1)];

            int num_slope = get_tensor_proto_data_size(slope);

            fprintf(pp, " 0=%d", num_slope);

            fwrite_tensor_proto_data(slope, bv);
        }
        else if (op == "Reciprocal")
        {
            int op_type = 15;
            fprintf(pp, " 0=%d", op_type);
        }
        else if (op == "Reshape")
        {
            std::vector<int> shape;

            if (node.input_size() == 1)
            {
                shape = get_node_attr_ai(node, "shape");
            }
            else
            {
                const ONNX_NAMESPACE::TensorProto& shape_tp = weights[node.input(1)];
                const int64_t* shape_data = shape_tp.int64_data().data();
                for (int j=0; j<shape_tp.int64_data_size(); j++)
                {
                    shape.push_back(shape_data[j]);
                }
            }

            if (shape.size() == 1) {
                fprintf(pp, " 0=%d", shape[0]);// should never reach here
            } else if (shape.size() == 2) {
                fprintf(pp, " 0=%d", shape[1]);
            } else if (shape.size() == 3) {
                fprintf(pp, " 0=%d", shape[2]);
                fprintf(pp, " 1=%d", shape[1]);
            } else if (shape.size() == 4) {
                fprintf(pp, " 0=%d", shape[3]);
                fprintf(pp, " 1=%d", shape[2]);
                fprintf(pp, " 2=%d", shape[1]);
            } else if (shape.size() == 5) {
                fprintf(pp, " 0=%d", shape[4] * shape[3]);
                fprintf(pp, " 1=%d", shape[2]);
                fprintf(pp, " 2=%d", shape[1]);
            }
        }
        else if (op == "Sigmoid")
        {
        }
        else if (op == "Sin")
        {
            int op_type = 9;
            fprintf(pp, " 0=%d", op_type);
        }
        else if (op == "Slice")
        {
            std::vector<int> starts = get_node_attr_ai(node, "starts");
            std::vector<int> ends = get_node_attr_ai(node, "ends");
            std::vector<int> steps = get_node_attr_ai(node, "steps");// TODO

            // assert step == 1
            for (int i=0; i<(int)steps.size(); i++)
            {
                if (steps[i] != 1) {
                    tl::make_unexpected("Unsupported slice step !");

                }
            }

            int woffset = 0;
            int hoffset = 0;
            int coffset = 0;
            int outw = -233;
            int outh = -233;
            int outc = -233;

            if (starts.size() == 2)
            {
                woffset = starts[1];
                outw = ends[1] == -1 ? -234 : ends[1] - starts[1];
            }
            else if (starts.size() == 3)
            {
                woffset = starts[2];
                hoffset = starts[1];
                outw = ends[2] == -1 ? -234 : ends[2] - starts[2];
                outh = ends[1] == -1 ? -234 : ends[1] - starts[1];
            }
            else if (starts.size() == 4)
            {
                woffset = starts[3];
                hoffset = starts[2];
                coffset = starts[1];
                outw = ends[3] == -1 ? -234 : ends[3] - starts[3];
                outh = ends[2] == -1 ? -234 : ends[2] - starts[2];
                outc = ends[1] == -1 ? -234 : ends[1] - starts[1];
            }

            fprintf(pp, " 0=%d", woffset);
            fprintf(pp, " 1=%d", hoffset);
            fprintf(pp, " 2=%d", coffset);
            fprintf(pp, " 3=%d", outw);
            fprintf(pp, " 4=%d", outh);
            fprintf(pp, " 5=%d", outc);
        }
        else if (op == "Softmax")
        {
            int axis = get_node_attr_i(node, "axis", 1);
            fprintf(pp, " 0=%d", axis-1);
            fprintf(pp, " 1=1");
        }
        else if (op == "Sqrt")
        {
            int op_type = 5;
            fprintf(pp, " 0=%d", op_type);
        }
        else if (op == "Sub")
        {
            int op_type = 1;
            fprintf(pp, " 0=%d", op_type);
        }
        else if (op == "Sum")
        {
            int op_type = 1;
            fprintf(pp, " 0=%d", op_type);
        }
        else if (op == "Tan")
        {
            int op_type = 11;
            fprintf(pp, " 0=%d", op_type);
        }
        else if (op == "Transpose")
        {
            std::vector<int> perm = get_node_attr_ai(node, "perm");

            if (perm.size() == 4) {
                if (perm[1] == 1 && perm[2] == 2 && perm[3] == 3)
                    fprintf(pp, " 0=0");// w h c
                else if (perm[1] == 1 && perm[2] == 3 && perm[3] == 2)
                    fprintf(pp, " 0=1");// h w c
                else if (perm[1] == 2 && perm[2] == 1 && perm[3] == 3)
                    fprintf(pp, " 0=2");// w c h
                else if (perm[1] == 2 && perm[2] == 3 && perm[3] == 1)
                    fprintf(pp, " 0=3");// c w h
                else if (perm[1] == 3 && perm[2] == 1 && perm[3] == 2)
                    fprintf(pp, " 0=4");// h c w
                else if (perm[1] == 3 && perm[2] == 2 && perm[3] == 1)
                    fprintf(pp, " 0=5");// c h w
            } else if (perm.size() == 5) {
                if (perm[1] == 1 && perm[2] == 2 && perm[3] == 3 && perm[4] == 4)
                    fprintf(pp, " 0=0");// wx h c
                else if (perm[1] == 1 && perm[2] == 3 && perm[3] == 4 && perm[4] == 2)
                    fprintf(pp, " 0=1");// h wx c
                else if (perm[1] == 2 && perm[2] == 1 && perm[3] == 3 && perm[4] == 4)
                    fprintf(pp, " 0=2");// wx c h
                else if (perm[1] == 2 && perm[2] == 3 && perm[3] == 4 && perm[4] == 1)
                    fprintf(pp, " 0=3");// c wx h
                else if (perm[1] == 3 && perm[2] == 4 && perm[3] == 1 && perm[4] == 2)
                    fprintf(pp, " 0=4");// h c wx
                else if (perm[1] == 3 && perm[2] == 4 && perm[3] == 2 && perm[4] == 1)
                    fprintf(pp, " 0=5");// c h wx
                else
                    return tl::make_unexpected( "Unsupported transpose type !");
            }
        }
        else if (op == "Upsample" || op == "Resize")
        {
            std::string mode = get_node_attr_s(node, "mode");

            std::vector<float> scales;

            if (node.input_size() == 1)
            {
                scales = get_node_attr_af(node, "scales");
            }
            else
            {
                const ONNX_NAMESPACE::TensorProto& scales_tp = weights[node.input(1)];
                const float* shape_data = scales_tp.has_raw_data() ? (const float*)scales_tp.raw_data().data() : scales_tp.float_data().data();
                
                int float_data_size = scales_tp.float_data_size();
                //float data is None, use raw data instead
                if (float_data_size == 0) {
                    float_data_size = scales_tp.dims().Get(0);
                }

                for (int j=0; j<float_data_size; j++)
                {
                    scales.push_back(shape_data[j]);
                }
            }

            int resize_type = 1;
            if (mode == "nearest")
            {
                resize_type = 1;
            }
            else if (mode == "bilinear" || mode == "linear")
            {
                resize_type = 2;
            }
            else if (mode == "trilinear")
            {
                return tl::make_unexpected("Unsupported Upsample/Resize mode !");
            }

            float h_scale = 1.f;
            float w_scale = 1.f;
            if (scales.size() == 2)
            {
                w_scale = scales[1];
            }
            else if (scales.size() == 3)
            {
                h_scale = scales[1];
                w_scale = scales[2];
            }
            else if (scales.size() == 4)
            {
                h_scale = scales[2];
                w_scale = scales[3];

                if (scales[1] != 1.f)
                    return tl::make_unexpected("Unsupported Upsample/Resize scales !");
            }
            else
            {
                return tl::make_unexpected("Unsupported Upsample/Resize scales !");
            }

            fprintf(pp, " 0=%d", resize_type);
            fprintf(pp, " 1=%f", h_scale);
            fprintf(pp, " 2=%f", w_scale);
        }
        else
        {
            // TODO op specific param
            for (int j=0; j<node.attribute_size(); j++)
            {
                const ONNX_NAMESPACE::AttributeProto& attr = node.attribute(j);
                if (attr.type() == 1)
                {
                    fprintf(stderr, "  # %s=%f\n", attr.name().c_str(), attr.f());
                }
                else if (attr.type() == 2)
                {
                    fprintf(stderr, "  # %s=%ld\n", attr.name().c_str(), attr.i());
                }
                else if (attr.type() == 3)
                {
                    fprintf(stderr, "  # %s=%s\n", attr.name().c_str(), attr.s().c_str());
                }
                else
                {
                    fprintf(stderr, "  # %s %d\n", attr.name().c_str(), attr.type());
                }
            }
        }

        fprintf(pp, "\n");

        for (int j=0; j<output_size; j++)
        {
            const std::string& output_name = node.output(j);
            if (node_reference.find(output_name) != node_reference.end())
            {
                int refcount = node_reference[output_name];
                if (refcount > 1)
                {
                    char splitname[256];
                    sprintf(splitname, "splitncnn_%d", internal_split);
                    fprintf(pp, "%-16s %-24s %d %d", "Split", splitname, 1, refcount);

                    fprintf(pp, " %s", output_name.c_str());

                    for (int k=0; k<refcount; k++)
                    {
                        fprintf(pp, " %s_splitncnn_%d", output_name.c_str(), k);
                    }
                    fprintf(pp, "\n");

                    internal_split++;
                }
            }
        }
    }

    return std::make_pair(pp, bv);
}

