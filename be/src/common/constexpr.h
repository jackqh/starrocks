// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

namespace starrocks {

// default value of chunk_size, it's a value decided at compile time
constexpr const int DEFAULT_CHUNK_SIZE = 4096;
constexpr const int MAX_CHUNK_SIZE = 65535;

// Lock is sharded into 32 shards
constexpr int NUM_LOCK_SHARD_LOG = 5;
constexpr int NUM_LOCK_SHARD = 1 << NUM_LOCK_SHARD_LOG;

} // namespace starrocks
