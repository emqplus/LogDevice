/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#pragma once

#include <gmock/gmock.h>

#include "logdevice/common/ClientID.h"
#include "logdevice/common/Connection.h"
#include "logdevice/common/ConnectionKind.h"
#include "logdevice/common/FlowGroup.h"
#include "logdevice/common/NetworkDependencies.h"
#include "logdevice/common/NodeID.h"
#include "logdevice/common/ResourceBudget.h"
#include "logdevice/common/Sockaddr.h"
#include "logdevice/common/SocketTypes.h"
#include "logdevice/common/network/IConnectionFactory.h"

namespace facebook { namespace logdevice {

struct MockConnectionFactory : public IConnectionFactory {
  MOCK_METHOD5(
      createConnection,
      std::unique_ptr<Connection>(NodeID node_id,
                                  SocketType socket_type,
                                  ConnectionType connection_type,
                                  FlowGroup& flow_group,
                                  std::unique_ptr<NetworkDependencies> deps));

  MOCK_CONST_METHOD9(
      createConnection,
      std::unique_ptr<Connection>(int fd,
                                  ClientID client_name,
                                  const Sockaddr& client_address,
                                  ResourceBudget::Token connection_token,
                                  SocketType socket_type,
                                  ConnectionType connection_type,
                                  FlowGroup& flow_group,
                                  std::unique_ptr<NetworkDependencies> deps,
                                  ConnectionKind connection_kind));
};
}} // namespace facebook::logdevice
