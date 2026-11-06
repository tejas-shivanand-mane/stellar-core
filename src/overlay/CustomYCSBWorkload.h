#pragma once

#include <cmath>
#include <cstdlib>
#include <string>
#include <unordered_map>

enum YCSBWorkload
{
    WORKLOAD_A,
    WORKLOAD_B,
    WORKLOAD_F,
    WORKLOAD_W,

    // New explicit read-ratio workloads
    WORKLOAD_R0,
    WORKLOAD_R10,
    WORKLOAD_R20,
    WORKLOAD_R25,
    WORKLOAD_R40,
    WORKLOAD_R50,
    WORKLOAD_R60,
    WORKLOAD_R75,
    WORKLOAD_R80,
    WORKLOAD_R100
};

static double zipfian_alpha = 0.99;
static uint64_t zipfian_n = 1000000;

// Change this for each experiment.
static YCSBWorkload currentWorkload = WORKLOAD_W;

static uint64_t
zipfianNext()
{
    static double zeta2 = 0.0;
    static double zetaN = 0.0;
    static bool initialized = false;

    if (!initialized)
    {
        for (uint64_t i = 1; i <= zipfian_n; i++)
        {
            zetaN += 1.0 / std::pow(i, zipfian_alpha);
        }

        zeta2 = 1.0 + 1.0 / std::pow(2, zipfian_alpha);
        initialized = true;
    }

    double u = static_cast<double>(std::rand()) / RAND_MAX;
    double uz = u * zetaN;

    if (uz < 1.0)
    {
        return 1;
    }

    if (uz < 1.0 + std::pow(0.5, zipfian_alpha))
    {
        return 2;
    }

    return static_cast<uint64_t>(
        zipfian_n *
        std::pow(zeta2 / zetaN * u, 1.0 / (1.0 - zipfian_alpha)));
}

static std::string
generateReadUpdateOp(std::string const& key, double r, double readRatio)
{
    if (r < readRatio)
    {
        return "READ " + key;
    }

    return "UPDATE " + key + " val" + std::to_string(std::rand());
}

static std::string
generateYCSBOp()
{
    std::string key = "user" + std::to_string(zipfianNext());
    double r = static_cast<double>(std::rand()) / RAND_MAX;

    switch (currentWorkload)
    {
        case WORKLOAD_A:
            return (r < 0.5) ? "READ " + key
                            : "UPDATE " + key + " val" + std::to_string(std::rand());

        case WORKLOAD_B:
            return (r < 0.95) ? "READ " + key
                            : "UPDATE " + key + " val" + std::to_string(std::rand());

        case WORKLOAD_F:
            return (r < 0.5) ? "READ " + key
                            : "RMW " + key + " val" + std::to_string(std::rand());

        case WORKLOAD_W:
            return "UPDATE " + key + " val" + std::to_string(std::rand());

        // New read-ratio workloads
        case WORKLOAD_R0:
            return generateReadUpdateOp(key, r, 0.00);

        case WORKLOAD_R10:
            return generateReadUpdateOp(key, r, 0.10);

        case WORKLOAD_R25:
            return generateReadUpdateOp(key, r, 0.25);

        case WORKLOAD_R20:
            return generateReadUpdateOp(key, r, 0.20);


        case WORKLOAD_R40:
            return generateReadUpdateOp(key, r, 0.40);

        case WORKLOAD_R60:
            return generateReadUpdateOp(key, r, 0.60);


        case WORKLOAD_R80:
            return generateReadUpdateOp(key, r, 0.80);

        case WORKLOAD_R50:
            return generateReadUpdateOp(key, r, 0.50);

        case WORKLOAD_R75:
            return generateReadUpdateOp(key, r, 0.75);

        case WORKLOAD_R100:
            return generateReadUpdateOp(key, r, 1.00);

    }

    return "READ " + key;
}