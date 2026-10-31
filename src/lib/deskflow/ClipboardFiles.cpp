/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/ClipboardFiles.h"

#include "base/Log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <mutex>

namespace fs = std::filesystem;

namespace {

const char kEntryFile = 'f';
const char kEntryFolder = 'd';

// matches the default clipboard size limit of the server settings
std::atomic<size_t> s_maximumSizeBytes{3 * 1024 * 1024};

struct Entry
{
  char type;
  fs::path relative;
  fs::path source;
};

std::string toUtf8(const fs::path &path)
{
  const auto u8 = path.generic_u8string();
  return std::string(u8.begin(), u8.end());
}

fs::path fromUtf8(const std::string &text)
{
  return fs::path(std::u8string(reinterpret_cast<const char8_t *>(text.data()), text.size()));
}

void writeUInt32(std::string &buffer, uint32_t value)
{
  for (int shift = 24; shift >= 0; shift -= 8) {
    buffer += static_cast<char>((value >> shift) & 0xff);
  }
}

void writeUInt64(std::string &buffer, uint64_t value)
{
  writeUInt32(buffer, static_cast<uint32_t>(value >> 32));
  writeUInt32(buffer, static_cast<uint32_t>(value & 0xffffffff));
}

bool readUInt32(const std::string &buffer, size_t &offset, uint32_t &value)
{
  if (buffer.size() - offset < 4) {
    return false;
  }
  value = 0;
  for (int i = 0; i < 4; ++i) {
    value = (value << 8) | static_cast<uint8_t>(buffer[offset++]);
  }
  return true;
}

bool readUInt64(const std::string &buffer, size_t &offset, uint64_t &value)
{
  uint32_t high = 0;
  uint32_t low = 0;
  if (!readUInt32(buffer, offset, high) || !readUInt32(buffer, offset, low)) {
    return false;
  }
  value = (static_cast<uint64_t>(high) << 32) | low;
  return true;
}

// the name an item keeps when pasted, e.g. "photos" for "C:\Users\me\photos\"
fs::path itemName(const fs::path &path)
{
  return path.has_filename() ? path.filename() : path.parent_path().filename();
}

// lists the copied items, and the contents of copied folders, with their sizes added up
bool collectEntries(const ClipboardFiles::Paths &paths, std::vector<Entry> &entries, uintmax_t &totalSize)
{
  std::error_code ec;
  for (const auto &path : paths) {
    const auto name = itemName(path);
    if (name.empty()) {
      continue;
    }

    if (fs::is_directory(path, ec)) {
      entries.push_back({kEntryFolder, name, path});
      auto options = fs::directory_options::skip_permission_denied;
      for (auto it = fs::recursive_directory_iterator(path, options, ec); !ec && it != fs::end(it); it.increment(ec)) {
        const auto relative = name / fs::relative(it->path(), path, ec);
        if (it->is_directory(ec)) {
          entries.push_back({kEntryFolder, relative, it->path()});
        } else if (it->is_regular_file(ec)) {
          totalSize += it->file_size(ec);
          entries.push_back({kEntryFile, relative, it->path()});
        }
      }
    } else if (fs::is_regular_file(path, ec)) {
      totalSize += fs::file_size(path, ec);
      entries.push_back({kEntryFile, name, path});
    }

    if (totalSize > s_maximumSizeBytes) {
      return false;
    }
  }
  return true;
}

} // namespace

void ClipboardFiles::setMaximumSize(size_t kilobytes)
{
  s_maximumSizeBytes = kilobytes * 1024;
}

size_t ClipboardFiles::maximumSizeBytes()
{
  return s_maximumSizeBytes;
}

