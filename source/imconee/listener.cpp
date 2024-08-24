#include "pch.h"

listener* listener::build(loader &ldr, const str::astr& name, const asts& bb)
{
	str::astr t = bb.get_string(ASTR("type"));
	if (t.empty())
	{
		ldr.exit_code = EXIT_FAIL_TYPE_UNDEFINED;
		LOG_E("{type} not defined for lisnener [%s]; type {imconee help listener} for more information", str::printable(name));
		return nullptr;
	}

	if (ASTR("tcp") == t)
	{
		tcp_listener *tcpl = new tcp_listener(ldr, name, bb);
		if (ldr.exit_code != EXIT_OK)
		{
			delete tcpl;
			return nullptr;
		}
		return tcpl;
	}

	LOG_E("unknown {type} [%s] for lisnener [%s]; type {imconee help listener} for more information", t.c_str(), str::printable(name));
	ldr.exit_code = EXIT_FAIL_TYPE_UNDEFINED;

	return nullptr;
}

listener::listener(loader& /*ldr*/, const str::astr& name, const asts& /*bb*/)
{
	this->name = name;
}

tcp_listener::tcp_listener(loader& ldr, const str::astr& name, const asts& bb):listener(ldr, name, bb)
{
	signed_t port = bb.get_int(ASTR("port"));
	if (0 == port)
	{
		ldr.exit_code = EXIT_FAIL_PORT_UNDEFINED;
		LOG_E( "port not defined for listener [%s]", str::printable(name));
		return;
	}

	const asts* hnd = bb.get(ASTR("handler"));
	if (nullptr == hnd)
	{
		ldr.exit_code = EXIT_FAIL_NOHANDLER;
		LOG_E("handler not defined for listener [%s]", str::printable(name));
		return;
	}

	handler* h = handler::build(ldr, this, *hnd);
	if (h == nullptr)
		return; // no warning message here due it generated by handler::build

	hand.reset(h);

	str::astr bs = bb.get_string(ASTR("bind"));
	netkit::ipap bindaddr = netkit::ipap::parse(bs);
	bindaddr.port = (u16)port;
	prepare(bindaddr);

}


void tcp_listener::acceptor()
{
	auto ss = state.lock_write();
	ss().stage = ACCEPTOR_WORKS;
	ss.unlock();

	auto r = state.lock_read();
	netkit::ipap bind2 = r().bind;
	r.unlock();

	if (tcp_listen(bind2))
	{
		LOG_N("listener {%s} has been started (bind ip: %s, port: %i)", str::printable(name), bind2.to_string(false).c_str(), bind2.port);

		for (; !state.lock_read()().need_stop;)
		{
			netkit::tcp_pipe* pipe = tcp_accept();
			if (nullptr != pipe)
				hand->on_pipe(pipe);
		}

	}

	ss = state.lock_write();
	ss().stage = IDLE;

	spinlock::decrement(engine::numlisteners);
}

void tcp_listener::prepare(const netkit::ipap &bind2)
{
	stop();

	auto ss = state.lock_write();
	ss().bind = bind2;
}

/*virtual*/ void tcp_listener::open()
{
	auto ss = state.lock_write();
	if (ss().stage != IDLE)
		return;

	ss().stage = ACCEPTOR_START;
	ss.unlock();

	spinlock::increment(engine::numlisteners);

	std::thread th(&tcp_listener::acceptor, this);
	th.detach();
}

/*virtual*/ void tcp_listener::stop()
{
	if (state.lock_read()().stage == IDLE)
		return;

	state.lock_write()().need_stop = true;
	hand->stop();

	// holly stupid linux behaviour...
	// we have to make a fake connection to the listening socket so that the damn [accept] will deign to give control.
	// There is no such crap in Windows

#ifdef _NIX
	auto st = state.lock_read();
	netkit::ipap cnct = netkit::ipap::localhost(st().bind.v4);
	if (!st().bind.is_wildcard())
        cnct = st().bind;

    SOCKET s = ::socket(cnct.v4 ? AF_INET : AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    cnct.connect(s);
    closesocket(s);
#endif

	close(false);

	while (state.lock_read()().stage != IDLE)
        Sleep(100);

	state.lock_write()().need_stop = false;
}
