//===--- Driver.cpp - Swift compiler driver -------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file contains implementations of parts of the compiler driver.
//
//===----------------------------------------------------------------------===//

#include "swift/Driver/Driver.h"

#include "ToolChains.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsDriver.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/Range.h"
#include "swift/Basic/Statistic.h"
#include "swift/Basic/TaskQueue.h"
#include "swift/Basic/Version.h"
#include "swift/Config.h"
#include "swift/Driver/Action.h"
#include "swift/Driver/Compilation.h"
#include "swift/Driver/Job.h"
#include "swift/Driver/PrettyStackTrace.h"
#include "swift/Driver/ToolChain.h"
#include "swift/Frontend/OutputFileMap.h"
#include "swift/Option/Options.h"
#include "swift/Option/SanitizerOptions.h"
#include "swift/Parse/Lexer.h"
#include "swift/Strings.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Config/config.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/OptTable.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/raw_ostream.h"

#include "CompilationRecord.h"

#include <memory>

using namespace swift;
using namespace swift::driver;
using namespace llvm::opt;

Driver::Driver(StringRef DriverExecutable,
               StringRef Name,
               ArrayRef<const char *> Args,
               DiagnosticEngine &Diags)
  : Opts(createSwiftOptTable()), Diags(Diags),
    Name(Name), DriverExecutable(DriverExecutable),
    DefaultTargetTriple(llvm::sys::getDefaultTargetTriple()) {
      
  // The driver kind must be parsed prior to parsing arguments, since that
  // affects how arguments are parsed.
  parseDriverKind(Args.slice(1));
}

Driver::~Driver() = default;

void Driver::parseDriverKind(ArrayRef<const char *> Args) {
  // The default driver kind is determined by Name.
  StringRef DriverName = Name;

  std::string OptName;
  // However, the driver kind may be overridden if the first argument is
  // --driver-mode.
  if (!Args.empty()) {
    OptName = getOpts().getOption(options::OPT_driver_mode).getPrefixedName();

    StringRef FirstArg(Args[0]);
    if (FirstArg.startswith(OptName))
      DriverName = FirstArg.drop_front(OptName.size());
  }

  Optional<DriverKind> Kind =
  llvm::StringSwitch<Optional<DriverKind>>(DriverName)
  .Case("swift", DriverKind::Interactive)
  .Case("swiftc", DriverKind::Batch)
  .Case("swift-autolink-extract", DriverKind::AutolinkExtract)
  .Case("swift-format", DriverKind::SwiftFormat)
  .Default(None);
  
  if (Kind.hasValue())
    driverKind = Kind.getValue();
  else if (!OptName.empty())
    Diags.diagnose({}, diag::error_invalid_arg_value, OptName, DriverName);
}

ArrayRef<const char *> Driver::getArgsWithoutProgramNameAndDriverMode(
                                      ArrayRef<const char *> Args) const {
  Args = Args.slice(1);
  if (Args.empty())
    return Args;

  const std::string OptName =
    getOpts().getOption(options::OPT_driver_mode).getPrefixedName();
  if (StringRef(Args[0]).startswith(OptName))
    Args = Args.slice(1);
  return Args;
}

static void validateBridgingHeaderArgs(DiagnosticEngine &diags,
                                       const ArgList &args) {
  if (args.hasArgNoClaim(options::OPT_import_underlying_module) &&
      args.hasArgNoClaim(options::OPT_import_objc_header)) {
    diags.diagnose({}, diag::error_framework_bridging_header);
  }
}

static void validateDeploymentTarget(DiagnosticEngine &diags,
                                     const ArgList &args) {
  const Arg *A = args.getLastArg(options::OPT_target);
  if (!A)
    return;

  // Check minimum supported OS versions.
  llvm::Triple triple(llvm::Triple::normalize(A->getValue()));
  if (triple.isMacOSX()) {
    if (triple.isMacOSXVersionLT(10, 9))
      diags.diagnose(SourceLoc(), diag::error_os_minimum_deployment,
                     "OS X 10.9");
  } else if (triple.isiOS()) {
    if (triple.isTvOS()) {
      if (triple.isOSVersionLT(9, 0)) {
        diags.diagnose(SourceLoc(), diag::error_os_minimum_deployment,
                       "tvOS 9.0");
        return;
      }
    }
    if (triple.isOSVersionLT(7))
      diags.diagnose(SourceLoc(), diag::error_os_minimum_deployment,
                     "iOS 7");
    if (triple.isArch32Bit() && !triple.isOSVersionLT(11)) {
      diags.diagnose(SourceLoc(), diag::error_ios_maximum_deployment_32,
                     triple.getOSMajorVersion());
    }
  } else if (triple.isWatchOS()) {
    if (triple.isOSVersionLT(2, 0)) {
      diags.diagnose(SourceLoc(), diag::error_os_minimum_deployment,
                     "watchOS 2.0");
      return;
    }
  }
}

static void validateWarningControlArgs(DiagnosticEngine &diags,
                                       const ArgList &args) {
  if (args.hasArg(options::OPT_suppress_warnings) &&
      args.hasArg(options::OPT_warnings_as_errors)) {
    diags.diagnose(SourceLoc(), diag::error_conflicting_options,
                   "-warnings-as-errors", "-suppress-warnings");
  }
}

static void validateProfilingArgs(DiagnosticEngine &diags,
                                  const ArgList &args) {
  const Arg *ProfileGenerate = args.getLastArg(options::OPT_profile_generate);
  const Arg *ProfileUse = args.getLastArg(options::OPT_profile_use);
  if (ProfileGenerate && ProfileUse) {
    diags.diagnose(SourceLoc(), diag::error_conflicting_options,
                   "-profile-generate", "-profile-use");
  }

  // Check if the profdata is missing
  if (ProfileUse && !llvm::sys::fs::exists(ProfileUse->getValue())) {
    diags.diagnose(SourceLoc(), diag::error_profile_missing,
                   ProfileUse->getValue());
  }
}

static void validateDebugInfoArgs(DiagnosticEngine &diags,
                                  const ArgList &args) {
  // Check for missing debug option when verifying debug info.
  if (args.hasArg(options::OPT_verify_debug_info)) {
    Arg *debugOpt = args.getLastArg(swift::options::OPT_g_Group);
    if (!debugOpt || debugOpt->getOption().matches(swift::options::OPT_gnone)) {
      diags.diagnose(SourceLoc(),
                     diag::verify_debug_info_requires_debug_option);
    }
  }
}

static void validateCompilationConditionArgs(DiagnosticEngine &diags,
                                             const ArgList &args) {
  for (const Arg *A : args.filtered(options::OPT_D)) {
    StringRef name = A->getValue();
    if (name.find('=') != StringRef::npos) {
      diags.diagnose(SourceLoc(),
                     diag::cannot_assign_value_to_conditional_compilation_flag,
                     name);
    } else if (name.startswith("-D")) {
      diags.diagnose(SourceLoc(), diag::redundant_prefix_compilation_flag,
                     name);
    } else if (!Lexer::isIdentifier(name)) {
      diags.diagnose(SourceLoc(), diag::invalid_conditional_compilation_flag,
                     name);
    }
  }
}

static void validateAutolinkingArgs(DiagnosticEngine &diags,
                                    const ArgList &args) {
  auto *forceLoadArg = args.getLastArg(options::OPT_autolink_force_load);
  if (!forceLoadArg)
    return;
  auto *incrementalArg = args.getLastArg(options::OPT_incremental);
  if (!incrementalArg)
    return;

  // Note: -incremental can itself be overridden by other arguments later
  // on, but since -autolink-force-load is a rare and not-really-recommended
  // option it's not worth modeling that complexity here (or moving the
  // check somewhere else).
  diags.diagnose(SourceLoc(), diag::error_option_not_supported,
                 forceLoadArg->getSpelling(), incrementalArg->getSpelling());
}

/// Perform miscellaneous early validation of arguments.
static void validateArgs(DiagnosticEngine &diags, const ArgList &args) {
  validateBridgingHeaderArgs(diags, args);
  validateDeploymentTarget(diags, args);
  validateWarningControlArgs(diags, args);
  validateProfilingArgs(diags, args);
  validateDebugInfoArgs(diags, args);
  validateCompilationConditionArgs(diags, args);
  validateAutolinkingArgs(diags, args);
}

std::unique_ptr<ToolChain>
Driver::buildToolChain(const llvm::opt::InputArgList &ArgList) {

  if (const Arg *A = ArgList.getLastArg(options::OPT_target))
    DefaultTargetTriple = llvm::Triple::normalize(A->getValue());

  const llvm::Triple target(DefaultTargetTriple);

  switch (target.getOS()) {
  case llvm::Triple::Darwin:
  case llvm::Triple::MacOSX:
  case llvm::Triple::IOS:
  case llvm::Triple::TvOS:
  case llvm::Triple::WatchOS:
    return llvm::make_unique<toolchains::Darwin>(*this, target);
  case llvm::Triple::Linux:
    if (target.isAndroid())
      return llvm::make_unique<toolchains::Android>(*this, target);
    return llvm::make_unique<toolchains::GenericUnix>(*this, target);
  case llvm::Triple::FreeBSD:
    return llvm::make_unique<toolchains::GenericUnix>(*this, target);
  case llvm::Triple::Win32:
    if (target.isWindowsCygwinEnvironment())
      return llvm::make_unique<toolchains::Cygwin>(*this, target);
    return llvm::make_unique<toolchains::Windows>(*this, target);
  case llvm::Triple::Haiku:
    return llvm::make_unique<toolchains::GenericUnix>(*this, target);
  default:
    Diags.diagnose(SourceLoc(), diag::error_unknown_target,
                   ArgList.getLastArg(options::OPT_target)->getValue());
    break;
  }
  return nullptr;
}

static void computeArgsHash(SmallString<32> &out, const DerivedArgList &args) {
  SmallVector<const Arg *, 32> interestingArgs;
  interestingArgs.reserve(args.size());
  std::copy_if(args.begin(), args.end(), std::back_inserter(interestingArgs),
               [](const Arg *arg) {
    return !arg->getOption().hasFlag(options::DoesNotAffectIncrementalBuild) &&
           arg->getOption().getKind() != Option::InputClass;
  });

  llvm::array_pod_sort(interestingArgs.begin(), interestingArgs.end(),
                       [](const Arg * const *lhs, const Arg * const *rhs)->int {
    auto cmpID = (*lhs)->getOption().getID() - (*rhs)->getOption().getID();
    if (cmpID != 0)
      return cmpID;
    return (*lhs)->getIndex() - (*rhs)->getIndex();
  });

  llvm::MD5 hash;
  for (const Arg *arg : interestingArgs) {
    hash.update(arg->getOption().getID());
    for (const char *value : const_cast<Arg *>(arg)->getValues())
      hash.update(value);
  }

  llvm::MD5::MD5Result hashBuf;
  hash.final(hashBuf);
  llvm::MD5::stringifyResult(hashBuf, out);
}

class Driver::InputInfoMap
    : public llvm::SmallDenseMap<const Arg *, CompileJobAction::InputInfo, 16> {
};
using InputInfoMap = Driver::InputInfoMap;

/// Get the filename for build record. Returns true if failed.
/// Additionally, set 'outputBuildRecordForModuleOnlyBuild' to true if this is
/// full compilation with swiftmodule.
static bool getCompilationRecordPath(std::string &buildRecordPath,
                                     bool &outputBuildRecordForModuleOnlyBuild,
                                     const OutputInfo &OI,
                                     const Optional<OutputFileMap> &OFM,
                                     DiagnosticEngine *Diags) {
  if (!OFM) {
    // FIXME: This should work without an output file map. We should have
    // another way to specify a build record and where to put intermediates.
    if (Diags)
      Diags->diagnose(SourceLoc(), diag::incremental_requires_output_file_map);
    return true;
  }

  if (auto *masterOutputMap = OFM->getOutputMapForSingleOutput())
    buildRecordPath = masterOutputMap->lookup(file_types::TY_SwiftDeps);

  if (buildRecordPath.empty()) {
    if (Diags)
      Diags->diagnose(SourceLoc(),
                      diag::incremental_requires_build_record_entry,
                      file_types::getTypeName(file_types::TY_SwiftDeps));
    return true;
  }

  // In 'emit-module' only mode, use build-record filename suffixed with
  // '~moduleonly'. So that module-only mode doesn't mess up build-record
  // file for full compilation.
  if (OI.CompilerOutputType == file_types::TY_SwiftModuleFile) {
    buildRecordPath = buildRecordPath.append("~moduleonly");
  } else if (OI.ShouldTreatModuleAsTopLevelOutput) {
    // If we emit module along with full compilation, emit build record
    // file for '-emit-module' only mode as well.
    outputBuildRecordForModuleOnlyBuild = true;
  }

  return false;
}

static bool failedToReadOutOfDateMap(bool ShowIncrementalBuildDecisions,
                                     StringRef buildRecordPath,
                                     StringRef reason = "") {
  if (ShowIncrementalBuildDecisions) {
    llvm::outs() << "Incremental compilation has been disabled due to "
                 << "malformed build record file '" << buildRecordPath << "'.";
    if (!reason.empty()) {
      llvm::outs() << " " << reason;
    }
    llvm::outs() << "\n";
  }
  return true;
}

