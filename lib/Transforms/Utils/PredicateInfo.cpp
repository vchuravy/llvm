//===-- PredicateInfo.cpp - PredicateInfo Builder--------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------===//
//
// This file implements the PredicateInfo class.
//
//===----------------------------------------------------------------===//

#include "llvm/Transforms/Utils/PredicateInfo.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/OrderedBasicBlock.h"
#include "llvm/IR/AssemblyAnnotationWriter.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Transforms/Scalar.h"
#include <algorithm>
#define DEBUG_TYPE "predicateinfo"
using namespace llvm;
using namespace PatternMatch;
using namespace llvm::PredicateInfoClasses;

INITIALIZE_PASS_BEGIN(PredicateInfoPrinterLegacyPass, "print-predicateinfo",
                      "PredicateInfo Printer", false, false)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(AssumptionCacheTracker)
INITIALIZE_PASS_END(PredicateInfoPrinterLegacyPass, "print-predicateinfo",
                    "PredicateInfo Printer", false, false)
static cl::opt<bool> VerifyPredicateInfo(
    "verify-predicateinfo", cl::init(false), cl::Hidden,
    cl::desc("Verify PredicateInfo in legacy printer pass."));
namespace llvm {
namespace PredicateInfoClasses {
enum LocalNum {
  // Operations that must appear first in the block.
  LN_First,
  // Operations that are somewhere in the middle of the block, and are sorted on
  // demand.
  LN_Middle,
  // Operations that must appear last in a block, like successor phi node uses.
  LN_Last
};

// Associate global and local DFS info with defs and uses, so we can sort them
// into a global domination ordering.
struct ValueDFS {
  int DFSIn = 0;
  int DFSOut = 0;
  unsigned int LocalNum = LN_Middle;
  PredicateBase *PInfo = nullptr;
  // Only one of Def or Use will be set.
  Value *Def = nullptr;
  Use *U = nullptr;
};

// This compares ValueDFS structures, creating OrderedBasicBlocks where
// necessary to compare uses/defs in the same block.  Doing so allows us to walk
// the minimum number of instructions necessary to compute our def/use ordering.
struct ValueDFS_Compare {
  DenseMap<const BasicBlock *, std::unique_ptr<OrderedBasicBlock>> &OBBMap;
  ValueDFS_Compare(
      DenseMap<const BasicBlock *, std::unique_ptr<OrderedBasicBlock>> &OBBMap)
      : OBBMap(OBBMap) {}
  bool operator()(const ValueDFS &A, const ValueDFS &B) const {
    if (&A == &B)
      return false;
    // The only case we can't directly compare them is when they in the same
    // block, and both have localnum == middle.  In that case, we have to use
    // comesbefore to see what the real ordering is, because they are in the
    // same basic block.

    bool SameBlock = std::tie(A.DFSIn, A.DFSOut) == std::tie(B.DFSIn, B.DFSOut);

    if (!SameBlock || A.LocalNum != LN_Middle || B.LocalNum != LN_Middle)
      return std::tie(A.DFSIn, A.DFSOut, A.LocalNum, A.Def, A.U) <
             std::tie(B.DFSIn, B.DFSOut, B.LocalNum, B.Def, B.U);
    return localComesBefore(A, B);
  }

  // Get the definition of an instruction that occurs in the middle of a block.
  Value *getMiddleDef(const ValueDFS &VD) const {
    if (VD.Def)
      return VD.Def;
    // It's possible for the defs and uses to be null.  For branches, the local
    // numbering will say the placed predicaeinfos should go first (IE
    // LN_beginning), so we won't be in this function. For assumes, we will end
    // up here, beause we need to order the def we will place relative to the
    // assume.  So for the purpose of ordering, we pretend the def is the assume
    // because that is where we will insert the info.
    if (!VD.U) {
      assert(VD.PInfo &&
             "No def, no use, and no predicateinfo should not occur");
      assert(isa<PredicateAssume>(VD.PInfo) &&
             "Middle of block should only occur for assumes");
      return cast<PredicateAssume>(VD.PInfo)->AssumeInst;
    }
    return nullptr;
  }

