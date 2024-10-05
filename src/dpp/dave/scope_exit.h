/************************************************************************************
 *
 * D++, A Lightweight C++ library for Discord
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2021 Craig Edwards and D++ contributors
 * (https://github.com/brainboxdotcc/DPP/graphs/contributors)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * This folder is a modified fork of libdave, https://github.com/discord/libdave
 * Copyright (c) 2024 Discord, Licensed under MIT
 *
 ************************************************************************************/
#pragma once

#include <algorithm>
#include <functional>
#include <utility>

namespace dpp::dave {

class [[nodiscard]] ScopeExit final {
public:
    template <typename Cleanup>
    explicit ScopeExit(Cleanup&& cleanup)
      : cleanup_{std::forward<Cleanup>(cleanup)}
    {
    }

    ScopeExit(ScopeExit&& rhs)
      : cleanup_{std::move(rhs.cleanup_)}
    {
        rhs.cleanup_ = nullptr;
    }

    ~ScopeExit()
    {
        if (cleanup_) {
            cleanup_();
        }
    }

    ScopeExit& operator=(ScopeExit&& rhs)
    {
        cleanup_ = std::move(rhs.cleanup_);
        rhs.cleanup_ = nullptr;
        return *this;
    }

    void Dismiss() { cleanup_ = nullptr; }

private:
    ScopeExit(ScopeExit const&) = delete;
    ScopeExit& operator=(ScopeExit const&) = delete;

    std::function<void()> cleanup_;
};

} // namespace dpp::dave