static bool populateOutOfDateMap(InputInfoMap &map,
                                 llvm::sys::TimePoint<> &LastBuildTime,
                                 StringRef argsHashStr,
                                 const InputFileList &inputs,
                                 StringRef buildRecordPath,
                                 bool ShowIncrementalBuildDecisions) {
  // Treat a missing file as "no previous build".
  auto buffer = llvm::MemoryBuffer::getFile(buildRecordPath);
  if (!buffer)
    return false;

  namespace yaml = llvm::yaml;
  using InputInfo = CompileJobAction::InputInfo;

  llvm::SourceMgr SM;
  yaml::Stream stream(buffer.get()->getMemBufferRef(), SM);

  auto I = stream.begin();
  if (I == stream.end() || !I->getRoot())
    return failedToReadOutOfDateMap(ShowIncrementalBuildDecisions,
                                    buildRecordPath);

  auto *topLevelMap = dyn_cast<yaml::MappingNode>(I->getRoot());
  if (!topLevelMap)
    return failedToReadOutOfDateMap(ShowIncrementalBuildDecisions,
                                    buildRecordPath);
  SmallString<64> scratch;

  llvm::StringMap<InputInfo> previousInputs;
  bool versionValid = false;
  bool optionsMatch = true;

  auto readTimeValue = [&scratch](yaml::Node *node,
                                  llvm::sys::TimePoint<> &timeValue) -> bool {
    auto *seq = dyn_cast<yaml::SequenceNode>(node);
    if (!seq)
      return true;

    auto seqI = seq->begin(), seqE = seq->end();
    if (seqI == seqE)
      return true;

    auto *secondsRaw = dyn_cast<yaml::ScalarNode>(&*seqI);
    if (!secondsRaw)
      return true;
    std::time_t parsedSeconds;
    if (secondsRaw->getValue(scratch).getAsInteger(10, parsedSeconds))
      return true;

    ++seqI;
    if (seqI == seqE)
      return true;

    auto *nanosecondsRaw = dyn_cast<yaml::ScalarNode>(&*seqI);
    if (!nanosecondsRaw)
      return true;
    std::chrono::system_clock::rep parsedNanoseconds;
    if (nanosecondsRaw->getValue(scratch).getAsInteger(10, parsedNanoseconds))
      return true;

    ++seqI;
    if (seqI != seqE)
      return true;

    timeValue = llvm::sys::TimePoint<>(std::chrono::seconds(parsedSeconds));
    timeValue += std::chrono::nanoseconds(parsedNanoseconds);
    return false;
  };

  // FIXME: LLVM's YAML support does incremental parsing in such a way that
  // for-range loops break.
  SmallString<64> CompilationRecordSwiftVersion;
  for (auto i = topLevelMap->begin(), e = topLevelMap->end(); i != e; ++i) {
    auto *key = cast<yaml::ScalarNode>(i->getKey());
    StringRef keyStr = key->getValue(scratch);

    using compilation_record::TopLevelKey;
    if (keyStr == compilation_record::getName(TopLevelKey::Version)) {
      auto *value = dyn_cast<yaml::ScalarNode>(i->getValue());
      if (!value) {
        auto reason = ("Malformed value for key '" + keyStr + "'.")
          .toStringRef(scratch);
        return failedToReadOutOfDateMap(ShowIncrementalBuildDecisions,
                                        buildRecordPath, reason);
      }

      // NB: We check against
      // swift::version::Version::getCurrentLanguageVersion() here because any
      // -swift-version argument is handled in the argsHashStr check that
      // follows.
      CompilationRecordSwiftVersion = value->getValue(scratch);
      versionValid = (CompilationRecordSwiftVersion
                      == version::getSwiftFullVersion(
                        version::Version::getCurrentLanguageVersion()));

    } else if (keyStr == compilation_record::getName(TopLevelKey::Options)) {
      auto *value = dyn_cast<yaml::ScalarNode>(i->getValue());
      if (!value)
        return true;
      optionsMatch = (argsHashStr == value->getValue(scratch));

    } else if (keyStr == compilation_record::getName(TopLevelKey::BuildTime)) {
      auto *value = dyn_cast<yaml::SequenceNode>(i->getValue());
      if (!value) {
        auto reason = ("Malformed value for key '" + keyStr + "'.")
          .toStringRef(scratch);
        return failedToReadOutOfDateMap(ShowIncrementalBuildDecisions,
                                        buildRecordPath, reason);
      }
      llvm::sys::TimePoint<> timeVal;
      if (readTimeValue(i->getValue(), timeVal))
        return true;
      LastBuildTime = timeVal;

    } else if (keyStr == compilation_record::getName(TopLevelKey::Inputs)) {
      auto *inputMap = dyn_cast<yaml::MappingNode>(i->getValue());
      if (!inputMap) {
        auto reason = ("Malformed value for key '" + keyStr + "'.")
          .toStringRef(scratch);
        return failedToReadOutOfDateMap(ShowIncrementalBuildDecisions,
                                        buildRecordPath, reason);
      }

      // FIXME: LLVM's YAML support does incremental parsing in such a way that
      // for-range loops break.
      for (auto i = inputMap->begin(), e = inputMap->end(); i != e; ++i) {
        auto *key = dyn_cast<yaml::ScalarNode>(i->getKey());
        if (!key)
          return true;

        auto *value = dyn_cast<yaml::SequenceNode>(i->getValue());
        if (!value)
          return true;

        using compilation_record::getInfoStatusForIdentifier;
        auto previousBuildState =
          getInfoStatusForIdentifier(value->getRawTag());
        if (!previousBuildState)
          return true;

        llvm::sys::TimePoint<> timeValue;
        if (readTimeValue(value, timeValue))
          return true;

        auto inputName = key->getValue(scratch);
        previousInputs[inputName] = { *previousBuildState, timeValue };
      }
    }
  }

  if (!versionValid) {
    if (ShowIncrementalBuildDecisions) {
      auto v = version::getSwiftFullVersion(
          version::Version::getCurrentLanguageVersion());
      llvm::outs() << "Incremental compilation has been disabled, due to a "
                   << "compiler version mismatch.\n"
                   << "\tCompiling with: " << v << "\n"
                   << "\tPreviously compiled with: "
                   << CompilationRecordSwiftVersion << "\n";
    }
    return true;
  }

  if (!optionsMatch) {
    if (ShowIncrementalBuildDecisions) {
      llvm::outs() << "Incremental compilation has been disabled, because "
                   << "different arguments were passed to the compiler.\n";
    }
    return true;
  }

  size_t numInputsFromPrevious = 0;
  for (auto &inputPair : inputs) {
    auto iter = previousInputs.find(inputPair.second->getValue());
    if (iter == previousInputs.end()) {
      map[inputPair.second] = InputInfo::makeNewlyAdded();
      continue;
    }
    ++numInputsFromPrevious;
    map[inputPair.second] = iter->getValue();
  }

  if (numInputsFromPrevious == previousInputs.size()) {
    return false;
  } else {
    // If a file was removed, we've lost its dependency info. Rebuild everything.
    // FIXME: Can we do better?
    if (ShowIncrementalBuildDecisions) {
      llvm::StringSet<> inputArgs;
      for (auto &inputPair : inputs) {
        inputArgs.insert(inputPair.second->getValue());
      }

      SmallVector<StringRef, 8> missingInputs;
      for (auto &previousInput : previousInputs) {
        auto previousInputArg = previousInput.getKey();
        if (inputArgs.find(previousInputArg) == inputArgs.end()) {
          missingInputs.push_back(previousInputArg);
        }
      }

      llvm::outs() << "Incremental compilation has been disabled, because "
                   << "the following inputs were used in the previous "
                   << "compilation, but not in the current compilation:\n";
      for (auto &missing : missingInputs) {
        llvm::outs() << "\t" << missing << "\n";
      }
    }
    return true;
  }
}

// warn if -embed-bitcode is set and the output type is not an object
static void validateEmbedBitcode(DerivedArgList &Args, const OutputInfo &OI,
                                 DiagnosticEngine &Diags) {
  if (Args.hasArg(options::OPT_embed_bitcode) &&
      OI.CompilerOutputType != file_types::TY_Object) {
    Diags.diagnose(SourceLoc(), diag::warn_ignore_embed_bitcode);
    Args.eraseArg(options::OPT_embed_bitcode);
  }
}

/// Gets the filelist threshold to use. Diagnoses and returns true on error.
static bool getFilelistThreshold(DerivedArgList &Args, size_t &FilelistThreshold,
                                   DiagnosticEngine &Diags) {
  FilelistThreshold = 128;

  // claim and diagnose deprecated -driver-use-filelists
  bool HasUseFilelists = Args.hasArg(options::OPT_driver_use_filelists);
  if (HasUseFilelists)
    Diags.diagnose(SourceLoc(), diag::warn_use_filelists_deprecated);

  if (const Arg *A = Args.getLastArg(options::OPT_driver_filelist_threshold)) {
    // Use the supplied threshold
    if (StringRef(A->getValue()).getAsInteger(10, FilelistThreshold)) {
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(Args), A->getValue());
      return true;
    }
  } else if (HasUseFilelists) {
    // Treat -driver-use-filelists as -driver-filelist-threshold=0
    FilelistThreshold = 0;
  } // else stick with the default

  return false;
}

static unsigned
getDriverBatchSeed(llvm::opt::InputArgList &ArgList,
                   DiagnosticEngine &Diags) {
  unsigned DriverBatchSeed = 0;
  if (const Arg *A = ArgList.getLastArg(options::OPT_driver_batch_seed)) {
    if (StringRef(A->getValue()).getAsInteger(10, DriverBatchSeed)) {
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(ArgList), A->getValue());
    }
  }
  return DriverBatchSeed;
}

static Optional<unsigned>
getDriverBatchCount(llvm::opt::InputArgList &ArgList,
                    DiagnosticEngine &Diags)
{
  if (const Arg *A = ArgList.getLastArg(options::OPT_driver_batch_count)) {
    unsigned Count = 0;
    if (StringRef(A->getValue()).getAsInteger(10, Count)) {
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(ArgList), A->getValue());
    } else {
      return Count;
    }
  }
  return None;
}

static Optional<unsigned>
getDriverBatchSizeLimit(llvm::opt::InputArgList &ArgList,
                        DiagnosticEngine &Diags)
{
  if (const Arg *A = ArgList.getLastArg(options::OPT_driver_batch_size_limit)) {
    unsigned Limit = 0;
    if (StringRef(A->getValue()).getAsInteger(10, Limit)) {
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(ArgList), A->getValue());
    } else {
      return Limit;
    }
  }
  return None;
}

