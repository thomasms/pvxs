﻿/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvxs is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */
#include <epicsAssert.h>

#include <pvxs/log.h>
#include <pvxs/nt.h>
#include "utilpvt.h"
#include "clientimpl.h"

namespace pvxs {
namespace client {

DEFINE_LOGGER(setup, "pvxs.client.setup");
DEFINE_LOGGER(io, "pvxs.client.io");

namespace detail {

struct PRBase::Args
{
    std::map<std::string, std::pair<Value, bool>> values;
    std::vector<std::string> names;

    // put() builder
    Value build(Value&& prototype) const
    {
        Value ret(prototype.cloneEmpty());

        for(auto& pair : values) {
            if(auto fld = ret[pair.first]) {
                try {
                    auto store = Value::Helper::store(pair.second.first);
                    fld.copyIn(static_cast<const void*>(&store->store), store->code);
                }catch(NoConvert& e){
                    if(pair.second.second)
                        throw;
                }

            } else if(pair.second.second) {
                throw std::runtime_error(SB()<<"PutBuilder server type missing required field '"<<pair.first<<"'");
            }
        }
        return ret;
    }

    Value uriArgs() const
    {
        TypeDef type(nt::NTURI{}.build());

        std::list<Member> arguments;

        for(auto& name : names) {
            auto it = values.find(name);
            if(it==values.end())
                throw std::logic_error("uriArgs() names vs. values mis-match");

            auto& value = it->second.first;

            arguments.push_back(TypeDef(value).as(name));
        }

        type += {Member(TypeCode::Struct, "query", arguments)};

        auto inst(type.create());

        for(auto& pair : values) {
            inst["query"][pair.first].assign(pair.second.first);
        }

        return inst;
    }
};

PRBase::~PRBase() {}

Value PRBase::_builder(Value&& prototype) const
{
    assert(_args);
    return _args->build(std::move(prototype));
}

Value PRBase::_uriArgs() const
{
    assert(_args);
    return _args->uriArgs();
}

void PRBase::_set(const std::string& name, const void *ptr, StoreType type, bool required)
{
    if(!_args)
        _args = std::make_shared<Args>();

    if(_args->values.find(name)!=_args->values.end())
        throw std::logic_error(SB()<<"PutBuilder can't assign a second value to field '"<<name<<"'");

    Value aval(Value::Helper::build(ptr, type));

    _args->values.emplace(std::piecewise_construct,
                         std::make_tuple(name),
                         std::make_tuple(std::move(aval), required));
    _args->names.push_back(name);
}

} // namespace detail

namespace {

struct GPROp : public OperationBase
{
    std::function<Value(Value&&)> builder;
    std::function<void(Result&&)> done;
    std::function<void (const Value&)> onInit;
    Value pvRequest;
    Value rpcarg;
    Result result;
    bool getOput = false;

    enum state_t : uint8_t {
        Connecting, // waiting for an active Channel
        Creating,   // waiting for reply to INIT
        GetOPut,    // waiting for reply to GET (CMD_PUT only)
        BuildPut,   // waiting for PUT builder callback
        Exec,       // waiting for reply to EXEC
        Done,
    } state = Connecting;

    INST_COUNTER(GPROp);

    GPROp(operation_t op, const std::shared_ptr<Channel>& chan)
        :OperationBase (op, chan)
    {}
    ~GPROp() {
        chan->context->tcp_loop.assertInLoop();
        _cancel(true);
    }

    void setDone(decltype (done)&& donecb, decltype (onInit)&& initcb)
    {
        onInit = std::move(initcb);
        if(donecb) {
            done = std::move(donecb);
        } else {
            auto waiter = this->waiter = std::make_shared<ResultWaiter>();
            done = [waiter](Result&& result) {
                waiter->complete(std::move(result), false);
            };
        }
    }

    void notify() {
        try {
            if(done)
                done(std::move(result));
        } catch(std::exception& e) {
            if(chan && chan->conn)
                log_err_printf(io, "Server %s channel %s error in result cb : %s\n",
                               chan->conn->peerName.c_str(), chan->name.c_str(), e.what());

            // keep first error (eg. from put builder)
            if(!result.error())
                result = Result(std::current_exception());
        }
    }

