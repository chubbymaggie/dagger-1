//===- SemanticsEmitter.cpp - Generate a Instruction Set Desc. ------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This tablegen backend is responsible for emitting a description of the
// instruction-level semantics of the target instruction set.
//
//===----------------------------------------------------------------------===//

#include "CodeGenDAGPatterns.h"
#include "CodeGenTarget.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/TableGen/Error.h"
#include "llvm/TableGen/Record.h"
#include "llvm/TableGen/TableGenBackend.h"
#include <algorithm>
#include <map>

using namespace llvm;

namespace {

static std::string sanitizeSelectFuncToEnumVal(Record &CP,
                                               const CodeGenDAGPatterns &CGP) {
  const ComplexPattern &CPI = CGP.getComplexPattern(&CP);
  StringRef F = CPI.getSelectFunc();

  if (!F.startswith("select") && !F.startswith("Select"))
    PrintFatalError(
        CP.getLoc(),
        Twine("ComplexPattern func doesn't start with 'select': '") + F + "'.");

  F = F.drop_front(StringRef("select").size());

  std::string SF = F.str();
  auto Pos = SF.find('<');
  if (Pos != SF.npos) {
    SF[Pos] = '_';
    SF.pop_back();
  }

  return SF;
}

/// The target we're generating semantics for: keeps around some useful
/// references to the parsed CodeGen target description, and some generation
/// variables.
class SemanticsTarget {
public:
  RecordKeeper &Records;
  CodeGenDAGPatterns CGPatterns;
  CodeGenTarget &CGTarget;

  /// Keep track of the equivalence between target-specific SDNodes and
  /// their target-independent equivalent, as described in definitions
  /// derived of the SDNodeEquiv class.
  DenseMap<Record *, Record *> SDNodeEquiv;

  /// Unique constant integers, keeping track of the order they appeared in.
  /// This is done so that the generated semantics table is an uint16_t[],
  /// with uint64_t constants only being an uint16_t index to this table.
  /// Use an std::map instead of:
  /// - SetVector, because knowing whether a key exists isn't enough,
  ///   we also need an efficient way to get its index.
  /// - DenseMap, because there is no acceptable tombstone key.
  /// There is no element at index 0.
  std::map<uint64_t, uint16_t> ConstantIdx;
  uint16_t CurConstantIdx;

  /// The set of encountered predicate (PatFrag) records, in order of emission.
  /// We later emit a debug dump function for each of these.
  SetVector<Record *> EncounteredPredicates;

  SemanticsTarget(RecordKeeper &Records);
};

struct LSResult {
  // The index of this result in the linear semantics definitions.
  unsigned char DefNo;
  // The MVT::SimpleValueType of this result.
  // 'Any' is 256, but it's never a concrete type.
  unsigned char VT;

  LSResult(unsigned char DefNo, MVT::SimpleValueType VT)
      : DefNo(DefNo), VT(VT) {}
};

typedef std::vector<LSResult> LSResults;

struct LSNode {
  const TreePatternNode *TPN = nullptr;

  /// The opcode for this operation: either an ISD (for SDNodes), or DCINS
  /// (for other operations, like manipulating operands, registers, etc..)
  /// opcode.
  StringRef Opcode;

  /// The types of each result generated by this operation.
  std::vector<MVT::SimpleValueType> Types;

  /// All the operands of this instruction.
  std::vector<std::string> Operands;

  void addOperand(std::string Op) { Operands.push_back(Op); }

  LSNode(const TreePatternNode &TPN) : TPN(&TPN) {
    for (auto &Ty : TPN.getExtTypes())
      Types.push_back(Ty.getConcrete());
  }
};

class InstSemantics {
public:
  const TreePattern *Pattern = nullptr;

  std::vector<LSNode> Semantics;

  SmallVector<Record *, 1> ExplicitDefs;
  SmallVector<Record *, 1> ImplicitDefs;

  // A reference to the last op that had at least one non-void definition,
  // as its index in the Semantics list.
  unsigned LastDefSemaIdx = ~0U;
  // As well as its first def index.
  unsigned LastDefNo = ~0U;

