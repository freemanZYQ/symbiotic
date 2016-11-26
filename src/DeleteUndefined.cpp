//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.

#include <cassert>
#include <vector>
#include <set>
#include <unordered_map>

#include "llvm/IR/DataLayout.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/TypeBuilder.h"
#if (LLVM_VERSION_MINOR >= 5)
  #include "llvm/IR/InstIterator.h"
#else
  #include "llvm/Support/InstIterator.h"
#endif
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include <llvm/IR/DebugInfoMetadata.h>

using namespace llvm;

class DeleteUndefined : public FunctionPass {
  Function *_vms = nullptr; // verifier_make_symbolic function
  Type *_size_t_Ty = nullptr; // type of size_t
  bool _nosym; // do not use symbolic values when replacing

  std::unordered_map<llvm::Type *, llvm::GlobalVariable *> added_globals;

  // add global of given type and initialize it in may as nondeterministic
  GlobalVariable *getGlobalNondet(llvm::Type *, llvm::Module *);
  Function *get_verifier_make_symbolic(llvm::Module *);
  Type *get_size_t(llvm::Module *);

  void replaceCall(CallInst *CI, Module *M);
protected:
  DeleteUndefined(char id) : FunctionPass(id), _nosym(true) {}

public:
  static char ID;

  DeleteUndefined() : FunctionPass(ID), _nosym(false) {}

  virtual bool runOnFunction(Function &F);
};

static RegisterPass<DeleteUndefined> DLTU("delete-undefined",
                                          "delete calls to undefined functions, "
                                          "possible return value is made symbolic");
char DeleteUndefined::ID;

class DeleteUndefinedNoSym : public DeleteUndefined {
  public:
    static char ID;

    DeleteUndefinedNoSym() : DeleteUndefined(ID) {}
};

static RegisterPass<DeleteUndefinedNoSym> DLTUNS("delete-undefined-nosym",
                                          "delete calls to undefined functions, "
                                          "possible return value is made 0");
char DeleteUndefinedNoSym::ID;



static bool array_match(StringRef &name, const char **array)
{
  for (const char **curr = array; *curr; curr++)
    if (name.equals(*curr))
      return true;
  return false;
}

/** Clone metadata from one instruction to another
 * @param i1 the first instruction
 * @param i2 the second instruction without any metadata
*/
static void CloneMetadata(const llvm::Instruction *i1, llvm::Instruction *i2)
{
    if (!i1->hasMetadata())
        return;

    assert(!i2->hasMetadata());
    llvm::SmallVector< std::pair< unsigned, llvm::MDNode * >, 2> mds;
    i1->getAllMetadata(mds);

    for (const auto& it : mds) {
        i2->setMetadata(it.first, it.second->clone().release());
    }
}

static void CallAddMetadata(CallInst *CI, Instruction *I)
{
  if (const DISubprogram *DS = I->getParent()->getParent()->getSubprogram()) {
    // no metadata? then it is going to be the instrumentation
    // of alloca or such at the beggining of function,
    // so just add debug loc of the beginning of the function
    CI->setDebugLoc(DebugLoc::get(DS->getLine(), 0, DS));
  }
}

Function *DeleteUndefined::get_verifier_make_symbolic(llvm::Module *M)
{
  if (_vms)
    return _vms;

  LLVMContext& Ctx = M->getContext();
  //void verifier_make_symbolic(void *addr, size_t nbytes, const char *name);
  Constant *C = M->getOrInsertFunction("__VERIFIER_make_symbolic",
                                       Type::getVoidTy(Ctx),
                                       Type::getInt8PtrTy(Ctx), // addr
                                       get_size_t(M),   // nbytes
                                       Type::getInt8PtrTy(Ctx), // name
                                       nullptr);
  _vms = cast<Function>(C);
  return _vms;
}

Type *DeleteUndefined::get_size_t(llvm::Module *M)
{
  if (_size_t_Ty)
    return _size_t_Ty;

  std::unique_ptr<DataLayout> DL
    = std::unique_ptr<DataLayout>(new DataLayout(M->getDataLayout()));
  LLVMContext& Ctx = M->getContext();

  if (DL->getPointerSizeInBits() > 32)
    _size_t_Ty = Type::getInt64Ty(Ctx);
  else
    _size_t_Ty = Type::getInt32Ty(Ctx);

  return _size_t_Ty;
}

