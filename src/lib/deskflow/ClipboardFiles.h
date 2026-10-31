/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

//! Packs copied files into the IClipboard::Format::Files format and back
/*!
The platform clipboards only hold paths to copied files, which mean nothing
on the other computer.  pack() reads the files (and the contents of any
copied folders) into one buffer so it can travel like any other clipboard
format.  unpack() writes that buffer into a temporary folder and returns the
paths of the top level items, which the platform clipboard then offers to
the file manager for pasting.

Data format, all integers big endian:
  4 bytes  => number of entries
  per entry:
    1 byte   => entry type ('f' file, 'd' folder)
    4 bytes  => path length n
    n bytes  => path relative to the copy, UTF-8, '/' separated
    8 bytes  => file size s (files only)
    s bytes  => file contents (files only)
*/
class ClipboardFiles
{
public:
  using Paths = std::vector<std::filesystem::path>;

  //! Set the largest total file size pack() will read, in kilobytes
  static void setMaximumSize(size_t kilobytes);

  //! Get the largest total file size pack() will read, in bytes
  static size_t maximumSizeBytes();

  //! Read the given files and folders into one buffer
  /*!
  Returns an empty string when nothing could be read or when the files are
  larger than the maximum size.
  */
  static std::string pack(const Paths &paths);

  //! Write packed files into a temporary folder
  /*!
  Returns the paths of the top level files and folders.  Unpacking the same
  data again returns the existing copies without writing them again.
  Folders from earlier unpacks are removed.
  */
  static Paths unpack(const std::string &data);

  //! Write packed files into the given folder
  static Paths unpackTo(const std::string &data, const std::filesystem::path &folder);

  //! Check that a relative path received from the peer stays inside the target folder
  static bool isSafeRelativePath(const std::filesystem::path &path);
};
