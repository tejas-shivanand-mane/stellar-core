// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/GetAndUnzipRemoteFileWork.h"
#include "catchup/LedgerApplyManager.h"
#include "history/HistoryArchive.h"
#include "historywork/GetRemoteFileWork.h"
#include "historywork/GunzipFileWork.h"
#include "util/Fs.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include <Tracy.hpp>
#include <fmt/format.h>

namespace stellar
{

GetAndUnzipRemoteFileWork::GetAndUnzipRemoteFileWork(
    Application& app, FileTransferInfo ft,
    std::shared_ptr<HistoryArchive> archive, size_t retry,
    bool logErrorOnFailure)
    : Work(app, std::string("get-and-unzip-remote-file ") + ft.remoteName(),
           retry)
    , mFt(std::move(ft))
    , mArchive(archive)
    , mLogErrorOnFailure(logErrorOnFailure)
{
}

std::string
GetAndUnzipRemoteFileWork::getStatus() const
{
    if (mGunzipFileWork)
    {
        return mGunzipFileWork->getStatus();
    }
    else if (mGetRemoteFileWork)
    {
        return mGetRemoteFileWork->getStatus();
    }
    return BasicWork::getStatus();
}

void
GetAndUnzipRemoteFileWork::doReset()
{
    fs::removeWithLog(mFt.localPath_nogz());
    fs::removeWithLog(mFt.localPath_gz());
    fs::removeWithLog(mFt.localPath_gz_tmp());
    mGetRemoteFileWork.reset();
    mGunzipFileWork.reset();
}

void
GetAndUnzipRemoteFileWork::onFailureRaise()
{
    // Blame archive if file was downloaded but failed validation
    std::shared_ptr<HistoryArchive> ar = getArchive();
    if (ar)
    {
        if (mLogErrorOnFailure)
        {
            CLOG_ERROR(History, "Archive {}: file {} is maybe corrupt",
                       ar->getName(), mFt.remoteName());
        }
        else
        {
            CLOG_WARNING(History, "Archive {}: file {} is maybe corrupt",
                         ar->getName(), mFt.remoteName());
        }
    }
    Work::onFailureRaise();
}

void
GetAndUnzipRemoteFileWork::onSuccess()
{
    mApp.getLedgerApplyManager().fileDownloaded(mFt.getType());
    Work::onSuccess();
}

BasicWork::State
GetAndUnzipRemoteFileWork::doWork()
{
    ZoneScoped;
    if (mGunzipFileWork)
    {
        // Download completed, unzipping started
        releaseAssert(mGetRemoteFileWork);
        releaseAssert(mGetRemoteFileWork->getState() == State::WORK_SUCCESS);
        auto state = mGunzipFileWork->getState();
        if (state == State::WORK_SUCCESS && !fs::exists(mFt.localPath_nogz()))
        {
            if (mLogErrorOnFailure)
            {
                CLOG_ERROR(History,
                           "Downloading and unzipping {}: .nogz not found",
                           mFt.remoteName());
            }
            else
            {
                CLOG_WARNING(History,
                             "Downloading and unzipping {}: .nogz not found",
                             mFt.remoteName());
            }
            return State::WORK_FAILURE;
        }
        return state;
    }
    else if (mGetRemoteFileWork)
    {
        // Download started
        auto state = mGetRemoteFileWork->getState();
        if (state == State::WORK_SUCCESS)
        {
            if (!validateFile())
            {
                return State::WORK_FAILURE;
            }
            mGunzipFileWork = addWork<GunzipFileWork>(mFt.localPath_gz(), false,
                                                      BasicWork::RETRY_NEVER);
            return State::WORK_RUNNING;
        }
        return state;
    }
    else
    {
        CLOG_DEBUG(History, "Downloading and unzipping {}", mFt.remoteName());
        mGetRemoteFileWork =
            addWork<GetRemoteFileWork>(mFt.remoteName(), mFt.localPath_gz_tmp(),
                                       mArchive, BasicWork::RETRY_NEVER);
        return State::WORK_RUNNING;
    }
}

bool
GetAndUnzipRemoteFileWork::validateFile()
{
    ZoneScoped;
    if (!fs::exists(mFt.localPath_gz_tmp()))
    {
        if (mLogErrorOnFailure)
        {
            CLOG_ERROR(History,
                       "Downloading and unzipping {}: .tmp file not found",
                       mFt.remoteName());
        }
        else
        {
            CLOG_WARNING(History,
                         "Downloading and unzipping {}: .tmp file not found",
                         mFt.remoteName());
        }
        return false;
    }

    CLOG_TRACE(History, "Downloading and unzipping {}: renaming .gz.tmp to .gz",
               mFt.remoteName());
    if (fs::exists(mFt.localPath_gz()) &&
        std::remove(mFt.localPath_gz().c_str()))
    {
        if (mLogErrorOnFailure)
        {
            CLOG_ERROR(History,
                       "Downloading and unzipping {}: failed to remove .gz",
                       mFt.remoteName());
        }
        else
        {
            CLOG_WARNING(History,
                         "Downloading and unzipping {}: failed to remove .gz",
                         mFt.remoteName());
        }
        return false;
    }

    if (std::rename(mFt.localPath_gz_tmp().c_str(), mFt.localPath_gz().c_str()))
    {
        if (mLogErrorOnFailure)
        {
            CLOG_ERROR(
                History,
                "Downloading and unzipping {}: failed to rename .gz.tmp to .gz",
                mFt.remoteName());
        }
        else
        {
            CLOG_WARNING(
                History,
                "Downloading and unzipping {}: failed to rename .gz.tmp to .gz",
                mFt.remoteName());
        }
        return false;
    }

    CLOG_TRACE(History, "Downloading and unzipping {}: renamed .gz.tmp to .gz",
               mFt.remoteName());

    if (!fs::exists(mFt.localPath_gz()))
    {
        if (mLogErrorOnFailure)
        {
            CLOG_ERROR(History, "Downloading and unzipping {}: .gz not found",
                       mFt.remoteName());
        }
        else
        {
            CLOG_WARNING(History, "Downloading and unzipping {}: .gz not found",
                         mFt.remoteName());
        }
        return false;
    }

    return true;
}

std::shared_ptr<HistoryArchive>
GetAndUnzipRemoteFileWork::getArchive() const
{
    if (mGetRemoteFileWork &&
        mGetRemoteFileWork->getState() == BasicWork::State::WORK_SUCCESS)
    {
        return mGetRemoteFileWork->getCurrentArchive();
    }
    return nullptr;
}
}
