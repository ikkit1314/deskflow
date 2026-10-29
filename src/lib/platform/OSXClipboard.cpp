/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2004 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/OSXClipboard.h"

#include "arch/ArchException.h"
#include "base/Log.h"
#include "deskflow/ClipboardFiles.h"
#include "platform/OSXClipboardBMPConverter.h"
#include "platform/OSXClipboardHTMLConverter.h"
#include "platform/OSXClipboardTextConverter.h"
#include "platform/OSXClipboardUTF16Converter.h"
#include "platform/OSXClipboardUTF8Converter.h"

#include <climits>
#include <filesystem>

namespace {
const CFStringRef kFileURLFlavor = CFSTR("public.file-url");

// pasted files get their own items, away from item 0 used by the converters
const uintptr_t kFirstFileItem = 1000;
} // namespace

//
// OSXClipboard
//

OSXClipboard::OSXClipboard() : m_time(0), m_pboard(nullptr)
{
  m_converters.push_back(new OSXClipboardHTMLConverter);
  m_converters.push_back(new OSXClipboardBMPConverter);
  m_converters.push_back(new OSXClipboardUTF8Converter);
  m_converters.push_back(new OSXClipboardUTF16Converter);
  m_converters.push_back(new OSXClipboardTextConverter);

  OSStatus createErr = PasteboardCreate(kPasteboardClipboard, &m_pboard);
  if (createErr != noErr) {
    LOG_WARN("failed to create clipboard reference: error %i", createErr);
    LOG_ERR("unable to connect to pasteboard, clipboard sharing disabled", createErr);
    m_pboard = nullptr;
    return;
  }

  OSStatus syncErr = PasteboardSynchronize(m_pboard);
  if (syncErr != noErr) {
    LOG_WARN("failed to syncronize clipboard: error %i", syncErr);
  }
}

OSXClipboard::~OSXClipboard()
{
  clearConverters();
}

bool OSXClipboard::empty()
{
  LOG_DEBUG("emptying clipboard");
  if (m_pboard == nullptr)
    return false;

  OSStatus err = PasteboardClear(m_pboard);
  if (err != noErr) {
    LOG_WARN("failed to clear clipboard: error %i", err);
    return false;
  }

  return true;
}

bool OSXClipboard::synchronize()
{
  if (m_pboard == nullptr)
    return false;

  PasteboardSyncFlags flags = PasteboardSynchronize(m_pboard);
  LOG_VERBOSE("flags: %x", flags);

  if (flags & kPasteboardModified) {
    return true;
  }
  return false;
}

void OSXClipboard::add(Format format, const std::string &data)
{
  if (m_pboard == nullptr)
    return;

  LOG_DEBUG("add %d bytes to clipboard format: %d", data.size(), format);
  if (format == IClipboard::Format::Text) {
    LOG_DEBUG("format of data to be added to clipboard was kText");
  } else if (format == IClipboard::Format::Bitmap) {
    LOG_DEBUG("format of data to be added to clipboard was kBitmap");
  } else if (format == IClipboard::Format::HTML) {
    LOG_DEBUG("format of data to be added to clipboard was kHTML");
  } else if (format == IClipboard::Format::Files) {
    LOG_DEBUG("format of data to be added to clipboard was kFiles");
    addFiles(data);
    return;
  }

  for (ConverterList::const_iterator index = m_converters.begin(); index != m_converters.end(); ++index) {

    IOSXClipboardConverter *converter = *index;

    // skip converters for other formats
    if (converter->getFormat() == format) {
      std::string osXData = converter->fromIClipboard(data);
      CFStringRef flavorType = converter->getOSXFormat();
      CFDataRef dataRef = CFDataCreate(kCFAllocatorDefault, (uint8_t *)osXData.data(), osXData.size());
      PasteboardItemID itemID = 0;

      if (dataRef) {
        PasteboardPutItemFlavor(m_pboard, itemID, flavorType, dataRef, kPasteboardFlavorNoFlags);

        CFRelease(dataRef);
        LOG_DEBUG("added %d bytes to clipboard format: %d", data.size(), format);
      }
    }
  }
}

bool OSXClipboard::open(Time time) const
{
  if (m_pboard == nullptr)
    return false;

  LOG_DEBUG("opening clipboard");
  m_time = time;
  return true;
}

void OSXClipboard::close() const
{
  LOG_DEBUG("closing clipboard");
  /* not needed */
}

IClipboard::Time OSXClipboard::getTime() const
{
  return m_time;
}

bool OSXClipboard::has(Format format) const
{
  if (m_pboard == nullptr)
    return false;

  if (format == IClipboard::Format::Files) {
    return hasFiles();
  }

  PasteboardItemID item;
  PasteboardGetItemIdentifier(m_pboard, (CFIndex)1, &item);

  for (ConverterList::const_iterator index = m_converters.begin(); index != m_converters.end(); ++index) {
    IOSXClipboardConverter *converter = *index;
    if (converter->getFormat() == format) {
      PasteboardFlavorFlags flags;
      CFStringRef type = converter->getOSXFormat();

      OSStatus res;

      if ((res = PasteboardGetItemFlavorFlags(m_pboard, item, type, &flags)) == noErr) {
        return true;
      }
    }
  }

  return false;
}

