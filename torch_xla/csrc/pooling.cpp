#include "torch_xla/csrc/pooling.h"
#include "tensorflow/compiler/xla/client/lib/pooling.h"
#include "tensorflow/compiler/xla/xla_client/debug_macros.h"
#include "tensorflow/compiler/xla/xla_client/util.h"
#include "torch/csrc/jit/autodiff.h"
#include "torch_xla/csrc/helpers.h"

namespace torch_xla {
namespace {

xla::TensorFormat MakeNCHWFormat(xla::int64 spatial_dim_count) {
  return {
      /*batch_dimension=*/0,
      /*feature_dimension=*/1,
      /*spatial_dimensions=*/xla::util::Iota<xla::int64>(spatial_dim_count, 2)};
}

// Holds the attributes common to all pooling operators.
struct PoolingOpAttributes {
  std::vector<xla::int64> kernel_size;
  std::vector<xla::int64> stride;
  std::vector<std::pair<xla::int64, xla::int64>> padding;
};

xla::XlaComputation CreateGeComputation(xla::PrimitiveType type) {
  xla::XlaBuilder reduction_builder("xla_ge_computation");
  xla::XlaOp x = xla::Parameter(&reduction_builder, 0,
                                xla::ShapeUtil::MakeShape(type, {}), "x");
  xla::XlaOp y = xla::Parameter(&reduction_builder, 1,
                                xla::ShapeUtil::MakeShape(type, {}), "y");
  xla::Ge(x, y);
  return ConsumeValue(reduction_builder.Build());
}

// Construct the pooling attributes for the given kernel size, stride and
// padding.
PoolingOpAttributes MakePoolingOpAttributes(
    tensorflow::gtl::ArraySlice<const xla::int64> kernel_size_attr,
    tensorflow::gtl::ArraySlice<const xla::int64> stride_attr,
    tensorflow::gtl::ArraySlice<const xla::int64> padding_attr,
    xla::int64 spatial_dim_count) {
  // Create a NCHW kernel size with 1 for batch size and feature.
  std::vector<xla::int64> kernel_size(2, 1);
  kernel_size.insert(kernel_size.end(), kernel_size_attr.begin(),
                     kernel_size_attr.end());
  // Create a NCHW stride size with 1 for batch size and feature. Same as kernel
  // size if not specified.
  std::vector<xla::int64> stride;
  if (stride_attr.empty()) {
    stride = kernel_size;
  } else {
    stride.resize(2, 1);
    stride.insert(stride.end(), stride_attr.begin(), stride_attr.end());
  }
  XLA_CHECK_EQ(padding_attr.size(), spatial_dim_count);
  std::vector<std::pair<xla::int64, xla::int64>> padding;
  for (const xla::int64 dim_pad : padding_attr) {
    padding.push_back(std::make_pair(dim_pad, dim_pad));
  }
  return {kernel_size, stride, padding};
}

// Compute the average pool kernel size required for the specified output_size
// from the given input_size, when the stride is the same as the kernel size.
std::vector<xla::int64> AdaptiveAvgPoolKernelSize(
    tensorflow::gtl::ArraySlice<const xla::int64> input_size,
    tensorflow::gtl::ArraySlice<const xla::int64> output_size) {
  // Create a NCHW kernel size with 1 for batch size and feature.
  std::vector<xla::int64> kernel_size(2, 1);
  for (int spatial_dim = 0; spatial_dim < 2; ++spatial_dim) {
    XLA_CHECK_EQ(input_size[2 + spatial_dim] % output_size[spatial_dim], 0)
        << "Target output size " << output_size[spatial_dim]
        << " doesn't divide the input size " << input_size[2 + spatial_dim];
    kernel_size.push_back(input_size[2 + spatial_dim] /
                          output_size[spatial_dim]);
  }
  return kernel_size;
}

}  // namespace

bool IsSupportedAdaptiveAvgPool2d(
    tensorflow::gtl::ArraySlice<const xla::int64> input_size,
    tensorflow::gtl::ArraySlice<const xla::int64> output_size) {
  for (int spatial_dim = 0; spatial_dim < 2; ++spatial_dim) {
    if (input_size[2 + spatial_dim] % output_size[spatial_dim] != 0) {
      return false;
    }
  }
  return true;
}

xla::XlaOp BuildMaxPoolNd(
    const xla::XlaOp& input, xla::int64 spatial_dim_count,
    tensorflow::gtl::ArraySlice<const xla::int64> kernel_size,
    tensorflow::gtl::ArraySlice<const xla::int64> stride,
    tensorflow::gtl::ArraySlice<const xla::int64> padding) {
  xla::XlaBuilder* builder = input.builder();
  xla::Shape input_shape = XlaHelpers::ShapeOfXlaOp(input);
  xla::Literal init_value =
      xla::LiteralUtil::MinValue(input_shape.element_type());
  xla::XlaOp xla_init_value = xla::ConstantLiteral(builder, init_value);
  xla::PaddingConfig padding_config = XlaHelpers::MakeXlaPaddingConfig(padding);
  xla::XlaOp padded_input = xla::Pad(input, xla_init_value, padding_config);
  PoolingOpAttributes pooling_op_attributes =
      MakePoolingOpAttributes(/*kernel_size_attr=*/kernel_size,
                              /*stride_attr=*/stride, /*padding_attr=*/padding,
                              /*spatial_dim_count=*/spatial_dim_count);
  return xla::MaxPool(
      /*operand=*/padded_input,
      /*kernel_size=*/pooling_op_attributes.kernel_size,
      /*stride=*/pooling_op_attributes.stride,
      /*padding=*/xla::Padding::kValid,
      /*data_format=*/MakeNCHWFormat(spatial_dim_count));
}

xla::XlaOp BuildMaxPoolNdBackward(
    const xla::XlaOp& out_backprop, const xla::XlaOp& input,
    xla::int64 spatial_dim_count,
    tensorflow::gtl::ArraySlice<const xla::int64> kernel_size,
    tensorflow::gtl::ArraySlice<const xla::int64> stride,
    tensorflow::gtl::ArraySlice<const xla::int64> padding) {
  xla::XlaBuilder* builder = out_backprop.builder();
  xla::Shape input_shape = XlaHelpers::ShapeOfXlaOp(input);
  xla::XlaOp init_value =
      XlaHelpers::ScalarValue<float>(0, input_shape.element_type(), builder);
  xla::XlaComputation select = CreateGeComputation(input_shape.element_type());
  xla::XlaComputation scatter =
      XlaHelpers::CreateAddComputation(input_shape.element_type());
  PoolingOpAttributes pooling_op_attributes =
      MakePoolingOpAttributes(/*kernel_size_attr=*/kernel_size,
                              /*stride_attr=*/stride, /*padding_attr=*/padding,
                              /*spatial_dim_count=*/spatial_dim_count);
  std::vector<std::pair<xla::int64, xla::int64>> window_padding;
  window_padding.resize(2);
  window_padding.insert(window_padding.end(),
                        pooling_op_attributes.padding.begin(),
                        pooling_op_attributes.padding.end());
  return xla::SelectAndScatterWithGeneralPadding(
      /*operand=*/input,
      /*select=*/select,
      /*window_dimensions=*/pooling_op_attributes.kernel_size,
      /*window_strides=*/pooling_op_attributes.stride,
      /*padding=*/window_padding,
      /*source=*/out_backprop,
      /*init_value=*/init_value,
      /*scatter=*/scatter);
}

xla::XlaOp BuildAvgPoolNd(
    const xla::XlaOp& input, xla::int64 spatial_dim_count,
    tensorflow::gtl::ArraySlice<const xla::int64> kernel_size,
    tensorflow::gtl::ArraySlice<const xla::int64> stride,
    tensorflow::gtl::ArraySlice<const xla::int64> padding,
    bool count_include_pad) {
  PoolingOpAttributes pooling_op_attributes =
      MakePoolingOpAttributes(/*kernel_size_attr=*/kernel_size,
                              /*stride_attr=*/stride, /*padding_attr=*/padding,
                              /*spatial_dim_count=*/spatial_dim_count);
  return xla::AvgPool(
      /*operand=*/input,
      /*kernel_size=*/pooling_op_attributes.kernel_size,
      /*stride=*/pooling_op_attributes.stride,
      /*padding=*/pooling_op_attributes.padding,
      /*data_format=*/MakeNCHWFormat(spatial_dim_count),
      /*counts_include_padding=*/count_include_pad);
}

xla::XlaOp BuildAvgPoolNdBackward(
    const xla::XlaOp& out_backprop, const xla::XlaOp& input,
    xla::int64 spatial_dim_count,
    tensorflow::gtl::ArraySlice<const xla::int64> kernel_size,
    tensorflow::gtl::ArraySlice<const xla::int64> stride,
    tensorflow::gtl::ArraySlice<const xla::int64> padding,
    bool count_include_pad) {
  PoolingOpAttributes pooling_op_attributes =
      MakePoolingOpAttributes(/*kernel_size_attr=*/kernel_size,
                              /*stride_attr=*/stride, /*padding_attr=*/padding,
                              /*spatial_dim_count=*/spatial_dim_count);
  auto gradients_size = XlaHelpers::SizesOfXlaOp(input);
  return xla::AvgPoolGrad(
      /*out_backprop=*/out_backprop,
      /*gradients_size=*/gradients_size,
      /*kernel_size=*/pooling_op_attributes.kernel_size,
      /*stride=*/pooling_op_attributes.stride,
      /*spatial_padding=*/pooling_op_attributes.padding,
      /*data_format=*/MakeNCHWFormat(spatial_dim_count),
      /*counts_include_padding=*/count_include_pad);
}

xla::XlaOp BuildAdaptiveAvgPool2d(
    const xla::XlaOp& input,
    tensorflow::gtl::ArraySlice<const xla::int64> output_size) {
  XLA_CHECK_EQ(output_size.size(), 2) << "Invalid output size rank";
  const auto input_size = XlaHelpers::SizesOfXlaOp(input);
  XLA_CHECK_EQ(input_size.size(), 4) << "Only 4D tensors supported";
  const auto kernel_size = AdaptiveAvgPoolKernelSize(input_size, output_size);
  std::vector<std::pair<xla::int64, xla::int64>> no_padding(2);
  return xla::AvgPool(
      /*operand=*/input,
      /*kernel_size=*/kernel_size,
      /*stride=*/kernel_size,
      /*padding=*/no_padding,
      /*data_format=*/MakeNCHWFormat(2),
      /*counts_include_padding=*/false);
}

xla::XlaOp BuildAdaptiveAvgPool2dBackward(const xla::XlaOp& out_backprop,
                                          const xla::XlaOp& input) {
  const auto out_backprop_size = XlaHelpers::SizesOfXlaOp(out_backprop);
  XLA_CHECK_EQ(out_backprop_size.size(), 4)
      << "Invalid rank of gradient output";
  std::vector<xla::int64> output_size{out_backprop_size[2],
                                      out_backprop_size[3]};
  const auto gradients_size = XlaHelpers::SizesOfXlaOp(input);
  XLA_CHECK_EQ(gradients_size.size(), 4) << "Only 4D tensors supported";
  const auto kernel_size =
      AdaptiveAvgPoolKernelSize(gradients_size, output_size);
  std::vector<std::pair<xla::int64, xla::int64>> no_padding(2);
  return xla::AvgPoolGrad(
      /*out_backprop=*/out_backprop,
      /*gradients_size=*/gradients_size,
      /*kernel_size=*/kernel_size,
      /*stride=*/kernel_size,
      /*spatial_padding=*/no_padding,
      /*data_format=*/MakeNCHWFormat(2),
      /*counts_include_padding=*/false);
}

}  // namespace torch_xla