  // Return either the Def, if it's not null, or the user of the Use, if the def
  // is null.
  const Instruction *getDefOrUser(const Value *Def, const Use *U) const {
    if (Def)
      return cast<Instruction>(Def);
    return cast<Instruction>(U->getUser());
  }

  // This performs the necessary local basic block ordering checks to tell
  // whether A comes before B, where both are in the same basic block.
  bool localComesBefore(const ValueDFS &A, const ValueDFS &B) const {
    auto *ADef = getMiddleDef(A);
    auto *BDef = getMiddleDef(B);

    // See if we have real values or uses. If we have real values, we are
    // guaranteed they are instructions or arguments. No matter what, we are
    // guaranteed they are in the same block if they are instructions.
    auto *ArgA = dyn_cast_or_null<Argument>(ADef);
    auto *ArgB = dyn_cast_or_null<Argument>(BDef);

    if (ArgA && !ArgB)
      return true;
    if (ArgB && !ArgA)
      return false;
    if (ArgA && ArgB)
      return ArgA->getArgNo() < ArgB->getArgNo();

    auto *AInst = getDefOrUser(ADef, A.U);
    auto *BInst = getDefOrUser(BDef, B.U);

    auto *BB = AInst->getParent();
    auto LookupResult = OBBMap.find(BB);
    if (LookupResult != OBBMap.end())
      return LookupResult->second->dominates(AInst, BInst);
    else {
      auto Result = OBBMap.insert({BB, make_unique<OrderedBasicBlock>(BB)});
      return Result.first->second->dominates(AInst, BInst);
    }
    return std::tie(ADef, A.U) < std::tie(BDef, B.U);
  }
};

} // namespace PredicateInfoClasses

bool PredicateInfo::stackIsInScope(const ValueDFSStack &Stack, int DFSIn,
                                   int DFSOut) const {
  if (Stack.empty())
    return false;
  return DFSIn >= Stack.back().DFSIn && DFSOut <= Stack.back().DFSOut;
}

void PredicateInfo::popStackUntilDFSScope(ValueDFSStack &Stack, int DFSIn,
                                          int DFSOut) {
  while (!Stack.empty() && !stackIsInScope(Stack, DFSIn, DFSOut))
    Stack.pop_back();
}

// Convert the uses of Op into a vector of uses, associating global and local
// DFS info with each one.
void PredicateInfo::convertUsesToDFSOrdered(
    Value *Op, SmallVectorImpl<ValueDFS> &DFSOrderedSet) {
  for (auto &U : Op->uses()) {
    if (auto *I = dyn_cast<Instruction>(U.getUser())) {
      ValueDFS VD;
      // Put the phi node uses in the incoming block.
      BasicBlock *IBlock;
      if (auto *PN = dyn_cast<PHINode>(I)) {
        IBlock = PN->getIncomingBlock(U);
        // Make phi node users appear last in the incoming block
        // they are from.
        VD.LocalNum = LN_Last;
      } else {
        // If it's not a phi node use, it is somewhere in the middle of the
        // block.
        IBlock = I->getParent();
        VD.LocalNum = LN_Middle;
      }
      DomTreeNode *DomNode = DT.getNode(IBlock);
      // It's possible our use is in an unreachable block. Skip it if so.
      if (!DomNode)
        continue;
      VD.DFSIn = DomNode->getDFSNumIn();
      VD.DFSOut = DomNode->getDFSNumOut();
      VD.U = &U;
      DFSOrderedSet.push_back(VD);
    }
  }
}

// Collect relevant operations from Comparison that we may want to insert copies
// for.
void collectCmpOps(CmpInst *Comparison, SmallVectorImpl<Value *> &CmpOperands) {
  auto *Op0 = Comparison->getOperand(0);
  auto *Op1 = Comparison->getOperand(1);
  if (Op0 == Op1)
    return;
  CmpOperands.push_back(Comparison);
  // Only want real values, not constants.  Additionally, operands with one use
  // are only being used in the comparison, which means they will not be useful
  // for us to consider for predicateinfo.
  //
  // FIXME: LLVM crashes trying to create an intrinsic declaration of some
  // pointer to function types that return structs, so we avoid them.
  if ((isa<Instruction>(Op0) || isa<Argument>(Op0)) && !Op0->hasOneUse() &&
      !(Op0->getType()->isPointerTy() &&
        Op0->getType()->getPointerElementType()->isFunctionTy()))
    CmpOperands.push_back(Op0);
  if ((isa<Instruction>(Op1) || isa<Argument>(Op1)) && !Op1->hasOneUse() &&
      !(Op1->getType()->isPointerTy() &&
        Op1->getType()->getPointerElementType()->isFunctionTy()))
    CmpOperands.push_back(Op1);
}

// Process an assume instruction and place relevant operations we want to rename
// into OpsToRename.
void PredicateInfo::processAssume(IntrinsicInst *II, BasicBlock *AssumeBB,
                                  SmallPtrSetImpl<Value *> &OpsToRename) {
  SmallVector<Value *, 8> CmpOperands;
  // Second, see if we have a comparison we support
  SmallVector<Value *, 2> ComparisonsToProcess;
  CmpInst::Predicate Pred;
  Value *Operand = II->getOperand(0);
  if (m_c_And(m_Cmp(Pred, m_Value(), m_Value()),
              m_Cmp(Pred, m_Value(), m_Value()))
          .match(II->getOperand(0))) {
    ComparisonsToProcess.push_back(
        cast<BinaryOperator>(Operand)->getOperand(0));
    ComparisonsToProcess.push_back(
        cast<BinaryOperator>(Operand)->getOperand(1));
  } else {
    ComparisonsToProcess.push_back(Operand);
  }
  for (auto Comparison : ComparisonsToProcess) {
    if (auto *Cmp = dyn_cast<CmpInst>(Comparison)) {
      collectCmpOps(Cmp, CmpOperands);
      // Now add our copy infos for our operands
      for (auto *Op : CmpOperands) {
        OpsToRename.insert(Op);
        auto &OperandInfo = getOrCreateValueInfo(Op);
        PredicateBase *PB = new PredicateAssume(Op, II, Cmp);
        AllInfos.push_back(PB);
        OperandInfo.Infos.push_back(PB);
      }
      CmpOperands.clear();
    }
  }
}

// Process a block terminating branch, and place relevant operations to be
// renamed into OpsToRename.
void PredicateInfo::processBranch(BranchInst *BI, BasicBlock *BranchBB,
                                  SmallPtrSetImpl<Value *> &OpsToRename) {
  SmallVector<Value *, 8> CmpOperands;
  BasicBlock *FirstBB = BI->getSuccessor(0);
  BasicBlock *SecondBB = BI->getSuccessor(1);
  bool FirstSinglePred = FirstBB->getSinglePredecessor();
  bool SecondSinglePred = SecondBB->getSinglePredecessor();
  SmallVector<BasicBlock *, 2> SuccsToProcess;
  bool isAnd = false;
  bool isOr = false;
  // First make sure we have single preds for these successors, as we can't
  // usefully propagate true/false info to them if there are multiple paths to
  // them.
  if (FirstSinglePred)
    SuccsToProcess.push_back(FirstBB);
  if (SecondSinglePred)
    SuccsToProcess.push_back(SecondBB);
  if (SuccsToProcess.empty())
    return;
  // Second, see if we have a comparison we support
  SmallVector<Value *, 2> ComparisonsToProcess;
  CmpInst::Predicate Pred;

  // Match combinations of conditions.
  if (match(BI->getCondition(), m_And(m_Cmp(Pred, m_Value(), m_Value()),
                                      m_Cmp(Pred, m_Value(), m_Value()))) ||
      match(BI->getCondition(), m_Or(m_Cmp(Pred, m_Value(), m_Value()),
                                     m_Cmp(Pred, m_Value(), m_Value())))) {
    auto *BinOp = cast<BinaryOperator>(BI->getCondition());
    if (BinOp->getOpcode() == Instruction::And)
      isAnd = true;
    else if (BinOp->getOpcode() == Instruction::Or)
      isOr = true;
    ComparisonsToProcess.push_back(BinOp->getOperand(0));
    ComparisonsToProcess.push_back(BinOp->getOperand(1));
  } else {
    ComparisonsToProcess.push_back(BI->getCondition());
  }
  for (auto Comparison : ComparisonsToProcess) {
    if (auto *Cmp = dyn_cast<CmpInst>(Comparison)) {
      collectCmpOps(Cmp, CmpOperands);
      // Now add our copy infos for our operands
      for (auto *Op : CmpOperands) {
        OpsToRename.insert(Op);
        auto &OperandInfo = getOrCreateValueInfo(Op);
        for (auto *Succ : SuccsToProcess) {
          bool TakenEdge = (Succ == FirstBB);
          // For and, only insert on the true edge
          // For or, only insert on the false edge
          if ((isAnd && !TakenEdge) || (isOr && TakenEdge))
            continue;
          PredicateBase *PB =
              new PredicateBranch(Op, BranchBB, Succ, Cmp, TakenEdge);
          AllInfos.push_back(PB);
          OperandInfo.Infos.push_back(PB);
        }
      }
      CmpOperands.clear();
    }
  }
}

// Build predicate info for our function
void PredicateInfo::buildPredicateInfo() {
  DT.updateDFSNumbers();
  // Collect operands to rename from all conditional branch terminators, as well
  // as assume statements.
  SmallPtrSet<Value *, 8> OpsToRename;
  for (auto DTN : depth_first(DT.getRootNode())) {
    BasicBlock *BranchBB = DTN->getBlock();
    if (auto *BI = dyn_cast<BranchInst>(BranchBB->getTerminator())) {
      if (!BI->isConditional())
        continue;
      processBranch(BI, BranchBB, OpsToRename);
    }
  }
  for (auto &Assume : AC.assumptions()) {
    if (auto *II = dyn_cast_or_null<IntrinsicInst>(Assume))
      processAssume(II, II->getParent(), OpsToRename);
  }
  // Now rename all our operations.
  renameUses(OpsToRename);
}
Value *PredicateInfo::materializeStack(unsigned int &Counter,
                                       ValueDFSStack &RenameStack,
                                       Value *OrigOp) {
  // Find the first thing we have to materialize
  auto RevIter = RenameStack.rbegin();
  for (; RevIter != RenameStack.rend(); ++RevIter)
    if (RevIter->Def)
      break;

  size_t Start = RevIter - RenameStack.rbegin();
  // The maximum number of things we should be trying to materialize at once
  // right now is 4, depending on if we had an assume, a branch, and both used
  // and of conditions.
  for (auto RenameIter = RenameStack.end() - Start;
       RenameIter != RenameStack.end(); ++RenameIter) {
    auto *Op =
        RenameIter == RenameStack.begin() ? OrigOp : (RenameIter - 1)->Def;
    ValueDFS &Result = *RenameIter;
    auto *ValInfo = Result.PInfo;
    // For branches, we can just place the operand in the split block.  For
    // assume, we have to place it right before the assume to ensure we dominate
    // all of our uses.
    if (isa<PredicateBranch>(ValInfo)) {
      auto *PBranch = cast<PredicateBranch>(ValInfo);
      // It's possible we are trying to insert multiple predicateinfos in the
      // same block at the beginning of the block.  When we do this, we need to
      // insert them one after the other, not one before the other. To see if we
      // have already inserted predicateinfo into this block, we see if Op !=
      // OrigOp && Op->getParent() == PBranch->SplitBB.  Op must be an
      // instruction we inserted if it's not the original op.
      BasicBlock::iterator InsertPt;
      if (Op == OrigOp ||
          cast<Instruction>(Op)->getParent() != PBranch->SplitBB) {
        InsertPt = PBranch->SplitBB->begin();
        // Insert after last phi node.
        while (isa<PHINode>(InsertPt))
          ++InsertPt;
      } else {
        // Insert after op.
        InsertPt = ++(cast<Instruction>(Op)->getIterator());
      }
      IRBuilder<> B(PBranch->SplitBB, InsertPt);
      Function *IF = Intrinsic::getDeclaration(
          F.getParent(), Intrinsic::ssa_copy, Op->getType());
      Value *PIC = B.CreateCall(IF, Op, Op->getName() + "." + Twine(Counter++));
      PredicateMap.insert({PIC, ValInfo});
      Result.Def = PIC;
    } else {
      auto *PAssume = dyn_cast<PredicateAssume>(ValInfo);
      assert(PAssume &&
             "Should not have gotten here without it being an assume");
      // Unlike above, this should already insert in the right order when we
      // insert multiple predicateinfos in the same block.  Because we are
      // always inserting right before the assume (instead of the beginning of a
      // block), newer insertions will end up after older ones.
      IRBuilder<> B(PAssume->AssumeInst->getParent(),
                    PAssume->AssumeInst->getIterator());
      Function *IF = Intrinsic::getDeclaration(
          F.getParent(), Intrinsic::ssa_copy, Op->getType());
      Value *PIC = B.CreateCall(IF, Op);
      PredicateMap.insert({PIC, ValInfo});
      Result.Def = PIC;
    }
  }
  return RenameStack.back().Def;
}

// Instead of the standard SSA renaming algorithm, which is O(Number of
// instructions), and walks the entire dominator tree, we walk only the defs +
// uses.  The standard SSA renaming algorithm does not really rely on the
// dominator tree except to order the stack push/pops of the renaming stacks, so
// that defs end up getting pushed before hitting the correct uses.  This does
// not require the dominator tree, only the *order* of the dominator tree. The
// complete and correct ordering of the defs and uses, in dominator tree is
// contained in the DFS numbering of the dominator tree. So we sort the defs and
// uses into the DFS ordering, and then just use the renaming stack as per
// normal, pushing when we hit a def (which is a predicateinfo instruction),
// popping when we are out of the dfs scope for that def, and replacing any uses
// with top of stack if it exists.  In order to handle liveness without
// propagating liveness info, we don't actually insert the predicateinfo
// instruction def until we see a use that it would dominate.  Once we see such
// a use, we materialize the predicateinfo instruction in the right place and
// use it.
//
// TODO: Use this algorithm to perform fast single-variable renaming in
// promotememtoreg and memoryssa.
void PredicateInfo::renameUses(SmallPtrSetImpl<Value *> &OpsToRename) {
  ValueDFS_Compare Compare(OBBMap);
  // Compute liveness, and rename in O(uses) per Op.
  for (auto *Op : OpsToRename) {
    unsigned Counter = 0;
    SmallVector<ValueDFS, 16> OrderedUses;
    const auto &ValueInfo = getValueInfo(Op);
    // Insert the possible copies into the def/use list.
    // They will become real copies if we find a real use for them, and never
    // created otherwise.
    for (auto &PossibleCopy : ValueInfo.Infos) {
      ValueDFS VD;
      BasicBlock *CopyBB = nullptr;
      // Determine where we are going to place the copy by the copy type.
      // The predicate info for branches always come first, they will get
      // materialized in the split block at the top of the block.
      // The predicate info for assumes will be somewhere in the middle,
      // it will get materialized in front of the assume.
      if (const auto *PBranch = dyn_cast<PredicateBranch>(PossibleCopy)) {
        CopyBB = PBranch->SplitBB;
        VD.LocalNum = LN_First;
      } else if (const auto *PAssume =
                     dyn_cast<PredicateAssume>(PossibleCopy)) {
        CopyBB = PAssume->AssumeInst->getParent();
        VD.LocalNum = LN_Middle;
      } else
        llvm_unreachable("Unhandled predicate info type");
      DomTreeNode *DomNode = DT.getNode(CopyBB);
      if (!DomNode)
        continue;
      VD.DFSIn = DomNode->getDFSNumIn();
      VD.DFSOut = DomNode->getDFSNumOut();
      VD.PInfo = PossibleCopy;
      OrderedUses.push_back(VD);
    }

    convertUsesToDFSOrdered(Op, OrderedUses);
    std::sort(OrderedUses.begin(), OrderedUses.end(), Compare);
    SmallVector<ValueDFS, 8> RenameStack;
    // For each use, sorted into dfs order, push values and replaces uses with
    // top of stack, which will represent the reaching def.
    for (auto &VD : OrderedUses) {
      // We currently do not materialize copy over copy, but we should decide if
      // we want to.
      bool PossibleCopy = VD.PInfo != nullptr;
      if (RenameStack.empty()) {
        DEBUG(dbgs() << "Rename Stack is empty\n");
      } else {
        DEBUG(dbgs() << "Rename Stack Top DFS numbers are ("
                     << RenameStack.back().DFSIn << ","
                     << RenameStack.back().DFSOut << ")\n");
      }

      DEBUG(dbgs() << "Current DFS numbers are (" << VD.DFSIn << ","
                   << VD.DFSOut << ")\n");

      bool ShouldPush = (VD.Def || PossibleCopy);
      bool OutOfScope = !stackIsInScope(RenameStack, VD.DFSIn, VD.DFSOut);
      if (OutOfScope || ShouldPush) {
        // Sync to our current scope.
        popStackUntilDFSScope(RenameStack, VD.DFSIn, VD.DFSOut);
        ShouldPush |= (VD.Def || PossibleCopy);
        if (ShouldPush) {
          RenameStack.push_back(VD);
        }
      }
      // If we get to this point, and the stack is empty we must have a use
      // with no renaming needed, just skip it.
      if (RenameStack.empty())
        continue;
      // Skip values, only want to rename the uses
      if (VD.Def || PossibleCopy)
        continue;
      ValueDFS &Result = RenameStack.back();

      // If the possible copy dominates something, materialize our stack up to
      // this point. This ensures every comparison that affects our operation
      // ends up with predicateinfo.
      if (!Result.Def)
        Result.Def = materializeStack(Counter, RenameStack, Op);

      DEBUG(dbgs() << "Found replacement " << *Result.Def << " for "
                   << *VD.U->get() << " in " << *(VD.U->getUser()) << "\n");
      assert(DT.dominates(cast<Instruction>(Result.Def), *VD.U) &&
             "Predicateinfo def should have dominated this use");
      VD.U->set(Result.Def);
    }
  }
}

PredicateInfo::ValueInfo &PredicateInfo::getOrCreateValueInfo(Value *Operand) {
  auto OIN = ValueInfoNums.find(Operand);
  if (OIN == ValueInfoNums.end()) {
    // This will grow it
    ValueInfos.resize(ValueInfos.size() + 1);
    // This will use the new size and give us a 0 based number of the info
    auto InsertResult = ValueInfoNums.insert({Operand, ValueInfos.size() - 1});
    assert(InsertResult.second && "Value info number already existed?");
    return ValueInfos[InsertResult.first->second];
  }
  return ValueInfos[OIN->second];
}

const PredicateInfo::ValueInfo &
PredicateInfo::getValueInfo(Value *Operand) const {
  auto OINI = ValueInfoNums.lookup(Operand);
  assert(OINI != 0 && "Operand was not really in the Value Info Numbers");
  assert(OINI < ValueInfos.size() &&
         "Value Info Number greater than size of Value Info Table");
  return ValueInfos[OINI];
}

PredicateInfo::PredicateInfo(Function &F, DominatorTree &DT,
                             AssumptionCache &AC)
    : F(F), DT(DT), AC(AC) {
  // Push an empty operand info so that we can detect 0 as not finding one
  ValueInfos.resize(1);
  buildPredicateInfo();
}

PredicateInfo::~PredicateInfo() {}

void PredicateInfo::verifyPredicateInfo() const {}

char PredicateInfoPrinterLegacyPass::ID = 0;

PredicateInfoPrinterLegacyPass::PredicateInfoPrinterLegacyPass()
    : FunctionPass(ID) {
  initializePredicateInfoPrinterLegacyPassPass(
      *PassRegistry::getPassRegistry());
}

void PredicateInfoPrinterLegacyPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequiredTransitive<DominatorTreeWrapperPass>();
  AU.addRequired<AssumptionCacheTracker>();
}

