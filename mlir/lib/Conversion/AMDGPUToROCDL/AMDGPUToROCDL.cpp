//===- AMDGPUToROCDL.cpp - AMDGPU to ROCDL dialect conversion -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "mlir/Conversion/AMDGPUToROCDL/AMDGPUToROCDL.h"

#include "mlir/Conversion/LLVMCommon/ConversionTarget.h"
#include "mlir/Conversion/LLVMCommon/Pattern.h"
#include "mlir/Dialect/AMDGPU/AMDGPUDialect.h"
#include "mlir/Dialect/LLVMIR/ROCDLDialect.h"
#include "mlir/Pass/Pass.h"

namespace mlir {
#define GEN_PASS_DEF_CONVERTAMDGPUTOROCDL
#include "mlir/Conversion/Passes.h.inc"
} // namespace mlir

using namespace mlir;
using namespace mlir::amdgpu;

static Value createI32Constant(ConversionPatternRewriter &rewriter,
                               Location loc, int32_t value) {
  Type llvmI32 = rewriter.getI32Type();
  return rewriter.create<LLVM::ConstantOp>(loc, llvmI32, value);
}

namespace {
/// Define lowering patterns for raw buffer ops
template <typename GpuOp, typename Intrinsic>
struct RawBufferOpLowering : public ConvertOpToLLVMPattern<GpuOp> {
  RawBufferOpLowering(LLVMTypeConverter &converter, Chipset chipset)
      : ConvertOpToLLVMPattern<GpuOp>(converter), chipset(chipset) {}

  Chipset chipset;
  static constexpr uint32_t maxVectorOpWidth = 128;