    virtual bool cancel() override final
    {
        auto context = chan->context;
        decltype (done) junk;
        decltype (onInit) junkI;
        bool ret;
        context->tcp_loop.call([this, &junk, &junkI, &ret](){
            ret = _cancel(false);
            junk = std::move(done);
            junkI = std::move(onInit);
            // leave opByIOID for GC
        });
        return ret;
    }


    bool _cancel(bool implicit) {
        if(implicit && state!=Done) {
            log_warn_printf(setup, "implied cancel of op%x on channel '%s'\n",
                            op, chan ? chan->name.c_str() : "");
        }
        if(state==GetOPut || state==Exec) {
            chan->conn->sendDestroyRequest(chan->sid, ioid);
        }
        if(state==Creating || state==GetOPut || state==Exec) {
            // This opens up a race with an in-flight reply.
            chan->conn->opByIOID.erase(ioid);
            chan->opByIOID.erase(ioid);
        }
        bool ret = state!=Done;
        state = Done;
        return ret;
    }

    virtual void createOp() override final
    {
        if(state!=Connecting) {
            return;
        }

        auto& conn = chan->conn;

        {
            (void)evbuffer_drain(conn->txBody.get(), evbuffer_get_length(conn->txBody.get()));

            EvOutBuf R(hostBE, conn->txBody.get());

            to_wire(R, chan->sid);
            to_wire(R, ioid);
            to_wire(R, uint8_t(0x08)); // INIT
            to_wire(R, Value::Helper::desc(pvRequest));
            to_wire_full(R, pvRequest);
        }
        conn->enqueueTxBody(pva_app_msg_t(uint8_t(op)));

        log_debug_printf(io, "Server %s channel '%s' op%02x INIT\n",
                         conn->peerName.c_str(), chan->name.c_str(), op);

        state = Creating;
    }

