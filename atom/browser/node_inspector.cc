// Copyright (c) 2017 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "atom/browser/node_inspector.h"

#include "atom/common/node_includes.h"

namespace atom {

NodeInspector::NodeInspector(node::Environment* env) :
    agent_(new node::inspector::Agent(env)) {
}

NodeInspector::~NodeInspector() {
  agent_->Stop();
}

void NodeInspector::Start() {
  node::DebugOptions options;
  options.EnableDebugAgent(node::DebugAgentType::kInspector);
  agent_->Start(gin::V8Platform::Get(), nullptr, options);
}

}  // namespace atom
