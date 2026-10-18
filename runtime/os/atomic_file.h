// SPDX-License-Identifier: GPL-3.0-or-later
//
// Saving host files (settings, save cards) so that a crash or power cut never
// leaves one half written.
#pragma once

#include <cstddef>
#include <string>

#include "os/utf8.h"

namespace os {

// Writes `size` bytes to `path` (UTF-8): to a temporary file next to it first,
// synced to the disk, which then replaces `path` in one step. A crash or power
// cut leaves either the old file or the new one, never a mix. Creates the
// folder if needed. On failure, `error` (if given) says why, and nothing is
// left behind.
bool write_file_atomically(const std::string& path, const void* data, std::size_t size, std::string* error = nullptr);

}  // namespace os
