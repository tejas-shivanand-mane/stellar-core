#!/usr/bin/env python3
"""Exercise the production normal-path adapters with queued network delivery.

Run after configuring/building XDR headers: python tests/tetrabft/multishot.py
Add --sanitize for ASan/UBSan. Only transport, client queue, and app are stubbed;
consensus transitions and work forwarding are compiled from production sources.
"""
import argparse
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def strip(path):
    return '\n'.join(line for line in (ROOT / path).read_text().splitlines()
                     if not line.startswith(('#include', '#pragma')))


PRE = r'''
#include "xdr/Stellar-overlay.h"
#include "util/XDROperators.h"
#include "overlay/CustomProtocolTypes.h"
#include "overlay/NormalPathConsensus.h"
#include "overlay/ForgetITConsensus.h"
#include "xdrpp/marshal.h"
#include <openssl/sha.h>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <iostream>
#include <random>
#define CLOG_INFO(...) ((void)0)
#define releaseAssert(x) assert(x)
namespace stellar {
Hash sha256(std::string const& s) { Hash h{}; SHA256((unsigned char const*)s.data(),s.size(),h.data());return h; }
std::string binToHex(Hash const& h){static char const* hex="0123456789abcdef";std::string s;for(auto b:h){s+=hex[b>>4];s+=hex[b&15];}return s;}
NodeID node(int n){NodeID id;id.ed25519()[31]=n;return id;}
struct Delivery {int sender, receiver;StellarMessage message;};
std::vector<Delivery> events;
int active=-1;
uint64_t fakeForgetNow=0;
std::set<std::pair<uint64_t,int>> proposals;
std::map<uint64_t,int> acked;
void emit(int receiver,StellarMessage const& m){
 auto t=m.customMessage().msgType;
 if(t==CUSTOM_FORGET_NATIVE){ForgetITPacket p;std::vector<char> b(m.customMessage().data.begin(),m.customMessage().data.end());xdr::xdr_from_opaque(b,p);if(p.kind==1)proposals.emplace(m.customMessage().view,active);}
 if(t==CUSTOM_TETRA_PROPOSE || t==CUSTOM_SIMPLE_PROPOSE || t==CUSTOM_FORGET_PREPARE)
  proposals.emplace(m.customMessage().view,active);
 events.push_back({active,receiver,m});
}
struct Secret {NodeID id;NodeID getPublicKey()const{return id;}};
struct Config {Secret NODE_SEED;bool SEND_CUSTOM_MESSAGE=false;std::set<NodeID> QUORUM_SET;};
struct Application {Config cfg;Config const& getConfig()const{return cfg;}};
struct LocalNode{template<class F>static void forAllNodes(std::set<NodeID> const&s,F fn){for(auto const& n:s)fn(n);}};
struct Peer {using pointer=std::shared_ptr<Peer>;NodeID id;void sendMessage(std::shared_ptr<StellarMessage> m){emit(id.ed25519()[31],*m);}};
namespace experiment {
bool TETRABFT_MODE=false,SIMPLE_IT_MODE=false,FORGET_IT_MODE=false,SIMPLE_IT_OPTIMISTIC=true;
constexpr size_t NORMAL_PATH_WINDOW=64,SERVER_BATCH_SIZE=2;
constexpr uint64_t FORGET_IT_DELTA_MS=100;
struct ClientAck {uint64_t batchId;int clientFd;};
struct BlockKey {uint64_t seq;Hash hash;bool operator<(BlockKey const& b)const{return std::tie(seq,hash)<std::tie(b.seq,b.hash);}};
bool g_clientListenerActive=false;
std::deque<TransactionBatch> input;
std::map<BlockKey,std::vector<ClientAck>> g_blockClientAcks;
std::map<std::string,std::string> g_kvStore;
bool tryBuildServerBatch(TransactionBatch& b,std::vector<ClientAck>& a){if(input.empty())return false;b=input.front();input.pop_front();a={{b.transactions[0].txnId,-1}};return true;}
void ackClientBatchesForBlock(uint64_t slot,Hash const& h){auto it=g_blockClientAcks.find({slot,h});if(it!=g_blockClientAcks.end()){for(auto a:it->second)++acked[a.batchId];g_blockClientAcks.erase(it);}}
}
class OverlayManagerImpl {public:
Application mApp;std::map<NodeID,Peer::pointer> peers;
auto const& getAuthenticatedPeers()const{return peers;}
'''

