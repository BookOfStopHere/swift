//===--- SimplifyInstruction.cpp - Fold instructions ----------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-simplify"
#include "swift/SILPasses/Utils/Local.h"
#include "swift/SIL/PatternMatch.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILVisitor.h"

using namespace swift;
using namespace swift::PatternMatch;

namespace swift {
  class ASTContext;
}

namespace {
  class InstSimplifier : public SILInstructionVisitor<InstSimplifier, SILValue>{
  public:
    SILValue visitSILInstruction(SILInstruction *I) { return SILValue(); }

    SILValue visitTupleExtractInst(TupleExtractInst *TEI);
    SILValue visitStructExtractInst(StructExtractInst *SEI);
    SILValue visitEnumInst(EnumInst *EI);
    SILValue visitUncheckedEnumDataInst(UncheckedEnumDataInst *UEDI);
    SILValue visitAddressToPointerInst(AddressToPointerInst *ATPI);
    SILValue visitPointerToAddressInst(PointerToAddressInst *PTAI);
    SILValue visitRefToRawPointerInst(RefToRawPointerInst *RRPI);
    SILValue
    visitUnconditionalCheckedCastInst(UnconditionalCheckedCastInst *UCCI);
    SILValue visitUncheckedRefCastInst(UncheckedRefCastInst *OPRI);
    SILValue visitUncheckedAddrCastInst(UncheckedAddrCastInst *UACI);
    SILValue visitStructInst(StructInst *SI);
    SILValue visitTupleInst(TupleInst *SI);
    SILValue visitApplyInst(ApplyInst *AI);
    SILValue visitUpcastInst(UpcastInst *UI);

    SILValue simplifyOverflowBuiltin(ApplyInst *AI,
                                     BuiltinFunctionRefInst *FR);
  };
} // end anonymous namespace

SILValue InstSimplifier::visitStructInst(StructInst *SI) {
  // Ignore empty structs.
  if (SI->getNumOperands() < 1)
    return SILValue();

  // Optimize structs that are generated from struct_extract instructions
  // from the same struct.
  if (auto *Ex0 = dyn_cast<StructExtractInst>(SI->getOperand(0))) {
    // Check that the constructed struct and the extracted struct are of the
    // same type.
    if (SI->getType() != Ex0->getOperand().getType())
      return SILValue();

    // Check that all of the operands are extracts of the correct kind.
    for (unsigned i = 0, e = SI->getNumOperands(); i < e; i++) {
      auto *Ex = dyn_cast<StructExtractInst>(SI->getOperand(i));
      // Must be an extract.
      if (!Ex)
        return SILValue();

      // Extract from the same struct as the first extract_inst.
      if (Ex0->getOperand() != Ex->getOperand())
        return SILValue();

      // And the order of the field must be identical to the construction order.
      if (Ex->getFieldNo() != i)
        return SILValue();
    }

    return Ex0->getOperand();
  }
  
  return SILValue();
}

SILValue InstSimplifier::visitTupleInst(TupleInst *TI) {
  // Ignore empty tuples.
  if (TI->getNumOperands() < 1)
    return SILValue();

  // Optimize tuples that are generated from tuple_extract instructions
  // from the same tuple.
  if (auto *Ex0 = dyn_cast<TupleExtractInst>(TI->getOperand(0))) {
    // Check that the constructed tuple and the extracted tuple are of the
    // same type.
    if (TI->getType() != Ex0->getOperand().getType())
      return SILValue();

    // Check that all of the operands are extracts of the correct kind.
    for (unsigned i = 0, e = TI->getNumOperands(); i < e; i++) {
      auto *Ex = dyn_cast<TupleExtractInst>(TI->getOperand(i));
      // Must be an extract.
      if (!Ex)
        return SILValue();

      // Extract from the same struct as the first extract_inst.
      if (Ex0->getOperand() != Ex->getOperand())
        return SILValue();

      // And the order of the field must be identical to the construction order.
      if (Ex->getFieldNo() != i)
        return SILValue();
    }

    return Ex0->getOperand();
  }

  return SILValue();
}

SILValue InstSimplifier::visitTupleExtractInst(TupleExtractInst *TEI) {
  // tuple_extract(tuple(x, y), 0) -> x
  if (TupleInst *TheTuple = dyn_cast<TupleInst>(TEI->getOperand()))
    return TheTuple->getElements()[TEI->getFieldNo()];

  // tuple_extract(apply([add|sub|...]overflow(x, 0)) -> x
  if (TEI->getFieldNo() == 0)
    if (ApplyInst *AI = dyn_cast<ApplyInst>(TEI->getOperand()))
      if (auto *BFRI = dyn_cast<BuiltinFunctionRefInst>(AI->getCallee()))
        return simplifyOverflowBuiltin(AI, BFRI);

  return SILValue();
}

SILValue InstSimplifier::visitStructExtractInst(StructExtractInst *SEI) {
  // struct_extract(struct(x, y), x) -> x
  if (StructInst *Struct = dyn_cast<StructInst>(SEI->getOperand()))
    return Struct->getFieldValue(SEI->getField());
  
  return SILValue();
}

