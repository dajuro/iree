// Copyright 2021 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/compiler/Dialect/Flow/Conversion/TensorToFlow/ConvertTensorToFlow.h"

#include "iree/compiler/Dialect/Flow/IR/FlowDialect.h"
#include "iree/compiler/Dialect/Flow/IR/FlowOps.h"
#include "mlir/Dialect/Arithmetic/IR/Arithmetic.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/MemRef/Transforms/Passes.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"

namespace mlir {
namespace iree_compiler {
namespace IREE {
namespace Flow {

/// An operation that uses `offsets`, `sizes` and `strides` (i.e. implements the
/// `OffsetSizeAndStrideInterface`) can be mapped to flow operations that
/// eventually map to DMA operations if the offsets/sizes/strides represent a
/// contiguous memory.
static bool isOffsetSizeAndStrideMappableToFlow(ArrayRef<OpFoldResult> offsets,
                                                ArrayRef<OpFoldResult> sizes,
                                                ArrayRef<OpFoldResult> strides,
                                                ArrayRef<int64_t> baseShape) {
  if (offsets.size() != baseShape.size()) {
    // Unhanded rank-reducing case.
    return false;
  }
  auto getVal = [](OpFoldResult valueOrAttr, int64_t dynamicVal) -> int64_t {
    auto attr = valueOrAttr.dyn_cast<Attribute>();
    return attr ? attr.cast<IntegerAttr>().getInt() : dynamicVal;
  };
  /// To ensure contiguity, start from the least signficant dimension. As long
  /// as the inner slices are "full slices", the current slice can be any offset
  /// and size. If the inner slices are not "full slices", the current slice
  /// must be of size 1. All strides must be one.

  bool fullSlices = true;
  for (size_t dim = offsets.size(); dim > 0; dim--) {
    int64_t staticOffset =
        getVal(offsets[dim - 1], ShapedType::kDynamicStrideOrOffset);
    int64_t staticSize = getVal(sizes[dim - 1], ShapedType::kDynamicSize);
    int64_t staticStride =
        getVal(strides[dim - 1], ShapedType::kDynamicStrideOrOffset);

    if (staticStride != 1) return false;
    // The offsets and sizes dont have to be static for all dimensions. When
    // `fullSlices` is true, the offset and sizes can be dynamic. But many
    // cases, the dynamic offset/size value is obtained by computing from
    // another tensor which lives on the device. To avoid host-round tripping
    // enforce that offset/size is also static.
    if (staticSize == ShapedType::kDynamicSize) return false;
    if (staticOffset == ShapedType::kDynamicStrideOrOffset) return false;

    if (fullSlices == false) {
      if (staticSize != 1) return false;
    } else {
      if (!(staticOffset == 0 && staticSize != ShapedType::kDynamicSize &&
            baseShape[dim - 1] != ShapedType::kDynamicSize &&
            staticSize == baseShape[dim - 1])) {
        fullSlices = false;
      }
    }
  }
  return true;
}

/// Returns the `Value`s for a list of `OpFoldResult` by generating std.constant
/// ops for the static values.
static SmallVector<Value, 4> getAsValues(
    OpBuilder &b, Location loc, ArrayRef<OpFoldResult> valueOrAttrList) {
  SmallVector<Value, 4> values;
  for (auto valueOrAttr : valueOrAttrList) {
    if (auto attr = valueOrAttr.dyn_cast<Attribute>()) {
      values.push_back(b.create<arith::ConstantIndexOp>(
          loc, attr.cast<IntegerAttr>().getInt()));
    } else {
      values.push_back(valueOrAttr.get<Value>());
    }
  }
  return values;
}

/// Gets the list of non-static values from a list of `OpFoldResult`.
static SmallVector<Value, 4> getDynamicValues(
    ArrayRef<OpFoldResult> valueOrAttrList) {
  SmallVector<Value, 4> dynamicDims;
  for (auto valueOrAttr : valueOrAttrList) {
    if (auto value = valueOrAttr.dyn_cast<Value>()) {
      dynamicDims.push_back(value);
    }
  }
  return dynamicDims;
}

/// Get shape of the tensor given the sizes as a list of `OpFoldResult`.
static SmallVector<int64_t, 4> getShapeFromSizes(
    ArrayRef<OpFoldResult> valueOrAttrList) {
  return llvm::to_vector<4>(llvm::map_range(
      valueOrAttrList, [&](OpFoldResult valueOrAttr) -> int64_t {
        if (auto attr = valueOrAttr.dyn_cast<Attribute>()) {
          return attr.cast<IntegerAttr>().getInt();
        }
        return ShapedType::kDynamicSize;
      }));
}

/// Generates `memref.dim` operations to get the dynamic sizes of a value `v`.
static SmallVector<Value, 4> getDynamicDimValues(OpBuilder &b, Location loc,
                                                 Value v) {
  SmallVector<Value, 4> dynamicDims;
  for (auto dim : llvm::enumerate(v.getType().cast<ShapedType>().getShape())) {
    if (dim.value() != ShapedType::kDynamicSize) continue;
    dynamicDims.push_back(b.createOrFold<tensor::DimOp>(loc, v, dim.index()));
  }
  return dynamicDims;
}

namespace {

/// Convert tensor.insert_slice ops into flow.tensor.update ops where possible.
struct ConvertTensorInsertSlicePattern
    : public OpRewritePattern<tensor::InsertSliceOp> {
  using OpRewritePattern<tensor::InsertSliceOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(tensor::InsertSliceOp insertOp,
                                PatternRewriter &rewriter) const override {
    if (insertOp->getParentOfType<Flow::DispatchWorkgroupsOp>()) {
      return failure();
    }

    SmallVector<OpFoldResult, 4> offsets = insertOp.getMixedOffsets();
    SmallVector<OpFoldResult, 4> sizes = insertOp.getMixedSizes();
    SmallVector<OpFoldResult, 4> strides = insertOp.getMixedStrides();
    ArrayRef<int64_t> dstShape = insertOp.getType().getShape();
    if (!isOffsetSizeAndStrideMappableToFlow(offsets, sizes, strides,
                                             dstShape)) {
      return failure();
    }

    Location loc = insertOp.getLoc();
    auto sourceDynamicDims = getDynamicValues(sizes);
    Value source = insertOp.getSource();
    ShapedType sourceType = insertOp.getSourceType();
    ShapedType destType = insertOp.getType();

    // Handle rank-reduced version.
    if (sourceType.getRank() < destType.getRank()) {
      // Get the un-rank-reduced shape of the source.
      auto unreducedShape = getShapeFromSizes(sizes);
      sourceType =
          RankedTensorType::get(unreducedShape, sourceType.getElementType());
      source = rewriter.create<IREE::Flow::TensorReshapeOp>(
          loc, sourceType, source, sourceDynamicDims, sourceDynamicDims);
    }

    auto offsetVals = getAsValues(rewriter, loc, insertOp.getMixedOffsets());
    Value dest = insertOp.getDest();
    auto destDynamicDims = getDynamicDimValues(rewriter, loc, dest);
    rewriter.replaceOpWithNewOp<TensorUpdateOp>(
        insertOp, insertOp.getType(), dest, destDynamicDims, offsetVals, source,
        sourceDynamicDims, rewriter.getIndexArrayAttr({0}));
    return success();
  }
};

/// Convert tensor.extract_slice ops into flow.tensor.slice ops where possible.
struct ConvertTensorExtractSlicePattern
    : public OpRewritePattern<tensor::ExtractSliceOp> {
  using OpRewritePattern<tensor::ExtractSliceOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(tensor::ExtractSliceOp sliceOp,
                                PatternRewriter &rewriter) const override {
    if (sliceOp->getParentOfType<Flow::DispatchWorkgroupsOp>()) {
      return failure();
    }

    SmallVector<OpFoldResult, 4> offsets = sliceOp.getMixedOffsets();
    SmallVector<OpFoldResult, 4> sizes = sliceOp.getMixedSizes();
    SmallVector<OpFoldResult, 4> strides = sliceOp.getMixedStrides();
    ArrayRef<int64_t> srcShape = sliceOp.getSourceType().getShape();
    if (!isOffsetSizeAndStrideMappableToFlow(offsets, sizes, strides,
                                             srcShape)) {
      return failure();
    }

    Location loc = sliceOp.getLoc();

    ShapedType sourceType = sliceOp.getSourceType();
    ShapedType resultType = sliceOp.getType();

    // Handle rank reduced version.
    if (resultType.getRank() < sourceType.getRank()) {
      // Get the un-rank-reduced shape of the result.
      auto unreducedShape = getShapeFromSizes(sizes);
      resultType =
          RankedTensorType::get(unreducedShape, sourceType.getElementType());
    }

    auto offsetVals = getAsValues(rewriter, loc, offsets);
    auto sizeVals = getAsValues(rewriter, loc, sizes);
    auto sourceDynamicDims =
        getDynamicDimValues(rewriter, loc, sliceOp.getSource());
    auto resultDynamicDims = getDynamicValues(sizes);
    Value replacement = rewriter.create<TensorSliceOp>(
        loc, resultType, sliceOp.getSource(), sourceDynamicDims, offsetVals,
        sizeVals, resultDynamicDims);
    if (resultType.getRank() > sliceOp.getType().getRank()) {
      replacement = rewriter.create<IREE::Flow::TensorReshapeOp>(
          loc, sliceOp.getType(), replacement, resultDynamicDims,
          resultDynamicDims);
    }
    rewriter.replaceOp(sliceOp, replacement);
    return success();
  }
};

struct ConvertTensorExtractPattern
    : public OpRewritePattern<tensor::ExtractOp> {
  using OpRewritePattern<tensor::ExtractOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tensor::ExtractOp op,
                                PatternRewriter &rewriter) const override {
    if (op->getParentOfType<Flow::DispatchWorkgroupsOp>()) {
      return failure();
    }

    rewriter.replaceOpWithNewOp<IREE::Flow::TensorLoadOp>(
        op, op.getResult().getType(), op.getTensor(), op.getIndices());
    return success();
  }
};

struct ConvertTensorCastPattern : public OpRewritePattern<tensor::CastOp> {
  using OpRewritePattern<tensor::CastOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(tensor::CastOp op,
                                PatternRewriter &rewriter) const override {
    if (op->getParentOfType<Flow::DispatchWorkgroupsOp>()) {
      return failure();
    }

    auto loc = op.getLoc();
    Value input = op.getOperand();
    ShapedType inputType = input.getType().dyn_cast<ShapedType>();
    ShapedType resultType =
        op.getResult().getType().dyn_cast_or_null<ShapedType>();
    if (!inputType || !resultType || !inputType.hasRank() ||
        !resultType.hasRank()) {
      return rewriter.notifyMatchFailure(op, "not ranked shaped types");
    }
    // This should not happen, except in the context of type conversion.
    if (inputType.getRank() != resultType.getRank()) {
      return rewriter.notifyMatchFailure(op, "mismatched rank");
    }

    // Resolve dims to the most specific value.
    int rank = inputType.getRank();
    SmallVector<Value> dimSizes(rank);
    auto resolveDimSize = [&](int position) -> Value {
      if (!dimSizes[position]) {
        // Find the most specific.
        if (!inputType.isDynamicDim(position) ||
            !resultType.isDynamicDim(position)) {
          // Static dim.
          int64_t dimSize = !inputType.isDynamicDim(position)
                                ? inputType.getDimSize(position)
                                : resultType.getDimSize(position);
          dimSizes[position] =
              rewriter.create<arith::ConstantIndexOp>(loc, dimSize);
        } else {
          // Dynamic dim.
          dimSizes[position] =
              rewriter.create<tensor::DimOp>(loc, input, position);
        }
      }

      return dimSizes[position];
    };

    SmallVector<Value> sourceDynamicDims;
    SmallVector<Value> targetDynamicDims;
    for (int i = 0; i < rank; i++) {
      if (inputType.isDynamicDim(i)) {
        sourceDynamicDims.push_back(resolveDimSize(i));
      }
      if (resultType.isDynamicDim(i)) {
        targetDynamicDims.push_back(resolveDimSize(i));
      }
    }

    // TODO: Decide if this needs to be replaced with a flow.tensor.cast
    // See https://github.com/google/iree/issues/6418
    rewriter.replaceOpWithNewOp<IREE::Flow::TensorReshapeOp>(
        op, resultType, input, sourceDynamicDims, targetDynamicDims);

    return success();
  }
};

struct ConvertTensorFromElementsPattern
    : public OpRewritePattern<tensor::FromElementsOp> {
  using OpRewritePattern<tensor::FromElementsOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(tensor::FromElementsOp op,
                                PatternRewriter &rewriter) const override {
    // TODO: This pattern was mainly added to iron out some kinks specific to
    // detensoring (see: https://github.com/google/iree/issues/1159). Do we need
    // to expand this check for other uses?
    if (op->getParentOfType<Flow::DispatchWorkgroupsOp>()) {
      return failure();
    }
    auto tensorType = op.getType();
    if (!tensorType.hasRank()) {
      return failure();
    }

    // Check that all the dimensions are 1.
    if (!llvm::all_of(tensorType.getShape(),
                      [](int64_t dim) { return dim == 1; })) {
      return failure();
    }

    rewriter.replaceOpWithNewOp<IREE::Flow::TensorSplatOp>(
        op, tensorType, op.getOperand(0), ValueRange());
    return success();
  }
};

/// Converts linalg.tensor_reshape operations into flow.tensor.reshape
/// operations.
template <typename TensorReshapeOp>
struct ConvertTensorReshapePattern : public OpRewritePattern<TensorReshapeOp> {
  using OpRewritePattern<TensorReshapeOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(TensorReshapeOp reshapeOp,
                                PatternRewriter &rewriter) const override {
    if (reshapeOp->template getParentOfType<Flow::DispatchWorkgroupsOp>()) {
      return failure();
    }
    SmallVector<SmallVector<Value>> outputShape;
    ReifyRankedShapedTypeOpInterface reifyShapedTypeInterface =
        cast<ReifyRankedShapedTypeOpInterface>(reshapeOp.getOperation());
    if (failed(reifyShapedTypeInterface.reifyResultShapes(rewriter,
                                                          outputShape))) {
      return failure();
    }
    SmallVector<Value> outputDynamicShapes;
    for (auto shape :
         llvm::zip(reshapeOp.getResultType().getShape(), outputShape[0])) {
      if (std::get<0>(shape) != ShapedType::kDynamicSize) continue;
      outputDynamicShapes.push_back(std::get<1>(shape));
    }
    rewriter.replaceOpWithNewOp<IREE::Flow::TensorReshapeOp>(
        reshapeOp, reshapeOp.getResultType(), reshapeOp.getSrc(),
        outputDynamicShapes);
    return success();
  }
};

/// Converts linalg.fill ops into flow.tensor.splat ops.
///
/// This is expected to improve performance because we can use DMA
/// functionalities for the fill, instead of dispatching kernels.
struct ConvertLinalgFillPattern final
    : public OpRewritePattern<linalg::FillOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(linalg::FillOp fillOp,
                                PatternRewriter &rewriter) const override {
    if (fillOp->getParentOfType<IREE::Flow::DispatchWorkgroupsOp>()) {
      // Don't convert linalg.fill ops that were fused together with other ops.
      return failure();
    }

    SmallVector<Value, 4> dynamicDims =
        getDynamicDimValues(rewriter, fillOp.getLoc(), fillOp.output());
    rewriter.replaceOpWithNewOp<TensorSplatOp>(
        fillOp, fillOp.output().getType(), fillOp.value(), dynamicDims);
    return success();
  }
};

}  // namespace

void populateTensorToFlowConversionPatterns(MLIRContext *context,
                                            RewritePatternSet &patterns) {
  patterns
      .insert<ConvertLinalgFillPattern, ConvertTensorCastPattern,
              ConvertTensorExtractPattern, ConvertTensorExtractSlicePattern,
              ConvertTensorInsertSlicePattern, ConvertTensorFromElementsPattern,
              ConvertTensorReshapePattern<tensor::CollapseShapeOp>,
              ConvertTensorReshapePattern<tensor::ExpandShapeOp>>(context);
}

}  // namespace Flow
}  // namespace IREE
}  // namespace iree_compiler
}  // namespace mlir
