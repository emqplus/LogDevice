/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#pragma once

#include "logdevice/admin/if/gen-cpp2/admin_types.h"
#include "logdevice/common/ThriftCodec.h"

namespace facebook { namespace logdevice {
/**
 * toString helpers
 */
std::string toString(const thrift::NodeID& node_id);
std::string toString(const thrift::SocketAddressFamily& family);
std::string toString(const thrift::SocketAddress& address);

template <class T>
std::string thriftToJson(T obj) {
  return facebook::logdevice::ThriftCodec::serialize<
      apache::thrift::SimpleJSONSerializer>(obj);
}
}} // namespace facebook::logdevice