  // Whether these semantics use an intrinsic.
  bool HasIntrinsic = false;
  // Whether these semantics use a complex pattern.
  bool HasComplexPattern = false;
};

/// The core of the Pattern->Semantics translation: a linearization of the
/// semantics pattern DAG.
class LinearSemantics {
public:
  LinearSemantics(SemanticsTarget &Target, const CodeGenInstruction &CGI,
                  InstSemantics &I)
      : Target(Target), CGI(CGI), I(I), CurDefNo(0) {}

private:
  SemanticsTarget &Target;
  const CodeGenInstruction &CGI;
  InstSemantics &I;

  StringMap<unsigned> OperandByName;

  unsigned CurDefNo;

  /// Get the CodeGenInstruction OperandInfo for \p Name.
  const CGIOperandList::OperandInfo *getNamedOperand(StringRef Name) {
    if (Name.empty())
      return nullptr;
    // FIXME: This is the slow, stupid, simple way.
    for (auto &Op : CGI.Operands)
      if (Name == Op.Name)
        return &Op;
    return nullptr;
  }

  /// Add the operation \p NS to the instruction semantics, keeping track of
  /// the defined values.
  void addSemantics(const LSNode &NS) {
    const unsigned FirstDefNo = CurDefNo;
    for (auto &Ty : NS.Types) {
      if (Ty != MVT::isVoid)
        ++CurDefNo;
      if (Ty == MVT::Untyped)
        I.HasIntrinsic = true;
    }
    if (FirstDefNo != CurDefNo) {
      I.LastDefNo = FirstDefNo;
      I.LastDefSemaIdx = I.Semantics.size();
    }
    I.Semantics.push_back(NS);
  }

  /// Make node semantics from an Operand patern:
  /// - if \p TPN is a RegisterClass or a RegisterOperand, generate:
  ///     DCINS::GET_RC, <inferred type>, <MIOperandNo of the Operand>
  /// - if \p TPN is an Operand, generate:
  ///     DCINS::CUSTOM_OP, <inferred types>,
  ///       <the Operand type as a Target::OpTypes:: enum value>,
  ///       <MIOperandNo of the first MI operand for this Operand>
  /// - if \p TPN is an OPERAND_IMMEDIATE Operand, generate:
  ///     DCINS::GET_IMMEDIATE, <inferred type>, <MIOperandNo of the Operand>
  ///
  LSResult flattenOperand(const TreePatternNode &TPN,
                          const CGIOperandList::OperandInfo *OpInfo) {
    Record *OpRec = OpInfo->Rec;
    LSNode Op(TPN);

    assert(TPN.getExtTypes().size() == 1);

    // RegisterOperands are the same thing as RegisterClasses.
    if (OpRec->isSubClassOf("RegisterOperand"))
      OpRec = OpRec->getValueAsDef("RegClass");

    if (OpRec->isSubClassOf("Operand")) {
      if (OpInfo->OperandType == "OPERAND_IMMEDIATE") {
        Op.Opcode = "DCINS::GET_IMMEDIATE";
      } else {
        Op.Opcode = "DCINS::CUSTOM_OP";
        Op.addOperand(CGI.Namespace + "::OpTypes::" + OpRec->getName().str());
        auto It = OperandByName.find(OpInfo->Name);
        if (It == OperandByName.end()) {
          OperandByName[OpInfo->Name] = CurDefNo;
        } else {
          // If we already found it, no need to generate the operation again.
          return LSResult(It->getValue(), Op.Types[0]);
        }
      }
    } else if (OpRec->isSubClassOf("RegisterClass")) {
      Op.Opcode = "DCINS::GET_RC";
    } else {
      llvm_unreachable("Unknown operand type");
    }
    LSResult R(CurDefNo, Op.Types[0]);
    Op.addOperand(utostr(OpInfo->MIOperandNo));
    addSemantics(Op);
    return R;
  }