std::unique_ptr<Compilation>
Driver::buildCompilation(const ToolChain &TC,
                         std::unique_ptr<llvm::opt::InputArgList> ArgList) {
  llvm::PrettyStackTraceString CrashInfo("Compilation construction");

  llvm::sys::TimePoint<> StartTime = std::chrono::system_clock::now();

  // Claim --driver-mode here, since it's already been handled.
  (void) ArgList->hasArg(options::OPT_driver_mode);

  bool DriverPrintActions = ArgList->hasArg(options::OPT_driver_print_actions);
  bool DriverPrintOutputFileMap =
    ArgList->hasArg(options::OPT_driver_print_output_file_map);
  bool DriverPrintDerivedOutputFileMap =
    ArgList->hasArg(options::OPT_driver_print_derived_output_file_map);
  DriverPrintBindings = ArgList->hasArg(options::OPT_driver_print_bindings);
  bool DriverSkipExecution =
    ArgList->hasArg(options::OPT_driver_skip_execution,
                    options::OPT_driver_print_jobs);
  bool ShowIncrementalBuildDecisions =
    ArgList->hasArg(options::OPT_driver_show_incremental);
  bool ShowJobLifecycle =
    ArgList->hasArg(options::OPT_driver_show_job_lifecycle);
  unsigned DriverBatchSeed = getDriverBatchSeed(*ArgList, Diags);
  Optional<unsigned> DriverBatchCount = getDriverBatchCount(*ArgList, Diags);
  Optional<unsigned> DriverBatchSizeLimit =
    getDriverBatchSizeLimit(*ArgList, Diags);
  bool DriverForceOneBatchRepartition =
      ArgList->hasArg(options::OPT_driver_force_one_batch_repartition);

  bool Incremental = ArgList->hasArg(options::OPT_incremental);
  if (ArgList->hasArg(options::OPT_whole_module_optimization)) {
    if (Incremental && ShowIncrementalBuildDecisions) {
      llvm::outs() << "Incremental compilation has been disabled, because it "
                   << "is not compatible with whole module optimization.";
    }
    Incremental = false;
  }
  if (ArgList->hasArg(options::OPT_embed_bitcode)) {
    if (Incremental && ShowIncrementalBuildDecisions) {
      llvm::outs() << "Incremental compilation has been disabled, because it "
                   << "is not currently compatible with embedding LLVM IR "
                   << "bitcode.";
    }
    Incremental = false;
  }

  bool SaveTemps = ArgList->hasArg(options::OPT_save_temps);
  bool ShowDriverTimeCompilation =
    ArgList->hasArg(options::OPT_driver_time_compilation);

  SmallString<128> workingDirectory;
  if (auto *A = ArgList->getLastArg(options::OPT_working_directory)) {
    workingDirectory = A->getValue();
    llvm::sys::fs::make_absolute(workingDirectory);
  }

  std::unique_ptr<DerivedArgList> TranslatedArgList(
      translateInputAndPathArgs(*ArgList, workingDirectory));

  validateArgs(Diags, *TranslatedArgList);

  if (Diags.hadAnyError())
    return nullptr;

  if (!handleImmediateArgs(*TranslatedArgList, TC)) {
    return nullptr;
  }

  // Construct the list of inputs.
  InputFileList Inputs;
  buildInputs(TC, *TranslatedArgList, Inputs);

  if (Diags.hadAnyError())
    return nullptr;

  // Determine the OutputInfo for the driver.
  OutputInfo OI;
  bool BatchMode = false;
  OI.CompilerMode = computeCompilerMode(*TranslatedArgList, Inputs, BatchMode);
  buildOutputInfo(TC, *TranslatedArgList, BatchMode, Inputs, OI);

  // Note: Batch mode handling of serialized diagnostics requires that all
  // batches get to run, in order to make sure that all diagnostics emitted
  // during the compilation end up in at least one serialized diagnostic file.
  // Therefore, treat batch mode as implying -continue-building-after-errors.
  // (This behavior could be limited to only when serialized diagnostics are
  // being emitted, but this seems more consistent and less surprising for
  // users.)
  // FIXME: We don't really need (or want) a full ContinueBuildingAfterErrors.
  // If we fail to precompile a bridging header, for example, there's no need
  // to go on to compilation of source files, and if compilation of source files
  // fails, we shouldn't try to link. Instead, we'd want to let all jobs finish
  // but not schedule any new ones.
  const bool ContinueBuildingAfterErrors =
      BatchMode || ArgList->hasArg(options::OPT_continue_building_after_errors);

  if (Diags.hadAnyError())
    return nullptr;

  std::unique_ptr<UnifiedStatsReporter> StatsReporter;
  if (const Arg *A =
      ArgList->getLastArgNoClaim(options::OPT_stats_output_dir)) {
    StringRef OptType;
    if (const Arg *OptA = ArgList->getLastArgNoClaim(options::OPT_O_Group)) {
      OptType = OptA->getSpelling();
    }
    StringRef InputName;
    if (Inputs.size() == 1) {
      InputName = Inputs[0].second->getSpelling();
    }
    StringRef OutputType = file_types::getTypeTempSuffix(OI.CompilerOutputType);
    StatsReporter = llvm::make_unique<UnifiedStatsReporter>("swift-driver",
                                                            OI.ModuleName,
                                                            InputName,
                                                            DefaultTargetTriple,
                                                            OutputType,
                                                            OptType,
                                                            A->getValue());
  }

  assert(OI.CompilerOutputType != file_types::ID::TY_INVALID &&
         "buildOutputInfo() must set a valid output type!");

  validateEmbedBitcode(*TranslatedArgList, OI, Diags);

  if (OI.CompilerMode == OutputInfo::Mode::REPL)
    // REPL mode expects no input files, so suppress the error.
    SuppressNoInputFilesError = true;

  Optional<OutputFileMap> OFM =
      buildOutputFileMap(*TranslatedArgList, workingDirectory);

  if (Diags.hadAnyError())
    return nullptr;

  if (DriverPrintOutputFileMap) {
    if (OFM)
      OFM->dump(llvm::errs(), true);
    else
      Diags.diagnose(SourceLoc(), diag::error_no_output_file_map_specified);
    return nullptr;
  }

  std::string buildRecordPath;
  bool outputBuildRecordForModuleOnlyBuild = false;
  getCompilationRecordPath(buildRecordPath, outputBuildRecordForModuleOnlyBuild,
                           OI, OFM, Incremental ? &Diags : nullptr);

  SmallString<32> ArgsHash;
  computeArgsHash(ArgsHash, *TranslatedArgList);
  llvm::sys::TimePoint<> LastBuildTime = llvm::sys::TimePoint<>::min();
  InputInfoMap outOfDateMap;
  bool rebuildEverything = true;
  if (Incremental && !buildRecordPath.empty()) {
    if (populateOutOfDateMap(outOfDateMap, LastBuildTime, ArgsHash, Inputs,
                             buildRecordPath, ShowIncrementalBuildDecisions)) {
      // FIXME: Distinguish errors from "file removed", which is benign.
    } else {
      rebuildEverything = false;
    }
  }

  unsigned NumberOfParallelCommands = 1;
  if (const Arg *A = ArgList->getLastArg(options::OPT_j)) {
    if (StringRef(A->getValue()).getAsInteger(10, NumberOfParallelCommands)) {
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(*ArgList), A->getValue());
      return nullptr;
    }
  }

  size_t DriverFilelistThreshold;
  if (getFilelistThreshold(*TranslatedArgList, DriverFilelistThreshold, Diags))
    return nullptr;

  OutputLevel Level = OutputLevel::Normal;
  if (const Arg *A =
          ArgList->getLastArg(options::OPT_driver_print_jobs, options::OPT_v,
                              options::OPT_parseable_output)) {
    if (A->getOption().matches(options::OPT_driver_print_jobs))
      Level = OutputLevel::PrintJobs;
    else if (A->getOption().matches(options::OPT_v))
      Level = OutputLevel::Verbose;
    else if (A->getOption().matches(options::OPT_parseable_output))
      Level = OutputLevel::Parseable;
    else
      llvm_unreachable("Unknown OutputLevel argument!");
  }

  std::unique_ptr<Compilation> C(
      new Compilation(Diags, TC, OI, Level,
                      std::move(ArgList),
                      std::move(TranslatedArgList),
                      std::move(Inputs),
                      buildRecordPath,
                      outputBuildRecordForModuleOnlyBuild,
                      ArgsHash,
                      StartTime,
                      LastBuildTime,
                      DriverFilelistThreshold,
                      NumberOfParallelCommands,
                      Incremental,
                      BatchMode,
                      DriverBatchSeed,
                      DriverBatchCount,
                      DriverBatchSizeLimit,
                      DriverForceOneBatchRepartition,
                      DriverSkipExecution,
                      SaveTemps,
                      ShowDriverTimeCompilation,
                      std::move(StatsReporter)));

  // Construct the graph of Actions.
  SmallVector<const Action *, 8> TopLevelActions;
  buildActions(TopLevelActions, TC, OI, OFM ? OFM.getPointer() : nullptr,
               rebuildEverything ? nullptr : &outOfDateMap, *C);

  if (Diags.hadAnyError())
    return nullptr;

  if (DriverPrintActions) {
    printActions(*C);
    return nullptr;
  }

  buildJobs(TopLevelActions, OI, OFM ? OFM.getPointer() : nullptr,
            workingDirectory, TC, *C);

  if (DriverPrintDerivedOutputFileMap) {
    C->getDerivedOutputFileMap().dump(llvm::outs(), true);
    return nullptr;
  }

  // For getting bulk fixits, or for when users explicitly request to continue
  // building despite errors.
  if (ContinueBuildingAfterErrors)
    C->setContinueBuildingAfterErrors();

  if (ShowIncrementalBuildDecisions || ShowJobLifecycle)
    C->setShowsIncrementalBuildDecisions();

  if (ShowJobLifecycle)
    C->setShowJobLifecycle();

  // This has to happen after building jobs, because otherwise we won't even
  // emit .swiftdeps files for the next build.
  if (rebuildEverything)
    C->disableIncrementalBuild();

  if (Diags.hadAnyError())
    return nullptr;

  if (DriverPrintBindings)
    return nullptr;

  return C;
}

static Arg *makeInputArg(const DerivedArgList &Args, OptTable &Opts,
                         StringRef Value) {
  Arg *A = new Arg(Opts.getOption(options::OPT_INPUT), Value,
                   Args.getBaseArgs().MakeIndex(Value), Value.data());
  A->claim();
  return A;
}

using RemainingArgsHandler = std::function<void(InputArgList &, unsigned)>;

std::unique_ptr<InputArgList>
parseArgsUntil(const llvm::opt::OptTable& Opts,
               const char *const *ArgBegin,
               const char *const *ArgEnd,
               unsigned &MissingArgIndex,
               unsigned &MissingArgCount,
               unsigned FlagsToInclude,
               unsigned FlagsToExclude,
               llvm::opt::OptSpecifier UntilOption,
               RemainingArgsHandler RemainingHandler) {
  auto Args = llvm::make_unique<InputArgList>(ArgBegin, ArgEnd);

  // FIXME: Handle '@' args (or at least error on them).

  bool CheckUntil = UntilOption != options::OPT_INVALID;
  MissingArgIndex = MissingArgCount = 0;
  unsigned Index = 0, End = ArgEnd - ArgBegin;
  while (Index < End) {
    // Ignore empty arguments (other things may still take them as arguments).
    StringRef Str = Args->getArgString(Index);
    if (Str == "") {
      ++Index;
      continue;
    }

    unsigned Prev = Index;
    Arg *A = Opts.ParseOneArg(*Args, Index, FlagsToInclude, FlagsToExclude);
    assert(Index > Prev && "Parser failed to consume argument.");

    // Check for missing argument error.
    if (!A) {
      assert(Index >= End && "Unexpected parser error.");
      assert(Index - Prev - 1 && "No missing arguments!");
      MissingArgIndex = Prev;
      MissingArgCount = Index - Prev - 1;
      break;
    }

    Args->append(A);

    if (CheckUntil && A->getOption().matches(UntilOption)) {
      if (Index < End)
        RemainingHandler(*Args, Index);
      return Args;
    }
  }

  return Args;
}

// Parse all args until we see an input, and then collect the remaining
// arguments into a synthesized "--" option.
static std::unique_ptr<InputArgList>
parseArgStringsForInteractiveDriver(const llvm::opt::OptTable& Opts,
                                    ArrayRef<const char *> Args,
                                    unsigned &MissingArgIndex,
                                    unsigned &MissingArgCount,
                                    unsigned FlagsToInclude,
                                    unsigned FlagsToExclude) {
  return parseArgsUntil(Opts, Args.begin(), Args.end(), MissingArgIndex,
                        MissingArgCount, FlagsToInclude, FlagsToExclude,
                        options::OPT_INPUT,
                        [&](InputArgList &Args, unsigned NextIndex) {
    assert(NextIndex < Args.getNumInputArgStrings());
    // Synthesize -- remaining args...
    Arg *Remaining =
        new Arg(Opts.getOption(options::OPT__DASH_DASH), "--", NextIndex);
    for (unsigned N = Args.getNumInputArgStrings(); NextIndex != N;
         ++NextIndex) {
      Remaining->getValues().push_back(Args.getArgString(NextIndex));
    }
    Args.append(Remaining);
  });
}

std::unique_ptr<InputArgList>
Driver::parseArgStrings(ArrayRef<const char *> Args) {
  unsigned IncludedFlagsBitmask = 0;
  unsigned ExcludedFlagsBitmask = options::NoDriverOption;
  unsigned MissingArgIndex, MissingArgCount;
  std::unique_ptr<InputArgList> ArgList;

  if (driverKind == DriverKind::Interactive) {
    ArgList = parseArgStringsForInteractiveDriver(getOpts(), Args,
        MissingArgIndex, MissingArgCount, IncludedFlagsBitmask,
        ExcludedFlagsBitmask);

  } else {
    ArgList = llvm::make_unique<InputArgList>(
        getOpts().ParseArgs(Args, MissingArgIndex, MissingArgCount,
                            IncludedFlagsBitmask, ExcludedFlagsBitmask));
  }

  assert(ArgList && "no argument list");

  // Check for missing argument error.
  if (MissingArgCount) {
    Diags.diagnose(SourceLoc(), diag::error_missing_arg_value,
                   ArgList->getArgString(MissingArgIndex), MissingArgCount);
    return nullptr;
  }

  // Check for unknown arguments.
  for (const Arg *A :  ArgList->filtered(options::OPT_UNKNOWN)) {
    Diags.diagnose(SourceLoc(), diag::error_unknown_arg,
                   A->getAsString(*ArgList));
  }

  // Check for unsupported options
  unsigned UnsupportedFlag = 0;
  if (driverKind == DriverKind::Interactive)
    UnsupportedFlag = options::NoInteractiveOption;
  else if (driverKind == DriverKind::Batch)
    UnsupportedFlag = options::NoBatchOption;

  if (UnsupportedFlag)
    for (const Arg *A : *ArgList)
      if (A->getOption().hasFlag(UnsupportedFlag))
        Diags.diagnose(SourceLoc(), diag::error_unsupported_option,
            ArgList->getArgString(A->getIndex()), Name,
            UnsupportedFlag == options::NoBatchOption ? "swift" : "swiftc");

  return ArgList;
}

DerivedArgList *
Driver::translateInputAndPathArgs(const InputArgList &ArgList,
                                  StringRef workingDirectory) const {
  DerivedArgList *DAL = new DerivedArgList(ArgList);

  auto addPath = [workingDirectory, DAL](Arg *A) {
    assert(A->getNumValues() == 1 && "multiple values not handled");
    StringRef path = A->getValue();
    if (workingDirectory.empty() || path == "-" ||
        llvm::sys::path::is_absolute(path)) {
      DAL->append(A);
      return;
    }

    SmallString<64> fullPath{workingDirectory};
    llvm::sys::path::append(fullPath, path);
    unsigned index = DAL->getBaseArgs().MakeIndex(fullPath);
    Arg *newArg = new Arg(A->getOption(), A->getSpelling(), index,
                          DAL->getBaseArgs().getArgString(index), A);
    DAL->AddSynthesizedArg(newArg);
    DAL->append(newArg);
  };

  for (Arg *A : ArgList) {
    if (A->getOption().hasFlag(options::ArgumentIsPath) ||
        A->getOption().matches(options::OPT_INPUT)) {
      addPath(A);
      continue;
    }

    // If we're not in immediate mode, pick up inputs via the -- option.
    if (driverKind != DriverKind::Interactive && A->getOption().matches(options::OPT__DASH_DASH)) {
      A->claim();
      for (unsigned i = 0, e = A->getNumValues(); i != e; ++i) {
        addPath(makeInputArg(*DAL, *Opts, A->getValue(i)));
      }
      continue;
    }

    DAL->append(A);
  }

  return DAL;
}

/// \brief Check that the file referenced by \p Input exists. If it doesn't,
/// issue a diagnostic and return false.
static bool checkInputExistence(const Driver &D, const DerivedArgList &Args,
                                DiagnosticEngine &Diags, StringRef Input) {
  if (!D.getCheckInputFilesExist())
    return true;

  // stdin always exists.
  if (Input == "-")
    return true;

  if (llvm::sys::fs::exists(Input))
    return true;

  Diags.diagnose(SourceLoc(), diag::error_no_such_file_or_directory, Input);
  return false;
}

