//===-------- Passes.cpp - Swift Compiler SIL Pass Entrypoints ------------===//
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
///
///  \file
///  \brief This file provides implementations of a few helper functions
///  which provide abstracted entrypoints to the SILPasses stage.
///
///  \note The actual SIL passes should be implemented in per-pass source files,
///  not in this file.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-optimizer"

#include "swift/SILPasses/Passes.h"
#include "swift/SILPasses/PassManager.h"
#include "swift/SILPasses/Transforms.h"
#include "swift/SILAnalysis/Analysis.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Module.h"
#include "swift/AST/SILOptions.h"
#include "swift/SIL/SILModule.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Debug.h"

using namespace swift;

static void registerAnalysisPasses(SILPassManager &PM, SILModule *Mod) {
  PM.registerAnalysis(createCallGraphAnalysis(Mod));
  PM.registerAnalysis(createAliasAnalysis(Mod));
  PM.registerAnalysis(createDominanceAnalysis(Mod));
}

bool swift::runSILDiagnosticPasses(SILModule &Module,
                                   const SILOptions &Options) {
  // If we parsed a .sil file that is already in canonical form, don't rerun
  // the diagnostic passes.
  if (Module.getStage() == SILStage::Canonical)
    return false;

  auto &Ctx = Module.getASTContext();

  SILPassManager PM(&Module, Options);
  registerAnalysisPasses(PM, &Module);
  // If we are asked do debug serialization, instead of running all diagnostic
  // passes, just run mandatory inlining with dead transparent function cleanup
  // disabled.
  PM.add(createMandatoryInlining());
  if (Options.DebugSerialization) {
    PM.run();
    return Ctx.hadError();
  }

  // Otherwise run the rest of diagnostics.
  PM.add(createCapturePromotion());
  PM.add(createAllocBoxToStack());
  PM.add(createInOutDeshadowing());
  PM.add(createNoReturnFolding());
  PM.add(createDefiniteInitialization());
  PM.add(createPredictableMemoryOptimizations());
  PM.add(createDiagnosticConstantPropagation());
  PM.add(createDiagnoseUnreachable());
  PM.add(createEmitDFDiagnostics());
  PM.run();

  // Generate diagnostics.
  Module.setStage(SILStage::Canonical);

  // If errors were produced during SIL analysis, return true.
  return Ctx.hadError();
}

void swift::runSILOptimizationPasses(SILModule &Module,
                                     const SILOptions &Options) {
  if (Options.DebugSerialization) {
    SILPassManager PM(&Module, Options);
    registerAnalysisPasses(PM, &Module);
    PM.add(createSILLinker());
    PM.run();
    return;
  }

  // Start by specializing generics and by cloning functions from stdlib.
  SILPassManager GenericsPM(&Module, Options);
  registerAnalysisPasses(GenericsPM, &Module);
  GenericsPM.add(createSILLinker());
  GenericsPM.add(createGenericSpecializer());
  GenericsPM.run();

  // Construct SSA and optimize it.
  SILPassManager SSAPM(&Module, Options);
  registerAnalysisPasses(SSAPM, &Module);
  SSAPM.add(createSimplifyCFG());
  SSAPM.add(createAllocBoxToStack());
  SSAPM.add(createLowerAggregate());
  SSAPM.add(createSILCombine());
  SSAPM.add(createSROA());
  SSAPM.add(createMem2Reg());

  // Perform classsic SSA optimizations.
  SSAPM.add(createPerformanceConstantPropagation());
  SSAPM.add(createDCE());
  SSAPM.add(createCSE());
  SSAPM.add(createSILCombine());
  SSAPM.add(createSimplifyCFG());

  // Perform retain/release code motion and run the first ARC optimizer.
  SSAPM.add(createLoadStoreOpts());
  SSAPM.add(createCodeMotion());
  SSAPM.add(createEnumSimplification());
  SSAPM.add(createGlobalARCOpts());

  // Devirtualize.
  SSAPM.add(createDevirtualization());
  SSAPM.add(createGenericSpecializer());
  SSAPM.add(createSILLinker());

  // Inline.
  SSAPM.add(createPerfInliner());
  SSAPM.add(createGlobalARCOpts());

  // Run three iteration of the SSA pass mananger.
  SSAPM.runOneIteration();
  SSAPM.runOneIteration();
  SSAPM.runOneIteration();

  // Perform lowering optimizations.
  SILPassManager LoweringPM(&Module, Options);
  registerAnalysisPasses(LoweringPM, &Module);
  LoweringPM.add(createDeadFunctionElimination());
  LoweringPM.add(createDeadObjectElimination());

  // Hoist globals out of loops.
  LoweringPM.add(createGlobalOpt());

  // Insert inline caches for virtual calls.
  LoweringPM.add(createDevirtualization());
  LoweringPM.add(createInlineCaches());
  LoweringPM.run();

  // Run another iteration of the SSA optimizations to optimize th
  // devirtualized inline caches.
  SSAPM.invalidateAnalysis(SILAnalysis::InvalidationKind::All);
  SSAPM.runOneIteration();

  // Invalidate the SILLoader and allow it to drop references to SIL functions.
  Module.invalidateSILLoader();
  performSILElimination(&Module);

  // Gather instruction counts if we are asked to do so.
  if (Options.PrintInstCounts) {
    SILPassManager PrinterPM(&Module, Options);
    PrinterPM.add(createSILInstCount());
    PrinterPM.runOneIteration();
  }

  DEBUG(Module.verify());
}
