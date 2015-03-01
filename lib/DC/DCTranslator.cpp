#define DEBUG_TYPE "dctranslator"
#include "llvm/DC/DCTranslator.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/DC/DCInstrSema.h"
#include "llvm/DC/DCRegisterSema.h"
#include "llvm/MC/MCAnalysis/MCFunction.h"
#include "llvm/MC/MCAnalysis/MCModule.h"
#include "llvm/MC/MCObjectDisassembler.h"
#include "llvm/Object/MachO.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <algorithm>
#include <vector>

using namespace llvm;
using namespace object;

DCTranslator::DCTranslator(LLVMContext &Ctx, TransOpt::Level TransOptLevel,
                           DCInstrSema &DIS, DCRegisterSema &DRS,
                           MCInstPrinter &IP, MCModule &MCM,
                           MCObjectDisassembler *MCOD, bool EnableIRAnnotation)
    : TheModule("output", Ctx), MCOD(MCOD), MCM(MCM), FPM(&TheModule),
      DTIT(), AnnotWriter(), DIS(DIS), OptLevel(TransOptLevel) {

  // FIXME: now this can move to print, we don't need to keep it around
  if (EnableIRAnnotation)
    AnnotWriter.reset(new DCAnnotationWriter(DTIT, DRS.MRI, IP));

  if (OptLevel >= TransOpt::Less)
    FPM.add(createPromoteMemoryToRegisterPass());
  if (OptLevel >= TransOpt::Default)
    FPM.add(createDeadCodeEliminationPass());
  if (OptLevel >= TransOpt::Aggressive)
    FPM.add(createInstructionCombiningPass());

  DIS.SwitchToModule(&TheModule);
  MCObjectDisassembler::AddressSetTy DummyTailCallTargets;
  for (auto &F : MCM.funcs())
    translateFunction(&*F, DummyTailCallTargets);
  DIS.FinalizeModule();
  (void)DRS;
}

DCTranslator::~DCTranslator() {}

Function *DCTranslator::getMainFunction() {
  return DIS.createMainFunction(getFunctionAt(getEntrypoint()));
}

Function *DCTranslator::getInitRegSetFunction() {
  return DIS.getInitRegSetFunction();
}

Function *DCTranslator::getFiniRegSetFunction() {
  return DIS.getFiniRegSetFunction();
}

Function *DCTranslator::getFunctionAt(uint64_t Addr) {
  SmallSetVector<uint64_t, 16> WorkList;
  WorkList.insert(Addr);
  for (size_t i = 0; i < WorkList.size(); ++i) {
    uint64_t Addr = WorkList[i];
    Function *F = TheModule.getFunction("fn_" + utohexstr(Addr));
    if (F && !F->isDeclaration())
      continue;

    DEBUG(dbgs() << "Translating function at " << utohexstr(Addr) << "\n");

    if (!MCOD) {
      llvm_unreachable(("Unable to translate unknown function at " +
                        utohexstr(Addr) + " without a disassembler!").c_str());
    }

    MCObjectDisassembler::AddressSetTy CallTargets, TailCallTargets;
    MCFunction *MCFN =
        MCOD->createFunction(&MCM, Addr, CallTargets, TailCallTargets);

    // If the function is empty, it is the declaration of an external function.
    if (MCFN->empty()) {
      StringRef ExtFnName = MCFN->getName();
      assert(!ExtFnName.empty() && "Unnamed function declaration!");
      DEBUG(dbgs() << "Found external function: " << ExtFnName << "\n");
      DIS.createExternalWrapperFunction(Addr, ExtFnName);
      continue;
    }

    translateFunction(MCFN, TailCallTargets);
    for (auto CallTarget : CallTargets)
      WorkList.insert(CallTarget);
  }
  return TheModule.getFunction("fn_" + utohexstr(Addr));
}

namespace {
  class AddrPrettyStackTraceEntry : public PrettyStackTraceEntry {
  public:
    uint64_t StartAddr;
    const char *Kind;
    AddrPrettyStackTraceEntry(uint64_t StartAddr, const char *Kind)
      : PrettyStackTraceEntry(), StartAddr(StartAddr), Kind(Kind) {}

    void print(raw_ostream &OS) const override {
      OS << "DC: Translating " << Kind << " at address "
         << utohexstr(StartAddr) << "\n";
    }
  };
} // end anonymous namespace

static bool BBBeginAddrLess(const MCBasicBlock *LHS, const MCBasicBlock *RHS) {
  return LHS->getStartAddr() < RHS->getStartAddr();
}

void DCTranslator::translateFunction(
    MCFunction *MCFN,
    const MCObjectDisassembler::AddressSetTy &TailCallTargets) {

  AddrPrettyStackTraceEntry X(MCFN->getEntryBlock()->getStartAddr(),"Function");

  DIS.SwitchToFunction(MCFN);

  // First, make sure all basic blocks are created, and sorted.
  std::vector<const MCBasicBlock *> BasicBlocks;
  std::copy(MCFN->begin(), MCFN->end(), std::back_inserter(BasicBlocks));
  std::sort(BasicBlocks.begin(), BasicBlocks.end(), BBBeginAddrLess);
  for (auto &BB : BasicBlocks)
    DIS.getOrCreateBasicBlock(BB->getStartAddr());

  for (auto &BB : *MCFN) {
    AddrPrettyStackTraceEntry X(BB->getStartAddr(), "Basic Block");

    DEBUG(dbgs() << "Translating basic block starting at "
                 << utohexstr(BB->getStartAddr()) << ", with " << BB->size()
                 << " instructions.\n");
    DIS.SwitchToBasicBlock(BB);
    for (auto &I : *BB) {
      DEBUG(dbgs() << "Translating instruction:\n ";
            dbgs() << I.Inst << "\n";);
      DCTranslatedInst TI(I);
      if (!DIS.translateInst(I, TI)) {
        errs() << "Cannot translate instruction: \n  ";
        errs() << I.Inst << "\n";
        llvm_unreachable("Couldn't translate instruction\n");
      }
      DTIT.trackInst(TI);
    }
    DIS.FinalizeBasicBlock();
  }

  for (auto TailCallTarget : TailCallTargets)
    DIS.createExternalTailCallBB(TailCallTarget);

  Function *Fn = DIS.FinalizeFunction();
  {
    // ValueToValueMapTy VMap;
    // Function *OrigFn = CloneFunction(Fn, VMap, false);
    // OrigFn->setName(Fn->getName() + "_orig");
    // TheModule.getFunctionList().push_back(OrigFn);
    FPM.run(*Fn);
  }

  if (!AnnotWriter)
    DTIT.clear();
}

void DCTranslator::print(raw_ostream &OS) {
  TheModule.print(OS, AnnotWriter.get());
}
