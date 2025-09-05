// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "medida/buckets.h"
#include "medida/timer.h"
#include <Tracy.hpp>
#include <map>
#include <memory>
#include <mutex>

namespace medida
{
class Buckets::Impl
{
    std::map<double, std::shared_ptr<Timer>> mBuckets;
    const std::chrono::nanoseconds mDurationUnit;
    std::int64_t mDurationUnitNanos;
    mutable std::mutex mMutex;

  public:
    Impl(std::set<double> const& bucketBoundaries,
         std::chrono::nanoseconds duration_unit,
         std::chrono::nanoseconds rate_unit)
        : mDurationUnit(duration_unit)
        , mDurationUnitNanos(duration_unit.count())
    {
        for (auto b: bucketBoundaries)
        {
            auto m = std::make_shared<Timer>(duration_unit, rate_unit);
            mBuckets.insert(std::make_pair(b, m));
        }
        mBuckets.insert(
            std::make_pair(std::numeric_limits<double>::max(),
                           std::make_shared<Timer>(duration_unit, rate_unit)));
    }

    void
    forBuckets(std::function<void(std::pair<double, std::shared_ptr<Timer>>)> f)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        for (auto const& kv : mBuckets)
        {
            f(kv);
        }
    }

    std::chrono::nanoseconds boundary_unit() const
    {
        return mDurationUnit;
    }

    void
    Update(std::chrono::nanoseconds value)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        double v = double(value.count()) / mDurationUnitNanos;
        auto it = mBuckets.lower_bound(v);
        it->second->Update(value);
    }

    void Clear()
    {
        std::lock_guard<std::mutex> lock(mMutex);
        for (auto kv: mBuckets)
        {
            kv.second->Clear();
        }
    }
};

Buckets::Buckets(
    std::set<double> const& boundaries,
    std::chrono::nanoseconds duration_unit,
                 std::chrono::nanoseconds rate_unit)
    : impl_(new Buckets::Impl(boundaries, duration_unit, rate_unit))
{
    ZoneScoped;
}

Buckets::~Buckets()
{
    ZoneScoped;
}

void
Buckets::Process(MetricProcessor& processor)
{
    ZoneScoped;
    processor.Process(*this);
}

void
Buckets::forBuckets(std::function<void(std::pair<double, std::shared_ptr<Timer>>)> f)
{
    ZoneScoped;
    return impl_->forBuckets(f);
}

std::chrono::nanoseconds
Buckets::boundary_unit() const
{
    ZoneScoped;
    return impl_->boundary_unit();
}

void
Buckets::Update(std::chrono::nanoseconds value)
{
    ZoneScoped;
    impl_->Update(value);
}

void
Buckets::Clear()
{
    ZoneScoped;
    impl_->Clear();
}

}
