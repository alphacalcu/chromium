// Copyright (c) 2009 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/common/sandbox_init_wrapper.h"

#include "base/command_line.h"
#include "chrome/common/chrome_switches.h"

bool SandboxInitWrapper::InitializeSandbox(const CommandLine& command_line,
                                           const std::string& process_type) {
  // TODO(port): Does Linux need to do anything here?
  return true;
}
