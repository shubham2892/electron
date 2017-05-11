// Copyright (c) 2017 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#ifndef ATOM_BROWSER_NODE_INSPECTOR_H_
#define ATOM_BROWSER_NODE_INSPECTOR_H_

#include "gin/public/v8_platform.h"
#include "node/inspector/inspector_agent.h"

namespace atom {

class NodeInspector {
 public:
  explicit NodeInspector(node::Environment* env);
  void Start();

 private:
  std::unique_ptr<node::inspector::Agent> agent_;
};

}  // namespace atom

#endif  // ATOM_BROWSER_NODE_INSPECTOR_H_
