/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "base/memory_tool.h"

#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#if defined(__linux__) && defined(__arm__)
#include <sys/personality.h>
#include <sys/utsname.h>
#endif

#include "android-base/logging.h"
#include "android-base/stringprintf.h"
#include "android-base/strings.h"

#include "arch/instruction_set_features.h"
#include "arch/mips/instruction_set_features_mips.h"
#include "art_method-inl.h"
#include "base/callee_save_type.h"
#include "base/dumpable.h"
#include "base/file_utils.h"
#include "base/leb128.h"
#include "base/macros.h"
#include "base/mutex.h"
#include "base/os.h"
#include "base/scoped_flock.h"
#include "base/stl_util.h"
#include "base/stringpiece.h"
#include "base/time_utils.h"
#include "base/timing_logger.h"
#include "base/unix_file/fd_file.h"
#include "base/utils.h"
#include "class_linker.h"
#include "class_loader_context.h"
#include "cmdline_parser.h"
#include "compiler.h"
#include "compiler_callbacks.h"
#include "debug/elf_debug_writer.h"
#include "debug/method_debug_info.h"
#include "dexlayout.h"
#include "dex/art_dex_file_loader.h"
#include "dex/descriptors_names.h"
#include "dex/dex_file-inl.h"
#include "dex/quick_compiler_callbacks.h"
#include "dex/verification_results.h"
#include "driver/compiler_driver.h"
#include "driver/compiler_options.h"
#include "driver/compiler_options_map-inl.h"
#include "elf_file.h"
#include "gc/space/image_space.h"
#include "gc/space/space-inl.h"
#include "gc/verification.h"
#include "interpreter/unstarted_runtime.h"
#include "java_vm_ext.h"
#include "jit/profile_compilation_info.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "oat_file.h"
#include "oat_file_assistant.h"
#include "runtime.h"
#include "runtime_options.h"
#include "scoped_thread_state_change-inl.h"
#include "vdex_file.h"
#include "verifier/verifier_deps.h"
#include "well_known_classes.h"
#include "zip_archive.h"

namespace art {

using android::base::StringAppendV;
using android::base::StringPrintf;

static int original_argc;
static char** original_argv;

static std::string CommandLine() {
  std::vector<std::string> command;
  for (int i = 0; i < original_argc; ++i) {
    command.push_back(original_argv[i]);
  }
  return android::base::Join(command, ' ');
}

static void UsageErrorV(const char* fmt, va_list ap) {
  std::string error;
  StringAppendV(&error, fmt, ap);
  LOG(ERROR) << error;
}

static void UsageError(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  UsageErrorV(fmt, ap);
  va_end(ap);
}

NO_RETURN static void Usage(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  UsageErrorV(fmt, ap);
  va_end(ap);

  UsageError("Command: %s", CommandLine().c_str());
  UsageError("Usage: vdex2dex <vdex file> <output dex file>");
  exit(EXIT_FAILURE);
}

static int DoVdex2dex(int argc, char** argv) {
  original_argc = argc;
  original_argv = argv;

  InitLogging(argv, Runtime::Abort);

  // Skip over argv[0].
  argv++;
  argc--;

  if (argc < 2) {
    Usage("Insufficient arguments.");
  }

  const char *in_vdex_fn = argv[0];
  const char *out_dex_fn = argv[1];
  art::MemMap::Init();

  std::string error_msg;
  std::unique_ptr<VdexFile> in_vdex = VdexFile::Open(in_vdex_fn,
                                                     /* writable */ false,
                                                     /* low_4gb */ false,
                                                     /* unquicken */ true,
                                                     &error_msg);
  if (in_vdex == nullptr) {
    LOG(ERROR) << "Failed to open vdex file: " << error_msg;
    return 1;
  }

  std::vector<std::unique_ptr<const DexFile>> dex_files;
  if (!in_vdex->OpenAllDexFiles(&dex_files, &error_msg)) {
    LOG(ERROR) << "Failed to convert vdex file: " << error_msg;
    return 1;
  }

  if (dex_files.size() < 1) {
    LOG(ERROR) << "Vdex file contains no dex files";
    return 1;
  } else if (dex_files.size() > 1) {
    LOG(WARNING) << "Vdex file contains " << dex_files.size() << " files, only handling first";
  }

  std::unique_ptr<const DexFile> dex(std::move(dex_files[0]));
  in_vdex->UnquickenDexFile(*dex, *dex, true);
  std::unique_ptr<File> out_dex_f(OS::CreateEmptyFile(out_dex_fn));
  if (out_dex_f == nullptr) {
    LOG(ERROR) << "Failed to open output file";
    return 1;
  }
  if (!out_dex_f->WriteFully(dex->Begin(), dex->Size()) || out_dex_f->Flush()) {
    LOG(ERROR) << "Failed to write to file";
  }
  UNUSED(out_dex_f->Close());

  LOG(INFO) << "Wrote dex file to " << out_dex_fn;

  if (dex_files.size() > 1) {
    LOG(WARNING) << "Multiple dex files; first written";
  }

  return 0;
}
}  // namespace art

int main(int argc, char** argv) {
  int result = static_cast<int>(art::DoVdex2dex(argc, argv));
  if (!art::kIsDebugBuild) {
    _exit(result);
  }
  return result;
}