  LogicalResult
  matchAndRewrite(GpuOp gpuOp, typename GpuOp::Adaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = gpuOp.getLoc();
    Value memref = adaptor.getMemref();
    Value unconvertedMemref = gpuOp.getMemref();
    MemRefType memrefType = unconvertedMemref.getType().cast<MemRefType>();

    if (chipset.majorVersion < 9)
      return gpuOp.emitOpError("Raw buffer ops require GCN or higher");

    Value storeData = adaptor.getODSOperands(0)[0];
    if (storeData == memref) // no write component to this op
      storeData = Value();
    Type wantedDataType;
    if (storeData)
      wantedDataType = storeData.getType();
    else
      wantedDataType = gpuOp.getODSResults(0)[0].getType();

    Type llvmWantedDataType = this->typeConverter->convertType(wantedDataType);

    Type i32 = rewriter.getI32Type();
    Type llvmI32 = this->typeConverter->convertType(i32);

    int64_t elementByteWidth = memrefType.getElementTypeBitWidth() / 8;
    Value byteWidthConst = createI32Constant(rewriter, loc, elementByteWidth);

    // If we want to load a vector<NxT> with total size <= 32
    // bits, use a scalar load and bitcast it. Similarly, if bitsize(T) < 32
    // and the total load size is >= 32, use a vector load of N / (bitsize(T) /
    // 32) x i32 and bitcast.
    Type llvmBufferValType = llvmWantedDataType;
    if (auto dataVector = wantedDataType.dyn_cast<VectorType>()) {
      uint32_t elemBits = dataVector.getElementTypeBitWidth();
      uint32_t totalBits = elemBits * dataVector.getNumElements();
      if (totalBits > maxVectorOpWidth)
        return gpuOp.emitOpError(
            "Total width of loads or stores must be no more than " +
            Twine(maxVectorOpWidth) + " bits, but we call for " +
            Twine(totalBits) +
            " bits. This should've been caught in validation");
      if (elemBits < 32) {
        if (totalBits > 32) {
          if (totalBits % 32 != 0)
            return gpuOp.emitOpError("Load or store of more than 32-bits that "
                                     "doesn't fit into words. Can't happen\n");
          llvmBufferValType = this->typeConverter->convertType(
              VectorType::get(totalBits / 32, i32));
        } else {
          llvmBufferValType = this->typeConverter->convertType(
              rewriter.getIntegerType(totalBits));
        }
      }
    }

    SmallVector<Value, 6> args;
    if (storeData) {
      if (llvmBufferValType != llvmWantedDataType) {
        Value castForStore =
            rewriter.create<LLVM::BitcastOp>(loc, llvmBufferValType, storeData);
        args.push_back(castForStore);
      } else {
        args.push_back(storeData);
      }
    }

    // Construct buffer descriptor from memref, attributes
    int64_t offset = 0;
    SmallVector<int64_t, 5> strides;
    if (failed(getStridesAndOffset(memrefType, strides, offset)))
      return gpuOp.emitOpError("Can't lower non-stride-offset memrefs");

    // Resource descriptor
    // bits 0-47: base address
    // bits 48-61: stride (0 for raw buffers)
    // bit 62: texture cache coherency (always 0)
    // bit 63: enable swizzles (always off for raw buffers)
    // bits 64-95 (word 2): Number of records, units of stride
    // bits 96-127 (word 3): See below

    Type llvm4xI32 = this->typeConverter->convertType(VectorType::get(4, i32));
    MemRefDescriptor memrefDescriptor(memref);
    Type llvmI64 = this->typeConverter->convertType(rewriter.getI64Type());
    Type llvm2xI32 = this->typeConverter->convertType(VectorType::get(2, i32));
    Value c32I64 = rewriter.create<LLVM::ConstantOp>(loc, llvmI64, 32);

    Value resource = rewriter.create<LLVM::UndefOp>(loc, llvm4xI32);

    Value ptr = memrefDescriptor.alignedPtr(rewriter, loc);
    Value ptrAsInt = rewriter.create<LLVM::PtrToIntOp>(loc, llvmI64, ptr);
    Value lowHalf = rewriter.create<LLVM::TruncOp>(loc, llvmI32, ptrAsInt);
    resource = rewriter.create<LLVM::InsertElementOp>(
        loc, llvm4xI32, resource, lowHalf,
        this->createIndexConstant(rewriter, loc, 0));

    // Bits 48-63 are used both for the stride of the buffer and (on gfx10) for
    // enabling swizzling. Prevent the high bits of pointers from accidentally
    // setting those flags.
    Value highHalfShifted = rewriter.create<LLVM::TruncOp>(
        loc, llvmI32, rewriter.create<LLVM::LShrOp>(loc, ptrAsInt, c32I64));
    Value highHalfTruncated = rewriter.create<LLVM::AndOp>(
        loc, llvmI32, highHalfShifted,
        createI32Constant(rewriter, loc, 0x0000ffff));
    resource = rewriter.create<LLVM::InsertElementOp>(
        loc, llvm4xI32, resource, highHalfTruncated,
        this->createIndexConstant(rewriter, loc, 1));

    Value numRecords;
    if (memrefType.hasStaticShape()) {
      numRecords = createI32Constant(
          rewriter, loc,
          static_cast<int32_t>(memrefType.getNumElements() * elementByteWidth));
    } else {
      Value maxIndex;
      for (uint32_t i = 0, e = memrefType.getRank(); i < e; ++i) {
        Value size = memrefDescriptor.size(rewriter, loc, i);
        Value stride = memrefDescriptor.stride(rewriter, loc, i);
        stride = rewriter.create<LLVM::MulOp>(loc, stride, byteWidthConst);
        Value maxThisDim = rewriter.create<LLVM::MulOp>(loc, size, stride);
        maxIndex = maxIndex ? rewriter.create<LLVM::MaximumOp>(loc, maxIndex,
                                                               maxThisDim)
                            : maxThisDim;
      }
      numRecords = rewriter.create<LLVM::TruncOp>(loc, llvmI32, maxIndex);
    }
    resource = rewriter.create<LLVM::InsertElementOp>(
        loc, llvm4xI32, resource, numRecords,
        this->createIndexConstant(rewriter, loc, 2));

    // Final word:
    // bits 0-11: dst sel, ignored by these intrinsics
    // bits 12-14: data format (ignored, must be nonzero, 7=float)
    // bits 15-18: data format (ignored, must be nonzero, 4=32bit)
    // bit 19: In nested heap (0 here)
    // bit 20: Behavior on unmap (0 means  "return 0 / ignore")
    // bits 21-22: Index stride for swizzles (N/A)
    // bit 23: Add thread ID (0)
    // bit 24: Reserved to 1 (RDNA) or 0 (CDNA)
    // bits 25-26: Reserved (0)
    // bit 27: Buffer is non-volatile (CDNA only)
    // bits 28-29: Out of bounds select (0 = structured, 1 = check index, 2 =
    //  none, 3 = either swizzles or testing against offset field) RDNA only
    // bits 30-31: Type (must be 0)
    uint32_t word3 = (7 << 12) | (4 << 15);
    if (chipset.majorVersion == 10) {
      word3 |= (1 << 24);
      uint32_t oob = adaptor.getBoundsCheck() ? 3 : 2;
      word3 |= (oob << 28);
    }
    Value word3Const = createI32Constant(rewriter, loc, word3);
    resource = rewriter.create<LLVM::InsertElementOp>(
        loc, llvm4xI32, resource, word3Const,
        this->createIndexConstant(rewriter, loc, 3));
    args.push_back(resource);

    // Indexing (voffset)
    Value voffset;
    for (auto &pair : llvm::enumerate(adaptor.getIndices())) {
      size_t i = pair.index();
      Value index = pair.value();
      Value strideOp;
      if (ShapedType::isDynamicStrideOrOffset(strides[i])) {
        strideOp = rewriter.create<LLVM::MulOp>(
            loc, memrefDescriptor.stride(rewriter, loc, i), byteWidthConst);
      } else {
        strideOp =
            createI32Constant(rewriter, loc, strides[i] * elementByteWidth);
      }
      index = rewriter.create<LLVM::MulOp>(loc, index, strideOp);
      voffset =
          voffset ? rewriter.create<LLVM::AddOp>(loc, voffset, index) : index;
    }
    if (adaptor.getIndexOffset()) {
      int32_t indexOffset = *gpuOp.getIndexOffset() * elementByteWidth;
      Value extraOffsetConst = createI32Constant(rewriter, loc, indexOffset);
      voffset =
          voffset ? rewriter.create<LLVM::AddOp>(loc, voffset, extraOffsetConst)
                  : extraOffsetConst;
    }
    args.push_back(voffset);

    Value sgprOffset = adaptor.getSgprOffset();
    if (!sgprOffset)
      sgprOffset = createI32Constant(rewriter, loc, 0);
    if (ShapedType::isDynamicStrideOrOffset(offset))
      sgprOffset = rewriter.create<LLVM::AddOp>(
          loc, memrefDescriptor.offset(rewriter, loc), sgprOffset);
    else if (offset > 0)
      sgprOffset = rewriter.create<LLVM::AddOp>(
          loc, sgprOffset, createI32Constant(rewriter, loc, offset));
    args.push_back(sgprOffset);

    // bit 0: GLC = 0 (atomics drop value, less coherency)
    // bits 1-2: SLC, DLC = 0 (similarly)
    // bit 3: swizzled (0 for raw)
    args.push_back(createI32Constant(rewriter, loc, 0));

    llvm::SmallVector<Type, 1> resultTypes(gpuOp->getNumResults(),
                                           llvmBufferValType);
    Operation *lowered = rewriter.create<Intrinsic>(loc, resultTypes, args,
                                                    ArrayRef<NamedAttribute>());
    if (lowered->getNumResults() == 1) {
      Value replacement = lowered->getResult(0);
      if (llvmBufferValType != llvmWantedDataType) {
        replacement = rewriter.create<LLVM::BitcastOp>(loc, llvmWantedDataType,
                                                       replacement);
      }
      rewriter.replaceOp(gpuOp, replacement);
    } else {
      rewriter.eraseOp(gpuOp);
    }
    return success();
  }
};

struct LDSBarrierOpLowering : public ConvertOpToLLVMPattern<LDSBarrierOp> {
  using ConvertOpToLLVMPattern<LDSBarrierOp>::ConvertOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(LDSBarrierOp op, LDSBarrierOp::Adaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto asmDialectAttr = LLVM::AsmDialectAttr::get(rewriter.getContext(),
                                                    LLVM::AsmDialect::AD_ATT);
    const char *asmStr = "s_waitcnt lgkmcnt(0)\ns_barrier";
    const char *constraints = "";
    rewriter.replaceOpWithNewOp<LLVM::InlineAsmOp>(
        op,
        /*resultTypes=*/TypeRange(), /*operands=*/ValueRange(),
        /*asm_string=*/asmStr, constraints, /*has_side_effects=*/true,
        /*is_align_stack=*/false, /*asm_dialect=*/asmDialectAttr,
        /*operand_attrs=*/ArrayAttr());
    return success();
  }
};

struct ConvertAMDGPUToROCDLPass
    : public impl::ConvertAMDGPUToROCDLBase<ConvertAMDGPUToROCDLPass> {
  ConvertAMDGPUToROCDLPass() = default;

  void runOnOperation() override {
    MLIRContext *ctx = &getContext();
    FailureOr<Chipset> maybeChipset = Chipset::parse(chipset);
    if (failed(maybeChipset)) {
      emitError(UnknownLoc::get(ctx), "Invalid chipset name: " + chipset);
      return signalPassFailure();
    }

    RewritePatternSet patterns(ctx);
    LLVMTypeConverter converter(ctx);
    populateAMDGPUToROCDLConversionPatterns(converter, patterns, *maybeChipset);
    LLVMConversionTarget target(getContext());
    target.addLegalDialect<::mlir::LLVM::LLVMDialect>();
    target.addLegalDialect<::mlir::ROCDL::ROCDLDialect>();
    if (failed(applyPartialConversion(getOperation(), target,
                                      std::move(patterns))))
      signalPassFailure();
  }
};
} // namespace

void mlir::populateAMDGPUToROCDLConversionPatterns(LLVMTypeConverter &converter,
                                                   RewritePatternSet &patterns,
                                                   Chipset chipset) {
  patterns.add<LDSBarrierOpLowering>(converter);
  patterns.add<
      RawBufferOpLowering<RawBufferLoadOp, ROCDL::RawBufferLoadOp>,
      RawBufferOpLowering<RawBufferStoreOp, ROCDL::RawBufferStoreOp>,
      RawBufferOpLowering<RawBufferAtomicFaddOp, ROCDL::RawBufferAtomicFAddOp>>(
      converter, chipset);
}

std::unique_ptr<Pass> mlir::createConvertAMDGPUToROCDLPass() {
  return std::make_unique<ConvertAMDGPUToROCDLPass>();
}
