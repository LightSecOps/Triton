#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "pin.H"

#include "AnalysisProcessor.h"
#include "IRBuilder.h"
#include "IRBuilderFactory.h"
#include "Inst.h"
#include "PINContextHandler.h"
#include "ProcessingPyConf.h"
#include "Trigger.h"


/* Pin options: -script */
KNOB<std::string>   KnobPythonModule(KNOB_MODE_WRITEONCE, "pintool", "script", "", "Python script");

AnalysisProcessor   ap;
Trigger             analysisTrigger = Trigger();
ProcessingPyConf    processingPyConf(&ap, &analysisTrigger);


VOID callbackBefore(IRBuilder *irb, CONTEXT *ctx, BOOL hasEA, ADDRINT ea, THREADID threadId)
{
  /* Some configurations must be applied before processing */
  processingPyConf.applyConfBeforeProcessing(irb);

  if (!analysisTrigger.getState())
  // Analysis locked
    return;

  if (hasEA)
    irb->setup(ea);

  /* Update the current context handler */
  ap.updateCurrentCtxH(new PINContextHandler(ctx, threadId));

  /* Setup Information into Irb */
  irb->setThreadID(ap.getThreadID());

  /* Python callback before IR processing */
  processingPyConf.callbackBeforeIRProc(irb, &ap);

  Inst *inst = irb->process(ap);
  ap.addInstructionToTrace(inst);

  /* Export some information from Irb to Inst */
  inst->setOpcode(irb->getOpcode());
  inst->setOpcodeCategory(irb->getOpcodeCategory());
  inst->setOperands(irb->getOperands());

  /* Python callback before instruction processing */
  processingPyConf.callbackBefore(inst, &ap);
}


VOID callbackAfter(CONTEXT *ctx, THREADID threadId)
{
  Inst *inst;

  if (!analysisTrigger.getState())
  // Analysis locked
    return;

  /* Update the current context handler */
  ap.updateCurrentCtxH(new PINContextHandler(ctx, threadId));

  /* Get the last instruction */
  inst = ap.getLastInstruction();

  /* Update statistics */
  ap.incNumberOfBranchesTaken(inst->isBranch());

  /* Python callback after instruction processing */
  processingPyConf.callbackAfter(inst, &ap);
}


VOID TRACE_Instrumentation(TRACE trace, VOID *v)
{
  for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl)){
    for (INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins)) {
      IRBuilder *irb = createIRBuilder(ins);

      /* Callback before */
      if (INS_MemoryOperandCount(ins) > 0)
        INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR) callbackBefore,
            IARG_PTR, irb,
            IARG_CONTEXT,
            IARG_BOOL, true,
            IARG_MEMORYOP_EA, 0,
            IARG_THREAD_ID,
            IARG_END);
      else
        INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR) callbackBefore,
            IARG_PTR, irb,
            IARG_CONTEXT,
            IARG_BOOL, false,
            IARG_ADDRINT, 0,
            IARG_THREAD_ID,
            IARG_END);

      /* Callback after */
      /* Syscall after context must be catcher with IDREF.CALLBACK.SYSCALL_EXIT */
      if (INS_IsSyscall(ins) == false){
        IPOINT where = IPOINT_AFTER;
        if (INS_HasFallThrough(ins) == false)
          where = IPOINT_TAKEN_BRANCH;
        INS_InsertCall(ins, where, (AFUNPTR)callbackAfter, IARG_CONTEXT, IARG_THREAD_ID, IARG_END);
      }

    }
  }
}


VOID toggleWrapper(bool flag)
{
  analysisTrigger.update(flag);
}


VOID callbackRoutineEntry(THREADID threadId, PyObject *callback)
{
  if (!analysisTrigger.getState())
  // Analysis locked
    return;
  processingPyConf.callbackRoutine(threadId, callback);
}


VOID callbackRoutineExit(THREADID threadId, PyObject *callback)
{
  if (!analysisTrigger.getState())
  // Analysis locked
    return;
  processingPyConf.callbackRoutine(threadId, callback);
}