  /// Make node semantics from a leaf pattern:
  /// - if \p TPN is an explicit Register, generate:
  ///     DCINS::GET_REG, <inferred type>, Target::RegName
  /// - if \p TPN is a compile-time constant, generate:
  ///     DCINS::GET_CONSTANT, <inferred type>, <Constant index>
  ///   The constant index points in an uint64_t array, where all compile-time
  ///   constants are uniqued (so that the semantics array remains uint16_t[].)
  ///
  LSResult flattenLeaf(const TreePatternNode &TPN) {
    LSNode Op(TPN);
    DefInit *OpDef = dyn_cast<DefInit>(TPN.getLeafValue());

    assert(TPN.getExtTypes().size() == 1);

    if (OpDef == nullptr) {
      IntInit *OpInt = cast<IntInit>(TPN.getLeafValue());
      Op.Opcode = "DCINS::GET_CONSTANT";
      uint16_t &Idx = Target.ConstantIdx[OpInt->getValue()];
      if (Idx == 0)
        Idx = ++Target.CurConstantIdx;
      Op.addOperand(utostr(Idx - 1));
      LSResult R(CurDefNo, Op.Types[0]);
      addSemantics(Op);
      return R;
    }

    Record *OpRec = OpDef->getDef();

    if (OpRec->isSubClassOf("Register")) {
      Op.Opcode = "DCINS::GET_REG";
      Op.addOperand(CGI.Namespace + "::" + OpRec->getName().str());
    } else {
      llvm_unreachable("Unknown operand type");
    }
    LSResult R(CurDefNo, Op.Types[0]);
    addSemantics(Op);
    return R;
  }

  /// Make node semantics for "set" nodes. For all defined values to be set:
  /// - if the destination is a RegisterClass/RegisterOperand, generate:
  ///     DCINS::PUT_RC, <inferred type>, <MIOperandNo of the reg>, <the value>
  /// - if the destination is an explicit Register, generate:
  ///     DCINS::PUT_REG, <inferred type>, <Target::Register name>, <the value>
  ///
  /// Keep track of the results that were dropped from the SDNode child because
  /// of SDNodeEquiv definitions.
  ///
  void flattenSet(const TreePatternNode &TPN) {
    const unsigned NumDefs = TPN.getNumChildren() - 1;
    const TreePatternNode &LastChild = *TPN.getChild(TPN.getNumChildren() - 1);

    assert(NumDefs <= LastChild.getNumTypes() &&
           "Invalid 'set': last child needs to define all the others.");

    // Visit the last (non-register) child, that defined the values for all
    // the other children.
    LSResults ChildResults = flattenSubtree(LastChild);
    // Count what the child defined, because equivalent SDNodes might not
    // define all the children.
    const unsigned NumOps = ChildResults.size();

    for (unsigned i = 0, e = NumDefs; i != e; ++i) {
      const TreePatternNode *Child = TPN.getChild(i);
      Record *OpRec = cast<DefInit>(TPN.getChild(i)->getLeafValue())->getDef();

      if (i >= NumOps) {
        assert(OpRec->isSubClassOf("Register") &&
               "Dropped implicit-def wasn't an explicit register set?");
        I.ImplicitDefs.push_back(OpRec);
        continue;
      }

      LSNode NS(TPN);

      // RegisterOperands are the same thing as RegisterClasses.
      if (OpRec->isSubClassOf("RegisterOperand"))
        OpRec = OpRec->getValueAsDef("RegClass");

      if (OpRec->isSubClassOf("RegisterClass")) {
        const CGIOperandList::OperandInfo *OpInfo =
            getNamedOperand(Child->getName());
        assert(OpInfo && "'set' output operand not found in instruction?");
        NS.Opcode = "DCINS::PUT_RC";
        NS.addOperand(utostr(OpInfo->MIOperandNo));
      } else if (OpRec->isSubClassOf("Register")) {
        NS.Opcode = "DCINS::PUT_REG";
        NS.addOperand(CGI.Namespace + "::" + OpRec->getName().str());
        I.ExplicitDefs.push_back(OpRec);
      } else {
        llvm_unreachable("SET operator should only set registers!");
      }

      LSResult Op = ChildResults[i];
      NS.addOperand(utostr(Op.DefNo));
      addSemantics(NS);
    }
  }

