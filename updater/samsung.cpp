/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "updater/samsung.h"

#include <fcntl.h>
#include <libgen.h>
#include <unistd.h>

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <edify/expr.h>
#include <otautil/error_code.h>
#include <ziparchive/zip_archive.h>

Value* MarkHeaderBtFn(const char* name, State* state,
                      const std::vector<std::unique_ptr<Expr>>& argv) {
  std::vector<std::string> args;
  const char* partition;
  uint32_t magicOffset;
  uint32_t numImages;
  uint32_t magic1;

  if (argv.size() < 4 || argv.size() > 4 || !ReadArgs(state, argv, &args))
    return ErrorAbort(state, kArgsParsingFailure,
                      "%s() error parsing arguments", name);

  partition = args[0].c_str();
  magicOffset = std::atoi(args[1].c_str());
  numImages = std::atoi(args[2].c_str());
  magic1 = std::atoi(args[3].c_str());

  int fd = open(partition, O_RDWR);
  if (fd < 0)
    return ErrorAbort(state, kFileOpenFailure, "%s() failed to open %s", name,
                      partition);

  magic1 = magic1 << (magicOffset * 8);
  numImages = numImages << (magicOffset * 8);
  write(fd, (char *)&magic1, sizeof(uint32_t));
  write(fd, (char *)&numImages, sizeof(uint32_t));
  close(fd);

  return StringValue("t");
}

Value *VerifyNoDowngradeFn(const char* name, State* state,
                           const std::vector<std::unique_ptr<Expr>>& argv) {
  std::vector<std::string> args;
  std::string bl1;
  std::string bl2;

  if (argv.size() < 2 || argv.size() > 2 || !ReadArgs(state, argv, &args))
    return ErrorAbort(state, kArgsParsingFailure,
                      "%s() error parsing arguments", name);

  bl1 = args[0].c_str();
  bl2 = args[1].c_str();

  std::string bl1_model = bl1.substr(0, bl1.size() - 8);
  std::string bl2_model = bl2.substr(0, bl2.size() - 8);
  if (bl1_model != bl2_model) {
    LOG(ERROR) << "Mismatched model (" << bl1_model << " vs. " << bl2_model << ")";
    return StringValue("");
  }

  std::string bl1_region = bl1.substr(bl1_model.size(), 2);
  std::string bl2_region = bl2.substr(bl2_model.size(), 2);
  if (bl1_region != bl2_region) {
    LOG(ERROR) << "Mismatched region (" << bl1_region << " vs. " << bl2_region << ")";
    return StringValue("");
  }

  char bl1_rp = bl1[bl1_model.size() + 3];
  char bl2_rp = bl2[bl2_model.size() + 3];
  if (bl1_rp > bl2_rp) {
    LOG(ERROR) << "Higher rollback bit (" << bl1_rp << " vs. " << bl2_rp << ")";
    return StringValue("");
  }

  char bl1_major = bl1[bl1_model.size() + 4];
  char bl2_major = bl2[bl2_model.size() + 4];
  if (bl1_major < bl2_major) {
    return StringValue("t");
  } else if (bl1_major > bl2_major) {
    LOG(ERROR) << "Higher OS major version (" << bl1_major << " vs. " << bl2_major << ")";
    return StringValue("");
  }

  char bl1_year = bl1[bl1_model.size() + 5];
  char bl2_year = bl2[bl2_model.size() + 5];
  if (bl1_year < bl2_year) {
    return StringValue("t");
  } else if (bl1_year > bl2_year) {
    LOG(ERROR) << "Higher OS year build (" << bl1_year << " vs. " << bl2_year << ")";
    return StringValue("");
  }

  char bl1_month = bl1[bl1_model.size() + 6];
  char bl2_month = bl2[bl2_model.size() + 6];
  if (bl1_month < bl2_month) {
    return StringValue("t");
  } else if (bl1_month > bl2_month) {
    LOG(ERROR) << "Higher OS month build (" << bl1_month << " vs. " << bl2_month << ")";
    return StringValue("");
  }

  char bl1_minor = bl1[bl1_model.size() + 7];
  char bl2_minor = bl2[bl2_model.size() + 7];
  if (bl1_minor < bl2_minor) {
    return StringValue("t");
  } else if (bl1_minor > bl2_minor) {
    LOG(ERROR) << "Higher OS minor version (" << bl1_minor << " vs. " << bl2_minor << ")";
    return StringValue("");
  }

  LOG(ERROR) << "End reached (" << bl1 << " vs. " << bl2 << ")";
  return StringValue("");
}

#define FILENAME_MAX_LEN 32

Value* WriteDataBtFn(const char* name, State* state,
                     const std::vector<std::unique_ptr<Expr>>& argv) {
  std::vector<std::string> args;
  const char* file;
  const char* filename;
  const char* partition;
  uint32_t offset;
  uint32_t filesize;

  if (argv.size() < 4 || argv.size() > 4 || !ReadArgs(state, argv, &args))
    return ErrorAbort(state, kArgsParsingFailure,
                      "%s() error parsing arguments", name);

  file = args[0].c_str();
  filename = basename(file);
  partition = args[1].c_str();
  offset = std::atoi(args[2].c_str());
  filesize = std::atoi(args[3].c_str());

  int fd = open(partition, O_RDWR);
  if (fd < 0)
    return ErrorAbort(state, kFileOpenFailure, "%s() failed to open %s", name,
                      partition);

  char filename_padded[FILENAME_MAX_LEN] = {0};
  strcpy(&filename_padded[0], filename);

  lseek(fd, offset, SEEK_SET);
  write(fd, &filename_padded[0], FILENAME_MAX_LEN);
  write(fd, (char *)&filesize, sizeof(uint32_t));

  // write data
  ZipArchiveHandle za = state->updater->GetPackageHandle();
  ZipEntry64 entry;
  if (FindEntry(za, file, &entry) != 0) {
    return ErrorAbort(state, kPackageExtractFileFailure,
                      "%s() %s not found in package", name, file);
  }

  if (ExtractEntryToFile(za, &entry, fd))
    return ErrorAbort(state, kPackageExtractFileFailure,
                      "%s() failed to extract %s from package", name, file);

  close(fd);

  return StringValue("t");
}

void RegisterSamsungFunctions() {
  RegisterFunction("mark_header_bt", MarkHeaderBtFn);
  RegisterFunction("verify_no_downgrade", VerifyNoDowngradeFn);
  RegisterFunction("write_data_bt", WriteDataBtFn);
}
