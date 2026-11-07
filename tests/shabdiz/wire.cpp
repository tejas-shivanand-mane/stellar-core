#include "xdr/Stellar-overlay.h"
#include "xdrpp/marshal.h"
#include <cassert>
#include <iostream>
int
main()
{
    stellar::ShabdizPacket p{};
    p.wireVersion = 1;
    p.kind = 1;
    p.view = 42;
    p.blockView = 42;
    p.selected.resize(256);
    p.dependencies.push_back(p.origin);
    p.hasBlock = true;
    p.data = std::string(90000, 'x');
    p.data[1] = '\0';
    p.blockHash[0] = 3;
    p.parentHash[0] = 7;
    auto bytes = xdr::xdr_to_opaque(p);
    assert(bytes.size() <= 100000);
    stellar::CustomMessage cm{};
    cm.msgType = stellar::CUSTOM_SHABDIZ_PAPER;
    cm.data.assign(reinterpret_cast<char const*>(bytes.data()), bytes.size());
    auto envelope = xdr::xdr_to_opaque(cm);
    stellar::CustomMessage copy{};
    xdr::xdr_from_opaque(envelope, copy);
    std::vector<char> payload(copy.data.begin(), copy.data.end());
    stellar::ShabdizPacket decoded{};
    xdr::xdr_from_opaque(payload, decoded);
    assert(xdr::xdr_to_opaque(decoded) == bytes);
    auto bad = payload;
    bad.pop_back();
    bool rejected = false;
    try
    {
        xdr::xdr_from_opaque(bad, decoded);
    }
    catch (std::exception const&)
    {
        rejected = true;
    }
    assert(rejected);
    auto domain = xdr::xstring<>("shabdiz-block-v1");
    auto canonical = xdr::xdr_to_opaque(domain, p.view, p.parentHash, p.data);
    p.parentHash[0]++;
    assert(canonical !=
           xdr::xdr_to_opaque(domain, p.view, p.parentHash, p.data));
    std::cout << "Shabdiz packet round-trip and bounds passed: " << bytes.size()
              << " bytes\n";
}