void Driver::buildInputs(const ToolChain &TC,
                         const DerivedArgList &Args,
                         InputFileList &Inputs) const {
  llvm::StringMap<StringRef> SourceFileNames;

  for (Arg *A : Args) {
    if (A->getOption().getKind() == Option::InputClass) {
      StringRef Value = A->getValue();
      file_types::ID Ty = file_types::TY_INVALID;

      // stdin must be handled specially.
      if (Value.equals("-")) {
        // By default, treat stdin as Swift input.
        Ty = file_types::TY_Swift;
      } else {
        // Otherwise lookup by extension.
        Ty = TC.lookupTypeForExtension(llvm::sys::path::extension(Value));

        if (Ty == file_types::TY_INVALID) {
          // By default, treat inputs with no extension, or with an
          // extension that isn't recognized, as object files.
          Ty = file_types::TY_Object;
        }
      }

      if (checkInputExistence(*this, Args, Diags, Value))
        Inputs.push_back(std::make_pair(Ty, A));

      if (Ty == file_types::TY_Swift) {
        StringRef Basename = llvm::sys::path::filename(Value);
        if (!SourceFileNames.insert({Basename, Value}).second) {
          Diags.diagnose(SourceLoc(), diag::error_two_files_same_name,
                         Basename, SourceFileNames[Basename], Value);
          Diags.diagnose(SourceLoc(), diag::note_explain_two_files_same_name);
        }
      }
    }
  }
}

static bool maybeBuildingExecutable(const OutputInfo &OI,
                                    const DerivedArgList &Args,
                                    const InputFileList &Inputs) {
  switch (OI.LinkAction) {
  case LinkKind::Executable:
    return true;
  case LinkKind::DynamicLibrary:
    return false;
  case LinkKind::None:
    break;
  }

  if (Args.hasArg(options::OPT_parse_as_library, options::OPT_parse_stdlib))
    return false;
  return Inputs.size() == 1;
}

static void diagnoseOutputModeArg(DiagnosticEngine &diags, const Arg *arg,
                                  bool hasInputs, const DerivedArgList &args,
                                  bool isInteractiveDriver,
                                  StringRef driverName) {
  switch (arg->getOption().getID()) {

  case options::OPT_i:
    diags.diagnose(SourceLoc(), diag::error_i_mode,
                   isInteractiveDriver ? driverName : "swift");
    break;

  case options::OPT_repl:
    if (isInteractiveDriver && !hasInputs)
      diags.diagnose(SourceLoc(), diag::warning_unnecessary_repl_mode,
                     args.getArgString(arg->getIndex()), driverName);
    break;

  default:
    break;
  }
}

static bool isSDKTooOld(StringRef sdkPath, clang::VersionTuple minVersion,
                        StringRef firstPrefix, StringRef secondPrefix = {}) {
  // FIXME: This is a hack.
  // We should be looking at the SDKSettings.plist.
  StringRef sdkDirName = llvm::sys::path::filename(sdkPath);

  size_t versionStart = sdkDirName.rfind(firstPrefix);
  if (versionStart != StringRef::npos) {
    versionStart += firstPrefix.size();
  } else if (!secondPrefix.empty()) {
    versionStart = sdkDirName.rfind(secondPrefix);
    if (versionStart != StringRef::npos)
      versionStart += secondPrefix.size();
  }
  if (versionStart == StringRef::npos)
    return false;

  size_t versionEnd = sdkDirName.rfind(".Internal");
  if (versionEnd == StringRef::npos)
    versionEnd = sdkDirName.rfind(".sdk");
  if (versionEnd == StringRef::npos)
    return false;

  clang::VersionTuple version;
  if (version.tryParse(sdkDirName.slice(versionStart, versionEnd)))
    return false;
  return version < minVersion;
}

/// Returns true if the given SDK path points to an SDK that is too old for
/// the given target.
static bool isSDKTooOld(StringRef sdkPath, const llvm::Triple &target) {
  if (target.isMacOSX()) {
    return isSDKTooOld(sdkPath, clang::VersionTuple(10, 14), "OSX");

  } else if (target.isiOS()) {
    // Includes both iOS and TVOS.
    return isSDKTooOld(sdkPath, clang::VersionTuple(12, 0), "Simulator", "OS");

  } else if (target.isWatchOS()) {
    return isSDKTooOld(sdkPath, clang::VersionTuple(5, 0), "Simulator", "OS");

  } else {
    return false;
  }
}

void Driver::buildOutputInfo(const ToolChain &TC, const DerivedArgList &Args,
                             const bool BatchMode, const InputFileList &Inputs,
                             OutputInfo &OI) const {
  // By default, the driver does not link its output; this will be updated
  // appropriately below if linking is required.

  OI.CompilerOutputType = driverKind == DriverKind::Interactive
                              ? file_types::TY_Nothing
                              : file_types::TY_Object;

  if (const Arg *A = Args.getLastArg(options::OPT_num_threads)) {
    if (BatchMode) {
      Diags.diagnose(SourceLoc(), diag::warning_cannot_multithread_batch_mode);
    } else if (StringRef(A->getValue()).getAsInteger(10, OI.numThreads)) {
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(Args), A->getValue());
    }
  }

  const Arg *const OutputModeArg = Args.getLastArg(options::OPT_modes_Group);

  if (!OutputModeArg) {
    if (Args.hasArg(options::OPT_emit_module, options::OPT_emit_module_path)) {
      OI.CompilerOutputType = file_types::TY_SwiftModuleFile;
    } else if (driverKind != DriverKind::Interactive) {
      OI.LinkAction = LinkKind::Executable;
    }
  } else {
    diagnoseOutputModeArg(Diags, OutputModeArg, !Inputs.empty(), Args,
                          driverKind == DriverKind::Interactive, Name);

    switch (OutputModeArg->getOption().getID()) {
    case options::OPT_emit_executable:
      OI.LinkAction = LinkKind::Executable;
      OI.CompilerOutputType = file_types::TY_Object;
      break;

    case options::OPT_emit_library:
      OI.LinkAction = LinkKind::DynamicLibrary;
      OI.CompilerOutputType = file_types::TY_Object;
      break;

    case options::OPT_emit_object:
      OI.CompilerOutputType = file_types::TY_Object;
      break;

    case options::OPT_emit_assembly:
      OI.CompilerOutputType = file_types::TY_Assembly;
      break;

    case options::OPT_emit_sil:
      OI.CompilerOutputType = file_types::TY_SIL;
      break;

    case options::OPT_emit_silgen:
      OI.CompilerOutputType = file_types::TY_RawSIL;
      break;

    case options::OPT_emit_sib:
      OI.CompilerOutputType = file_types::TY_SIB;
      break;

    case options::OPT_emit_sibgen:
      OI.CompilerOutputType = file_types::TY_RawSIB;
      break;

    case options::OPT_emit_ir:
      OI.CompilerOutputType = file_types::TY_LLVM_IR;
      break;

    case options::OPT_emit_bc:
      OI.CompilerOutputType = file_types::TY_LLVM_BC;
      break;

    case options::OPT_emit_pch:
      OI.CompilerMode = OutputInfo::Mode::SingleCompile;
      OI.CompilerOutputType = file_types::TY_PCH;
      break;

    case options::OPT_emit_imported_modules:
      OI.CompilerOutputType = file_types::TY_ImportedModules;
      // We want the imported modules from the module as a whole, not individual
      // files, so let's do it in one invocation rather than having to collate
      // later.
      OI.CompilerMode = OutputInfo::Mode::SingleCompile;
      break;

    case options::OPT_index_file:
      OI.CompilerMode = OutputInfo::Mode::SingleCompile;
      OI.CompilerOutputType = file_types::TY_IndexData;
      break;

    case options::OPT_update_code:
      OI.CompilerOutputType = file_types::TY_Remapping;
      OI.LinkAction = LinkKind::None;
      break;
    case options::OPT_parse:
    case options::OPT_typecheck:
    case options::OPT_dump_parse:
    case options::OPT_dump_ast:
    case options::OPT_emit_syntax:
    case options::OPT_print_ast:
    case options::OPT_dump_type_refinement_contexts:
    case options::OPT_dump_scope_maps:
    case options::OPT_dump_interface_hash:
    case options::OPT_verify_debug_info:
      OI.CompilerOutputType = file_types::TY_Nothing;
      break;

    case options::OPT_i:
      // Keep the default output/mode; this flag was removed and should already
      // have been diagnosed above.
      assert(Diags.hadAnyError() && "-i flag was removed");
      break;

    case options::OPT_repl:
    case options::OPT_deprecated_integrated_repl:
    case options::OPT_lldb_repl:
      OI.CompilerOutputType = file_types::TY_Nothing;
      OI.CompilerMode = OutputInfo::Mode::REPL;
      break;

    default:
      llvm_unreachable("unknown mode");
    }
  }

  assert(OI.CompilerOutputType != file_types::ID::TY_INVALID);

  if (const Arg *A = Args.getLastArg(options::OPT_g_Group)) {
    if (A->getOption().matches(options::OPT_g))
      OI.DebugInfoKind = IRGenDebugInfoKind::Normal;
    else if (A->getOption().matches(options::OPT_gline_tables_only))
      OI.DebugInfoKind = IRGenDebugInfoKind::LineTables;
    else if (A->getOption().matches(options::OPT_gdwarf_types))
      OI.DebugInfoKind = IRGenDebugInfoKind::DwarfTypes;
    else
      assert(A->getOption().matches(options::OPT_gnone) &&
             "unknown -g<kind> option");
  }

  if (Args.hasArg(options::OPT_emit_module, options::OPT_emit_module_path)) {
    // The user has requested a module, so generate one and treat it as
    // top-level output.
    OI.ShouldGenerateModule = true;
    OI.ShouldTreatModuleAsTopLevelOutput = true;
  } else if ((OI.DebugInfoKind > IRGenDebugInfoKind::LineTables &&
              OI.shouldLink()) ||
             Args.hasArg(options::OPT_emit_objc_header,
                         options::OPT_emit_objc_header_path)) {
    // An option has been passed which requires a module, but the user hasn't
    // requested one. Generate a module, but treat it as an intermediate output.
    OI.ShouldGenerateModule = true;
    OI.ShouldTreatModuleAsTopLevelOutput = false;
  } else {
    // No options require a module, so don't generate one.
    OI.ShouldGenerateModule = false;
    OI.ShouldTreatModuleAsTopLevelOutput = false;
  }

  if (OI.ShouldGenerateModule &&
      (OI.CompilerMode == OutputInfo::Mode::REPL ||
       OI.CompilerMode == OutputInfo::Mode::Immediate)) {
    Diags.diagnose(SourceLoc(), diag::error_mode_cannot_emit_module);
    return;
  }

  if (const Arg *A = Args.getLastArg(options::OPT_module_name)) {
    OI.ModuleName = A->getValue();
  } else if (OI.CompilerMode == OutputInfo::Mode::REPL) {
    // REPL mode should always use the REPL module.
    OI.ModuleName = "REPL";
  } else if (const Arg *A = Args.getLastArg(options::OPT_o)) {
    OI.ModuleName = llvm::sys::path::stem(A->getValue());
    if (OI.LinkAction == LinkKind::DynamicLibrary &&
        !llvm::sys::path::extension(A->getValue()).empty() &&
        StringRef(OI.ModuleName).startswith("lib")) {
      // Chop off a "lib" prefix if we're building a library.
      OI.ModuleName.erase(0, strlen("lib"));
    }
  } else if (Inputs.size() == 1) {
    OI.ModuleName = llvm::sys::path::stem(Inputs.front().second->getValue());
  }

  if (!Lexer::isIdentifier(OI.ModuleName) ||
      (OI.ModuleName == STDLIB_NAME &&
       !Args.hasArg(options::OPT_parse_stdlib))) {
    OI.ModuleNameIsFallback = true;
    if (OI.CompilerOutputType == file_types::TY_Nothing ||
        maybeBuildingExecutable(OI, Args, Inputs))
      OI.ModuleName = "main";
    else if (!Inputs.empty() || OI.CompilerMode == OutputInfo::Mode::REPL) {
      // Having an improper module name is only bad if we have inputs or if
      // we're in REPL mode.
      auto DID = (OI.ModuleName == STDLIB_NAME) ? diag::error_stdlib_module_name
                                                : diag::error_bad_module_name;
      Diags.diagnose(SourceLoc(), DID,
                     OI.ModuleName, !Args.hasArg(options::OPT_module_name));
      OI.ModuleName = "__bad__";
    }
  }

  {
    if (const Arg *A = Args.getLastArg(options::OPT_sdk)) {
      OI.SDKPath = A->getValue();
    } else if (const char *SDKROOT = getenv("SDKROOT")) {
      OI.SDKPath = SDKROOT;
    } else if (OI.CompilerMode == OutputInfo::Mode::Immediate ||
               OI.CompilerMode == OutputInfo::Mode::REPL) {
      if (TC.getTriple().isMacOSX()) {
        // In immediate modes, use the SDK provided by xcrun.
        // This will prefer the SDK alongside the Swift found by "xcrun swift".
        // We don't do this in compilation modes because defaulting to the
        // latest SDK may not be intended.
        auto xcrunPath = llvm::sys::findProgramByName("xcrun");
        if (!xcrunPath.getError()) {
          const char *args[] = {
            "--show-sdk-path", "--sdk", "macosx", nullptr
          };
          sys::TaskQueue queue;
          queue.addTask(xcrunPath->c_str(), args);
          queue.execute(nullptr,
                        [&OI](sys::ProcessId PID,
                              int returnCode,
                              StringRef output,
                              StringRef errors,
                              void *unused) -> sys::TaskFinishedResponse {
            if (returnCode == 0) {
              output = output.rtrim();
              auto lastLineStart = output.find_last_of("\n\r");
              if (lastLineStart != StringRef::npos)
                output = output.substr(lastLineStart+1);
              if (output.empty())
                OI.SDKPath = "/";
              else
                OI.SDKPath = output.str();
            }
            return sys::TaskFinishedResponse::ContinueExecution;
          });
        }
      }
    }

    if (!OI.SDKPath.empty()) {
      // Delete a trailing /.
      if (OI.SDKPath.size() > 1 &&
          llvm::sys::path::is_separator(OI.SDKPath.back())) {
        OI.SDKPath.erase(OI.SDKPath.end()-1);
      }

      if (!llvm::sys::fs::exists(OI.SDKPath)) {
        Diags.diagnose(SourceLoc(), diag::warning_no_such_sdk, OI.SDKPath);
      } else if (isSDKTooOld(OI.SDKPath, TC.getTriple())) {
        Diags.diagnose(SourceLoc(), diag::error_sdk_too_old,
                       llvm::sys::path::filename(OI.SDKPath));
      }
    }
  }

  if (const Arg *A = Args.getLastArg(options::OPT_sanitize_EQ))
    OI.SelectedSanitizers = parseSanitizerArgValues(
        Args, A, TC.getTriple(), Diags,
        [&](StringRef sanitizerName, bool shared) {
          return TC.sanitizerRuntimeLibExists(Args, sanitizerName, shared);
        });

  if (const Arg *A = Args.getLastArg(options::OPT_sanitize_coverage_EQ)) {

    // Check that the sanitizer coverage flags are supported if supplied.
    // Dismiss the output, as we will grab the value later.
    (void)parseSanitizerCoverageArgValue(A, TC.getTriple(), Diags,
                                         OI.SelectedSanitizers);

  }

}

