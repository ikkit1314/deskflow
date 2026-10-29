/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/MSWindowsClipboardFilesConverter.h"

#include "base/Log.h"
#include "deskflow/ClipboardFiles.h"

#include <ShlObj.h>

#include <cstring>

IClipboard::Format MSWindowsClipboardFilesConverter::getFormat() const
{
  return IClipboard::Format::Files;
}

UINT MSWindowsClipboardFilesConverter::getWin32Format() const
{
  return CF_HDROP;
}

HANDLE MSWindowsClipboardFilesConverter::fromIClipboard(const std::string &data) const
{
  const auto paths = ClipboardFiles::unpack(data);
  if (paths.empty()) {
    return nullptr;
  }

  // a DROPFILES header followed by the paths, each null terminated, and a
  // final null to end the list
  std::wstring list;
  for (const auto &path : paths) {
    list += path.wstring();
    list += L'\0';
  }
  list += L'\0';

  const SIZE_T listBytes = list.size() * sizeof(wchar_t);
  HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, sizeof(DROPFILES) + listBytes);
  if (handle == nullptr) {
    return nullptr;
  }

  auto *dropFiles = static_cast<DROPFILES *>(GlobalLock(handle));
  if (dropFiles == nullptr) {
    GlobalFree(handle);
    return nullptr;
  }
  std::memset(dropFiles, 0, sizeof(DROPFILES));
  dropFiles->pFiles = sizeof(DROPFILES);
  dropFiles->fWide = TRUE;
  std::memcpy(reinterpret_cast<char *>(dropFiles) + sizeof(DROPFILES), list.data(), listBytes);
  GlobalUnlock(handle);

  return handle;
}

std::string MSWindowsClipboardFilesConverter::toIClipboard(HANDLE data) const
{
  const auto *dropFiles = static_cast<const DROPFILES *>(GlobalLock(data));
  if (dropFiles == nullptr) {
    return {};
  }

  const SIZE_T size = GlobalSize(data);
  ClipboardFiles::Paths paths;
  if (dropFiles->pFiles < size) {
    const auto *start = reinterpret_cast<const char *>(dropFiles) + dropFiles->pFiles;
    const auto *end = reinterpret_cast<const char *>(dropFiles) + size;
    if (dropFiles->fWide) {
      const auto *name = reinterpret_cast<const wchar_t *>(start);
      const auto *last = reinterpret_cast<const wchar_t *>(end);
      while (name < last && *name != L'\0') {
        const auto length = wcsnlen(name, static_cast<size_t>(last - name));
        paths.emplace_back(std::wstring(name, length));
        name += length + 1;
      }
    } else {
      const auto *name = start;
      while (name < end && *name != '\0') {
        const auto length = strnlen(name, static_cast<size_t>(end - name));
        paths.emplace_back(std::string(name, length));
        name += length + 1;
      }
    }
  }
  GlobalUnlock(data);

  LOG_DEBUG("reading %zu copied files from the clipboard", paths.size());
  return ClipboardFiles::pack(paths);
}
