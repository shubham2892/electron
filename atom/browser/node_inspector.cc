// Copyright (c) 2017 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "atom/browser/node_inspector.h"

#include "atom/common/node_includes.h"
#include "base/command_line.h"

namespace atom {

NodeInspector::NodeInspector(node::Environment* env) :
    agent_(new node::inspector::Agent(env)) {
}

void NodeInspector::Start() {
  node::DebugOptions options;
  base::CommandLine* cmd = base::CommandLine::ForCurrentProcess();
  for (const auto& arg : cmd->argv())
    options.ParseOption(arg);
  if (options.inspector_enabled())
    agent_->Start(gin::V8Platform::Get(), nullptr, options);
}

}  // namespace atom
