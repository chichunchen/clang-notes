// This file translates the following C code to LLVM IR without using Clang
// codegen wrapper, however, this implementation has been abandoned due to the
// dependence of malloc:
//
//   struct descriptors {
//     int64_t offset;
//     int64_t count;
//     int64_t stride;
//   };
// 
//   void foo(void *D) {
//     descriptors **DD = (descriptors **)D;
//     for (int i = 0; i < 3; i++) {
//       for (int j = 0; j < 5; j++) {
//         printf("DD[%d][%d] = %d\n", i, j, DD[i][j].offset);
//       }
//     }
//   }
// 
// #define N 1
// #define M 3
// 
//   int main() {
//     descriptors **D = (descriptors **)malloc(N * sizeof(descriptors *));
//     for (int i = 0; i < N; i++) {
//       D[i] = (descriptors *)malloc(M * sizeof(descriptors));
//       for (int j = 0; j < M; j++) {
//         D[i][j].offset = i + j;
//         D[i][j].count = i + j;
//         D[i][j].stride = i + j;
//       }
//     }
//     foo(D);
//   }

// Build an array of struct descriptor_dim and then assign it to
// offload_args
if (Info.NumberOfPtrs) {
  // Build struct descriptor_dim {
  //  int64_t offset;
  //  int64_t count;
  //  int64_t stride
  // };
  llvm::Type *FieldTypes[] = {
      CGM.Int64Ty, // Offset
      CGM.Int64Ty, // Count
      CGM.Int64Ty  // Stride
  };
  llvm::StructType *DescriptorDim = llvm::StructType::create(
      CGM.getLLVMContext(), FieldTypes, "descriptor_dim");
  // descriptor_dim **D =
  //  (descriptor_dim**) malloc(NumOfPointers * (VoidPtrTy size));
  llvm::Type *DescriptorTy = DescriptorDim->getPointerTo()->getPointerTo();
  llvm::AllocaInst *DescriptorInst =
      CGF.Builder.CreateAlloca(DescriptorTy, nullptr, "D");
  unsigned PointerAlignment = CGM.getPointerSize().getQuantity();
  DescriptorInst->setAlignment(llvm::MaybeAlign(PointerAlignment));
  llvm::Value *DescriptorSize = CGF.Builder.CreateMul(
      llvm::ConstantInt::get(CGF.SizeTy, PointerAlignment),
      llvm::ConstantInt::get(CGF.SizeTy, Info.NumberOfPtrs));
  llvm::Type *TypeParams[] = {CGM.Int64Ty};
  auto *MallocFnTy =
      llvm::FunctionType::get(CGM.Int8PtrTy, TypeParams, /*isVarArg*/ false);
  // TODO alloc size attributes
  llvm::FunctionCallee RTLFn = CGM.CreateRuntimeFunction(MallocFnTy, "malloc");
  llvm::CallInst *CInst = CGF.EmitRuntimeCall(RTLFn, DescriptorSize);
  llvm::Value *CCast = CGF.Builder.CreateBitCast(CInst, DescriptorTy);
  CGF.Builder.CreateAlignedStore(CCast, DescriptorInst,
                                 llvm::MaybeAlign(PointerAlignment),
                                 /*IsVolatile*/ false);

  llvm::Value *IVal = CGF.Builder.CreateAlloca(CGM.Int32Ty, nullptr, "I");
  for (unsigned I = 0, E = Info.NumberOfPtrs; I < E; ++I) {
    // Dim being zero to indicate that this base is contiguous
    if (Dims[I] == 0)
      continue;
    // D[i] = (descriptors*) malloc(DimSize * (sizeof(descriptors)));
    llvm::MaybeAlign IntAlign =
        llvm::MaybeAlign(CGM.getIntAlign().getQuantity());
    CGF.Builder.CreateAlignedStore(llvm::ConstantInt::get(CGF.Int32Ty, I), IVal,
                                   IntAlign,
                                   /*IsVolatile*/ false);
    llvm::Value *DimSize = CGF.Builder.CreateMul(
        llvm::ConstantInt::get(CGF.SizeTy, CGM.getIntAlign().getQuantity() * 3),
        llvm::ConstantInt::get(CGF.SizeTy, Dims[I]));
    CInst = CGF.EmitRuntimeCall(RTLFn, DimSize);
    CCast = CGF.Builder.CreateBitCast(CInst, DescriptorDim->getPointerTo());
    llvm::LoadInst *LI = CGF.Builder.CreateAlignedLoad(
        DescriptorInst, llvm::MaybeAlign(PointerAlignment));
    llvm::Value *Arg = CGF.Builder.CreateAlignedLoad(IVal, IntAlign);
    llvm::Value *NumberOfPtrsIdx =
        CGF.Builder.CreateInBoundsGEP(LI, Arg, "numbers_idx");
    CGF.Builder.CreateAlignedStore(CCast, NumberOfPtrsIdx,
                                   llvm::MaybeAlign(PointerAlignment),
                                   /*IsVolatile*/ false);
    llvm::LoadInst *LD = CGF.Builder.CreateAlignedLoad(
        NumberOfPtrsIdx, llvm::MaybeAlign(PointerAlignment));

    enum { OffsetFD = 0, CountFD, StrideFD };
    llvm::Value *IIVal = CGF.Builder.CreateAlloca(CGM.Int32Ty, nullptr, "II");
    // Fill Descriptor with data
    for (unsigned II = 0, EE = Dims[I]; II < EE; ++II) {
      CGF.Builder.CreateAlignedStore(llvm::ConstantInt::get(CGF.Int32Ty, II),
                                     IIVal, IntAlign,
                                     /*IsVolatile*/ false);
      Arg = CGF.Builder.CreateAlignedLoad(IIVal, IntAlign);
      llvm::Value *DimsIdx = CGF.Builder.CreateInBoundsGEP(LD, Arg, "dims_idx");
      // Offset
      llvm::Value *OffsetArgs[] = {
          llvm::ConstantInt::get(CGF.Int32Ty, 0),
          llvm::ConstantInt::get(CGF.Int32Ty, OffsetFD)};
      llvm::Value *OffsetVal =
          CGF.Builder.CreateInBoundsGEP(DimsIdx, OffsetArgs, "offset");
      CGF.Builder.CreateAlignedStore(Offsets[I][II], OffsetVal,
                                     llvm::MaybeAlign(8));
      // Count
      llvm::Value *CountArgs[] = {llvm::ConstantInt::get(CGF.Int32Ty, 0),
                                  llvm::ConstantInt::get(CGF.Int32Ty, CountFD)};
      llvm::Value *CountVal =
          CGF.Builder.CreateInBoundsGEP(DimsIdx, CountArgs, "count");
      CGF.Builder.CreateAlignedStore(Counts[I][II], CountVal,
                                     llvm::MaybeAlign(8));
      // Stride
      llvm::Value *StrideArgs[] = {
          llvm::ConstantInt::get(CGF.Int32Ty, 0),
          llvm::ConstantInt::get(CGF.Int32Ty, StrideFD)};
      llvm::Value *StrideVal =
          CGF.Builder.CreateInBoundsGEP(DimsIdx, StrideArgs, "stride");
      CGF.Builder.CreateAlignedStore(Strides[I][II], StrideVal,
                                     llvm::MaybeAlign(8));
    }
    // Cast and store the descriptor to Info.PointersArray[I]
    LD = CGF.Builder.CreateAlignedLoad(DescriptorInst,
                                       llvm::MaybeAlign(PointerAlignment));
    CCast = CGF.Builder.CreateBitCast(LD, CGF.Int8PtrTy);
    llvm::Value *P = CGF.Builder.CreateConstInBoundsGEP2_32(
        llvm::ArrayType::get(CGM.VoidPtrTy, Info.NumberOfPtrs),
        Info.PointersArray, 0, I);
    CGF.Builder.CreateAlignedStore(CCast, P,
                                   llvm::MaybeAlign(PointerAlignment));
  }
}