OutputInfo::Mode
Driver::computeCompilerMode(const DerivedArgList &Args,
                            const InputFileList &Inputs,
                            bool &BatchModeOut) const {

  if (driverKind == Driver::DriverKind::Interactive)
    return Inputs.empty() ? OutputInfo::Mode::REPL
                          : OutputInfo::Mode::Immediate;

  const Arg *ArgRequiringSingleCompile = Args.getLastArg(
      options::OPT_whole_module_optimization, options::OPT_index_file);

  BatchModeOut = Args.hasFlag(options::OPT_enable_batch_mode,
                              options::OPT_disable_batch_mode,
                              false);

  if (!ArgRequiringSingleCompile)
    return OutputInfo::Mode::StandardCompile;

  // Override batch mode if given -wmo or -index-file.
  if (BatchModeOut) {
    BatchModeOut = false;
    // Emit a warning about such overriding (FIXME: we might conditionalize
    // this based on the user or xcode passing -disable-batch-mode).
    Diags.diagnose(SourceLoc(), diag::warn_ignoring_batch_mode,
                   ArgRequiringSingleCompile->getOption().getPrefixedName());
  }
  return OutputInfo::Mode::SingleCompile;
}

void Driver::buildActions(SmallVectorImpl<const Action *> &TopLevelActions,
                          const ToolChain &TC, const OutputInfo &OI,
                          const OutputFileMap *OFM,
                          const InputInfoMap *OutOfDateMap,
                          Compilation &C) const {
  const DerivedArgList &Args = C.getArgs();
  ArrayRef<InputPair> Inputs = C.getInputFiles();

  if (!SuppressNoInputFilesError && Inputs.empty()) {
    Diags.diagnose(SourceLoc(), diag::error_no_input_files);
    return;
  }

  SmallVector<const Action *, 2> AllModuleInputs;
  SmallVector<const Action *, 2> AllLinkerInputs;

  switch (OI.CompilerMode) {
  case OutputInfo::Mode::StandardCompile: {

    // If the user is importing a textual (.h) bridging header and we're in
    // standard-compile (non-WMO) mode, we take the opportunity to precompile
    // the header into a temporary PCH, and replace the import argument with the
    // PCH in the subsequent frontend jobs.
    JobAction *PCH = nullptr;
    if (Args.hasFlag(options::OPT_enable_bridging_pch,
                     options::OPT_disable_bridging_pch,
                     true)) {
      if (Arg *A = Args.getLastArg(options::OPT_import_objc_header)) {
        StringRef Value = A->getValue();
        auto Ty = TC.lookupTypeForExtension(llvm::sys::path::extension(Value));
        if (Ty == file_types::TY_ObjCHeader) {
          auto *HeaderInput = C.createAction<InputAction>(*A, Ty);
          StringRef PersistentPCHDir;
          if (const Arg *A = Args.getLastArg(options::OPT_pch_output_dir)) {
            PersistentPCHDir = A->getValue();
          }
          PCH = C.createAction<GeneratePCHJobAction>(HeaderInput,
                                                     PersistentPCHDir);
        }
      }
    }

    for (const InputPair &Input : Inputs) {
      file_types::ID InputType = Input.first;
      const Arg *InputArg = Input.second;

      Action *Current = C.createAction<InputAction>(*InputArg, InputType);
      switch (InputType) {
      case file_types::TY_Swift:
      case file_types::TY_SIL:
      case file_types::TY_SIB: {
        // Source inputs always need to be compiled.
        assert(file_types::isPartOfSwiftCompilation(InputType));

        CompileJobAction::InputInfo previousBuildState = {
          CompileJobAction::InputInfo::NeedsCascadingBuild,
          llvm::sys::TimePoint<>::min()
        };
        if (OutOfDateMap)
          previousBuildState = OutOfDateMap->lookup(InputArg);
        if (Args.hasArg(options::OPT_embed_bitcode)) {
          Current = C.createAction<CompileJobAction>(
              Current, file_types::TY_LLVM_BC, previousBuildState);
          if (PCH)
            cast<JobAction>(Current)->addInput(PCH);
          AllModuleInputs.push_back(Current);
          Current = C.createAction<BackendJobAction>(Current,
                                                     OI.CompilerOutputType, 0);
        } else {
          Current = C.createAction<CompileJobAction>(Current,
                                                     OI.CompilerOutputType,
                                                     previousBuildState);
          if (PCH)
            cast<JobAction>(Current)->addInput(PCH);
          AllModuleInputs.push_back(Current);
        }
        AllLinkerInputs.push_back(Current);
        break;
      }
      case file_types::TY_SwiftModuleFile:
      case file_types::TY_SwiftModuleDocFile:
        if (OI.ShouldGenerateModule && !OI.shouldLink()) {
          // When generating a .swiftmodule as a top-level output (as opposed
          // to, for example, linking an image), treat .swiftmodule files as
          // inputs to a MergeModule action.
          AllModuleInputs.push_back(Current);
          break;
        } else if (OI.shouldLink()) {
          // Otherwise, if linking, pass .swiftmodule files as inputs to the
          // linker, so that their debug info is available.
          AllLinkerInputs.push_back(Current);
          break;
        } else {
          Diags.diagnose(SourceLoc(), diag::error_unexpected_input_file,
                         InputArg->getValue());
          continue;
        }
      case file_types::TY_AutolinkFile:
      case file_types::TY_Object:
        // Object inputs are only okay if linking.
        if (OI.shouldLink()) {
          AllLinkerInputs.push_back(Current);
          break;
        }
        LLVM_FALLTHROUGH;
      case file_types::TY_Image:
      case file_types::TY_dSYM:
      case file_types::TY_Dependencies:
      case file_types::TY_Assembly:
      case file_types::TY_LLVM_IR:
      case file_types::TY_LLVM_BC:
      case file_types::TY_SerializedDiagnostics:
      case file_types::TY_ObjCHeader:
      case file_types::TY_ClangModuleFile:
      case file_types::TY_SwiftDeps:
      case file_types::TY_Remapping:
      case file_types::TY_IndexData:
      case file_types::TY_PCH:
      case file_types::TY_ImportedModules:
      case file_types::TY_TBD:
      case file_types::TY_ModuleTrace:
      case file_types::TY_OptRecord:
        // We could in theory handle assembly or LLVM input, but let's not.
        // FIXME: What about LTO?
        Diags.diagnose(SourceLoc(), diag::error_unexpected_input_file,
                       InputArg->getValue());
        continue;
      case file_types::TY_RawSIB:
      case file_types::TY_RawSIL:
      case file_types::TY_Nothing:
      case file_types::TY_INVALID:
        llvm_unreachable("these types should never be inferred");
      }
    }
    break;
  }
  case OutputInfo::Mode::SingleCompile: {
    if (Inputs.empty()) break;
    if (Args.hasArg(options::OPT_embed_bitcode)) {
      // Make sure we can handle the inputs.
      bool HandledHere = true;
      for (const InputPair &Input : Inputs) {
        file_types::ID InputType = Input.first;
        if (!file_types::isPartOfSwiftCompilation(InputType)) {
          HandledHere = false;
          break;
        }
      }
      if (HandledHere) {
        // Create a single CompileJobAction and a single BackendJobAction.
        JobAction *CA =
            C.createAction<CompileJobAction>(file_types::TY_LLVM_BC);
        AllModuleInputs.push_back(CA);

        int InputIndex = 0;
        for (const InputPair &Input : Inputs) {
          file_types::ID InputType = Input.first;
          const Arg *InputArg = Input.second;

          CA->addInput(C.createAction<InputAction>(*InputArg, InputType));
          if (OI.isMultiThreading()) {
            // With multi-threading we need a backend job for each output file
            // of the compilation.
            auto *BJA = C.createAction<BackendJobAction>(CA,
                                                         OI.CompilerOutputType,
                                                         InputIndex);
            AllLinkerInputs.push_back(BJA);
          }
          InputIndex++;
        }
        if (!OI.isMultiThreading()) {
          // No multi-threading: the compilation only produces a single output
          // file.
          CA = C.createAction<BackendJobAction>(CA, OI.CompilerOutputType, 0);
          AllLinkerInputs.push_back(CA);
        }
        break;
      }
    }

    // Create a single CompileJobAction for all of the driver's inputs.
    auto *CA = C.createAction<CompileJobAction>(OI.CompilerOutputType);
    for (const InputPair &Input : Inputs) {
      file_types::ID InputType = Input.first;
      const Arg *InputArg = Input.second;

      CA->addInput(C.createAction<InputAction>(*InputArg, InputType));
    }
    AllModuleInputs.push_back(CA);
    AllLinkerInputs.push_back(CA);
    break;
  }
  case OutputInfo::Mode::BatchModeCompile: {
    llvm_unreachable("Batch mode should not be used to build actions");
  }
  case OutputInfo::Mode::Immediate: {
    if (Inputs.empty())
      return;

    assert(OI.CompilerOutputType == file_types::TY_Nothing);
    auto *CA = C.createAction<InterpretJobAction>();
    for (const InputPair &Input : Inputs) {
      file_types::ID InputType = Input.first;
      const Arg *InputArg = Input.second;

      CA->addInput(C.createAction<InputAction>(*InputArg, InputType));
    }
    TopLevelActions.push_back(CA);
    return;
  }
  case OutputInfo::Mode::REPL: {
    if (!Inputs.empty()) {
      // REPL mode requires no inputs.
      Diags.diagnose(SourceLoc(), diag::error_repl_requires_no_input_files);
      return;
    }

    REPLJobAction::Mode Mode = REPLJobAction::Mode::PreferLLDB;
    if (const Arg *A = Args.getLastArg(options::OPT_lldb_repl,
                                       options::OPT_deprecated_integrated_repl)) {
      if (A->getOption().matches(options::OPT_lldb_repl))
        Mode = REPLJobAction::Mode::RequireLLDB;
      else
        Mode = REPLJobAction::Mode::Integrated;
    }

    TopLevelActions.push_back(C.createAction<REPLJobAction>(Mode));
    return;
  }
  }

  JobAction *MergeModuleAction = nullptr;
  if (OI.ShouldGenerateModule &&
      OI.CompilerMode != OutputInfo::Mode::SingleCompile &&
      !AllModuleInputs.empty()) {
    // We're performing multiple compilations; set up a merge module step
    // so we generate a single swiftmodule as output.
    MergeModuleAction = C.createAction<MergeModuleJobAction>(AllModuleInputs);
  }

  if (OI.shouldLink() && !AllLinkerInputs.empty()) {
    auto *LinkAction = C.createAction<LinkJobAction>(AllLinkerInputs,
                                                     OI.LinkAction);

    // On ELF platforms there's no built in autolinking mechanism, so we
    // pull the info we need from the .o files directly and pass them as an
    // argument input file to the linker.
    SmallVector<const Action *, 2> AutolinkExtractInputs;
    for (const Action *A : AllLinkerInputs)
      if (A->getType() == file_types::TY_Object)
        AutolinkExtractInputs.push_back(A);
    if (!AutolinkExtractInputs.empty() &&
        (TC.getTriple().getObjectFormat() == llvm::Triple::ELF ||
         TC.getTriple().isOSCygMing())) {
      auto *AutolinkExtractAction =
          C.createAction<AutolinkExtractJobAction>(AutolinkExtractInputs);
      // Takes the same inputs as the linker...
      // ...and gives its output to the linker.
      LinkAction->addInput(AutolinkExtractAction);
    }

    if (MergeModuleAction) {
      if (OI.DebugInfoKind == IRGenDebugInfoKind::Normal) {
        if (TC.getTriple().getObjectFormat() == llvm::Triple::ELF) {
          auto *ModuleWrapAction =
              C.createAction<ModuleWrapJobAction>(MergeModuleAction);
          LinkAction->addInput(ModuleWrapAction);
        } else {
          LinkAction->addInput(MergeModuleAction);
        }
        // FIXME: Adding the MergeModuleAction as top-level regardless would
        // allow us to get rid of the special case flag for that.
      } else {
        TopLevelActions.push_back(MergeModuleAction);
      }
    }
    TopLevelActions.push_back(LinkAction);

    if (TC.getTriple().isOSDarwin() &&
        OI.DebugInfoKind > IRGenDebugInfoKind::None) {
      auto *dSYMAction = C.createAction<GenerateDSYMJobAction>(LinkAction);
      TopLevelActions.push_back(dSYMAction);
      if (Args.hasArg(options::OPT_verify_debug_info)) {
        TopLevelActions.push_back(
            C.createAction<VerifyDebugInfoJobAction>(dSYMAction));
      }
    }
  } else {
    // We can't rely on the merge module action being the only top-level
    // action that needs to run. There may be other actions (e.g.
    // BackendJobActions) that are not merge-module inputs but should be run
    // anyway.
    if (MergeModuleAction)
      TopLevelActions.push_back(MergeModuleAction);
    TopLevelActions.append(AllLinkerInputs.begin(), AllLinkerInputs.end());
  }
}

