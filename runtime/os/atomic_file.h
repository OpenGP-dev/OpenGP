// SPDX-License-Identifier: GPL-3.0-or-later
//
// Saving host files (settings, save cards) so that a crash or power cut never
// leaves one half written.
#pragma once

#include <cstddef>
#include <string>

#include "os/utf8.h"

namespace os {

// Writes `size` bytes to `path` (UTF-8): to a temporary file next to it first, which
// then replaces `path`. Creates the folder if needed.
bool write_file_atomically(const std::string& path, const void* data, std::size_t size);

}  // namespace os