POST = r'''
using namespace stellar;using namespace stellar::experiment;
struct Replica {
 OverlayManagerImpl overlay;
 decltype(input) queue;
 decltype(g_blockClientAcks) acks;
 decltype(g_kvStore) kv;
 bool listener=false;
 void swap(){input.swap(queue);g_blockClientAcks.swap(acks);g_kvStore.swap(kv);std::swap(g_clientListenerActive,listener);}
};
TransactionBatch batch(uint64_t id){TransactionBatch b;b.transactions={{id,"UPDATE x "+std::to_string(id),5,"client"},{id,"UPDATE y two",6,"client"}};return b;}
struct Network {
 int n, ingress;
 std::vector<Replica> replicas;
 explicit Network(int mode,int count):n(count),ingress(n-1),replicas(n){
  TETRABFT_MODE=mode==0;SIMPLE_IT_MODE=mode==1||mode==2;FORGET_IT_MODE=mode==3;SIMPLE_IT_OPTIMISTIC=mode==1;
  events.clear();proposals.clear();acked.clear();fakeForgetNow=0;
  for(int id=0;id<n;++id){auto& r=replicas[id];auto& cfg=r.overlay.mApp.cfg;
   cfg.NODE_SEED.id=node(id);cfg.SEND_CUSTOM_MESSAGE=id==ingress;r.listener=id==ingress;
   for(int j=0;j<n;++j){cfg.QUORUM_SET.insert(node(j));if(j!=id){auto p=std::make_shared<Peer>();p->id=node(j);r.overlay.peers[node(j)]=p;}}
   r.overlay.initializeNormalPath();
  }
 }
 template<class F>void invoke(int id,F action){active=id;replicas[id].swap();action(replicas[id].overlay);replicas[id].swap();active=-1;}
 void deliver(Delivery const& d){invoke(d.receiver,[&](auto& o){if(d.message.customMessage().msgType==CUSTOM_EXPERIMENT_WORK)o.normalReceiveWork(d.message.customMessage(),node(d.sender));else if(d.message.customMessage().msgType==CUSTOM_FORGET_NATIVE)o.handleForgetITMessage(d.message.customMessage(),node(d.sender));else o.handleNormalMessage(d.message,node(d.sender));});}
 void enqueue(int count,int first=1){for(int i=first;i<first+count;++i)replicas[ingress].queue.push_back(batch(i));invoke(ingress,[](auto&o){o.normalProp();});}
 void drain(unsigned seed,bool holdFirstCommit=false){std::mt19937 rng(seed);size_t count=0;
  while(!events.empty()){
   assert(++count<2000000);size_t k=rng()%events.size();
   // The Forget-IT benchmark learns its initial leader from instance 1;
   // preserve TCP's per-sender/receiver FIFO for that regression. Other paths
   // additionally tolerate arbitrary reordering across the entire queue.
   if(FORGET_IT_MODE)for(size_t j=0;j<k;++j)if(events[j].sender==events[k].sender && events[j].receiver==events[k].receiver){k=j;break;}
   auto d=events[k];events.erase(events.begin()+k);
   if(holdFirstCommit && d.message.customMessage().msgType==CUSTOM_SIMPLE_COMMIT && d.message.customMessage().view==1)continue;
   deliver(d);if(rng()%7==0)deliver(d); // delayed and duplicate deliveries
  }
 }
 void check(int batches){
  for(auto const& r:replicas){
   if(r.overlay.mNormalExecuted<uint64_t(batches)){std::cerr<<"stalled: executed="<<r.overlay.mNormalExecuted<<" expected="<<batches<<" tetra="<<TETRABFT_MODE<<" simple="<<SIMPLE_IT_MODE<<"\n";abort();}
   assert(r.kv.at("x")==std::to_string(batches));assert(r.acks.empty());
  }
  assert(acked.size()==size_t(batches));for(auto [id,count]:acked)assert(count==1);
  for(auto [slot,id]:proposals)assert(id==(FORGET_IT_MODE?ingress:int((slot-1)%n)));
 }
};
void network(int mode,int n,unsigned seed,int batches){
 Network net(mode,n);net.enqueue(batches);net.drain(seed);net.check(batches);
 assert(proposals.size()==size_t(batches+(TETRABFT_MODE?3:0)));
 for(auto const& r:net.replicas)assert(r.overlay.mNormalExecuted==uint64_t(batches));
}
void sequentialClients(int mode){
 Network net(mode,4);
 for(int id=1;id<=8;++id){net.enqueue(1,id);net.drain(id);assert(acked[id]==1);for(auto const&r:net.replicas)assert(r.kv.at("x")==std::to_string(id));}
 assert(acked.size()==8);
}
void transitions(){
 // Tetra next leader proposes at vote eligibility, before current notarization.
 Network tetra(0,4);auto& o=tetra.replicas[1].overlay;
 tetra.invoke(1,[&](auto& s){
  auto& first=s.mNormalSlots[1];first.proposed=true;first.data=batch(1).serialize();first.digest=s.normalDigest(1,{},first.data);
  s.mNormalWork.emplace(2,OverlayManagerImpl::NormalWork{batch(2).serialize()});
  s.normalAdvance(1);assert(s.mNormalReady==1);assert(s.mNormalNotarized==0);assert(s.mNormalSlots.at(2).proposed);assert(!s.mNormalSlots.at(2).tetraVoted);
 });
 // Complete a four-block chain locally. Duplicate votes cannot notarize.
 tetra.invoke(1,[&](auto& s){
  for(uint64_t slot=1;slot<=4;++slot){auto& block=s.mNormalSlots[slot];block.proposed=true;block.data=batch(slot).serialize();block.parent=slot==1?Hash{}:s.mNormalSlots.at(slot-1).digest;block.digest=s.normalDigest(slot,block.parent,block.data);
   s.advanceTetraBFT(slot);assert(s.mNormalExecuted==0);
   block.tetraVotes.emplace(node(0),block.digest);block.tetraVotes.emplace(node(0),block.digest);assert(!s.advanceTetraBFT(slot));
   block.tetraVotes.emplace(node(2),block.digest);s.normalAdvance(slot);
   assert(s.mNormalExecuted==(slot==4?1u:0u));
  }
 });
 // Simple-IT delivers the parent through a committed child even when every
 // network COMMIT for round 1 is delayed (Figure 2 Deliver(Log(r))).
 for(int mode:{1,2}){Network simple(mode,4);simple.enqueue(2);simple.drain(29,true);simple.check(2);}
}
void rejectMalformed(){
 for(int mode:{0,1,2}){Network net(mode,4);
  net.invoke(2,[&](auto& s){
   StellarMessage msg;msg.type(CUSTOM_MESSAGE);auto& cm=msg.customMessage();cm.msgType=s.normalProposalType();cm.view=1;cm.vp=s.normalMetadata(1);cm.data=batch(1).serialize();cm.blockHash=s.normalDigest(1,{},cm.data);
   s.handleNormalMessage(msg,node(3));assert(s.mNormalSlots.empty()); // wrong leader
   cm.blockHash[0]^=1;s.handleNormalMessage(msg,node(0));assert(s.mNormalSlots.empty());
   cm.blockHash=s.normalDigest(1,{},cm.data);cm.bp[0]=1;s.handleNormalMessage(msg,node(0));assert(s.mNormalSlots.empty());
   cm.bp={};cm.data="invalid";cm.blockHash=s.normalDigest(1,{},cm.data);s.handleNormalMessage(msg,node(0));assert(s.mNormalSlots.empty());
  });assert(events.empty());
 }
}
void forgetWireValues(){
 Network net(3,4);auto a=batch(1),b=batch(2);
 a.transactions[0].payload="UPDATE x "+std::string(60000,'a');
 b.transactions[0].payload="UPDATE x "+std::string(60000,'b');
 auto av=a.serialize(),bv=b.serialize();
 net.invoke(3,[&](auto&o){using C=ForgetITConsensus<NodeID>;o.sendForgetIT(1,{C::Vote2,2,C::Value::value(av),C::Value::value(bv)});});
 assert(!events.empty());auto const& cm=events.front().message.customMessage();assert(cm.data.size()>100000);
 ForgetITPacket p;std::vector<char> bytes(cm.data.begin(),cm.data.end());xdr::xdr_from_opaque(bytes,p);
 assert(p.value==av && p.guard==bv && p.view==2 && p.kind==3);
 net.invoke(0,[&](auto&o){CustomMessage broken;broken.msgType=CUSTOM_FORGET_NATIVE;broken.view=1;broken.data="bad";o.handleForgetITMessage(broken,node(3));assert(o.mNormalSlots.empty());});
}
void forgetNativeRotation(){
 Network net(3,4);net.enqueue(1);
 size_t steps=0;
 while(!events.empty()){
  assert(++steps<10000);auto d=events.front();events.erase(events.begin());
  ForgetITPacket p;std::vector<char> b(d.message.customMessage().data.begin(),d.message.customMessage().data.end());xdr::xdr_from_opaque(b,p);
  if(p.kind!=ForgetITConsensus<NodeID>::Vote1)net.deliver(d);
 }
 for(auto const&r:net.replicas)assert(r.overlay.mNormalExecuted==0);
 fakeForgetNow=300;for(int id=0;id<4;++id)net.invoke(id,[](auto&o){o.tickForgetIT();});net.drain(81);
 fakeForgetNow=400;for(int id=0;id<4;++id)net.invoke(id,[](auto&o){o.tickForgetIT();});net.drain(82);
 for(auto const&r:net.replicas){assert(r.overlay.mNormalExecuted==1);assert(r.overlay.mNormalSlots.at(1).forget->state().view==2);assert(r.kv.at("x")=="1");}
 assert(acked[1]==1);assert(proposals.count({1,3}) && proposals.count({1,0}));
}
int main(){
 transitions();rejectMalformed();forgetNativeRotation();forgetWireValues();
 for(int mode=0;mode<4;++mode){sequentialClients(mode);for(int n:{4,7,8}){if(mode==3 && n==8)continue;for(unsigned seed=0;seed<10;++seed)network(mode,n,seed,2*n+3);}network(mode,4,99,140);}
 std::cout<<"114 adapter simulations passed; native TetraBFT/Simple-IT rotation and overlap, Forget-IT healthy path and native view-change wire test, full lock/guard payloads, duplicate/reordered delivery, bounded retention, single outstanding batch, empty-chain drain, exactly-once ACKs, malformed proposals.\n";
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    header = (ROOT / 'src/overlay/OverlayManagerImpl.h').read_text()
    fields = header[header.index('    void initializeNormalPath();'):
                    header.index('    void tryExecutePBFT();')]
    body = '\n'.join(strip(f'src/overlay/experiments/{name}.cpp') for name in
                     ('NormalPathProtocol', 'TetraBFT', 'SimpleIT', 'ForgetIT'))
    # Substitute only the clock, leaving the production state machine and wire
    # decoder/encoder intact for deterministic timeout tests.
    clock_start = body.index('uint64_t\nforgetNow()')
    clock_end = body.index('bool\nvalidForgetValue', clock_start)
    body = body[:clock_start] + 'uint64_t forgetNow(){return fakeForgetNow;}\n' + body[clock_end:]
    source = PRE + fields + '\n};\n}\n' + body + POST
    with tempfile.TemporaryDirectory(prefix='stellar-multishot-') as tmp:
        cpp, exe = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
        cpp.write_text(source)
        flags = ['-O1', '-g', '-fsanitize=address,undefined',
                 '-fno-omit-frame-pointer'] if args.sanitize else ['-O2']
        subprocess.run(['g++', '-std=c++17', *flags, '-I.', '-Isrc',
                        '-Isrc/protocol-curr', '-Ilib/xdrpp', str(cpp),
                        'lib/xdrpp/xdrpp/marshal.cc', '-lcrypto', '-o', str(exe)], cwd=ROOT, check=True)
        subprocess.run([str(exe)], check=True,
                       env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})


if __name__ == '__main__':
    main()
