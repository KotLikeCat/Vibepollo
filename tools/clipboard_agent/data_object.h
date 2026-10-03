/**
 * @file tools/clipboard_agent/data_object.h
 * @brief OLE data object exposing an offer as virtual files (and CF_HDROP when prefetched).
 */
#pragma once

#include "range_source.h"

#include <objidl.h>
#include <windows.h>

#include <filesystem>
#include <memory>

namespace clipboard_agent {
  inline constexpr std::size_t k_max_descriptor_name = 259;  ///< FILEDESCRIPTORW::cFileName holds MAX_PATH wchar_t incl. NUL

  /// Creates the IDataObject (refcount 1). It owns a prefetcher when `o.prefetch`.
  /// Entries whose relative path exceeds 259 UTF-16 units cannot be expressed in a FILEDESCRIPTORW; they are
  /// left out of the descriptor list (see descriptor_omitted_count) and only reachable through CF_HDROP.
  IDataObject *create_data_object(offer o, std::shared_ptr<range_source> src, std::filesystem::path prefetch_root);
  /// Number of entries create_data_object() leaves out of the descriptor list.
  std::size_t descriptor_omitted_count(const offer &o);
}  // namespace clipboard_agent