std::string OSXClipboard::get(Format format) const
{
  CFStringRef type;
  PasteboardItemID item;
  std::string result;

  if (m_pboard == nullptr)
    return result;

  if (format == IClipboard::Format::Files) {
    return getFiles();
  }

  PasteboardGetItemIdentifier(m_pboard, (CFIndex)1, &item);

  // find the converter for the first clipboard format we can handle
  IOSXClipboardConverter *converter = nullptr;
  for (ConverterList::const_iterator index = m_converters.begin(); index != m_converters.end(); ++index) {
    converter = *index;

    PasteboardFlavorFlags flags;
    type = converter->getOSXFormat();

    if (converter->getFormat() == format && PasteboardGetItemFlavorFlags(m_pboard, item, type, &flags) == noErr) {
      break;
    }
    converter = nullptr;
  }

  // if no converter then we don't recognize any formats
  if (converter == nullptr) {
    LOG_DEBUG("unable to find converter for data");
    return result;
  }

  // get the clipboard data.
  CFDataRef buffer = nullptr;
  try {
    OSStatus err = PasteboardCopyItemFlavorData(m_pboard, item, type, &buffer);

    if (err != noErr) {
      throw err;
    }

    result = std::string((char *)CFDataGetBytePtr(buffer), CFDataGetLength(buffer));
  } catch (OSStatus err) {
    LOG_DEBUG("exception thrown in OSXClipboard::get MacError (%d)", err);
  } catch (...) {
    LOG_DEBUG("unknown exception in OSXClipboard::get");
    RETHROW_THREADEXCEPTION
  }

  if (buffer != nullptr)
    CFRelease(buffer);

  return converter->toIClipboard(result);
}

void OSXClipboard::clearConverters()
{
  if (m_pboard == nullptr)
    return;

  for (ConverterList::iterator index = m_converters.begin(); index != m_converters.end(); ++index) {
    delete *index;
  }
  m_converters.clear();
}

void OSXClipboard::addFiles(const std::string &data)
{
  const auto paths = ClipboardFiles::unpack(data);
  for (size_t i = 0; i < paths.size(); ++i) {
    const auto path = paths[i].string();
    std::error_code ec;
    const bool isFolder = std::filesystem::is_directory(paths[i], ec);

    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(path.c_str()), static_cast<CFIndex>(path.size()), isFolder
    );
    if (url == nullptr) {
      continue;
    }

    CFDataRef urlData = CFURLCreateData(kCFAllocatorDefault, url, kCFStringEncodingUTF8, true);
    if (urlData != nullptr) {
      auto itemID = reinterpret_cast<PasteboardItemID>(kFirstFileItem + i);
      PasteboardPutItemFlavor(m_pboard, itemID, kFileURLFlavor, urlData, kPasteboardFlavorNoFlags);
      CFRelease(urlData);
    }
    CFRelease(url);
  }
  LOG_DEBUG("added %zu files to clipboard", paths.size());
}

bool OSXClipboard::hasFiles() const
{
  ItemCount count = 0;
  if (PasteboardGetItemCount(m_pboard, &count) != noErr) {
    return false;
  }

  for (ItemCount index = 1; index <= count; ++index) {
    PasteboardItemID item;
    PasteboardFlavorFlags flags;
    if (PasteboardGetItemIdentifier(m_pboard, static_cast<CFIndex>(index), &item) == noErr &&
        PasteboardGetItemFlavorFlags(m_pboard, item, kFileURLFlavor, &flags) == noErr) {
      return true;
    }
  }
  return false;
}

std::string OSXClipboard::getFiles() const
{
  ItemCount count = 0;
  if (PasteboardGetItemCount(m_pboard, &count) != noErr) {
    return {};
  }

  ClipboardFiles::Paths paths;
  for (ItemCount index = 1; index <= count; ++index) {
    PasteboardItemID item;
    CFDataRef urlData = nullptr;
    if (PasteboardGetItemIdentifier(m_pboard, static_cast<CFIndex>(index), &item) != noErr ||
        PasteboardCopyItemFlavorData(m_pboard, item, kFileURLFlavor, &urlData) != noErr || urlData == nullptr) {
      continue;
    }

    CFURLRef url = CFURLCreateWithBytes(
        kCFAllocatorDefault, CFDataGetBytePtr(urlData), CFDataGetLength(urlData), kCFStringEncodingUTF8, nullptr
    );
    CFRelease(urlData);
    if (url == nullptr) {
      continue;
    }

    // Finder can copy file reference urls (file:///.file/id=...), which need resolving to a path
    CFURLRef pathURL = CFURLCreateFilePathURL(kCFAllocatorDefault, url, nullptr);
    CFRelease(url);
    if (pathURL == nullptr) {
      continue;
    }

    UInt8 buffer[PATH_MAX];
    if (CFURLGetFileSystemRepresentation(pathURL, true, buffer, sizeof(buffer))) {
      paths.emplace_back(reinterpret_cast<const char *>(buffer));
    }
    CFRelease(pathURL);
  }

  LOG_DEBUG("reading %zu copied files from the clipboard", paths.size());
  return ClipboardFiles::pack(paths);
}
