/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "base/Log.h"

#include <QTemporaryDir>
#include <QTest>

class ClipboardFilesTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void roundTripFilesAndFolders();
  void unpackSameDataReusesFiles();
  void unpackSkipsUnsafePaths();
  void unpackStopsAtTruncatedData();
  void packRefusesFilesOverLimit();
  void safeRelativePaths();

private:
  Log m_log;
  QTemporaryDir m_dir;
};
