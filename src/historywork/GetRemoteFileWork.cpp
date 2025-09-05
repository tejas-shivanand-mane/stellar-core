// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/GetRemoteFileWork.h"
#include "fmt/format.h"
#include "history/HistoryArchive.h"
#include "history/HistoryArchiveManager.h"
#include "history/HistoryManager.h"
#include "main/Application.h"
#include "util/Fs.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"

namespace stellar
{
GetRemoteFileWork::GetRemoteFileWork(Application& app,
                                     std::string const& remote,
                                     std::string const& local,
                                     std::shared_ptr<HistoryArchive> archive,
                                     size_t maxRetries)
    : RunCommandWork(app, std::string("get-remote-file ") + remote, maxRetries)
    , mRemote(remote)
    , mLocal(local)
    , mArchive(archive)
    , mFailuresPerSecond(
          app.getMetrics().NewMeter({"history", "get", "failure"}, "failure"))
    , mBytesPerSecond(
          app.getMetrics().NewMeter({"history", "get", "throughput"}, "bytes"))
{
}

CommandInfo
GetRemoteFileWork::getCommand()
{
    mCurrentArchive = mArchive;
    if (!mCurrentArchive)
    {
        mCurrentArchive = mApp.getHistoryArchiveManager()
                              .selectRandomReadableHistoryArchive();
        CLOG_INFO(History, "Selected archive {} to download {}",
                  mCurrentArchive->getName(),
                  std::filesystem::path(mRemote).filename().string());
    }
    releaseAssert(mCurrentArchive);
    releaseAssert(mCurrentArchive->hasGetCmd());
    auto cmdLine = mCurrentArchive->getFileCmd(mRemote, mLocal);
    CLOG_DEBUG(History, "Downloading file: cmd: {}", cmdLine);

    return CommandInfo{cmdLine, std::string()};
}

void
GetRemoteFileWork::onReset()
{
    fs::removeWithLog(mLocal);
    RunCommandWork::onReset();
}

void
GetRemoteFileWork::onSuccess()
{
    releaseAssert(mCurrentArchive);
    mBytesPerSecond.Mark(fs::size(mLocal));
    RunCommandWork::onSuccess();
}

void
GetRemoteFileWork::onFailureRaise()
{
    releaseAssert(mCurrentArchive);
    mFailuresPerSecond.Mark(1);
    CLOG_WARNING(History,
                 "Could not download file: archive {} maybe missing file {}",
                 mCurrentArchive->getName(), mRemote);
    RunCommandWork::onFailureRaise();
}

std::shared_ptr<HistoryArchive>
GetRemoteFileWork::getCurrentArchive() const
{
    return mCurrentArchive;
}
}