bool PredicateInfoPrinterLegacyPass::runOnFunction(Function &F) {
  auto &DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  auto &AC = getAnalysis<AssumptionCacheTracker>().getAssumptionCache(F);
  auto PredInfo = make_unique<PredicateInfo>(F, DT, AC);
  PredInfo->print(dbgs());
  if (VerifyPredicateInfo)
    PredInfo->verifyPredicateInfo();
  return false;
}

PreservedAnalyses PredicateInfoPrinterPass::run(Function &F,
                                                FunctionAnalysisManager &AM) {
  auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
  auto &AC = AM.getResult<AssumptionAnalysis>(F);
  OS << "PredicateInfo for function: " << F.getName() << "\n";
  make_unique<PredicateInfo>(F, DT, AC)->print(OS);

  return PreservedAnalyses::all();
}

/// \brief An assembly annotator class to print PredicateInfo information in
/// comments.
class PredicateInfoAnnotatedWriter : public AssemblyAnnotationWriter {
  friend class PredicateInfo;
  const PredicateInfo *PredInfo;

public:
  PredicateInfoAnnotatedWriter(const PredicateInfo *M) : PredInfo(M) {}

  virtual void emitBasicBlockStartAnnot(const BasicBlock *BB,
                                        formatted_raw_ostream &OS) {}

