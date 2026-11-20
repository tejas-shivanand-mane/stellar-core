#!/usr/bin/env python3
"""Run the production PBFT adapter on queued, authenticated test channels.

Requires generated Stellar XDR headers (as for tests/tetrabft/multishot.py).
PBFT_TEST_INCLUDES can supply additional generated include directories.
"""
import os
from pathlib import Path
import runpy
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
common = runpy.run_path(str(ROOT / 'tests/tetrabft/multishot.py'))
pre = common['PRE']
# Use the same peer transport, client queue, state machine and ACK fixtures.
# Replace just the test key and clock interfaces needed by PBFT.
pre = pre.replace('namespace stellar {', '''
namespace stellar {
struct TestClock : std::chrono::steady_clock {
    static inline time_point t{std::chrono::seconds(1)};
    static time_point now(){return t;}
};
template<class B> Hash sha256(B const& b){Hash h{};SHA256(reinterpret_cast<unsigned char const*>(b.data()),b.size(),h.data());return h;}
''', 1)
pre = pre.replace('struct Secret {', '''
// Each test node owns a reproducible Ed25519 key. Exercise transferable
// signatures over exactly the bytes the production adapter builds.
using TestKey = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using TestCtx = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
EVP_PKEY* testKey(NodeID const& id) {
    static std::map<NodeID, TestKey> keys;
    auto it = keys.find(id);
    if (it == keys.end()) {
        auto raw = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
                                                id.ed25519().data(), 32);
        assert(raw);
        it = keys.emplace(id, TestKey(raw, EVP_PKEY_free)).first;
    }
    return it->second.get();
}
namespace PubKeyUtils {
template<class B> bool verifySig(NodeID const& id, Signature const& sig, B const& b) {
    TestCtx ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    assert(EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, testKey(id)) == 1);
    return EVP_DigestVerify(ctx.get(), sig.data(), sig.size(), b.data(), b.size()) == 1;
}
}
struct Secret {
    template<class B> Signature sign(B const& b) const {
        TestCtx ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
        assert(EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, testKey(id)) == 1);
        Signature sig(64);
        size_t length = sig.size();
        assert(EVP_DigestSign(ctx.get(), sig.data(), &length, b.data(), b.size()) == 1);
        return sig;
    }
''')
pre = pre.replace('struct Application {', 'struct Application {Hash getNetworkID()const{return {};}')
pre = pre.replace('bool TETRABFT_MODE=', 'bool PBFT_ROTATING=false; constexpr uint64_t PBFT_TIMEOUT_MS=100,PBFT_HISTORY=1000; bool TETRABFT_MODE=')
pre = '#include <openssl/evp.h>\n' + pre
pre = pre.replace('namespace experiment {', """
namespace KeyUtils {std::string toShortString(NodeID const& id){return std::to_string(id.ed25519()[31]);}}
namespace experiment {
int txn_count=0;
TransactionBatch makeSyntheticYCSBBatch(Application&,std::string const&,uint64_t,size_t){std::abort();}
""")
header = (ROOT / 'src/overlay/OverlayManagerImpl.h').read_text()
fields = header[header.index('    struct PbftRuntime;'):header.index('    void tryCommitShabdizBlock')]
body = common['strip']('src/overlay/experiments/PBFT.cpp').replace(
    'using Clock = std::chrono::steady_clock;', 'using Clock = TestClock;')
