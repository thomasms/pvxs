/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvxs is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */

#include <vector>

#include <epicsUnitTest.h>
#include <testMain.h>
#include <osiProcess.h>

#include <epicsThread.h>

#include <pvxs/unittest.h>
#include <pvxs/util.h>
#include <utilpvt.h>

namespace {
using namespace pvxs;

void testFill()
{
    testShow()<<__func__;

    MPSCFIFO<int> Q(4u);

    for(int i=0; i<4; i++)
        Q.push(i);

    testEq(Q.pop(), 0);
    testEq(Q.pop(), 1);
    testEq(Q.pop(), 2);
    testEq(Q.pop(), 3);
}

struct Spammer : public epicsThreadRunable
{
    MPSCFIFO<int>& Q;
    const int begin, end;
    epicsThread worker;
    Spammer(MPSCFIFO<int>& Q, int begin, int end)
        :Q(Q)
        ,begin(begin)
        ,end(end)
        ,worker(*this, "spammer", epicsThreadGetStackSize(epicsThreadStackBig))
    {
        worker.start();
    }

    void run() override final {
        for(auto i=begin; i<end; i++)
            Q.push(i);
    }
};

void testSpam()
{
    testShow()<<__func__;

    MPSCFIFO<int> Q(32u);
    std::vector<bool> rxd(1024, false);

    Spammer A(Q, 0, 256);
    Spammer B(Q, 256, 512);
    Spammer C(Q, 512, 768);
    Spammer D(Q, 768, 1024);

    // not critical, but try to get some of the spammers to block
    epicsThreadSleep(0.1);

    for(size_t i=0; i<rxd.size(); i++) {
        auto n = Q.pop();
        rxd.at(n) = true;
    }

    bool ok = true;
    for(size_t i=0; i<rxd.size(); i++) {
        ok &= rxd[i];
    }
    testTrue(ok)<<" Received all";
}

void testAccount()
{
    testShow()<<__func__;

    std::string account;
    {
        std::vector<char> buf(128);
        testOk(osiGetUserName(buf.data(), buf.size()-1u)==osiGetUserNameSuccess, "osiGetUserName()");
        buf.back() = '\0';
        account = buf.data();
    }
    testOk(!account.empty(), "User: '%s'", account.c_str());

    std::set<std::string> roles;
    osdGetRoles(account, roles);

    testNotEq(roles.size(), 0u);
    for(auto& role : roles) {
        testDiag(" %s", role.c_str());
    }
}

} // namespace

MAIN(testutil)
{
    testPlan(9);
    testTrue(version_abi_check())<<" 0x"<<std::hex<<PVXS_VERSION<<" ~= 0x"<<std::hex<<PVXS_ABI_VERSION;
    testFill();
    testSpam();
    testAccount();
    return testDone();
}
