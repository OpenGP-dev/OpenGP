// SPDX-License-Identifier: GPL-3.0-or-later
//
// The player's save cards (Mario Cards): one file per card in a folder, and
// the card the player holds, which they insert when the game asks for one.
// The card reader (magcard.h) uses it on the guest thread and the launcher's
// menu on the main thread, so it is thread-safe.
#pragma once

#include <mutex>
#include <string>
#include <vector>

namespace triforce {

class CardFolder {
 public:
  // Cards are `dir`/<name>.bin.
  explicit CardFolder(std::string dir);

  const std::string& dir() const { return dir_; }
  std::string path(const std::string& name) const { return dir_ + "/" + name + ".bin"; }
  bool exists(const std::string& name) const;
  // The cards in the folder, by name.
  std::vector<std::string> list() const;
  // A name no card has yet ("Card 2", ...), for a new card.
  std::string unused_name() const;

  // The card the player holds ("" for none). It need not exist yet: a card
  // the game dispenses is saved under the held name if that has no file.
  std::string held() const;
  void hold(std::string name);

  // Whether the held card is in the reader (set by the reader).
  bool inserted() const;
  void set_inserted(bool inserted);

 private:
  std::string dir_;
  mutable std::mutex mutex_;
  std::string held_;
  bool inserted_ = false;
};

}  // namespace triforce