SILValue
InstSimplifier::
visitUncheckedEnumDataInst(UncheckedEnumDataInst *UEDI) {
  // (unchecked_enum_data (enum payload)) -> payload
  if (EnumInst *EI = dyn_cast<EnumInst>(UEDI->getOperand())) {
    if (EI->getElement() != UEDI->getElement())
      return SILValue();

    assert(EI->hasOperand() &&
           "Should only get data from an enum with payload.");
    return EI->getOperand();
  }

  return SILValue();
}

SILValue InstSimplifier::visitEnumInst(EnumInst *EI) {
  // Simplify enum insts to the value from a switch_enum when possible, e.g.
  // for
  //   switch_enum %0 : $Bool, case #Bool.true!enumelt: bb1
  // bb1:
  //   %1 = enum $Bool, #Bool.true!enumelt
  //
  // we'll return %0
  if (EI->hasOperand())
    return SILValue();

  auto *BB = EI->getParent();
  auto *Pred = BB->getSinglePredecessor();
  if (!Pred)
    return SILValue();

  if (auto *SEI = dyn_cast<SwitchEnumInst>(Pred->getTerminator())) {
    if (EI->getType() != SEI->getOperand().getType())
      return SILValue();

    if (BB == SEI->getCaseDestination(EI->getElement()))
      return SEI->getOperand();
  }

  return SILValue();
}

SILValue InstSimplifier::visitAddressToPointerInst(AddressToPointerInst *ATPI) {
  // (address_to_pointer (pointer_to_address x)) -> x
  if (auto *PTAI = dyn_cast<PointerToAddressInst>(ATPI->getOperand()))
    if (PTAI->getType() == ATPI->getOperand().getType())
      return PTAI->getOperand();

  return SILValue();
}

SILValue InstSimplifier::visitPointerToAddressInst(PointerToAddressInst *PTAI) {
  // (pointer_to_address (address_to_pointer x)) -> x
  if (auto *ATPI = dyn_cast<AddressToPointerInst>(PTAI->getOperand()))
    if (ATPI->getOperand().getType() == PTAI->getType())
      return ATPI->getOperand();

  return SILValue();
}

SILValue InstSimplifier::visitRefToRawPointerInst(RefToRawPointerInst *RefToRaw) {
  // Perform the following simplification:
  //
  // (ref_to_raw_pointer (raw_pointer_to_ref x)) -> x
  //
  // *NOTE* We don't need to check types here.
  if (auto *RawToRef = dyn_cast<RawPointerToRefInst>(&*RefToRaw->getOperand()))
    return RawToRef->getOperand();

  return SILValue();
}

SILValue
InstSimplifier::
visitUnconditionalCheckedCastInst(UnconditionalCheckedCastInst *UCCI) {
  // (UCCI downcast (upcast x #type1 to #type2) #type2 to #type1) -> x
  if (UCCI->getCastKind() == CheckedCastKind::Downcast)
    if (auto *Upcast = dyn_cast<UpcastInst>(UCCI->getOperand()))
      if (UCCI->getOperand().getType() == Upcast->getType() &&
          UCCI->getType() == Upcast->getOperand().getType())
      return Upcast->getOperand();

  return SILValue();
}

SILValue
InstSimplifier::
visitUncheckedRefCastInst(UncheckedRefCastInst *OPRI) {
  // (unchecked-ref-cast Y->X (unchecked-ref-cast x X->Y)) -> x
  if (auto *ROPI = dyn_cast<UncheckedRefCastInst>(&*OPRI->getOperand()))
    if (ROPI->getOperand().getType() == OPRI->getType())
      return ROPI->getOperand();

  // (unchecked-ref-cast Y->X (upcast x X->Y)) -> x
  if (auto *UI = dyn_cast<UpcastInst>(OPRI->getOperand()))
    if (UI->getOperand().getType() == OPRI->getType())
      return UI->getOperand();

  // (unchecked-ref-cast X->X x) -> x
  if (OPRI->getOperand().getType() == OPRI->getType())
    return OPRI->getOperand();

  return SILValue();
}

SILValue
InstSimplifier::
visitUncheckedAddrCastInst(UncheckedAddrCastInst *UACI) {
  // (unchecked-addr-cast Y->X (unchecked-addr-cast x X->Y)) -> x
  if (auto *OtherUACI = dyn_cast<UncheckedAddrCastInst>(&*UACI->getOperand()))
    if (OtherUACI->getOperand().getType() == UACI->getType())
      return OtherUACI->getOperand();

  // (unchecked-addr-cast X->X x) -> x
  if (UACI->getOperand().getType() == UACI->getType())
    return UACI->getOperand();

  return SILValue();
}