bool Driver::handleImmediateArgs(const ArgList &Args, const ToolChain &TC) {
  if (Args.hasArg(options::OPT_help)) {
    printHelp(false);
    return false;
  }

  if (Args.hasArg(options::OPT_help_hidden)) {
    printHelp(true);
    return false;
  }

  if (Args.hasArg(options::OPT_version)) {
    // Follow gcc/clang behavior and use stdout for --version and stderr for -v.
    printVersion(TC, llvm::outs());
    return false;
  }

  if (Args.hasArg(options::OPT_v)) {
    printVersion(TC, llvm::errs());
    SuppressNoInputFilesError = true;
  }

  if (const Arg *A = Args.getLastArg(options::OPT_driver_use_frontend_path))
    DriverExecutable = A->getValue();

  return true;
}

Optional<OutputFileMap>
Driver::buildOutputFileMap(const llvm::opt::DerivedArgList &Args,
                           StringRef workingDirectory) const {
  const Arg *A = Args.getLastArg(options::OPT_output_file_map);
  if (!A)
    return None;

  // TODO: perform some preflight checks to ensure the file exists.
  llvm::Expected<OutputFileMap> OFM =
      OutputFileMap::loadFromPath(A->getValue(), workingDirectory);
  if (auto Err = OFM.takeError()) {
    Diags.diagnose(SourceLoc(), diag::error_unable_to_load_output_file_map,
                   llvm::toString(std::move(Err)), A->getValue());
    return None;
  }
  return *OFM;
}

void Driver::buildJobs(ArrayRef<const Action *> TopLevelActions,
                       const OutputInfo &OI, const OutputFileMap *OFM,
                       StringRef workingDirectory, const ToolChain &TC,
                       Compilation &C) const {
  llvm::PrettyStackTraceString CrashInfo("Building compilation jobs");

  const DerivedArgList &Args = C.getArgs();
  JobCacheMap JobCache;

  if (Args.hasArg(options::OPT_o) && !OI.shouldLink() &&
      !OI.ShouldTreatModuleAsTopLevelOutput) {
    bool ShouldComplain;
    if (OI.isMultiThreading()) {
      // Multi-threading compilation has multiple outputs unless there's only
      // one input.
      ShouldComplain = C.getInputFiles().size() > 1;
    } else {
      // Single-threaded compilation is a problem if we're compiling more than
      // one file.
      ShouldComplain = 1 < llvm::count_if(C.getActions(), [](const Action *A) {
        return isa<CompileJobAction>(A);
      });
    }

    if (ShouldComplain) {
      Diags.diagnose(SourceLoc(),
                     diag::error_cannot_specify__o_for_multiple_outputs);
    }
  }

  for (const Action *A : TopLevelActions) {
    if (auto *JA = dyn_cast<JobAction>(A)) {
      (void)buildJobsForAction(C, JA, OI, OFM, workingDirectory, TC,
                               /*TopLevel=*/true, JobCache);
    }
  }
}

/// Form a filename based on \p base in \p result, optionally setting its
/// extension to \p newExt and in \p workingDirectory.
static void formFilenameFromBaseAndExt(StringRef base, StringRef newExt,
                                       StringRef workingDirectory,
                                       SmallVectorImpl<char> &result) {
  if (workingDirectory.empty() || llvm::sys::path::is_absolute(base)) {
    result.assign(base.begin(), base.end());
  } else {
    assert(!base.empty() && base != "-" && "unexpected basename");
    result.assign(workingDirectory.begin(), workingDirectory.end());
    llvm::sys::path::append(result, base);
  }

  if (!newExt.empty()) {
    llvm::sys::path::replace_extension(result, newExt);
  }
}

static Optional<StringRef> getOutputFilenameFromPathArgOrAsTopLevel(
    const OutputInfo &OI, const llvm::opt::DerivedArgList &Args,
    llvm::opt::OptSpecifier PathArg, file_types::ID ExpectedOutputType,
    bool TreatAsTopLevelOutput, StringRef workingDirectory, StringRef ext,
    llvm::SmallString<128> &Buffer) {
  if (const Arg *A = Args.getLastArg(PathArg))
    return StringRef(A->getValue());

  if (TreatAsTopLevelOutput) {
    if (const Arg *A = Args.getLastArg(options::OPT_o)) {
      if (OI.CompilerOutputType == ExpectedOutputType)
        return StringRef(A->getValue());

      // Otherwise, put the file next to the top-level output.
      Buffer = A->getValue();
      llvm::sys::path::remove_filename(Buffer);
      llvm::sys::path::append(Buffer, OI.ModuleName);
      llvm::sys::path::replace_extension(Buffer, ext);
      return Buffer.str();
    }

    // A top-level output wasn't specified, so just output to
    // <ModuleName>.<ext>.
    formFilenameFromBaseAndExt(OI.ModuleName, ext, workingDirectory, Buffer);
    return Buffer.str();
  }

  return None;
}

static StringRef assignOutputName(Compilation &C, const JobAction *JA,
                                  DiagnosticEngine &Diags,
                                  llvm::SmallString<128> &Buffer,
                                  StringRef BaseName,
                                  PreserveOnSignal ShouldPreserveOnSignal) {
  // We should output to a temporary file, since we're not at the top level
  // (or are generating a bridging PCH, which is currently always a temp).
  StringRef Stem = llvm::sys::path::stem(BaseName);
  StringRef Suffix = file_types::getTypeTempSuffix(JA->getType());
  std::error_code EC = llvm::sys::fs::createTemporaryFile(Stem, Suffix, Buffer);
  if (EC) {
    Diags.diagnose(SourceLoc(), diag::error_unable_to_make_temporary_file,
                   EC.message());
    return {};
  }
  C.addTemporaryFile(Buffer.str(), ShouldPreserveOnSignal);

  return Buffer.str();
}

static StringRef baseNameForImage(const JobAction *JA, const OutputInfo &OI,
                                  const llvm::Triple &Triple,
                                  llvm::SmallString<128> &Buffer,
                                  StringRef BaseInput, StringRef BaseName) {
  if (JA->size() == 1 && OI.ModuleNameIsFallback && BaseInput != "-")
    return llvm::sys::path::stem(BaseInput);
  auto link = dyn_cast<LinkJobAction>(JA);
  if (!link)
    return BaseName;
  if (link->getKind() != LinkKind::DynamicLibrary)
    return BaseName;

  Buffer = Triple.isOSWindows() ? "" : "lib";
  Buffer.append(BaseName);

  if (Triple.isOSDarwin())
    Buffer.append(".dylib");
  else if (Triple.isOSWindows())
    Buffer.append(".dll");
  else
    Buffer.append(".so");
  return Buffer.str();
}

static StringRef getOutputFilename(Compilation &C,
                                   const JobAction *JA,
                                   const OutputInfo &OI,
                                   const TypeToPathMap *OutputMap,
                                   StringRef workingDirectory,
                                   const llvm::Triple &Triple,
                                   const llvm::opt::DerivedArgList &Args,
                                   bool AtTopLevel,
                                   StringRef BaseInput,
                                   StringRef PrimaryInput,
                                   DiagnosticEngine &Diags,
                                   llvm::SmallString<128> &Buffer) {
  if (JA->getType() == file_types::TY_Nothing)
    return {};

  // If available, check the OutputMap first.
  if (OutputMap) {
    auto iter = OutputMap->find(JA->getType());
    if (iter != OutputMap->end())
      return iter->second;
  }

  // Process Action-specific output-specifying options next,
  // since we didn't find anything applicable in the OutputMap.

  if (isa<MergeModuleJobAction>(JA)) {
    auto optFilename = getOutputFilenameFromPathArgOrAsTopLevel(
        OI, Args, options::OPT_emit_module_path, file_types::TY_SwiftModuleFile,
        OI.ShouldTreatModuleAsTopLevelOutput, workingDirectory,
        SERIALIZED_MODULE_EXTENSION, Buffer);
    if (optFilename)
      return *optFilename;
  }

  // dSYM actions are never treated as top-level.
  if (isa<GenerateDSYMJobAction>(JA)) {
    Buffer = PrimaryInput;
    Buffer.push_back('.');
    Buffer.append(file_types::getTypeTempSuffix(JA->getType()));
    return Buffer.str();
  }

  auto ShouldPreserveOnSignal = PreserveOnSignal::No;

  if (auto *PCHAct = dyn_cast<GeneratePCHJobAction>(JA)) {
    // For a persistent PCH we don't use an output, the frontend determines
    // the filename to use for the PCH.
    assert(!PCHAct->isPersistentPCH());
    ShouldPreserveOnSignal = PreserveOnSignal::Yes;
  }

  // We don't have an output from an Action-specific command line option,
  // so figure one out using the defaults.
  if (AtTopLevel) {
    if (Arg *FinalOutput = Args.getLastArg(options::OPT_o))
      return FinalOutput->getValue();
    if (file_types::isTextual(JA->getType()))
      return "-";
  }

  assert(!BaseInput.empty() &&
         "A Job which produces output must have a BaseInput!");
  StringRef BaseName(BaseInput);
  if (isa<MergeModuleJobAction>(JA) ||
      (OI.CompilerMode == OutputInfo::Mode::SingleCompile &&
       !OI.isMultiThreading()) ||
      JA->getType() == file_types::TY_Image)
    BaseName = OI.ModuleName;

  // We don't yet have a name, assign one.
  if (!AtTopLevel)
    return assignOutputName(C, JA, Diags, Buffer, BaseName,
                            ShouldPreserveOnSignal);

  if (JA->getType() == file_types::TY_Image) {
    SmallString<16> Base =
        baseNameForImage(JA, OI, Triple, Buffer, BaseInput, BaseName);
    formFilenameFromBaseAndExt(Base, /*newExt=*/"", workingDirectory, Buffer);
    return Buffer.str();
  }

  StringRef Suffix = file_types::getTypeTempSuffix(JA->getType());
  assert(Suffix.data() &&
         "All types used for output should have a suffix.");

  formFilenameFromBaseAndExt(llvm::sys::path::filename(BaseName), Suffix,
                             workingDirectory, Buffer);
  return Buffer.str();
}

static bool hasExistingAdditionalOutput(CommandOutput &output,
                                        file_types::ID outputType,
                                        StringRef outputPath = StringRef()) {

  auto existing = output.getAdditionalOutputForType(outputType);
  if (!existing.empty()) {
    assert(outputPath.empty() || outputPath == existing);
    return true;
  }
  return false;
}

static void addAuxiliaryOutput(Compilation &C, CommandOutput &output,
                               file_types::ID outputType, const OutputInfo &OI,
                               const TypeToPathMap *outputMap,
                               StringRef workingDirectory,
                               StringRef outputPath = StringRef()) {

  if (hasExistingAdditionalOutput(output, outputType, outputPath))
    return;

  StringRef outputMapPath;
  if (outputMap) {
    auto iter = outputMap->find(outputType);
    if (iter != outputMap->end())
      outputMapPath = iter->second;
  }

  if (!outputMapPath.empty()) {
    // Prefer a path from the OutputMap.
    output.setAdditionalOutputForType(outputType, outputMapPath);
  } else if (!outputPath.empty()) {
    output.setAdditionalOutputForType(outputType, outputPath);
  } else {
    // Put the auxiliary output file next to "the" primary output file.
    //
    // FIXME: when we're in WMO and have multiple primary outputs, we derive the
    // additional filename here from the _first_ primary output name, which
    // means that in the derived OFM (in Job.cpp) the additional output will
    // have a possibly-surprising name. But that's only half the problem: it
    // also get associated with the first primary _input_, even when there are
    // multiple primary inputs; really it should be associated with the build as
    // a whole -- derived OFM input "" -- but that's a more general thing to
    // fix.
    llvm::SmallString<128> path;
    if (output.getPrimaryOutputType() != file_types::TY_Nothing)
      path = output.getPrimaryOutputFilenames()[0];
    else if (!output.getBaseInput(0).empty())
      path = llvm::sys::path::stem(output.getBaseInput(0));
    else {
      formFilenameFromBaseAndExt(OI.ModuleName, /*newExt=*/"", workingDirectory,
                                 path);
    }
    assert(!path.empty());

    bool isTempFile = C.isTemporaryFile(path);
    llvm::sys::path::replace_extension(
        path, file_types::getTypeTempSuffix(outputType));
    output.setAdditionalOutputForType(outputType, path);
    if (isTempFile)
      C.addTemporaryFile(path);
  }
}

static void addDiagFileOutputForPersistentPCHAction(
    Compilation &C, const GeneratePCHJobAction *JA, CommandOutput &output,
    const OutputInfo &OI, const TypeToPathMap *outputMap,
    StringRef workingDirectory, DiagnosticEngine &diags) {
  assert(JA->isPersistentPCH());

  // For a persistent PCH we don't use an output, the frontend determines
  // the filename to use for the PCH. For the diagnostics file, try to
  // determine an invocation-specific path inside the directory where the
  // PCH is going to be written, and fallback to a temporary file if we
  // cannot determine such a path.

  StringRef pchOutDir = JA->getPersistentPCHDir();
  StringRef headerPath = output.getBaseInput(JA->getInputIndex());
  StringRef stem = llvm::sys::path::stem(headerPath);
  StringRef suffix =
      file_types::getTypeTempSuffix(file_types::TY_SerializedDiagnostics);
  SmallString<256> outPathBuf;

  if (const Arg *A = C.getArgs().getLastArg(options::OPT_emit_module_path)) {
    // The module file path is unique for a specific module and architecture
    // (it won't be concurrently written to) so we can use the path as hash
    // for determining the filename to use for the diagnostic file.
    StringRef ModuleOutPath = A->getValue();
    outPathBuf = pchOutDir;
    llvm::sys::path::append(outPathBuf, stem);
    outPathBuf += '-';
    auto code = llvm::hash_value(ModuleOutPath);
    outPathBuf += llvm::APInt(64, code).toString(36, /*Signed=*/false);
    llvm::sys::path::replace_extension(outPathBuf, suffix);
  }

  if (outPathBuf.empty()) {
    // Fallback to creating a temporary file.
    std::error_code EC =
    llvm::sys::fs::createTemporaryFile(stem, suffix, outPathBuf);
    if (EC) {
      diags.diagnose(SourceLoc(),
                     diag::error_unable_to_make_temporary_file,
                     EC.message());
      return;
    }
    C.addTemporaryFile(outPathBuf.str());
  }

  if (!outPathBuf.empty()) {
    addAuxiliaryOutput(C, output, file_types::TY_SerializedDiagnostics, OI,
                       outputMap, workingDirectory, outPathBuf.str());
  }
}