// add global of given type and initialize it in may as nondeterministic
// FIXME: use the same variables as in InitializeUninitialized
GlobalVariable *DeleteUndefined::getGlobalNondet(llvm::Type *Ty, llvm::Module *M)
{
  auto it = added_globals.find(Ty);
  if (it != added_globals.end())
    return it->second;

  LLVMContext& Ctx = M->getContext();
  GlobalVariable *G = new GlobalVariable(*M, Ty, false /* constant */,
                                         GlobalValue::PrivateLinkage,
                                         /* initializer */
                                         Constant::getNullValue(Ty),
                                         "nondet_gl_undef");

  added_globals.emplace(Ty, G);

  // insert initialization of the new global variable
  // at the beginning of main
  Function *vms = get_verifier_make_symbolic(M);
  CastInst *CastI = CastInst::CreatePointerCast(G, Type::getInt8PtrTy(Ctx));

  std::vector<Value *> args;
  //XXX: we should not build the new DL every time
  std::unique_ptr<DataLayout> DL
    = std::unique_ptr<DataLayout>(new DataLayout(M->getDataLayout()));

  args.push_back(CastI);
  args.push_back(ConstantInt::get(get_size_t(M), DL->getTypeAllocSize(Ty)));
  Constant *name = ConstantDataArray::getString(Ctx, "nondet");
  GlobalVariable *nameG = new GlobalVariable(*M, name->getType(), true /*constant */,
                                             GlobalVariable::PrivateLinkage, name);
  args.push_back(ConstantExpr::getPointerCast(nameG, Type::getInt8PtrTy(Ctx)));
  CallInst *CI = CallInst::Create(vms, args);

  Function *main = M->getFunction("main");
  assert(main && "Do not have main");
  BasicBlock& block = main->getBasicBlockList().front();
  // there must be some instruction, otherwise we would not be calling
  // this function
  Instruction& I = *(block.begin());
  CastI->insertBefore(&I);
  CI->insertBefore(&I);

  // add metadata due to the inliner pass
  CallAddMetadata(CI, &I);
  //CloneMetadata(&I, CastI);

  return G;
}


void DeleteUndefined::replaceCall(CallInst *CI, Module *M)
{
  bool modified = false;
  LLVMContext& Ctx = M->getContext();
  DataLayout *DL = new DataLayout(M->getDataLayout());
  Constant *name_init = ConstantDataArray::getString(Ctx, "nondet");
  GlobalVariable *name = new GlobalVariable(*M, name_init->getType(), true,
                                            GlobalValue::PrivateLinkage,
                                            name_init);

  Type *Ty = CI->getType();
  // we checked for this before
  assert(!Ty->isVoidTy());
  // what to do in this case?
  assert(Ty->isSized());

  LoadInst *LI = new LoadInst(getGlobalNondet(Ty, M));
  LI->insertBefore(CI);
  CI->replaceAllUsesWith(LI);

  delete DL;
}

static const char *leave_calls[] = {
  "__assert_fail",
  "abort",
  "klee_make_symbolic",
  "klee_assume",
  "klee_abort",
  "klee_silent_exit",
  "klee_report_error",
  "klee_warning_once",
  "exit",
  "_exit",
  "malloc",
  "calloc",
  "realloc",
  "free",
  "memset",
  "memcmp",
  "memcpy",
  "memmove",
  "kzalloc",
  "__errno_location",
  NULL
};

bool DeleteUndefined::runOnFunction(Function &F)
{
  // static set for the calls that we removed, so that
  // we can print those call only once
  static std::set<const llvm::Value *> removed_calls;
  bool modified = false;
  Module *M = F.getParent();

  for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E;) {
    Instruction *ins = &*I;
    ++I;
    if (CallInst *CI = dyn_cast<CallInst>(ins)) {
      if (CI->isInlineAsm())
        continue;

      const Value *val = CI->getCalledValue()->stripPointerCasts();
      const Function *callee = dyn_cast<Function>(val);
      if (!callee || callee->isIntrinsic())
        continue;

      assert(callee->hasName());
      StringRef name = callee->getName();

      if (name.equals("nondet_int") ||
          name.equals("klee_int") || array_match(name, leave_calls)) {
        continue;
      }

      // if this is __VERIFIER_something call different that to nondet,
      // keep it
      if (name.startswith("__VERIFIER_"))
        continue;

      if (callee->isDeclaration()) {
        if (removed_calls.insert(callee).second) {
          // print only once
          errs() << "Prepare: removed calls to '" << name << "' (function is undefined";
          if (!CI->getType()->isVoidTy()) {
            if (_nosym)
                errs() << ", retval set to 0)\n";
            else
                errs() << ", retval made symbolic)\n";
          } else
            errs() << ")\n";
        }

        if (!CI->getType()->isVoidTy()) {
          if (_nosym) {
            // replace the return value with 0, since we don't want
            // to use the symbolic value
            CI->replaceAllUsesWith(Constant::getNullValue(CI->getType()));
          } else
            // replace the return value with symbolic value
            replaceCall(CI, M);
        }

        CI->eraseFromParent();
        modified = true;
      }
    }
  }
  return modified;
}