  /// Make node semantics from SDNodes, generate:
  ///   <SDNode opcode>, <inferred types>, <value indices for the operands>
  ///
  /// Also, try to make an equivalence between the SDNode's operator and
  /// one with less results, as defined by the SDNodeEquiv tablegen definitions.
  ///
  LSResults flattenSDNode(const TreePatternNode &TPN) {
    LSNode NS(TPN);

    Record *Operator = TPN.getOperator();
    if (Operator->isSubClassOf("ComplexPattern")) {

      if (TPN.getIntrinsicInfo(Target.CGPatterns))
        I.HasIntrinsic = true;
      I.HasComplexPattern = false;

      NS.Opcode = "DCINS::COMPLEX_PATTERN";
      NS.addOperand(CGI.Namespace + "::ComplexPattern::" +
                    sanitizeSelectFuncToEnumVal(*Operator, Target.CGPatterns));

      for (unsigned i = 0, e = TPN.getNumChildren(); i != e; ++i) {
        LSResults ChildRes = flattenSubtree(*TPN.getChild(i));
        assert(!ChildRes.empty() && "Subtree didn't defined anything?");

        // Now add one result for each child only.
        // For instance:
        //   (store (umul_lohi x, y), addr)
        // This ignores the second result of umul_lohi, and only stores the
        // first.
        NS.addOperand(utostr(ChildRes.front().DefNo));
      }

      // Now produce our results, ignoring Void "defs", and adjusting for
      // the SDNode equivalence, if necessary.
      LSResults R;
      for (unsigned i = 0, e = NS.Types.size(); i != e; ++i) {
        MVT::SimpleValueType ResVT = NS.Types[i];
        assert(ResVT < MVT::Any);
        if (ResVT != MVT::isVoid)
          R.emplace_back(CurDefNo + i, ResVT);
        if (ResVT == MVT::Untyped)
          I.HasIntrinsic = true;
      }

      addSemantics(NS);
      return R;
    }

    if (!Operator->isSubClassOf("SDNode"))
      llvm_unreachable("Unable to handle operator.");

    if (TPN.getIntrinsicInfo(Target.CGPatterns))
      I.HasIntrinsic = true;
    if (TPN.getComplexPatternInfo(Target.CGPatterns))
      I.HasComplexPattern = true;

    const SDNodeInfo &SDNI = Target.CGPatterns.getSDNodeInfo(Operator);
    NS.Opcode = SDNI.getEnumName();
    auto EquivIt = Target.SDNodeEquiv.find(Operator);
    if (EquivIt != Target.SDNodeEquiv.end()) {
      Record *EquivNode = EquivIt->second;
      const SDNodeInfo &EquivSDNI = Target.CGPatterns.getSDNodeInfo(EquivNode);
      NS.Opcode = EquivSDNI.getEnumName();
      assert((TPN.getNumTypes() - EquivSDNI.getNumResults()) > 0);
      NS.Types.resize(NS.Types.size() -
                      (TPN.getNumTypes() - EquivSDNI.getNumResults()));
    }

    ArrayRef<TreePredicateFn> Preds = TPN.getPredicateFns();
    if (!Preds.empty()) {
      Record *PredRec = Preds.back().getOrigPatFragRecord()->getRecord();
      NS.Opcode = "DCINS::PREDICATE";
      // FIXME: Once we can generate the TargetOpcode::Predicate enum once, we
      // should add a Namespace field to PatFrag to be able to distinguish
      // between targets.
      NS.addOperand("TargetOpcode::Predicate::" + PredRec->getName().str());
      Target.EncounteredPredicates.insert(PredRec);
    }

    for (unsigned i = 0, e = TPN.getNumChildren(); i != e; ++i) {
      LSResults ChildRes = flattenSubtree(*TPN.getChild(i));
      assert(!ChildRes.empty() && "Subtree didn't defined anything?");

      // Now add one result for each child only.
      // For instance:
      //   (store (umul_lohi x, y), addr)
      // This ignores the second result of umul_lohi, and only stores the first.
      NS.addOperand(utostr(ChildRes.front().DefNo));
    }

    // Now produce our results, ignoring Void "defs", and adjusting for
    // the SDNode equivalence, if necessary.
    LSResults R;
    for (unsigned i = 0, e = NS.Types.size(); i != e; ++i) {
      MVT::SimpleValueType ResVT = NS.Types[i];
      assert(ResVT < MVT::Any);
      if (ResVT != MVT::isVoid)
        R.emplace_back(CurDefNo + i, ResVT);
    }

    addSemantics(NS);
    return R;
  }