/// If the file at \p input has not been modified since the last build (i.e. its
/// mtime has not changed), adjust the Job's condition accordingly.
static void
handleCompileJobCondition(Job *J, CompileJobAction::InputInfo inputInfo,
                          StringRef input, bool alwaysRebuildDependents) {
  if (inputInfo.status == CompileJobAction::InputInfo::NewlyAdded) {
    J->setCondition(Job::Condition::NewlyAdded);
    return;
  }

  bool hasValidModTime = false;
  llvm::sys::fs::file_status inputStatus;
  if (!llvm::sys::fs::status(input, inputStatus)) {
    J->setInputModTime(inputStatus.getLastModificationTime());
    hasValidModTime = true;
  }

  Job::Condition condition;
  if (hasValidModTime && J->getInputModTime() == inputInfo.previousModTime) {
    switch (inputInfo.status) {
    case CompileJobAction::InputInfo::UpToDate:
      if (llvm::sys::fs::exists(J->getOutput().getPrimaryOutputFilename()))
        condition = Job::Condition::CheckDependencies;
      else
        condition = Job::Condition::RunWithoutCascading;
      break;
    case CompileJobAction::InputInfo::NeedsCascadingBuild:
      condition = Job::Condition::Always;
      break;
    case CompileJobAction::InputInfo::NeedsNonCascadingBuild:
      condition = Job::Condition::RunWithoutCascading;
      break;
    case CompileJobAction::InputInfo::NewlyAdded:
      llvm_unreachable("handled above");
    }
  } else {
    if (alwaysRebuildDependents ||
        inputInfo.status == CompileJobAction::InputInfo::NeedsCascadingBuild) {
      condition = Job::Condition::Always;
    } else {
      condition = Job::Condition::RunWithoutCascading;
    }
  }

  J->setCondition(condition);
}

Job *Driver::buildJobsForAction(Compilation &C, const JobAction *JA,
                                const OutputInfo &OI, const OutputFileMap *OFM,
                                StringRef workingDirectory, const ToolChain &TC,
                                bool AtTopLevel, JobCacheMap &JobCache) const {

  PrettyStackTraceDriverAction CrashInfo("building jobs", JA);

  // 1. See if we've already got this cached.
  std::pair<const Action *, const ToolChain *> Key(JA, &TC);
  {
    auto CacheIter = JobCache.find(Key);
    if (CacheIter != JobCache.end()) {
      return CacheIter->second;
    }
  }

  // 2. Build up the list of input jobs.
  SmallVector<const Action *, 4> InputActions;
  SmallVector<const Job *, 4> InputJobs;
  for (const Action *Input : *JA) {
    if (auto *InputJobAction = dyn_cast<JobAction>(Input)) {
      InputJobs.push_back(buildJobsForAction(
          C, InputJobAction, OI, OFM, workingDirectory, TC, false, JobCache));
    } else {
      InputActions.push_back(Input);
    }
  }

  // 3. Determine the CommandOutput for the job.
  StringRef BaseInput;
  StringRef PrimaryInput;
  if (!InputActions.empty()) {
    // Use the first InputAction as our BaseInput and PrimaryInput.
    const InputAction *IA = cast<InputAction>(InputActions[0]);
    BaseInput = IA->getInputArg().getValue();
    PrimaryInput = BaseInput;
  } else if (!InputJobs.empty()) {
    const CommandOutput &Out = InputJobs.front()->getOutput();
    size_t i = JA->getInputIndex();
    // Use the first Job's BaseInput as our BaseInput.
    BaseInput = Out.getBaseInput(i);
    // Use the first Job's Primary Output as our PrimaryInput.
    PrimaryInput = Out.getPrimaryOutputFilenames()[i];
  }

  // With -index-file option, the primary input is the one passed with
  // -index-file-path.
  // FIXME: Figure out how this better fits within the driver infrastructure.
  if (JA->getType() == file_types::TY_IndexData) {
    if (Arg *A = C.getArgs().getLastArg(options::OPT_index_file_path)) {
      BaseInput = A->getValue();
      PrimaryInput = A->getValue();
    }
  }

  const TypeToPathMap *OutputMap = nullptr;
  if (OFM) {
    if (isa<CompileJobAction>(JA)) {
      if (OI.CompilerMode == OutputInfo::Mode::SingleCompile) {
        OutputMap = OFM->getOutputMapForSingleOutput();
      } else {
        OutputMap = OFM->getOutputMapForInput(BaseInput);
      }
    } else if (isa<BackendJobAction>(JA)) {
      OutputMap = OFM->getOutputMapForInput(BaseInput);
    }
  }

  std::unique_ptr<CommandOutput> Output(
      new CommandOutput(JA->getType(), C.getDerivedOutputFileMap()));

  PrettyStackTraceDriverCommandOutput CrashInfo2("determining output",
                                                 Output.get());
  llvm::SmallString<128> Buf;
  computeMainOutput(C, JA, OI, OFM, TC, AtTopLevel, InputActions, InputJobs,
                    OutputMap, workingDirectory, BaseInput, PrimaryInput, Buf,
                    Output.get());

  if (OI.ShouldGenerateModule && isa<CompileJobAction>(JA))
    chooseSwiftModuleOutputPath(C, OI, OFM, OutputMap, workingDirectory,
                                Output.get());

  if (OI.ShouldGenerateModule &&
      (isa<CompileJobAction>(JA) || isa<MergeModuleJobAction>(JA)))
    chooseSwiftModuleDocOutputPath(C, OutputMap, workingDirectory,
                                   Output.get());

  if (C.getArgs().hasArg(options::OPT_update_code) && isa<CompileJobAction>(JA))
    chooseRemappingOutputPath(C, OutputMap, Output.get());

  if (isa<CompileJobAction>(JA) || isa<GeneratePCHJobAction>(JA)) {
    // Choose the serialized diagnostics output path.
    if (C.getArgs().hasArg(options::OPT_serialize_diagnostics))
      chooseSerializedDiagnosticsPath(C, JA, OI, OutputMap, workingDirectory,
                                      Output.get());
  }

  if (isa<CompileJobAction>(JA))
    chooseDependenciesOutputPaths(C, OI, OutputMap, workingDirectory, Buf,
                                  Output.get());

  if (C.getArgs().hasArg(options::OPT_save_optimization_record,
                         options::OPT_save_optimization_record_path))
    chooseOptimizationRecordPath(C, OI, workingDirectory, Buf, Output.get());

  if ((isa<MergeModuleJobAction>(JA) ||
       (isa<CompileJobAction>(JA) &&
        OI.CompilerMode == OutputInfo::Mode::SingleCompile)) &&
      C.getArgs().hasArg(options::OPT_emit_objc_header,
                         options::OPT_emit_objc_header_path))
    chooseObjectiveCHeaderOutputPath(C, OI, OutputMap, workingDirectory,
                                     Output.get());

  // 4. Construct a Job which produces the right CommandOutput.
  std::unique_ptr<Job> ownedJob = TC.constructJob(*JA, C, std::move(InputJobs),
                                                  InputActions, 
                                                  std::move(Output), OI);
  Job *J = C.addJob(std::move(ownedJob));

  // If we track dependencies for this job, we may be able to avoid running it.
  if (!J->getOutput()
           .getAdditionalOutputForType(file_types::TY_SwiftDeps)
           .empty()) {
    if (InputActions.size() == 1) {
      auto compileJob = cast<CompileJobAction>(JA);
      bool alwaysRebuildDependents =
          C.getArgs().hasArg(options::OPT_driver_always_rebuild_dependents);
      handleCompileJobCondition(J, compileJob->getInputInfo(), BaseInput,
                                alwaysRebuildDependents);
    }
  }

  // 5. Add it to the JobCache, so we don't construct the same Job multiple
  // times.
  JobCache[Key] = J;

  if (DriverPrintBindings) {
    llvm::outs() << "# \"" << TC.getTriple().str()
      << "\" - \"" << llvm::sys::path::filename(J->getExecutable())
      << "\", inputs: [";

    interleave(InputActions.begin(), InputActions.end(),
               [](const Action *A) {
                 auto Input = cast<InputAction>(A);
                 llvm::outs() << '"' << Input->getInputArg().getValue() << '"';
               },
               [] { llvm::outs() << ", "; });
    if (!InputActions.empty() && !J->getInputs().empty())
      llvm::outs() << ", ";
    interleave(J->getInputs().begin(), J->getInputs().end(),
               [](const Job *Input) {
                 auto FileNames = Input->getOutput().getPrimaryOutputFilenames();
                 interleave(FileNames.begin(), FileNames.end(),
                            [](const std::string &FileName) {
                              llvm::outs() << '"' << FileName << '"';
                            },
                            [] { llvm::outs() << ", "; });
               },
               [] { llvm::outs() << ", "; });

    llvm::outs() << "], output: {";
    auto OutputFileNames = J->getOutput().getPrimaryOutputFilenames();
    StringRef TypeName =
        file_types::getTypeName(J->getOutput().getPrimaryOutputType());
    interleave(OutputFileNames.begin(), OutputFileNames.end(),
               [TypeName](const std::string &FileName) {
                 llvm::outs() << TypeName << ": \"" << FileName << '"';
               },
               [] { llvm::outs() << ", "; });

    file_types::forAllTypes([&J](file_types::ID Ty) {
      StringRef AdditionalOutput =
        J->getOutput().getAdditionalOutputForType(Ty);
      if (!AdditionalOutput.empty()) {
        llvm::outs() << ", " << file_types::getTypeName(Ty) << ": \""
                     << AdditionalOutput << '"';
      }
    });
    llvm::outs() << '}';

    switch (J->getCondition()) {
    case Job::Condition::Always:
      break;
    case Job::Condition::RunWithoutCascading:
      llvm::outs() << ", condition: run-without-cascading";
      break;
    case Job::Condition::CheckDependencies:
      llvm::outs() << ", condition: check-dependencies";
      break;
    case Job::Condition::NewlyAdded:
      llvm::outs() << ", condition: newly-added";
      break;
    }

    llvm::outs() << '\n';
  }

  return J;
}

void Driver::computeMainOutput(
    Compilation &C, const JobAction *JA, const OutputInfo &OI,
    const OutputFileMap *OFM, const ToolChain &TC, bool AtTopLevel,
    SmallVectorImpl<const Action *> &InputActions,
    SmallVectorImpl<const Job *> &InputJobs, const TypeToPathMap *OutputMap,
    StringRef workingDirectory, StringRef BaseInput, StringRef PrimaryInput,
    llvm::SmallString<128> &Buf, CommandOutput *Output) const {
  StringRef OutputFile;
  if (OI.isMultiThreading() && isa<CompileJobAction>(JA) &&
      file_types::isAfterLLVM(JA->getType())) {
    // Multi-threaded compilation: A single frontend command produces multiple
    // output file: one for each input files.

    // In batch mode, the driver will try to reserve multiple differing main
    // outputs to a bridging header. Another assertion will trip, but the cause
    // will be harder to track down. Since the driver now ignores -num-threads
    // in batch mode, the user should never be able to falsify this assertion.
    assert(!C.getBatchModeEnabled() && "Batch mode produces only one main "
                                       "output per input action, cannot have "
                                       "batch mode & num-threads");

    auto OutputFunc = [&](StringRef Base, StringRef Primary) {
      const TypeToPathMap *OMForInput = nullptr;
      if (OFM)
        OMForInput = OFM->getOutputMapForInput(Base);

      OutputFile = getOutputFilename(C, JA, OI, OMForInput, workingDirectory,
                                     TC.getTriple(), C.getArgs(), AtTopLevel,
                                     Base, Primary, Diags, Buf);
      Output->addPrimaryOutput(CommandInputPair(Base, Primary),
                               OutputFile);
    };
    // Add an output file for each input action.
    for (const Action *A : InputActions) {
      const InputAction *IA = cast<InputAction>(A);
      StringRef IV = IA->getInputArg().getValue();
      OutputFunc(IV, IV);
    }
    // Add an output file for each primary output of each input job.
    for (const Job *IJ : InputJobs) {
      size_t i = 0;
      CommandOutput const &Out = IJ->getOutput();
      for (auto OutPrimary : Out.getPrimaryOutputFilenames()) {
        OutputFunc(Out.getBaseInput(i++), OutPrimary);
      }
    }
  } else {
    // The common case: there is a single output file.
    OutputFile = getOutputFilename(C, JA, OI, OutputMap, workingDirectory,
                                   TC.getTriple(), C.getArgs(), AtTopLevel,
                                   BaseInput, PrimaryInput, Diags, Buf);
    Output->addPrimaryOutput(CommandInputPair(BaseInput, PrimaryInput),
                             OutputFile);
  }
}

