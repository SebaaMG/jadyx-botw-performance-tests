// SPDX-FileCopyrightText: Copyright 2026 SeriesX contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <atomic>
#include "video_core/buffer_cache/binding_dependency.h"
namespace VideoCommon {
namespace { std::atomic<bool> binding_dependency_reuse{false}; }
void SetBindingDependencyReuse(bool enabled) {
    binding_dependency_reuse.store(enabled, std::memory_order_relaxed);
}
bool BindingDependencyReuseEnabled() {
    return binding_dependency_reuse.load(std::memory_order_relaxed);
}
} // namespace VideoCommon
