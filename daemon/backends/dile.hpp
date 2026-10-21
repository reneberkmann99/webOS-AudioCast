// SPDX-License-Identifier: MIT
#pragma once
#include "capture.hpp"
namespace ac {
// Read-only inventory: exact local library fingerprints, no dlopen or HAL calls.
Json dile_inventory();
std::unique_ptr<Capture> make_dile_capture(bool allow_test);
bool dile_restore_verified();
unsigned dile_test_limit_ms(bool allow_test);
// Owner-only recovery of a persisted, interrupted upload-gain transaction. No recording/streaming.
Json restore_dile_gain(const std::string &state_dir, bool allow_test);
}