  virtual void emitInstructionAnnot(const Instruction *I,
                                    formatted_raw_ostream &OS) {
    if (const auto *PI = PredInfo->getPredicateInfoFor(I)) {
      OS << "; Has predicate info\n";
      if (const auto *PB = dyn_cast<PredicateBranch>(PI))
        OS << "; branch predicate info { TrueEdge: " << PB->TrueEdge
           << " Comparison:" << *PB->Comparison << " }\n";
      else if (const auto *PA = dyn_cast<PredicateAssume>(PI))
        OS << "; assume predicate info {"
           << " Comparison:" << *PA->Comparison << " }\n";
    }
  }
};

void PredicateInfo::print(raw_ostream &OS) const {
  PredicateInfoAnnotatedWriter Writer(this);
  F.print(OS, &Writer);
}

void PredicateInfo::dump() const {
  PredicateInfoAnnotatedWriter Writer(this);
  F.print(dbgs(), &Writer);
}

PreservedAnalyses PredicateInfoVerifierPass::run(Function &F,
                                                 FunctionAnalysisManager &AM) {
  auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
  auto &AC = AM.getResult<AssumptionAnalysis>(F);
  make_unique<PredicateInfo>(F, DT, AC)->verifyPredicateInfo();

  return PreservedAnalyses::all();
}
}
