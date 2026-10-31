/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ClipboardFilesTests.h"

#include "deskflow/ClipboardFiles.h"

#include <fstream>

namespace fs = std::filesystem;

namespace {

void writeFile(const fs::path &path, const std::string &contents)
{
  fs::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary);
  file << contents;
}

std::string readFile(const fs::path &path)
{
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void appendUInt32(std::string &buffer, uint32_t value)
{
  for (int shift = 24; shift >= 0; shift -= 8) {
    buffer += static_cast<char>((value >> shift) & 0xff);
  }
}

} // namespace

void ClipboardFilesTests::initTestCase()
{
  QVERIFY(m_dir.isValid());
  ClipboardFiles::setMaximumSize(1024);
}

void ClipboardFilesTests::roundTripFilesAndFolders()
{
  const fs::path source = m_dir.filePath("roundTrip/source").toStdString();
  writeFile(source / "photo.png", std::string("\x89PNG\0data", 9));
  writeFile(source / "docs" / "notes.txt", "hello");
  fs::create_directories(source / "docs" / "empty");

  const auto data = ClipboardFiles::pack({source / "photo.png", source / "docs"});
  QVERIFY(!data.empty());

  const fs::path target = m_dir.filePath("roundTrip/target").toStdString();
  const auto paths = ClipboardFiles::unpackTo(data, target);
  QCOMPARE(paths.size(), size_t(2));
  QCOMPARE(paths[0], target / "photo.png");
  QCOMPARE(paths[1], target / "docs");
  QCOMPARE(readFile(target / "photo.png"), std::string("\x89PNG\0data", 9));
  QCOMPARE(readFile(target / "docs" / "notes.txt"), std::string("hello"));
  QVERIFY(fs::is_directory(target / "docs" / "empty"));
}

void ClipboardFilesTests::unpackSameDataReusesFiles()
{
  const fs::path source = m_dir.filePath("reuse").toStdString();
  writeFile(source / "a.txt", "a");

  const auto data = ClipboardFiles::pack({source / "a.txt"});
  const auto first = ClipboardFiles::unpack(data);
  QCOMPARE(first.size(), size_t(1));
  QCOMPARE(ClipboardFiles::unpack(data), first);
}

void ClipboardFilesTests::unpackSkipsUnsafePaths()
{
  std::string data;
  appendUInt32(data, 2);
  data += 'f';
  appendUInt32(data, 9);
  data += "../escape";
  appendUInt32(data, 0);
  appendUInt32(data, 1);
  data += 'x';
  data += 'f';
  appendUInt32(data, 6);
  data += "ok.txt";
  appendUInt32(data, 0);
  appendUInt32(data, 2);
  data += "ok";

  const fs::path target = m_dir.filePath("unsafe/target").toStdString();
  const auto paths = ClipboardFiles::unpackTo(data, target);
  QCOMPARE(paths.size(), size_t(1));
  QCOMPARE(readFile(target / "ok.txt"), std::string("ok"));
  QVERIFY(!fs::exists(target.parent_path() / "escape"));
}

void ClipboardFilesTests::unpackStopsAtTruncatedData()
{
  std::string data;
  appendUInt32(data, 1);
  data += 'f';
  appendUInt32(data, 5);
  data += "a.txt";
  appendUInt32(data, 0);
  appendUInt32(data, 100);
  data += "short";

  const fs::path target = m_dir.filePath("truncated").toStdString();
  QVERIFY(ClipboardFiles::unpackTo(data, target).empty());
  QVERIFY(!fs::exists(target / "a.txt"));
}

void ClipboardFilesTests::packRefusesFilesOverLimit()
{
  const fs::path source = m_dir.filePath("limit").toStdString();
  writeFile(source / "big.bin", std::string(2048, 'x'));

  ClipboardFiles::setMaximumSize(1);
  QVERIFY(ClipboardFiles::pack({source / "big.bin"}).empty());
  ClipboardFiles::setMaximumSize(1024);
  QVERIFY(!ClipboardFiles::pack({source / "big.bin"}).empty());
}

void ClipboardFilesTests::safeRelativePaths()
{
  QVERIFY(ClipboardFiles::isSafeRelativePath("a.txt"));
  QVERIFY(ClipboardFiles::isSafeRelativePath("folder/a.txt"));
  QVERIFY(!ClipboardFiles::isSafeRelativePath(""));
  QVERIFY(!ClipboardFiles::isSafeRelativePath(".."));
  QVERIFY(!ClipboardFiles::isSafeRelativePath("folder/../../a.txt"));
  QVERIFY(!ClipboardFiles::isSafeRelativePath("/etc/passwd"));
}

QTEST_MAIN(ClipboardFilesTests)
