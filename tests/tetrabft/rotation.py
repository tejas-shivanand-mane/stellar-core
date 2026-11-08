#!/usr/bin/env python3
"""Exercise actual benchmark adapters with isolated replica state and fake transport.

Run after generating the project's XDR headers. Requires a C++17 compiler and
OpenSSL development headers. --sanitize enables Address/Undefined sanitizers.
The test extracts production method bodies; only application/transport/client
I/O are stubbed. Temporary build output is removed on exit.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

r = Path(__file__).resolve().parents[2]
def strip(p): return '\n'.join(x for x in (r/p).read_text().splitlines() if not x.startswith('#include') and not x.startswith('#pragma'))
e=strip('src/overlay/experiments/Experiment.cpp')
pre='''
#include "xdr/Stellar-overlay.h"
#include "util/XDROperators.h"
#include "xdrpp/marshal.h"
#include "overlay/CustomProtocolTypes.h"
#include "overlay/NormalPathConsensus.h"
#include <openssl/sha.h>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <iostream>
#include <random>
#define CLOG_DEBUG(...) ((void)0)
#define CLOG_INFO(...) ((void)0)
#define CLOG_WARNING(...) ((void)0)
#define releaseAssert(x) assert(x)
namespace stellar {
Hash sha256(std::string const& s) { Hash h{}; SHA256((unsigned char const*)s.data(),s.size(),h.data());return h; }
std::string binToHex(Hash const& h){return std::string(h.begin(),h.end());}
NodeID node(int n){NodeID id;id.ed25519()[31]=n;return id;}
namespace KeyUtils {std::string toShortString(NodeID const& n){return std::to_string(n.ed25519()[31]);} std::string toStrKey(NodeID const& n){return toShortString(n);}}
struct Delivery {int sender, receiver;StellarMessage message;};
std::vector<Delivery> events;
int active=-1;
std::set<std::pair<uint64_t,int>> proposals;
std::map<uint64_t,int> acked;
void emit(int receiver,StellarMessage const& m){
 auto t=m.customMessage().msgType;
 auto const& c=m.customMessage();
 if(t==CUSTOM_PBFT_PREPARE || t==CUSTOM_PBFT_COMMIT ||
    (t>=CUSTOM_TETRA_VOTE1 && t<=CUSTOM_TETRA_VOTE4) ||
    (t>=CUSTOM_SIMPLE_ECHO && t<=CUSTOM_SIMPLE_COMMIT) ||
    t==CUSTOM_FORGET_VOTE1 || t==CUSTOM_FORGET_VOTE2) assert(c.data.empty());
 if(t==CUSTOM_SIMPLE_COMMIT)assert(c.blockHash==Hash{} && c.vp==0);
 if(t==CUSTOM_PBFT_PRE_PREPARE || t==CUSTOM_ITHS_PROPOSE || t==CUSTOM_TETRA_PROPOSE || t==CUSTOM_SIMPLE_PROPOSE || t==CUSTOM_FORGET_PREPARE)
  proposals.emplace(m.customMessage().view,active);
 events.push_back({active,receiver,m});
}
struct Secret {NodeID id;NodeID getPublicKey()const{return id;}};
struct Config {Secret NODE_SEED;bool SEND_CUSTOM_MESSAGE=false;std::set<NodeID> QUORUM_SET;};
struct Application {Config cfg;Config const& getConfig()const{return cfg;}};
struct LocalNode{template<class F>static void forAllNodes(std::set<NodeID> const&s,F fn){for(auto const& n:s)fn(n);}};
struct Peer {using pointer=std::shared_ptr<Peer>;NodeID id;NodeID getPeerID(){return id;}void sendMessage(std::shared_ptr<StellarMessage> m){emit(id.ed25519()[31],*m);}};
namespace experiment {
bool TETRABFT_MODE=false,SIMPLE_IT_MODE=false,FORGET_IT_MODE=false,SIMPLE_IT_OPTIMISTIC=true,PBFT_MODE=false,ITHS_MODE=false,NonPrune_MODE=false,ENABLE_SCP_TRACKING=false;
constexpr size_t PBFT_HISTORY=5,MAX_VIEW_HISTORY=5,NORMAL_PATH_WINDOW=64,SERVER_BATCH_SIZE=2;
constexpr int CLIENT_PORT=12000;
std::deque<TransactionBatch> input;
}
}
'''
state=strip('src/overlay/experiments/ExperimentState.h')
# Real shared globals, excluding getRSS/cleanup (defined below).
start=e.index('namespace experiment\n{');end=e.index('size_t\ngetRSS_MB()');glob=e[start:end]+'}\n'
# Real method declarations/fields for normal adapters and work transport.
h=(r/'src/overlay/OverlayManagerImpl.h').read_text();start=h.index('    void initializeNormalPath();');end=h.index('    void tryCommitShabdizBlock',start)
cls='''namespace stellar {
class OverlayManagerImpl {public:
Application mApp;std::map<NodeID,Peer::pointer> peers;
auto getAuthenticatedPeers()const{return peers;} size_t getAuthenticatedPeersCount()const{return peers.size();}
void broadcastMessage(std::shared_ptr<StellarMessage> msg){for(auto const&p:peers)p.second->sendMessage(msg);}
bool paperShabdizEnabled()const{return false;}void tickPaperShabdiz(){}void receivePaperShabdiz(CustomMessage const&,NodeID const&){}
void shabdizProp(){}void shabdizCollect(){}void acceptShabdizLocalProposal(CustomMessage const&){}void handleShabdizMessage(StellarMessage const&,Peer::pointer){}
void prop();void legacyProp(bool);void ithsProp();void recvCustomMessage(StellarMessage const&,Peer::pointer);
void handleITHSMessage(StellarMessage const&,Peer::pointer);void acceptITHSLocalProposal(CustomMessage const&);void deliverBufferedForCurrentView();
void sendPBFTPrePrepare(uint64_t,uint64_t,Hash const&,std::string const&);void sendPBFTPrepare(uint64_t,uint64_t,Hash const&);void sendPBFTCommit(uint64_t,uint64_t,Hash const&);
void sendITHSEcho(uint64_t,Hash const&,std::string const&);void sendITHSAccept(uint64_t,Hash const&,std::string const&);void sendITHSLock(uint64_t,Hash const&,std::string const&);void sendITHSCommit(uint64_t,Hash const&,std::string const&);
'''+h[start:end]+'''};
'''+glob+'''
namespace experiment {
size_t getClientRequestQueueSize(){return input.size()*SERVER_BATCH_SIZE;}
bool tryBuildServerBatch(TransactionBatch& b,std::vector<ClientAck>& a){if(input.empty())return false;b=input.front();input.pop_front();a={{b.transactions[0].txnId,-1}};return true;}
void ackClientBatchesForBlock(uint64_t slot,Hash const& h){auto it=g_blockClientAcks.find({slot,h});if(it!=g_blockClientAcks.end()){assert(it->second[0].batchId==slot);++acked[slot];g_blockClientAcks.erase(it);}}
void updateForcedCollectState(){}
TransactionBatch makeSyntheticYCSBBatch(Application&,std::string const&,uint64_t,size_t){assert(false);return {};}
}
}
'''
def func(src,name,ret='void'):
 start=src.index(ret+'\nOverlayManagerImpl::'+name+'(');brace=src.index('{',start);depth=1;i=brace+1
 while depth: depth+=(src[i]=='{')-(src[i]=='}');i+=1
 return src[start:i]
# Keep actual cleanup, makeBlock and all message dispatch/proposal paths.
start=e.index('Hash\nmakeBlock(');end=e.index('static std::shared_ptr<StellarMessage>')
body='namespace stellar {using namespace experiment; namespace experiment {\n'+e[start:end] # ends experiment namespace only
start=e.index('static std::shared_ptr<StellarMessage>');end=e.index('void\nOverlayManagerImpl::tickExperiment()');body+=e[start:end]
for name in ['prop','recvCustomMessage','deliverBufferedForCurrentView','legacyProp']:body+=func(e,name)+'\n'
body+='}\n'
for n in ['NormalPathProtocol','PBFT','ITHS','SimpleIT','ForgetIT','TetraBFT']:body+=strip(f'src/overlay/experiments/{n}.cpp')+'\n'
# Snapshot process-global benchmark state so replicas have independent state.
names=['g_txn','g_blockClientAcks','g_ps','currentView','latestCommittedView','latestCommittedBlock','txn_count','g_ithsLockView','g_ithsLockBlock','g_ithsEchoSentForView','g_ithsAcceptSentForView','g_ithsLockSentForView','g_ithsCommitSentForView','g_fastProposedViews','g_proposedViews','g_futureFastMsgs','g_clientListenerActive','g_kvStore','input','g_pbft','g_pbftAcceptedDigest','g_pbftNextSeq','g_pbftLastExecuted']
post='using namespace stellar;using namespace stellar::experiment;\nstruct Replica {OverlayManagerImpl overlay;\n'
for n in names:post+=f'decltype({n}) s_{n} = {n};\n'
post+='void swap(){\n'
for n in names:post+=f'std::swap({n},s_{n});\n'
post+='}};\n'
post+='''
void run(int mode,int n,unsigned seed){
 PBFT_MODE=mode==0;ITHS_MODE=mode==1;TETRABFT_MODE=mode==2;FORGET_IT_MODE=mode==3;SIMPLE_IT_MODE=mode>=4;SIMPLE_IT_OPTIMISTIC=mode==4;
 events.clear();proposals.clear();acked.clear();
 std::vector<Replica> replicas(n);int slots=2*n+3;
 for(int id=0;id<n;++id){
  auto& r=replicas[id];r.overlay.mApp.cfg.NODE_SEED.id=node(id);r.overlay.mApp.cfg.SEND_CUSTOM_MESSAGE=id==n-1;
  for(int j=0;j<n;++j){r.overlay.mApp.cfg.QUORUM_SET.insert(node(j));if(j!=id){auto p=std::make_shared<Peer>();p->id=node(j);r.overlay.peers[node(j)]=p;}}
  r.overlay.initializeNormalPath();r.s_g_clientListenerActive=id==n-1;
 }
 for(int slot=1;slot<=slots;++slot){TransactionBatch b;b.transactions={{uint64_t(slot),"UPDATE x "+std::to_string(slot),5,"client"},{uint64_t(slot),"UPDATE y two",6,"client"}};replicas[n-1].s_input.push_back(b);}
 auto invoke=[&](int id,auto action){active=id;replicas[id].swap();action(replicas[id].overlay);replicas[id].swap();active=-1;};
 // An authenticated nonleader cannot produce a proposal for slot 1.
 for(int id=0;id<n;++id)invoke(id,[&](auto& o){
  StellarMessage m;m.type(CUSTOM_MESSAGE);auto& c=m.customMessage();c.msgType=PBFT_MODE?CUSTOM_PBFT_PRE_PREPARE:ITHS_MODE?CUSTOM_ITHS_PROPOSE:o.normalProposalType();c.view=1;c.vp=(PBFT_MODE||ITHS_MODE)?0:o.normalMetadata(1);
  TransactionBatch b;b.transactions={{1,"UPDATE bad bad",5,"client"},{1,"UPDATE bad bad",6,"client"}};c.data=b.serialize();c.blockHash=sha256(c.data);auto p=std::make_shared<Peer>();p->id=node(n-1);o.recvCustomMessage(m,p);
 });assert(events.empty());
 invoke(n-1,[](auto& o){o.prop();});
 std::mt19937 rng(seed);size_t count=0;
 while(!events.empty()){
  assert(++count<1000000);size_t k=rng()%events.size();auto d=events[k];events.erase(events.begin()+k);
  invoke(d.receiver,[&](auto& o){auto p=std::make_shared<Peer>();p->id=node(d.sender);o.recvCustomMessage(d.message,p);if(rng()%5==0)o.recvCustomMessage(d.message,p);});
 }
 for(int id=0;id<n;++id){auto const& r=replicas[id];if(r.overlay.mExperimentExecuted!=uint64_t(slots)){std::cerr<<"stalled mode="<<mode<<" n="<<n<<" seed="<<seed<<" replica="<<id<<" executed="<<r.overlay.mExperimentExecuted<<" expected="<<slots<<"\\n";abort();}assert(r.s_g_kvStore.at("x")==std::to_string(slots));assert(r.s_g_blockClientAcks.empty());assert(r.overlay.mExperimentWork.empty());}
 assert(proposals.size()==size_t(slots));for(auto [slot,id]:proposals)assert(id==int((slot-1)%n));
 assert(acked.size()==size_t(slots));for(auto [slot,count]:acked)assert(count==1);
}
int main(){for(int mode=0;mode<6;++mode)for(int n:{4,7,8})for(unsigned seed=0;seed<10;++seed)run(mode,n,seed);std::cout<<"180 rotating-adapter simulations passed: PBFT, IT-HS, TetraBFT, Forget-IT, Simple-IT optimistic/Bracha; 4/7/8 replicas, two full rotations, reordered/duplicate deliveries, leader checks, ordered execution, exactly-once ingress ACKs.\\n";}
'''

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--sanitize", action="store_true")
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix="stellar-rotation-") as tmp:
    source = Path(tmp) / "rotation.cpp"
    binary = Path(tmp) / "rotation"
    source.write_text("\n".join([pre, state, cls, body, post]))
    command = [os.environ.get("CXX", "g++"), "-std=c++17", "-O1", "-g"]
    if args.sanitize:
        command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    command += ["-I" + str(r / path) for path in
                ("src", "src/protocol-curr", "lib/xdrpp")]
    command += [str(source), "-lcrypto", "-o", str(binary)]
    subprocess.run(command, check=True)
    env = os.environ.copy()
    if args.sanitize:
        env.setdefault("ASAN_OPTIONS", "detect_leaks=0")
    subprocess.run([str(binary)], check=True, env=env)