VOID IMG_Instrumentation(IMG img, VOID *)
{
  /* Lock / Unlock the Analysis */
  if (PyTritonOptions::startAnalysisFromSymbol != nullptr){

    RTN targetRTN = RTN_FindByName(img, PyTritonOptions::startAnalysisFromSymbol);
    if (RTN_Valid(targetRTN)){
      RTN_Open(targetRTN);

      RTN_InsertCall(targetRTN,
          IPOINT_BEFORE,
          (AFUNPTR) toggleWrapper,
          IARG_BOOL, true,
          IARG_END);

      RTN_InsertCall(targetRTN,
          IPOINT_AFTER,
          (AFUNPTR) toggleWrapper,
          IARG_BOOL, false,
          IARG_END);

      RTN_Close(targetRTN);
    }
  }

  /* Callback on routien entry */
  std::map<const char *, PyObject *>::iterator it;
  for (it = PyTritonOptions::callbackRoutineEntry.begin(); it != PyTritonOptions::callbackRoutineEntry.end(); it++){
    RTN targetRTN = RTN_FindByName(img, it->first);
    if (RTN_Valid(targetRTN)){
      RTN_Open(targetRTN);
      RTN_InsertCall(targetRTN, IPOINT_BEFORE, (AFUNPTR)callbackRoutineEntry, IARG_THREAD_ID, IARG_PTR, it->second, IARG_END);
      RTN_Close(targetRTN);
    }
  }

  /* Callback on routien exit */
  for (it = PyTritonOptions::callbackRoutineExit.begin(); it != PyTritonOptions::callbackRoutineExit.end(); it++){
    RTN targetRTN = RTN_FindByName(img, it->first);
    if (RTN_Valid(targetRTN)){
      RTN_Open(targetRTN);
      RTN_InsertCall(targetRTN, IPOINT_AFTER, (AFUNPTR)callbackRoutineExit, IARG_THREAD_ID, IARG_PTR, it->second, IARG_END);
      RTN_Close(targetRTN);
    }
  }

}


VOID Fini(INT32, VOID *)
{
  /* Python callback at the end of execution */
  processingPyConf.callbackFini();

  /* End of Python */
  Py_Finalize();
}


VOID callbackSyscallEntry(THREADID threadId, CONTEXT *ctx, SYSCALL_STANDARD std, VOID *v)
{
  if (!analysisTrigger.getState())
  // Analysis locked
    return;

  /* Update the current context handler */
  ap.updateCurrentCtxH(new PINContextHandler(ctx, threadId));

  /* Python callback at the end of execution */
  processingPyConf.callbackSyscallEntry(threadId, std);
}


VOID callbackSyscallExit(THREADID threadId, CONTEXT *ctx, SYSCALL_STANDARD std, VOID *v)
{
  if (!analysisTrigger.getState())
  // Analysis locked
    return;

  /* Update the current context handler */
  ap.updateCurrentCtxH(new PINContextHandler(ctx, threadId));

  /* Python callback at the end of execution */
  processingPyConf.callbackSyscallExit(threadId, std);
}


// Usage function if Pin fail to start.
// Display the help message.
INT32 Usage()
{
  std::cerr << KNOB_BASE::StringKnobSummary() << std::endl;
  return -1;
}


int main(int argc, char *argv[])
{
  PIN_InitSymbols();
  PIN_SetSyntaxIntel();
  if(PIN_Init(argc, argv))
      return Usage();

  // Init Python Bindings
  initBindings();

  // Image callback
  IMG_AddInstrumentFunction(IMG_Instrumentation, nullptr);

  // Instruction callback
  TRACE_AddInstrumentFunction(TRACE_Instrumentation, nullptr);

  // End instrumentation callback
  PIN_AddFiniFunction(Fini, nullptr);

  // Syscall entry callback
  PIN_AddSyscallEntryFunction(callbackSyscallEntry, 0);

  // Syscall exit callback
  PIN_AddSyscallExitFunction(callbackSyscallExit, 0);

  // Exec the python bindings file
  if (!execBindings(KnobPythonModule.Value().c_str())) {
    std::cerr << "Error: Script file can't be found!" << std::endl;
    exit(1);
  }

  return 0;
}

