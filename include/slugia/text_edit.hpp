#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slugia {

class TextEditState {
public:
  std::string value;
  std::size_t caret = 0;
  std::size_t anchor = 0;

  void reset(std::string_view text, bool select_all = false) {
    value.assign(text);
    caret = value.size();
    anchor = select_all ? 0U : caret;
    clearHistory();
  }

  [[nodiscard]] bool hasSelection() const noexcept { return caret != anchor; }
  [[nodiscard]] std::size_t selectionBegin() const noexcept { return std::min(caret, anchor); }
  [[nodiscard]] std::size_t selectionEnd() const noexcept { return std::max(caret, anchor); }
  void selectAll() noexcept { anchor = 0; caret = value.size(); }
  void collapseSelection() noexcept { anchor = caret; }

  void beginTransaction() noexcept { transaction_active_ = true; transaction_recorded_ = false; }
  void endTransaction() noexcept { transaction_active_ = false; transaction_recorded_ = false; }

  bool undo() {
    if (undo_.empty()) return false;
    pushBounded(redo_, snapshot());
    const auto previous = std::move(undo_.back());
    undo_.pop_back();
    restore(previous);
    transaction_recorded_ = false;
    return true;
  }

  bool redo() {
    if (redo_.empty()) return false;
    pushBounded(undo_, snapshot());
    const auto next = std::move(redo_.back());
    redo_.pop_back();
    restore(next);
    transaction_recorded_ = false;
    return true;
  }

  void clearHistory() { undo_.clear(); redo_.clear(); transaction_recorded_ = false; }

  bool insert(std::string_view text) {
    if (text.empty() && !hasSelection()) return false;
    recordBeforeMutation();
    eraseSelectionRaw();
    value.insert(caret, text);
    caret += text.size();
    anchor = caret;
    return true;
  }

  bool backspace() {
    if (!hasSelection() && caret == 0) return false;
    recordBeforeMutation();
    if (eraseSelectionRaw()) return true;
    const auto begin = previousBoundary(caret);
    value.erase(begin, caret - begin);
    caret = anchor = begin;
    return true;
  }

  bool deleteForward() {
    if (!hasSelection() && caret >= value.size()) return false;
    recordBeforeMutation();
    if (eraseSelectionRaw()) return true;
    const auto end = nextBoundary(caret);
    value.erase(caret, end - caret);
    anchor = caret;
    return true;
  }

  bool deleteSelection() {
    if (!hasSelection()) return false;
    recordBeforeMutation();
    return eraseSelectionRaw();
  }

  void moveLeft(bool extend = false) noexcept {
    if (!extend && hasSelection()) caret = selectionBegin();
    else caret = previousBoundary(caret);
    if (!extend) anchor = caret;
  }

  void moveRight(bool extend = false) noexcept {
    if (!extend && hasSelection()) caret = selectionEnd();
    else caret = nextBoundary(caret);
    if (!extend) anchor = caret;
  }

  void moveHome(bool extend = false) noexcept { caret = 0; if (!extend) anchor = caret; }
  void moveEnd(bool extend = false) noexcept { caret = value.size(); if (!extend) anchor = caret; }

private:
  struct Snapshot final {
    std::string value;
    std::size_t caret = 0;
    std::size_t anchor = 0;
  };

  static constexpr std::size_t kHistoryLimit = 128;
  std::vector<Snapshot> undo_;
  std::vector<Snapshot> redo_;
  bool transaction_active_ = false;
  bool transaction_recorded_ = false;

  [[nodiscard]] Snapshot snapshot() const { return {value, caret, anchor}; }

  void restore(const Snapshot& state) {
    value = state.value;
    caret = std::min(state.caret, value.size());
    anchor = std::min(state.anchor, value.size());
  }

  static void pushBounded(std::vector<Snapshot>& stack, Snapshot state) {
    if (stack.size() >= kHistoryLimit) stack.erase(stack.begin());
    stack.push_back(std::move(state));
  }

  void recordBeforeMutation() {
    if (transaction_active_ && transaction_recorded_) return;
    pushBounded(undo_, snapshot());
    redo_.clear();
    if (transaction_active_) transaction_recorded_ = true;
  }

  [[nodiscard]] std::size_t previousBoundary(std::size_t position) const noexcept {
    position = std::min(position, value.size());
    if (position == 0) return 0;
    --position;
    while (position > 0 && (static_cast<unsigned char>(value[position]) & 0xc0U) == 0x80U)
      --position;
    return position;
  }

  [[nodiscard]] std::size_t nextBoundary(std::size_t position) const noexcept {
    position = std::min(position, value.size());
    if (position >= value.size()) return value.size();
    ++position;
    while (position < value.size() &&
           (static_cast<unsigned char>(value[position]) & 0xc0U) == 0x80U)
      ++position;
    return position;
  }

  bool eraseSelectionRaw() {
    if (!hasSelection()) return false;
    const auto begin = selectionBegin();
    value.erase(begin, selectionEnd() - begin);
    caret = anchor = begin;
    return true;
  }
};

} // namespace slugia