std::string ClipboardFiles::pack(const Paths &paths)
{
  std::vector<Entry> entries;
  uintmax_t totalSize = 0;
  if (!collectEntries(paths, entries, totalSize)) {
    LOG_WARN("not sharing copied files, they are larger than the clipboard size limit");
    return {};
  }
  if (entries.empty()) {
    return {};
  }

  std::string buffer;
  buffer.reserve(static_cast<size_t>(totalSize) + entries.size() * 64);
  writeUInt32(buffer, 0); // entry count, filled in below

  uint32_t count = 0;
  for (const auto &entry : entries) {
    std::string contents;
    if (entry.type == kEntryFile) {
      std::ifstream file(entry.source, std::ios::binary);
      if (!file) {
        LOG_WARN("unable to read copied file: %s", toUtf8(entry.source).c_str());
        continue;
      }
      contents.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    const auto relative = toUtf8(entry.relative);
    buffer += entry.type;
    writeUInt32(buffer, static_cast<uint32_t>(relative.size()));
    buffer += relative;
    if (entry.type == kEntryFile) {
      writeUInt64(buffer, contents.size());
      buffer += contents;
    }
    ++count;

    // a file may have grown since it was measured
    if (buffer.size() > s_maximumSizeBytes) {
      LOG_WARN("not sharing copied files, they are larger than the clipboard size limit");
      return {};
    }
  }

  std::string header;
  writeUInt32(header, count);
  buffer.replace(0, 4, header);
  LOG_DEBUG("packed %u copied files and folders, %zu bytes", count, buffer.size());
  return buffer;
}

ClipboardFiles::Paths ClipboardFiles::unpack(const std::string &data)
{
  static std::mutex mutex;
  static size_t lastHash = 0;
  static Paths lastPaths;

  std::scoped_lock lock{mutex};

  // the same clipboard is set again each time the cursor enters this computer
  const auto hash = std::hash<std::string>{}(data);
  if (hash == lastHash && !lastPaths.empty()) {
    std::error_code ec;
    bool allExist = true;
    for (const auto &path : lastPaths) {
      allExist = allExist && fs::exists(path, ec);
    }
    if (allExist) {
      return lastPaths;
    }
  }

  std::error_code ec;
  const auto base = fs::temp_directory_path(ec) / "deskflow-clipboard";
  if (ec) {
    LOG_WARN("unable to find a temporary folder for pasted files");
    return {};
  }

  // remove files from earlier copies, files still in use are left for next time
  for (const auto &old : fs::directory_iterator(base, ec)) {
    std::error_code removeError;
    fs::remove_all(old.path(), removeError);
  }

  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto folder = base / std::to_string(stamp);
  lastPaths = unpackTo(data, folder);
  lastHash = lastPaths.empty() ? 0 : hash;
  return lastPaths;
}

ClipboardFiles::Paths ClipboardFiles::unpackTo(const std::string &data, const fs::path &folder)
{
  std::error_code ec;
  fs::create_directories(folder, ec);
  if (ec) {
    LOG_WARN("unable to create folder for pasted files: %s", toUtf8(folder).c_str());
    return {};
  }

  size_t offset = 0;
  uint32_t count = 0;
  if (!readUInt32(data, offset, count)) {
    return {};
  }

  Paths topLevel;
  for (uint32_t i = 0; i < count; ++i) {
    if (offset >= data.size()) {
      LOG_WARN("copied files data is truncated");
      break;
    }
    const char type = data[offset++];

    uint32_t nameSize = 0;
    if (!readUInt32(data, offset, nameSize) || data.size() - offset < nameSize) {
      LOG_WARN("copied files data is truncated");
      break;
    }
    const auto relative = fromUtf8(data.substr(offset, nameSize));
    offset += nameSize;

    uint64_t fileSize = 0;
    if (type == kEntryFile && (!readUInt64(data, offset, fileSize) || data.size() - offset < fileSize)) {
      LOG_WARN("copied files data is truncated");
      break;
    }

    // never trust a path from the other computer to stay inside the folder
    if ((type != kEntryFile && type != kEntryFolder) || !isSafeRelativePath(relative)) {
      LOG_WARN("ignoring copied file with an unsafe name");
      offset += static_cast<size_t>(fileSize);
      continue;
    }

    const auto target = folder / relative;
    if (type == kEntryFolder) {
      fs::create_directories(target, ec);
    } else {
      fs::create_directories(target.parent_path(), ec);
      std::ofstream file(target, std::ios::binary | std::ios::trunc);
      file.write(data.data() + offset, static_cast<std::streamsize>(fileSize));
      if (!file) {
        LOG_WARN("unable to write pasted file: %s", toUtf8(target).c_str());
      }
      offset += static_cast<size_t>(fileSize);
    }

    const auto top = folder / *relative.begin();
    if (std::find(topLevel.begin(), topLevel.end(), top) == topLevel.end()) {
      topLevel.push_back(top);
    }
  }

  LOG_DEBUG("unpacked %zu copied items to %s", topLevel.size(), toUtf8(folder).c_str());
  return topLevel;
}

bool ClipboardFiles::isSafeRelativePath(const fs::path &path)
{
  if (path.empty() || path.has_root_name() || path.has_root_directory()) {
    return false;
  }
  for (const auto &part : path) {
    const auto text = toUtf8(part);
    if (text.empty() || text == "." || text == "..") {
      return false;
    }
#ifdef _WIN32
    // drive letters and alternate data streams
    if (text.find(':') != std::string::npos) {
      return false;
    }
#endif
  }
  return true;
}