  /// Make node semantics for the whole tree \p TPN.
  LSResults flattenSubtree(const TreePatternNode &TPN) {
    if (const CGIOperandList::OperandInfo *OpInfo =
            getNamedOperand(TPN.getName())) {
      return LSResults(1, flattenOperand(TPN, OpInfo));
    }
    if (TPN.isLeaf()) {
      return LSResults(1, flattenLeaf(TPN));
    }

    return flattenSDNode(TPN);
  }

public:
  void computeImplicitDefs() {
    CodeGenRegBank &RegBank =  Target.CGTarget.getRegBank();

    SmallPtrSet<CodeGenRegister *, 4> ExplicitDefs;
    for (Record *R : I.ExplicitDefs)
      ExplicitDefs.insert(RegBank.getReg(R));

    // Remember any implicit-def we encountered (because of SDNodeEquiv),
    // or that were in the instruction description.
    SmallVector<Record *, 4> AllImplicitDefs;
    AllImplicitDefs.append(CGI.ImplicitDefs.begin(), CGI.ImplicitDefs.end());
    AllImplicitDefs.append(I.ImplicitDefs.begin(), I.ImplicitDefs.end());
    I.ImplicitDefs.clear();

    // And finally build our final list of implicit-def'd registers that weren't
    // explicitly defined anywhere in the semantics.
    SmallPtrSet<CodeGenRegister *, 4> ImplicitDefs;
    for (Record *R : AllImplicitDefs) {
      CodeGenRegister *CGR = RegBank.getReg(R);
      if (ExplicitDefs.count(CGR) == 0 &&
          ImplicitDefs.count(CGR) == 0) {
        I.ImplicitDefs.push_back(R);
        ImplicitDefs.insert(CGR);
      }
    }
  }

  void flatten(const TreePatternNode &TPN) {
    Record *Operator = TPN.getOperator();

    // Ignore "implicit" nodes: we'll handle implicit-def'd regs uniformly.
    if (Operator->getName() == "implicit")
      return;

    if (Operator->getName() == "set") {
      flattenSet(TPN);
      return;
    }

    LSResults R = flattenSDNode(TPN);

    (void)R;
    assert(R.empty() && "Top-level SDNodes can't produce results!");
  }
};

class SemanticsEmitter {
  typedef std::vector<InstSemantics> InstSemaList;

  InstSemaList InstSemas;

  // List mapping Instruction enum values to indices:
  // - first, index of the semantics in InstSemas
  // - replaced by the start offset in the generated array
  std::vector<unsigned> InstIdx;
  unsigned CurSemaOffset;

  void parseInstSemantics(unsigned InstEnumValue,
                          const CodeGenInstruction &CGI,
                          const TreePattern &TP);

public:
  SemanticsEmitter(RecordKeeper &Records);

  // run - Output the semantics.
  void run(raw_ostream &OS);

  void ParseSemantics();