void Driver::chooseSwiftModuleOutputPath(Compilation &C, const OutputInfo &OI,
                                         const OutputFileMap *OFM,
                                         const TypeToPathMap *OutputMap,
                                         StringRef workingDirectory,
                                         CommandOutput *Output) const {

  if (hasExistingAdditionalOutput(*Output, file_types::TY_SwiftModuleFile))
    return;

  StringRef OFMModuleOutputPath;
  if (OutputMap) {
    auto iter = OutputMap->find(file_types::TY_SwiftModuleFile);
    if (iter != OutputMap->end())
      OFMModuleOutputPath = iter->second;
  }

  const Arg *A = C.getArgs().getLastArg(options::OPT_emit_module_path);

  if (!OFMModuleOutputPath.empty()) {
    // Prefer a path from the OutputMap.
    Output->setAdditionalOutputForType(file_types::TY_SwiftModuleFile,
                                       OFMModuleOutputPath);
  } else if (A && OI.CompilerMode == OutputInfo::Mode::SingleCompile) {
    // We're performing a single compilation (and thus no merge module step),
    // so prefer to use -emit-module-path, if present.
    Output->setAdditionalOutputForType(file_types::TY_SwiftModuleFile,
                                       A->getValue());
  } else if (Output->getPrimaryOutputType() == file_types::TY_SwiftModuleFile) {
    // If the primary type is already a module type, we're out of
    // options for overriding the primary name choice: stop now.
    assert(!Output->getPrimaryOutputFilename().empty());
    return;
  } else if (OI.CompilerMode == OutputInfo::Mode::SingleCompile &&
             OI.ShouldTreatModuleAsTopLevelOutput) {
    // We're performing a single compile and don't have -emit-module-path,
    // but have been told to treat the module as a top-level output.
    // Determine an appropriate path.
    if (const Arg *A = C.getArgs().getLastArg(options::OPT_o)) {
      // Put the module next to the top-level output.
      llvm::SmallString<128> Path(A->getValue());
      llvm::sys::path::remove_filename(Path);
      llvm::sys::path::append(Path, OI.ModuleName);
      llvm::sys::path::replace_extension(Path, SERIALIZED_MODULE_EXTENSION);
      Output->setAdditionalOutputForType(file_types::TY_SwiftModuleFile, Path);
    } else {
      // A top-level output wasn't specified, so just output to
      // <ModuleName>.swiftmodule.
      llvm::SmallString<128> Path(OI.ModuleName);
      llvm::sys::path::replace_extension(Path, SERIALIZED_MODULE_EXTENSION);
      Output->setAdditionalOutputForType(file_types::TY_SwiftModuleFile, Path);
    }
  } else if (Output->getPrimaryOutputType() != file_types::TY_Nothing) {
    // We're only generating the module as an intermediate, so put it next
    // to the primary output of the compile command.
    llvm::SmallString<128> Path(Output->getPrimaryOutputFilenames()[0]);
    assert(!Path.empty());
    bool isTempFile = C.isTemporaryFile(Path);
    llvm::sys::path::replace_extension(Path, SERIALIZED_MODULE_EXTENSION);
    Output->setAdditionalOutputForType(file_types::ID::TY_SwiftModuleFile,
                                       Path);
    if (isTempFile)
      C.addTemporaryFile(Path);
  }
}

void Driver::chooseSwiftModuleDocOutputPath(Compilation &C,
                                            const TypeToPathMap *OutputMap,
                                            StringRef workingDirectory,
                                            CommandOutput *Output) const {

  if (hasExistingAdditionalOutput(*Output, file_types::TY_SwiftModuleDocFile))
    return;

  StringRef OFMModuleDocOutputPath;
  if (OutputMap) {
    auto iter = OutputMap->find(file_types::TY_SwiftModuleDocFile);
    if (iter != OutputMap->end())
      OFMModuleDocOutputPath = iter->second;
  }
  if (!OFMModuleDocOutputPath.empty()) {
    // Prefer a path from the OutputMap.
    Output->setAdditionalOutputForType(file_types::TY_SwiftModuleDocFile,
                                       OFMModuleDocOutputPath);
  } else if (Output->getPrimaryOutputType() != file_types::TY_Nothing) {
    // Otherwise, put it next to the swiftmodule file.
    llvm::SmallString<128> Path(
        Output->getAnyOutputForType(file_types::TY_SwiftModuleFile));
    bool isTempFile = C.isTemporaryFile(Path);
    llvm::sys::path::replace_extension(Path, SERIALIZED_MODULE_DOC_EXTENSION);
    Output->setAdditionalOutputForType(file_types::TY_SwiftModuleDocFile, Path);
    if (isTempFile)
      C.addTemporaryFile(Path);
  }
}

void Driver::chooseRemappingOutputPath(Compilation &C,
                                       const TypeToPathMap *OutputMap,
                                       CommandOutput *Output) const {

  if (hasExistingAdditionalOutput(*Output, file_types::TY_Remapping))
    return;

  StringRef OFMFixitsOutputPath;
  if (OutputMap) {
    auto iter = OutputMap->find(file_types::TY_Remapping);
    if (iter != OutputMap->end())
      OFMFixitsOutputPath = iter->second;
  }
  if (!OFMFixitsOutputPath.empty()) {
    Output->setAdditionalOutputForType(file_types::ID::TY_Remapping,
                                       OFMFixitsOutputPath);
  } else {
    llvm::SmallString<128> Path(Output->getPrimaryOutputFilenames()[0]);
    bool isTempFile = C.isTemporaryFile(Path);
    llvm::sys::path::replace_extension(Path, "remap");
    Output->setAdditionalOutputForType(file_types::ID::TY_Remapping, Path);
    if (isTempFile)
      C.addTemporaryFile(Path);
  }
}

void Driver::chooseSerializedDiagnosticsPath(Compilation &C,
                                             const JobAction *JA,
                                             const OutputInfo &OI,
                                             const TypeToPathMap *OutputMap,
                                             StringRef workingDirectory,
                                             CommandOutput *Output) const {
  if (C.getArgs().hasArg(options::OPT_serialize_diagnostics)) {
    auto pchJA = dyn_cast<GeneratePCHJobAction>(JA);
    if (pchJA && pchJA->isPersistentPCH()) {
      addDiagFileOutputForPersistentPCHAction(C, pchJA, *Output, OI, OutputMap,
                                              workingDirectory, Diags);
    } else {
      addAuxiliaryOutput(C, *Output, file_types::TY_SerializedDiagnostics, OI,
                         OutputMap, workingDirectory);
    }

    // Remove any existing diagnostics files so that clients can detect their
    // presence to determine if a command was run.
    StringRef OutputPath =
        Output->getAnyOutputForType(file_types::TY_SerializedDiagnostics);
    if (llvm::sys::fs::is_regular_file(OutputPath))
      llvm::sys::fs::remove(OutputPath);
  }
}

void Driver::chooseDependenciesOutputPaths(Compilation &C, const OutputInfo &OI,
                                           const TypeToPathMap *OutputMap,
                                           StringRef workingDirectory,
                                           llvm::SmallString<128> &Buf,
                                           CommandOutput *Output) const {
  if (C.getArgs().hasArg(options::OPT_emit_dependencies)) {
    addAuxiliaryOutput(C, *Output, file_types::TY_Dependencies, OI, OutputMap,
                       workingDirectory);
  }
  if (C.getIncrementalBuildEnabled()) {
    addAuxiliaryOutput(C, *Output, file_types::TY_SwiftDeps, OI, OutputMap,
                       workingDirectory);
  }
  chooseLoadedModuleTracePath(C, OI, workingDirectory, Buf, Output);
  chooseTBDPath(C, OI, workingDirectory, Buf, Output);
}

void Driver::chooseLoadedModuleTracePath(Compilation &C, const OutputInfo &OI,
                                         StringRef workingDirectory,
                                         llvm::SmallString<128> &Buf,
                                         CommandOutput *Output) const {
  // The loaded-module-trace is the same for all compile jobs: all `import`
  // statements are processed, even ones from non-primary files. Thus, only
  // one of those jobs needs to emit the file, and we can get it to write
  // straight to the desired final location.
  auto tracePathEnvVar = getenv("SWIFT_LOADED_MODULE_TRACE_FILE");
  auto shouldEmitTrace =
      tracePathEnvVar ||
      C.getArgs().hasArg(options::OPT_emit_loaded_module_trace,
                         options::OPT_emit_loaded_module_trace_path);

  if (shouldEmitTrace &&
      C.requestPermissionForFrontendToEmitLoadedModuleTrace()) {
    StringRef filename;
    // Prefer the environment variable.
    if (tracePathEnvVar)
      filename = StringRef(tracePathEnvVar);
    else {
      // By treating this as a top-level output, the return value always
      // exists.
      filename = *getOutputFilenameFromPathArgOrAsTopLevel(
          OI, C.getArgs(), options::OPT_emit_loaded_module_trace_path,
          file_types::TY_ModuleTrace,
          /*TreatAsTopLevelOutput=*/true, workingDirectory, "trace.json", Buf);
    }

    Output->setAdditionalOutputForType(file_types::TY_ModuleTrace, filename);
  }
}

void Driver::chooseTBDPath(Compilation &C, const OutputInfo &OI,
                           StringRef workingDirectory,
                           llvm::SmallString<128> &Buf,
                           CommandOutput *Output) const {
  if (C.getArgs().hasArg(options::OPT_emit_tbd, options::OPT_emit_tbd_path)) {
    if (OI.CompilerMode != OutputInfo::Mode::SingleCompile) {
      llvm::outs() << "TBD emission has been disabled, because it requires a "
                   << "single compiler invocation: consider enabling the "
                   << "-whole-module-optimization flag.\n";
    } else {
      auto filename = *getOutputFilenameFromPathArgOrAsTopLevel(
          OI, C.getArgs(), options::OPT_emit_tbd_path, file_types::TY_TBD,
          /*TreatAsTopLevelOutput=*/true, workingDirectory, "tbd", Buf);

      Output->setAdditionalOutputForType(file_types::TY_TBD, filename);
    }
  }
}

void Driver::chooseOptimizationRecordPath(Compilation &C, const OutputInfo &OI,
                                          StringRef workingDirectory,
                                          llvm::SmallString<128> &Buf,
                                          CommandOutput *Output) const {
  if (OI.CompilerMode == OutputInfo::Mode::SingleCompile) {
    auto filename = *getOutputFilenameFromPathArgOrAsTopLevel(
        OI, C.getArgs(), options::OPT_save_optimization_record_path,
        file_types::TY_OptRecord, /*TreatAsTopLevelOutput=*/true,
        workingDirectory, "opt.yaml", Buf);

    Output->setAdditionalOutputForType(file_types::TY_OptRecord, filename);
  } else
    // FIXME: We should use the OutputMap in this case.
    Diags.diagnose({}, diag::warn_opt_remark_disabled);
}

void Driver::chooseObjectiveCHeaderOutputPath(Compilation &C,
                                              const OutputInfo &OI,
                                              const TypeToPathMap *OutputMap,
                                              StringRef workingDirectory,
                                              CommandOutput *Output) const {

  if (hasExistingAdditionalOutput(*Output, file_types::TY_ObjCHeader))
    return;

  StringRef ObjCHeaderPath;
  if (OutputMap) {
    auto iter = OutputMap->find(file_types::TY_ObjCHeader);
    if (iter != OutputMap->end())
      ObjCHeaderPath = iter->second;
  }

  if (ObjCHeaderPath.empty())
    if (auto A = C.getArgs().getLastArg(options::OPT_emit_objc_header_path))
      ObjCHeaderPath = A->getValue();

  if (!ObjCHeaderPath.empty()) {
    Output->setAdditionalOutputForType(file_types::TY_ObjCHeader,
                                       ObjCHeaderPath);
  } else {
    // Put the header next to the primary output file.
    // FIXME: That's not correct if the user /just/ passed -emit-header
    // and not -emit-module.
    addAuxiliaryOutput(C, *Output, file_types::TY_ObjCHeader, OI,
                       /*output file map*/ nullptr, workingDirectory);
  }
}

static unsigned printActions(const Action *A,
                             llvm::DenseMap<const Action *, unsigned> &Ids) {
  if (Ids.count(A))
    return Ids[A];

  std::string str;
  llvm::raw_string_ostream os(str);

  os << Action::getClassName(A->getKind()) << ", ";
  if (const auto *IA = dyn_cast<InputAction>(A)) {
    os << "\"" << IA->getInputArg().getValue() << "\"";
  } else {
    os << "{";
    interleave(*cast<JobAction>(A),
               [&](const Action *Input) { os << printActions(Input, Ids); },
               [&] { os << ", "; });
    os << "}";
  }

  unsigned Id = Ids.size();
  Ids[A] = Id;
  llvm::errs() << Id << ": " << os.str() << ", "
               << file_types::getTypeName(A->getType()) << "\n";

  return Id;
}

void Driver::printActions(const Compilation &C) const {
  llvm::DenseMap<const Action *, unsigned> Ids;
  for (const Action *A : C.getActions()) {
    ::printActions(A, Ids);
  }
}

void Driver::printVersion(const ToolChain &TC, raw_ostream &OS) const {
  OS << version::getSwiftFullVersion(
    version::Version::getCurrentLanguageVersion()) << '\n';
  OS << "Target: " << TC.getTriple().str() << '\n';
}

void Driver::printHelp(bool ShowHidden) const {
  unsigned IncludedFlagsBitmask = 0;
  unsigned ExcludedFlagsBitmask = options::NoDriverOption;

  switch (driverKind) {
  case DriverKind::Interactive:
    ExcludedFlagsBitmask |= options::NoInteractiveOption;
    break;
  case DriverKind::Batch:
  case DriverKind::AutolinkExtract:
  case DriverKind::SwiftFormat:
    ExcludedFlagsBitmask |= options::NoBatchOption;
    break;
  }

  if (!ShowHidden)
    ExcludedFlagsBitmask |= HelpHidden;

  getOpts().PrintHelp(llvm::outs(), Name.c_str(), "Swift compiler",
                      IncludedFlagsBitmask, ExcludedFlagsBitmask,
                      /*ShowAllAliases*/false);
}

bool OutputInfo::mightHaveExplicitPrimaryInputs(
    const CommandOutput &Output) const {
  switch (CompilerMode) {
  case Mode::StandardCompile:
  case Mode::BatchModeCompile:
    return true;
  case Mode::SingleCompile:
    return false;
  case Mode::Immediate:
  case Mode::REPL:
    llvm_unreachable("REPL and immediate modes handled elsewhere");
  }
}