    virtual void disconnected(const std::shared_ptr<OperationBase> &self) override final
    {
        if(state==Connecting || state==Done) {
            // noop

        } else if(state==Creating || state==GetOPut || (state==Exec && op==Get)) {
            // return to pending

            chan->pending.push_back(self);
            state = Connecting;

        } else if(state==Exec) {
            // can't restart as server side-effects may occur
            state = Done;
            result = Result(std::make_exception_ptr(Disconnect()));

            notify();

        } else {
            state = Done;
            result = Result(std::make_exception_ptr(std::logic_error("GPR Disconnect in unexpected state")));

            notify();
        }
    }
};

} // namespace

void Connection::handle_GPR(pva_app_msg_t cmd)
{
    EvInBuf M(peerBE, segBuf.get(), 16);

    uint32_t ioid;
    uint8_t subcmd=0;
    Status sts;
    Value data; // hold prototype (INIT) or reply data (GET)

    from_wire(M, ioid);
    from_wire(M, subcmd);
    from_wire(M, sts);
    bool init = subcmd&0x08;
    bool get  = subcmd&0x40;

    // immediately deserialize in unambigous cases

    if(M.good() && cmd!=CMD_RPC && init && sts.isSuccess()) {
        // INIT of PUT or GET, decode type description

        from_wire_type(M, rxRegistry, data);

    } else if(M.good() && cmd==CMD_RPC && !init &&  sts.isSuccess()) {
        // RPC reply

        from_wire_type(M, rxRegistry, data);
        if(data)
            from_wire_full(M, rxRegistry, data);
    }

    // need type info from INIT reply to decode PUT/GET

    RequestInfo* info=nullptr;
    if(M.good()) {
        auto it = opByIOID.find(ioid);
        if(it!=opByIOID.end()) {
            info = &it->second;

        } else {
            auto lvl = Level::Debug;
            if(cmd!=CMD_RPC && !init) {
                // We don't have enough information to decode the rest of the payload.
                // This *may* leave rxRegistry out of sync (if it contains Variant Unions).
                // We can't know whether this is the case.
                // Failing soft here may lead to failures decoding future replies.
                // We could force close the Connection here to be "safe".
                // However, we assume the such usage of Variant is relatively rare

                lvl = Level::Err;
            }

            log_printf(io, lvl,  "Server %s uses non-existant IOID %u.  Ignoring...\n",
                       peerName.c_str(), unsigned(ioid));
            return;
        }

        if(cmd!=CMD_RPC && init && sts.isSuccess()) {
            // INIT of PUT or GET, store type description
            info->prototype = data;

        } else if(M.good() && !init && (cmd==CMD_GET || (cmd==CMD_PUT && get)) &&  sts.isSuccess()) {
            // GET reply

            data = info->prototype.cloneEmpty();
            if(data)
                from_wire_valid(M, rxRegistry, data);
        }
    }

    // validate received message against operation state

    std::shared_ptr<OperationBase> op;
    GPROp* gpr = nullptr;
    if(M.good() && info) {
        op = info->handle.lock();
        if(!op) {
            // assume op has already sent CMD_DESTROY_REQUEST
            log_debug_printf(io, "Server %s ignoring stale cmd%02x ioid %u\n",
                             peerName.c_str(), cmd, unsigned(ioid));
            return;
        }

        if(uint8_t(op->op)!=cmd) {
            // peer mixes up IOID and operation type
            M.fault(__FILE__, __LINE__);

        } else {
            gpr = static_cast<GPROp*>(op.get());

            // check that subcmd is as expected based on operation state
            if((gpr->state==GPROp::Creating) && init) {

            } else if((gpr->state==GPROp::GetOPut) && !init && get) {

            } else if((gpr->state==GPROp::Exec) && !init && !get) {

            } else {
                M.fault(__FILE__, __LINE__);
            }
        }
    }

    if(!M.good() || !gpr) {
        log_crit_printf(io, "%s:%d Server %s sends invalid op%02x.  Disconnecting...\n",
                        M.file(), M.line(), peerName.c_str(), cmd);
        bev.reset();
        return;
    }

    // advance operation state

    decltype (gpr->state) prev = gpr->state;

    if(!sts.isSuccess()) {
        gpr->result = Result(std::make_exception_ptr(RemoteError(sts.msg)));
        gpr->state = GPROp::Done;

    } else if(gpr->state==GPROp::Creating) {

        try {
            if(gpr->onInit)
                gpr->onInit(data);
        } catch(std::exception& e) {
            gpr->result = Result(std::current_exception());
            gpr->state = GPROp::Done;
        }

        if(cmd==CMD_PUT && gpr->getOput) {
            gpr->state = GPROp::GetOPut;

        } else if(cmd==CMD_PUT && !gpr->getOput) {
            gpr->state = GPROp::BuildPut;

        } else {
            gpr->state = GPROp::Exec;
        }

    } else if(gpr->state==GPROp::GetOPut) {
        gpr->state = GPROp::BuildPut;

        info->prototype.assign(data);

    } else if(gpr->state==GPROp::Exec) {
        gpr->state = GPROp::Done;

        // data always empty for CMD_PUT
        gpr->result = Result(std::move(data), peerName);

    } else {
        // should be avoided above
        throw std::logic_error("GPR advance state inconsistent");
    }

    // transient state (because builder callback is synchronous)
    if(gpr->state==GPROp::BuildPut) {
        Value arg(info->prototype.clone());

        try {
            info->prototype = gpr->builder(std::move(arg));
            gpr->state = GPROp::Exec;

        } catch(std::exception& e) {
            gpr->result = Result(std::current_exception());
            gpr->state = GPROp::Done;
        }
    }

    log_debug_printf(io, "Server %s channel %s op%02x state %d -> %d\n",
                     peerName.c_str(), op->chan->name.c_str(), cmd, prev, gpr->state);

    // act on new operation state

    {
        (void)evbuffer_drain(txBody.get(), evbuffer_get_length(txBody.get()));

        EvOutBuf R(hostBE, txBody.get());

        to_wire(R, op->chan->sid);
        to_wire(R, ioid);
        if(gpr->state==GPROp::GetOPut) {
            to_wire(R, uint8_t(0x40));

        } else if(gpr->state==GPROp::Exec) {
            to_wire(R, uint8_t(0x00));
            if(cmd==CMD_PUT) {
                to_wire_valid(R, info->prototype);

            } else if(cmd==CMD_RPC) {
                to_wire(R, Value::Helper::desc(gpr->rpcarg));
                if(gpr->rpcarg)
                    to_wire_full(R, gpr->rpcarg);
            }

        } else if(gpr->state==GPROp::Done) {
            // we're actually building CMD_DESTROY_REQUEST
            // nothing more needed
        }
    }
    enqueueTxBody(gpr->state==GPROp::Done ? CMD_DESTROY_REQUEST :  cmd);

    if(gpr->state==GPROp::Done) {
        // CMD_DESTROY_REQUEST is not acknowledged (sigh...)
        // but at this point a server should not send further GET/PUT/RPC w/ this IOID
        // so we can ~safely forget about it.
        // we might get CMD_MESSAGE, but these could be ignored with no ill effects.
        opByIOID.erase(ioid);
        gpr->chan->opByIOID.erase(ioid);

        gpr->notify();
    }
}

void Connection::handle_GET() { handle_GPR(CMD_GET); }
void Connection::handle_PUT() { handle_GPR(CMD_PUT); }
void Connection::handle_RPC() { handle_GPR(CMD_RPC); }

static
void gpr_cleanup(std::shared_ptr<Operation>& ret, std::shared_ptr<GPROp>&& op)
{
    auto cap(std::move(op));
    auto loop(cap->chan->context->tcp_loop);
    ret.reset(cap.get(), [cap, loop](Operation*) mutable {
        auto L(std::move(loop));
        // from use thread
        L.call([&cap]() {
            auto temp(std::move(cap));
            // on worker
            try {
                temp->_cancel(true);
            }catch(std::exception& e){
                log_exc_printf(setup, "Channel %s error in get cancel(): %s",
                               temp->chan->name.c_str(), e.what());
            }
            // ensure dtor on worker
            temp.reset();
        });
    });
}

std::shared_ptr<Operation> GetBuilder::_exec_get()
{
    if(!ctx)
        throw std::logic_error("NULL Builder");

    std::shared_ptr<Operation> ret;
    assert(_get);

    ctx->tcp_loop.call([&ret, this]() {
        auto chan = Channel::build(ctx->shared_from_this(), _name);

        auto op = std::make_shared<GPROp>(Operation::Get, chan);
        op->setDone(std::move(_result), std::move(_onInit));
        op->pvRequest = _buildReq();

        chan->pending.push_back(op);
        chan->createOperations();

        gpr_cleanup(ret, std::move(op));
        assert(ret);
    });

    return  ret;
}

std::shared_ptr<Operation> PutBuilder::exec()
{
    if(!ctx)
        throw std::logic_error("NULL Builder");

    std::shared_ptr<Operation> ret;

    if(!_builder && !_args)
        throw std::logic_error("put() needs either a .build() or at least one .set()");

    ctx->tcp_loop.call([&ret, this]() {
        auto chan = Channel::build(ctx->shared_from_this(), _name);

        auto op = std::make_shared<GPROp>(Operation::Put, chan);
        op->setDone(std::move(_result), std::move(_onInit));

        if(_builder) {
            op->builder = std::move(_builder);
        } else if(_args) {
            // PRBase builder doesn't use current value
            _doGet = false;

            auto build = std::move(_args);
            op->builder = [build](Value&& prototype) -> Value {
                return build->build(std::move(prototype));
            };
        } else {
            // handled above
        }
        op->getOput = _doGet;
        op->pvRequest = _buildReq();

        chan->pending.push_back(op);
        chan->createOperations();

        gpr_cleanup(ret, std::move(op));
    });

    return  ret;
}

std::shared_ptr<Operation> RPCBuilder::exec()
{
    if(!ctx)
        throw std::logic_error("NULL Builder");

    std::shared_ptr<Operation> ret;

    if(_args && _argument)
        throw std::logic_error("Use of rpc() with argument and builder .arg() are mutually exclusive");

    ctx->tcp_loop.call([&ret, this]() {
        auto chan = Channel::build(ctx->shared_from_this(), _name);

        auto op = std::make_shared<GPROp>(Operation::RPC, chan);
        op->setDone(std::move(_result), std::move(_onInit));
        if(_argument) {
            op->rpcarg = std::move(_argument);
        } else if(_args) {
            op->rpcarg = _args->uriArgs();
            op->rpcarg["path"] = _name;
        }
        op->pvRequest = _buildReq();

        chan->pending.push_back(op);
        chan->createOperations();

        gpr_cleanup(ret, std::move(op));
    });

    return  ret;
}

} // namespace client
} // namespace pvxs