  RecordKeeper &Records;
  SemanticsTarget SemaTarget;
  CodeGenDAGPatterns &CGPatterns;
  CodeGenTarget &Target;
};

SemanticsEmitter::SemanticsEmitter(RecordKeeper &Records)
    : InstSemas(), InstIdx(), CurSemaOffset(0), Records(Records),
      SemaTarget(Records), CGPatterns(SemaTarget.CGPatterns),
      Target(SemaTarget.CGTarget) {

  const std::vector<const CodeGenInstruction *> &CGIByEnum =
      Target.getInstructionsByEnumValue();
  InstIdx.resize(CGIByEnum.size(), ~0U);

  // First, look for Semantics instances.
  ParseSemantics();

  // For the rest, try to use the patterns that are in Instruction instances.
  for (unsigned i = 0, e = CGIByEnum.size(); i != e; ++i) {
    const CodeGenInstruction &CGI = *CGIByEnum[i];
    Record *TheDef = CGI.TheDef;
    const DAGInstruction &DI = CGPatterns.getInstruction(TheDef);
    if (InstIdx[i] != ~0U)
      continue;
    if (DI.getPattern() && !CGI.isCodeGenOnly)
      parseInstSemantics(i, CGI, *DI.getPattern());
  }
}

void SemanticsEmitter::parseInstSemantics(unsigned InstEnumValue,
                                          const CodeGenInstruction &CGI,
                                          const TreePattern &TP) {
  InstSemantics Sema;
  Sema.Pattern = &TP;
  LinearSemantics Flat(SemaTarget, CGI, Sema);
  for (TreePatternNode *TPN : TP.getTrees())
    Flat.flatten(*TPN);
  Flat.computeImplicitDefs();

  // Ignore semantics that would involve intrinsics or complex patterns, as
  // we don't really support either yet.
  if (Sema.HasIntrinsic || Sema.HasComplexPattern)
    return;

  // Ignore semantics that imp-def multiple registers.
  if (Sema.ImplicitDefs.size() > 1)
    return;

  // Ignore semantics that imp-def registers without defining anything else:
  // currently, we can only infer imp-defs from other defs.
  if (!Sema.ImplicitDefs.empty() && Sema.LastDefNo == ~0U)
    return;

  InstIdx[InstEnumValue] = InstSemas.size();
  InstSemas.push_back(Sema);
}

void SemanticsEmitter::ParseSemantics() {
  std::vector<Record *> Instrs = Records.getAllDerivedDefinitions("Semantics");
  const std::vector<const CodeGenInstruction *> &CGIByEnum =
      Target.getInstructionsByEnumValue();

  std::map<Record *, DAGInstruction, LessRecordByID> DAGInsts;
  for (Record *I : Instrs) {
    ListInit *LI = nullptr;

    if (isa<ListInit>(I->getValueInit("Pattern")))
      LI = I->getValueAsListInit("Pattern");

    Record *InstDef = I->getValueAsDef("Inst");

    CodeGenInstruction &CGI = Target.getInstruction(InstDef);
    const DAGInstruction &TheInst = CGPatterns.parseInstructionPattern(
        CGI, LI, DAGInsts, /*CanUseOutputOps=*/true);

    // FIXME: Instead of looking for the instruction *every* time, what about:
    // - iterating on InstructionsByEnumValue, and mapping CGI->Semantics before
    // - adding EnumValue to CGI

    auto It = std::find(CGIByEnum.begin(), CGIByEnum.end(), &CGI);
    assert(It != CGIByEnum.end() && *It == &CGI);

    parseInstSemantics(std::distance(CGIByEnum.begin(), It),
                       CGI, *TheInst.getPattern());
  }
}

void SemanticsEmitter::run(raw_ostream &OS) {
  emitSourceFileHeader("Target Instruction Semantics", OS);

  StringRef TGName = Target.getName();
  const std::vector<const CodeGenInstruction *> &CGIByEnum =
      Target.getInstructionsByEnumValue();
  assert(CGIByEnum.size() == InstIdx.size());

  CodeGenRegBank &RegBank = Target.getRegBank();

  OS << "namespace llvm {\n";
  OS << "#ifdef GET_INSTR_SEMA\n";

  std::vector<Record *> CPs = Records.getAllDerivedDefinitions("ComplexPattern");

  std::vector<std::string> CPKinds;

  for (Record *CP : CPs)
    CPKinds.push_back(sanitizeSelectFuncToEnumVal(*CP, CGPatterns));

  std::stable_sort(CPKinds.begin(), CPKinds.end());
  CPKinds.erase(std::unique(CPKinds.begin(), CPKinds.end()), CPKinds.end());

  OS << "namespace " << TGName << " {\n";
  OS << "namespace ComplexPattern {\n";
  OS << "enum {\n";
  for (auto &CPK : CPKinds)
    OS << "  " << CPK << ",\n";
  OS << "};\n} // End ComplexPattern namespace\n\n";

  // Now, emit the anonymous data structures.
  OS << "namespace {\n\n";
  OS << "const uint16_t InstSemantics[] = {\n";
  OS << "  DCINS::END_OF_INSTRUCTION,\n";
  CurSemaOffset = 1;
  for (unsigned I = 1, E = InstIdx.size(); I != E; ++I) {
    // Don't emit opcodes for instructions without semantics.
    if (InstIdx[I] == ~0U)
      continue;
    InstSemantics &Sema = InstSemas[InstIdx[I]];
    InstIdx[I] = CurSemaOffset++;
    if (Sema.Pattern) {
      OS << "  /*\n";
      Sema.Pattern->print(OS);
      OS << "  */\n";
    }

    unsigned LastDefSemaOffset = ~0U;

    for (unsigned I = 0, E = Sema.Semantics.size(); I != E; ++I) {
      if (I == Sema.LastDefSemaIdx)
        LastDefSemaOffset = CurSemaOffset;

      LSNode &NS = Sema.Semantics[I];
      OS.indent(2) << NS.Opcode;
      ++CurSemaOffset;

      // Emit the number of results/operands.
      OS << ", (" << NS.Types.size() << "<<8)|" << NS.Operands.size() << "";
      ++CurSemaOffset;

      for (auto &Ty : NS.Types)
        OS << ", " << llvm::getEnumName(Ty);
      CurSemaOffset += NS.Types.size();
      for (auto &Op : NS.Operands)
        OS << ", " << Op;
      CurSemaOffset += NS.Operands.size();
      OS << ",\n";
    }

    assert(Sema.ImplicitDefs.size() <= 1 &&
           "Can't handle multiple IMPLICITs yet!");
    if (!Sema.ImplicitDefs.empty()) {
      assert(Sema.LastDefNo != ~0U &&
             "Can't handle IMPLICIT without any other def!");
      Record *R = Sema.ImplicitDefs[0];
      OS << "  DCINS::IMPLICIT, 0, " << TGName
         << "::" << RegBank.getReg(R)->getName();
      CurSemaOffset += 3;
      OS << ",\n";
    }
    OS << "  DCINS::END_OF_INSTRUCTION,\n";
  }
  OS << "};\n\n";

  OS << "const unsigned OpcodeToSemaIdx[] = {\n";
  for (unsigned I = 0, E = InstIdx.size(); I != E; ++I) {
    unsigned Idx = InstIdx[I];
    if (Idx == ~0U)
      OS << "~0U";
    else
      OS << Idx;
    OS << ", \t// " << CGIByEnum[I]->TheDef->getName() << "\n";
  }
  OS << "};\n\n";

  assert(SemaTarget.CurConstantIdx == SemaTarget.ConstantIdx.size());
  std::vector<uint64_t> Constants(SemaTarget.CurConstantIdx);
  for (auto &CI : SemaTarget.ConstantIdx)
    Constants[CI.second - 1] = CI.first;
  OS << "const uint64_t ConstantArray[] = {\n";
  for (uint64_t Constant : Constants)
    OS.indent(2) << Constant << "ULL,\n";
  OS << "};\n\n";

  OS << "\n} // end anonymous namespace\n";
  OS << "} // end namespace " << TGName << "\n";

  // Emit the various debug dump helpers.
  OS << "StringRef " << TGName
     << "DCInstruction::getDCComplexPatternName(unsigned CPKind) const {\n"
   << "  switch(CPKind) {\n";
  for (auto &CPK : CPKinds)
    OS << "  case " << TGName << "::ComplexPattern::" << CPK << ": return \""
       << CPK << "\";\n";
  OS << "  default: return \"<unknown>\";\n  }\n}\n\n";

  OS << "StringRef " << TGName
     << "DCInstruction::getDCPredicateName(unsigned PredKind) const {\n"
     << "  switch(PredKind) {\n";
  for (auto *Pred : SemaTarget.EncounteredPredicates) {
    StringRef PredName = Pred->getName();
    OS << "  case TargetOpcode::Predicate::" << PredName << ": return \""
       << PredName << "\";\n";
  }
  OS << "  default: return \"<unknown>\";\n  }\n}\n\n";

  OS << "StringRef " << TGName
     << "DCInstruction::getDCCustomOpName(unsigned CustomOpKind) const {\n"
     << "  switch(CustomOpKind) {\n";
  for (auto *Op : Records.getAllDerivedDefinitions("Operand"))
    if (!Op->isAnonymous())
      OS << "  case " << TGName << "::OpTypes::" << Op->getName()
         << ": return \"" << Op->getName() << "\";\n";
  OS << "  default: return \"<unknown>\";\n  }\n}\n\n";

  {
    SetVector<StringRef> UniqueSDNodeNames;
    for (auto *SDNodeRec : Records.getAllDerivedDefinitions("SDNode")) {
      StringRef EnumName = CGPatterns.getSDNodeInfo(SDNodeRec).getEnumName();
      // Some targets have weird invalid SDNodes. Ignore them.
      if (!EnumName.contains("::"))
        continue;
      UniqueSDNodeNames.insert(EnumName);
    }
    OS << "StringRef " << TGName
       << "DCInstruction::getDCOpcodeName(unsigned Opcode) const {\n"
       << "  switch(Opcode) {";
    for (StringRef SDNodeName : UniqueSDNodeNames)
      OS << "  case " << SDNodeName << ":\n"
         << "    return \"" << SDNodeName << "\";\n";
    OS << "  default: return \"<unknown op>\";\n  }\n}\n";
  }
  OS << "#endif // GET_INSTR_SEMA\n";

  OS << "#ifdef GET_REGISTER_SEMA\n";
  OS << "namespace " << TGName << " {\n";
  OS << "namespace {\n\n";

  // FIXME: Also generate the rest of DCRegisterSema here
  OS << "const MVT::SimpleValueType RegClassVTs[] = {\n";
  const std::list<CodeGenRegisterClass> RCs =
      Target.getRegBank().getRegClasses();
  std::vector<const CodeGenRegisterClass*> RCByEnumValue(RCs.size());
  for (auto &RC : RCs)
    RCByEnumValue[RC.EnumValue] = &RC;
  for (auto *RC : RCByEnumValue) {
    MVT::SimpleValueType VT = RC->VTs[0];
    if (VT == MVT::x86mmx)
      VT = MVT::i64;
    OS.indent(2) << llvm::getName(VT) << ", // " << RC->getName() << " \n";
  }
  OS << "};\n\n";
  OS << "\n} // end anonymous namespace\n";
  OS << "} // end namespace " << TGName << "\n";
  OS << "#endif // GET_REGISTER_SEMA\n";

  OS << "} // end namespace llvm\n";
}

} // end anonymous namespace

SemanticsTarget::SemanticsTarget(RecordKeeper &Records)
    : Records(Records), CGPatterns(Records),
      CGTarget(CGPatterns.getTargetInfo()), SDNodeEquiv(), ConstantIdx(),
      CurConstantIdx(0) {
  for (Record *Equiv : Records.getAllDerivedDefinitions("SDNodeEquiv"))
    SDNodeEquiv[Equiv->getValueAsDef("TargetSpecific")] =
        Equiv->getValueAsDef("TargetIndependent");
}

namespace llvm {

bool EmitSemantics(RecordKeeper &Records, raw_ostream &OS) {
  SemanticsEmitter(Records).run(OS);
  return false;
}

} // end namespace llvm