fixture = common['POST']
replicas = fixture[:fixture.index('struct Network {')]
source = pre + fields + '\n void pbftProp();\n};\n}\n' + body + replicas + r'''
struct Net
{
    int n;
    std::vector<Replica> nodes;
    std::mt19937 random;
    std::set<int> stopped;
    bool suppressCommits = false;
    std::map<uint64_t, std::set<int>> primaries;
    std::map<uint64_t, std::set<uint64_t>> installedViews;
    Net(int count, bool rotating, int seed) : n(count), nodes(n), random(seed)
    {
        PBFT_ROTATING = rotating;
        events.clear();
        acked.clear();
        for (int i = 0; i < n; ++i)
        {
            auto& r = nodes[i];
            auto& cfg = r.overlay.mApp.cfg;
            cfg.NODE_SEED.id = node(i);
            r.listener = cfg.SEND_CUSTOM_MESSAGE = i == n - 1;
            for (int j = 0; j < n; ++j)
            {
                cfg.QUORUM_SET.insert(node(j));
                if (j != i)
                {
                    auto p = std::make_shared<Peer>();
                    p->id = node(j);
                    r.overlay.peers[node(j)] = p;
                }
            }
            r.overlay.initializePBFT();
        }
    }
    template <class F>
    void
    invoke(int i, F f)
    {
        active = i;
        nodes[i].swap();
        f(nodes[i].overlay);
        nodes[i].swap();
        active = -1;
    }
    void
    add(int first, int count)
    {
        for (int i = first; i < first + count; ++i)
            nodes[n - 1].queue.push_back(batch(i));
        invoke(n - 1, [](auto& o) { o.pbftProp(); });
    }
    void
    drain()
    {
        size_t budget = 0;
        while (!events.empty())
        {
            assert(++budget < 500000);
            auto pos = random() % events.size();
            auto e = std::move(events[pos]);
            events.erase(events.begin() + pos);
            if (stopped.count(e.sender) || stopped.count(e.receiver))
                continue;
            auto& cm = e.message.customMessage();
            PbftPacket p;
            xdr::xdr_from_opaque(
                std::vector<char>(cm.data.begin(), cm.data.end()), p);
            if (p.kind == Commit && suppressCommits)
                continue;
            if (p.kind == Proposal)
                primaries[cm.view].insert(e.sender);
            if (p.kind == NewView)
                installedViews[cm.view].insert(p.view);
            invoke(e.receiver, [&](auto& o) {
                o.handlePBFTMessage(cm, node(e.sender));
                if (random() % 7 == 0)
                    o.handlePBFTMessage(cm, node(e.sender));
            });
        }
    }
    void
    tick()
    {
        TestClock::t += std::chrono::milliseconds(50);
        for (int i = 0; i < n; ++i)
            if (!stopped.count(i))
                invoke(i, [](auto& o) { o.mPbft->tick(); });
        drain();
    }
    void
    finish(int last)
    {
        drain();
        for (int t = 0; t < 500; ++t)
        {
            bool done = true;
            for (int i = 0; i < n; ++i)
                if (!stopped.count(i))
                    done &= nodes[i].overlay.mPbft->seq == uint64_t(last + 1);
            if (done)
                break;
            tick();
        }
        for (int i = 0; i < n; ++i)
            if (!stopped.count(i) &&
                nodes[i].overlay.mPbft->seq != uint64_t(last + 1))
            {
                std::cerr << "stalled n=" << n << " rotating=" << PBFT_ROTATING
                          << " target=" << last << " replica=" << i << "\n";
                for (auto& node : nodes)
                {
                    auto& r = *node.overlay.mPbft;
                    std::cerr << r.self << " seq=" << r.seq
                              << " view=" << r.view
                              << " changing=" << r.changing
                              << " reports=" << r.changes.size()
                              << " proposal=" << bool(r.proposal)
                              << " pending=" << !r.pending.empty()
                              << " candidate=" << !r.candidate.empty()
                              << " prepared=" << bool(r.prepared)
                              << " commits=" << r.commits.size() << "\n";
                }
                break;
            }
        for (int i = 0; i < n; ++i)
            if (!stopped.count(i))
            {
                assert(nodes[i].overlay.mPbft->seq == uint64_t(last + 1));
                assert(nodes[i].kv.at("x") == std::to_string(last));
                for (int s = 1; s <= last; ++s)
                {
                    auto& d = nodes[i].overlay.mPbft->history.at(s);
                    assert(d.value == batch(s).serialize());
                }
            }
        for (int s = 1; s <= last; ++s)
            assert(acked[s] == 1);
    }
};
int
main()
{
    for (bool rotating : {false, true})
        for (int n : {4, 7, 8})
            for (int seed = 1; seed <= 3; ++seed)
            {
                Net net(n, rotating, seed);
                net.add(1, 2 * n);
                net.finish(2 * n);
                for (int s = 1; s <= 2 * n; ++s)
                {
                    assert(net.primaries[s] ==
                           std::set<int>{rotating ? (s - 1) % n : 0});
                    if (rotating && s > 1)
                        assert(net.installedViews[s].count(s));
                    if (!rotating)
                        assert(net.installedViews[s].empty());
                }
            }
    for (bool rotating : {false, true})
    {
        // Lose the first primary before it can propose, then recover a lagging
        // replica from retained, verified decisions after it reconnects.
        Net net(4, rotating, 65);
        net.stopped.insert(0);
        net.add(1, 3);
        net.finish(3);
        net.stopped.clear();
        net.finish(3); // rejoin an idle cluster
        net.add(4, 2);
        net.finish(5);
    }
    for (bool rotating : {false, true})
    {
        Net net(4, rotating, 68);
        net.suppressCommits = true;
        net.add(1, 1);
        net.drain();
        for (auto& r : net.nodes)
        {
            assert(r.overlay.mPbft->prepared);
            assert(r.overlay.mPbft->seq == 1);
        }
        // Corrupt evidence, duplicate signers, replay a proof at another slot,
        // and a non-primary NEW-VIEW must all be rejected.
        auto& r = *net.nodes[1].overlay.mPbft;
        auto proof = *r.prepared;
        assert(r.certificate(proof, 1, false));
        auto bad = proof;
        bad.proof.back() = bad.proof.front();
        assert(!r.certificate(bad, 1, false));
        bad = proof;
        bad.proof.front().signature[0] ^= 1;
        assert(!r.certificate(bad, 1, false));
        bad = proof;
        bad.value += "x";
        assert(!r.certificate(bad, 1, false));
        ++r.seq;
        assert(!r.certificate(proof, 1, false));
        --r.seq;
        PbftPacket nv = proof;
        nv.kind = NewView;
        nv.view = 2;
        for (int i : {0, 1, 2})
        {
            PbftReport report;
            report.preparedView = 1;
            report.digest = proof.digest;
            report.author = net.nodes[i].overlay.mPbft->sign(ViewChange, 2,
                                                             proof.digest, 1);
            nv.reports.push_back(report);
        }
        auto& backup = *net.nodes[3].overlay.mPbft;
        backup.install(0, nv); // only primary 1 may install view 2
        assert(backup.view == 1);
        bad = nv;
        bad.reports.back() = bad.reports.front();
        backup.install(1, bad);
        assert(backup.view == 1);
        bad = nv;
        bad.value = batch(777).serialize();
        backup.install(1, bad);
        assert(backup.view == 1);
        // Give the next primary an alternative batch; its NEW-VIEW must carry
        // the already-prepared value despite that local candidate.
        net.nodes[1].overlay.mPbft->candidate = batch(999).serialize();
        net.stopped.insert(0);
        net.suppressCommits = false;
        net.finish(1);
        auto& d = net.nodes[1].overlay.mPbft->history.at(1);
        assert(d.view > 1 && d.valueView == 1 && d.digest == proof.digest);
        assert(net.installedViews[1].count(d.view));
        auto recovered = d.view;
        net.add(2, 1);
        net.finish(2);
        auto& next = net.nodes[1].overlay.mPbft->history.at(2);
        assert(next.view >= recovered + (rotating ? 1 : 0));
        assert(!net.primaries[2].count(0));
    }
    for (bool rotating : {false, true})
    {
        Net net(49, rotating, 17);
        net.add(1, 2);
        net.finish(2);
        assert(net.primaries[2] == std::set<int>{rotating ? 1 : 0});
    }
    std::cout << "PBFT stable/rotating simulations passed: reordered and "
                 "duplicate messages, "
                 "healthy rotation, primary loss before proposal and after "
                 "preparation, "
                 "signed certificate checks, ordered catch-up, client ACKs.\n";
}
'''
with tempfile.TemporaryDirectory(prefix='pbft-recovery-') as tmp:
    cpp, exe = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
    cpp.write_text(source)
    includes = ['-I' + p for p in os.environ.get('PBFT_TEST_INCLUDES','').split(os.pathsep) if p]
    subprocess.run(['g++', '-std=c++17', '-O1', *includes, '-Isrc', '-Isrc/protocol-curr',
                    '-Ilib/xdrpp', str(cpp), 'lib/xdrpp/xdrpp/marshal.cc', '-lcrypto',
                    '-o', str(exe)], cwd=ROOT, check=True)
    subprocess.run([str(exe)], check=True)