SILValue InstSimplifier::visitUpcastInst(UpcastInst *UI) {
  // (upcast Y->X (unchecked-ref-cast x X->Y)) -> x
  if (auto *URCI = dyn_cast<UncheckedRefCastInst>(UI->getOperand()))
    if (URCI->getOperand().getType() == UI->getType())
      return URCI->getOperand();

  return SILValue();
}

/// Simplify an apply of the builtin canBeClass to either 0 or 1
/// when we can statically determine the result.
SILValue InstSimplifier::visitApplyInst(ApplyInst *AI) {
  auto *BFRI = dyn_cast<BuiltinFunctionRefInst>(AI->getCallee());
  if (!BFRI)
    return SILValue();

  // If we have an expect optimizer hint with a constant value input, there is
  // nothing left to expect so propagate the input, i.e.,
  //
  // apply(expect, constant, _) -> constant.
  if (BFRI->getIntrinsicInfo().ID == llvm::Intrinsic::expect)
    if (auto *Literal = dyn_cast<IntegerLiteralInst>(AI->getArgument(0)))
      return Literal;

  return SILValue();
}

/// \brief Simplify arithmetic intrinsics with overflow and known identity
/// constants such as 0 and 1.
/// If this returns a value other than SILValue() then the instruction was
/// simplified to a value which doesn't overflow.  The overflow case is handled
/// in SILCombine.
static SILValue simplifyBinaryWithOverflow(ApplyInst *AI,
                                           llvm::Intrinsic::ID ID) {
  OperandValueArrayRef Args = AI->getArguments();
  assert(Args.size() >= 2);

  const SILValue &Op1 = Args[0];
  const SILValue &Op2 = Args[1];

  IntegerLiteralInst *IntOp1 = dyn_cast<IntegerLiteralInst>(Op1);
  IntegerLiteralInst *IntOp2 = dyn_cast<IntegerLiteralInst>(Op2);

  // If both ops are not constants, we cannot do anything.
  // FIXME: Add cases where we can do something, eg, (x - x) -> 0
  if (!IntOp1 && !IntOp2)
    return SILValue();

  // Calculate the result.

  switch (ID) {
    default: llvm_unreachable("Invalid case");
    case llvm::Intrinsic::sadd_with_overflow:
    case llvm::Intrinsic::uadd_with_overflow:
      // 0 + X -> X
      if (match(Op1, m_Zero()))
        return Op2;
      // X + 0 -> X
      if (match(Op2, m_Zero()))
        return Op1;
      return SILValue();
    case llvm::Intrinsic::ssub_with_overflow:
    case llvm::Intrinsic::usub_with_overflow:
      // X - 0 -> X
      if (match(Op2, m_Zero()))
        return Op1;
      return SILValue();
    case llvm::Intrinsic::smul_with_overflow:
    case llvm::Intrinsic::umul_with_overflow:
      // 0 * X -> 0
      if (match(Op1, m_Zero()))
        return Op1;
      // X * 0 -> 0
      if (match(Op2, m_Zero()))
        return Op2;
      // 1 * X -> X
      if (match(Op1, m_One()))
        return Op2;
      // X * 1 -> X
      if (match(Op2, m_One()))
        return Op1;
      return SILValue();
  }
  return SILValue();
}

SILValue InstSimplifier::simplifyOverflowBuiltin(ApplyInst *AI,
                                                 BuiltinFunctionRefInst *FR) {
  const IntrinsicInfo &Intrinsic = FR->getIntrinsicInfo();

  // If it's an llvm intrinsic, fold the intrinsic.
  switch (Intrinsic.ID) {
    default:
      return SILValue();
    case llvm::Intrinsic::not_intrinsic:
      break;
    case llvm::Intrinsic::sadd_with_overflow:
    case llvm::Intrinsic::uadd_with_overflow:
    case llvm::Intrinsic::ssub_with_overflow:
    case llvm::Intrinsic::usub_with_overflow:
    case llvm::Intrinsic::smul_with_overflow:
    case llvm::Intrinsic::umul_with_overflow:
      return simplifyBinaryWithOverflow(AI, Intrinsic.ID);
  }

  // Otherwise, it should be one of the builtin functions.
  const BuiltinInfo &Builtin = FR->getBuiltinInfo();

  switch (Builtin.ID) {
    default: break;

      // Check and simplify binary arithmetic with overflow.
#define BUILTIN(id, name, Attrs)
#define BUILTIN_BINARY_OPERATION_WITH_OVERFLOW(id, name, _, attrs, overload) \
case BuiltinValueKind::id:
#include "swift/AST/Builtins.def"
      return simplifyBinaryWithOverflow(AI,
                          getLLVMIntrinsicIDForBuiltinWithOverflow(Builtin.ID));

  }
  return SILValue();
}

/// \brief Try to simplify the specified instruction, performing local
/// analysis of the operands of the instruction, without looking at its uses
/// (e.g. constant folding).  If a simpler result can be found, it is
/// returned, otherwise a null SILValue is returned.
///
SILValue swift::simplifyInstruction(SILInstruction *I) {
  return InstSimplifier().visit(I);
}
